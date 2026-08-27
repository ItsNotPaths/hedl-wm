-- hedl's default configuration. Installed to $DATADIR/hedl/hedl.lua and read
-- when $XDG_CONFIG_HOME/hedl/hedl.lua is absent.
--
-- To change any of it, copy this file:
--
--     mkdir -p ~/.config/hedl
--     cp /usr/share/hedl/hedl.lua ~/.config/hedl/
--
-- A Lua config replaces the whole key table, so what is here is everything
-- the keyboard does. That includes the VT switches at the bottom.

local mod  = "SUPER"
local term = "foot"
local menu = "wmenu-run"

-- wlr_direction, which focusmon and tagmon take as a plain number.
local LEFT, RIGHT = 4, 8

hedl.config({
  general    = { border_size = 1, gaps = 0, resize_margin = 64 },
  colors     = {
    focus  = "#005577",
    border = "#444444",
    urgent = "#ff0000",
    root   = "#222222",
  },
  decoration = { active_opacity = 1.0, inactive_opacity = 1.0 },
  animation  = { enabled = false, divisor = 6, snap = 2 },
  input      = {
    follow_mouse = true,
    repeat_rate  = 25,
    repeat_delay = 600,
    touchpad = { tap = true, tap_and_drag = true, drag_lock = true, dwt = true },
  },
})

hedl.bind(mod .. " + SHIFT + Return", "Terminal",     hedl.dsp.spawn(term))
hedl.bind(mod .. " + P",              "Run",          hedl.dsp.spawn(menu))
hedl.bind(mod .. " + SHIFT + C",      "Close window", hedl.dsp.killclient())
hedl.bind(mod .. " + SHIFT + Q",      "Leave",        hedl.dsp.quit())
hedl.bind("CTRL + ALT + Terminate_Server", "Leave",   hedl.dsp.quit())

hedl.bind(mod .. " + J",         "Focus next",     hedl.dsp.focusstack(1))
hedl.bind(mod .. " + K",         "Focus prev",     hedl.dsp.focusstack(-1))
hedl.bind(mod .. " + Return",    "Zoom",           hedl.dsp.zoom())
hedl.bind(mod .. " + I",         "More masters",   hedl.dsp.incnmaster(1))
hedl.bind(mod .. " + D",         "Fewer masters",  hedl.dsp.incnmaster(-1))
hedl.bind(mod .. " + H",         "Shrink master",  hedl.dsp.setmfact(-0.05))
hedl.bind(mod .. " + L",         "Grow master",    hedl.dsp.setmfact(0.05))

hedl.bind(mod .. " + T",             "Tile",         hedl.dsp.setlayout("tile"))
hedl.bind(mod .. " + F",             "Float all",    hedl.dsp.setlayout("floating"))
hedl.bind(mod .. " + M",             "Monocle",      hedl.dsp.setlayout("monocle"))
hedl.bind(mod .. " + space",         "Last layout",  hedl.dsp.setlayout())
hedl.bind(mod .. " + SHIFT + space", "Float window", hedl.dsp.togglefloating())
hedl.bind(mod .. " + E",             "Fullscreen",   hedl.dsp.togglefullscreen())

hedl.bind(mod .. " + comma",           "Monitor left",  hedl.dsp.focusmon(LEFT))
hedl.bind(mod .. " + period",          "Monitor right", hedl.dsp.focusmon(RIGHT))
hedl.bind(mod .. " + SHIFT + comma",   "Send left",     hedl.dsp.tagmon(LEFT))
hedl.bind(mod .. " + SHIFT + period",  "Send right",    hedl.dsp.tagmon(RIGHT))

-- 0 is every tag, -1 is the set that was up before.
hedl.bind(mod .. " + Tab",       "Last tags", hedl.dsp.view(-1))
hedl.bind(mod .. " + 0",         "All tags",  hedl.dsp.view(0))
hedl.bind(mod .. " + SHIFT + 0", "Pin",       hedl.dsp.tag(0))

for i = 1, 9 do
  hedl.bind(mod .. " + " .. i,                 "Tag " .. i,       hedl.dsp.view(i))
  hedl.bind(mod .. " + CTRL + " .. i,          "Also show " .. i, hedl.dsp.toggleview(i))
  hedl.bind(mod .. " + SHIFT + " .. i,         "Move to " .. i,   hedl.dsp.tag(i))
  hedl.bind(mod .. " + CTRL + SHIFT + " .. i,  "Also on " .. i,   hedl.dsp.toggletag(i))
end

-- The X server used to own these. Without them there is no way off the seat.
for i = 1, 12 do
  hedl.bind("CTRL + ALT + XF86Switch_VT_" .. i, nil, hedl.dsp.chvt(i))
end
