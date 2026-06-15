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
| GUI toolkit | Xlib/Xt | GTK 3 or Xlib (Linux) / Cocoa (macOS) |
| WSL2 / Wayland | No | Yes (no X atoms needed) |
| macOS native | No | Yes (NSWindow/NSView, v0.2+) |

The calling program opens the device `/gs`, renders frames as PNG into the server
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
Resizing an interactive window likewise replots at the new size over the
server→client `RESIZE` channel, so the figure is re-laid-out rather than
bitmap-scaled.

Mouse interaction is verified on macOS as well: zooming (pinch or
Ctrl+scroll) and panning (drag while zoomed) are handled entirely in the
viewer, and the cursor position, clicks, and zoom/pan state are reported
back to the client over the `CURSOR`, `PICK`, and `ZOOM` channels — see
**Mouse interaction** below.

## Backends

The viewer is a **standalone binary** (`giza_server`) separate from the `/gs`
driver inside libgiza.  The backend is selected at *configure* time and has no
effect on the protocol or the driver.

| Platform | Backend | Dependencies |
|----------|---------|--------------|
| Linux / WSL2 | **Xlib + Cairo** (default, no GTK/GLib) | `libx11-dev libcairo2-dev` |
| Linux / WSL2 | GTK 3 + Cairo (legacy, display-only) | `libgtk-3-dev libcairo2-dev` |
| macOS (Apple Silicon / x86_64) | Cocoa (NSWindow) | system Cocoa.framework |

The backend is detected automatically at configure time (`--with-viewer=auto`,
which picks **Xlib** on Linux and Cocoa on macOS).  Override with:

```bash
./configure --with-viewer=xlib     # Xlib only (Linux default) — no GTK/GLib/GIO
./configure --with-viewer=gtk      # GTK 3 (Linux, display-only legacy backend)
./configure --with-viewer=cocoa    # macOS Cocoa
```

**Feature parity.** The **Cocoa** (macOS) viewer implements the full
interactive set: `SLIDER` reverse channel, `File ▸ Save` (PNG and
reverse-channel PDF/SVG), `RESIZE` resize replot (re-renders an interactive
window's figure at the new size on resize, rather than bitmap-scaling),
**mouse zoom/pan and cursor/pick reporting** (`CURSOR`/`PICK`/`ZOOM`
channels), per-PID tab grouping, and the close-signals-the-client lifecycle.
The **Xlib** (Linux) viewer implements
the `SLIDER` reverse channel (a bottom strip drives slider id 0 = k, a right
strip drives id 1 = A; dragging sends the value to the client),
**mouse zoom/pan and cursor/pick reporting** (wheel to zoom, drag to pan
while zoomed, middle-click to reset), per-PID tab
grouping (figures from the same client process share one window with a
cairo-drawn tab bar — click to switch, per-tab `×` to close, WM-close discards
the whole group), and the close-signals-the-client lifecycle; vector
`File ▸ Save` and `RESIZE` resize replot are not yet wired on Xlib. The
**GTK** viewer is the original display-only backend (PNG frames, titles,
persistence) — no sliders, save, mouse interaction, or tabs. On Linux, the
default `./configure` now auto-detects **Xlib**, so you get the interactive
sliders, mouse zoom/pan, and tabs out of the box; the legacy GTK backend is
still available via `--with-viewer=gtk` if you specifically need it.

## Architecture

```
┌─────────────────────────────┐      Unix socket         ┌─────────────────────────┐
│  Your program               │  ──────────────────────► │  giza-server            │
│  (giza /gs driver)          │   GSP wire protocol      │  Linux:  GTK 3 or Xlib viewer   │
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
    uint8_t  type;     /* PNG, NEWWIN, CLOSE, PING/PONG, TITLE, SLIDER,
                          SAVEREQ, SAVEDATA, RESIZE, ZOOM, CURSOR, PICK ... */
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
| `SAVEREQ` | server→client | **none** (reverse-channel save request) |
| `SAVEDATA` | client→server | **none** (rendered vector bytes) |
| `RESIZE` | server→client | **none** (reverse-channel resize replot) |
| `ZOOM` | server→client | **none** (zoom/pan changed, optional notify) |
| `CURSOR` | server→client | **none** (cursor moved, image fraction) |
| `PICK` | server→client | **none** (mouse click, image fraction) |

`SLIDER` carries a 5-byte packed `gsp_slider_t` payload (`uint8_t` slider id
+ `float` value), letting one message type drive any number of sliders.

`SAVEREQ`/`SAVEDATA` implement the **reverse-channel vector save**: when the
user picks *Save as PDF/SVG* in the viewer, the server sends `SAVEREQ(fmt)`
to the still-running client, which re-renders the current figure to the
requested vector format and returns the bytes as `SAVEDATA`. Raster *Save as
PNG* needs no round-trip — the viewer holds the last received PNG and writes
it directly.

`RESIZE` implements **resize replot**: when the user resizes a window whose
client is still alive (an interactive session), the viewer coalesces the
live-drag burst and, once it settles, sends `RESIZE(width_px, height_px)`
(an 8-byte packed `gsp_resize_t`). The client re-renders its current figure
at the new size and pushes it back through the normal `PNG` path, so the plot
is re-laid-out for the new geometry instead of being bitmap-scaled. A
client-absent window (a plain `show()` window after its script exited) gets no
`RESIZE` — the viewer just letterbox-scales its last frame.

`ZOOM`, `CURSOR`, and `PICK` implement **mouse interaction**. Zooming and
panning are handled entirely inside the viewer (the received PNG is scaled
and offset on screen — no re-render round-trip), so they work even for a
client that has already exited. When a client is alive, the viewer also
reports activity back so the client can react:

- `CURSOR(x, y, buttons)` — sent on mouse-move, where `(x, y)` are **image
  fractions** in `[0, 1]`: `(0, 0)` is the top-left of the rendered image and
  `(1, 1)` the bottom-right. The client converts to data coordinates using its
  own `xlim`/`ylim`.
- `PICK(x, y, button)` — sent on a mouse-button press inside the plot, same
  image-fraction convention; `button` is 1 = left, 2 = middle, 3 = right.
- `ZOOM(zoom, pan_x, pan_y)` — sent when the zoom factor or pan offset
  changes (`zoom` 1.0 = fit, `pan_*` fractional offsets). Purely a notify;
  the client need not act on it.

All three are fire-and-forget (`gsp_cursor_t` is a 9-byte packed
`float x, float y, uint8_t buttons`; `gsp_zoom_t` is 12 bytes of three
`float`s). Each is optional on the client side — `Driver::GS` invokes the
matching callback (`on_cursor` / `on_pick` / `on_zoom`) only if one was
registered, and otherwise consumes and ignores the message.

### Mouse interaction (user controls)

| Action | macOS (Cocoa) | Linux (Xlib) |
|--------|---------------|--------------|
| Zoom | pinch, or **Ctrl + scroll** | **scroll wheel** |
| Pan (only while zoomed) | drag the plot, or two-finger scroll | drag the plot |
| Reset zoom/pan | **double-click** | **middle-click** |
| Pick (report click) | single click in plot | single click in plot |
| Cursor readout | move the mouse over the plot | move the mouse over the plot |

On macOS a live coordinate label is shown in the top-right of the plot area
while the cursor is inside it. The coordinates reported to the client are
image fractions; converting them to data coordinates is the client's job (it
knows the axis limits and margins).

## Build

### Linux — Xlib (default)

The Xlib backend is the Linux default. It links only against `libX11`,
`cairo`, `cairo-xlib`, `libpng`, and `libpthread` — all standard X11 system
libraries, no GTK/GLib/GIO — and carries the full interactive feature set
(sliders, mouse zoom/pan, cursor/pick reporting, tabs). This lighter
dependency footprint matches the preference expressed in giza issue #85.

```bash
# Prerequisites
sudo apt install libx11-dev libcairo2-dev

# Build
autoreconf -fi
./configure            # auto-detects Xlib on Linux
make

# Verify no GTK/GLib linkage
ldd giza_server | grep -i gtk   # should be empty

sudo make install
```

### Linux — GTK 3 (legacy, display-only)

The original GTK backend is display-only (no sliders, save, mouse
interaction, or tabs) and is kept for compatibility. Build it explicitly:

```bash
# Prerequisites
sudo apt install libgtk-3-dev libcairo2-dev

# Build
autoreconf -fi
./configure --with-viewer=gtk
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
autoreconf -if
./configure CC=clang OBJC=clang PKG_CONFIG=/opt/local/bin/pkg-config
make -j4
make check
sudo make install
pkill giza_server   # terminate any running instance before testing
```

Force a specific backend:

```bash
./configure --with-viewer=xlib     # Xlib only (Linux default, full features)
./configure --with-viewer=gtk      # GTK 3 (Linux, legacy display-only)
./configure --with-viewer=cocoa    # macOS Cocoa
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

`examples/client_mouse.c` — the mouse counterpart, demonstrating the
server→client `GSP_MSG_CURSOR` / `GSP_MSG_PICK` / `GSP_MSG_ZOOM` channels.
Also pure GSP + Cairo, no libgiza.

It draws a scatter plot. Moving the mouse over the plot prints the cursor
position (image fraction and the data coordinates this client maps it to);
clicking reports the nearest data point and highlights it; scrolling/pinching
to zoom and dragging to pan print the zoom/pan state. Coordinates arrive as
image fractions in `[0, 1]`, which the client converts to data coordinates
using the plot rectangle it drew into.

```bash
# build (cairo required; the relative include resolves ../viewer/protocol.h)
clang examples/client_mouse.c \
  $(pkg-config --cflags --libs cairo) -lm -o client_mouse

# run: start the server first, then the client
./giza_server &
./client_mouse
```

Cursor events are coalesced the same way as slider events, so moving the
mouse quickly will not flood the client. See **Mouse interaction** above for
the per-platform controls.

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

- Windows opened by the same client process are tab-grouped: they share a
  per-PID `tabbingIdentifier` and are inserted explicitly with
  `addTabbedWindow:ordered:` so creation order (1, 2, 3) is preserved
  regardless of the user's "prefer tabs" system setting.

- `GSP_MSG_TITLE` is fire-and-forget on both backends — **no ACK**.

- **Bidirectional sliders.** Native `NSSlider`s carry a `tag` used as the
  slider id in the packed `gsp_slider_t` payload. The server→client `SLIDER`
  write shares the client's socket fd with the connection's ACK path; since
  ACK is written from the per-connection thread and `SLIDER` from the main
  thread, every write to one fd is serialized through a per-window
  `write_lock` (`_send_msg_locked`).

- **Lifetime, close & quit.** The app stays alive after a client exits (the
  `pgxwin_server` persistence role) via `disableAutomaticTermination`.
  Closing a window (red button) is an explicit discard: the slot is
  invalidated and, when it was the last live window on that client's socket,
  the server `shutdown()`s the socket so an *interactive* client (one in a
  `show_interactive` run loop) receives EOF and returns — no ⌘Q needed.
  Explicit quit (⌘Q or the **Quit** menu item) is honored because
  `applicationShouldTerminate:` returns `NSTerminateNow`.

- **File ▸ Save.** *Save as PNG* writes the last received frame straight from
  the in-memory buffer (works even after the client has exited). *Save as
  PDF* / *Save as SVG* use the `SAVEREQ`/`SAVEDATA` reverse channel to have
  the live client re-render true vector output. For a client-absent window the
  vector items gray out (`validateMenuItem:`), since there is no client left
  to re-render; PNG stays enabled.

- **Mouse zoom/pan & reporting.** The `GsView` keeps its own zoom/pan state and
  applies it inside `drawRect:` (the same letterbox maths, then scaled by
  `zoom` and offset by `pan_x/pan_y`), so zooming never round-trips to the
  client. An `NSTrackingArea` enables `mouseMoved:` for the cursor readout;
  `magnifyWithEvent:` and `scrollWheel:` drive zoom, `mouseDragged:` drives pan
  (only while zoomed). View points are converted to **image fractions** through
  `_viewPoint:toImageFx:fy:`, which inverts the letterbox+zoom transform and
  flips Cocoa's bottom-left origin to the image's top-down convention before
  the value is sent as `CURSOR`/`PICK`. The Xlib viewer mirrors this with
  per-container zoom/pan state and a matching `_cont_to_image_frac` (no y-flip
  there, since X is already top-down).

## Files

```
viewer/
  giza-server-protocol.h   — GSP wire format (shared by driver + viewer)
  giza-server-gtk.c        — GTK 3 viewer (Linux, legacy display-only)
  giza-server-xlib.c       — Xlib viewer (Linux, default; no GTK/GLib)
  giza-server-cocoa.m      — Cocoa viewer (macOS)
  giza-server-main.m       — macOS main-thread bootstrapper
src/
  giza-driver-gs.c         — /gs device driver (add to libgiza)
  giza-driver-gs-private.h — driver header
examples/
  client_slider.c          — bidirectional slider demo (GSP + Cairo)
  client_mouse.c           — mouse cursor/pick/zoom demo (GSP + Cairo)
patches/
  giza-v1.5.0-drivers.patch — patch for giza upstream
```

## License

LGPL-2.1 — same as giza itself.
