# Makefile.am — giza-server
#
# After editing this file or configure.ac, ALWAYS re-run:
#   autoreconf -fi && ./configure
#
# autoreconf -fi is REQUIRED — do not skip it.

AUTOMAKE_OPTIONS = foreign

# ------------------------------------------------------------------ #
# Installed binary                                                    #
# ------------------------------------------------------------------ #

bin_PROGRAMS = giza_server

# --- GTK backend (Linux default) ---
if USE_GTK
giza_server_SOURCES = \
    viewer/giza-server-gtk.c \
    viewer/giza-server-protocol.h

giza_server_CFLAGS  = \
    -I$(srcdir)/viewer \
    $(GTK_CFLAGS)

giza_server_LDADD   = \
    $(GTK_LIBS) \
    $(PTHREAD_LIBS)
endif

# --- Xlib backend (Linux, X11 only — no GTK/GLib/GIO required) ---
# Depends only on libX11, cairo, cairo-xlib, libpng, and libpthread.
# Build with:  ./configure --with-viewer=xlib
# Verify afterwards:  ldd giza_server | grep -i gtk   (should be empty)

if USE_XLIB
giza_server_SOURCES = \
    viewer/giza-server-xlib.c \
    viewer/giza-server-protocol.h

giza_server_CFLAGS  = \
    -I$(srcdir)/viewer \
    $(XLIB_CFLAGS)

giza_server_LDADD   = \
    $(XLIB_LIBS) \
    -lpng \
    $(PTHREAD_LIBS)
endif

# --- Cocoa backend (macOS) ---
# Two .m files: the viewer logic and the main-thread bootstrapper.
# Compiled as Objective-C; linked against Cocoa + pthread.

if USE_COCOA
giza_server_SOURCES = \
    viewer/giza-server-cocoa.m \
    viewer/giza-server-main.m \
    viewer/giza-server-protocol.h
# -x objective-c is embedded in COCOA_CFLAGS from configure.ac
giza_server_CFLAGS  = \
    -I$(srcdir)/viewer \
    $(COCOA_CFLAGS)
giza_server_LDADD   = \
    $(COCOA_LIBS)
endif

# ------------------------------------------------------------------ #
# Test programs (built by 'make check', not installed)               #
# ------------------------------------------------------------------ #

check_PROGRAMS = test/test_ping test/test_png

test_test_ping_SOURCES = test/test_ping.c
test_test_ping_CFLAGS  = -I$(srcdir)/viewer
test_test_ping_LDADD   =

test_test_png_SOURCES  = test/test_png.c
test_test_png_CFLAGS   = -I$(srcdir)/viewer $(CAIRO_CFLAGS)
test_test_png_LDADD    = $(CAIRO_LIBS)

# ------------------------------------------------------------------ #
# Test suite (automake TESTS)                                         #
# ------------------------------------------------------------------ #

TESTS = test/test_ping.sh test/test_png.sh

TEST_EXTENSIONS = .sh
SH_LOG_COMPILER = sh

# ------------------------------------------------------------------ #
# Extra dist (files included in 'make dist' tarball)                 #
# ------------------------------------------------------------------ #

EXTRA_DIST = \
    viewer/giza-server-xlib.c \
    src/giza-driver-gs.c \
    src/giza-driver-gs-private.h \
    patches/giza-v1.5.0-drivers.patch \
    giza-gs-driver.patch.annotation \
    test/test_ping.sh \
    test/test_png.sh \
    README.md \
    CHANGES.md

# ------------------------------------------------------------------ #
# check-driver: syntax-check the libgiza driver (optional)           #
# ------------------------------------------------------------------ #

.PHONY: check-driver

if HAVE_GIZA_SRC
check-driver:
	$(CC) -fsyntax-only \
	    $(CFLAGS) \
	    $(CAIRO_CFLAGS) \
	    -I$(srcdir)/viewer \
	    -I$(GIZA_SRC) \
	    -D_GIZA_HAS_GS \
	    $(srcdir)/src/giza-driver-gs.c
	@echo "Driver syntax OK"
else
check-driver:
	@echo "check-driver requires --with-giza-src=DIR at configure time."
	@echo "Example: ./configure --with-giza-src=/path/to/giza/src"
	@exit 1
endif
