#!/bin/sh
# test/test_png.sh
#
# Visual PNG window display test for giza_server.
#
# macOS: no DISPLAY needed (Cocoa backend).
# Linux: skips if neither DISPLAY nor WAYLAND_DISPLAY is set.
#
# Exit 0  = pass
# Exit 1  = fail
# Exit 77 = skip (no display available on Linux)

BINARY=./giza_server
TEST_PNG=./test/test_png
SOCK="/tmp/giza_server_$(id -u).sock"
DISPLAY_SECS="${GIZA_TEST_DISPLAY_SECS:-5}"
SERVER_PID=""

# ------------------------------------------------------------------ #
cleanup() {
    if [ -n "$SERVER_PID" ]; then
        echo "INFO: stopping giza_server (pid=$SERVER_PID)"
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        rm -f "$SOCK"
    fi
}
trap cleanup EXIT

# ------------------------------------------------------------------ #
# 1. Display check (macOS Cocoa: no DISPLAY needed)
# ------------------------------------------------------------------ #
case "$(uname)" in
    Darwin)
        : # Cocoa always has a display
        ;;
    *)
        if [ -z "$DISPLAY" ] && [ -z "$WAYLAND_DISPLAY" ]; then
            echo "SKIP: no display (DISPLAY/WAYLAND_DISPLAY unset)"
            exit 77
        fi
        ;;
esac

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
# ------------------------------------------------------------------ #
if [ -S "$SOCK" ]; then
    echo "INFO: reusing running giza_server (socket: $SOCK)"
else
    echo "INFO: starting $BINARY ..."
    "$BINARY" &
    SERVER_PID=$!
    i=0
    while [ $i -lt 100 ]; do
        [ -S "$SOCK" ] && break
        sleep 0.1
        i=$((i+1))
    done
    if [ ! -S "$SOCK" ]; then
        echo "FAIL: giza_server did not create socket within 10s"
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
# 5. Keep window visible, then auto-close
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
