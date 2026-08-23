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
	ARG_UI,      /* arg->ui, a tag bitmask, a vt, or a cursor mode */
	ARG_F,       /* arg->f,  a fraction */
	ARG_LAYOUT,  /* arg->v,  const Layout * */
	ARG_CMD,     /* arg->v,  char *const *, an argv */
} ArgKind;

typedef struct {
	const char *name;
	void (*func)(const Arg *);
	ArgKind kind;
} Action;

/* dwl's whole action surface. It is 18 and it stays 18. */
static const Action actions[] = {
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
	{ "tag",              tag,              ARG_UI     },
	{ "tagmon",           tagmon,           ARG_I      },
	{ "togglefloating",   togglefloating,   ARG_NONE   },
	{ "togglefullscreen", togglefullscreen, ARG_NONE   },
	{ "toggletag",        toggletag,        ARG_UI     },
	{ "toggleview",       toggleview,       ARG_UI     },
	{ "view",             view,             ARG_UI     },
	{ "zoom",             zoom,             ARG_NONE   },
};

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
	const Key *k;
	for (k = keys; k < END(keys); k++) {
		if (CLEANMASK(mods) != CLEANMASK(k->mod)
				|| xkb_keysym_to_lower(sym) != xkb_keysym_to_lower(k->keysym)
				|| !k->action)
			continue;
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
