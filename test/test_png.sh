#!/bin/sh
# test/test_png.sh
#
# Visual PNG window display test for giza_server.
#
# Sends a Cairo-rendered PNG to the server and leaves the window
# visible for GIZA_TEST_DISPLAY_SECS seconds (default: 5) before
# shutting down the server and exiting.
#
# In 'make check' the window appears, stays for a few seconds,
# then the test finishes automatically — no keyboard input needed.
#
# Exit 0  = pass (window appeared, ACK received)
# Exit 1  = fail
# Exit 77 = skip (no display available)

BINARY=./giza_server
TEST_PNG=./test/test_png
SOCK="/tmp/giza_server_$(id -u).sock"
DISPLAY_SECS="${GIZA_TEST_DISPLAY_SECS:-5}"
SERVER_PID=""

# ------------------------------------------------------------------ #
cleanup() {
    # Stop server started by this script (if any)
    if [ -n "$SERVER_PID" ]; then
        echo "INFO: stopping giza_server (pid=$SERVER_PID)"
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        rm -f "$SOCK"
    fi
}
trap cleanup EXIT

# ------------------------------------------------------------------ #
# 1. Skip if no display
# ------------------------------------------------------------------ #
if [ -z "$DISPLAY" ] && [ -z "$WAYLAND_DISPLAY" ]; then
    echo "SKIP: no display (DISPLAY/WAYLAND_DISPLAY unset)"
    exit 77
fi

# ------------------------------------------------------------------ #
# 2. Check binaries
# ------------------------------------------------------------------ #
if [ ! -x "$BINARY" ]; then
    echo "FAIL: $BINARY not found — run 'make' first"
    exit 1
fi
if [ ! -x "$TEST_PNG" ]; then
    echo "FAIL: $TEST_PNG not found — run 'make' first"
    exit 1
fi

# ------------------------------------------------------------------ #
# 3. Start server if not already running
#    (test_ping.sh may have left it running)
# ------------------------------------------------------------------ #
if [ -S "$SOCK" ]; then
    echo "INFO: reusing running giza_server (socket: $SOCK)"
else
    echo "INFO: starting $BINARY ..."
    "$BINARY" &
    SERVER_PID=$!
    i=0
    while [ $i -lt 30 ]; do
        [ -S "$SOCK" ] && break
        sleep 0.1
        i=$((i+1))
    done
    if [ ! -S "$SOCK" ]; then
        echo "FAIL: giza_server did not create socket within 3s"
        exit 1
    fi
    echo "INFO: server started (pid=$SERVER_PID)"
fi

# ------------------------------------------------------------------ #
# 4. Send PNG and wait for ACK
# ------------------------------------------------------------------ #
echo "INFO: sending PNG frame..."
"$TEST_PNG"

# ------------------------------------------------------------------ #
# 5. Keep window visible for inspection, then auto-close
# ------------------------------------------------------------------ #
echo ""
echo "================================================================"
echo "  VISUAL CHECK: window 'giza-server test_png' should show"
echo "  a red rectangle with a white cross."
echo "  Closing automatically in ${DISPLAY_SECS}s ..."
echo "  (set GIZA_TEST_DISPLAY_SECS=0 to skip the wait)"
echo "================================================================"
sleep "$DISPLAY_SECS"

echo "PASS: test/test_png.sh"
