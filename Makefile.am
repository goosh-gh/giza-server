# Makefile.am — giza-server
#
# After editing this file or configure.ac, ALWAYS re-run:
#   autoreconf -fi && ./configure
#
# autoreconf -fi is REQUIRED — do not skip it.
# (Lesson from giza PR #86: autoconf/automake/libtool generated files are
#  libtool-version-dependent; always regenerate, never commit them.)

AUTOMAKE_OPTIONS = foreign

# ------------------------------------------------------------------ #
# Installed binary                                                    #
# ------------------------------------------------------------------ #

bin_PROGRAMS = giza_server

giza_server_SOURCES = \
    viewer/giza-server-gtk.c \
    viewer/giza-server-protocol.h

giza_server_CFLAGS  = \
    -I$(srcdir)/viewer \
    $(GTK_CFLAGS)

giza_server_LDADD   = \
    $(GTK_LIBS) \
    $(PTHREAD_LIBS)

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
#                                                                     #
# test_ping.sh: automated ping/pong — starts giza_server itself,     #
#               no display needed.                                    #
# test_png.sh:  visual PNG window test — skips (exit 77) if no       #
#               DISPLAY or WAYLAND_DISPLAY is set.                    #
# ------------------------------------------------------------------ #

TESTS = test/test_ping.sh test/test_png.sh

# Tell automake these are shell scripts, not compiled programs
TEST_EXTENSIONS = .sh
SH_LOG_COMPILER = sh

# ------------------------------------------------------------------ #
# Extra dist (files included in 'make dist' tarball)                 #
# ------------------------------------------------------------------ #

EXTRA_DIST = \
    src/giza-driver-gs.c \
    src/giza-driver-gs-private.h \
    patches/giza-v1.5.0-drivers.patch \
    giza-gs-driver.patch.annotation \
    test/test_ping.sh \
    test/test_png.sh \
    README.md

# ------------------------------------------------------------------ #
# check-driver: syntax-check the libgiza driver (optional)           #
# Requires:  ./configure --with-giza-src=DIR                         #
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
