/*
 * The window manager.
 *
 * These 23 functions were lifted out of dwl.c so that upstream's file holds
 * only Wayland glue. Nothing here is new code: it is dwl's, in dwl's order,
 * and a merge conflict on one of them is resolved by pasting upstream's hunk
 * in. dwl.c keeps the declarations, the globals and every call site.
 *
 * client.h is the precedent for a .h that holds bodies. See D10.
 */

/* HEDL: defined in script.h, which is included after this file. */
static void applyopacity(Client *c, int focused);
static void emit(int e, Client *c);
static int animstep(Monitor *m);
static void wrulesapply(Client *c);
static int animate(Client *c);
enum { EV_START, EV_MAP, EV_UNMAP, EV_FOCUS, EV_TITLE, EV_URGENT };

void
applyrules(Client *c)
{
	/* rule matching */
	const char *appid, *title;
	uint32_t newtags = 0;
	int i;
	const Rule *r;
	Monitor *mon = selmon, *m;

	appid = client_get_appid(c);
	title = client_get_title(c);

	for (r = rules; r < END(rules); r++) {
		if ((!r->title || strstr(title, r->title))
				&& (!r->id || strstr(appid, r->id))) {
			c->isfloating = r->isfloating;
			newtags |= r->tags;
			i = 0;
			wl_list_for_each(m, &mons, link) {
				if (r->monitor == i++)
					mon = m;
			}
		}
	}

	c->isfloating |= client_is_float_type(c);
	/* HEDL: remember the float the rules and the client asked for, so
	 * setlayout can tell it apart from one the user made by hand. */
	c->rulefloat = c->isfloating;
	setmon(c, mon, newtags);
	wrulesapply(c); /* HEDL: last, because setmon overwrites bw and tags */
}

void
arrange(Monitor *m)
{
	Client *c;

	if (!m->wlr_output->enabled)
		return;

	wl_list_for_each(c, &clients, link) {
		if (c->mon == m) {
			wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, m));
			client_set_suspended(c, !VISIBLEON(c, m));
		}
	}

	wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
			(c = focustop(m)) && c->isfullscreen);

	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));

	/* We move all clients (except fullscreen and unmanaged) to LyrTile while
	 * in floating layout to avoid "real" floating clients be always on top */
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || c->scene->node.parent == layers[LyrFS])
			continue;

		wlr_scene_node_reparent(&c->scene->node,
				(!m->lt[m->sellt]->arrange && c->isfloating)
						? layers[LyrTile]
						: (m->lt[m->sellt]->arrange && c->isfloating)
								? layers[LyrFloat]
								: c->scene->node.parent);
	}

	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
	motionnotify(0, NULL, 0, 0, 0, 0);
	checkidleinhibitor(NULL);
}

void
chvt(const Arg *arg)
{
	wlr_session_change_vt(session, arg->ui);
}

/*
 * HEDL: the pointers we have configured. createpointer applies the user's
 * input settings, so a reload has to be able to find the devices again.
 */
static struct wlr_pointer **pointers;
static size_t npointers;

static int
pointerknown(struct wlr_pointer *p)
{
	size_t i;
	for (i = 0; i < npointers; i++)
		if (pointers[i] == p)
			return 1;
	return 0;
}

static void
pointerkeep(struct wlr_pointer *p)
{
	struct wlr_pointer **n = realloc(pointers, (npointers + 1) * sizeof(*n));
	if (!n)
		die("pointerkeep:");
	pointers = n;
	pointers[npointers++] = p;
}


void
createpointer(struct wlr_pointer *pointer)
{
	struct libinput_device *device;

	/* HEDL: remember it, so hedl.config() on reload reaches the pointers
	 * that are already plugged in and not only the next one. */
	if (!pointerknown(pointer))
		pointerkeep(pointer);

	if (wlr_input_device_is_libinput(&pointer->base)
			&& (device = wlr_libinput_get_device_handle(&pointer->base))) {

		/* HEDL: every one of these is nested under `touchpad` in the Lua
		 * config, so a device that is not one gets none of them. Applied to
		 * a mouse, natural_scroll inverts its wheel and left_handed swaps
		 * its buttons. A finger count is libinput's touchpad test, and dwl
		 * already used it for the tap settings alone. */
		if (libinput_device_config_tap_get_finger_count(device)) {
			libinput_device_config_tap_set_enabled(device, tap_to_click);
			libinput_device_config_tap_set_drag_enabled(device, tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(device, drag_lock);
			libinput_device_config_tap_set_button_map(device, button_map);

			if (libinput_device_config_scroll_has_natural_scroll(device))
				libinput_device_config_scroll_set_natural_scroll_enabled(device,
						natural_scrolling);

			if (libinput_device_config_dwt_is_available(device))
				libinput_device_config_dwt_set_enabled(device, disable_while_typing);

			if (libinput_device_config_left_handed_is_available(device))
				libinput_device_config_left_handed_set(device, left_handed);

			if (libinput_device_config_middle_emulation_is_available(device))
				libinput_device_config_middle_emulation_set_enabled(device,
						middle_button_emulation);

			if (libinput_device_config_scroll_get_methods(device)
					!= LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
				libinput_device_config_scroll_set_method(device, scroll_method);

			if (libinput_device_config_click_get_methods(device)
					!= LIBINPUT_CONFIG_CLICK_METHOD_NONE)
				libinput_device_config_click_set_method(device, click_method);

			if (libinput_device_config_accel_is_available(device)) {
				libinput_device_config_accel_set_profile(device, accel_profile);
				libinput_device_config_accel_set_speed(device, accel_speed);
			}
		}

		/* Not a touchpad setting: whether a device sends events at all. */
		if (libinput_device_config_send_events_get_modes(device))
			libinput_device_config_send_events_set_mode(device, send_events_mode);
	}

	wlr_cursor_attach_input_device(cursor, &pointer->base);
}

void
focusclient(Client *c, int lift)
{
	struct wlr_surface *old = seat->keyboard_state.focused_surface;
	int unused_lx, unused_ly, old_client_type;
	Client *old_c = NULL;
	LayerSurface *old_l = NULL;

	if (locked)
		return;

	/* Raise client in stacking order if requested */
	if (c && lift)
		wlr_scene_node_raise_to_top(&c->scene->node);

	if (c && client_surface(c) == old)
		return;

	if ((old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l)) == XDGShell) {
		struct wlr_xdg_popup *popup, *tmp;
		wl_list_for_each_safe(popup, tmp, &old_c->surface.xdg->popups, link)
			wlr_xdg_popup_destroy(popup);
	}

	/* Put the new client atop the focus stack and select its monitor */
	if (c && !client_is_unmanaged(c)) {
		wl_list_remove(&c->flink);
		wl_list_insert(&fstack, &c->flink);
		selmon = c->mon;
		c->isurgent = 0;

		/* Don't change border color if there is an exclusive focus or we are
		 * handling a drag operation */
		if (!exclusive_focus && !seat->drag) {
			client_set_border_color(c, focuscolor);
			applyopacity(c, 1); /* HEDL */
		}
		emit(EV_FOCUS, c); /* HEDL */
	}

	/* Deactivate old client if focus is changing */
	if (old && (!c || client_surface(c) != old)) {
		/* If an overlay is focused, don't focus or activate the client,
		 * but only update its position in fstack to render its border with focuscolor
		 * and focus it after the overlay is closed. */
		if (old_client_type == LayerShell && wlr_scene_node_coords(
					&old_l->scene->node, &unused_lx, &unused_ly)
				&& old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
			return;
		} else if (old_c && old_c == exclusive_focus && client_wants_focus(old_c)) {
			return;
		/* Don't deactivate old client if the new one wants focus, as this causes issues with winecfg
		 * and probably other clients */
		} else if (old_c && !client_is_unmanaged(old_c) && (!c || !client_wants_focus(c))) {
			client_set_border_color(old_c, bordercolor);
			applyopacity(old_c, 0); /* HEDL */

			client_activate_surface(old, 0);
		}
	}
	printstatus();

	if (!c) {
		/* With no client, all we have left is to clear focus */
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}

	/* Change cursor surface */
	motionnotify(0, NULL, 0, 0, 0, 0);

	/* Have a client, so focus its top-level wlr_surface */
	client_notify_enter(client_surface(c), wlr_seat_get_keyboard(seat));

	/* Activate the new client */
	client_activate_surface(client_surface(c), 1);
}

void
focusmon(const Arg *arg)
{
	int i = 0, nmons = wl_list_length(&mons);
	if (nmons) {
		do /* don't switch to disabled mons */
			selmon = dirtomon(arg->i);
		while (!selmon->wlr_output->enabled && i++ < nmons);
	}
	focusclient(focustop(selmon), 1);
}

void
focusstack(const Arg *arg)
{
	/* Focus the next or previous client (in tiling order) on selmon */
	Client *c, *sel = focustop(selmon);
	if (!sel || (sel->isfullscreen && !client_has_children(sel)))
		return;
	if (arg->i > 0) {
		wl_list_for_each(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break; /* found it */
		}
	} else {
		wl_list_for_each_reverse(c, &sel->link, link) {
			if (&c->link == &clients)
				continue; /* wrap past the sentinel node */
			if (VISIBLEON(c, selmon))
				break; /* found it */
		}
	}
	/* If only one client is visible on selmon, then c == sel */
	focusclient(c, 1);
}

void
incnmaster(const Arg *arg)
{
	if (!arg || !selmon)
		return;
	selmon->nmaster = MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

void
killclient(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		client_send_close(sel);
}

/* HEDL: half the gap on every side of a window, laid out over an area already
 * inset by the same, so the space between two windows and the space at the
 * screen edge both come to gappx. */
static struct wlr_box
gapped(struct wlr_box b)
{
	int half = gappx / 2;

	b.x += half;
	b.y += half;
	b.width = MAX(1, b.width - gappx);
	b.height = MAX(1, b.height - gappx);
	return b;
}

void
monocle(Monitor *m)
{
	Client *c;
	int n = 0;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		/* Both halves tile applies: one for the area, one for the window. */
		resize(c, gapped(gapped(m->w)), 0);
		n++;
	}
	if (n)
		snprintf(m->ltsymbol, LENGTH(m->ltsymbol), "[%d]", n);
	if ((c = focustop(m)))
		wlr_scene_node_raise_to_top(&c->scene->node);
}

/* HEDL: the pointer says which way it is about to stretch things. */
static const char *
edgecursor(unsigned int edge)
{
	switch (edge) {
	case EdgeLeft:               return "w-resize";
	case EdgeRight:              return "e-resize";
	case EdgeTop:                return "n-resize";
	case EdgeBottom:             return "s-resize";
	case EdgeLeft | EdgeTop:     return "nw-resize";
	case EdgeRight | EdgeTop:    return "ne-resize";
	case EdgeLeft | EdgeBottom:  return "sw-resize";
	default:                     return "se-resize";
	}
}

/* HEDL: which edges the pointer is on, if any. The margin is capped at a
 * third of the window so that a small one keeps a middle you can move it by.
 */
static unsigned int
edgesat(Client *c, double x, double y)
{
	int mx = MIN(resize_margin, c->geom.width / 3);
	int my = MIN(resize_margin, c->geom.height / 3);
	unsigned int edge = 0;

	if (x - c->geom.x <= mx)
		edge |= EdgeLeft;
	else if (c->geom.x + c->geom.width - x <= mx)
		edge |= EdgeRight;
	if (y - c->geom.y <= my)
		edge |= EdgeTop;
	else if (c->geom.y + c->geom.height - y <= my)
		edge |= EdgeBottom;
	return edge;
}

/* HEDL: dwl has no way to move a window inside the layout, only zoom to the
 * top of it. This is the dwm patch of the same name: take the focused window
 * out of the list and put it back one place along, so the layout redraws with
 * it somewhere else. */
void
movestack(const Arg *arg)
{
	Client *c = NULL, *sel = focustop(selmon);

	if (!sel || sel->isfullscreen)
		return;

	/* Moving a window along the layout is a statement that it belongs in
	 * one. A floating window would otherwise change places in a list that
	 * decides nothing about where it is drawn, and appear not to move. */
	if (sel->isfloating)
		setfloating(sel, 0);

	if (arg->i > 0) {
		wl_list_for_each(c, &sel->link, link) {
			if (&c->link == &clients)
				continue;   /* the head is not a client */
			if (VISIBLEON(c, selmon))
				break;
		}
	} else {
		wl_list_for_each_reverse(c, &sel->link, link) {
			if (&c->link == &clients)
				continue;
			if (VISIBLEON(c, selmon))
				break;
		}
	}
	if (!c || c == sel)
		return;   /* nothing else is on this tag */

	wl_list_remove(&sel->link);
	if (arg->i > 0)
		wl_list_insert(&c->link, &sel->link);
	else
		wl_list_insert(c->link.prev, &sel->link);

	arrange(selmon);
	printstatus();
}

void
moveresize(const Arg *arg)
{
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
	if (!grabc || client_is_unmanaged(grabc) || grabc->isfullscreen)
		return;

	/* Float the window and tell motionnotify to grab it */
	setfloating(grabc, 1);
	grabedge = edgesat(grabc, cursor->x, cursor->y);

	switch (cursor_mode = arg->ui) {
	case CurMove:
		/* HEDL: a drag that starts on an edge stretches that edge, whichever
		 * button it was. Only the middle of a window moves it. */
		if (grabedge) {
			cursor_mode = CurResize;
			wlr_cursor_set_xcursor(cursor, cursor_mgr, edgecursor(grabedge));
			break;
		}
		grabcx = (int)round(cursor->x) - grabc->geom.x;
		grabcy = (int)round(cursor->y) - grabc->geom.y;
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "all-scroll");
		break;
	case CurResize:
		if (!grabedge) {
			grabedge = EdgeRight | EdgeBottom;
			/* Doesn't work for X11 output - the next absolute motion event
			 * returns the cursor to where it started */
			wlr_cursor_warp_closest(cursor, NULL,
					grabc->geom.x + grabc->geom.width,
					grabc->geom.y + grabc->geom.height);
		}
		wlr_cursor_set_xcursor(cursor, cursor_mgr, edgecursor(grabedge));
		break;
	}
}

void
quit(const Arg *arg)
{
	wl_display_terminate(dpy);
}

void
resize(Client *c, struct wlr_box geo, int interact)
{
	struct wlr_box *bbox;

	if (!c->mon || !client_surface(c)->mapped)
		return;

	bbox = interact ? &sgeom : &c->mon->w;

	client_set_bounds(c, geo.width, geo.height);
	c->geom = geo;
	applybounds(c, bbox);

	/* HEDL: geom is the target. anim is what is drawn, and rendermon walks
	 * it there. A drag is already following the pointer and a window that
	 * has never been placed has nowhere to come from, so both snap. */
	if (!animate(c) || interact || c->anim.width == 0)
		c->anim = c->geom;
	drawgeom(c);
}

/*
 * HEDL: the second half of dwl's resize, against c->anim rather than c->geom.
 * The surface is still told its target size, so a client renders once per
 * change and not once per frame; only the box it is shown in moves.
 */
static void
drawgeom(Client *c)
{
	struct wlr_box g = c->anim, clip;

	wlr_scene_node_set_position(&c->scene->node, g.x, g.y);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	wlr_scene_rect_set_size(c->border[0], g.width, c->bw);
	wlr_scene_rect_set_size(c->border[1], g.width, c->bw);
	wlr_scene_rect_set_size(c->border[2], c->bw, g.height - 2 * c->bw);
	wlr_scene_rect_set_size(c->border[3], c->bw, g.height - 2 * c->bw);
	wlr_scene_node_set_position(&c->border[1]->node, 0, g.height - c->bw);
	wlr_scene_node_set_position(&c->border[2]->node, 0, c->bw);
	wlr_scene_node_set_position(&c->border[3]->node, g.width - c->bw, c->bw);

	/* this is a no-op if size hasn't changed */
	c->resize = client_set_size(c, c->geom.width - 2 * c->bw,
			c->geom.height - 2 * c->bw);
	client_get_clip(c, &clip);
	clip.width = g.width - c->bw;
	clip.height = g.height - c->bw;
	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
}

void
setfullscreen(Client *c, int fullscreen)
{
	c->isfullscreen = fullscreen;
	if (!c->mon || !client_surface(c)->mapped)
		return;
	c->bw = fullscreen ? 0 : borderpx;
	client_set_fullscreen(c, fullscreen);
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen
			? LyrFS : c->isfloating ? LyrFloat : LyrTile]);

	if (fullscreen) {
		c->prev = c->geom;
		resize(c, c->mon->m, 0);
	} else {
		/* restore previous size instead of arrange for floating windows since
		 * client positions are set by the user and cannot be recalculated */
		resize(c, c->prev, 0);
	}
	arrange(c->mon);
	printstatus();
}

void
setlayout(const Arg *arg)
{
	Client *c;

	if (!selmon)
		return;
	if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
		selmon->sellt ^= 1;
	if (arg && arg->v)
		selmon->lt[selmon->sellt] = (Layout *)arg->v;
	strncpy(selmon->ltsymbol, selmon->lt[selmon->sellt]->symbol, LENGTH(selmon->ltsymbol));
	/* HEDL: a tiling layout is a reset. A window the user dragged or toggled
	 * goes back into the tiling; a dialog or a rule-floated window does not,
	 * because it never asked to be tiled in the first place. */
	if (selmon->lt[selmon->sellt]->arrange) {
		wl_list_for_each(c, &clients, link)
			if (c->mon == selmon && c->isfloating && !c->rulefloat)
				setfloating(c, 0);
	}
	arrange(selmon);
	printstatus();
}

/* arg > 1.0 will set mfact absolutely */
void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0f ? arg->f + selmon->mfact : arg->f - 1.0f;
	if (f < 0.1 || f > 0.9)
		return;
	selmon->mfact = f;
	arrange(selmon);
}

void
spawn(const Arg *arg)
{
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		die("dwl: execvp %s failed:", ((char **)arg->v)[0]);
	}
}

void
tag(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel || (arg->ui & TAGMASK) == 0)
		return;

	sel->tags = arg->ui & TAGMASK;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
tagmon(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		setmon(sel, dirtomon(arg->i), 0);
}

void
tile(Monitor *m)
{
	unsigned int mw, my, ty;
	int i, n = 0;
	Client *c;
	/* HEDL: the area a gap has already been taken out of. Every box below is
	 * measured in it, and gapped() takes the other half out of each window.
	 * The running totals use the box, not c->geom, because the geometry a
	 * client ends up with is the gapped one and the next window starts where
	 * the ungapped one ended. */
	struct wlr_box wa = gapped(m->w);

	wl_list_for_each(c, &clients, link)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;
	if (n == 0)
		return;

	if (n > m->nmaster)
		mw = m->nmaster ? (int)roundf(wa.width * m->mfact) : 0;
	else
		mw = wa.width;
	i = my = ty = 0;
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		if (i < m->nmaster) {
			struct wlr_box b = {.x = wa.x, .y = wa.y + my, .width = mw,
				.height = (wa.height - my) / (MIN(n, m->nmaster) - i)};
			resize(c, gapped(b), 0);
			my += b.height;
		} else {
			struct wlr_box b = {.x = wa.x + mw, .y = wa.y + ty,
				.width = wa.width - mw, .height = (wa.height - ty) / (n - i)};
			resize(c, gapped(b), 0);
			ty += b.height;
		}
		i++;
	}
}

void
togglefloating(const Arg *arg)
{
	Client *sel = focustop(selmon);
	/* return if fullscreen */
	if (sel && !sel->isfullscreen)
		setfloating(sel, !sel->isfloating);
}

void
togglefullscreen(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		setfullscreen(sel, !sel->isfullscreen);
}

void
toggletag(const Arg *arg)
{
	uint32_t newtags;
	Client *sel = focustop(selmon);
	if (!sel || !(newtags = sel->tags ^ (arg->ui & TAGMASK)))
		return;

	sel->tags = newtags;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
toggleview(const Arg *arg)
{
	uint32_t newtagset;
	if (!(newtagset = selmon ? selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK) : 0))
		return;

	selmon->tagset[selmon->seltags] = newtagset;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
view(const Arg *arg)
{
	if (!selmon || (arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
		return;
	selmon->seltags ^= 1; /* toggle sel tagset */
	if (arg->ui & TAGMASK)
		selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
	focusclient(focustop(selmon), 1);
	arrange(selmon);
	printstatus();
}

void
zoom(const Arg *arg)
{
	Client *c, *sel = focustop(selmon);

	if (!sel || !selmon || !selmon->lt[selmon->sellt]->arrange || sel->isfloating)
		return;

	/* Search for the first tiled window that is not sel, marking sel as
	 * NULL if we pass it along the way */
	wl_list_for_each(c, &clients, link) {
		if (VISIBLEON(c, selmon) && !c->isfloating) {
			if (c != sel)
				break;
			sel = NULL;
		}
	}

	/* Return if no other tiled window was found */
	if (&c->link == &clients)
		return;

	/* If we passed sel, move c to the front; otherwise, move sel to the
	 * front */
	if (!sel)
		sel = c;
	wl_list_remove(&sel->link);
	wl_list_insert(&clients, &sel->link);

	focusclient(sel, 1);
	arrange(selmon);
}
