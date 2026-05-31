# giza-server

A persistent plot-window server for [giza](https://github.com/danieljprice/giza) / PGPLOT,
inspired by the classic `pgxwin_server` from PGPLOT/X11.

## Overview

In PGPLOT, `pgxwin_server` kept plot windows alive after the calling program exited.
`giza-server` provides the same behaviour for the modern giza era:

| Feature | pgxwin_server | giza-server |
|---------|--------------|-------------|
| Rendering | X11 Pixmap | Cairo (publication quality) |
| IPC | X atoms + shared memory | Unix-domain socket |
| GUI toolkit | Xlib/Xt | GTK 3 (Linux) / Cocoa (macOS) |
| WSL2 / Wayland | No | Yes (no X atoms needed) |
| macOS native | No | Yes (NSWindow/NSView, v0.2+) |

The calling program opens the device `/GS`, renders frames as PNG into the server
via a lightweight binary protocol over a Unix-domain socket, and exits.  The server
window stays open until the user closes it.

### Client handshake (auto-launch)

A client checks for a running server on the per-user socket
(`/tmp/giza_server_$UID.sock`) and, if absent, forks and `exec`s the
`giza_server` binary (detached via `setsid`), then polls until it is
listening — exactly the handshake described in giza issue #85. Because the
server outlives the client, plot windows persist after the calling program
exits, just like `pgxwin_server`.

### Verified end-to-end

The server has been driven end-to-end on macOS (Apple Silicon, macOS 15)
from `PDL::Graphics::Cairo`, which renders a figure to an in-memory PNG
(`cairo_surface_write_to_png_stream`) and sends it over GSP with **no
temporary files** on either side. Both the bundled `test/test_png` client
and the Perl `Driver::GS` backend display correctly in native windows.

## Backends

| Platform | Backend | Toolkit |
|----------|---------|---------|
| Linux / WSL2 | GTK 3 + Cairo | `libgtk-3-dev` |
| macOS (Apple Silicon / x86_64) | Cocoa (NSWindow) | system Cocoa.framework |

The backend is detected automatically at configure time (`--with-viewer=auto`).
You can override with `--with-viewer=gtk` or `--with-viewer=cocoa`.

## Architecture

```
┌─────────────────────────────┐      Unix socket         ┌─────────────────────────┐
│  Your program               │  ──────────────────────► │  giza-server            │
│  (giza /GS driver)          │   GSP wire protocol      │  Linux:  GTK 3 viewer   │
│                             │   PNG frames, titles,    │  macOS:  Cocoa viewer   │
│  giza → Cairo PNG surface   │   NEWWIN / CLOSE msgs    │                         │
└─────────────────────────────┘                          │  persistent window(s)   │
                                                         └─────────────────────────┘
```

### Wire protocol (GSP)

16-byte fixed header + payload:

```c
typedef struct {
    uint32_t magic;    /* 0x47495A41 "GIZA" */
    uint8_t  version;
    uint8_t  type;     /* PNG, NEWWIN, CLOSE, PING/PONG, TITLE ... */
    uint16_t flags;
    uint32_t length;   /* payload bytes */
    uint32_t seq;
} gsp_header_t;
```

| Message | Direction | ACK? |
|---------|-----------|------|
| `PING`  | client→server | PONG |
| `NEWWIN` | client→server | ACK |
| `PNG`   | client→server | ACK |
| `TITLE` | client→server | **none** (fire-and-forget) |
| `CLOSE` | client→server | ACK |

## Build

### Linux (GTK 3)

```bash
# Prerequisites
sudo apt install libgtk-3-dev libcairo2-dev

# Build
autoreconf -fi
./configure            # auto-detects GTK on Linux
make
sudo make install
```

### macOS (Cocoa)

```bash
# No extra dependencies — Cocoa.framework is part of macOS SDK
# (cairo is required for the test client; install via MacPorts/Homebrew)

# Build
autoreconf -fi
./configure            # auto-detects Cocoa on macOS
make
sudo make install
```

**MacPorts note.** On a MacPorts system the default `gcc` may be picked up
by `AC_PROG_OBJC` and lacks Objective-C/Cocoa support, and `pkg-config`
lives under `/opt/local`. Use the Xcode `clang` and the MacPorts
`pkg-config` explicitly:

```bash
./configure CC=clang OBJC=clang PKG_CONFIG=/opt/local/bin/pkg-config
```

Force a specific backend:

```bash
./configure --with-viewer=cocoa    # macOS Cocoa
./configure --with-viewer=gtk      # GTK 3 (Linux)
```

## Test

```bash
make check
```

- `test/test_ping.sh` — automated ping/pong (no display needed on macOS)
- `test/test_png.sh`  — visual: red rectangle + white cross in a window

On macOS the Cocoa viewer does not need `DISPLAY`.
On Linux the GTK viewer requires `DISPLAY` or `WAYLAND_DISPLAY`; tests are
skipped (exit 77) in headless CI environments.

## macOS Cocoa design notes

The macOS backend (`viewer/giza-server-cocoa.m` + `viewer/giza-server-main.m`)
follows the same pattern as the giza `/osx` driver (PR #86):

- `giza-server-main.m` **hijacks `main()`**: calls `giza_server_cocoa_init()`
  (which creates `[NSApplication sharedApplication]`), then launches a POSIX
  worker thread for the accept loop, and finally calls `[NSApp run]` on the
  main thread — so `NSApp` always runs on the OS-designated main thread.

- All `NSWindow` / `NSView` operations are dispatched to the main queue via
  `dispatch_async(dispatch_get_main_queue(), ^{})`, replacing GTK's
  `g_idle_add()`.

- PNG frames are decoded as `NSImage` from `NSData` in memory (no temp files).

- Windows share a common `tabbingIdentifier` so macOS tab-groups them
  automatically (same trick as `PDL::Graphics::Cairo`'s `pdlcairo_viewer`).

- `GSP_MSG_TITLE` is fire-and-forget on both backends — **no ACK**.

## Files

```
viewer/
  giza-server-protocol.h   — GSP wire format (shared by driver + viewer)
  giza-server-gtk.c        — GTK 3 viewer (Linux)
  giza-server-cocoa.m      — Cocoa viewer (macOS)
  giza-server-main.m       — macOS main-thread bootstrapper
src/
  giza-driver-gs.c         — /gs device driver (add to libgiza)
  giza-driver-gs-private.h — driver header
patches/
  giza-v1.5.0-drivers.patch — patch for giza upstream
```

## License

LGPL-2.1 — same as giza itself.
