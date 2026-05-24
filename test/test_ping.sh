#!/bin/sh
# test/test_ping.sh
#
# Automated ping/pong test for giza_server.
# Starts giza_server, sends PING, checks PONG, then stops the server.
# The server is left running for subsequent tests in the same 'make check'.
#
# Exit 0 = pass, exit 1 = fail

set -e

BINARY=./giza_server
TEST_PING=./test/test_ping
SOCK="/tmp/giza_server_$(id -u).sock"

# ------------------------------------------------------------------ #
# 1. Check binaries exist
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
# 2. Start server if not already running
# ------------------------------------------------------------------ #
if [ -S "$SOCK" ]; then
    echo "INFO: giza_server already running (socket: $SOCK)"
else
    echo "INFO: starting $BINARY ..."
    "$BINARY" &
    # Wait up to 3 seconds for socket to appear
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
    echo "INFO: server started (socket=$SOCK)"
fi

# ------------------------------------------------------------------ #
# 3. Ping/pong
# ------------------------------------------------------------------ #
echo "INFO: running ping/pong..."
"$TEST_PING"

# NOTE: server is intentionally left running for test_png.sh
echo "PASS: test/test_ping.sh"
