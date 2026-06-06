/* giza-server-xlib.c
 *
 * giza_server — persistent plot window server for giza/PGPLOT
 * Xlib backend: depends only on libX11, cairo, cairo-xlib, libpng, libpthread.
 * No GTK / GLib / GIO required.
 *
 * Architecture mirrors giza-server-gtk.c:
 *   main thread      — X event loop (XNextEvent / select hybrid)
 *   accept thread    — listens on Unix socket, spawns per-connection threads
 *   connection thread— reads GSP messages, dispatches window ops via Xlib
 *
 * Build (Ubuntu 24.04 / Debian):
 *   gcc -o giza_server giza-server-xlib.c \
 *       $(pkg-config --cflags --libs x11 cairo cairo-xlib) \
 *       -lpng -lpthread -Wall -Wextra -O2
 *
 * Copyright (c) 2026 goosh-gh — LGPL-2.1 (same as giza)
 */

/* ------------------------------------------------------------------ */
/* Feature-test macros (required before any system header)             */
/* ------------------------------------------------------------------ */
#define _GNU_SOURCE          /* for accept4, pipe2                    */
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <cairo.h>
#include <cairo-xlib.h>

#include <png.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>

#include "giza-server-protocol.h"

/* ------------------------------------------------------------------ */
/* Compile-time knobs                                                  */
/* ------------------------------------------------------------------ */

#define DEFAULT_WIN_W   800
#define DEFAULT_WIN_H   600
#define MAX_WINDOWS     64          /* total figures (tabs) across all containers */
#define MAX_CONTAINERS  MAX_WINDOWS /* worst case: every figure standalone        */
#define MAX_TITLE_LEN   255

/* Tab bar (drawn only when a container holds >= 2 figures). Xlib has no
 * native window tabbing, so we paint our own strip at the top of the
 * container window and route clicks: a hit on a tab body selects it, a hit
 * on the tab's small "x" box closes that figure. Mirrors the Cocoa backend,
 * which gets the same grouping for free via NSWindowTabbingIdentifier. */
#define TABBAR_H        26          /* tab strip height (px)                  */
#define TAB_CLOSE_BOX   16          /* clickable close-x region width (px)    */

/* Interactive sliders (viewer-side fixed controls, mirroring the Cocoa
 * backend): a horizontal strip at the bottom drives slider id 0 (freq k)
 * and a vertical strip at the right drives id 1 (amplitude A). Moving one
 * sends GSP_MSG_SLIDER(id, value) back to the window's client. */
#define SLIDER_H        28          /* bottom strip height (horizontal, id 0) */
#define SLIDER_W        28          /* right  strip width  (vertical,   id 1) */
#define SLD_K_MIN       0.5         /* id 0 (k) range — matches Cocoa */
#define SLD_K_MAX       8.0
#define SLD_A_MIN       0.1         /* id 1 (A) range — matches Cocoa */
#define SLD_A_MAX       1.0
#define TWO_PI          6.283185307179586

/* ------------------------------------------------------------------ */
/* Thread-safe wakeup pipe: connection threads poke the X event loop  */
/* ------------------------------------------------------------------ */

static int g_wake_rd = -1, g_wake_wr = -1;   /* pipe fds            */

/* ------------------------------------------------------------------ */
/* Commands sent from connection threads to the main (X) thread        */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_NEWWIN,    /* open a new window                               */
    CMD_PNG,       /* update window with a new PNG surface            */
    CMD_TITLE,     /* set window title                                */
    CMD_CLOSE,     /* close a window                                  */
    CMD_SAVEPATH,  /* write vector bytes (SAVEDATA) to disk           */
} CmdType;

typedef struct {
    CmdType          type;
    int              win_id;
    int              fd;                           /* CMD_NEWWIN: client socket */
    uint32_t         group_id;                    /* CMD_NEWWIN: client PID (tab group); 0 = standalone */
    int              width, height;               /* CMD_NEWWIN        */
    cairo_surface_t *surface;                     /* CMD_PNG (ref handed off) */
    char             title[MAX_TITLE_LEN + 1];    /* CMD_TITLE         */
    uint8_t          save_fmt;                    /* CMD_SAVEPATH      */
    char             save_path[512];              /* CMD_SAVEPATH      */
    unsigned char   *save_bytes;                  /* CMD_SAVEPATH      */
    size_t           save_len;                    /* CMD_SAVEPATH      */
    int             *result;                      /* optional reply slot */
    pthread_mutex_t *result_mu;
    pthread_cond_t  *result_cv;
} Cmd;

/* Lock-free-ish bounded queue using a pipe + heap-allocated Cmd*.    */
/* We just malloc/free — simplest correct approach.                   */

static void _send_cmd(Cmd *cmd)
{
    Cmd **p = malloc(sizeof(Cmd *));
    if (!p) { free(cmd); return; }
    *p = cmd;
    if (write(g_wake_wr, p, sizeof(Cmd *)) != (ssize_t)sizeof(Cmd *)) {
        free(p);
        free(cmd);
    }
}

/* ------------------------------------------------------------------ */
/* Per-window state (accessed only from main thread except where noted)*/
/* ------------------------------------------------------------------ */

/* A GsWindow is one figure = one tab. It no longer owns an X window; the X
 * window belongs to the GsContainer it lives in (win->container). Several
 * tabs sharing a container = a tabbed window. */
typedef struct {
    int              id;
    int              container;  /* index into GS.conts[]; -1 if unattached  */
    cairo_surface_t *surface;   /* current Cairo surface (IMAGE type)  */
    char             title[MAX_TITLE_LEN + 1];
    int              alive;     /* 0 once closed                        */

    /* Server8: reverse-channel vector save.
     * Written from connection thread (under write_lock), read from main. */
    pthread_mutex_t  write_lock;
    int              client_fd;    /* -1 if client has exited           */
    char            *save_path;    /* pending save path (NULL if none)  */
    uint8_t          save_fmt;

    /* Pipe: main thread → connection thread (for SAVEREQ)            */
    int              savereq_wr;   /* write end; -1 if unused           */

    /* Interactive slider values (main-thread only). [0]=k, [1]=A.      */
    double           sld_val[2];
} GsWindow;

/* A GsContainer owns one X top-level window and groups 1..N tabs (figures)
 * that share a client PID. Touched only from the main (X) thread. */
typedef struct {
    int        alive;
    Window     xwin;
    GC         gc;
    uint32_t   group_id;    /* client PID; 0 = standalone (never shares)  */
    int        width, height;
    int        active;      /* GS.wins[] slot index of the visible tab; -1 */
} GsContainer;

/* ------------------------------------------------------------------ */
/* Global server state                                                 */
/* ------------------------------------------------------------------ */

static struct {
    Display    *dpy;
    int         screen;
    Atom        wm_delete;     /* WM_DELETE_WINDOW atom                */
    GsWindow    wins[MAX_WINDOWS];
    GsContainer conts[MAX_CONTAINERS];
    pthread_mutex_t wins_lock;
    char        sock_path[256];
    int         n_wins;
    int         listen_fd;
} GS;

/* Forward declarations (definitions appear below in dependency-free order). */
static void _repaint_container(GsContainer *c);
static void _set_container_title(GsContainer *c);
static int  _container_tabs(int ci, int *out, int max);
static void _close_tab(int id, int user_initiated);

/* ------------------------------------------------------------------ */
/* Cairo PNG-from-memory                                               */
/* ------------------------------------------------------------------ */

typedef struct { const unsigned char *data; size_t len; size_t pos; } PngRead;

static cairo_status_t
_png_read_fn(void *cl, unsigned char *buf, unsigned int n)
{
    PngRead *s = cl;
    if (s->pos + n > s->len) return CAIRO_STATUS_READ_ERROR;
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t *
_decode_png(const unsigned char *data, size_t len)
{
    PngRead s = { data, len, 0 };
    return cairo_image_surface_create_from_png_stream(_png_read_fn, &s);
}

/* ------------------------------------------------------------------ */
/* Socket I/O helpers                                                  */
/* ------------------------------------------------------------------ */

static int
_read_exact(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int
_write_exact(int fd, const void *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = write(fd, (const char *)buf + sent, n - sent);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return 0;
}

static int
_send_hdr(int fd, uint8_t type, uint32_t len, uint32_t seq)
{
    gsp_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic   = GSP_MAGIC;
    h.version = GSP_VERSION;
    h.type    = type;
    h.length  = len;
    h.seq     = seq;
    return _write_exact(fd, &h, sizeof(h));
}

/* Same as _send_hdr, but serialized by the target window's write_lock so a
 * server→client frame from the connection thread (e.g. PNG ACK) is never
 * interleaved with a SLIDER frame sent from the main X thread on the same
 * fd. Falls back to an unlocked send when the window does not exist yet
 * (NEWWIN ACK / PONG happen before/at window creation, with no concurrent
 * slider traffic). */
static int
_send_hdr_locked(int wid, int fd, uint8_t type, uint32_t len, uint32_t seq)
{
    GsWindow *win = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (GS.wins[i].alive && GS.wins[i].id == wid) { win = &GS.wins[i]; break; }

    if (!win) return _send_hdr(fd, type, len, seq);

    pthread_mutex_lock(&win->write_lock);
    int r = _send_hdr(fd, type, len, seq);
    pthread_mutex_unlock(&win->write_lock);
    return r;
}

/* ------------------------------------------------------------------ */
/* Window management (main-thread only)                                */
/* ------------------------------------------------------------------ */

static GsWindow *_find_or_create_win(int id)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (GS.wins[i].alive && GS.wins[i].id == id)
            return &GS.wins[i];
    return NULL;
}

static double _clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Collect the GS.wins[] slot indices of the live tabs in container ci, in
 * slot (≈creation) order. Returns the count. Main-thread only. */
static int _container_tabs(int ci, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS && n < max; i++)
        if (GS.wins[i].alive && GS.wins[i].container == ci)
            out[n++] = i;
    return n;
}

/* Find the live container holding group gid (a client PID). gid 0 never
 * shares a window, so it always reports "no container" → a fresh one. */
static int _find_container_by_group(uint32_t gid)
{
    if (gid == 0) return -1;
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (GS.conts[i].alive && GS.conts[i].group_id == gid)
            return i;
    return -1;
}

/* Create a new X top-level window to host a tab group. Returns container
 * index or -1. */
static int _create_container(uint32_t gid, int w, int h)
{
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (GS.conts[i].alive) continue;
        GsContainer *c = &GS.conts[i];
        memset(c, 0, sizeof(*c));
        c->alive    = 1;
        c->group_id = gid;
        c->width    = w ? w : DEFAULT_WIN_W;
        c->height   = h ? h : DEFAULT_WIN_H;
        c->active   = -1;

        c->xwin = XCreateSimpleWindow(
            GS.dpy, RootWindow(GS.dpy, GS.screen),
            0, 0, (unsigned)c->width, (unsigned)c->height, 1,
            BlackPixel(GS.dpy, GS.screen),
            WhitePixel(GS.dpy, GS.screen));
        XSetWMProtocols(GS.dpy, c->xwin, &GS.wm_delete, 1);
        c->gc = XCreateGC(GS.dpy, c->xwin, 0, NULL);
        XSelectInput(GS.dpy, c->xwin,
                     ExposureMask | StructureNotifyMask |
                     ButtonPressMask | Button1MotionMask);
        XMapRaised(GS.dpy, c->xwin);
        return i;
    }
    fprintf(stderr, "giza_server: too many containers (max %d)\n", MAX_CONTAINERS);
    return -1;
}

/* The WM title tracks the active tab's title. */
static void _set_container_title(GsContainer *c)
{
    int ci = (int)(c - GS.conts);
    const char *t = "giza";
    if (c->active >= 0 && GS.wins[c->active].alive &&
        GS.wins[c->active].container == ci)
        t = GS.wins[c->active].title;
    XStoreName(GS.dpy, c->xwin, t);
}

/* Open a new figure (tab). With a client PID (group_id != 0) the figure
 * joins that client's existing container as a new tab; otherwise (or if the
 * client has none yet) a fresh container window is created. The new tab
 * becomes active. */
static void
_open_window(int id, int client_fd, int w, int h, const char *title,
             uint32_t group_id)
{
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!GS.wins[i].alive) { slot = i; break; }
    if (slot < 0) {
        fprintf(stderr, "giza_server: too many windows (max %d)\n", MAX_WINDOWS);
        return;
    }

    int newc = 0;
    int ci = _find_container_by_group(group_id);
    if (ci < 0) {
        ci = _create_container(group_id, w, h);
        newc = 1;
        if (ci < 0) return;
    }

    GsWindow *win = &GS.wins[slot];
    memset(win, 0, sizeof(*win));
    win->id        = id;
    win->container = ci;
    win->alive     = 1;
    win->client_fd  = client_fd;   /* this tab's own client socket */
    win->savereq_wr = -1;
    pthread_mutex_init(&win->write_lock, NULL);
    win->sld_val[0] = 1.0;   /* k initial (matches Cocoa) */
    win->sld_val[1] = 1.0;   /* A initial                 */
    snprintf(win->title, sizeof(win->title), "%s", title[0] ? title : "giza");

    GS.conts[ci].active = slot;    /* newly opened tab is shown */
    GS.n_wins++;

    _set_container_title(&GS.conts[ci]);
    if (!newc) XRaiseWindow(GS.dpy, GS.conts[ci].xwin);
    _repaint_container(&GS.conts[ci]);
}

/* Geometry of the container's body — where the active tab's plot + sliders
 * are drawn. The tab bar steals the top TABBAR_H only when 2+ tabs exist. */
static void _body_geom(GsContainer *c, int n_tabs,
                       int *bx, int *by, int *bw, int *bh)
{
    int top = (n_tabs >= 2) ? TABBAR_H : 0;
    *bx = 0;
    *by = top;
    *bw = c->width;
    *bh = c->height - top;
}

/* Draw the two slider strips (bottom = k, right = A) within the body
 * rectangle (bx,by,bw,bh). Called from _repaint_container with a live
 * cairo_t on the container's xlib surface. */
static void
_draw_sliders(cairo_t *cr, GsWindow *win, int bx, int by, int bw, int bh)
{
    double pw = (double)bw - SLIDER_W;   /* plot-area width  (body-local) */
    double ph = (double)bh - SLIDER_H;   /* plot-area height (body-local) */
    if (pw < 20.0 || ph < 20.0) return;

    cairo_save(cr);
    cairo_translate(cr, bx, by);

    /* strip backgrounds */
    cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
    cairo_rectangle(cr, 0.0, ph, (double)bw, (double)SLIDER_H);
    cairo_rectangle(cr, pw,  0.0, (double)SLIDER_W, (double)bh);
    cairo_fill(cr);

    cairo_set_line_width(cr, 2.0);

    /* horizontal slider (id 0 = k): track + thumb */
    double cyh = ph + SLIDER_H / 2.0;
    cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
    cairo_move_to(cr, 8.0, cyh);
    cairo_line_to(cr, pw - 8.0, cyh);
    cairo_stroke(cr);
    double kf = (win->sld_val[0] - SLD_K_MIN) / (SLD_K_MAX - SLD_K_MIN);
    cairo_set_source_rgb(cr, 0.20, 0.40, 0.80);
    cairo_arc(cr, 8.0 + kf * (pw - 16.0), cyh, 7.0, 0.0, TWO_PI);
    cairo_fill(cr);

    /* vertical slider (id 1 = A): track + thumb (min bottom, max top) */
    double cxv = pw + SLIDER_W / 2.0;
    cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
    cairo_move_to(cr, cxv, 8.0);
    cairo_line_to(cr, cxv, ph - 8.0);
    cairo_stroke(cr);
    double af = (win->sld_val[1] - SLD_A_MIN) / (SLD_A_MAX - SLD_A_MIN);
    cairo_set_source_rgb(cr, 0.20, 0.40, 0.80);
    cairo_arc(cr, cxv, 8.0 + (1.0 - af) * (ph - 16.0), 7.0, 0.0, TWO_PI);
    cairo_fill(cr);

    cairo_restore(cr);
}

/* X coordinate (left edge) where the close-x of tab i sits in the bar. */
static double _tab_close_cx(double x0, double tw)
{
    return x0 + tw - TAB_CLOSE_BOX / 2.0 - 4.0;
}

/* Draw the tab strip across the top of the container. tabs[] holds n>=2
 * live slot indices in display order. */
static void
_draw_tabbar(cairo_t *cr, GsContainer *c, const int *tabs, int n)
{
    double W  = (double)c->width;
    double tw = W / n;

    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.0);

    for (int i = 0; i < n; i++) {
        GsWindow *t   = &GS.wins[tabs[i]];
        double    x0  = i * tw;
        int       act = (tabs[i] == c->active);

        /* tab background */
        if (act) cairo_set_source_rgb(cr, 1.00, 1.00, 1.00);
        else     cairo_set_source_rgb(cr, 0.82, 0.82, 0.82);
        cairo_rectangle(cr, x0, 0.0, tw, (double)TABBAR_H);
        cairo_fill(cr);

        /* left separator */
        cairo_set_source_rgb(cr, 0.60, 0.60, 0.60);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, x0 + 0.5, 0.0);
        cairo_line_to(cr, x0 + 0.5, (double)TABBAR_H);
        cairo_stroke(cr);

        /* label (clipped to the tab, leaving room for the close box) */
        cairo_save(cr);
        cairo_rectangle(cr, x0 + 6.0, 0.0,
                        tw - 6.0 - (double)TAB_CLOSE_BOX, (double)TABBAR_H);
        cairo_clip(cr);
        cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);
        cairo_move_to(cr, x0 + 8.0, TABBAR_H / 2.0 + 4.0);
        cairo_show_text(cr, t->title);
        cairo_restore(cr);

        /* close "x" box */
        double cx = _tab_close_cx(x0, tw);
        double cy = TABBAR_H / 2.0;
        double r  = 4.0;
        cairo_set_source_rgb(cr, 0.35, 0.35, 0.35);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, cx - r, cy - r); cairo_line_to(cr, cx + r, cy + r);
        cairo_move_to(cr, cx + r, cy - r); cairo_line_to(cr, cx - r, cy + r);
        cairo_stroke(cr);
    }

    /* bottom edge under the whole bar */
    cairo_set_source_rgb(cr, 0.60, 0.60, 0.60);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, 0.0, TABBAR_H - 0.5);
    cairo_line_to(cr, W,   TABBAR_H - 0.5);
    cairo_stroke(cr);
}

/* Hit-test a click in the tab bar. Returns the clicked tab's slot index (or
 * -1), and sets *want_close when the click landed on that tab's close box. */
static int
_tabbar_hit(GsContainer *c, const int *tabs, int n, int ex, int ey,
            int *want_close)
{
    *want_close = 0;
    if (ey < 0 || ey >= TABBAR_H) return -1;
    double tw = (double)c->width / n;
    int i = (int)((double)ex / tw);
    if (i < 0 || i >= n) return -1;

    double x0 = i * tw;
    double cx = _tab_close_cx(x0, tw);
    int d = ex - (int)cx;
    if (d < 0) d = -d;
    if (d <= TAB_CLOSE_BOX / 2 + 2) *want_close = 1;
    return tabs[i];
}

/* Repaint a container: clear, paint the active tab's surface + sliders into
 * the body, then the tab bar on top (if 2+ tabs). */
static void
_repaint_container(GsContainer *c)
{
    if (!c || !c->alive) return;
    int ci = (int)(c - GS.conts);

    int tabs[MAX_WINDOWS];
    int n = _container_tabs(ci, tabs, MAX_WINDOWS);

    cairo_surface_t *xsurf = cairo_xlib_surface_create(
        GS.dpy, c->xwin, DefaultVisual(GS.dpy, GS.screen),
        c->width, c->height);
    if (cairo_surface_status(xsurf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(xsurf);
        return;
    }
    cairo_t *cr = cairo_create(xsurf);

    /* clear */
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    /* resolve active tab (repair if the stored active is stale) */
    GsWindow *win = NULL;
    if (c->active >= 0 && GS.wins[c->active].alive &&
        GS.wins[c->active].container == ci) {
        win = &GS.wins[c->active];
    } else if (n > 0) {
        c->active = tabs[0];
        win = &GS.wins[tabs[0]];
    }

    int bx, by, bw, bh;
    _body_geom(c, n, &bx, &by, &bw, &bh);

    if (win && win->surface) {
        cairo_save(cr);
        cairo_set_source_surface(cr, win->surface, bx, by);
        cairo_paint(cr);
        cairo_restore(cr);
        _draw_sliders(cr, win, bx, by, bw, bh);
    }

    if (n >= 2)
        _draw_tabbar(cr, c, tabs, n);

    cairo_destroy(cr);
    cairo_surface_destroy(xsurf);
    XFlush(GS.dpy);
}

/* Tear down one figure (tab) and, for a user-initiated close, signal an
 * interactive client so it stops blocking.
 *
 * user_initiated:
 *   1  the user discarded the figure (WM_DELETE on the container, or a click
 *      on the tab's close "x"). An interactive client (Perl Driver::GS
 *      show_interactive) is blocked reading its socket and its run loop only
 *      returns on EOF, so we shutdown() its fd here. We shutdown(SHUT_RDWR),
 *      never close(): it delivers EOF to the client and wakes our own
 *      connection thread, which then performs the single close() in its
 *      teardown (see _connection_thread `done:`). Using close() here would
 *      race fd reuse against a concurrent accept().
 *   0  the client asked us to close (GSP_MSG_CLOSE). Its connection thread is
 *      already on its way to close the fd, so we must NOT touch it.
 *
 * One connection backs exactly one tab in this backend (the connection thread
 * binds a fixed win_id), so there is no shared-fd sibling case: the tab owns
 * its fd outright. client_fd is read and cleared under write_lock before the
 * lock is destroyed; in the user-initiated case the connection thread is
 * parked in read() and not touching the lock, and the shutdown is issued
 * last, after the slot is marked dead (alive=0), so when the thread wakes it
 * skips the now-destroyed lock and just close()s.
 *
 * The container's X window is destroyed only when its last tab goes away;
 * otherwise the bar/body reflow and the container is repainted. */
static void
_close_tab(int id, int user_initiated)
{
    GsWindow *win = _find_or_create_win(id);
    if (!win) return;
    int slot = (int)(win - GS.wins);
    int ci   = win->container;

    int fd = -1;
    pthread_mutex_lock(&win->write_lock);
    fd = win->client_fd;
    win->client_fd = -1;          /* stop reverse sends */
    pthread_mutex_unlock(&win->write_lock);

    if (win->surface) { cairo_surface_destroy(win->surface); win->surface = NULL; }
    free(win->save_path);
    win->save_path = NULL;
    pthread_mutex_destroy(&win->write_lock);
    win->alive     = 0;
    win->container = -1;
    GS.n_wins--;

    if (user_initiated && fd >= 0)
        shutdown(fd, SHUT_RDWR);

    /* container bookkeeping */
    if (ci >= 0 && ci < MAX_CONTAINERS && GS.conts[ci].alive) {
        GsContainer *c = &GS.conts[ci];
        int tabs[MAX_WINDOWS];
        int n = _container_tabs(ci, tabs, MAX_WINDOWS);
        if (n == 0) {
            XDestroyWindow(GS.dpy, c->xwin);
            XFreeGC(GS.dpy, c->gc);
            c->alive = 0;
        } else {
            if (c->active == slot) c->active = tabs[0];   /* pick a survivor */
            _set_container_title(c);
            _repaint_container(c);
        }
    }
}

/* Close every tab in a container (user closed the whole window). The final
 * _close_tab destroys the X window when the tab count reaches zero. */
static void
_close_container(int ci, int user_initiated)
{
    if (ci < 0 || ci >= MAX_CONTAINERS || !GS.conts[ci].alive) return;
    int ids[MAX_WINDOWS], n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (GS.wins[i].alive && GS.wins[i].container == ci)
            ids[n++] = GS.wins[i].id;
    for (int k = 0; k < n; k++)
        _close_tab(ids[k], user_initiated);
}

/* ------------------------------------------------------------------ */
/* Main-thread command dispatcher                                       */
/* ------------------------------------------------------------------ */

static void
_dispatch(Cmd *cmd)
{
    GsWindow *win;
    switch (cmd->type) {

    case CMD_NEWWIN:
        _open_window(cmd->win_id, cmd->fd, cmd->width, cmd->height,
                     cmd->title, cmd->group_id);
        break;

    case CMD_PNG:
        win = _find_or_create_win(cmd->win_id);
        if (win) {
            if (win->surface) cairo_surface_destroy(win->surface);
            win->surface = cmd->surface;   /* takes ownership          */
            cmd->surface = NULL;
            /* Repaint only if this tab is the visible one; a background
             * tab just stores its surface for when it is selected.    */
            int ci   = win->container;
            int slot = (int)(win - GS.wins);
            if (ci >= 0 && GS.conts[ci].alive && GS.conts[ci].active == slot)
                _repaint_container(&GS.conts[ci]);
        } else {
            if (cmd->surface) cairo_surface_destroy(cmd->surface);
        }
        break;

    case CMD_TITLE:
        win = _find_or_create_win(cmd->win_id);
        if (win) {
            snprintf(win->title, sizeof(win->title), "%s", cmd->title);
            int ci = win->container;
            if (ci >= 0 && GS.conts[ci].alive) {
                _set_container_title(&GS.conts[ci]);
                _repaint_container(&GS.conts[ci]);   /* tab label may show it */
            }
        }
        break;

    case CMD_CLOSE:
        _close_tab(cmd->win_id, 0);   /* client asked: it closes its own fd */
        break;

    case CMD_SAVEPATH:
        /* Write vector bytes received from the client to disk.       */
        if (cmd->save_bytes && cmd->save_len > 0 && cmd->save_path[0]) {
            FILE *f = fopen(cmd->save_path, "wb");
            if (f) {
                fwrite(cmd->save_bytes, 1, cmd->save_len, f);
                fclose(f);
            } else {
                fprintf(stderr, "giza_server: cannot write %s: %s\n",
                        cmd->save_path, strerror(errno));
            }
        }
        free(cmd->save_bytes);
        break;
    }
    free(cmd);
}

/* ------------------------------------------------------------------ */
/* X event handler (main thread)                                       */
/* ------------------------------------------------------------------ */

/* Send GSP_MSG_SLIDER(id, value) to this window's client. Serialized by
 * write_lock so the 16-byte header + 5-byte payload are not interleaved
 * with the connection thread's ACK/PONG writes on the same fd. No-op if
 * the client has exited (client_fd == -1), which is the case for plain
 * show() windows. */
static void
_slider_send(GsWindow *win, uint8_t id, float value)
{
    if (!win) return;
    pthread_mutex_lock(&win->write_lock);
    int fd = win->client_fd;
    if (fd >= 0) {
        gsp_slider_t body;
        memset(&body, 0, sizeof(body));
        body.slider_id = id;
        body.value     = value;
        if (_send_hdr(fd, GSP_MSG_SLIDER, (uint32_t)sizeof(body), 0) == 0)
            _write_exact(fd, &body, sizeof(body));
    }
    pthread_mutex_unlock(&win->write_lock);
}

/* Pointer pressed/dragged at container-relative (ex,ey) over the active
 * tab's body rect (bx,by,bw,bh). If it lands on the bottom (k) or right (A)
 * slider strip, update that slider, repaint, and push the value to the tab's
 * client. */
static void
_slider_input(GsWindow *win, int bx, int by, int bw, int bh, int ex, int ey)
{
    int x = ex - bx;
    int y = ey - by;
    double pw = (double)bw - SLIDER_W;
    double ph = (double)bh - SLIDER_H;
    if (pw < 20.0 || ph < 20.0) return;
    if (x < 0 || y < 0) return;

    int ci = win->container;
    GsContainer *c = (ci >= 0 && ci < MAX_CONTAINERS) ? &GS.conts[ci] : NULL;

    if (y >= ph && x <= pw) {                 /* bottom strip → k (id 0) */
        double frac = _clampd(((double)x - 8.0) / (pw - 16.0), 0.0, 1.0);
        double val  = SLD_K_MIN + frac * (SLD_K_MAX - SLD_K_MIN);
        win->sld_val[0] = val;
        if (c) _repaint_container(c);
        _slider_send(win, 0, (float)val);
    } else if (x >= pw && y <= ph) {          /* right strip → A (id 1) */
        double frac = _clampd(1.0 - ((double)y - 8.0) / (ph - 16.0), 0.0, 1.0);
        double val  = SLD_A_MIN + frac * (SLD_A_MAX - SLD_A_MIN);
        win->sld_val[1] = val;
        if (c) _repaint_container(c);
        _slider_send(win, 1, (float)val);
    }
}

static GsContainer *_container_for_xwin(Window xw)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (GS.conts[i].alive && GS.conts[i].xwin == xw)
            return &GS.conts[i];
    return NULL;
}

/* Route a body event (button/motion) to the active tab's slider strips,
 * unless it landed on the tab bar (handled separately). */
static void _body_slider_event(GsContainer *c, int ex, int ey)
{
    int tabs[MAX_WINDOWS];
    int n = _container_tabs((int)(c - GS.conts), tabs, MAX_WINDOWS);
    if (n >= 2 && ey < TABBAR_H) return;            /* in the tab bar */
    if (c->active < 0 || !GS.wins[c->active].alive) return;
    int bx, by, bw, bh;
    _body_geom(c, n, &bx, &by, &bw, &bh);
    _slider_input(&GS.wins[c->active], bx, by, bw, bh, ex, ey);
}

static void
_handle_xevent(XEvent *ev)
{
    switch (ev->type) {
    case Expose: {
        GsContainer *c = _container_for_xwin(ev->xexpose.window);
        if (c) _repaint_container(c);
        break;
    }

    case ConfigureNotify: {
        GsContainer *c = _container_for_xwin(ev->xconfigure.window);
        if (c) {
            c->width  = ev->xconfigure.width;
            c->height = ev->xconfigure.height;
            _repaint_container(c);
        }
        break;
    }

    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == GS.wm_delete) {
            /* User closed the whole window — close every tab in it and signal
             * each interactive client (EOF) so its run loop returns. */
            GsContainer *c = _container_for_xwin(ev->xclient.window);
            if (c) _close_container((int)(c - GS.conts), 1);
        }
        break;

    case ButtonPress:
        if (ev->xbutton.button == Button1) {
            GsContainer *c = _container_for_xwin(ev->xbutton.window);
            if (!c) break;
            int tabs[MAX_WINDOWS];
            int n = _container_tabs((int)(c - GS.conts), tabs, MAX_WINDOWS);
            if (n >= 2 && ev->xbutton.y < TABBAR_H) {
                int want_close = 0;
                int slot = _tabbar_hit(c, tabs, n, ev->xbutton.x,
                                       ev->xbutton.y, &want_close);
                if (slot >= 0) {
                    if (want_close) {
                        /* per-tab close = user discard → signal client EOF */
                        _close_tab(GS.wins[slot].id, 1);
                    } else if (slot != c->active) {
                        c->active = slot;
                        _set_container_title(c);
                        _repaint_container(c);
                    }
                }
                break;
            }
            _body_slider_event(c, ev->xbutton.x, ev->xbutton.y);
        }
        break;

    case MotionNotify: {
        /* Button1MotionMask ⇒ only delivered while button 1 is held. */
        GsContainer *c = _container_for_xwin(ev->xmotion.window);
        if (c) _body_slider_event(c, ev->xmotion.x, ev->xmotion.y);
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Per-connection thread                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int  fd;
    int  win_id;
} ConnArg;

static void *
_connection_thread(void *arg)
{
    ConnArg *ca  = arg;
    int      fd  = ca->fd;
    int      wid = ca->win_id;
    free(ca);

    /* client_fd is bound to the window's own slot inside _open_window
     * (CMD_NEWWIN carries fd). The previous registration here used
     * GS.wins[wid % MAX_WINDOWS], a different slot than _open_window
     * allocates, and ran before that slot's write_lock was initialized -
     * so the window's real slot kept client_fd == -1 and close/reverse
     * sends silently no-op'd. */

    /* Tab-grouping key: the connecting client's PID via SO_PEERCRED (the
     * Linux equivalent of Cocoa's LOCAL_PEERPID). All windows opened by the
     * same process land in one tab group. Unavailable → 0 (standalone). */
    uint32_t peer_pid = 0;
    {
        struct ucred uc;
        socklen_t ul = sizeof(uc);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &ul) == 0 && uc.pid > 0)
            peer_pid = (uint32_t)uc.pid;
    }

    uint32_t seq_out = 0;

    for (;;) {
        gsp_header_t hdr;
        if (_read_exact(fd, &hdr, sizeof(hdr)) < 0) break;
        if (hdr.magic != GSP_MAGIC) break;

        uint32_t len = hdr.length;
        unsigned char *payload = NULL;
        if (len > 0) {
            if (len > GSP_MAX_PNG_BYTES) break;
            payload = malloc(len);
            if (!payload) break;
            if (_read_exact(fd, payload, len) < 0) { free(payload); break; }
        }

        switch (hdr.type) {

        case GSP_MSG_PING: {
            _send_hdr(fd, GSP_MSG_PONG, 0, seq_out++);
            break;
        }

        case GSP_MSG_NEWWIN: {
            Cmd *cmd = calloc(1, sizeof(Cmd));
            if (!cmd) { free(payload); goto done; }
            cmd->type   = CMD_NEWWIN;
            cmd->win_id = wid;
            cmd->fd     = fd;          /* wire client_fd to THIS window's slot */
            cmd->group_id = peer_pid;  /* tab group (same client PID = tabs)   */
            if (len >= sizeof(gsp_newwin_t)) {
                gsp_newwin_t nw;
                memcpy(&nw, payload, sizeof(nw));
                cmd->width  = (int)nw.width_px;
                cmd->height = (int)nw.height_px;
            }
            _send_cmd(cmd);
            _send_hdr(fd, GSP_MSG_ACK, 0, seq_out++);
            break;
        }

        case GSP_MSG_PNG: {
            cairo_surface_t *surf = _decode_png(payload, len);
            if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
                if (surf) cairo_surface_destroy(surf);
                _send_hdr_locked(wid, fd, GSP_MSG_ERR, 0, seq_out++);
                break;
            }
            Cmd *cmd = calloc(1, sizeof(Cmd));
            if (!cmd) { cairo_surface_destroy(surf); free(payload); goto done; }
            cmd->type    = CMD_PNG;
            cmd->win_id  = wid;
            cmd->surface = surf;
            _send_cmd(cmd);
            _send_hdr_locked(wid, fd, GSP_MSG_ACK, 0, seq_out++);
            break;
        }

        case GSP_MSG_TITLE: {
            Cmd *cmd = calloc(1, sizeof(Cmd));
            if (!cmd) { free(payload); goto done; }
            cmd->type   = CMD_TITLE;
            cmd->win_id = wid;
            size_t n = len < MAX_TITLE_LEN ? len : MAX_TITLE_LEN;
            memcpy(cmd->title, payload, n);
            cmd->title[n] = '\0';
            _send_cmd(cmd);
            /* TITLE is fire-and-forget: no ACK */
            break;
        }

        case GSP_MSG_SAVEDATA: {
            /* Server8: client returns vector bytes for a pending save */
            if (len < 1) break;
            uint8_t fmt = payload[0];
            size_t  blen = len - 1;
            unsigned char *bytes = malloc(blen);
            if (!bytes) break;
            memcpy(bytes, payload + 1, blen);

            /* Retrieve the pending save path that was set by the main
             * thread when it sent SAVEREQ.                           */
            GsWindow *win = NULL;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (GS.wins[i].alive && GS.wins[i].id == wid) {
                    win = &GS.wins[i];
                    break;
                }
            }
            if (!win) { free(bytes); break; }

            char *path = NULL;
            pthread_mutex_lock(&win->write_lock);
            if (win->save_path && win->save_fmt == fmt) {
                path = win->save_path;
                win->save_path = NULL;   /* cleared — ready for next save */
            }
            pthread_mutex_unlock(&win->write_lock);

            if (path) {
                Cmd *cmd = calloc(1, sizeof(Cmd));
                if (cmd) {
                    cmd->type       = CMD_SAVEPATH;
                    cmd->win_id     = wid;
                    cmd->save_fmt   = fmt;
                    cmd->save_bytes = bytes;
                    cmd->save_len   = blen;
                    snprintf(cmd->save_path, sizeof(cmd->save_path), "%s", path);
                    _send_cmd(cmd);
                } else {
                    free(bytes);
                }
                free(path);
            } else {
                free(bytes);
            }
            break;
        }

        case GSP_MSG_CLOSE: {
            Cmd *cmd = calloc(1, sizeof(Cmd));
            if (!cmd) { free(payload); goto done; }
            cmd->type   = CMD_CLOSE;
            cmd->win_id = wid;
            _send_cmd(cmd);
            _send_hdr(fd, GSP_MSG_ACK, 0, seq_out++);
            free(payload);
            goto done;
        }

        default:
            break;
        }

        free(payload);
        payload = NULL;
    }

done:
    /* Mark client as gone so _vectorSave can detect it.             */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (GS.wins[i].alive && GS.wins[i].id == wid) {
            pthread_mutex_lock(&GS.wins[i].write_lock);
            GS.wins[i].client_fd = -1;
            pthread_mutex_unlock(&GS.wins[i].write_lock);
            break;
        }
    }
    close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Accept thread                                                        */
/* ------------------------------------------------------------------ */

static void *
_accept_thread(void *arg)
{
    (void)arg;
    int next_id = 0;

    for (;;) {
        int fd = accept(GS.listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("giza_server: accept");
            break;
        }

        ConnArg *ca = malloc(sizeof(ConnArg));
        if (!ca) { close(fd); continue; }
        ca->fd     = fd;
        ca->win_id = next_id++;

        pthread_t t;
        if (pthread_create(&t, NULL, _connection_thread, ca) != 0) {
            perror("giza_server: pthread_create");
            free(ca);
            close(fd);
            continue;
        }
        pthread_detach(t);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Socket setup                                                         */
/* ------------------------------------------------------------------ */

static int
_setup_socket(void)
{
    /* sun_path is only 108 bytes on Linux; keep the path well under that. */
    gsp_resolve_sock_path(GS.sock_path, sizeof(GS.sock_path));
    if (strlen(GS.sock_path) >= 104) {
        fprintf(stderr, "giza_server: socket path too long: %s\n", GS.sock_path);
        return -1;
    }

    unlink(GS.sock_path);   /* remove stale socket if any             */

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, GS.sock_path, strlen(GS.sock_path) + 1);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(lfd);
        return -1;
    }
    chmod(GS.sock_path, 0700);

    if (listen(lfd, 16) < 0) {
        perror("listen");
        close(lfd);
        return -1;
    }
    GS.listen_fd = lfd;
    return 0;
}

/* ------------------------------------------------------------------ */
/* main()                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    /* Wake pipe: connection threads → X event loop.                  */
    {
        int pfd[2];
        if (pipe(pfd) != 0) { perror("pipe"); return 1; }
        g_wake_rd = pfd[0];
        g_wake_wr = pfd[1];
        /* Set O_NONBLOCK on both ends for robustness.                */
        fcntl(g_wake_rd, F_SETFL, O_NONBLOCK);
        fcntl(g_wake_wr, F_SETFL, O_NONBLOCK);
    }

    /* Open X display.                                                */
    GS.dpy = XOpenDisplay(NULL);
    if (!GS.dpy) {
        fprintf(stderr, "giza_server: cannot open display %s\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    GS.screen    = DefaultScreen(GS.dpy);
    GS.wm_delete = XInternAtom(GS.dpy, "WM_DELETE_WINDOW", False);

    /* Set up listening socket.                                       */
    if (_setup_socket() < 0) return 1;

    /* Launch accept thread.                                          */
    {
        pthread_t at;
        if (pthread_create(&at, NULL, _accept_thread, NULL) != 0) {
            perror("giza_server: pthread_create"); return 1;
        }
        pthread_detach(at);
    }

    fprintf(stderr, "giza_server (xlib): listening on %s\n", GS.sock_path);

    /* ---- Main event loop ----------------------------------------- */
    int xfd = ConnectionNumber(GS.dpy);

    for (;;) {
        /* Process all pending X events first.                        */
        while (XPending(GS.dpy)) {
            XEvent ev;
            XNextEvent(GS.dpy, &ev);
            _handle_xevent(&ev);
        }

        /* If all windows are closed and no clients, we could exit.
         * For pgxwin_server parity we stay alive; add an exit policy
         * here if desired.                                           */

        /* Wait for X events or pipe wakeup.                          */
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(xfd,       &rd);
        FD_SET(g_wake_rd, &rd);
        int nfds = (xfd > g_wake_rd ? xfd : g_wake_rd) + 1;

        struct timeval tv = { 0, 50000 };   /* 50 ms timeout          */
        int s = select(nfds, &rd, NULL, NULL, &tv);
        if (s < 0 && errno != EINTR) { perror("select"); break; }

        /* Drain the wake pipe and dispatch all queued Cmds.          */
        if (s > 0 && FD_ISSET(g_wake_rd, &rd)) {
            Cmd *cmd;
            while (read(g_wake_rd, &cmd, sizeof(Cmd *)) == (ssize_t)sizeof(Cmd *))
                _dispatch(cmd);
        }
    }

    XCloseDisplay(GS.dpy);
    unlink(GS.sock_path);
    return 0;
}
