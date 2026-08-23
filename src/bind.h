/*
 * The action registry.
 *
 * dwl's 18 actions already share one signature, so the only thing stopping a
 * bind from being data was that keys[] held C function pointers. Naming them
 * here is what lets a Lua dispatcher and a line on the command channel reach
 * the same 18 functions the keyboard does. One table, three callers. See D15.
 *
 * keybinding() lives here rather than in policy.h because this is the function
 * the Lua config replaces. policy.h is upstream's code kept verbatim so a
 * merge pastes in; this file is ours and conflicts here are expected.
 */

typedef enum {
	ARG_NONE,
	ARG_I,       /* arg->i,  a signed step */
	ARG_UI,      /* arg->ui, a vt number or a cursor mode */
	ARG_TAG,     /* arg->ui, a tag bitmask. Lua says 1, not 1 << 0 */
	ARG_F,       /* arg->f,  a fraction */
	ARG_LAYOUT,  /* arg->v,  const Layout * */
	ARG_CMD,     /* arg->v,  char *const *, an argv */
} ArgKind;

typedef struct {
	const char *name;
	void (*func)(const Arg *);
	ArgKind kind;
} Action;

/* Defined in script.h, which needs this file's registry to exist first. */
void reload(const Arg *arg);
static void luacall(int ref);
static void luadrop(int ref);

/*
 * A bind is a named action or a Lua function, never both, so the two share
 * one row: action == NULL means the Lua registry reference is in arg.i.
 * Keeping it out of the Key struct is what lets config.h's rows stay as
 * upstream wrote them, with no trailing field nobody can explain.
 */
#define ISLUA(k)  ((k)->action == NULL)
#define LUAREF(k) ((k)->arg.i)

/* dwl's 18, plus reload. `emit` joins them when the event socket does. */
static const Action actions[] = {
	{ "reload",           reload,           ARG_NONE   },
	{ "chvt",             chvt,             ARG_UI     },
	{ "focusmon",         focusmon,         ARG_I      },
	{ "focusstack",       focusstack,       ARG_I      },
	{ "incnmaster",       incnmaster,       ARG_I      },
	{ "killclient",       killclient,       ARG_NONE   },
	{ "moveresize",       moveresize,       ARG_UI     },
	{ "quit",             quit,             ARG_NONE   },
	{ "setlayout",        setlayout,        ARG_LAYOUT },
	{ "setmfact",         setmfact,         ARG_F      },
	{ "spawn",            spawn,            ARG_CMD    },
	{ "tag",              tag,              ARG_TAG     },
	{ "tagmon",           tagmon,           ARG_I      },
	{ "togglefloating",   togglefloating,   ARG_NONE   },
	{ "togglefullscreen", togglefullscreen, ARG_NONE   },
	{ "toggletag",        toggletag,        ARG_TAG     },
	{ "toggleview",       toggleview,       ARG_TAG     },
	{ "view",             view,             ARG_TAG     },
	{ "zoom",             zoom,             ARG_NONE   },
};

/*
 * Binds from the Lua config. While this is empty config.h's keys[] is in
 * charge, which is what makes a broken config a working session (D14).
 */
static Key *luakeys;
static size_t nluakeys;
static void **owned;   /* strings and argvs the binds point at */
static size_t nowned;

static void *
keep(void *p)
{
	void **o = realloc(owned, (nowned + 1) * sizeof(*owned));
	if (!o)
		die("keep:");
	owned = o;
	owned[nowned++] = p;
	return p;
}

static void
bindclear(void)
{
	while (nluakeys) {
		nluakeys--;
		if (ISLUA(&luakeys[nluakeys]))
			luadrop(LUAREF(&luakeys[nluakeys]));
	}
	while (nowned)
		free(owned[--nowned]);
	free(owned);
	free(luakeys);
	owned = NULL;
	luakeys = NULL;
	nluakeys = 0;
}

static void
bindadd(uint32_t mod, xkb_keysym_t sym, const char *action, Arg arg)
{
	Key *k = realloc(luakeys, (nluakeys + 1) * sizeof(*luakeys));
	if (!k)
		die("bindadd:");
	luakeys = k;
	k = &luakeys[nluakeys++];
	k->mod = mod;
	k->keysym = sym;
	k->action = action;
	k->arg = arg;
}

/* Last one wins, so a later bind on the same key replaces an earlier one. */
static void
bindremove(uint32_t mod, xkb_keysym_t sym)
{
	size_t i;
	for (i = nluakeys; i-- > 0;) {
		if (luakeys[i].mod != mod || luakeys[i].keysym != sym)
			continue;
		if (ISLUA(&luakeys[i]))
			luadrop(LUAREF(&luakeys[i]));
		memmove(&luakeys[i], &luakeys[i + 1],
				(nluakeys - i - 1) * sizeof(*luakeys));
		nluakeys--;
	}
}

static const Action *
actionbyname(const char *name)
{
	const Action *a;
	for (a = actions; a < END(actions); a++)
		if (!strcmp(a->name, name))
			return a;
	return NULL;
}

static int
keybinding(uint32_t mods, xkb_keysym_t sym)
{
	/*
	 * Compositor keybindings, as opposed to keys passed on to the client.
	 * An unknown action name says so once and then behaves as unbound, so a
	 * typo costs one key rather than the session.
	 */
	const Action *a;
	const Key *k, *first = nluakeys ? luakeys : keys;
	const Key *last = nluakeys ? luakeys + nluakeys : END(keys);
	for (k = first; k < last; k++) {
		if (CLEANMASK(mods) != CLEANMASK(k->mod)
				|| xkb_keysym_to_lower(sym) != xkb_keysym_to_lower(k->keysym))
			continue;
		if (ISLUA(k)) {
			/* config.h never leaves an action NULL, so this is only ever
			 * reached for a bind the Lua config made. */
			if (k < luakeys || k >= luakeys + nluakeys)
				continue;
			luacall(LUAREF(k));
			return 1;
		}
		if (!(a = actionbyname(k->action))) {
			fprintf(stderr, "hedl: no action named '%s'\n", k->action);
			return 0;
		}
		a->func(&k->arg);
		return 1;
	}
	return 0;
}

static int
buttonbinding(uint32_t mods, unsigned int button)
{
	const Action *a;
	const Button *b;
	for (b = buttons; b < END(buttons); b++) {
		if (CLEANMASK(mods) != CLEANMASK(b->mod)
				|| button != b->button || !b->action)
			continue;
		if (!(a = actionbyname(b->action))) {
			fprintf(stderr, "hedl: no action named '%s'\n", b->action);
			return 0;
		}
		a->func(&b->arg);
		return 1;
	}
	return 0;
}
