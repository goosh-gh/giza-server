#!/usr/bin/env bash
#
# verify_server12.sh — verify giza-server server12 changes after `git apply`.
#
#   [A] Save-menu grayout for client-absent windows   (Cocoa)
#   [B] window close -> shutdown(client_fd) so an interactive client
#       (show_interactive) gets EOF and its run loop returns  (Cocoa + Xlib)
#
# What it does automatically:
#   1. source assertions   - confirm the patch is actually applied
#   2. build + make check  - compile the host backend, run the GSP smoke tests
#   3. (Linux only) a real [B] regression: drive a synthetic WM_DELETE at a
#      blocked client and assert it receives EOF instead of hanging
#
# What still needs a human (GUI clicks): the Cocoa [A] menu grayout and the
# on-screen feel of [B] — a printed checklist is shown at the end.
#
# Usage:   ./verify_server12.sh [REPO_DIR]     (defaults to the script's dir)
#
set -u

REPO="${1:-$(cd "$(dirname "$0")" && pwd)}"
cd "$REPO" || { echo "cannot cd into $REPO"; exit 2; }

COCOA=viewer/giza-server-cocoa.m
XLIB=viewer/giza-server-xlib.c

pass=0 fail=0
ok()   { echo "  PASS  $1"; pass=$((pass+1)); }
no()   { echo "  FAIL  $1"; fail=$((fail+1)); }
hdr()  { echo; echo "== $1 =="; }
have() { command -v "$1" >/dev/null 2>&1; }

[ -f configure.ac ] && [ -d viewer ] || { echo "not a giza-server checkout: $REPO"; exit 2; }

# ---------------------------------------------------------------------------
hdr "1. source assertions (is the patch applied?)"

# --- Cocoa [B] ---
grep -q 'shutdown(fd_to_shutdown, SHUT_RDWR)' "$COCOA" \
  && ok  "cocoa [B] windowWillClose: shutdown(SHUT_RDWR)" \
  || no  "cocoa [B] shutdown missing in $COCOA"

grep -q 'if (!still_used) fd_to_shutdown = fd;' "$COCOA" \
  && ok  "cocoa [B] sibling-fd guard (one connection, many windows)" \
  || no  "cocoa [B] sibling guard missing in $COCOA"

# --- Cocoa [A] ---
grep -q 'last_png != nil' "$COCOA" \
  && ok  "cocoa [A] PNG validated on last_png" \
  || no  "cocoa [A] PNG validation missing in $COCOA"

grep -q 'client_fd >= 0' "$COCOA" \
  && ok  "cocoa [A] PDF/SVG validated on live client_fd" \
  || no  "cocoa [A] vector validation missing in $COCOA"

# --- Xlib [B] ---
grep -q '_close_window(int id, int user_initiated)' "$XLIB" \
  && ok  "xlib [B] _close_window has user_initiated flag" \
  || no  "xlib [B] signature not updated in $XLIB"

grep -q 'shutdown(fd, SHUT_RDWR)' "$XLIB" \
  && ok  "xlib [B] shutdown(SHUT_RDWR) on user close" \
  || no  "xlib [B] shutdown missing in $XLIB"

grep -q '_close_window(GS.wins\[i\].id, 1)' "$XLIB" \
  && ok  "xlib [B] WM_DELETE passes user_initiated=1" \
  || no  "xlib [B] WM_DELETE caller not updated in $XLIB"

grep -q '_close_window(cmd->win_id, 0)' "$XLIB" \
  && ok  "xlib [B] CMD_CLOSE passes user_initiated=0 (client owns its fd)" \
  || no  "xlib [B] CMD_CLOSE caller not updated in $XLIB"

# ---------------------------------------------------------------------------
hdr "2. build + make check (host backend)"

case "$(uname -s)" in
  Darwin) BK=cocoa ;;
  Linux)  BK=xlib  ;;     # the file we changed on Linux
  *)      BK=auto  ;;
esac
echo "  host=$(uname -s)  ->  --with-viewer=$BK"

build_ok=0
if have autoreconf; then
  CONFARGS="--with-viewer=$BK"
  if [ "$(uname -s)" = "Darwin" ]; then
    # MacPorts/Apple toolchain: real GCC can't see the Cocoa framework, so
    # configure falls back and fails the framework check. Force clang and the
    # MacPorts pkg-config (override by exporting CC/OBJC/PKG_CONFIG yourself).
    CONFARGS="$CONFARGS CC=${CC:-clang} OBJC=${OBJC:-clang} PKG_CONFIG=${PKG_CONFIG:-/opt/local/bin/pkg-config}"
  fi
  echo "  configure: $CONFARGS"
  ( autoreconf -fi && ./configure $CONFARGS && make ) >/tmp/gs_build.log 2>&1 \
    && { ok "build (giza_server, $BK backend)"; build_ok=1; } \
    || { no "build failed — see /tmp/gs_build.log"; tail -15 /tmp/gs_build.log; }
else
  no "autoreconf not found (install autotools) — skipping build"
fi

if [ "$build_ok" = 1 ]; then
  make check >/tmp/gs_check.log 2>&1 \
    && ok "make check (GSP ping/png smoke tests)" \
    || { no "make check failed — see /tmp/gs_check.log"; tail -20 /tmp/gs_check.log; }
fi

# ---------------------------------------------------------------------------
hdr "3. automated [B] regression — synthetic WM_DELETE -> client EOF (Linux/Xlib)"

if [ "$(uname -s)" != "Linux" ]; then
  echo "  SKIP  not Linux (Cocoa close is verified by the manual checklist below)"
elif [ "$build_ok" != 1 ] || [ ! -x ./giza_server ]; then
  echo "  SKIP  no giza_server binary built"
elif ! have xdotool || { ! have Xvfb && [ -z "${DISPLAY:-}" ]; }; then
  echo "  SKIP  needs xdotool and (Xvfb or a live \$DISPLAY)"
  echo "        Ubuntu/Debian:  sudo apt install xdotool xvfb"
else
  TMP="$(mktemp -d)"
  SOCK="$TMP/gs.sock"
  TITLE="gizaB_$$"
  XVFB_PID=""; SRV_PID=""; CLI_PID=""
  cleanup_b() {
    [ -n "$CLI_PID" ]  && kill "$CLI_PID"  2>/dev/null
    [ -n "$SRV_PID" ]  && kill "$SRV_PID"  2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    rm -rf "$TMP"
  }
  trap cleanup_b RETURN

  # A minimal GSP client that mimics show_interactive's run loop: announce a
  # window, then block reading the socket. It exits 0 printing EOF the moment
  # the server shuts the socket down (the [B] fix), or times out -> exit 1.
  cat > "$TMP/btest.c" <<'EOF'
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "giza-server-protocol.h"
static int W(int fd,const void*b,size_t n){const char*p=b;while(n){ssize_t r=write(fd,p,n);if(r<=0)return -1;p+=r;n-=r;}return 0;}
static int snd(int fd,uint8_t t,uint32_t*seq,const void*pl,uint32_t len){
  gsp_header_t h; h.magic=GSP_MAGIC; h.version=GSP_VERSION; h.type=t; h.flags=0; h.length=len; h.seq=(*seq)++;
  if(W(fd,&h,sizeof h)) return -1; if(len&&W(fd,pl,len)) return -1; return 0; }
static void on_alarm(int s){(void)s; fprintf(stderr,"BLOCKED\n"); _exit(1);}
int main(int argc,char**argv){
  if(argc<2){fprintf(stderr,"usage: btest TITLE\n");return 2;}
  char path[256]; gsp_resolve_sock_path(path,sizeof path);
  int fd=socket(AF_UNIX,SOCK_STREAM,0); if(fd<0){perror("socket");return 2;}
  struct sockaddr_un a; memset(&a,0,sizeof a); a.sun_family=AF_UNIX;
  strncpy(a.sun_path,path,sizeof a.sun_path-1);
  if(connect(fd,(struct sockaddr*)&a,sizeof a)){perror("connect");return 2;}
  uint32_t seq=0; gsp_newwin_t nw={400,300};
  snd(fd,GSP_MSG_NEWWIN,&seq,&nw,sizeof nw);
  snd(fd,GSP_MSG_TITLE,&seq,argv[1],(uint32_t)strlen(argv[1]));
  /* drain the NEWWIN ACK, then block exactly like the run loop does */
  signal(SIGALRM,on_alarm); alarm(10);
  gsp_header_t h;
  for(;;){ ssize_t r=read(fd,&h,sizeof h);
    if(r==0){ fprintf(stderr,"EOF\n"); return 0; }   /* <-- the [B] win */
    if(r<0){  fprintf(stderr,"EOF\n"); return 0; } }
}
EOF
  CF="$(pkg-config --cflags cairo 2>/dev/null)"
  if cc -O2 -I viewer $CF -o "$TMP/btest" "$TMP/btest.c" 2>/tmp/gs_btest_build.log; then
    : > /tmp/gs_btest.out
    GIZA_SERVER_SOCK="$SOCK" ./giza_server >/dev/null 2>&1 &
    SRV_PID=$!
    # Start Xvfb if we have no display
    if [ -z "${DISPLAY:-}" ]; then
      Xvfb :97 -screen 0 640x480x24 >/dev/null 2>&1 & XVFB_PID=$!
      export DISPLAY=:97; sleep 1
    fi
    # wait for the socket
    for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done

    GIZA_SERVER_SOCK="$SOCK" "$TMP/btest" "$TITLE" 2>"$TMP/btest.out" & CLI_PID=$!
    # give the window time to map + take its title, then close it via WM_DELETE
    win=""
    for _ in $(seq 1 50); do
      win="$(xdotool search --name "$TITLE" 2>/dev/null | head -1)"
      [ -n "$win" ] && break; sleep 0.1
    done
    if [ -n "$win" ]; then
      xdotool windowclose "$win"          # sends WM_DELETE_WINDOW ClientMessage
    else
      echo "  WARN  could not find the test window by title (continuing)"
    fi
    # the client should now unblock with EOF; wait for it (it self-times-out at 10s)
    if wait "$CLI_PID"; then rc=0; else rc=$?; fi; CLI_PID=""
    if grep -q EOF "$TMP/btest.out" && [ "$rc" = 0 ]; then
      ok "xlib [B] interactive client received EOF on window close (no hang)"
    else
      no "xlib [B] client did not get EOF: $(cat "$TMP/btest.out")"
    fi
  else
    no "could not build the GSP test client — see /tmp/gs_btest_build.log"
  fi
  trap - RETURN; cleanup_b
fi

# ---------------------------------------------------------------------------
hdr "manual checklist (GUI — needs a human)"
cat <<'TXT'
  Cocoa [B]  run a show_interactive script; close the window's red button
             (and, with several tabs from one process, close ONE tab):
               - the script proceeds to its next stage WITHOUT Cmd-Q
               - closing one tab leaves sibling tabs alive
  Cocoa [A]  let a plain show() script finish (client-absent window), open
             File menu:  Save as PDF / Save as SVG are GRAYED OUT,
                         Save as PNG stays ENABLED.
             On a live show_interactive window all three are enabled.
  Note       on macOS use the MacPorts toolchain:
               CC=clang OBJC=clang PKG_CONFIG=/opt/local/bin/pkg-config \
                 ./configure --with-viewer=cocoa
TXT

# ---------------------------------------------------------------------------
hdr "summary"
echo "  $pass passed, $fail failed"
[ "$fail" = 0 ]
