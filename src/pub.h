/*
 * Publication (D13). hedl formats kipp itself rather than printing dwl's own
 * status and having the shell translate it.
 *
 * D7 says an adapter between two of our own components means the format has
 * drifted, and the fix is the format. So this is D4: tab separated, kind
 * first, with a version line first.
 *
 *   version	1
 *   tags	eDP-1	3	1	1	0
 *   layout	eDP-1	tile
 *   selmon	eDP-1	1
 *
 * Kind first, not monitor first, so a parser matches the same column here as
 * on every other kipp line. Tabs, not spaces, so a field can contain one.
 * Both break dwlb and every existing dwl bar; nothing here uses one.
 *
 * It goes two places. Stdout, as dwl has always done, which is what `-s` feeds
 * and what Phase A tested. And a listening socket at $HEDLDIR/kipp, which is
 * how anything reads it without being hedl's child.
 *
 * **D2 is un-reversed.** It was reversed on the reasoning that hedl would print
 * dwl's own format and the shell would translate, so the shell had to be the
 * `-s` child and own the socket. hedl speaks kipp itself now (D13), so there
 * is nothing to translate and no reason to require the parent relationship.
 * D3 exists to avoid exactly that: nothing has to inherit anything, and a
 * reader derives the path instead. A consumer is one line of config:
 *
 *   { name = "wm", adapter = "wm/hedl.lua", sock = "$XDG_RUNTIME_DIR/hedl/kipp" }
 *
 * One correction to the original D2 while un-reversing it. It said no state
 * dump on accept, because a separate state file carried current values. There
 * is no state file, so a late reader would know nothing. Every publish is a
 * full picture rather than a delta, so the dump on accept is just a publish.
 */

#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>

#define KIPP_VERSION 1
#define MAXREADERS   8

static int publisten = -1;
static int readers[MAXREADERS];
static int nreaders;
static char pubpath[288];

static char pubbuf[16384];
static size_t publen;

static void
pubout(const char *s, size_t n)
{
	if (publen + n >= sizeof(pubbuf))
		return;   /* a title long enough to fill this is not worth a realloc */
	memcpy(pubbuf + publen, s, n);
	publen += n;
}

static void
pubf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(pubbuf + publen, sizeof(pubbuf) - publen, fmt, ap);
	va_end(ap);
	if (n > 0 && (size_t)n < sizeof(pubbuf) - publen)
		publen += (size_t)n;
}

/*
 * X5: a window title is whatever the client says it is, so a newline in one
 * would forge a status line and a tab would forge a field. Neither survives.
 */
static void
putsafe(const char *s)
{
	char c;

	if (!s)
		return;
	for (; *s; s++) {
		c = (unsigned char)*s < 0x20 || *s == 0x7f ? ' ' : *s;
		pubout(&c, 1);
	}
}

/* Send what is in the buffer to one reader. A full write or nothing: every
 * publish is the whole picture, so a dropped one is corrected by the next. */
static int
pubsend(int fd)
{
	ssize_t n = send(fd, pubbuf, publen, MSG_NOSIGNAL | MSG_DONTWAIT);

	if (n >= 0)
		return 1;
	return errno == EAGAIN || errno == EWOULDBLOCK;   /* slow, not dead */
}

static void
pubflush(void)
{
	int i;

	fwrite(pubbuf, 1, publen, stdout);
	fflush(stdout);
	for (i = 0; i < nreaders;) {
		if (pubsend(readers[i])) {
			i++;
			continue;
		}
		close(readers[i]);
		readers[i] = readers[--nreaders];
	}
	publen = 0;
}

static void publish(void);

/*
 * kipp, per SPEC.md: kind first, then positional subject up to the first field
 * holding '=', then key=value attributes.
 *
 * One fact for the whole picture rather than one for each tag. The reason is
 * the store every consumer sits behind: it passes on a line that changed and
 * drops one that did not. A per-tag line says "tag 4 is no longer selected" by
 * not mentioning tag 4, and an omission is not something a store can see, so
 * moving between two empty tags reached nobody. Here any change to any part of
 * it changes this line.
 */
static void
publist(const char *key, uint32_t mask)
{
	uint32_t bit;
	int n, first = 1;

	pubf("\t%s=", key);
	for (n = 1; n <= TAGCOUNT; n++) {
		bit = (uint32_t)1 << (n - 1);
		if (!(mask & bit))
			continue;
		if (!first)
			pubout(",", 1);
		pubf("%d", n);
		first = 0;
	}
}

/*
 * The keyboard, one line for each key the config bound:
 *
 *   bind\tSUPER + SHIFT + R\tdesc=Reload
 *
 * Published when the config loads and when a reader connects, not on every
 * change: the keyboard is only new when the config is. A consumer that keeps
 * facts keeps these, so a menu that wants to list them asks nobody.
 *
 * A session on config.h's fallback binds publishes nothing here, because
 * nothing named them. That is honest: those are what you get when the config
 * did not load, and they are not what the config says.
 */
static void
pubbinds(void)
{
	size_t i;

	for (i = 0; i < nluakeys; i++) {
		if (!luanames[i].spec)
			continue;
		/* The buffer truncates rather than growing, so a long keyboard
		 * goes out in pieces instead of stopping halfway. */
		if (publen > sizeof(pubbuf) - 512)
			pubflush();
		pubout("bind\t", 5);
		putsafe(luanames[i].spec);
		pubout("\tdesc=", 6);
		putsafe(luanames[i].desc ? luanames[i].desc : "");
		pubout("\n", 1);
	}
	pubflush();
}

static void
pubtags(const char *mon, uint32_t occ, uint32_t sel, uint32_t urg)
{
	pubf("tags\t%s", mon);
	publist("used", occ);
	publist("sel", sel);
	publist("urgent", urg);
	pubout("\n", 1);
}

static void
publish(void)
{
	Monitor *m;
	Client *c;
	uint32_t occ, urg, sel;

	wl_list_for_each(m, &mons, link) {
		const char *name = m->wlr_output->name;

		occ = urg = 0;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			occ |= c->tags;
			if (c->isurgent)
				urg |= c->tags;
		}
		c = focustop(m);
		/* Which tags are being looked at is a fact about the monitor, not
		 * about whatever window happens to be focused. With no clients at
		 * all the old reading published nothing, so a bar could not tell
		 * which tag it was on. */
		sel = m->tagset[m->seltags];

		pubf("mon\t%s\tw=%d\th=%d\n", name, m->m.width, m->m.height);
		if (m == selmon)
			pubf("focus\t%s\n", name);
		pubtags(name, occ, sel, urg);
		pubf("layout\t%s\tname=%s\n", name, m->lt[m->sellt]->name);

		/* X4: title and appid churn, so they are their own facts and a
		 * consumer that does not draw them ignores two lines. */
		pubf("title\t%s\ttext=", name);
		putsafe(c ? client_get_title(c) : "");
		pubout("\n", 1);
		pubf("app\t%s\tid=", name);
		putsafe(c ? client_get_appid(c) : "");
		pubout("\n", 1);
		pubf("win\t%s\tfullscreen=%d\tfloating=%d\n", name,
				c ? c->isfullscreen : 0, c ? c->isfloating : 0);
	}

	/* What a screen recorder can point at. One line per window rather than
	 * per monitor, because the subject is the window: the identifier is the
	 * one ext-foreign-toplevel-list hands its clients, and it is what a
	 * portal names a capture target by. A window with no handle is one that
	 * has not mapped, and there is nothing to capture yet. */
	wl_list_for_each(c, &clients, link) {
		if (!c->foreign)
			continue;
		pubf("cap\t%s\tmon=%s\tapp=", c->foreign->identifier,
				c->mon ? c->mon->wlr_output->name : "");
		putsafe(client_get_appid(c));
		pubout("\ttitle=", 7);
		putsafe(client_get_title(c));
		pubout("\n", 1);
	}
	pubflush();
}

/* A reader connects, gets the whole picture, and then gets it again on every
 * change. Nothing is queued and nothing is replied to. */
static int
pubaccept(int fd, uint32_t mask, void *data)
{
	int c = accept(fd, NULL, NULL);

	if (c < 0)
		return 0;
	if (nreaders == MAXREADERS) {
		close(c);
		return 0;
	}
	readers[nreaders++] = c;

	/* The session opening the spec asks for: a version line, the whole
	 * current state, then sync. Every publish is already a full picture, so
	 * the dump is just a publish. */
	publen = 0;
	pubf("version\t%d\thedl\tproto=%d\n", KIPP_VERSION, KIPP_VERSION);
	if (!pubsend(c)) {
		close(c);
		nreaders--;
		publen = 0;
		return 0;
	}
	publen = 0;
	publish();
	publen = 0;
	pubbinds();
	publen = 0;
	pubf("sync\tstate\n");
	pubflush();
	return 0;
}

static void
pubsetup(void)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};

	if (!*cmddir)
		return;   /* cmdsetup gave up, so the directory is not ours */
	snprintf(pubpath, sizeof(pubpath), "%s/kipp", cmddir);
	if (strlen(pubpath) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "hedl: %s is too long for a socket\n", pubpath);
		return;
	}
	strcpy(addr.sun_path, pubpath);

	unlink(pubpath);
	if ((publisten = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0
			|| bind(publisten, (struct sockaddr *)&addr, sizeof(addr)) < 0
			|| listen(publisten, 8) < 0) {
		fprintf(stderr, "hedl: cannot listen on %s\n", pubpath);
		if (publisten >= 0)
			close(publisten);
		publisten = -1;
		return;
	}
	wl_event_loop_add_fd(event_loop, publisten, WL_EVENT_READABLE, pubaccept, NULL);
	fprintf(stderr, "hedl: %s\n", pubpath);
}

static void
pubcleanup(void)
{
	/* Closing is the only end-of-session anyone gets, so it has to happen. */
	while (nreaders)
		close(readers[--nreaders]);
	if (publisten >= 0) {
		close(publisten);
		unlink(pubpath);
	}
	publisten = -1;
}
