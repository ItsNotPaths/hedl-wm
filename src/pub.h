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
 * Still on stdout, still through -s. Phase A proved that path works untouched,
 * and it is the whole publication mechanism.
 */

#define KIPP_VERSION 1

/*
 * X5: a window title is whatever the client says it is, so a newline in one
 * would forge a status line and a tab would forge a field. Neither survives.
 */
static void
putsafe(const char *s)
{
	if (!s)
		return;
	for (; *s; s++)
		putchar((unsigned char)*s < 0x20 || *s == 0x7f ? ' ' : *s);
}

static void
publish(void)
{
	static int announced;
	Monitor *m;
	Client *c;
	uint32_t occ, urg, sel;

	if (!announced) {
		printf("version\t%d\n", KIPP_VERSION);
		announced = 1;
	}

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

		/* Split by rate: title and appid churn, the rest does not, so a
		 * consumer can wake only for what it draws (X4). */
		c = focustop(m);
		printf("title\t%s\t", name);
		putsafe(c ? client_get_title(c) : "");
		putchar('\n');
		printf("appid\t%s\t", name);
		putsafe(c ? client_get_appid(c) : "");
		putchar('\n');

		sel = c ? c->tags : 0;
		printf("fullscreen\t%s\t%d\n", name, c ? c->isfullscreen : 0);
		printf("floating\t%s\t%d\n", name, c ? c->isfloating : 0);
		printf("selmon\t%s\t%d\n", name, m == selmon);
		printf("tags\t%s\t%"PRIu32"\t%"PRIu32"\t%"PRIu32"\t%"PRIu32"\n",
				name, occ, m->tagset[m->seltags], sel, urg);
		printf("layout\t%s\t%s\n", name, m->lt[m->sellt]->name);
	}
	fflush(stdout);
}
