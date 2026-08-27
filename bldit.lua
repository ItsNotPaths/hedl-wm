-- pkgit build file. https://git.symlinx.net/pkgit
--
-- Only wlroots is declared. wayland, libinput, libxkbcommon, xcb, xcb-icccm,
-- lua 5.4 and wayland-protocols ship with every distribution. wlroots 0.20 is
-- the one that usually does not, and the version is pinned in config.mk for
-- the same reason.

bldit_version   = "1.2.0"
package_version = "0.8"

dependencies = {
  wlroots = {
    url     = "https://gitlab.freedesktop.org/wlroots/wlroots.git",
    version = "0.20.0",
    target  = "default",
  },
}

local function make(args)
  return os.execute("make PREFIX=" .. prefix .. " " .. args)
end

targets = {
  default = {
    build     = function() return make("") end,
    install   = function() return make("install") end,
    uninstall = function() return make("uninstall") end,
  },

  -- No XWayland. Steam and any game that captures the cursor need it, so this
  -- is for a machine that runs neither.
  ["no-xwayland"] = {
    build     = function() return make("XWAYLAND= XLIBS=") end,
    install   = function() return make("install") end,
    uninstall = function() return make("uninstall") end,
  },
}
