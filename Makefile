.POSIX:
.SUFFIXES:

include config.mk

# flags for compiling
NIXTILECPPFLAGS = -I. -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L \
	-DVERSION=\"$(VERSION)\" -DNIXTILE_DATADIR=\"$(DATADIR)\" $(XWAYLAND)
NIXTILEDEVCFLAGS = -g -Wpedantic -Wall -Wextra -Wdeclaration-after-statement \
	-Wno-unused-parameter -Wshadow -Wunused-macros -Werror=strict-prototypes \
	-Werror=implicit -Werror=return-type -Werror=incompatible-pointer-types \
	-Wfloat-conversion

# CFLAGS / LDFLAGS
PKGS      = wayland-server xkbcommon libinput libdrm $(XLIBS)
NIXTILECFLAGS = `$(PKG_CONFIG) --cflags $(PKGS)` $(WLR_INCS) $(NIXTILECPPFLAGS) $(NIXTILEDEVCFLAGS) $(CFLAGS)
LDLIBS    = `$(PKG_CONFIG) --libs $(PKGS)` $(WLR_LIBS) -lm $(LIBS)

all: nixtile
nixtile: nixtile.o util.o gpu_acceleration.o
	$(CC) nixtile.o util.o gpu_acceleration.o $(NIXTILECFLAGS) $(LDFLAGS) $(LDLIBS) -o $@
nixtile.o: nixtile.c client.h config.h config.mk cursor-shape-v1-protocol.h \
	pointer-constraints-unstable-v1-protocol.h wlr-layer-shell-unstable-v1-protocol.h \
	wlr-output-power-management-unstable-v1-protocol.h xdg-shell-protocol.h
util.o: util.c util.h
gpu_acceleration.o: gpu_acceleration.c gpu_acceleration.h

# wayland-scanner is a tool which generates C headers and rigging for Wayland
# protocols, which are specified in XML. wlroots requires you to rig these up
# to your build system yourself and provide them in the include path.
WAYLAND_SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`
WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`

cursor-shape-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml $@
pointer-constraints-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@
wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@
wlr-output-power-management-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		protocols/wlr-output-power-management-unstable-v1.xml $@
xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

config.h:
	cp config.def.h $@
clean:
	rm -f nixtile *.o *-protocol.h

dist: clean
	mkdir -p nixtile-$(VERSION)
	cp -R LICENSE* Makefile CHANGELOG.md README.md client.h config.def.h \
		config.mk protocols nixtile.1 nixtile.c util.c util.h nixtile.desktop \
		nixtile-$(VERSION)
	tar -caf nixtile-$(VERSION).tar.gz nixtile-$(VERSION)
	rm -rf nixtile-$(VERSION)

install: nixtile
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	rm -f $(DESTDIR)$(PREFIX)/bin/nixtile
	cp -f nixtile $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/nixtile
	mkdir -p $(DESTDIR)$(DATADIR)/nixtile/wallpapers
	cp -f wallpapers/* $(DESTDIR)$(DATADIR)/nixtile/wallpapers/
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp -f nixtile.1 $(DESTDIR)$(MANDIR)/man1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/nixtile.1
	mkdir -p $(DESTDIR)$(DATADIR)/wayland-sessions
	cp -f nixtile.desktop $(DESTDIR)$(DATADIR)/wayland-sessions/nixtile.desktop
	chmod 644 $(DESTDIR)$(DATADIR)/wayland-sessions/nixtile.desktop
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nixtile $(DESTDIR)$(MANDIR)/man1/nixtile.1 \
		$(DESTDIR)$(DATADIR)/wayland-sessions/nixtile.desktop

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CPPFLAGS) $(NIXTILECFLAGS) -o $@ -c $<
