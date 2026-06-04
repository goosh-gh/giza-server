#!/bin/sh
# test/test_ping.sh
#
# Automated ping/pong test for giza_server.
# Starts giza_server, sends PING, checks PONG, then stops the server.
#
# Exit 0  = pass
# Exit 1  = fail
# Exit 77 = skip (GTK backend needs a display; Cocoa backend: never skip)

set -e

BINARY=./giza_server
TEST_PING=./test/test_ping
SOCK="/tmp/giza_server_$(id -u).sock"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        rm -f "$SOCK"
    fi
}
trap cleanup EXIT

# ------------------------------------------------------------------ #
# 1. Display check
#    GTK backend: needs DISPLAY or WAYLAND_DISPLAY.
#    Cocoa backend (macOS): no check needed.
# ------------------------------------------------------------------ #
case "$(uname)" in
    Darwin)
        : # Cocoa — always OK
        ;;
    *)
        # GTK needs a display even for headless ping/pong (gtk_init)
        if [ -z "$DISPLAY" ] && [ -z "$WAYLAND_DISPLAY" ]; then
            echo "SKIP: GTK backend requires DISPLAY (headless CI)"
            exit 77
        fi
        ;;
esac

# ------------------------------------------------------------------ #
# 2. Check binaries exist
# ------------------------------------------------------------------ #
if [ ! -x "$BINARY" ]; then
    echo "FAIL: $BINARY not found — run 'make' first"
    exit 1
fi
if [ ! -x "$TEST_PING" ]; then
    echo "FAIL: $TEST_PING not found — run 'make' first"
    exit 1
fi

# ------------------------------------------------------------------ #
# 3. Start server if not already running
# ------------------------------------------------------------------ #
if [ -S "$SOCK" ]; then
    echo "INFO: giza_server already running (socket: $SOCK)"
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
    echo "INFO: server started (socket=$SOCK)"
fi

# ------------------------------------------------------------------ #
# 4. Ping/pong
# ------------------------------------------------------------------ #
echo "INFO: running ping/pong..."
"$TEST_PING"

echo "PASS: test/test_ping.sh"
