/* giza-server-protocol.h
 *
 * Giza Server Protocol (GSP) - shared between giza_server viewer
 * and the /gs driver inside libgiza.
 *
 * Architecture:
 *   libgiza (/gs device) --- Unix socket --> giza_server (viewer)
 *
 * The giza_server process persists after the client (xspec, PHANTOM, etc.)
 * exits, keeping plot windows alive — the same role pgxwin_server played
 * for PGPLOT/X11, but Cairo-native and cross-platform.
 *
 * Copyright (c) 2026 goosh-gh
 * LGPL-2.1 — same license as giza itself.
 */

#ifndef GIZA_SERVER_PROTOCOL_H
#define GIZA_SERVER_PROTOCOL_H

#include <stdint.h>
#include <stdlib.h>   /* getenv   */
#include <unistd.h>   /* getuid   */
#include <stdio.h>    /* snprintf */
#include <string.h>   /* strncpy  */

/* ------------------------------------------------------------------ */
/* Socket path                                                         */
/* ------------------------------------------------------------------ */

/* Per-user socket: only one giza_server per user.                    */
/* Multiple plot windows are managed as tabs inside the viewer.        */
#define GIZA_SERVER_SOCK_DIR  "/tmp"
#define GIZA_SERVER_SOCK_NAME "giza_server_%d.sock"   /* %d = uid     */
/* Full path constructed at runtime: /tmp/giza_server_<uid>.sock      */

/* Resolve the giza_server socket path into buf (size n).
 * Precedence:
 *   1. $GIZA_SERVER_SOCK  - explicit path: isolated/parallel servers and
 *                           the test harness (each test owns its socket).
 *   2. /tmp/giza_server_<uid>.sock  - default, one server per user.
 * Always NUL-terminated. Shared by the viewer, the test clients and the
 * examples so every component agrees on the path. The Perl Driver::GS
 * applies the same precedence in _sock_path(). */
static inline void
gsp_resolve_sock_path(char *buf, size_t n)
{
    const char *env = getenv("GIZA_SERVER_SOCK");
    if (env && env[0]) {
        strncpy(buf, env, n - 1);
        buf[n - 1] = '\0';
    } else {
        snprintf(buf, n,
                 GIZA_SERVER_SOCK_DIR "/" GIZA_SERVER_SOCK_NAME,
                 (int)getuid());
    }
}

/* ------------------------------------------------------------------ */
/* Protocol magic & version                                            */
/* ------------------------------------------------------------------ */

#define GSP_MAGIC    0x47495A41u   /* "GIZA" in ASCII                 */
#define GSP_VERSION  1

/* ------------------------------------------------------------------ */
/* Message types                                                       */
/* ------------------------------------------------------------------ */

#define GSP_MSG_PING    0x01u  /* client → server: are you alive?      */
#define GSP_MSG_PONG    0x02u  /* server → client: yes                 */
#define GSP_MSG_PNG     0x10u  /* client → server: here is a PNG page  */
#define GSP_MSG_TITLE   0x11u  /* client → server: window/tab title    */
#define GSP_MSG_NEWWIN  0x12u  /* client → server: open a new window   */
#define GSP_MSG_SLIDER  0x13u  /* server → client: slider value (float)*/
#define GSP_MSG_SAVEREQ 0x14u  /* server → client: render & return vector (PDF/SVG) */
#define GSP_MSG_SAVEDATA 0x15u /* client → server: vector bytes for a pending save   */
#define GSP_MSG_CLOSE   0x20u  /* client → server: close this window   */
#define GSP_MSG_ACK     0x21u  /* server → client: acknowledged        */
#define GSP_MSG_ERR     0xFFu  /* server → client: error (+ message)   */

/* GSP_MSG_SLIDER payload:
 *   uint8_t  slider_id   (which slider: 0=freq k, 1=amplitude A, ...)
 *   float    value       (4 bytes, little-endian) — current slider value
 *   server → client, fire-and-forget (no ACK).
 *   Total payload: 5 bytes.
 */

/* Vector-save format codes (GSP_MSG_SAVEREQ / GSP_MSG_SAVEDATA payloads). */
#define GSP_SAVE_FMT_PDF  0u
#define GSP_SAVE_FMT_SVG  1u

/* GSP_MSG_SAVEREQ payload:
 *   uint8_t  fmt   (GSP_SAVE_FMT_PDF or GSP_SAVE_FMT_SVG)
 *   server → client, fire-and-forget (no ACK).  Sent ONLY when the user
 *   picks File > Save as PDF/SVG and the client is still alive.  The
 *   client re-renders its current figure to that vector format and
 *   replies with GSP_MSG_SAVEDATA.  Total payload: 1 byte.
 *
 * GSP_MSG_SAVEDATA payload:
 *   uint8_t  fmt   (echoes the requested format, for a sanity check)
 *   <vector bytes...>   (the rendered PDF or SVG document)
 *   client → server, fire-and-forget (no ACK).  The server writes the
 *   bytes to the path the user chose in the save panel.  The server
 *   remembers (path, fmt) per window between SAVEREQ and SAVEDATA, so
 *   the client never needs to know the destination path.
 */

/* ------------------------------------------------------------------ */
/* Wire format                                                         */
/* ------------------------------------------------------------------ */

/*
 * Every message on the socket starts with this 16-byte fixed header,
 * followed immediately by `length` bytes of payload.
 *
 * All fields are little-endian (host order on x86/arm64; swap on BE).
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;    /* must equal GSP_MAGIC                        */
    uint8_t  version;  /* GSP_VERSION                                 */
    uint8_t  type;     /* GSP_MSG_* constant                          */
    uint16_t flags;    /* reserved, set to 0                          */
    uint32_t length;   /* payload length in bytes (0 = no payload)    */
    uint32_t seq;      /* sequence number (monotonically increasing)  */
} gsp_header_t;        /* 16 bytes total                              */

/* GSP_MSG_PNG payload:
 *   Raw PNG bytes.  The viewer decodes via cairo_image_surface_create_from_png()
 *   using a read callback — no temp files needed.
 *
 * GSP_MSG_TITLE payload:
 *   UTF-8 string, NOT NUL-terminated (length tells you how many bytes).
 *
 * GSP_MSG_NEWWIN payload:
 *   uint32_t  width_px    (suggested initial window width,  0 = default)
 *   uint32_t  height_px   (suggested initial window height, 0 = default)
 *
 * GSP_MSG_CLOSE / GSP_MSG_ACK / GSP_MSG_PING / GSP_MSG_PONG:
 *   No payload (length = 0).
 *
 * GSP_MSG_ERR payload:
 *   UTF-8 human-readable error string (not NUL-terminated).
 */

/* ------------------------------------------------------------------ */
/* Helper: NEWWIN payload struct                                       */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint32_t width_px;
    uint32_t height_px;
} gsp_newwin_t;

typedef struct __attribute__((packed)) {
    uint8_t  slider_id;
    float    value;
} gsp_slider_t;   /* 5 bytes */

/* ------------------------------------------------------------------ */
/* Timeouts & limits                                                   */
/* ------------------------------------------------------------------ */

#define GSP_CONNECT_TIMEOUT_MS   500   /* wait for server to start     */
#define GSP_CONNECT_RETRIES      10    /* × 50 ms = 500 ms max wait    */
#define GSP_MAX_PNG_BYTES        (64 * 1024 * 1024)  /* 64 MiB safety  */

#endif /* GIZA_SERVER_PROTOCOL_H */
