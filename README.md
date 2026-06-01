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

Bidirectional control is also verified on macOS: native sliders in the
viewer send their values back to the client (server→client `SLIDER`), which
recomputes the plot and pushes a new frame live — see **Examples** below.

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
    uint8_t  type;     /* PNG, NEWWIN, CLOSE, PING/PONG, TITLE, SLIDER ... */
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
| `SLIDER` | server→client | **none** (fire-and-forget) |

`SLIDER` carries a 5-byte packed `gsp_slider_t` payload (`uint8_t` slider id
+ `float` value), letting one message type drive any number of sliders.

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

## Examples

`examples/client_slider.c` — a minimal **bidirectional** client that
demonstrates server→client messaging (`GSP_MSG_SLIDER`). It is pure GSP +
Cairo with **no libgiza**, deliberately isolating the bidirectional
protocol from the device-driver layer.

The viewer shows a sine wave with two native sliders — horizontal for
frequency, vertical for amplitude. Dragging either one recomputes the wave
on the client and pushes a new frame back to the window, live:

```bash
# build (cairo required; the relative include resolves ../viewer/protocol.h)
clang examples/client_slider.c \
  $(pkg-config --cflags --libs cairo) -o client_slider

# run: start the server first, then the client
./giza_server &
./client_slider
```

Slider events are coalesced on the client (a non-blocking `MSG_DONTWAIT`
peek drains the backlog, latest value wins), so fast dragging stays
responsive even when redraws are heavy. Quit the viewer with ⌘Q or the
**Quit** menu item.

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

- **Bidirectional sliders.** Native `NSSlider`s carry a `tag` used as the
  slider id in the packed `gsp_slider_t` payload. The server→client `SLIDER`
  write shares the client's socket fd with the connection's ACK path; since
  ACK is written from the per-connection thread and `SLIDER` from the main
  thread, every write to one fd is serialized through a per-window
  `write_lock` (`_send_msg_locked`).

- **Lifetime & quit.** The app stays alive after a client exits (the
  `pgxwin_server` persistence role) via `disableAutomaticTermination`;
  explicit quit (⌘Q or the **Quit** menu item) is honored because
  `applicationShouldTerminate:` returns `NSTerminateNow`. A `File` menu is
  present as a stub for future *Save as PDF / SVG*.

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
examples/
  client_slider.c          — bidirectional slider demo (GSP + Cairo)
patches/
  giza-v1.5.0-drivers.patch — patch for giza upstream
```

## License

LGPL-2.1 — same as giza itself.
