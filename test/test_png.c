/* test/test_png.c
 *
 * PNG window display test for giza_server.
 *
 * Creates a 400×300 PNG in memory using Cairo (red background, white
 * diagonal cross), sends it via the wire protocol, waits for ACK.
 * The window should appear on screen — visual confirmation required.
 *
 * Exit 0  = protocol exchange OK (window appeared)
 * Exit 1  = error
 * Exit 77 = skipped (no display)
 *
 * Build via:  make check
 * Or manually:
 *   gcc -o test/test_png test/test_png.c -Iviewer \
 *       $(pkg-config --cflags --libs cairo) && ./test/test_png
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cairo.h>

#include "giza-server-protocol.h"

/* ------------------------------------------------------------------ */
/* PNG generation via Cairo (in-memory, no temp files)                */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char *data;
    size_t         len;
    size_t         cap;
} MemBuf;

static cairo_status_t
_png_write_fn(void *closure, const unsigned char *data, unsigned int length)
{
    MemBuf *b = closure;
    if (b->len + length > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 65536;
        while (newcap < b->len + length) newcap *= 2;
        unsigned char *p = realloc(b->data, newcap);
        if (!p) return CAIRO_STATUS_WRITE_ERROR;
        b->data = p;
        b->cap  = newcap;
    }
    memcpy(b->data + b->len, data, length);
    b->len += length;
    return CAIRO_STATUS_SUCCESS;
}

static int
make_test_png(MemBuf *out)
{
    memset(out, 0, sizeof(*out));

    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_RGB24, 400, 300);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "FAIL: cairo_image_surface_create\n");
        return -1;
    }

    cairo_t *cr = cairo_create(surf);

    /* Red background */
    cairo_set_source_rgb(cr, 0.8, 0.1, 0.1);
    cairo_paint(cr);

    /* White diagonal cross */
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width(cr, 6.0);
    cairo_move_to(cr,   0,   0); cairo_line_to(cr, 400, 300);
    cairo_move_to(cr, 400,   0); cairo_line_to(cr,   0, 300);
    cairo_stroke(cr);

    /* Label */
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_select_font_face(cr, "Sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 22.0);
    cairo_move_to(cr, 80, 160);
    cairo_show_text(cr, "giza-server test_png OK");

    cairo_destroy(cr);

    cairo_status_t st =
        cairo_surface_write_to_png_stream(surf, _png_write_fn, out);
    cairo_surface_destroy(surf);

    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "FAIL: PNG encode: %s\n", cairo_status_to_string(st));
        free(out->data);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Socket helpers                                                      */
/* ------------------------------------------------------------------ */

static char sock_path[256];

static void
make_sock_path(void)
{
    gsp_resolve_sock_path(sock_path, sizeof(sock_path));
}

static int
connect_to_server(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

static int
send_msg(int fd, uint8_t type, const void *payload, uint32_t len, uint32_t seq)
{
    gsp_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic   = GSP_MAGIC;
    h.version = GSP_VERSION;
    h.type    = type;
    h.flags   = 0;
    h.length  = len;
    h.seq     = seq;

    if (write(fd, &h, sizeof(h)) != (ssize_t)sizeof(h)) {
        perror("write header"); return -1;
    }
    if (len > 0 && payload) {
        if (write(fd, payload, len) != (ssize_t)len) {
            perror("write payload"); return -1;
        }
    }
    return 0;
}

static int
recv_ack(int fd, const char *context)
{
    gsp_header_t h;
    memset(&h, 0, sizeof(h));
    ssize_t n = read(fd, &h, sizeof(h));
    if (n != (ssize_t)sizeof(h)) {
        fprintf(stderr, "FAIL [%s]: read %zd of %zu bytes\n",
                context, n, sizeof(h));
        return -1;
    }
    if (h.magic != GSP_MAGIC) {
        fprintf(stderr, "FAIL [%s]: bad magic 0x%08X\n", context, h.magic);
        return -1;
    }
    if (h.type != GSP_MSG_ACK) {
        fprintf(stderr, "FAIL [%s]: expected ACK (0x%02X), got 0x%02X\n",
                context, GSP_MSG_ACK, h.type);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int
main(void)
{
    /* Skip if no display */
    if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
        fprintf(stderr, "SKIP: no display (DISPLAY/WAYLAND_DISPLAY unset)\n");
        return 77;   /* automake SKIP */
    }

    make_sock_path();
    printf("test_png: connecting to %s\n", sock_path);

    int fd = connect_to_server();
    if (fd < 0) {
        fprintf(stderr, "FAIL: cannot connect — is giza_server running?\n");
        return 1;
    }

    /* 1. Open new window */
    gsp_newwin_t nw = { 400, 300 };
    if (send_msg(fd, GSP_MSG_NEWWIN, &nw, sizeof(nw), 1) < 0)
        { close(fd); return 1; }
    if (recv_ack(fd, "NEWWIN") < 0) { close(fd); return 1; }

    /* 2. Set title (server does not ACK TITLE — fire and forget) */
    const char *title = "giza-server test_png";
    if (send_msg(fd, GSP_MSG_TITLE, title, (uint32_t)strlen(title), 2) < 0)
        { close(fd); return 1; }

    /* 3. Generate PNG in memory */
    MemBuf png;
    if (make_test_png(&png) < 0) { close(fd); return 1; }
    printf("test_png: generated %zu-byte PNG\n", png.len);

    /* 4. Send PNG frame */
    if (send_msg(fd, GSP_MSG_PNG, png.data, (uint32_t)png.len, 3) < 0)
        { free(png.data); close(fd); return 1; }
    if (recv_ack(fd, "PNG") < 0)
        { free(png.data); close(fd); return 1; }

    free(png.data);
    close(fd);

    printf("PASS: PNG accepted (ACK received)\n");
    printf("      Window \"%s\" should be visible on screen.\n", title);
    return 0;
}
