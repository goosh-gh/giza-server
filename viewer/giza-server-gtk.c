/* giza-server-gtk.c
 *
 * giza_server — persistent plot window server for giza/PGPLOT
 *
 * The server persists after the calling program exits, keeping plot
 * windows alive — the same role pgxwin_server played for PGPLOT/X11.
 *
 * Key differences from pgxwin_server:
 *   • Cairo-native  — same surface as giza's PDF/PNG → pub-quality output
 *   • GTK3-based    — native look on Linux (X11 + Wayland via XWayland)
 *   • Unix socket   — no shared memory, no X atoms, WSL2-friendly
 *   • macOS-ready   — swap backend for Cocoa; protocol is identical
 *
 * Build (Ubuntu 24.04 / Debian):
 *   gcc -o giza_server giza-server-gtk.c \
 *       $(pkg-config --cflags --libs gtk+-3.0 cairo gio-unix-2.0) \
 *       -lpthread -Wall -O2
 *
 * Copyright (c) 2026 goosh-gh — LGPL-2.1 (same as giza)
 */

#include <gtk/gtk.h>
#include <cairo.h>
#include <gio/gunixsocketaddress.h>
#include <gio/gio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>

#include "giza-server-protocol.h"

/* ------------------------------------------------------------------ */
/* Per-window state                                                    */
/* ------------------------------------------------------------------ */

#define MAX_WINDOWS 64

typedef struct {
    int              id;
    GtkWidget       *window;
    GtkWidget       *drawing_area;
    cairo_surface_t *surface;      /* current decoded Cairo surface   */
    GMutex           lock;
    char             title[256];
    int              width, height;
    gboolean         alive;
} GsWindow;

/* ------------------------------------------------------------------ */
/* Global server state                                                 */
/* ------------------------------------------------------------------ */

static struct {
    GsWindow   wins[MAX_WINDOWS];
    GMutex     wins_lock;
    char       sock_path[256];
    GSocketService *service;
    int        n_wins;
} GS;

/* ------------------------------------------------------------------ */
/* Cairo PNG-from-memory helper                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const unsigned char *data;
    size_t len;
    size_t pos;
} PngReadState;

static cairo_status_t
_png_read_fn(void *closure, unsigned char *buf, unsigned int count)
{
    PngReadState *s = closure;
    if (s->pos + count > s->len) return CAIRO_STATUS_READ_ERROR;
    memcpy(buf, s->data + s->pos, count);
    s->pos += count;
    return CAIRO_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* GTK3 drawing (expose-event / draw signal)                           */
/* ------------------------------------------------------------------ */

static gboolean
on_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    GsWindow *w = user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    int width  = alloc.width;
    int height = alloc.height;

    g_mutex_lock(&w->lock);

    if (!w->surface) {
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        g_mutex_unlock(&w->lock);
        return FALSE;
    }

    int sw = cairo_image_surface_get_width(w->surface);
    int sh = cairo_image_surface_get_height(w->surface);

    /* Letterbox: maintain aspect ratio, white background */
    double sx    = (double)width  / sw;
    double sy    = (double)height / sh;
    double scale = (sx < sy) ? sx : sy;
    double ox    = (width  - sw * scale) / 2.0;
    double oy    = (height - sh * scale) / 2.0;

    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);

    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, w->surface, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    g_mutex_unlock(&w->lock);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Idle callbacks (must execute on GTK main thread)                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int    win_id;
    char  *png_data;
    size_t png_len;
    char   title[256];
} UpdateMsg;

static gboolean
_update_window_idle(gpointer user_data)
{
    UpdateMsg *msg = user_data;
    GsWindow  *w   = &GS.wins[msg->win_id];

    PngReadState rs = { (unsigned char *)msg->png_data, msg->png_len, 0 };
    cairo_surface_t *surf =
        cairo_image_surface_create_from_png_stream(_png_read_fn, &rs);

    if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
        g_mutex_lock(&w->lock);
        if (w->surface) cairo_surface_destroy(w->surface);
        w->surface = surf;
        g_mutex_unlock(&w->lock);
        if (msg->title[0])
            gtk_window_set_title(GTK_WINDOW(w->window), msg->title);
        gtk_widget_queue_draw(w->drawing_area);
    } else {
        cairo_surface_destroy(surf);
        fprintf(stderr, "giza_server: PNG decode failed\n");
    }

    free(msg->png_data);
    free(msg);
    return G_SOURCE_REMOVE;
}

/* Passed through g_idle_add to create a window on the main thread   */
typedef struct {
    int      suggested_w, suggested_h;
    char     title[256];
    int     *out_id;
    GMutex  *mutex;
    GCond   *cond;
    gboolean done;
} NewWinMsg;

static gboolean
_new_window_idle(gpointer user_data)
{
    NewWinMsg *m = user_data;

    g_mutex_lock(&GS.wins_lock);
    if (GS.n_wins >= MAX_WINDOWS) {
        g_mutex_unlock(&GS.wins_lock);
        g_mutex_lock(m->mutex);
        *m->out_id = -1;
        m->done = TRUE;
        g_cond_signal(m->cond);
        g_mutex_unlock(m->mutex);
        return G_SOURCE_REMOVE;
    }
    int idx = GS.n_wins++;
    g_mutex_unlock(&GS.wins_lock);

    GsWindow *w = &GS.wins[idx];
    memset(w, 0, sizeof(*w));
    w->id    = idx;
    w->alive = TRUE;
    g_mutex_init(&w->lock);
    snprintf(w->title, sizeof(w->title), "%s", m->title[0] ? m->title : "giza");

    int iw = m->suggested_w > 0 ? m->suggested_w : 800;
    int ih = m->suggested_h > 0 ? m->suggested_h : 600;

    /* GTK3 API */
    w->window       = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    w->drawing_area = gtk_drawing_area_new();

    gtk_window_set_title(GTK_WINDOW(w->window), w->title);
    gtk_window_set_default_size(GTK_WINDOW(w->window), iw, ih);
    gtk_window_set_resizable(GTK_WINDOW(w->window), TRUE);

    gtk_container_add(GTK_CONTAINER(w->window), w->drawing_area);

    g_signal_connect(w->drawing_area, "draw", G_CALLBACK(on_draw), w);

    gtk_widget_show_all(w->window);

    g_mutex_lock(m->mutex);
    *m->out_id = idx;
    m->done    = TRUE;
    g_cond_signal(m->cond);
    g_mutex_unlock(m->mutex);

    return G_SOURCE_REMOVE;
}

/* Synchronously create a window from a non-main thread               */
static int
_create_window_sync(int w_px, int h_px, const char *title)
{
    GMutex  m; GCond c; int out_id = -1;
    g_mutex_init(&m); g_cond_init(&c);

    NewWinMsg *nm = calloc(1, sizeof(*nm));
    nm->suggested_w = w_px;
    nm->suggested_h = h_px;
    strncpy(nm->title, title ? title : "giza", sizeof(nm->title)-1);
    nm->out_id = &out_id;
    nm->mutex  = &m;
    nm->cond   = &c;
    nm->done   = FALSE;

    g_idle_add(_new_window_idle, nm);

    g_mutex_lock(&m);
    while (!nm->done) g_cond_wait(&c, &m);
    g_mutex_unlock(&m);

    free(nm);
    g_mutex_clear(&m);
    g_cond_clear(&c);
    return out_id;
}

/* ------------------------------------------------------------------ */
/* Low-level I/O                                                       */
/* ------------------------------------------------------------------ */

static int
_read_exactly(GInputStream *in, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        GError *err = NULL;
        gssize r = g_input_stream_read(in, (char*)buf + done,
                                       n - done, NULL, &err);
        if (r <= 0) { if (err) g_error_free(err); return -1; }
        done += (size_t)r;
    }
    return 0;
}

static int
_write_exactly(GOutputStream *out, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        GError *err = NULL;
        gssize r = g_output_stream_write(out, (const char*)buf + done,
                                         n - done, NULL, &err);
        if (r <= 0) { if (err) g_error_free(err); return -1; }
        done += (size_t)r;
    }
    return 0;
}

static int
_send_msg(GOutputStream *out, uint8_t type, uint32_t seq,
          const void *payload, uint32_t plen)
{
    gsp_header_t hdr;
    hdr.magic   = GSP_MAGIC;
    hdr.version = GSP_VERSION;
    hdr.type    = type;
    hdr.flags   = 0;
    hdr.length  = plen;
    hdr.seq     = seq;
    if (_write_exactly(out, &hdr, sizeof(hdr)) < 0) return -1;
    if (plen && _write_exactly(out, payload, plen) < 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-connection thread                                               */
/* ------------------------------------------------------------------ */

typedef struct { GSocketConnection *conn; } ConnArg;

static gpointer
_handle_connection(gpointer user_data)
{
    ConnArg *arg = user_data;
    GSocketConnection *conn = arg->conn;
    free(arg);

    GInputStream  *in  = g_io_stream_get_input_stream (G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));

    int      current_win = -1;
    uint32_t seq_out     = 0;
    char     title_buf[256] = {0};

    for (;;) {
        gsp_header_t hdr;
        if (_read_exactly(in, &hdr, sizeof(hdr)) < 0) break;

        if (hdr.magic != GSP_MAGIC) {
            fprintf(stderr, "giza_server: bad magic %08X\n", hdr.magic);
            break;
        }
        if (hdr.length > GSP_MAX_PNG_BYTES) {
            fprintf(stderr, "giza_server: payload too large\n");
            break;
        }

        char *payload = NULL;
        if (hdr.length > 0) {
            payload = malloc(hdr.length);
            if (!payload) break;
            if (_read_exactly(in, payload, hdr.length) < 0) {
                free(payload); break;
            }
        }

        switch (hdr.type) {

        case GSP_MSG_PING:
            _send_msg(out, GSP_MSG_PONG, seq_out++, NULL, 0);
            break;

        case GSP_MSG_NEWWIN: {
            gsp_newwin_t nw = {0, 0};
            if (payload && hdr.length >= sizeof(nw))
                memcpy(&nw, payload, sizeof(nw));
            const char *ttl = title_buf[0] ? title_buf : "giza";
            current_win = _create_window_sync(nw.width_px, nw.height_px, ttl);
            _send_msg(out, GSP_MSG_ACK, seq_out++, NULL, 0);
            break;
        }

        case GSP_MSG_TITLE:
            if (payload) {
                size_t len = hdr.length < 255 ? hdr.length : 255;
                memcpy(title_buf, payload, len);
                title_buf[len] = '\0';
                if (current_win >= 0)
                    gtk_window_set_title(
                        GTK_WINDOW(GS.wins[current_win].window), title_buf);
            }
            break;

        case GSP_MSG_PNG: {
            if (current_win < 0) {
                /* Auto-open if client skipped NEWWIN */
                current_win = _create_window_sync(800, 600, "giza");
            }
            if (current_win < 0) { free(payload); payload = NULL; break; }

            UpdateMsg *um = calloc(1, sizeof(*um));
            um->win_id   = current_win;
            um->png_data = payload;
            um->png_len  = hdr.length;
            snprintf(um->title, sizeof(um->title), "%s", title_buf);
            payload = NULL;   /* ownership transferred to idle cb     */

            g_idle_add(_update_window_idle, um);
            _send_msg(out, GSP_MSG_ACK, seq_out++, NULL, 0);
            break;
        }

        case GSP_MSG_CLOSE:
            _send_msg(out, GSP_MSG_ACK, seq_out++, NULL, 0);
            free(payload);
            goto disconnect;

        default:
            fprintf(stderr, "giza_server: unknown msg type 0x%02X\n",
                    hdr.type);
            break;
        }

        free(payload);
    }

disconnect:
    g_object_unref(conn);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* GSocketService incoming callback                                    */
/* ------------------------------------------------------------------ */

static gboolean
on_incoming(GSocketService    *service __attribute__((unused)),
            GSocketConnection *conn,
            GObject           *source_object __attribute__((unused)),
            gpointer           user_data __attribute__((unused)))
{
    g_object_ref(conn);
    ConnArg *arg = malloc(sizeof(*arg));
    arg->conn = conn;
    GThread *t = g_thread_new("giza-conn", _handle_connection, arg);
    g_thread_unref(t);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    memset(&GS, 0, sizeof(GS));
    g_mutex_init(&GS.wins_lock);

    gtk_init(&argc, &argv);

    /* Build socket path */
    gsp_resolve_sock_path(GS.sock_path, sizeof(GS.sock_path));
    unlink(GS.sock_path);   /* remove stale socket if any */

    /* Create Unix socket service */
    GError *err = NULL;
    GS.service = g_socket_service_new();
    GSocketAddress *addr = g_unix_socket_address_new(GS.sock_path);

    if (!g_socket_listener_add_address(
            G_SOCKET_LISTENER(GS.service), addr,
            G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
            NULL, NULL, &err)) {
        fprintf(stderr, "giza_server: cannot bind %s: %s\n",
                GS.sock_path, err->message);
        return 1;
    }
    g_object_unref(addr);

    g_signal_connect(GS.service, "incoming", G_CALLBACK(on_incoming), NULL);
    g_socket_service_start(GS.service);

    fprintf(stdout, "giza_server: listening on %s\n", GS.sock_path);
    fflush(stdout);

    gtk_main();
    return 0;
}
