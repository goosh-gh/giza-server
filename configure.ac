dnl configure.ac — giza-server
dnl
dnl Run:  autoreconf -fi && ./configure && make
dnl
dnl autoreconf -fi is REQUIRED after any edit to this file or Makefile.am.
dnl (Lesson learned from giza PR #86: skipping autoreconf causes mysterious
dnl  build failures that are very hard to diagnose.)

AC_PREREQ([2.69])
AC_INIT([giza-server], [0.1], [https://github.com/goosh-gh/giza-server/issues])
AC_CONFIG_SRCDIR([viewer/giza-server-gtk.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign -Wall -Werror subdir-objects])

dnl --- Compiler ---
AC_PROG_CC

dnl --- pthread ---
AC_CHECK_LIB([pthread], [pthread_create],
    [PTHREAD_LIBS="-lpthread"],
    [AC_MSG_ERROR([libpthread not found. Install: sudo apt install libc6-dev])])
AC_SUBST([PTHREAD_LIBS])

dnl --- Required libraries (viewer) ---
PKG_CHECK_MODULES([GTK], [gtk+-3.0 gio-unix-2.0 cairo],
    [],
    [AC_MSG_ERROR([gtk+-3.0, gio-unix-2.0, and cairo are required.
  Ubuntu/Debian:  sudo apt install libgtk-3-dev libcairo2-dev])])

dnl --- Cairo cflags for check-driver ---
PKG_CHECK_MODULES([CAIRO], [cairo], [], [])

dnl --- Optional: path to giza source tree (for 'make check-driver') ---
AC_ARG_WITH([giza-src],
    [AS_HELP_STRING([--with-giza-src=DIR],
        [path to giza source tree (needed only for 'make check-driver')])],
    [GIZA_SRC="$withval"],
    [GIZA_SRC=""])
AC_SUBST([GIZA_SRC])
AM_CONDITIONAL([HAVE_GIZA_SRC], [test -n "$GIZA_SRC"])

dnl --- Emit Makefile ---
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

AC_MSG_NOTICE([

  giza-server $VERSION configured successfully.

  GTK libs    : $GTK_LIBS
  pthread     : $PTHREAD_LIBS
  giza src    : ${GIZA_SRC:-not set  ('make check-driver' disabled)}

  Next steps:
    make
    sudo make install     (installs to $prefix/bin)

  NOTE: if you edit configure.ac or Makefile.am, always re-run:
    autoreconf -fi && ./configure
])
