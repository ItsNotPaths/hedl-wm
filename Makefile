.POSIX:
.SUFFIXES:

include config.mk

# flags for compiling
DWLCPPFLAGS = -Isrc -Iwayland-gen -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L \
	-DVERSION=\"$(VERSION)\" $(XWAYLAND)
DWLDEVCFLAGS = -g -Wpedantic -Wall -Wextra -Wdeclaration-after-statement \
	-Wno-unused-parameter -Wshadow -Wunused-macros -Werror=strict-prototypes \
	-Werror=implicit -Werror=return-type -Werror=incompatible-pointer-types \
	-Wfloat-conversion

# CFLAGS / LDFLAGS
PKGS      = wayland-server xkbcommon libinput $(LUA) $(XLIBS)
DWLCFLAGS = `$(PKG_CONFIG) --cflags $(PKGS)` $(WLR_INCS) $(DWLCPPFLAGS) $(DWLDEVCFLAGS) $(CFLAGS)
LDLIBS    = `$(PKG_CONFIG) --libs $(PKGS)` $(WLR_LIBS) -lm $(LIBS)

# wayland-scanner output. Generated, gitignored, never edited.
GEN = wayland-gen/cursor-shape-v1-protocol.h \
	wayland-gen/pointer-constraints-unstable-v1-protocol.h \
	wayland-gen/wlr-layer-shell-unstable-v1-protocol.h \
	wayland-gen/wlr-output-power-management-unstable-v1-protocol.h \
	wayland-gen/xdg-shell-protocol.h

all: hedl
hedl: dwl.o util.o
	$(CC) dwl.o util.o $(DWLCFLAGS) $(LDFLAGS) $(LDLIBS) -o $@
dwl.o: src/dwl.c src/client.h src/config.h src/policy.h src/bind.h \
		src/script.h config.mk $(GEN)
	$(CC) $(CPPFLAGS) $(DWLCFLAGS) -o $@ -c src/dwl.c
util.o: src/util.c src/util.h
	$(CC) $(CPPFLAGS) $(DWLCFLAGS) -o $@ -c src/util.c

# wayland-scanner is a tool which generates C headers and rigging for Wayland
# protocols, which are specified in XML. wlroots requires you to rig these up
# to your build system yourself and provide them in the include path.
WAYLAND_SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`
WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`

wayland-gen/cursor-shape-v1-protocol.h:
	mkdir -p wayland-gen
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml $@
wayland-gen/pointer-constraints-unstable-v1-protocol.h:
	mkdir -p wayland-gen
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@
wayland-gen/wlr-layer-shell-unstable-v1-protocol.h:
	mkdir -p wayland-gen
	$(WAYLAND_SCANNER) enum-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@
wayland-gen/wlr-output-power-management-unstable-v1-protocol.h:
	mkdir -p wayland-gen
	$(WAYLAND_SCANNER) server-header \
		protocols/wlr-output-power-management-unstable-v1.xml $@
wayland-gen/xdg-shell-protocol.h:
	mkdir -p wayland-gen
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

src/config.h:
	cp src/config.def.h $@
clean:
	rm -f hedl *.o
	rm -rf wayland-gen

dist: clean
	mkdir -p hedl-$(VERSION)
	cp -R LICENSE licenses Makefile CHANGELOG.md README.md config.mk \
		src protocols hedl.1 hedl.desktop hedl-$(VERSION)
	tar -caf hedl-$(VERSION).tar.gz hedl-$(VERSION)
	rm -rf hedl-$(VERSION)

install: hedl
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	rm -f $(DESTDIR)$(PREFIX)/bin/hedl
	cp -f hedl $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/hedl
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp -f hedl.1 $(DESTDIR)$(MANDIR)/man1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/hedl.1
	mkdir -p $(DESTDIR)$(DATADIR)/wayland-sessions
	cp -f hedl.desktop $(DESTDIR)$(DATADIR)/wayland-sessions/hedl.desktop
	chmod 644 $(DESTDIR)$(DATADIR)/wayland-sessions/hedl.desktop
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/hedl $(DESTDIR)$(MANDIR)/man1/hedl.1 \
		$(DESTDIR)$(DATADIR)/wayland-sessions/hedl.desktop
