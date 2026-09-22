/*
 * test_gs.c
 * ---------------------------------------------------------------------------
 * Minimal /gs client used to check the auto-launch path of the driver from
 * the outside: it opens a window through the PGPLOT C API, draws, closes the
 * device and exits.
 *
 * Two things are worth checking with it.
 *
 *   1. The window outlives the client. giza_server is detached, so the plot
 *      stays on screen after this program returns.
 *
 *   2. The client does not leave a pipeline open. The driver points the
 *      server's stdin/stdout at /dev/null and its stderr at
 *      $TMPDIR/giza_server_<uid>.log, so none of the three stays attached to
 *      the caller. Run the client through a pipe and it must return as soon
 *      as the client exits:
 *
 *          pkill giza_server            # force the launch path
 *          ./tools/test_gs 2>&1 | cat   # returns immediately; exit 0
 *          cat "${TMPDIR:-/tmp}/giza_server_$(id -u).log"
 *
 *      A hang here means the server inherited a descriptor it should not
 *      have: the reader never sees EOF while the server holds the write end.
 *
 * The failure path can be exercised by pointing GIZA_SERVER at something that
 * cannot be executed; cpgopen then returns -1 without hanging, and the log
 * carries one "could not exec giza_server" line:
 *
 *     pkill giza_server
 *     GIZA_SERVER=/nonexistent ./tools/test_gs 2>&1 | cat
 *
 * Build against the giza that carries the /gs driver (adjust the prefix):
 *
 *     clang tools/test_gs.c -o tools/test_gs \
 *         -I/usr/local/giza-2.0.0/include \
 *         -L/usr/local/giza-2.0.0/lib -lcpgplot -lgiza
 *
 * Note on macOS: SIP strips DYLD_* when a protected binary such as /bin/bash
 * is executed, so a DYLD_LIBRARY_PATH meant to select an uninstalled build is
 * lost as soon as a shell sits between the variable and this program. Link
 * against the prefix you actually want to test, or drive it through a
 * non-protected shell.
 */
#include <stdio.h>
#include "cpgplot.h"

int main(void)
{
  float x[5] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
  float y[5] = {1.0f, 4.0f, 9.0f, 16.0f, 25.0f};
  int id;

  id = cpgopen("/GS");
  printf("cpgopen(\"/GS\") -> %d\n", id);
  if (id <= 0) {
    fprintf(stderr, "cpgopen failed\n");
    return 1;
  }

  cpgenv(0.0f, 6.0f, 0.0f, 30.0f, 0, 1);
  cpglab("x", "y = x\\u2\\d", "giza-server /gs client");
  cpgsci(2);
  cpgline(5, x, y);
  cpgsci(3);
  cpgpt(5, x, y, 17);
  cpgsci(1);

  cpgclos();
  printf("cpgclos() returned; client is about to exit\n");
  return 0;
}
