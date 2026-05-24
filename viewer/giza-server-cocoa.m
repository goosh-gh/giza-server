/* viewer/giza-server-cocoa.m
 *
 * giza_server — macOS Cocoa backend
 *
 * Replaces giza-server-gtk.c for macOS builds.  The GSP wire protocol
 * (giza-server-protocol.h) is 100% identical; only the windowing layer
 * changes.
 *
 * Architecture:
 *   main thread      — NSApp run loop (set up in giza-server-main.m)
 *   worker thread    — accept() loop, per-connection threads (POSIX)
 *   UI updates       — dispatch_async(dispatch_get_main_queue(), ^{})
 *                      (replaces GTK's g_idle_add)
 *
 * Key design choices (inherited from giza PR #86 experience):
 *   • NSApp MUST run on the main thread — enforced by giza-server-main.m
 *   • All NSWindow/NSView calls dispatched to main queue
 *   • PNG decoded to NSImage via NSData (no temp files)
 *   • Letterbox scaling in drawRect:  (same maths as GTK on_draw())
 *   • Window tabbing via NSWindowTabbingIdentifier (matches PDL::Graphics::Cairo)
 *   • Max 64 windows (matches GTK backend constant)
 *
 * Copyright (c) 2026 goosh-gh — LGPL-2.1 (same as giza)
 */

#import <Cocoa/Cocoa.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "giza-server-protocol.h"

/* ------------------------------------------------------------------ */
/* GsView — custom NSView that letterbox-scales the current NSImage    */
/* ------------------------------------------------------------------ */

@interface GsView : NSView {
@public
    NSImage  *_image;   /* current plot frame          */
    NSLock   *_lock;
}
- (void)setImage:(NSImage *)img;
@end

@implementation GsView

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        _lock  = [[NSLock alloc] init];
        _image = nil;
    }
    return self;
}

- (void)setImage:(NSImage *)img
{
    [_lock lock];
    [_image release];
    _image = [img retain];
    [_lock unlock];
    dispatch_async(dispatch_get_main_queue(), ^{
        [self setNeedsDisplay:YES];
    });
}

- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;

    /* white background */
    [[NSColor whiteColor] setFill];
    NSRectFill(self.bounds);

    [_lock lock];
    NSImage *img = [_image retain];
    [_lock unlock];

    if (!img) return;

    NSSize  isize  = img.size;
    NSRect  bounds = self.bounds;
    double  sx     = bounds.size.width  / isize.width;
    double  sy     = bounds.size.height / isize.height;
    double  scale  = (sx < sy) ? sx : sy;
    double  dw     = isize.width  * scale;
    double  dh     = isize.height * scale;
    double  ox     = (bounds.size.width  - dw) / 2.0;
    double  oy     = (bounds.size.height - dh) / 2.0;

    NSRect dst = NSMakeRect(ox, oy, dw, dh);
    [img drawInRect:dst
           fromRect:NSZeroRect
          operation:NSCompositingOperationSourceOver
           fraction:1.0];

    [img release];
}

- (BOOL)isFlipped { return NO; }   /* Cocoa default: origin bottom-left */

@end /* GsView */


/* ------------------------------------------------------------------ */
/* Per-window state                                                    */
/* ------------------------------------------------------------------ */

#define MAX_WINDOWS 64

typedef struct {
    int        id;
    NSWindow  *window;    /* retained — only touch from main thread   */
    GsView    *view;      /* retained — setImage: is thread-safe       */
    char       title[256];
    int        alive;
} GsWindow;

/* ------------------------------------------------------------------ */
/* Global server state                                                 */
/* ------------------------------------------------------------------ */

static struct {
    GsWindow    wins[MAX_WINDOWS];
    pthread_mutex_t wins_lock;
    char        sock_path[256];
    int         listen_fd;
    int         n_wins;
} GS;

/* ------------------------------------------------------------------ */
/* Public init — called from main thread before worker starts          */
/* ------------------------------------------------------------------ */

void giza_server_cocoa_init(void)
{
    memset(&GS, 0, sizeof(GS));
    pthread_mutex_init(&GS.wins_lock, NULL);
    GS.listen_fd = -1;

    /* Build socket path */
    snprintf(GS.sock_path, sizeof(GS.sock_path),
             GIZA_SERVER_SOCK_DIR "/" GIZA_SERVER_SOCK_NAME,
             (int)getuid());

    /* NSApplication singleton */
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    [NSApp activateIgnoringOtherApps:YES];
}

/* ------------------------------------------------------------------ */
/* Window creation — must run on main thread                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int  suggested_w, suggested_h;
    char title[256];
    int  result_id;           /* filled in by main-thread block       */
    dispatch_semaphore_t sem;
} NewWinReq;

static void _create_window_on_main(void *ptr)
{
    NewWinReq *req = ptr;

    pthread_mutex_lock(&GS.wins_lock);
    if (GS.n_wins >= MAX_WINDOWS) {
        pthread_mutex_unlock(&GS.wins_lock);
        req->result_id = -1;
        dispatch_semaphore_signal(req->sem);
        return;
    }
    int idx = GS.n_wins++;
    pthread_mutex_unlock(&GS.wins_lock);

    GsWindow *w = &GS.wins[idx];
    memset(w, 0, sizeof(*w));
    w->id    = idx;
    w->alive = 1;
    strncpy(w->title,
            req->title[0] ? req->title : "giza",
            sizeof(w->title) - 1);

    int iw = req->suggested_w > 0 ? req->suggested_w : 800;
    int ih = req->suggested_h > 0 ? req->suggested_h : 600;

    NSRect frame = NSMakeRect(100 + idx * 24, 100 + idx * 24, iw, ih);
    NSUInteger style =
        NSWindowStyleMaskTitled          |
        NSWindowStyleMaskClosable        |
        NSWindowStyleMaskMiniaturizable  |
        NSWindowStyleMaskResizable;

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:style
                    backing:NSBackingStoreBuffered
                      defer:NO];

    [win setTitle:[NSString stringWithUTF8String:w->title]];
    [win setReleasedWhenClosed:NO];

    /* Tab grouping — all giza_server windows share one tabbing ID */
    if (@available(macOS 10.12, *)) {
        win.tabbingIdentifier = @"giza_server_windows";
        win.tabbingMode       = NSWindowTabbingModePreferred;
    }

    GsView *view = [[GsView alloc] initWithFrame:win.contentView.bounds];
    [view setAutoresizingMask:
        NSViewWidthSizable | NSViewHeightSizable];
    [win.contentView addSubview:view];

    w->window = win;    /* retained */
    w->view   = view;   /* retained */

    [win makeKeyAndOrderFront:nil];

    req->result_id = idx;
    dispatch_semaphore_signal(req->sem);
}

/* Synchronously create a window from a non-main thread               */
static int _create_window_sync(int w_px, int h_px, const char *title)
{
    NewWinReq *req = calloc(1, sizeof(*req));
    req->suggested_w = w_px;
    req->suggested_h = h_px;
    strncpy(req->title, title ? title : "giza", sizeof(req->title) - 1);
    req->result_id = -1;
    req->sem = dispatch_semaphore_create(0);

    dispatch_async(dispatch_get_main_queue(), ^{
        _create_window_on_main(req);
    });

    dispatch_semaphore_wait(req->sem, DISPATCH_TIME_FOREVER);
    dispatch_release(req->sem);

    int id = req->result_id;
    free(req);
    return id;
}

/* ------------------------------------------------------------------ */
/* PNG update — decode NSImage from memory, push to view               */
/* ------------------------------------------------------------------ */

static void _update_window(int win_id, const char *png_data, size_t png_len,
                            const char *title)
{
    if (win_id < 0 || win_id >= GS.n_wins) return;
    GsWindow *w = &GS.wins[win_id];
    if (!w->alive) return;

    /* Copy data so the block can own it */
    char  *buf  = malloc(png_len);
    if (!buf) return;
    memcpy(buf, png_data, png_len);


    GsView   *view   = w->view;
    NSWindow *window = w->window;

    /* 配列はブロックにキャプチャできないのでNSStringに変換してからretain */
    NSString *ns_title = (title && title[0])
        ? [[NSString stringWithUTF8String:title] retain]
        : nil;

    dispatch_async(dispatch_get_main_queue(), ^{
        NSData  *data = [NSData dataWithBytesNoCopy:buf
                                             length:png_len
                                       freeWhenDone:YES];
        NSImage *img  = [[NSImage alloc] initWithData:data];
        if (img) {
            [view setImage:img];
            [img release];
        } else {
            fprintf(stderr, "giza_server: PNG decode failed\n");
        }
        if (ns_title) {
            [window setTitle:ns_title];
            [ns_title release];
        }
    });




}

/* ------------------------------------------------------------------ */
/* Low-level POSIX I/O                                                 */
/* ------------------------------------------------------------------ */

static int _read_exactly(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int _write_exactly(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(fd, (const char *)buf + done, n - done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int _send_msg(int fd, uint8_t type, uint32_t seq,
                     const void *payload, uint32_t plen)
{
    gsp_header_t hdr;
    hdr.magic   = GSP_MAGIC;
    hdr.version = GSP_VERSION;
    hdr.type    = type;
    hdr.flags   = 0;
    hdr.length  = plen;
    hdr.seq     = seq;
    if (_write_exactly(fd, &hdr, sizeof(hdr)) < 0) return -1;
    if (plen && _write_exactly(fd, payload, plen) < 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-connection thread                                               */
/* ------------------------------------------------------------------ */

typedef struct { int fd; } ConnArg;

static void *_handle_connection(void *arg)
{
    ConnArg *ca  = arg;
    int       fd = ca->fd;
    free(ca);

    int      current_win = -1;
    uint32_t seq_out     = 0;
    char     title_buf[256] = {0};

    for (;;) {
        gsp_header_t hdr;
        if (_read_exactly(fd, &hdr, sizeof(hdr)) < 0) break;

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
            if (_read_exactly(fd, payload, hdr.length) < 0) {
                free(payload); break;
            }
        }

        switch (hdr.type) {

        case GSP_MSG_PING:
            _send_msg(fd, GSP_MSG_PONG, seq_out++, NULL, 0);
            break;

        case GSP_MSG_NEWWIN: {
            gsp_newwin_t nw = {0, 0};
            if (payload && hdr.length >= sizeof(nw))
                memcpy(&nw, payload, sizeof(nw));
            const char *ttl = title_buf[0] ? title_buf : "giza";
            current_win = _create_window_sync(nw.width_px, nw.height_px, ttl);
            _send_msg(fd, GSP_MSG_ACK, seq_out++, NULL, 0);
            break;
        }

        case GSP_MSG_TITLE:

            /* fire-and-forget: NO ACK (matches GTK backend & protocol spec) */
            if (payload) {
                size_t len = hdr.length < 255 ? hdr.length : 255;
                memcpy(title_buf, payload, len);
                title_buf[len] = '\0';
                if (current_win >= 0) {
                    NSWindow *win = GS.wins[current_win].window;
                    NSString *ns  = [[NSString stringWithUTF8String:title_buf] retain];
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [win setTitle:ns];
                        [ns release];
                    });
                }
            }
            break;


        case GSP_MSG_PNG: {
            if (current_win < 0)
                current_win = _create_window_sync(800, 600, "giza");
            if (current_win >= 0) {
                /* ownership of payload transferred to _update_window  */
                _update_window(current_win, payload, hdr.length, title_buf);
                payload = NULL;
            }
            _send_msg(fd, GSP_MSG_ACK, seq_out++, NULL, 0);
            break;
        }

        case GSP_MSG_CLOSE:
            _send_msg(fd, GSP_MSG_ACK, seq_out++, NULL, 0);
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
    close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Accept loop — runs in worker thread                                 */
/* ------------------------------------------------------------------ */

int giza_server_worker_main(void)
{
    /* Remove stale socket */
    unlink(GS.sock_path);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("giza_server: socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, GS.sock_path, sizeof(addr.sun_path) - 1);

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("giza_server: bind");
        close(lfd);
        return 1;
    }
    if (listen(lfd, 8) < 0) {
        perror("giza_server: listen");
        close(lfd);
        return 1;
    }

    GS.listen_fd = lfd;
    fprintf(stdout, "giza_server: listening on %s\n", GS.sock_path);
    fflush(stdout);

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("giza_server: accept");
            break;
        }

        ConnArg *arg = malloc(sizeof(*arg));
        arg->fd = cfd;

        pthread_t thr;
        if (pthread_create(&thr, NULL, _handle_connection, arg) != 0) {
            perror("giza_server: pthread_create");
            close(cfd);
            free(arg);
        } else {
            pthread_detach(thr);
        }
    }

    close(lfd);
    unlink(GS.sock_path);
    return 0;
}
