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
| GUI toolkit | Xlib/Xt | GTK 3 (Linux) |
| WSL2 support | No | Yes (no X atoms needed) |

The calling program (e.g. a Fortran/C/Perl program linked against giza) opens the device
`/GS` (or `/GIZA_SERVER`), renders frames as PNG into the server via a lightweight
binary protocol over a Unix-domain socket, and exits.  The server window stays open
until the user closes it.

## Architecture

```
┌─────────────────────────────┐      Unix socket         ┌────────────────────┐
│  Your program               │  ──────────────────────► │  giza-server       │
│  (giza /GS driver)          │   binary protocol        │  (GTK 3 viewer)    │
│                             │   PNG frames, titles,    │                    │
│  giza → Cairo PNG surface   │   NEWWIN / CLOSE msgs    │  persistent window │
└─────────────────────────────┘                          └────────────────────┘
```

### Wire protocol

A simple length-prefixed binary protocol over a Unix-domain socket:

```
┌──────────┬──────────┬────────────┐
│  type    │  length  │  payload   │
│ (uint8)  │ (uint32) │ (n bytes)  │
└──────────┴──────────┴────────────┘
```

| Message type | Code | Payload |
|---|---|---|
| `MSG_PNG`    | 0x01 | Raw PNG bytes (decoded via `cairo_image_surface_create_from_png`) |
| `MSG_TITLE`  | 0x02 | UTF-8 window title (not NUL-terminated) |
| `MSG_NEWWIN` | 0x03 | `uint32 width_px, uint32 height_px` (0 = default) |
| `MSG_CLOSE`  | 0x04 | *(no payload)* |
| `MSG_ACK`    | 0x10 | *(no payload)* |
| `MSG_PING`   | 0x11 | *(no payload)* |
| `MSG_PONG`   | 0x12 | *(no payload)* |
| `MSG_ERR`    | 0xFF | UTF-8 error string |

Maximum PNG payload: 64 MiB per frame.

## Files

```
giza-server/
├── README.md                          ← this file
├── configure.ac                       ← autoconf input
├── Makefile.am                        ← automake input
├── patches/
│   └── giza-v1.5.0-drivers.patch     ← patch adding /GS driver to giza
├── src/
│   ├── giza-driver-gs.c              ← giza /GS driver (client side)
│   └── giza-driver-gs-private.h
└── viewer/
    ├── giza-server-protocol.h        ← wire protocol definitions
    └── giza-server-gtk.c             ← GTK 3 persistent viewer (server)
```

## Build

### Prerequisites

Install the following before building.  Missing packages are the most common
cause of `./configure` failures — check these first.

**Ubuntu / Debian:**
```bash
sudo apt install \
    build-essential \
    autoconf automake libtool \
    pkg-config \
    libgtk-3-dev \
    libcairo2-dev
```

> **Note:** `autoconf`, `automake`, and `libtool` are needed to run
> `autoreconf -fi`.  They are not needed when installing from a released tarball.

### Build from source (git clone)

> **Important:** `autoreconf -fi` must be run before `./configure` after
> cloning the repository, or after editing `configure.ac` / `Makefile.am`.
> Skipping this step causes build failures that are difficult to diagnose.
> (This was encountered during giza PR #86, where the maintainer hit exactly
> this problem on GitHub Actions CI.)

```bash
git clone https://github.com/goosh-gh/giza-server.git
cd giza-server
autoreconf -fi          # REQUIRED — generates configure, Makefile.in, etc.
./configure
make
sudo make install       # installs giza_server to /usr/local/bin
```

#### Optional: also enable the driver syntax-check (`make check-driver`)

`src/giza-driver-gs.c` is the client-side driver that gets compiled into giza
itself (via the patch).  To syntax-check it you must tell `./configure` where
your giza source tree is — **this must be done before `make`**, not after:

```bash
autoreconf -fi
./configure --with-giza-src=/path/to/giza/src   # ← specify HERE, before make
make
make check-driver
```

If you forget `--with-giza-src` and run plain `./configure`, `make check-driver`
will print an error and exit 1 — you will need to re-run `./configure` with the
option and then `make` again.  There is no way to enable this target after the
fact without reconfiguring.

### Applying the giza /GS driver patch

To add the `/GS` device to giza itself:

```bash
cd /path/to/giza
patch -p1 < /path/to/giza-server/patches/giza-v1.5.0-drivers.patch
autoreconf -fi          # REQUIRED — same reason as above
./configure
make
```

The patch was developed against **giza v1.5.0**.

## Usage

### Start the server

```bash
giza_server &
```

The server listens on a Unix socket at `$XDG_RUNTIME_DIR/giza-server.sock`
(fallback: `/tmp/giza-server-<uid>.sock`).

### Use from your program

Open the `/GS` or `/GIZA_SERVER` device as you would any giza/PGPLOT device:

```fortran
! Fortran / PGPLOT
CALL PGOPEN('/GS')
```

```c
/* C / giza */
giza_open_device("/GS", "My Plot");
```

```perl
# Perl / PDL::Graphics::PGPLOT
my $w = PDL::Graphics::PGPLOT::Window->new(Device => '/GS');
```

The window stays open after your program exits.  Close it by pressing **Q** or
closing the window normally.

### Multiple windows

Each call to open the `/GS` device in `NEWWIN` mode creates a new window.
Up to **64** simultaneous windows are supported.

## Tested platforms

| OS | Architecture | Status |
|---|---|---|
| Ubuntu 24.04 | ARM64 | ✅ Working |

## Relationship to giza PR #86

The macOS Cocoa window driver (`/osx`) for giza was submitted as
[PR #86](https://github.com/danieljprice/giza/pull/86).
`giza-server` is a separate companion project that adds the persistent-window
server daemon.

## License

LGPL-2.1 — the same license as giza itself.

## Author

[goosh-gh](https://github.com/goosh-gh)
