/*
 * The command channel (D2, D3).
 *
 *   printf 'tag\t4\n' > /run/user/1000/hedl/cmd
 *   wweft --send /run/user/1000/hedl/cmd "view	2"
 *
 * One command a line, kind first, tab separated, dispatched through the same
 * registry the keyboard uses (D15). A key press and a command line therefore
 * share one parser, and anything that can write a line can drive the window
 * manager without a library, a socket handshake or a generated protocol.
 *
 * The FIFO is opened O_RDWR so hedl holds a writer of its own and never sees
 * EOF when the last real writer closes. Its fd goes on the event loop dwl
 * already owns, so this adds no poll and no thread.
 *
 * Path is derivable, per D3: /run/user/$UID/hedl/, with $HEDLDIR as an
 * override for a nested instance and never the primary mechanism.
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define CMDMAX 1024   /* X9: a line over PIPE_BUF can interleave with another */

static int cmdfd = -1;
static char cmddir[256];
static char cmdpath[288];
static char cmdlock[288];

static void
cmdparse(char *line)
{
	const Action *a;
	char *arg;
	Arg v = {0};
	const Layout *l;
	int n, i;

	if (!(arg = strchr(line, '\t')))
		arg = strchr(line, ' ');
	if (arg)
		*arg++ = '\0';
	if (!*line)
		return;
	if (!(a = actionbyname(line))) {
		fprintf(stderr, "hedl: cmd: no action named '%s'\n", line);
		return;
	}

	switch (a->kind) {
	case ARG_NONE:
		break;
	case ARG_I:
		v.i = arg ? atoi(arg) : 0;
		break;
	case ARG_UI:
		v.ui = arg ? (uint32_t)strtoul(arg, NULL, 0) : 0;
		break;
	case ARG_TAG:
		/* A person counts workspaces from 1, as in Lua. 0 is all of them. */
		n = arg ? atoi(arg) : 0;
		v.ui = n <= 0 ? ~0u : (uint32_t)1 << (n - 1);
		break;
	case ARG_F:
		v.f = arg ? strtof(arg, NULL) : 0;
		break;
	case ARG_LAYOUT:
		if (!arg)
			break;            /* no layout means toggle, as dwl has it */
		for (l = layouts; l < END(layouts); l++)
			if (!strcmp(l->name, arg))
				v.v = l;
		for (i = 0; i < nlualayouts; i++)
			if (!strcmp(lualayoutnames[i], arg))
				v.v = &lualayouts[i];
		if (!v.v) {
			fprintf(stderr, "hedl: cmd: no layout named '%s'\n", arg);
			return;
		}
		break;
	case ARG_CMD: {
		/* The rest of the line goes to a shell, the same as a Lua string
		 * dispatcher, so a command can carry a pipe. */
		static const char *argv[4];
		if (!arg)
			return;
		argv[0] = "/bin/sh";
		argv[1] = "-c";
		argv[2] = arg;
		argv[3] = NULL;
		v.v = argv;
		break;
	}
	}
	a->func(&v);
}

static int
cmdread(int fd, uint32_t mask, void *data)
{
	static char buf[CMDMAX * 2];
	static size_t len;
	char *nl, *start;
	ssize_t got;

	while ((got = read(fd, buf + len, sizeof(buf) - len - 1)) > 0) {
		len += (size_t)got;
		buf[len] = '\0';
		start = buf;
		while ((nl = strchr(start, '\n'))) {
			*nl = '\0';
			if (nl - start > CMDMAX)
				fprintf(stderr, "hedl: cmd: line over %d bytes, ignored\n", CMDMAX);
			else
				cmdparse(start);
			start = nl + 1;
		}
		len = strlen(start);
		memmove(buf, start, len + 1);
		/* A writer sending more than the buffer holds without a newline is
		 * either broken or hostile. Drop what we have and resynchronise. */
		if (len >= sizeof(buf) - 1) {
			fprintf(stderr, "hedl: cmd: no newline in %zu bytes, dropped\n", len);
			len = 0;
		}
	}
	return 0;
}

/* X6: two instances would clobber one FIFO in silence, where a socket would
 * refuse to bind. The lock says who owns it, and a dead owner is replaced. */
static int
cmdclaim(void)
{
	char buf[32];
	int fd, n;
	pid_t other;

	while ((fd = open(cmdlock, O_CREAT | O_EXCL | O_WRONLY, 0600)) < 0) {
		if (errno != EEXIST)
			return 0;
		if ((fd = open(cmdlock, O_RDONLY)) < 0)
			return 0;
		n = (int)read(fd, buf, sizeof(buf) - 1);
		close(fd);
		buf[n > 0 ? n : 0] = '\0';
		other = (pid_t)atoi(buf);
		if (other > 0 && kill(other, 0) == 0) {
			fprintf(stderr, "hedl: %s is held by pid %d, no command channel\n",
					cmddir, (int)other);
			return 0;
		}
		unlink(cmdlock);
	}
	n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
	if (write(fd, buf, (size_t)n) != n)
		fprintf(stderr, "hedl: could not write %s\n", cmdlock);
	close(fd);
	return 1;
}

static void
cmdsetup(void)
{
	const char *dir = getenv("HEDLDIR"), *run = getenv("XDG_RUNTIME_DIR");

	if (dir)
		snprintf(cmddir, sizeof(cmddir), "%s", dir);
	else
		snprintf(cmddir, sizeof(cmddir), "%s/hedl", run ? run : "/tmp");
	snprintf(cmdpath, sizeof(cmdpath), "%s/cmd", cmddir);
	snprintf(cmdlock, sizeof(cmdlock), "%s/lock", cmddir);

	if (mkdir(cmddir, 0700) < 0 && errno != EEXIST) {
		fprintf(stderr, "hedl: cannot make %s, no command channel\n", cmddir);
		return;
	}
	if (!cmdclaim())
		return;

	unlink(cmdpath);
	if (mkfifo(cmdpath, 0600) < 0) {
		fprintf(stderr, "hedl: cannot make %s\n", cmdpath);
		unlink(cmdlock);
		return;
	}
	/* O_RDWR, so this process is also a writer and read() never returns 0
	 * when the last outside writer closes. */
	if ((cmdfd = open(cmdpath, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) {
		fprintf(stderr, "hedl: cannot open %s\n", cmdpath);
		unlink(cmdpath);
		unlink(cmdlock);
		return;
	}
	wl_event_loop_add_fd(event_loop, cmdfd, WL_EVENT_READABLE, cmdread, NULL);
	setenv("HEDLDIR", cmddir, 1);
	fprintf(stderr, "hedl: %s\n", cmdpath);
}

static void
cmdcleanup(void)
{
	if (cmdfd < 0)
		return;
	close(cmdfd);
	cmdfd = -1;
	unlink(cmdpath);
	unlink(cmdlock);
}
