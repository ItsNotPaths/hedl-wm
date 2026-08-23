/*
 * The Lua config.
 *
 * Shaped after Hyprland's `hl.*` (D14), because that shape is proven and
 * already familiar on this machine. The global is `hedl`:
 *
 *   hedl.bind("SUPER + Return", "Terminal", hedl.dsp.spawn("foot"))
 *   hedl.bind("SUPER + J", "Focus next",    hedl.dsp.focusstack(1))
 *   hedl.unbind("SUPER + P")
 *
 * A dispatcher is a value, not a call (D15). hedl.dsp.<name>(arg) coerces the
 * argument by the kind bind.h records and hands back a bound Arg, so the same
 * object can be given to a bind, to a rule, or later to the command channel.
 *
 * Nothing here can leave the session without keys. A missing file, a syntax
 * error or a config that binds nothing all leave config.h's keys[] in charge,
 * which is what D14 means by keeping it as the fallback.
 *
 * require() stays, unlike in wweft's sandbox, because D14's layering is built
 * on it: our defaults load first and the user's file overrides them.
 */

#include <regex.h>
#include <strings.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#define CLIENT_MT   "hedl.client"
#define MONITOR_MT  "hedl.monitor"
#define DISPATCH_MT "hedl.dispatch"
#define SYSCONF     "/usr/share/hedl"

typedef struct {
	const Action *a;
	Arg arg;
} Dispatch;

static lua_State *L;

#define MAXLUALAYOUTS 16
#define LUASTEPS      2000000  /* generous for geometry, fatal for a loop */

static Layout lualayouts[MAXLUALAYOUTS];
static char lualayoutnames[MAXLUALAYOUTS][32];
static int lualayoutrefs[MAXLUALAYOUTS];
static int nlualayouts;

/* ---------------------------------------------------------------- callback */

static void
luacall(int ref)
{
	if (!L || ref == LUA_NOREF)
		return;
	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		fprintf(stderr, "hedl: %s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
	}
}

static void
luadrop(int ref)
{
	if (L && ref != LUA_NOREF)
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

/* ----------------------------------------------------------------- events */

/*
 * hedl.on(event, fn). Events fan out, so a name holds a list and every
 * listener runs. That is the difference from a layout, which has to be exactly
 * one function because something must decide the geometry.
 *
 * Every hook point is a place dwl already calls printstatus(), which is the
 * complete list of things that happen at human rate.
 */
static const char *events[] = {"start", "map", "unmap", "focus", "title", "urgent"};

static int hooks[LENGTH(events)][16];
static int nhooks[LENGTH(events)];

static void
hooksclear(void)
{
	size_t e;
	int i;

	for (e = 0; e < LENGTH(events); e++) {
		for (i = 0; i < nhooks[e]; i++)
			luadrop(hooks[e][i]);
		nhooks[e] = 0;
	}
}

static void pushclient(lua_State *S, Client *c);
static int tocolor(lua_State *S, int idx, float out[4]);
static int field(lua_State *S, const char *k);

/* A listener that throws says so and the rest still run. */
static void
emit(int e, Client *c)
{
	Client *live;
	int i, found = 0;

	if (!L || !nhooks[e])
		return;
	/* A title can be set before a window is mapped and after it is gone, so
	 * check before handing it over rather than making every listener do it. */
	if (c) {
		wl_list_for_each(live, &clients, link)
			if (live == c)
				found = 1;
		if (!found)
			return;
	}
	for (i = 0; i < nhooks[e]; i++) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, hooks[e][i]);
		pushclient(L, c);
		if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
			fprintf(stderr, "hedl: on(%s): %s\n", events[e], lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}
}

static int
l_on(lua_State *S)
{
	const char *name = luaL_checkstring(S, 1);
	size_t e;

	luaL_checktype(S, 2, LUA_TFUNCTION);
	for (e = 0; e < LENGTH(events); e++) {
		if (strcmp(events[e], name))
			continue;
		if (nhooks[e] == (int)LENGTH(hooks[e]))
			return luaL_error(S, "too many listeners on '%s'", name);
		lua_pushvalue(S, 2);
		hooks[e][nhooks[e]++] = luaL_ref(S, LUA_REGISTRYINDEX);
		return 0;
	}
	return luaL_error(S, "no event named '%s'", name);
}

/* ---------------------------------------------------------------- windows */

/*
 * A window handed to Lua is a pointer plus a check. Clients die whenever a
 * program exits, and a script holding a stale one would otherwise read freed
 * memory, so every access walks the list first. It is O(n) on a list that is
 * never long, at human rate, which is the trade dwl already makes everywhere.
 */
static Client *
checkclient(lua_State *S, int idx)
{
	Client **p = luaL_checkudata(S, idx, CLIENT_MT), *c;

	wl_list_for_each(c, &clients, link)
		if (c == *p)
			return c;
	luaL_error(S, "that window is gone");
	return NULL;
}

static void
pushclient(lua_State *S, Client *c)
{
	Client **p;

	if (!c) {
		lua_pushnil(S);
		return;
	}
	p = lua_newuserdatauv(S, sizeof(*p), 0);
	*p = c;
	luaL_setmetatable(S, CLIENT_MT);
}

static int
l_client_focus(lua_State *S)
{
	Client *c = checkclient(S, 1);
	if (c->mon)
		selmon = c->mon;
	focusclient(c, 1);
	return 0;
}

static int
l_client_kill(lua_State *S)
{
	client_send_close(checkclient(S, 1));
	return 0;
}

/* One resize for a whole rectangle. Setting x, y, width and height one at a
 * time is four of them, which a layout doing this per window would feel. */
static int
l_client_place(lua_State *S)
{
	Client *c = checkclient(S, 1);
	struct wlr_box g;

	g.x = (int)luaL_checkinteger(S, 2);
	g.y = (int)luaL_checkinteger(S, 3);
	g.width = (int)luaL_checkinteger(S, 4);
	g.height = (int)luaL_checkinteger(S, 5);
	resize(c, g, 0);
	return 0;
}

static int
l_client_tostring(lua_State *S)
{
	Client *c = checkclient(S, 1);
	lua_pushfstring(S, "window(%s: %s)", client_get_appid(c), client_get_title(c));
	return 1;
}

static int
l_client_index(lua_State *S)
{
	Client *c = checkclient(S, 1);
	const char *k = luaL_checkstring(S, 2);

	if (!strcmp(k, "appid"))      lua_pushstring(S, client_get_appid(c));
	else if (!strcmp(k, "title")) lua_pushstring(S, client_get_title(c));
	else if (!strcmp(k, "tags"))  lua_pushinteger(S, c->tags);
	else if (!strcmp(k, "floating"))   lua_pushboolean(S, c->isfloating);
	else if (!strcmp(k, "fullscreen")) lua_pushboolean(S, c->isfullscreen);
	else if (!strcmp(k, "urgent"))     lua_pushboolean(S, c->isurgent);
	else if (!strcmp(k, "x"))      lua_pushinteger(S, c->geom.x);
	else if (!strcmp(k, "y"))      lua_pushinteger(S, c->geom.y);
	else if (!strcmp(k, "width"))  lua_pushinteger(S, c->geom.width);
	else if (!strcmp(k, "height")) lua_pushinteger(S, c->geom.height);
	else if (!strcmp(k, "focused")) lua_pushboolean(S, c == focustop(selmon));
	else if (!strcmp(k, "visible")) lua_pushboolean(S, VISIBLEON(c, c->mon));
	else if (!strcmp(k, "monitor"))
		lua_pushstring(S, c->mon ? c->mon->wlr_output->name : NULL);
	else if (!strcmp(k, "borderpx")) lua_pushinteger(S, c->bw);
	else if (!strcmp(k, "focus")) lua_pushcfunction(S, l_client_focus);
	else if (!strcmp(k, "kill"))  lua_pushcfunction(S, l_client_kill);
	else if (!strcmp(k, "place")) lua_pushcfunction(S, l_client_place);
	else lua_pushnil(S);
	return 1;
}

static int
l_client_newindex(lua_State *S)
{
	Client *c = checkclient(S, 1);
	const char *k = luaL_checkstring(S, 2);
	struct wlr_box g = c->geom;

	if (!strcmp(k, "tags")) {
		c->tags = (uint32_t)luaL_checkinteger(S, 3);
		focusclient(focustop(selmon), 1);
		arrange(c->mon);
		printstatus();
		return 0;
	}
	if (!strcmp(k, "floating")) {
		setfloating(c, lua_toboolean(S, 3));
		return 0;
	}
	if (!strcmp(k, "fullscreen")) {
		setfullscreen(c, lua_toboolean(S, 3));
		return 0;
	}
	if (!strcmp(k, "borderpx")) {
		c->bw = (unsigned int)luaL_checkinteger(S, 3);
		resize(c, c->geom, 0);
		return 0;
	}
	if (!strcmp(k, "bordercolor")) {
		float col[4];
		tocolor(S, 3, col);
		client_set_border_color(c, col);
		return 0;
	}
	/* Geometry only means anything while the window is floating, which is
	 * the same rule dwl's own resize() follows. */
	if (!strcmp(k, "x"))           g.x = (int)luaL_checkinteger(S, 3);
	else if (!strcmp(k, "y"))      g.y = (int)luaL_checkinteger(S, 3);
	else if (!strcmp(k, "width"))  g.width = (int)luaL_checkinteger(S, 3);
	else if (!strcmp(k, "height")) g.height = (int)luaL_checkinteger(S, 3);
	else return luaL_error(S, "a window has no '%s' to set", k);

	resize(c, g, 1);
	return 0;
}

/* ------------------------------------------------------------------ state */

static int
l_focused(lua_State *S)
{
	pushclient(S, focustop(selmon));
	return 1;
}

/* Every window, in dwl's own tiling order. */
static int
l_clients(lua_State *S)
{
	Client *c;
	int n = 0;

	lua_newtable(S);
	wl_list_for_each(c, &clients, link) {
		pushclient(S, c);
		lua_rawseti(S, -2, ++n);
	}
	return 1;
}

/*
 * A monitor is a handle, not a snapshot, because nmaster and mfact have to be
 * settable. dwl only offers incnmaster and setmfact, which are deltas, and
 * "make this exactly 3" cannot be written with a delta.
 */
static Monitor *
checkmonitor(lua_State *S, int idx)
{
	Monitor **p = luaL_checkudata(S, idx, MONITOR_MT), *m;

	wl_list_for_each(m, &mons, link)
		if (m == *p)
			return m;
	luaL_error(S, "that monitor is gone");
	return NULL;
}

static void
pushmonitor(lua_State *S, Monitor *m)
{
	Monitor **p;

	if (!m) {
		lua_pushnil(S);
		return;
	}
	p = lua_newuserdatauv(S, sizeof(*p), 0);
	*p = m;
	luaL_setmetatable(S, MONITOR_MT);
}

static int
l_monitor_index(lua_State *S)
{
	Monitor *m = checkmonitor(S, 1);
	const char *k = luaL_checkstring(S, 2);

	if (!strcmp(k, "name"))         lua_pushstring(S, m->wlr_output->name);
	else if (!strcmp(k, "tags"))    lua_pushinteger(S, m->tagset[m->seltags]);
	else if (!strcmp(k, "layout"))  lua_pushstring(S, m->lt[m->sellt]->name);
	else if (!strcmp(k, "nmaster")) lua_pushinteger(S, m->nmaster);
	else if (!strcmp(k, "mfact"))   lua_pushnumber(S, m->mfact);
	else if (!strcmp(k, "focused")) lua_pushboolean(S, m == selmon);
	else if (!strcmp(k, "x"))       lua_pushinteger(S, m->m.x);
	else if (!strcmp(k, "y"))       lua_pushinteger(S, m->m.y);
	else if (!strcmp(k, "width"))   lua_pushinteger(S, m->m.width);
	else if (!strcmp(k, "height"))  lua_pushinteger(S, m->m.height);
	/* The window area, which is the monitor minus any layer-shell strut. */
	else if (!strcmp(k, "wx"))      lua_pushinteger(S, m->w.x);
	else if (!strcmp(k, "wy"))      lua_pushinteger(S, m->w.y);
	else if (!strcmp(k, "wwidth"))  lua_pushinteger(S, m->w.width);
	else if (!strcmp(k, "wheight")) lua_pushinteger(S, m->w.height);
	else lua_pushnil(S);
	return 1;
}

static int
l_monitor_newindex(lua_State *S)
{
	Monitor *m = checkmonitor(S, 1);
	const char *k = luaL_checkstring(S, 2);

	if (!strcmp(k, "nmaster")) {
		m->nmaster = MAX(0, (int)luaL_checkinteger(S, 3));
	} else if (!strcmp(k, "mfact")) {
		float f = (float)luaL_checknumber(S, 3);
		m->mfact = f < 0.05f ? 0.05f : f > 0.95f ? 0.95f : f;
	} else {
		return luaL_error(S, "a monitor has no '%s' to set", k);
	}
	arrange(m);
	printstatus();
	return 0;
}

static int
l_monitor_tostring(lua_State *S)
{
	lua_pushfstring(S, "monitor(%s)", checkmonitor(S, 1)->wlr_output->name);
	return 1;
}

static int
l_monitor(lua_State *S)
{
	pushmonitor(S, selmon);
	return 1;
}

static int
l_monitors(lua_State *S)
{
	Monitor *m;
	int n = 0;

	lua_newtable(S);
	wl_list_for_each(m, &mons, link) {
		pushmonitor(S, m);
		lua_rawseti(S, -2, ++n);
	}
	return 1;
}

/* The layouts this build has, so a script can cycle them without a list. */
static int
l_layouts(lua_State *S)
{
	const Layout *l;
	int n = 0, i;

	lua_newtable(S);
	for (l = layouts; l < END(layouts); l++) {
		lua_pushstring(S, l->name);
		lua_rawseti(S, -2, ++n);
	}
	for (i = 0; i < nlualayouts; i++) {
		lua_pushstring(S, lualayoutnames[i]);
		lua_rawseti(S, -2, ++n);
	}
	return 1;
}

/* ---------------------------------------------------------- monitor rules */

/*
 * hedl.monitor_rule("eDP-1", { scale = 1.5, layout = "tile", x = 0, y = 0 })
 *
 * Replaces config.h's monrules[]. Name matching is a substring, as dwl does
 * it, and a nil name is the fallback every monitor falls through to. Applied
 * in createmon, so a rule reaches a screen plugged in later as well.
 */
typedef struct {
	char name[64];   /* empty means every monitor */
	int ref;
} MonRule;

static MonRule *mrules;
static size_t nmrules;

static void
mrulesclear(void)
{
	while (nmrules)
		luadrop(mrules[--nmrules].ref);
	free(mrules);
	mrules = NULL;
}

static int
l_monitor_rule(lua_State *S)
{
	const char *name = lua_isnoneornil(S, 1) ? "" : luaL_checkstring(S, 1);
	MonRule *grown;

	luaL_checktype(S, 2, LUA_TTABLE);
	if (strlen(name) >= sizeof(mrules->name))
		return luaL_error(S, "monitor name '%s' is too long", name);
	if (!(grown = realloc(mrules, (nmrules + 1) * sizeof(*mrules))))
		die("monitor_rule:");
	mrules = grown;
	strcpy(mrules[nmrules].name, name);
	lua_pushvalue(S, 2);
	mrules[nmrules].ref = luaL_ref(S, LUA_REGISTRYINDEX);
	nmrules++;
	return 0;
}

/* Returns the scale to set, or 0 for "leave it alone". */
static float
mrulesapply(Monitor *m, const char *name)
{
	float scale = 0;
	size_t i;

	for (i = 0; i < nmrules; i++) {
		if (mrules[i].name[0] && !strstr(name, mrules[i].name))
			continue;
		lua_rawgeti(L, LUA_REGISTRYINDEX, mrules[i].ref);
		if (field(L, "mfact"))   { m->mfact = (float)lua_tonumber(L, -1); lua_pop(L, 1); }
		if (field(L, "nmaster")) { m->nmaster = (int)lua_tointeger(L, -1); lua_pop(L, 1); }
		if (field(L, "x"))       { m->m.x = (int)lua_tointeger(L, -1); lua_pop(L, 1); }
		if (field(L, "y"))       { m->m.y = (int)lua_tointeger(L, -1); lua_pop(L, 1); }
		if (field(L, "scale"))   { scale = (float)lua_tonumber(L, -1); lua_pop(L, 1); }
		if (field(L, "layout")) {
			const char *want = lua_tostring(L, -1);
			const Layout *l;
			int j;
			for (l = layouts; l < END(layouts); l++)
				if (!strcmp(l->name, want))
					m->lt[0] = l;
			for (j = 0; j < nlualayouts; j++)
				if (!strcmp(lualayoutnames[j], want))
					m->lt[0] = &lualayouts[j];
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	return scale;
}

static int
l_env(lua_State *S)
{
	setenv(luaL_checkstring(S, 1), luaL_checkstring(S, 2), 1);
	return 0;
}

/* ------------------------------------------------------------------ rules */

/*
 * hedl.window_rule({ class = "^steam$", title = "^Steam$" }, { tile = true })
 *
 * Replaces dwl's static rules[] array. Matching is the same regex machinery
 * the opacity lists use, on app-id and title separately rather than either,
 * so a rule can name both and mean both. Applied in applyrules, which runs
 * once when a window maps.
 *
 * Properties: tags, floating, tile, opacity, borderpx, bordercolor, monitor.
 */
typedef struct {
	regex_t id, title;
	int hasid, hastitle;
	int ref;            /* the property table */
} WindowRule;

static WindowRule *wrules;
static size_t nwrules;

static void
wrulesclear(void)
{
	while (nwrules) {
		nwrules--;
		if (wrules[nwrules].hasid)
			regfree(&wrules[nwrules].id);
		if (wrules[nwrules].hastitle)
			regfree(&wrules[nwrules].title);
		luadrop(wrules[nwrules].ref);
	}
	free(wrules);
	wrules = NULL;
}

static int
l_window_rule(lua_State *S)
{
	WindowRule *r, *grown;
	const char *pat;

	luaL_checktype(S, 1, LUA_TTABLE);
	luaL_checktype(S, 2, LUA_TTABLE);
	if (!(grown = realloc(wrules, (nwrules + 1) * sizeof(*wrules))))
		die("window_rule:");
	wrules = grown;
	r = &wrules[nwrules];
	memset(r, 0, sizeof(*r));

	lua_pushvalue(S, 1);
	if (field(S, "class")) {
		pat = luaL_checkstring(S, -1);
		if (regcomp(&r->id, pat, REG_EXTENDED | REG_NOSUB))
			return luaL_error(S, "bad class pattern '%s'", pat);
		r->hasid = 1;
		lua_pop(S, 1);
	}
	if (field(S, "title")) {
		pat = luaL_checkstring(S, -1);
		if (regcomp(&r->title, pat, REG_EXTENDED | REG_NOSUB)) {
			if (r->hasid)
				regfree(&r->id);
			return luaL_error(S, "bad title pattern '%s'", pat);
		}
		r->hastitle = 1;
		lua_pop(S, 1);
	}
	lua_pop(S, 1);
	if (!r->hasid && !r->hastitle) {
		return luaL_error(S, "a window rule needs a class or a title to match");
	}

	lua_pushvalue(S, 2);
	r->ref = luaL_ref(S, LUA_REGISTRYINDEX);
	nwrules++;
	return 0;
}

/*
 * Called from the end of applyrules, after setmon, and that order matters.
 * setmon calls setfullscreen, which does `c->bw = fullscreen ? 0 : borderpx`
 * unconditionally, and it assigns c->tags itself. Running the rules before it
 * meant every borderpx and every tags a rule set was silently stamped over.
 */
static void
wrulesapply(Client *c)
{
	const char *appid = client_get_appid(c), *title = client_get_title(c);
	size_t i;
	int v, moved = 0;

	for (i = 0; i < nwrules; i++) {
		WindowRule *r = &wrules[i];
		if (r->hasid && (!appid || regexec(&r->id, appid, 0, NULL, 0)))
			continue;
		if (r->hastitle && (!title || regexec(&r->title, title, 0, NULL, 0)))
			continue;

		lua_rawgeti(L, LUA_REGISTRYINDEX, r->ref);
		if (field(L, "tags")) {
			v = (int)lua_tointeger(L, -1);
			c->tags = v <= 0 ? c->tags : (uint32_t)1 << (v - 1);
			moved = 1;
			lua_pop(L, 1);
		}
		/* Both spellings, because "tile = true" and "floating = false" are
		 * the same wish and people reach for whichever fits the sentence. */
		if (field(L, "floating")) {
			setfloating(c, lua_toboolean(L, -1));
			lua_pop(L, 1);
		}
		if (field(L, "tile")) {
			setfloating(c, !lua_toboolean(L, -1));
			lua_pop(L, 1);
		}
		if (field(L, "borderpx")) {
			c->bw = (unsigned int)lua_tointeger(L, -1);
			moved = 1;
			lua_pop(L, 1);
		}
		if (field(L, "bordercolor")) {
			float col[4];
			tocolor(L, -1, col);
			client_set_border_color(c, col);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	}
	if (moved && c->mon) {
		c->rulefloat = c->isfloating;
		arrange(c->mon);
		printstatus();
	}
}

/* -------------------------------------------------------------- animation */

/*
 * One curve (D16): x += (target - x) / divisor, snapping once it is inside a
 * couple of pixels or it inches forever on integers. Stepped in rendermon,
 * which dwl already runs per output at refresh rate, so this adds no loop.
 *
 * Only the box a window is drawn in moves. The surface is told its target size
 * once, so a client renders on the change and not on every frame of it.
 */
static int
animate(Client *c)
{
	return anim_enabled && !c->isfullscreen;
}

static int
step(int now, int target)
{
	int d = target - now;

	if (d > -anim_snap && d < anim_snap)
		return target;
	/* Round away from zero, or the last pixel never arrives. */
	return now + (d > 0 ? (d + anim_divisor - 1) : (d - anim_divisor + 1)) / anim_divisor;
}

static int
animstep(Monitor *m)
{
	Client *c;
	int moving = 0;

	if (!anim_enabled)
		return 0;
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || !client_surface(c)->mapped)
			continue;
		if (c->anim.x == c->geom.x && c->anim.y == c->geom.y
				&& c->anim.width == c->geom.width
				&& c->anim.height == c->geom.height)
			continue;
		c->anim.x = step(c->anim.x, c->geom.x);
		c->anim.y = step(c->anim.y, c->geom.y);
		c->anim.width = step(c->anim.width, c->geom.width);
		c->anim.height = step(c->anim.height, c->geom.height);
		drawgeom(c);
		moving = 1;
	}
	return moving;
}

/* ---------------------------------------------------------------- layouts */

/*
 * A layout in Lua (D12).
 *
 *   hedl.layout("dwindle", function(ctx)
 *     for i, c in ipairs(ctx.clients) do c:place(...) end
 *   end)
 *
 * arrange() calls the layout once and the loop over windows is inside it, so
 * this is one Lua call per arrange, not one per window. During an interactive
 * drag that is once a frame, and a pure Lua function returning a few
 * rectangles is microseconds.
 *
 * The hazard was never throughput, it is a script that never returns: the
 * event loop is single threaded, so `while true do end` in a layout would
 * freeze the whole desktop with no way out. The instruction-count hook below
 * is the answer. It stops a runaway, says which layout did it, and that
 * layout falls back to tile until the config is fixed.
 *
 * Entries live in a fixed array because a monitor holds a `const Layout *`.
 * Rebuilding the list on reload would leave those pointers dangling, so the
 * addresses stay put and only the Lua reference behind them is replaced.
 */
static void
luawatchdog(lua_State *S, lua_Debug *ar)
{
	lua_sethook(S, NULL, 0, 0);
	luaL_error(S, "layout ran for %d steps without finishing", LUASTEPS);
}

static void
luaarrange(Monitor *m)
{
	const Layout *lt = m->lt[m->sellt];
	Client *c;
	int idx = (int)(lt - lualayouts), n = 0;

	if (idx < 0 || idx >= nlualayouts || !L)
		return;
	if (lualayoutrefs[idx] == LUA_NOREF) {
		tile(m);   /* the config went away under us */
		return;
	}

	lua_rawgeti(L, LUA_REGISTRYINDEX, lualayoutrefs[idx]);
	lua_newtable(L);                                  /* ctx */
	pushmonitor(L, m);           lua_setfield(L, -2, "monitor");
	lua_pushinteger(L, m->w.x);       lua_setfield(L, -2, "x");
	lua_pushinteger(L, m->w.y);       lua_setfield(L, -2, "y");
	lua_pushinteger(L, m->w.width);   lua_setfield(L, -2, "width");
	lua_pushinteger(L, m->w.height);  lua_setfield(L, -2, "height");
	lua_pushinteger(L, m->nmaster);   lua_setfield(L, -2, "nmaster");
	lua_pushnumber(L, m->mfact);      lua_setfield(L, -2, "mfact");

	/* Exactly what tile() iterates: visible, tiled, not fullscreen. */
	lua_newtable(L);
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		pushclient(L, c);
		lua_rawseti(L, -2, ++n);
	}
	lua_setfield(L, -2, "clients");

	if (n == 0) {
		lua_pop(L, 2);
		return;
	}

	lua_sethook(L, luawatchdog, LUA_MASKCOUNT, LUASTEPS);
	if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
		lua_sethook(L, NULL, 0, 0);
		fprintf(stderr, "hedl: layout '%s': %s\n", lualayoutnames[idx],
				lua_tostring(L, -1));
		lua_pop(L, 1);
		/* Stop calling it. A layout that throws every frame would fill the
		 * log faster than anyone can read it. */
		luadrop(lualayoutrefs[idx]);
		lualayoutrefs[idx] = LUA_NOREF;
		tile(m);
		return;
	}
	lua_sethook(L, NULL, 0, 0);
}

static int
l_layout(lua_State *S)
{
	const char *name = luaL_checkstring(S, 1);
	int i;

	luaL_checktype(S, 2, LUA_TFUNCTION);
	for (i = 0; i < nlualayouts; i++)
		if (!strcmp(lualayoutnames[i], name))
			break;
	if (i == nlualayouts) {
		const Layout *l;
		for (l = layouts; l < END(layouts); l++)
			if (!strcmp(l->name, name))
				return luaL_error(S, "'%s' is a built-in layout", name);
		if (nlualayouts == MAXLUALAYOUTS)
			return luaL_error(S, "no room for more layouts");
		if (strlen(name) >= sizeof(lualayoutnames[0]))
			return luaL_error(S, "layout name '%s' is too long", name);
		strcpy(lualayoutnames[i], name);
		lualayouts[i].name = lualayoutnames[i];
		lualayouts[i].symbol = lualayoutnames[i];
		lualayouts[i].arrange = luaarrange;
		nlualayouts++;
	} else {
		luadrop(lualayoutrefs[i]);
	}
	lua_pushvalue(S, 2);
	lualayoutrefs[i] = luaL_ref(S, LUA_REGISTRYINDEX);
	return 0;
}

/* ------------------------------------------------------------------ keys */

static uint32_t
modbyname(const char *s)
{
	if (!strcasecmp(s, "SUPER") || !strcasecmp(s, "LOGO"))  return WLR_MODIFIER_LOGO;
	if (!strcasecmp(s, "SHIFT"))                            return WLR_MODIFIER_SHIFT;
	if (!strcasecmp(s, "CTRL") || !strcasecmp(s, "CONTROL"))return WLR_MODIFIER_CTRL;
	if (!strcasecmp(s, "ALT") || !strcasecmp(s, "MOD1"))    return WLR_MODIFIER_ALT;
	if (!strcasecmp(s, "CAPS"))                             return WLR_MODIFIER_CAPS;
	if (!strcasecmp(s, "MOD2"))                             return WLR_MODIFIER_MOD2;
	if (!strcasecmp(s, "MOD3"))                             return WLR_MODIFIER_MOD3;
	if (!strcasecmp(s, "MOD5"))                             return WLR_MODIFIER_MOD5;
	return 0;
}

/*
 * "SUPER + SHIFT + Q" -> mods and a keysym. The last token is the key and
 * everything before it is a modifier, so a stray name is an error rather than
 * a bind that never fires.
 */
static int
parsekey(const char *spec, uint32_t *mod, xkb_keysym_t *sym)
{
	char buf[256], *tok, *last = NULL;
	uint32_t m = 0, one;

	if (strlen(spec) >= sizeof(buf))
		return 0;
	strcpy(buf, spec);

	/* Every token but the last is a modifier, so a misspelled one is an
	 * error rather than a bind that quietly never fires. */
	for (tok = buf;;) {
		char *plus = strchr(tok, '+');
		char *s = tok, *e = plus ? plus : tok + strlen(tok);

		while (s < e && (*s == ' ' || *s == '\t'))
			s++;
		while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
			e--;
		if (s == e)
			return 0;
		*e = '\0';
		if (last) {
			if (!(one = modbyname(last)))
				return 0;
			m |= one;
		}
		last = s;
		if (!plus)
			break;
		tok = plus + 1;
	}
	if (!last || (*sym = xkb_keysym_from_name(last, XKB_KEYSYM_CASE_INSENSITIVE))
			== XKB_KEY_NoSymbol)
		return 0;
	*mod = m;
	return 1;
}

/* ----------------------------------------------------------- dispatchers */

/* Turn one Lua value into the Arg the action expects. */
static int
coerce(lua_State *S, const Action *a, int idx, Arg *arg)
{
	const Layout *l;
	const char **argv;
	int n, i;

	switch (a->kind) {
	case ARG_NONE:
		*arg = (Arg){0};
		return 1;
	case ARG_I:
		arg->i = (int)luaL_checkinteger(S, idx);
		return 1;
	case ARG_UI:
		arg->ui = (uint32_t)luaL_checkinteger(S, idx);
		return 1;
	case ARG_TAG:
		/* A person counts workspaces from 1. 0 means all of them. */
		n = (int)luaL_checkinteger(S, idx);
		arg->ui = n <= 0 ? ~0u : (uint32_t)1 << (n - 1);
		return 1;
	case ARG_F:
		arg->f = (float)luaL_checknumber(S, idx);
		return 1;
	case ARG_LAYOUT:
		if (lua_isnoneornil(S, idx)) {
			*arg = (Arg){0};   /* no layout means toggle, as dwl has it */
			return 1;
		}
		{
			const char *want = luaL_checkstring(S, idx);
			for (l = layouts; l < END(layouts); l++) {
				if (!strcmp(l->name, want)) {
					arg->v = l;
					return 1;
				}
			}
			for (i = 0; i < nlualayouts; i++) {
				if (!strcmp(lualayoutnames[i], want)) {
					arg->v = &lualayouts[i];
					return 1;
				}
			}
		}
		return luaL_error(S, "no layout named '%s'", lua_tostring(S, idx));
	case ARG_CMD:
		/* A string goes through a shell, the way Hyprland's exec_cmd does,
		 * so a bind can carry a pipe. A table is a literal argv. */
		if (lua_istable(S, idx)) {
			n = (int)lua_rawlen(S, idx);
			argv = keep(calloc(n + 1, sizeof(*argv)));
			for (i = 0; i < n; i++) {
				lua_rawgeti(S, idx, i + 1);
				argv[i] = keep(strdup(luaL_checkstring(S, -1)));
				lua_pop(S, 1);
			}
		} else {
			argv = keep(calloc(4, sizeof(*argv)));
			argv[0] = "/bin/sh";
			argv[1] = "-c";
			argv[2] = keep(strdup(luaL_checkstring(S, idx)));
		}
		arg->v = argv;
		return 1;
	}
	return luaL_error(S, "unknown argument kind");
}

/*
 * A dispatcher stays data so a bind, a rule and the command channel can all
 * hold the same object (D15). Calling one runs it, which is what lets a script
 * compose two of them into one bind:
 *
 *   hedl.bind("ALT + 1", "follow", function()
 *     hedl.dsp.tag(1)()
 *     hedl.dsp.view(1)()
 *   end)
 */
static int
l_dispatch_call(lua_State *S)
{
	Dispatch *d = luaL_checkudata(S, 1, DISPATCH_MT);
	d->a->func(&d->arg);
	return 0;
}

static int
l_dispatch(lua_State *S)
{
	const Action *a = lua_touserdata(S, lua_upvalueindex(1));
	Dispatch *d;
	Arg arg;

	/* Coerce before allocating. lua_newuserdatauv pushes, so doing it first
	 * puts the new value at index 1 and a zero-argument dispatcher then
	 * reads itself as its own argument. */
	coerce(S, a, 1, &arg);
	d = lua_newuserdatauv(S, sizeof(*d), 0);
	d->a = a;
	d->arg = arg;
	luaL_setmetatable(S, DISPATCH_MT);
	return 1;
}

/* ----------------------------------------------------------------- config */

/*
 * Colors are "#rrggbb" or "#rrggbbaa". D19's theme file is Omarchy shaped and
 * writes hex strings, so that is the form that has to read well. An integer is
 * accepted too and is 0xRRGGBBAA, matching dwl's COLOR() macro rather than
 * wweft's 0xAARRGGBB.
 */
static int
tocolor(lua_State *S, int idx, float out[4])
{
	unsigned long v;
	const char *h;
	char *end;
	size_t n;

	if (lua_isnumber(S, idx)) {
		v = (unsigned long)lua_tointeger(S, idx);
		n = 8;
	} else {
		h = luaL_checkstring(S, idx);
		if (*h == '#')
			h++;
		n = strlen(h);
		if (n != 6 && n != 8)
			return luaL_error(S, "color '%s' is not #rrggbb or #rrggbbaa", h);
		v = strtoul(h, &end, 16);
		if (*end)
			return luaL_error(S, "color '%s' is not hex", h);
		if (n == 6)
			v = (v << 8) | 0xff;
	}
	out[0] = ((v >> 24) & 0xff) / 255.0f;
	out[1] = ((v >> 16) & 0xff) / 255.0f;
	out[2] = ((v >> 8) & 0xff) / 255.0f;
	out[3] = (v & 0xff) / 255.0f;
	return 1;
}

/* Read one field out of the table on top of the stack, if it is there. */
static int
field(lua_State *S, const char *k)
{
	if (lua_getfield(S, -1, k) == LUA_TNIL) {
		lua_pop(S, 1);
		return 0;
	}
	return 1;
}

static void
getbool(lua_State *S, const char *k, int *out)
{
	if (field(S, k)) {
		*out = lua_toboolean(S, -1);
		lua_pop(S, 1);
	}
}

static void
getint(lua_State *S, const char *k, int *out)
{
	if (field(S, k)) {
		*out = (int)luaL_checkinteger(S, -1);
		lua_pop(S, 1);
	}
}

static void
getcolor(lua_State *S, const char *k, float out[4])
{
	if (field(S, k)) {
		tocolor(S, -1, out);
		lua_pop(S, 1);
	}
}

/* A libinput enum reads as a word, because nobody should type the constant. */
static void
getenum(lua_State *S, const char *k, const char *names[], const int vals[], void *out)
{
	const char *v;
	int i;

	if (!field(S, k))
		return;
	v = luaL_checkstring(S, -1);
	for (i = 0; names[i]; i++) {
		if (!strcmp(names[i], v)) {
			*(int *)out = vals[i];
			lua_pop(S, 1);
			return;
		}
	}
	luaL_error(S, "'%s' is not a valid %s", v, k);
}

/*
 * Which windows get inactive_opacity.
 *
 * Two lists, both POSIX extended regexes matched against app-id and then
 * title, so the anchored form you already write in a Hyprland config keeps
 * working: "^mpv$", "^Brave".
 *
 *   opaque       never faded, whatever else is set
 *   translucent  if this list is non-empty, nothing outside it is faded
 *
 * opaque wins, so a window in both stays solid. The popup that reads a
 * window's app-id and hands you the string to paste is shell work, later.
 */
typedef struct {
	regex_t *re;
	size_t n;
} Patterns;

static Patterns opaque, translucent;

static void
patclear(Patterns *p)
{
	while (p->n)
		regfree(&p->re[--p->n]);
	free(p->re);
	p->re = NULL;
}

/* A bad pattern is skipped with a message. One typo should not blank a list. */
static void
patload(lua_State *S, const char *key, Patterns *p)
{
	const char *src;
	regex_t *grown;
	int n, i;

	patclear(p);
	if (!field(S, key))
		return;
	luaL_checktype(S, -1, LUA_TTABLE);
	n = (int)lua_rawlen(S, -1);
	if (n && !(p->re = calloc(n, sizeof(*p->re))))
		die("patload:");
	for (i = 0; i < n; i++) {
		lua_rawgeti(S, -1, i + 1);
		src = luaL_checkstring(S, -1);
		if (regcomp(&p->re[p->n], src, REG_EXTENDED | REG_NOSUB) == 0)
			p->n++;
		else
			fprintf(stderr, "hedl: %s: bad pattern '%s', skipped\n", key, src);
		lua_pop(S, 1);
	}
	lua_pop(S, 1);
	grown = realloc(p->re, (p->n ? p->n : 1) * sizeof(*p->re));
	if (grown)
		p->re = grown;
}

static int
patmatch(const Patterns *p, Client *c)
{
	const char *appid = client_get_appid(c), *title = client_get_title(c);
	size_t i;

	for (i = 0; i < p->n; i++) {
		if ((appid && regexec(&p->re[i], appid, 0, NULL, 0) == 0)
				|| (title && regexec(&p->re[i], title, 0, NULL, 0) == 0))
			return 1;
	}
	return 0;
}

/* 1.0 means leave it alone, which is also what happens with no config. */
static float
clientopacity(Client *c, int focused)
{
	if (focused)
		return active_opacity;
	if (patmatch(&opaque, c))
		return 1.0f;
	if (translucent.n && !patmatch(&translucent, c))
		return 1.0f;
	return inactive_opacity;
}

static void
setbufferopacity(struct wlr_scene_buffer *buf, int sx, int sy, void *data)
{
	wlr_scene_buffer_set_opacity(buf, *(float *)data);
}

static void
applyopacity(Client *c, int focused)
{
	float o = clientopacity(c, focused);
	if (c->scene_surface)
		wlr_scene_node_for_each_buffer(&c->scene_surface->node,
				setbufferopacity, &o);
}

static void
applypointers(void)
{
	size_t i;
	for (i = 0; i < npointers; i++)
		createpointer(pointers[i]);
}

/* Colors and border width are on every client, so a reload walks them. */
static void
applyclients(void)
{
	Client *c, *sel = focustop(selmon);
	int i;

	wl_list_for_each(c, &clients, link) {
		for (i = 0; i < 4; i++)
			wlr_scene_rect_set_size(c->border[i], c->border[i]->width,
					c->border[i]->height);
		client_set_border_color(c, c == sel ? focuscolor
				: c->isurgent ? urgentcolor : bordercolor);
		applyopacity(c, c == sel);
	}
	if (root_bg)
		wlr_scene_rect_set_color(root_bg, rootcolor);
}

/* The keymap is one object on one group, so swapping it is swapping it. */
static void
applykeymap(void)
{
	struct xkb_context *ctx;
	struct xkb_keymap *km;

	if (!kb_group)
		return;
	if (!(ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS)))
		return;
	if ((km = xkb_keymap_new_from_names(ctx, &xkb_rules, XKB_KEYMAP_COMPILE_NO_FLAGS))) {
		wlr_keyboard_set_keymap(&kb_group->wlr_group->keyboard, km);
		xkb_keymap_unref(km);
	} else {
		fprintf(stderr, "hedl: bad xkb rules, keeping the old keymap\n");
	}
	xkb_context_unref(ctx);
	wlr_keyboard_set_repeat_info(&kb_group->wlr_group->keyboard,
			repeat_rate, repeat_delay);
}

static int
l_config(lua_State *S)
{
	static const char *scrollnames[] = {"none", "two_finger", "edge", "on_button_down", NULL};
	static const int scrollvals[] = {LIBINPUT_CONFIG_SCROLL_NO_SCROLL,
			LIBINPUT_CONFIG_SCROLL_2FG, LIBINPUT_CONFIG_SCROLL_EDGE,
			LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN};
	static const char *clicknames[] = {"none", "button_areas", "clickfinger", NULL};
	static const int clickvals[] = {LIBINPUT_CONFIG_CLICK_METHOD_NONE,
			LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS,
			LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER};
	static const char *accelnames[] = {"flat", "adaptive", NULL};
	static const int accelvals[] = {LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT,
			LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE};
	static const char *mapnames[] = {"lrm", "lmr", NULL};
	static const int mapvals[] = {LIBINPUT_CONFIG_TAP_MAP_LRM, LIBINPUT_CONFIG_TAP_MAP_LMR};
	int n;

	luaL_checktype(S, 1, LUA_TTABLE);

	if (field(S, "general")) {
		n = (int)borderpx;
		getint(S, "border_size", &n);
		borderpx = n < 0 ? 0 : (unsigned int)n;
		lua_pop(S, 1);
	}
	if (field(S, "animation")) {
		getbool(S, "enabled", &anim_enabled);
		getint(S, "divisor", &anim_divisor);
		getint(S, "snap", &anim_snap);
		if (anim_divisor < 1)
			anim_divisor = 1;
		if (anim_snap < 1)
			anim_snap = 1;
		lua_pop(S, 1);
	}
	if (field(S, "decoration")) {
		if (field(S, "active_opacity")) {
			active_opacity = (float)luaL_checknumber(S, -1);
			lua_pop(S, 1);
		}
		if (field(S, "inactive_opacity")) {
			inactive_opacity = (float)luaL_checknumber(S, -1);
			lua_pop(S, 1);
		}
		patload(S, "opaque", &opaque);
		patload(S, "translucent", &translucent);
		lua_pop(S, 1);
	}
	if (field(S, "colors")) {
		getcolor(S, "focus", focuscolor);
		getcolor(S, "border", bordercolor);
		getcolor(S, "urgent", urgentcolor);
		getcolor(S, "root", rootcolor);
		lua_pop(S, 1);
	}
	if (field(S, "input")) {
		getint(S, "repeat_rate", &repeat_rate);
		getint(S, "repeat_delay", &repeat_delay);
		getbool(S, "follow_mouse", &sloppyfocus);
		if (field(S, "kb_options")) {
			xkb_rules.options = keep(strdup(luaL_checkstring(S, -1)));
			lua_pop(S, 1);
		}
		if (field(S, "touchpad")) {
			getbool(S, "tap", &tap_to_click);
			getbool(S, "tap_and_drag", &tap_and_drag);
			getbool(S, "drag_lock", &drag_lock);
			getbool(S, "natural_scroll", &natural_scrolling);
			getbool(S, "dwt", &disable_while_typing);
			getbool(S, "left_handed", &left_handed);
			getbool(S, "middle_emulation", &middle_button_emulation);
			getenum(S, "scroll_method", scrollnames, scrollvals, &scroll_method);
			getenum(S, "click_method", clicknames, clickvals, &click_method);
			getenum(S, "accel_profile", accelnames, accelvals, &accel_profile);
			getenum(S, "tap_button_map", mapnames, mapvals, &button_map);
			if (field(S, "accel_speed")) {
				accel_speed = luaL_checknumber(S, -1);
				lua_pop(S, 1);
			}
			lua_pop(S, 1);
		}
		lua_pop(S, 1);
	}
	return 0;
}

/* ------------------------------------------------------------------ binds */

static int
l_bind(lua_State *S)
{
	const char *spec = luaL_checkstring(S, 1);
	Dispatch *d;
	uint32_t mod;
	xkb_keysym_t sym;
	int at, ref;

	/* The description sits at 2 the way Omarchy puts it, and may be nil, so
	 * the payload is at 2 or 3. It is either a dispatcher or a plain Lua
	 * function; a function is what lets a bind do more than one thing. */
	at = (lua_isfunction(S, 2) || luaL_testudata(S, 2, DISPATCH_MT)) ? 2 : 3;

	if (!parsekey(spec, &mod, &sym))
		return luaL_error(S, "cannot parse key '%s'", spec);

	if (lua_isfunction(S, at)) {
		lua_pushvalue(S, at);
		ref = luaL_ref(S, LUA_REGISTRYINDEX);
		bindremove(mod, sym);
		bindadd(mod, sym, NULL, (Arg){.i = ref});
		return 0;
	}
	d = luaL_checkudata(S, at, DISPATCH_MT);
	bindremove(mod, sym);
	bindadd(mod, sym, d->a->name, d->arg);
	return 0;
}

static int
l_unbind(lua_State *S)
{
	const char *spec = luaL_checkstring(S, 1);
	uint32_t mod;
	xkb_keysym_t sym;

	if (!parsekey(spec, &mod, &sym))
		return luaL_error(S, "cannot parse key '%s'", spec);
	bindremove(mod, sym);
	return 0;
}

/* ------------------------------------------------------------------ setup */

static void
scriptopen(void)
{
	const Action *a;
	const char *home = getenv("HOME");
	char path[512];

	L = luaL_newstate();
	luaL_openlibs(L);
	luaL_newmetatable(L, DISPATCH_MT);
	lua_pushcfunction(L, l_dispatch_call);
	lua_setfield(L, -2, "__call");
	lua_pop(L, 1);

	luaL_newmetatable(L, MONITOR_MT);
	lua_pushcfunction(L, l_monitor_index);    lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, l_monitor_newindex); lua_setfield(L, -2, "__newindex");
	lua_pushcfunction(L, l_monitor_tostring); lua_setfield(L, -2, "__tostring");
	lua_pop(L, 1);

	luaL_newmetatable(L, CLIENT_MT);
	lua_pushcfunction(L, l_client_index);    lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, l_client_newindex); lua_setfield(L, -2, "__newindex");
	lua_pushcfunction(L, l_client_tostring); lua_setfield(L, -2, "__tostring");
	lua_pop(L, 1);

	/* Ours first, so a user file can require() the defaults it overrides. */
	snprintf(path, sizeof(path), "%s/hedl/?.lua;" SYSCONF "/?.lua",
			home ? home : "/etc");
	lua_getglobal(L, "package");
	lua_pushstring(L, path);
	lua_setfield(L, -2, "path");
	lua_pop(L, 1);

	lua_newtable(L);                            /* hedl */
	lua_pushcfunction(L, l_bind);
	lua_setfield(L, -2, "bind");
	lua_pushcfunction(L, l_unbind);
	lua_setfield(L, -2, "unbind");
	lua_pushcfunction(L, l_config);
	lua_setfield(L, -2, "config");
	lua_pushcfunction(L, l_focused);
	lua_setfield(L, -2, "focused");
	lua_pushcfunction(L, l_clients);
	lua_setfield(L, -2, "clients");
	lua_pushcfunction(L, l_monitor);
	lua_setfield(L, -2, "monitor");
	lua_pushcfunction(L, l_monitors);
	lua_setfield(L, -2, "monitors");
	lua_pushcfunction(L, l_layouts);
	lua_setfield(L, -2, "layouts");
	lua_pushcfunction(L, l_on);
	lua_setfield(L, -2, "on");
	lua_pushcfunction(L, l_layout);
	lua_setfield(L, -2, "layout");
	lua_pushcfunction(L, l_window_rule);
	lua_setfield(L, -2, "window_rule");
	lua_pushcfunction(L, l_monitor_rule);
	lua_setfield(L, -2, "monitor_rule");
	lua_pushcfunction(L, l_env);
	lua_setfield(L, -2, "env");

	lua_newtable(L);                            /* hedl.dsp */
	for (a = actions; a < END(actions); a++) {
		lua_pushlightuserdata(L, (void *)a);
		lua_pushcclosure(L, l_dispatch, 1);
		lua_setfield(L, -2, a->name);
	}
	lua_setfield(L, -2, "dsp");
	lua_setglobal(L, "hedl");
}

/*
 * Load the config. Anything that goes wrong leaves the previous binds alone,
 * so a bad reload costs you the edit rather than the session.
 */
/*
 * Load into a fresh set and swap it in only if the whole file ran. A typo on
 * line 40 used to leave the first 39 lines applied and the rest missing, which
 * is worse than either outcome.
 */
static void
scriptload(void)
{
	const char *home = getenv("HOME"), *cfg = getenv("HEDL_CONFIG");
	char path[512];
	Key *oldkeys = luakeys;
	size_t oldn = nluakeys, oldnowned = nowned;
	void **oldowned = owned;
	int oldhooks[LENGTH(events)][16], oldnhooks[LENGTH(events)];
	size_t e;

	memcpy(oldhooks, hooks, sizeof(hooks));
	memcpy(oldnhooks, nhooks, sizeof(nhooks));

	if (!cfg) {
		snprintf(path, sizeof(path), "%s/.config/hedl/hedl.lua",
				home ? home : "/root");
		cfg = path;
	}
	if (access(cfg, R_OK) != 0) {
		fprintf(stderr, "hedl: no config at %s, using config.h\n", cfg);
		return;
	}

	luakeys = NULL;
	nluakeys = 0;
	owned = NULL;
	nowned = 0;
	memset(nhooks, 0, sizeof(nhooks));

	if (luaL_dofile(L, cfg) != LUA_OK) {
		fprintf(stderr, "hedl: %s\n", lua_tostring(L, -1));
		lua_pop(L, 1);
		bindclear();     /* whatever this attempt managed to build */
		hooksclear();
		wrulesclear();
		mrulesclear();
		luakeys = oldkeys;
		nluakeys = oldn;
		owned = oldowned;
		nowned = oldnowned;
		memcpy(hooks, oldhooks, sizeof(hooks));
		memcpy(nhooks, oldnhooks, sizeof(nhooks));
		fprintf(stderr, "hedl: config not loaded, keeping %s\n",
				oldn ? "the previous binds" : "config.h");
		return;
	}

	/* It ran, so the old set is what gets thrown away. */
	for (e = 0; e < LENGTH(events); e++) {
		int i;
		for (i = 0; i < oldnhooks[e]; i++)
			luadrop(oldhooks[e][i]);
	}
	while (oldn)
		if (!oldkeys[--oldn].action)
			luadrop(oldkeys[oldn].arg.i);
	while (oldnowned)
		free(oldowned[--oldnowned]);
	free(oldowned);
	free(oldkeys);
	fprintf(stderr, "hedl: %s, %zu binds\n", cfg, nluakeys);
	/* Everything the config touched is already live somewhere, so push it
	 * out rather than making the user log back in. */
	applyclients();
	applypointers();
	applykeymap();
}

void
reload(const Arg *arg)
{
	scriptload();
}

static void
scriptsetup(void)
{
	scriptopen();
	scriptload();
}

static void
scriptcleanup(void)
{
	hooksclear();
	wrulesclear();
	mrulesclear();
	patclear(&opaque);
	patclear(&translucent);
	if (L)
		lua_close(L);
	L = NULL;
	bindclear();
}
