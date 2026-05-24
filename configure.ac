dnl configure.ac — giza-server
dnl
dnl Run:  autoreconf -fi && ./configure && make
dnl
dnl autoreconf -fi is REQUIRED after any edit to this file or Makefile.am.
dnl (Lesson learned from giza PR #86: skipping autoreconf causes mysterious
dnl  build failures that are very hard to diagnose.)

AC_PREREQ([2.69])
AC_INIT([giza-server], [0.2], [https://github.com/goosh-gh/giza-server/issues])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign -Wall -Werror subdir-objects])

dnl --- Detect OS (must come before AC_PROG_CC for $host_os) ---
AC_CANONICAL_HOST

dnl --- Compiler (C and Objective-C) ---
AC_PROG_CC
AC_PROG_OBJC

dnl --- Detect macOS ---
case "$host_os" in
    darwin*)
        build_cocoa=yes
        ;;
    *)
        build_cocoa=no
        ;;
esac

dnl --- Viewer backend selection ---
AC_ARG_WITH([viewer],
    [AS_HELP_STRING([--with-viewer=gtk|cocoa|auto],
        [windowing backend: gtk (Linux), cocoa (macOS), auto (default)])],
    [viewer_backend="$withval"],
    [viewer_backend="auto"])

dnl Resolve 'auto'
if test "$viewer_backend" = "auto"; then
    if test "$build_cocoa" = "yes"; then
        viewer_backend="cocoa"
    else
        viewer_backend="gtk"
    fi
fi

dnl Validate choice
case "$viewer_backend" in
    gtk|cocoa) ;;
    *)
        AC_MSG_ERROR([--with-viewer must be gtk, cocoa, or auto (got: $viewer_backend)])
        ;;
esac

AM_CONDITIONAL([USE_COCOA], [test "$viewer_backend" = "cocoa"])
AM_CONDITIONAL([USE_GTK],   [test "$viewer_backend" = "gtk"])

dnl --- pthread ---
AC_CHECK_LIB([pthread], [pthread_create],
    [PTHREAD_LIBS="-lpthread"],
    [AC_MSG_ERROR([libpthread not found.])])
AC_SUBST([PTHREAD_LIBS])

dnl --- GTK backend libraries ---
if test "$viewer_backend" = "gtk"; then
    AC_CONFIG_SRCDIR([viewer/giza-server-gtk.c])
    PKG_CHECK_MODULES([GTK], [gtk+-3.0 gio-unix-2.0 cairo],
        [],
        [AC_MSG_ERROR([gtk+-3.0, gio-unix-2.0, and cairo are required.
  Ubuntu/Debian:  sudo apt install libgtk-3-dev libcairo2-dev])])
fi

dnl --- Cocoa backend: no pkg-config needed (system frameworks) ---
if test "$viewer_backend" = "cocoa"; then
    AC_CONFIG_SRCDIR([viewer/giza-server-cocoa.m])
    dnl Check that we have the ObjC compiler and Cocoa framework
    AC_LANG_PUSH([Objective C])
    AC_MSG_CHECKING([for Cocoa framework])
    save_OBJCFLAGS="$OBJCFLAGS"
    save_LIBS="$LIBS"
    OBJCFLAGS="$OBJCFLAGS -framework Cocoa"
    LIBS="$LIBS -framework Cocoa"
    AC_LINK_IFELSE(
        [AC_LANG_PROGRAM([[#import <Cocoa/Cocoa.h>]],
                         [[NSApplication *a = [NSApplication sharedApplication]; (void)a;]])],
        [AC_MSG_RESULT([yes])
         COCOA_CFLAGS="-x objective-c"
         COCOA_LIBS="-framework Cocoa -lpthread"],
        [AC_MSG_RESULT([no])
         AC_MSG_ERROR([Cocoa framework not found. macOS SDK required.])])
    OBJCFLAGS="$save_OBJCFLAGS"
    LIBS="$save_LIBS"
    AC_LANG_POP([Objective C])
    AC_SUBST([COCOA_CFLAGS])
    AC_SUBST([COCOA_LIBS])
fi

dnl --- Cairo for test_png (both backends need it) ---
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

  Viewer backend : $viewer_backend
  Host OS        : $host_os
  pthread        : $PTHREAD_LIBS
  giza src       : ${GIZA_SRC:-not set  ('make check-driver' disabled)}

  Next steps:
    make
    sudo make install     (installs to $prefix/bin)

  NOTE: if you edit configure.ac or Makefile.am, always re-run:
    autoreconf -fi && ./configure
])
