/*
 * bench_xlib_repaint.c
 * ---------------------------------------------------------------------------
 * Reproduces, in isolation from giza-server, the xlib 3D-repaint pathology
 * that the double-buffer + coalesce patch fixes (the "#106 class" problem:
 * a 60fps 3D stream repainted straight onto the X window, one primitive per
 * X request), and measures the improvement.
 *
 * It does NOT drive the real server. It reproduces the exact *mechanism* of
 * the two _repaint_container paths so the before/after is measurable on any
 * X display (your Ubuntu VM's XWayland works fine):
 *
 *   DIRECT   = unpatched _repaint_container:
 *              cairo_xlib_surface on the window, clear, stroke every segment
 *              straight onto it, XFlush.  (drawing IS the on-screen update)
 *   BUFFERED = patched _repaint_container:
 *              compose the whole frame into an image surface, then blit it to
 *              the window in one cairo_paint.  (one image transfer per update)
 *
 * A synthetic frame of K short coloured segments stands in for a line-mode 3D
 * frame (cortex10K idle is ~41000 segments, so --strokes 40000 is realistic).
 *
 * Metrics per frame:
 *   - wall time (ms), with XSync forcing the server to process everything
 *     each frame -- this is the round-trip cost that saturates the real main
 *     thread and starves Expose/close/resize (the "can't close the window").
 *   - Xlib protocol requests issued (NextRequest delta; best-effort: reads
 *     low if your cairo is built on XCB, in which case trust the time column).
 *
 * Plus a pure-logic unit demo of the coalesce half: N 3D frames delivered in
 * one main-loop drain cost N repaints unpatched vs 1 repaint patched.
 *
 * Build (standalone; needs only cairo + X11, not the server):
 *   gcc -O2 -o bench_xlib_repaint bench_xlib_repaint.c \
 *       $(pkg-config --cflags --libs cairo x11) -lm
 *
 * Run:
 *   ./bench_xlib_repaint                 # 40000 strokes, 60 frames, both modes
 *   ./bench_xlib_repaint --strokes 20000 --frames 120
 *   ./bench_xlib_repaint --watch         # slow, visible: watch the sweep vs snap
 * ---------------------------------------------------------------------------
 */
#include <X11/Xlib.h>
#include <cairo.h>
#include <cairo-xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

typedef struct { float x0, y0, x1, y1; unsigned char r, g, b; } Seg;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* One synthetic line-mode frame: K short coloured segments scattered across
 * the window, standing in for a dense wireframe/scanline mesh. Generated once
 * and reused for both modes so they draw identical geometry. */
static Seg *make_frame(int K, int W, int H, unsigned seed) {
    srand(seed);
    Seg *s = malloc((size_t)K * sizeof(Seg));
    if (!s) { perror("malloc"); exit(1); }
    for (int i = 0; i < K; i++) {
        float x = (float)(rand() % W);
        float y = (float)(rand() % H);
        float a = (float)(rand() % 628) / 100.0f;
        float L = (float)(5 + rand() % 25);
        s[i].x0 = x;  s[i].y0 = y;
        s[i].x1 = x + cosf(a) * L;  s[i].y1 = y + sinf(a) * L;
        s[i].r = (unsigned char)(rand() & 255);
        s[i].g = (unsigned char)(rand() & 255);
        s[i].b = (unsigned char)(rand() & 255);
    }
    return s;
}

/* Identical content in both modes: clear white (as _repaint_container does),
 * then stroke every segment. sync_every>0 inserts XSync + a short sleep so the
 * bottom-to-top sweep is watchable in DIRECT mode (--watch only). */
static void draw_content(cairo_t *cr, const Seg *s, int K,
                         Display *dpy, int sync_every) {
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i < K; i++) {
        cairo_set_source_rgb(cr, s[i].r / 255.0, s[i].g / 255.0, s[i].b / 255.0);
        cairo_move_to(cr, s[i].x0, s[i].y0);
        cairo_line_to(cr, s[i].x1, s[i].y1);
        cairo_stroke(cr);
        if (sync_every && dpy && (i % sync_every) == 0) {
            XSync(dpy, False);
            struct timespec ts = { 0, 8 * 1000 * 1000 };  /* 8 ms */
            nanosleep(&ts, NULL);
        }
    }
}

/* Unpatched _repaint_container: draw straight onto the window. */
static unsigned long repaint_direct(Display *dpy, int screen, Window win,
                                    int W, int H, const Seg *s, int K,
                                    int sync_every) {
    unsigned long r0 = NextRequest(dpy);
    cairo_surface_t *xs = cairo_xlib_surface_create(
        dpy, win, DefaultVisual(dpy, screen), W, H);
    cairo_t *cr = cairo_create(xs);
    draw_content(cr, s, K, dpy, sync_every);
    cairo_destroy(cr);
    cairo_surface_destroy(xs);
    XSync(dpy, False);                     /* force the server to finish */
    return NextRequest(dpy) - r0;
}

/* Patched _repaint_container: compose off-screen, blit once. */
static unsigned long repaint_buffered(Display *dpy, int screen, Window win,
                                      int W, int H, const Seg *s, int K) {
    unsigned long r0 = NextRequest(dpy);
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    cairo_t *cr = cairo_create(img);
    draw_content(cr, s, K, NULL, 0);       /* all client-side: no X requests */
    cairo_destroy(cr);

    cairo_surface_t *xs = cairo_xlib_surface_create(
        dpy, win, DefaultVisual(dpy, screen), W, H);
    cairo_t *xcr = cairo_create(xs);
    cairo_set_source_surface(xcr, img, 0, 0);
    cairo_paint(xcr);                      /* one composite */
    cairo_destroy(xcr);
    cairo_surface_flush(xs);
    cairo_surface_destroy(xs);
    cairo_surface_destroy(img);
    XSync(dpy, False);
    return NextRequest(dpy) - r0;
}

typedef struct { double ms_mean, ms_min; double req_mean; } Stats;

static Stats run_mode(int buffered, Display *dpy, int screen, Window win,
                      int W, int H, const Seg *s, int K, int frames) {
    /* warm up (first frame allocates X-side resources) */
    for (int i = 0; i < 3; i++)
        buffered ? repaint_buffered(dpy, screen, win, W, H, s, K)
                 : repaint_direct(dpy, screen, win, W, H, s, K, 0);

    double sum = 0, mn = 1e30, reqsum = 0;
    for (int i = 0; i < frames; i++) {
        double t0 = now_ms();
        unsigned long req = buffered
            ? repaint_buffered(dpy, screen, win, W, H, s, K)
            : repaint_direct(dpy, screen, win, W, H, s, K, 0);
        double dt = now_ms() - t0;
        sum += dt; if (dt < mn) mn = dt; reqsum += (double)req;
    }
    Stats st = { sum / frames, mn, reqsum / frames };
    return st;
}

static void coalesce_demo(int burst) {
    printf("\n--- coalesce (CMD_3D_FRAME -> repaint) : unit demo, no X --------\n");
    printf("  3D frames delivered in one main-loop drain : %d\n", burst);
    printf("  repaints, unpatched (repaint per frame)    : %d\n", burst);
    printf("  repaints, patched   (flag + one per drain) : %d\n", 1);
    printf("  => window-update work per drain scales x%d -> x1\n", burst);
}

int main(int argc, char **argv) {
    int K = 40000, frames = 60, watch = 0, W = 720, H = 720, burst = 8;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--strokes") && i + 1 < argc) K = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames")  && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--burst")   && i + 1 < argc) burst = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--watch"))                   watch = 1;
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr,
            "cannot open X display (set DISPLAY; on the VM this is XWayland).\n"
            "Running the no-X coalesce demo only.\n");
        coalesce_demo(burst);
        return 0;
    }
    int screen = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(
        dpy, RootWindow(dpy, screen), 0, 0, (unsigned)W, (unsigned)H, 0,
        BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XStoreName(dpy, win, "bench_xlib_repaint");
    XMapRaised(dpy, win);
    XSync(dpy, False);

    Seg *frame = make_frame(K, W, H, 12345u);

    printf("frame: %d segments, window %dx%d, %d timed frames/mode\n",
           K, W, H, frames);

    if (watch) {
        printf("\n--watch: DIRECT draws with periodic sync so you can see the\n"
               "bottom-to-top sweep; BUFFERED snaps in one blit. 3 rounds each.\n");
        for (int r = 0; r < 3; r++) {
            repaint_direct(dpy, screen, win, W, H, frame, K, 2000);
            struct timespec ts = { 0, 400 * 1000 * 1000 }; nanosleep(&ts, NULL);
        }
        for (int r = 0; r < 3; r++) {
            repaint_buffered(dpy, screen, win, W, H, frame, K);
            struct timespec ts = { 0, 400 * 1000 * 1000 }; nanosleep(&ts, NULL);
        }
    }

    Stats d = run_mode(0, dpy, screen, win, W, H, frame, K, frames);
    Stats b = run_mode(1, dpy, screen, win, W, H, frame, K, frames);

    printf("\n%-10s %12s %12s %14s %12s\n",
           "mode", "ms/frame", "min ms", "X reqs/frame", "max fps");
    printf("%-10s %12.2f %12.2f %14.0f %12.0f\n",
           "DIRECT",   d.ms_mean, d.ms_min, d.req_mean, 1000.0 / d.ms_mean);
    printf("%-10s %12.2f %12.2f %14.0f %12.0f\n",
           "BUFFERED", b.ms_mean, b.ms_min, b.req_mean, 1000.0 / b.ms_mean);
    printf("\nper-frame speedup : %.1fx   |   X-request reduction : %.0fx\n",
           d.ms_mean / (b.ms_mean > 0 ? b.ms_mean : 1e-9),
           b.req_mean > 0 ? d.req_mean / b.req_mean : 0.0);
    printf("at a 60fps client stream, DIRECT %s keep up (needs <=16.7 ms);\n"
           "when it can't, the main thread never services close/resize -> the\n"
           "window can't be closed and the frame sweeps.\n",
           d.ms_mean <= 16.7 ? "can" : "canNOT");

    coalesce_demo(burst);

    free(frame);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
