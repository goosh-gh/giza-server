/* giza-driver-gs.c  —  /gs (Giza Server) device driver
 *
 * Client side of the Giza Server Protocol (GSP).
 * Pairs with the giza_server viewer binary (giza-server-gtk.c).
 *
 * Accepted device strings:
 *   /gs        primary name  (Giza Server)
 *   /xs        PGPLOT-compatible alias
 *   /xserve    PGPLOT-compatible alias
 *
 * On macOS the /osx driver provides the native Cocoa viewer; /gs uses
 * the same wire protocol but the viewer binary uses a Cocoa backend.
 * This driver code is identical on all platforms.
 *
 * Copyright (c) 2026 goosh-gh — LGPL-2.1 (same as giza)
 */

#include "giza-private.h"
#include "giza-io-private.h"
#include "giza-drivers-private.h"
#include "giza-driver-gs-private.h"
#include "giza-transforms-private.h"
#include "giza-character-size-private.h"

#ifdef _GIZA_HAS_GS

#include <giza.h>
#include <cairo/cairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#include "giza-server-protocol.h"

/* ------------------------------------------------------------------ */
/* Per-device state                                                    */
/* ------------------------------------------------------------------ */

#define GIZA_GS_DEFAULT_WIDTH  800
#define GIZA_GS_DEFAULT_HEIGHT 600
#define GIZA_GS_UNITS_PER_MM     3.7054
#define GIZA_GS_UNITS_PER_PIXEL  1.0

typedef struct {
    int      sock_fd;
    uint32_t seq;
    int      in_use;
} GsState;

static GsState GsDev[GIZA_MAX_DEVICES];

/* ------------------------------------------------------------------ */
/* PNG-to-memory write callback (ANSI C — no blocks/closures)         */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *buf;
    size_t         len;
    size_t         cap;
} GsBuf;

static cairo_status_t
_gs_png_write_cb(void *closure, const unsigned char *data, unsigned int length)
{
    GsBuf *b = closure;
    if (b->len + length > b->cap) {
        size_t newcap = (b->cap + length) * 2 + 65536;
        unsigned char *tmp = realloc(b->buf, newcap);
        if (!tmp) return CAIRO_STATUS_WRITE_ERROR;
        b->buf = tmp;
        b->cap = newcap;
    }
    memcpy(b->buf + b->len, data, length);
    b->len += length;
    return CAIRO_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Low-level I/O helpers                                               */
/* ------------------------------------------------------------------ */

static int
_gs_write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int
_gs_read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int
_gs_send_msg(int fd, uint8_t type, uint32_t *seq,
             const void *payload, uint32_t plen)
{
    gsp_header_t hdr;
    hdr.magic   = GSP_MAGIC;
    hdr.version = GSP_VERSION;
    hdr.type    = type;
    hdr.flags   = 0;
    hdr.length  = plen;
    hdr.seq     = (*seq)++;
    if (_gs_write_all(fd, &hdr, sizeof(hdr)) < 0) return -1;
    if (plen && _gs_write_all(fd, payload, plen) < 0) return -1;
    return 0;
}

static void
_gs_recv_ack(int fd)
{
    gsp_header_t hdr;
    /* Non-blocking best-effort — we don't stall on it */
    recv(fd, &hdr, sizeof(hdr), MSG_DONTWAIT);
}

/* ------------------------------------------------------------------ */
/* Connect or launch giza_server                                       */
/* ------------------------------------------------------------------ */

static void
_gs_sock_path(char *path, size_t n)
{
    snprintf(path, n,
             GIZA_SERVER_SOCK_DIR "/" GIZA_SERVER_SOCK_NAME,
             (int)getuid());
}

static int
_gs_try_connect(void)
{
    char path[256];
    _gs_sock_path(path, sizeof(path));

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        return fd;
    close(fd);
    return -1;
}

static int
_gs_connect_or_launch(void)
{
    /* 1. Already running? */
    int fd = _gs_try_connect();
    if (fd >= 0) return fd;

    /* 2. Launch giza_server as a background daemon */
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, STDIN_FILENO); dup2(dn, STDOUT_FILENO); close(dn); }
        execlp("giza_server", "giza_server", (char *)NULL);
        execlp("./giza_server", "giza_server", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        _giza_error("_giza_open_device_gs",
                    "fork() failed — cannot start giza_server");
        return -1;
    }
    waitpid(pid, NULL, WNOHANG);   /* adopt by init */

    /* 3. Retry with 50 ms back-off */
    for (int i = 0; i < GSP_CONNECT_RETRIES; i++) {
        struct timespec ts = { 0, 50L * 1000000L };
        nanosleep(&ts, NULL);
        fd = _gs_try_connect();
        if (fd >= 0) return fd;
    }

    _giza_error("_giza_open_device_gs",
                "could not connect to giza_server after launch");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Driver entry points (called from giza-drivers.c)                   */
/* ------------------------------------------------------------------ */

int
_giza_open_device_gs(double width, double height, int units)
{
    static int didInit = 0;
    if (!didInit) { memset(GsDev, 0, sizeof(GsDev)); didInit = 1; }

    if (GsDev[id].in_use) {
        _giza_error("_giza_open_device_gs",
                    "Internal: GsDev[%d] still in use", id);
        return 7;
    }
    memset(&GsDev[id], 0, sizeof(GsState));
    GsDev[id].sock_fd = -1;
    GsDev[id].in_use  = 1;

    Dev[id].deviceUnitsPermm    = GIZA_GS_UNITS_PER_MM;
    Dev[id].deviceUnitsPerPixel = GIZA_GS_UNITS_PER_PIXEL;
    Dev[id].isInteractive       = 0;   /* no cursor/band support yet  */

    if (width > 0. && height > 0. && units > 0) {
        _giza_get_specified_size(width, height, units,
                                 &Dev[id].width, &Dev[id].height);
    } else {
        Dev[id].width  = GIZA_GS_DEFAULT_WIDTH;
        Dev[id].height = GIZA_GS_DEFAULT_HEIGHT;
    }

    /* Cairo image surface — same format as /png                      */
    Dev[id].surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                   Dev[id].width, Dev[id].height);
    if (!Dev[id].surface) {
        _giza_error("_giza_open_device_gs", "Could not create cairo surface");
        GsDev[id].in_use = 0;
        return 5;
    }
    Dev[id].defaultBackgroundAlpha = 1.;

    /* Connect / launch server */
    GsDev[id].sock_fd = _gs_connect_or_launch();
    if (GsDev[id].sock_fd < 0) {
        cairo_surface_destroy(Dev[id].surface);
        GsDev[id].in_use = 0;
        return 1;
    }

    /* Announce window */
    gsp_newwin_t nw;
    nw.width_px  = (uint32_t)Dev[id].width;
    nw.height_px = (uint32_t)Dev[id].height;
    _gs_send_msg(GsDev[id].sock_fd, GSP_MSG_NEWWIN,
                 &GsDev[id].seq, &nw, sizeof(nw));

    /* Send title */
    const char *title = Dev[id].prefix[0] ? Dev[id].prefix : "giza";
    _gs_send_msg(GsDev[id].sock_fd, GSP_MSG_TITLE,
                 &GsDev[id].seq, title, (uint32_t)strlen(title));

    return 0;
}

/* Serialise surface as PNG and push to viewer                        */
void
_giza_flush_device_gs(void)
{
    if (GsDev[id].sock_fd < 0) return;

    cairo_surface_flush(Dev[id].surface);

    GsBuf b = { NULL, 0, 0 };
    cairo_status_t st =
        cairo_surface_write_to_png_stream(Dev[id].surface,
                                          _gs_png_write_cb, &b);
    if (st != CAIRO_STATUS_SUCCESS || !b.buf) {
        free(b.buf);
        _giza_warning("_giza_flush_device_gs", "PNG encode failed");
        return;
    }

    _gs_send_msg(GsDev[id].sock_fd, GSP_MSG_PNG,
                 &GsDev[id].seq, b.buf, (uint32_t)b.len);
    free(b.buf);
    _gs_recv_ack(GsDev[id].sock_fd);
}

/* Ship current page, reset surface for next page                     */
void
_giza_change_page_gs(void)
{
    _giza_flush_device_gs();

    cairo_destroy(Dev[id].context);
    cairo_surface_finish(Dev[id].surface);
    cairo_surface_destroy(Dev[id].surface);

    Dev[id].surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                   Dev[id].width, Dev[id].height);
    Dev[id].context = cairo_create(Dev[id].surface);
}

void
_giza_init_norm_gs(void)
{
    cairo_matrix_init(&Dev[id].Win.normCoords,
                      (double)Dev[id].width,  0,
                      0, (double)-Dev[id].height,
                      0, (double) Dev[id].height);
}

void
_giza_close_device_gs(void)
{
    if (!GsDev[id].in_use) return;

    _giza_flush_device_gs();   /* send final page                     */

    if (GsDev[id].sock_fd >= 0) {
        /* Tell server "window stays, connection closes"              */
        _gs_send_msg(GsDev[id].sock_fd, GSP_MSG_CLOSE,
                     &GsDev[id].seq, NULL, 0);
        _gs_recv_ack(GsDev[id].sock_fd);
        close(GsDev[id].sock_fd);
        GsDev[id].sock_fd = -1;
    }

    if (Dev[id].surface) {
        cairo_surface_finish(Dev[id].surface);
        cairo_surface_destroy(Dev[id].surface);
        Dev[id].surface = NULL;
    }

    memset(&GsDev[id], 0, sizeof(GsState));
}

#endif /* _GIZA_HAS_GS */
