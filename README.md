# hedl

A soft fork of [dwl], the wlroots compositor that fills dwm's place on Wayland.

hedl keeps dwl's model. Tags, not workspaces. Master and stack. Client rules.
No bar, no client-side decoration, no compositor-side application launcher. It
adds a Lua configuration file, a status format that a bar can read from a
socket, a command channel, and two visual effects.

hedl publishes what it knows and reads commands. It owns windows, tags, focus,
layout. nothing else.

The file layout keeps upstream merges cheap. `src/dwl.c` is still upstream's
file in upstream's order, `policy.h` holds upstream's code word for word, and
`client.h` follows upstream's own precedent of a header that holds function
bodies. The split is deletions only. Where a function diverges it carries a
comment that says what changed and why:

    grep -n 'HEDL:' src/*.h src/*.c

## Build

Dependencies:

- wlroots 0.19, built with the libinput backend
- wayland
- libinput
- xkbcommon
- lua 5.4
- libxcb and xcb-icccm, for XWayland
- wayland-protocols and pkg-config, at compile time only
- Xwayland, at run time only

Install these packages with their development packages. Then run `make`.

The version of wlroots is in the package name, so an API break arrives when you
change the pin in `config.mk` and not before. The same is true for Lua.

XWayland is on by default. Steam and any game that captures the cursor need it.
To build without it, comment out `XWAYLAND` and `XLIBS` in `config.mk`.

## Run

hedl runs on any backend that wlroots supports. You can run it from a VT
console, or inside an existing X11 or Wayland session as one window.

    hedl
    hedl -s 'some-startup-command'

The `-s` command runs under `/bin/sh -c` as a child process. hedl sends it
SIGTERM at shutdown and waits for it to exit. hedl writes its status to the
standard output of this command. A command that does not read the status must
close its standard input, or hedl blocks:

    hedl -s 'foot --server <&-'

A child process cannot change the environment of hedl. Set `XDG_RUNTIME_DIR`
and anything else that the session needs before you start hedl.

## Configuration

hedl reads `~/.config/hedl/hedl.lua` at start and again on the `reload`
dispatcher. `HEDL_CONFIG` names a different file. The search path for
`require()` is `~/.config/hedl/?.lua` and then `/usr/share/hedl/?.lua`.

If the file is absent, if it has a syntax error, or if it binds no key, the
keys in `config.h` stay in charge. A bad configuration cannot leave the session
without a keyboard.

```lua
hedl.config({
  general    = { border_size = 2 },
  colors     = { focus = "#7aa2f7", border = "#3b4261", urgent = "#f7768e" },
  decoration = { active_opacity = 1.0, inactive_opacity = 0.92 },
  animation  = { enabled = true, divisor = 6, snap = 2 },
  input      = { follow_mouse = true, touchpad = { tap = true } },
})

hedl.bind("SUPER + Return", "Terminal",   hedl.dsp.spawn("foot"))
hedl.bind("SUPER + J",      "Focus next", hedl.dsp.focusstack(1))
hedl.bind("SUPER + 1",      "Tag 1",      hedl.dsp.view(1))
hedl.unbind("SUPER + P")

hedl.window_rule({ class = "^steam$" }, { tile = true })
hedl.monitor_rule("eDP-1", { scale = 1.5, layout = "tile", x = 0, y = 0 })

hedl.on("map", function(c) print(c.title) end)
```

A dispatcher is a value, `hedl.dsp.focusstack(1)` returns an object
that holds a function and a bound argument. The same object goes to a key bind,
to a rule, or to the command channel. A key press and a command line share one
parser as a result.

19 flat dispatchers: `reload`, `chvt`, `focusmon`,
`focusstack`, `incnmaster`, `killclient`, `moveresize`, `quit`, `setlayout`,
`setmfact`, `spawn`, `tag`, `tagmon`, `togglefloating`, `togglefullscreen`,
`toggletag`, `toggleview`, `view`, `zoom`.

Window rules match on `class` and `title` with POSIX regular expressions. They
set `tags`, `floating` or `tile`, `borderpx` and `bordercolor`. Monitor rules
match the output name as a substring, as dwl does, and they set `mfact`,
`nmaster`, `scale`, `layout`, `x` and `y`. A monitor rule with no name is the
fallback that every monitor falls through to.

`hedl.on(event, fn)` takes `start`, `map`, `unmap`, `focus`, `title` and
`urgent`. Each event holds a list, so more than one function can listen. To read
the state, use `hedl.focused()`, `hedl.clients()`, `hedl.monitor()`,
`hedl.monitors()` and `hedl.layouts()`. `hedl.env(name, value)` sets an
environment variable for the processes that hedl spawns.

### Layouts in Lua

`arrange()` calls the layout once, and the loop over windows is inside it. A
layout is therefore one Lua call for each arrange, not one for each window.

```lua
hedl.layout("dwindle", function(ctx)
  for i, c in ipairs(ctx.clients) do
    c:place(x, y, w, h)
  end
end)
```

The context gives the usable area of the monitor, `nmaster`, `mfact`, and the
list of visible tiled clients. C keeps the box math. Lua decides which box goes
where.

A layout that never returns would freeze the event loop, because the loop is
single threaded. An instruction count hook stops a layout after 2000000 steps.
hedl then names the layout on standard error and falls back to `tile`.

## Status and commands

hedl writes its status in the [kipp] format: kind first, then the subject, then
`key=value` attributes, tab separated. One line for each fact.

    mon	eDP-1	w=2560	h=1440
    focus	eDP-1
    tag	eDP-1	1	state=focused,occupied
    layout	eDP-1	name=tile
    title	eDP-1	text=README.md
    app	eDP-1	id=foot
    win	eDP-1	fullscreen=0	floating=0

This replaces dwl's `printstatus` output. It breaks dwlb and every other
existing dwl bar. a lua file in kippsrv can be used to send the correct info 
to those if you really require them.

The status goes to two places. It goes to the standard output, which is what
`-s` feeds. It also goes to a listening socket at `$XDG_RUNTIME_DIR/hedl/kipp`.
A reader that connects gets the whole picture at once, then gets it again on
every change. Nothing is queued and nothing is answered.

The command channel is a FIFO at `$XDG_RUNTIME_DIR/hedl/cmd`. One command for
each line, in the same tab separated format, dispatched through the same
registry that the keyboard uses:

    printf 'view\t2\n' > "$XDG_RUNTIME_DIR/hedl/cmd"

Anything that can write a line can drive the window manager. There is no
library, no handshake and no generated protocol. `HEDLDIR` overrides the
directory for a nested instance.

## Effects

There are two:

- Per-window opacity, active and inactive, with pattern lists that make a
  window always opaque or always translucent.
- One animation curve, `x += (target - x) / divisor`, that snaps to the target
  inside a few pixels. It is stepped in `rendermon`, which already runs for
  each output at the refresh rate.

Small eye-easements that where cheap and small, likely nothing more will be added.

## Acknowledgements

dwl began by extending the TinyWL example that the sway and wlroots developers
provided under CC0. hedl began when I bought a claude subscription and 
decided that inscrutable token churn slopping was my passion.

Thanks to suckless.org, to the dwm developers and community, and to the dwl
contributors, in particular:

- **Devin J. Pohly, for creating and nurturing the project**
- Alexander Courtis, for the XWayland implementation
- Guido Cella, for the layer-shell protocol implementation and patch maintenance
- Stivvo, for output management and fullscreen support

[dwl]: https://codeberg.org/dwl/dwl
[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots/
[dwm]: https://dwm.suckless.org/
[kipp]: https://github.com/ItsNotPaths/kipp
[kippsrv]: https://github.com/ItsNotPaths/kippsrv
