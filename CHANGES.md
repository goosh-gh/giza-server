# Changes

All notable changes to **giza-server** are recorded here. Dates are
approximate; the authoritative record is the git history.

The project is developed as a series of `serverN` milestones; they are
grouped below under the backend/feature themes they belong to.

## Unreleased

### Added — 2D double-click and press-drag reporting (Cocoa)

The Cocoa viewer reports two more 2D gestures to the client. A double-click is
sent as `PICK` with the double-click bit set in the button byte, and the native
zoom reset now fires only when the plot is zoomed (a double-click on an unzoomed
plot is purely the client gesture). A button-held drag streams `CURSOR` events
with the button byte non-zero. Together these let a client open a zoomed child
window for the subplot under the cursor and scrub along it. Xlib is unchanged.

### Added — `/gs` registration patch for giza 2.0.0

`patches/giza-v2.0.0-drivers.patch` registers the `/gs` device against
**giza 2.0.0**. `GIZA_DEVICE_GS 30` now sits below the new
`AQT`/`OSXCOCOA`/`CAIRO` entries, the five dispatch sites are placed after
the `#ifdef _GIZA_HAS_OSXCOCOA` blocks as independent
`#ifdef _GIZA_HAS_GS` sections, and the `Makefile.am` SOURCES hunks account
for `giza-driver-cairo.c` and `giza-streamplot.c`. `/xs` and `/xserve`
continue to resolve to `GIZA_DEVICE_GS`.

`patches/giza-v1.5.0-drivers.patch` is kept alongside it for giza 1.5.0.

Verified on Ubuntu 24.04 (aarch64): the patch applies to a pristine
giza-2.0.0 tree with both `git apply` and `patch -p1`, giza builds, its own
test suite passes (19 C tests, 10 Fortran tests), and the PGPLOT `/GS` path
works. Not yet tested on macOS.

### Fixed — Xlib viewer destroyed the window when the client sent `GSP_MSG_CLOSE`

`_giza_close_device_gs()` sends `GSP_MSG_CLOSE` to mean *the window stays,
the connection closes* — this is what makes `/gs` a persistent-window
device. The GTK and Cocoa viewers ACK and disconnect, leaving the window up.
The Xlib viewer instead dispatched `CMD_CLOSE`, tearing down the tab as soon
as the client program exited. It now ACKs and falls through to the existing
disconnect path, which clears `client_fd` and closes the socket without
touching the window.

This only showed up through the PGPLOT `/GS` route; `PDL::Graphics::Cairo`'s
`Driver::GS` disconnects without sending `CLOSE` and was unaffected.

### Changed — Linux default backend is now Xlib (was GTK 3)

`./configure` with no `--with-viewer` argument now selects the **Xlib**
viewer on Linux, not GTK 3. The Xlib backend is the more capable of the two
(interactive sliders, mouse zoom/pan, cursor/pick reporting, per-PID tabs)
and links only against X11 + Cairo — no GTK/GLib/GIO. This lighter
dependency footprint matches the preference expressed in
[giza issue #85](https://github.com/danieljprice/giza/issues/85).

The GTK 3 backend remains available and unchanged, but is now considered a
legacy display-only backend. Build it explicitly with
`./configure --with-viewer=gtk`.

**Migration:** if you relied on `./configure` producing a GTK build on Linux,
add `--with-viewer=gtk` to keep that behaviour.

### Fixed — server15: macOS mouse events now delivered correctly

- **`[win setAcceptsMouseMovedEvents:YES]`** added in `_create_window_on_main`
  (before `makeKeyAndOrderFront:`). Without this flag, `mouseMoved:` is never
  delivered by the Cocoa runtime even when a tracking area with
  `NSTrackingMouseMoved` is registered — cursor readout and `CURSOR`/`PICK`
  messages were silently dropped on macOS while Xlib (PointerMotionMask) worked
  fine.
- **`-[GsView updateTrackingAreas]`** implemented so that the `NSTrackingArea`
  is re-registered after every window resize, keeping cursor tracking alive
  across resizes.

Both fixes are in `viewer/giza-server-cocoa.m`; the Xlib and GTK backends are
unaffected.

### Added — server14: mouse interaction

- New protocol messages `GSP_MSG_ZOOM` (0x17), `GSP_MSG_CURSOR` (0x18) and
  `GSP_MSG_PICK` (0x19), with packed payloads `gsp_zoom_t` (12 B) and
  `gsp_cursor_t` (9 B).
- Zoom and pan are handled entirely inside the viewer (the received PNG is
  scaled/offset on screen — no re-render round-trip), so they work even after
  the client has exited.
- Server→client reporting of cursor position (`CURSOR`), clicks (`PICK`) and
  zoom/pan state (`ZOOM`), as image fractions in `[0,1]`; the client maps
  these to data coordinates using its own axis limits.
- **Cocoa:** pinch / Ctrl+scroll zoom, drag-to-pan (while zoomed), live
  coordinate readout label, double-click reset.
- **Xlib:** wheel zoom, drag-to-pan, middle-click reset, motion cursor;
  per-container zoom/pan state applied via Cairo `translate`/`scale`.
- `examples/client_mouse.c` — a libgiza-free C demo of the CURSOR/PICK/ZOOM
  channels (companion to `client_slider.c`).
- `Driver::GS` (in PDL::Graphics::Cairo) gains `on_cursor` / `on_pick` /
  `on_zoom` callbacks for `show_interactive`.

## Xlib backend feature parity (server9–server13)

- **server13:** `GSP_MSG_RESIZE` (0x16) with debounce; coalesce SLIDER and
  RESIZE into a unified redraw; Xlib tab-ordering fix via a monotonic
  `tab_seq`.
- **server12:** Save-menu gray-out; red-button close →
  `shutdown(fd, SHUT_RDWR)` so interactive clients get EOF; fixed an Xlib
  `client_fd` slot-mismatch bug (windows' real slot kept `client_fd == -1`).
- **server11:** macOS driver unification (`Driver::GS` promoted as default,
  retiring `Driver::OSX`/`pdlcairo_viewer`); per-PID tab grouping
  (`LOCAL_PEERPID` on macOS, `SO_PEERCRED` on Linux); socket isolation via
  `$GIZA_SERVER_SOCK`; test-harness hardening.
- **server9:** GTK-free **Xlib viewer** (`giza-server-xlib.c`) added, with the
  `--with-viewer=gtk|xlib|cocoa|auto` configure option. Interactive sliders
  and per-PID tab grouping followed, bringing the Xlib backend to parity with
  Cocoa except for vector `File ▸ Save` and `RESIZE` replot.

## Save and bidirectional control (server5–server8)

- **server8:** true vector PDF/SVG via the `GSP_MSG_SAVEREQ` (0x14) /
  `GSP_MSG_SAVEDATA` (0x15) reverse-channel protocol, verified end-to-end on
  macOS and in the Perl `Driver::GS`.
- **server7:** `File ▸ Save` menu (PNG/PDF/SVG) with `validateMenuItem:`
  gray-out.
- **server5–6:** bidirectional `GSP_MSG_SLIDER` (0x13) channel; packed
  `gsp_slider_t`; coalescing via non-blocking peek; per-fd `write_lock` so
  server→client writes do not interleave with the ACK path.

## Initial server (server0–server4)

- GSP wire protocol (16-byte header: magic `"GIZA"` + version + type + flags
  + length + seq).
- GTK 3 viewer (`giza-server-gtk.c`) and native Cocoa viewer
  (`giza-server-cocoa.m`); autotools build.
- Client handshake with auto-launch (per-user socket; fork/exec `giza_server`
  detached via `setsid`, then poll until listening) — the `pgxwin_server`
  persistence model for the giza era, addressing giza issue #85.
- `/gs` device driver as a patch against giza 1.5.0; window persists after the
  client exits.

## v0.2

- macOS Cocoa viewer backend added (`NSWindow`/`NSView`); SIGPIPE ignore,
  `disableAutomaticTermination`, block-capture fixes.

## v0.1

- Initial release: persistent plot-window server with the GTK 3 viewer and the
  `/gs` driver patch.
