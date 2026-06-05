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
#define MAX_WINDOWS     64
#define MAX_TITLE_LEN   255

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

typedef struct {
    int              id;
    Window           xwin;
    Display         *dpy;       /* same as global, kept for convenience */
    GC               gc;
    cairo_surface_t *surface;   /* current Cairo surface (IMAGE type)  */
    int              width, height;
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
} GsWindow;

/* ------------------------------------------------------------------ */
/* Global server state                                                 */
/* ------------------------------------------------------------------ */

static struct {
    Display    *dpy;
    int         screen;
    Atom        wm_delete;     /* WM_DELETE_WINDOW atom                */
    GsWindow    wins[MAX_WINDOWS];
    pthread_mutex_t wins_lock;
    char        sock_path[256];
    int         n_wins;
    int         listen_fd;
} GS;

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

static void
_open_window(int id, int w, int h, const char *title)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (GS.wins[i].alive) continue;

        GsWindow *win = &GS.wins[i];
        memset(win, 0, sizeof(*win));
        win->id      = id;
        win->dpy     = GS.dpy;
        win->width   = w ? w : DEFAULT_WIN_W;
        win->height  = h ? h : DEFAULT_WIN_H;
        win->alive   = 1;
        win->client_fd  = -1;
        win->savereq_wr = -1;
        pthread_mutex_init(&win->write_lock, NULL);
        snprintf(win->title, sizeof(win->title), "%s", title[0] ? title : "giza");

        win->xwin = XCreateSimpleWindow(
            GS.dpy, RootWindow(GS.dpy, GS.screen),
            0, 0, (unsigned)win->width, (unsigned)win->height,
            1,
            BlackPixel(GS.dpy, GS.screen),
            WhitePixel(GS.dpy, GS.screen));

        XStoreName(GS.dpy, win->xwin, win->title);
        XSetWMProtocols(GS.dpy, win->xwin, &GS.wm_delete, 1);
        win->gc = XCreateGC(GS.dpy, win->xwin, 0, NULL);
        XSelectInput(GS.dpy, win->xwin, ExposureMask | StructureNotifyMask);
        XMapRaised(GS.dpy, win->xwin);
        GS.n_wins++;
        return;
    }
    fprintf(stderr, "giza_server: too many windows (max %d)\n", MAX_WINDOWS);
}

static void
_repaint_window(GsWindow *win)
{
    if (!win || !win->surface || !win->alive) return;

    /* Draw via cairo-xlib surface */
    cairo_surface_t *xsurf = cairo_xlib_surface_create(
        GS.dpy, win->xwin, DefaultVisual(GS.dpy, GS.screen),
        win->width, win->height);
    if (cairo_surface_status(xsurf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(xsurf);
        return;
    }
    cairo_t *cr = cairo_create(xsurf);
    cairo_set_source_surface(cr, win->surface, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(xsurf);
    XFlush(GS.dpy);
}

/* Tear down a window's resources and, for a user-initiated close, signal an
 * interactive client so it stops blocking.
 *
 * user_initiated:
 *   1  the user closed the window (WM_DELETE). An interactive client
 *      (Perl Driver::GS show_interactive) is blocked reading its socket and
 *      its run loop only returns on EOF, so we shutdown() its fd here. We
 *      shutdown(SHUT_RDWR), never close(): it delivers EOF to the client and
 *      wakes our own connection thread, which then performs the single
 *      close() in its teardown (see _connection_thread `done:`). Using
 *      close() here would race fd reuse against a concurrent accept().
 *   0  the client asked us to close (GSP_MSG_CLOSE). Its connection thread is
 *      already on its way to close the fd, so we must NOT touch it.
 *
 * One connection backs exactly one window in this backend (the connection
 * thread binds a fixed win_id), so there is no shared-fd sibling case: the
 * window owns its fd outright. client_fd is read and cleared under write_lock
 * before the lock is destroyed; in the user-initiated case the connection
 * thread is parked in read() and not touching the lock, and the shutdown is
 * issued last, after the slot is marked dead (alive=0), so when the thread
 * wakes it skips the now-destroyed lock and just close()s. */
static void
_close_window(int id, int user_initiated)
{
    GsWindow *win = _find_or_create_win(id);
    if (!win) return;

    int fd = -1;
    pthread_mutex_lock(&win->write_lock);
    fd = win->client_fd;
    win->client_fd = -1;          /* stop reverse sends */
    pthread_mutex_unlock(&win->write_lock);

    XDestroyWindow(GS.dpy, win->xwin);
    XFreeGC(GS.dpy, win->gc);
    if (win->surface) { cairo_surface_destroy(win->surface); win->surface = NULL; }
    free(win->save_path);
    win->save_path = NULL;
    pthread_mutex_destroy(&win->write_lock);
    win->alive = 0;
    GS.n_wins--;

    if (user_initiated && fd >= 0)
        shutdown(fd, SHUT_RDWR);
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
        _open_window(cmd->win_id, cmd->width, cmd->height, cmd->title);
        break;

    case CMD_PNG:
        win = _find_or_create_win(cmd->win_id);
        if (win) {
            if (win->surface) cairo_surface_destroy(win->surface);
            win->surface = cmd->surface;   /* takes ownership          */
            cmd->surface = NULL;
            _repaint_window(win);
        } else {
            if (cmd->surface) cairo_surface_destroy(cmd->surface);
        }
        break;

    case CMD_TITLE:
        win = _find_or_create_win(cmd->win_id);
        if (win) {
            snprintf(win->title, sizeof(win->title), "%s", cmd->title);
            XStoreName(GS.dpy, win->xwin, win->title);
            XFlush(GS.dpy);
        }
        break;

    case CMD_CLOSE:
        _close_window(cmd->win_id, 0);   /* client asked: it closes its own fd */
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

static void
_handle_xevent(XEvent *ev)
{
    switch (ev->type) {
    case Expose:
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (GS.wins[i].alive && GS.wins[i].xwin == ev->xexpose.window) {
                _repaint_window(&GS.wins[i]);
                break;
            }
        }
        break;

    case ConfigureNotify:
        for (int i = 0; i < MAX_WINDOWS; i++) {
            GsWindow *win = &GS.wins[i];
            if (!win->alive || win->xwin != ev->xconfigure.window) continue;
            win->width  = ev->xconfigure.width;
            win->height = ev->xconfigure.height;
            _repaint_window(win);
            break;
        }
        break;

    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == GS.wm_delete) {
            /* User closed a window — mark dead and signal an interactive
             * client (EOF) so its run loop returns instead of blocking. */
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (GS.wins[i].alive && GS.wins[i].xwin == ev->xclient.window) {
                    _close_window(GS.wins[i].id, 1);
                    break;
                }
            }
        }
        break;

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

    /* Register client_fd on the window for SAVEREQ reverse-sends.   */
    {
        pthread_mutex_lock(&GS.wins[wid % MAX_WINDOWS].write_lock);
        GS.wins[wid % MAX_WINDOWS].client_fd = fd;
        pthread_mutex_unlock(&GS.wins[wid % MAX_WINDOWS].write_lock);
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
                _send_hdr(fd, GSP_MSG_ERR, 0, seq_out++);
                break;
            }
            Cmd *cmd = calloc(1, sizeof(Cmd));
            if (!cmd) { cairo_surface_destroy(surf); free(payload); goto done; }
            cmd->type    = CMD_PNG;
            cmd->win_id  = wid;
            cmd->surface = surf;
            _send_cmd(cmd);
            _send_hdr(fd, GSP_MSG_ACK, 0, seq_out++);
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
