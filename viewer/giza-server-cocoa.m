/* viewer/giza-server-cocoa.m
 *
 * giza_server - macOS Cocoa backend
 *
 * Replaces giza-server-gtk.c for macOS builds.  The GSP wire protocol
 * (giza-server-protocol.h) is 100% identical; only the windowing layer
 * changes.
 *
 * Architecture:
 *   main thread      - NSApp run loop (set up in giza-server-main.m)
 *   worker thread    - accept() loop, per-connection threads (POSIX)
 *   UI updates       - dispatch_async(dispatch_get_main_queue(), ^{})
 *                      (replaces GTK's g_idle_add)
 *
 * Key design choices (inherited from giza PR #86 experience):
 *   - NSApp MUST run on the main thread - enforced by giza-server-main.m
 *   - All NSWindow/NSView calls dispatched to main queue
 *   - PNG decoded to NSImage via NSData (no temp files)
 *   - Letterbox scaling in drawRect:  (same maths as GTK on_draw())
 *   - Window tabbing via NSWindowTabbingIdentifier (matches PDL::Graphics::Cairo)
 *   - Max 64 windows (matches GTK backend constant)
 *
 * Window lifecycle (persistence):
 *   A window survives its client exiting (that is the whole point, like
 *   pgxwin_server).  But the user closing a window (red button) IS an
 *   explicit "discard": windowWillClose: marks the slot dead (alive=0),
 *   nils window/view, frees last_png.  Slots are never compacted - the
 *   array index is the win_id, so closed slots stay as holes and every
 *   reader checks `alive`.  Closing also shutdown()s the client socket when
 *   it was that window's last live tab, so an interactive client
 *   (show_interactive) blocked reading the socket gets EOF and its run loop
 *   returns - it no longer waits for Cmd-Q.
 *
 * File menu (Save):
 *   - Save as PNG  - viewer-side, lossless (writes the received PNG bytes
 *     kept in last_png).  Works for any client (C, Perl/Driver::GS), and
 *     even after the client has exited, since the bytes are held locally.
 *   - Save as PDF / SVG - true vector output via reverse channel: the
 *     viewer sends SAVEREQ to the still-running client, which re-renders
 *     the current figure to PDF/SVG and returns the bytes as SAVEDATA
 *     (see _vectorSave: / _save_vector_for_fd).  If the client has already
 *     exited there is nobody to re-render, so these items gray out
 *     (validateMenuItem: checks client_fd); the _vectorSaveExited: dialog
 *     remains only as a defensive fallback for a client that dies between
 *     validation and the action firing.
 *   - All Save items also gray out when no live giza window exists, so
 *     "selectable but silent" never happens.
 *
 * Reusable lessons (persistent-window GUI, apply if Driver::OSX /
 * pdlcairo_viewer ever grows a Save dialog):
 *   1. Close all three "gone" states separately: client exited, user
 *      closed the window, and stale slot. Guard every reader with
 *      alive + window/view identity, or freed objects get touched.
 *   2. Never leave a menu item "selectable but silent" - use
 *      validateMenuItem: to gray it out when the action is meaningless.
 *   3. keyWindow can be nil; fall back keyWindow -> mainWindow ->
 *      orderedWindows when resolving the front window.
 *   4. UI work queued with dispatch_async may run after state changed,
 *      so re-check liveness inside the block before using captured refs.
 *
 * Copyright (c) 2026 goosh-gh - LGPL-2.1 (same as giza)
 *
 * --- Phase 2 (2026-06-20): 3D viewer support added ---
 * GsWindow gained an is_3d flag, latched true on the first
 * GSP_MSG_3D_FRAME received for a window. GsView gained a parallel draw
 * path (_draw3DFrame) and mouse handling for rotate-drag (sends
 * GSP_MSG_3D_INPUT) instead of the 2D pan/zoom logic. See the "Phase 2"
 * comments scattered through this file for the specific additions; the
 * original 2D PNG-viewer logic above this notice is unchanged.
 */

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
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
#include "gsp_3d.h"   /* Phase 2: 3D message types (0x24-0x27) */
static void _slider_send(int win_id, uint8_t slider_id, float value);
static void _savereq_send(int win_id, uint8_t fmt);
static void _resize_send(int win_id, uint32_t width_px, uint32_t height_px);
static void _cursor_send(int win_id, float fx, float fy, uint8_t buttons, uint8_t type);
static void _zoom_send(int win_id, float zoom, float pan_x, float pan_y);
static void _3d_input_send(int win_id, float dx, float dy, float dzoom,
                           uint8_t type, uint8_t key);   /* Phase 2 */

/* Live-drag fires windowDidResize: continuously; coalesce the burst and
 * emit one RESIZE this long after the last event (drag settled). Each
 * RESIZE triggers a full client re-render + PNG round-trip, so we never
 * want to fire per intermediate frame. */
#define GS_RESIZE_DEBOUNCE_SEC 0.06

/* ------------------------------------------------------------------ */
/* GsView - custom NSView that letterbox-scales the current NSImage    */
/* ------------------------------------------------------------------ */

@interface GsView : NSView {
@public
    NSImage  *_image;   /* current plot frame          */
    NSLock   *_lock;
    int       win_id;   /* which window this view belongs to          */

    /* Zoom / pan state (main thread only) */
    double    _zoom;    /* 1.0 = fit-to-window, >1 = magnified        */
    double    _pan_x;   /* fractional offset: 0 = centred             */
    double    _pan_y;

    /* Pan drag state */
    NSPoint   _drag_start;  /* view-coords of mouseDown start point   */
    double    _pan_x_start;
    double    _pan_y_start;

    /* Cursor label (overlay in top-right of plot area) */
    NSTextField *_coord_label;

    /* --- Phase 2: 3D mode ---
     * is_3d is latched true on the first GSP_MSG_3D_FRAME received for
     * this window and never reset, so a window is either a 2D PNG
     * viewer or a 3D primitive viewer for its whole lifetime - matching
     * how Driver::GS3D opens a dedicated connection per 3D scene rather
     * than switching an existing one. _3d_lock guards _3d_payload only
     * (PNG path's _lock is untouched).
     *
     * REVIEW NOTE (2026-06-21): there used to be a separate _3d_labels
     * NSMutableArray here, populated by per-message add3DLabel: calls.
     * That design caused the label flicker described at set3DPayload:/
     * _draw3DFrame - labels are now embedded in _3d_payload itself, so
     * no separate label storage or clear/add methods are needed. */
    int       _is_3d;
    NSLock   *_3d_lock;
    NSData   *_3d_payload;       /* raw GSP_MSG_3D_FRAME payload (lines+points+labels), parsed in drawRect: */
    NSPoint   _rot_drag_start;   /* mouseDown point, 3D-rotate drag (separate from 2D _drag_start) */
}
- (void)setImage:(NSImage *)img;
- (void)sliderChanged:(NSSlider *)sender;
/* Compute the letterbox destination rect for the current zoom/pan state.
 * Helper used by drawRect: and the view→image coordinate converter. */
- (NSRect)_plotDstRect;
/* Convert a view point (Cocoa bottom-left origin) to image fractions.
 * Returns YES if the point is within the image area. */
- (BOOL)_viewPoint:(NSPoint)pt toImageFx:(double *)fx fy:(double *)fy;
/* --- Phase 2: 3D mode --- */
- (void)set3DPayload:(NSData *)payload;
@end



/* ------------------------------------------------------------------ */
/* NSApplicationDelegate (+ NSWindowDelegate) - termination & close   */
/* ------------------------------------------------------------------ */

@interface GsAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation GsAppDelegate


- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;
    [[NSProcessInfo processInfo]
        disableAutomaticTermination:@"giza_server running"];
    [[NSProcessInfo processInfo]
        disableSuddenTermination];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    (void)sender;
    return NSTerminateNow;
}

/* windowWillClose: / validateMenuItem: are implemented in the FileSave
 * category below, where GsWindow / GS are finally in scope. */

@end

@implementation GsView

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
        _lock   = [[NSLock alloc] init];
        _image  = nil;
        win_id  = -1;
        _zoom   = 1.0;
        _pan_x  = 0.0;
        _pan_y  = 0.0;

        /* --- Phase 2: 3D mode --- */
        _is_3d       = 0;
        _3d_lock     = [[NSLock alloc] init];
        _3d_payload  = nil;

        /* Coordinate readout label (top-right corner, hidden until cursor enters) */
        _coord_label = [[NSTextField alloc] initWithFrame:NSMakeRect(0,0,180,20)];
        [_coord_label setEditable:NO];
        [_coord_label setSelectable:NO];
        [_coord_label setBordered:NO];
        [_coord_label setDrawsBackground:YES];
        [_coord_label setBackgroundColor:
            [NSColor colorWithCalibratedWhite:1.0 alpha:0.75]];
        [_coord_label setFont:
            [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular]];
        [_coord_label setTextColor:[NSColor darkGrayColor]];
        [_coord_label setAlignment:NSTextAlignmentRight];
        [_coord_label setHidden:YES];
        [self addSubview:_coord_label];

        /* Enable mouse-move events (needed for cursor tracking) */
        NSTrackingAreaOptions opts =
            NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
            NSTrackingActiveInActiveApp | NSTrackingInVisibleRect;
        NSTrackingArea *ta = [[NSTrackingArea alloc]
            initWithRect:NSZeroRect options:opts owner:self userInfo:nil];
        [self addTrackingArea:ta];
        [ta release];
    }
    return self;
}

- (void)dealloc
{
    [_image release];
    [_lock  release];
    [_coord_label release];
    [_3d_lock release];
    [_3d_payload release];
    [super dealloc];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *ta in [self trackingAreas])
        [self removeTrackingArea:ta];
    NSTrackingAreaOptions opts =
        NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
        NSTrackingActiveInActiveApp | NSTrackingInVisibleRect;
    NSTrackingArea *ta = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect options:opts owner:self userInfo:nil];
    [self addTrackingArea:ta];
    [ta release];
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

- (void)sliderChanged:(NSSlider *)sender
{
    _slider_send(self->win_id, (uint8_t)[sender tag], (float)[sender doubleValue]);
}

/* --- Phase 2: 3D mode ---
 * Mirrors setImage:'s lock-copy-async pattern, but for the raw
 * GSP_MSG_3D_FRAME payload instead of a decoded NSImage. Parsing
 * (splitting into gsp_3d_line_t / gsp_3d_point_t records) happens in
 * drawRect:/_draw3DFrame rather than here, matching how _project()/
 * depth-sort already happen server(Perl)-side - this method only owns
 * the bytes.
 *
 * FIXED (2026-06-20, persistent flicker): unlike setImage: (which is
 * called directly from the reader thread and genuinely needs its own
 * dispatch_async to reach the main thread), this method is only ever
 * called from _update_window_3d's dispatch_async block - i.e. it is
 * already running on the main thread by the time it gets here. The
 * extra inner dispatch_async this used to have was a redundant re-hop:
 * every frame got queued an extra tick behind the run loop, decoupling
 * setNeedsDisplay: timing from the actual frame arrival and from each
 * other when frames arrived faster than the queue drained - a likely
 * contributor to the flicker that frame-rate limiting in GS3D.pm's
 * run() loop did not fix on its own. setNeedsDisplay: is now called
 * synchronously, in place, matching how Driver::GS3D actually delivers
 * one frame at a time. */
- (void)set3DPayload:(NSData *)payload
{
    [_3d_lock lock];
    [_3d_payload release];
    _3d_payload = [payload retain];
    _is_3d = 1;
    [_3d_lock unlock];
    [self setNeedsDisplay:YES];
}

/* ---- coordinate helpers ------------------------------------------- */

/* Return the destination NSRect where the image is drawn (bottom-left
 * Cocoa coords), accounting for letterbox + zoom + pan. */
- (NSRect)_plotDstRect
{
    [_lock lock];
    NSImage *img = [_image retain];
    [_lock unlock];
    if (!img) return NSZeroRect;

    NSSize  isize  = img.size;
    [img release];

    NSRect  bounds = self.bounds;
    double  sx     = bounds.size.width  / isize.width;
    double  sy     = bounds.size.height / isize.height;
    double  scale  = (sx < sy) ? sx : sy;
    double  dw     = isize.width  * scale * _zoom;
    double  dh     = isize.height * scale * _zoom;
    /* pan offsets are fractions of the *zoomed* image size */
    double  ox     = (bounds.size.width  - dw) / 2.0 + _pan_x * dw;
    double  oy     = (bounds.size.height - dh) / 2.0 + _pan_y * dh;
    return NSMakeRect(ox, oy, dw, dh);
}

/* Convert view point (Cocoa bottom-left) → image fraction [0,1]×[0,1].
 * (0,0) = top-left of image, (1,1) = bottom-right (top-down convention
 * for the client, matching Cairo image surface origin). */
- (BOOL)_viewPoint:(NSPoint)pt toImageFx:(double *)fx fy:(double *)fy
{
    NSRect dst = [self _plotDstRect];
    if (dst.size.width == 0 || dst.size.height == 0) return NO;
    double rx = (pt.x - dst.origin.x) / dst.size.width;
    /* Cocoa y: up; image y: down — flip */
    double ry = 1.0 - (pt.y - dst.origin.y) / dst.size.height;
    if (rx < 0.0 || rx > 1.0 || ry < 0.0 || ry > 1.0) return NO;
    *fx = rx;
    *fy = ry;
    return YES;
}

/* ---- drawRect: ------------------------------------------------------- */

- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;

    /* --- Phase 2: 3D mode ---
     * Completely different draw path, no PNG/letterbox involved.
     * Checked first and returns early so the 2D NSImage path below
     * never runs for a 3D window. */
    if (_is_3d) {
        [self _draw3DFrame];
        return;
    }

    /* white background */
    [[NSColor whiteColor] setFill];
    NSRectFill(self.bounds);

    [_lock lock];
    NSImage *img = [_image retain];
    [_lock unlock];

    if (!img) return;

    NSRect dst = [self _plotDstRect];
    [img drawInRect:dst
           fromRect:NSZeroRect
          operation:NSCompositingOperationSourceOver
           fraction:1.0];

    [img release];

    /* Keep coord label in top-right of actual plot area */
    if (![_coord_label isHidden]) {
        NSRect lb  = [_coord_label frame];
        double margin = 6.0;
        [_coord_label setFrameOrigin:NSMakePoint(
            dst.origin.x + dst.size.width - lb.size.width - margin,
            dst.origin.y + dst.size.height - lb.size.height - margin)];
    }
}

/* --- Phase 2: 3D mode draw routine -------------------------------------
 *
 * Layout of the raw payload (see gsp_3d.h, numbers 0x24-0x27 after the
 * collision fix with GSP_MSG_CLOSE/ACK):
 *   gsp_3d_frame_hdr_t (13 bytes): n_lines(u16) n_points(u16) flags(u8)
 *                                  cx(f32) cy(f32)
 *   then n_lines * gsp_3d_line_t  (24 bytes each)
 *   then n_points * gsp_3d_point_t (20 bytes each)
 *
 * Records arrive already depth-sorted back-to-front by Driver::GS3D
 * (_send_3d_frame's $order = $depth->qsorti loop), so this method must
 * NOT re-sort - it just strokes/fills in array order, preserving the
 * painter's-algorithm ordering computed server-side.
 *
 * Y-axis: Driver::GS3D's _project() computes screen Y as
 * "data_y * scale + cy" with cy = height/2, i.e. a top-down image
 * convention (Y increases downward), same as the 2D PNG path's image
 * coordinates. GsView's isFlipped is NO (Cocoa default, Y increases
 * upward), so each Y coordinate is flipped here (view_h - y), mirroring
 * how _viewPoint:toImageFx:fy: already flips Y for the 2D cursor path
 * elsewhere in this class.
 *
 * UNCONFIRMED (flag for visual check once a 3D client is running):
 * whether this flip direction actually matches what looks "upright" -
 * if labels/axes appear upside-down, this is the first place to check. */
- (void)_draw3DFrame
{
    [_3d_lock lock];
    NSData *payload = [_3d_payload retain];
    [_3d_lock unlock];

    NSRect bounds = self.bounds;
    float view_h  = (float)bounds.size.height;

    /* dark background, distinct from the 2D viewer's white, so a 3D
     * window is visually unmistakable even before any frame arrives */
    [[NSColor colorWithCalibratedRed:0.08 green:0.08 blue:0.12 alpha:1.0] setFill];
    NSRectFill(bounds);

    if (!payload || [payload length] < sizeof(gsp_3d_frame_hdr_t)) {
        [payload release];
        return;
    }

    const uint8_t *buf = (const uint8_t *)[payload bytes];
    size_t buflen = (size_t)[payload length];
    gsp_3d_frame_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    const uint8_t *p = buf + sizeof(hdr);

    size_t need = sizeof(hdr)
                + (size_t)hdr.n_lines  * sizeof(gsp_3d_line_t)
                + (size_t)hdr.n_points * sizeof(gsp_3d_point_t);

    if (buflen < need) {
        /* truncated/corrupt payload: draw nothing rather than read OOB */
        [payload release];
        return;
    }

    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];

    /* ---- lines (already back-to-front sorted by the server) ---- */
    for (uint16_t i = 0; i < hdr.n_lines; i++) {
        gsp_3d_line_t ln;
        memcpy(&ln, p, sizeof(ln));
        p += sizeof(ln);

        CGContextSetRGBStrokeColor(ctx,
            ln.r / 255.0f, ln.g / 255.0f, ln.b / 255.0f, ln.a / 255.0f);
        CGContextSetLineWidth(ctx, 1.0f);
        CGContextMoveToPoint(ctx, ln.x0, view_h - ln.y0);
        CGContextAddLineToPoint(ctx, ln.x1, view_h - ln.y1);
        CGContextStrokePath(ctx);
    }

    /* ---- points ---- */
    for (uint16_t i = 0; i < hdr.n_points; i++) {
        gsp_3d_point_t pt;
        memcpy(&pt, p, sizeof(pt));
        p += sizeof(pt);

        float r = pt.size;
        CGRect circle = CGRectMake(pt.x - r, view_h - pt.y - r, 2*r, 2*r);
        CGContextSetRGBFillColor(ctx,
            pt.r / 255.0f, pt.g / 255.0f, pt.b / 255.0f, pt.a / 255.0f);
        CGContextFillEllipseInRect(ctx, circle);
    }

    /* --- Phase 2 (2026-06-21, label flicker fix): labels are parsed
     * straight out of this same payload buffer, right after the point
     * records - they are no longer a separate NSMutableArray populated
     * by independent add3DLabel: calls. This is what makes the whole
     * frame (lines+points+labels) a single atomic snapshot: payload is
     * one NSData swapped in by exactly one set3DPayload: call, so there
     * is no "halfway through adding labels" state for drawRect: to
     * observe anymore (which was the actual cause of the flicker -
     * labels.count was seen jumping 0,1,2,...,20 between frames).
     *
     * Layout (see gsp_3d.h): uint16_t n_labels, then n_labels records of
     * (point_idx u16, x f32, y f32, r,g,b u8, len u8, text[len]). Bounds-
     * checked defensively per-record since text length is attacker/bug
     * controlled data coming off the wire - a malformed length must not
     * walk `p` past the end of `buf`. */
    if (buflen >= need + sizeof(uint16_t)) {
        uint16_t n_labels;
        memcpy(&n_labels, p, sizeof(n_labels));
        p += sizeof(n_labels);

        const uint8_t *end = buf + buflen;
        for (uint16_t i = 0; i < n_labels; i++) {
            /* fixed part: point_idx(u16) x(f32) y(f32) r,g,b(u8) len(u8) = 14 bytes */
            if (p + 14 > end) break;   /* truncated: stop drawing labels, not a crash */

            uint16_t point_idx; float lx, ly; uint8_t r, g, b, len;
            memcpy(&point_idx, p,      2);
            memcpy(&lx,        p + 2,  4);
            memcpy(&ly,        p + 6,  4);
            r   = p[10];
            g   = p[11];
            b   = p[12];
            len = p[13];
            p += 14;

            if (p + len > end) break;  /* truncated text: stop here */

            NSString *text = [[NSString alloc]
                initWithBytes:p length:len encoding:NSUTF8StringEncoding];
            p += len;

            if (!text) continue;   /* invalid UTF-8: skip this label, keep going */

            float draw_x = lx;
            float draw_y = view_h - ly;
            NSColor *color = [NSColor colorWithCalibratedRed:r/255.0f
                                                         green:g/255.0f
                                                          blue:b/255.0f
                                                         alpha:1.0f];
            NSDictionary *attrs = @{
                NSFontAttributeName: [NSFont systemFontOfSize:11],
                NSForegroundColorAttributeName: color,
            };
            [text drawAtPoint:NSMakePoint(draw_x, draw_y) withAttributes:attrs];
            [text release];
        }
    }

    [payload release];
}

/* ---- zoom / pan event handlers -------------------------------------- */

/* Accept first-responder so keyboard/scroll events reach us */
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder  { return YES; }

- (void)magnifyWithEvent:(NSEvent *)event
{
    /* Pinch gesture or Ctrl+two-finger-scroll on a trackpad */
    _zoom *= (1.0 + event.magnification);
    if (_zoom < 1.0) { _zoom = 1.0; _pan_x = 0.0; _pan_y = 0.0; }
    if (_zoom > 40.0) _zoom = 40.0;
    _zoom_send(self->win_id, (float)_zoom, (float)_pan_x, (float)_pan_y);
    [self setNeedsDisplay:YES];
}

- (void)scrollWheel:(NSEvent *)event
{
    if (event.modifierFlags & NSEventModifierFlagControl) {
        /* Ctrl + scroll = zoom */
        double delta = event.scrollingDeltaY * 0.05;
        _zoom *= (1.0 + delta);
        if (_zoom < 1.0) { _zoom = 1.0; _pan_x = 0.0; _pan_y = 0.0; }
        if (_zoom > 40.0) _zoom = 40.0;
    } else if (_zoom > 1.0) {
        /* Two-finger pan (only when zoomed) */
        NSRect dst = [self _plotDstRect];
        if (dst.size.width > 0) {
            _pan_x += event.scrollingDeltaX / dst.size.width;
            _pan_y -= event.scrollingDeltaY / dst.size.height; /* Cocoa y-flip */
            /* clamp: don't pan so far the image leaves the viewport */
            double max_px = 0.5 * (1.0 - 1.0/_zoom);
            if (_pan_x < -max_px) _pan_x = -max_px;
            if (_pan_x >  max_px) _pan_x =  max_px;
            if (_pan_y < -max_px) _pan_y = -max_px;
            if (_pan_y >  max_px) _pan_y =  max_px;
        }
    }
    _zoom_send(self->win_id, (float)_zoom, (float)_pan_x, (float)_pan_y);
    [self setNeedsDisplay:YES];
}

/* Double-click resets zoom/pan */
- (void)mouseDown:(NSEvent *)event
{
    /* --- Phase 2: 3D mode ---
     * Rotate-drag start. Double-click reset and the 2D pan/PICK logic
     * below don't apply in 3D mode ('r'-key reset is handled by
     * Driver::GS3D's run() loop on the 'r' key input, not here).
     *
     * makeFirstResponder: explicit here (rather than relying on
     * NSWindow's implicit click-to-first-responder behavior) because
     * this override never calls [super mouseDown:event], and p/r keys
     * were observed not reaching keyDown: in testing even after a
     * click - first-responder status not being reliably granted is the
     * leading suspect. */
    if (_is_3d) {
        [[self window] makeFirstResponder:self];
        _rot_drag_start = [self convertPoint:[event locationInWindow] fromView:nil];
        return;
    }
    if (event.clickCount == 2) {
        _zoom  = 1.0; _pan_x = 0.0; _pan_y = 0.0;
        _zoom_send(self->win_id, 1.0f, 0.0f, 0.0f);
        [self setNeedsDisplay:YES];
        return;
    }
    /* Begin drag-pan */
    _drag_start    = [self convertPoint:[event locationInWindow] fromView:nil];
    _pan_x_start   = _pan_x;
    _pan_y_start   = _pan_y;

    /* Also send PICK event if inside plot */
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    double fx, fy;
    if ([self _viewPoint:pt toImageFx:&fx fy:&fy]) {
        uint8_t btn = (uint8_t)([event buttonNumber] + 1);
        _cursor_send(self->win_id, (float)fx, (float)fy, btn, GSP_MSG_PICK);
    }
}

- (void)mouseDragged:(NSEvent *)event
{
    /* --- Phase 2: 3D mode (トラックボール回転, 2026-06-21更新) ---
     * 旧実装はここでドラッグ差分を「角度」(dtheta/dphi)に変換して
     * 送っていたが、Perl側(GS3D.pm)がオイラー角2軸合成から回転行列
     * 累積方式(トラックボール回転)に変わったため、Cocoa側はもう角度を
     * 計算しない。生のドラッグ差分(画面px)をそのままGSP_MSG_3D_INPUTの
     * dx/dyフィールドに乗せて送るだけにする - 軸の割り当て・感度係数の
     * 計算は全てPerl側(_apply_drag_rotation)の責任にする。
     *
     * Y軸の向き(UNCONFIRMED): Cocoaのビュー座標はY-up(isFlipped==NO)。
     * Perl側_apply_drag_rotationは「dy>0(上にドラッグ)→X軸回転+方向」
     * という素朴な対応にしている。これが「上にドラッグ→奥が持ち上がる/
     * 頭頂が手前に来る」という直感と一致するかは実機で確認が必要 -
     * 違っていたらここでdyの符号を反転するのが直しやすい場所。 */
    if (_is_3d) {
        NSPoint cur = [self convertPoint:[event locationInWindow] fromView:nil];
        float dx = (float)(cur.x - _rot_drag_start.x);
        float dy = (float)(cur.y - _rot_drag_start.y);
        _3d_input_send(self->win_id, dx, dy, 0.0f, /*type=drag*/0, /*key*/0);
        _rot_drag_start = cur;
        return;
    }
    if (_zoom <= 1.0) return;   /* no pan when not zoomed */
    NSPoint cur = [self convertPoint:[event locationInWindow] fromView:nil];
    NSRect  dst = [self _plotDstRect];
    if (dst.size.width == 0) return;
    _pan_x = _pan_x_start + (cur.x - _drag_start.x) / dst.size.width;
    _pan_y = _pan_y_start + (cur.y - _drag_start.y) / dst.size.height;
    double max_px = 0.5 * (1.0 - 1.0/_zoom);
    if (_pan_x < -max_px) _pan_x = -max_px;
    if (_pan_x >  max_px) _pan_x =  max_px;
    if (_pan_y < -max_px) _pan_y = -max_px;
    if (_pan_y >  max_px) _pan_y =  max_px;
    [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)event
{
    (void)event;
    if (_zoom > 1.0)
        _zoom_send(self->win_id, (float)_zoom, (float)_pan_x, (float)_pan_y);
}

/* ---- cursor (mouse-move) tracking ----------------------------------- */

- (void)mouseMoved:(NSEvent *)event
{
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    double fx, fy;
    BOOL inside = [self _viewPoint:pt toImageFx:&fx fy:&fy];
    [_coord_label setHidden:!inside];
    if (inside) {
        /* Update label text.  (0,0)=top-left per image convention.  */
        NSString *s = [NSString stringWithFormat:@" x=%.4f  y=%.4f ", fx, fy];
        [_coord_label setStringValue:s];
        /* Resize label to fit */
        [_coord_label sizeToFit];
        [self setNeedsDisplay:YES];
        _cursor_send(self->win_id, (float)fx, (float)fy, 0, GSP_MSG_CURSOR);
    }
}

- (void)mouseEntered:(NSEvent *)event { (void)event; }
- (void)mouseExited:(NSEvent *)event
{
    (void)event;
    [_coord_label setHidden:YES];
    [self setNeedsDisplay:YES];
}

/* --- Phase 2: 3D mode ---
 * CONFIRMED MISSING (2026-06-20): the original 2D GsView never
 * implemented keyDown: at all - acceptsFirstResponder/
 * becomeFirstResponder return YES (so key events do reach this view),
 * but with no keyDown: override, NSView's default just beeps/discards
 * them. This is why pressing 'p'/'r' had no effect when tested - there
 * was no code path sending GSP_MSG_3D_INPUT for a key event, unlike
 * mouseDragged: which already had one. Only handled in 3D mode; 2D mode
 * has no keyboard interaction so falls through to the default (which
 * matches its prior - silent - behavior). */
- (void)keyDown:(NSEvent *)event
{
    if (!_is_3d) { [super keyDown:event]; return; }

    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 0) { [super keyDown:event]; return; }
    unichar c = [chars characterAtIndex:0];

    /* Only 'p'/'P' (toggle perspective) and 'r'/'R' (reset view) are
     * meaningful to Driver::GS3D::run() (see its type==2 branch), so
     * only those are forwarded - anything else falls through to the
     * default responder-chain handling rather than being silently sent
     * as a no-op 3D_INPUT. */
    if (c == 'p' || c == 'P' || c == 'r' || c == 'R') {
        _3d_input_send(self->win_id, 0.0f, 0.0f, 0.0f, /*type=key*/2, (uint8_t)c);
        return;
    }
    [super keyDown:event];
}

- (BOOL)isFlipped { return NO; }   /* Cocoa default: origin bottom-left */

@end /* GsView */


/* ------------------------------------------------------------------ */
/* Per-window state                                                    */
/* ------------------------------------------------------------------ */

#define MAX_WINDOWS 64

typedef struct {
    int        id;
    NSWindow  *window;    /* retained - only touch from main thread   */
    GsView    *view;      /* retained - setImage: is thread-safe       */
    char       title[256];
    int        alive;
    int        client_fd;         /* socket back to client (-1 if none) */
    uint32_t   group_id;          /* tab group (client PID); 0 = ungrouped */
    uint32_t   seq_out;           /* seq for SLIDER messages we send    */
    pthread_mutex_t write_lock;   /* serialize writes to client_fd      */
    uint32_t   last_sent_w;       /* last RESIZE px emitted (suppress dup/no-op moves) */
    uint32_t   last_sent_h;       /*   touched on main thread only       */
    NSData         *last_png;     /* latest page PNG (owned copy, for saving) */
    char           *save_path;    /* pending vector-save destination (malloc'd, NULL if none) */
    uint8_t         save_fmt;     /* GSP_SAVE_FMT_* for the pending save  */
    int             is_3d;        /* Phase 2: latched 1 on first GSP_MSG_3D_FRAME */
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
/* GsAppDelegate (FileSave) - File menu actions, validation, close     */
/*                                                                     */
/* Why here: GsAppDelegate's main @implementation sits near the top of    */
/* the file, where GsWindow / GS are not yet declared. These methods      */
/* reference both, so they are added as a category at this point, after   */
/* both are defined.                                                      */
/* ------------------------------------------------------------------ */

@interface GsAppDelegate (FileSave)
- (GsView *)_frontGsView;
- (void)savePNG:(id)sender;
- (void)savePDF:(id)sender;
- (void)saveSVG:(id)sender;
- (void)_vectorSave:(NSString *)fmt;
- (void)_vectorSaveExited:(NSString *)fmt view:(GsView *)view;
- (void)_emitResize:(NSNumber *)boxedWinId;
@end

@implementation GsAppDelegate (FileSave)

/* The window lays a GsView and the h/v sliders side by side directly
 * under contentView, so contentView itself is not the GsView. Small
 * helper that walks subviews to find it. */
static GsView *_gsview_in(NSWindow *win)
{
    if (!win) return nil;
    if ([[win contentView] isKindOfClass:[GsView class]])
        return (GsView *)[win contentView];
    for (NSView *v in [[win contentView] subviews])
        if ([v isKindOfClass:[GsView class]]) return (GsView *)v;
    return nil;
}

/* Is this GsView backed by a live GS.wins[] slot? Rejects windows the
 * user closed via the red button (view already niled) and stale refs. */
static BOOL _gsview_is_live(GsView *v)
{
    if (!v) return NO;
    int id = v->win_id;
    if (id < 0 || id >= GS.n_wins) return NO;
    GsWindow *w = &GS.wins[id];
    return (w->alive && w->view == v);
}

/* Return the frontmost LIVE GsView (nil if none).
 * Falls back in stages so it works even when keyWindow is nil - e.g.
 * right after the client died or a window closed and focus moved away;
 * resolves the front-most still-open giza window. */
- (GsView *)_frontGsView
{
    NSWindow *w = [NSApp keyWindow];
    if (w) { GsView *v = _gsview_in(w); if (_gsview_is_live(v)) return v; }

    w = [NSApp mainWindow];
    if (w) { GsView *v = _gsview_in(w); if (_gsview_is_live(v)) return v; }

    /* Front-to-back order. First live GsView wins. */
    for (NSWindow *win in [NSApp orderedWindows]) {
        if (![win isVisible]) continue;
        GsView *v = _gsview_in(win);
        if (_gsview_is_live(v)) return v;
    }
    return nil;
}

/* Menu item enable/disable. PNG and the two vector items have different
 * requirements, so they are validated separately:
 *
 *   Save as PNG  - served from the retained last_png buffer, so it works
 *                  for any live window, even one whose client has exited
 *                  (a plain show() window after its script finished). Enabled
 *                  whenever the front window has a frame buffered.
 *   Save as PDF/SVG - true vector output needs the client alive to re-render
 *                  over the reverse channel. For a client-absent window there
 *                  is nobody to render, so these gray out (rather than being
 *                  pressable only to raise the "program has exited" dialog).
 *
 * All three still gray out when no live giza window exists at all. Reads of
 * last_png / client_fd here are on the main thread; last_png is only mutated
 * on the main thread, and a stale client_fd read is harmless because
 * _vectorSave: re-checks it under write_lock before sending SAVEREQ. */
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    SEL a = [item action];

    if (a == @selector(savePNG:)) {
        GsView *v = [self _frontGsView];
        return (v != nil && GS.wins[v->win_id].last_png != nil);
    }
    if (a == @selector(savePDF:) || a == @selector(saveSVG:)) {
        GsView *v = [self _frontGsView];
        return (v != nil && GS.wins[v->win_id].client_fd >= 0);
    }
    return YES;
}

/* File > Save as PNG: write the front window's latest PNG verbatim.
 * We keep an owned copy of the received bytes, so it is lossless with
 * no re-encoding. */
- (void)savePNG:(id)sender
{
    (void)sender;

    GsView *view = [self _frontGsView];
    if (!view) { NSBeep(); return; }      /* no live giza window (normally grayed out) */

    GsWindow *w   = &GS.wins[view->win_id];
    NSData   *png = w->last_png;   /* on the main thread, no lock needed */
    if (!png) { NSBeep(); return; }
    /* MRC: the completionHandler below retains png automatically when the
     * block is copied, so it stays safe even if the next frame replaces
     * last_png while the save panel is open. */

    NSString *base = (w->title[0])
        ? [NSString stringWithUTF8String:w->title]
        : @"giza_plot";
    base = [base stringByReplacingOccurrencesOfString:@"/" withString:@"-"];

    NSSavePanel *panel = [NSSavePanel savePanel];
    [panel setNameFieldStringValue:[base stringByAppendingPathExtension:@"png"]];
    if (@available(macOS 11.0, *)) {
        panel.allowedContentTypes = @[ UTTypePNG ];
    }
    /* macOS < 11: no extension filter (the default name already ends .png) */

    NSWindow *key = [view window];
    [panel beginSheetModalForWindow:key
                  completionHandler:^(NSModalResponse result) {
        if (result != NSModalResponseOK) return;
        NSURL   *url = [panel URL];
        NSError *err = nil;
        if (![png writeToURL:url options:NSDataWritingAtomic error:&err]) {
            fprintf(stderr, "giza_server: PNG save failed: %s\n",
                    err ? [[err localizedDescription] UTF8String] : "unknown");
            NSBeep();
        }
    }];
}

/* File > Save as PDF/SVG.
 *
 * The viewer only ever holds a rasterised PNG (last_png), so true vector
 * output cannot be produced viewer-side.  Instead we ask the still-running
 * client to re-render its current figure to PDF/SVG and send the bytes
 * back, mirroring the bidirectional SLIDER path:
 *
 *   1. user picks a destination in the save panel
 *   2. we remember (save_path, save_fmt) on the window and send SAVEREQ(fmt)
 *      to the client over its socket fd (serialized by write_lock)
 *   3. the per-connection reader thread later receives SAVEDATA and writes
 *      the bytes to save_path (see _save_vector_for_fd)
 *
 * If the client has already exited (client_fd < 0) there is nobody to
 * re-render, so we fall back to the guidance dialog (_vectorSaveExited).  */
- (void)_vectorSaveExited:(NSString *)fmt view:(GsView *)view
{
    NSAlert *alert = [[[NSAlert alloc] init] autorelease];
    [alert setMessageText:
        [NSString stringWithFormat:@"Cannot save as %@", fmt]];
    [alert setInformativeText:
        @"The program that created this window has exited, so it can no "
         "longer re-render the plot. To save vector output, re-run it and "
         "export from the program itself (for example, its native "
         "PDF/SVG/PostScript output device), or use File \u2192 Save as PNG."];
    [alert addButtonWithTitle:@"OK"];

    NSWindow *key = [view window];
    if (key) {
        [alert beginSheetModalForWindow:key
                      completionHandler:^(NSModalResponse r) { (void)r; }];
    } else {
        [alert runModal];
    }
}

- (void)_vectorSave:(NSString *)fmt   /* @"PDF" or @"SVG" */
{
    GsView *view = [self _frontGsView];
    if (!view) { NSBeep(); return; }

    GsWindow *w     = &GS.wins[view->win_id];
    BOOL      alive = (w->client_fd >= 0);
    if (!alive) { [self _vectorSaveExited:fmt view:view]; return; }

    /* Only one reverse-render save may be in flight per window: the
     * reader thread clears save_path when SAVEDATA arrives. */
    pthread_mutex_lock(&w->write_lock);
    BOOL busy = (w->save_path != NULL);
    pthread_mutex_unlock(&w->write_lock);
    if (busy) {
        NSBeep();   /* a save is already pending for this window */
        return;
    }

    BOOL      is_svg = [fmt isEqualToString:@"SVG"];
    uint8_t   code   = is_svg ? GSP_SAVE_FMT_SVG : GSP_SAVE_FMT_PDF;
    NSString *ext    = is_svg ? @"svg" : @"pdf";

    NSString *base = (w->title[0])
        ? [NSString stringWithUTF8String:w->title]
        : @"giza_plot";
    base = [base stringByReplacingOccurrencesOfString:@"/" withString:@"-"];

    NSSavePanel *panel = [NSSavePanel savePanel];
    [panel setNameFieldStringValue:[base stringByAppendingPathExtension:ext]];
    if (@available(macOS 11.0, *)) {
        panel.allowedContentTypes = @[ is_svg ? UTTypeSVG : UTTypePDF ];
    }

    int       win_id = view->win_id;
    NSWindow *key    = [view window];
    [panel beginSheetModalForWindow:key
                  completionHandler:^(NSModalResponse result) {
        if (result != NSModalResponseOK) return;
        const char *cpath = [[panel URL] fileSystemRepresentation];
        if (!cpath) { NSBeep(); return; }

        GsWindow *ww = &GS.wins[win_id];
        pthread_mutex_lock(&ww->write_lock);
        /* Re-check under the lock: the client may have died or the window
         * closed while the panel was open (both nil client_fd to -1). */
        if (!ww->alive || ww->client_fd < 0) {
            pthread_mutex_unlock(&ww->write_lock);
            NSBeep();
            return;
        }
        free(ww->save_path);
        ww->save_path = strdup(cpath);
        ww->save_fmt  = code;
        pthread_mutex_unlock(&ww->write_lock);

        /* Ask the client to render & return the vector bytes. */
        _savereq_send(win_id, code);
    }];
}

- (void)savePDF:(id)sender { (void)sender; [self _vectorSave:@"PDF"]; }
- (void)saveSVG:(id)sender { (void)sender; [self _vectorSave:@"SVG"]; }

/* ---- NSWindowDelegate ---- */
/* Window resized by the user. For a connected window (show_interactive)
 * we ask the client to re-render at the new canvas size so the plot is
 * laid out for the new geometry instead of being letterbox-scaled. The
 * live drag fires this continuously, so we coalesce: (re)arm a short
 * timer keyed by win_id and only emit once the drag settles.
 *
 * Client-absent windows (a plain show() window after its script exited)
 * are left alone — drawRect: scales their last PNG, which is all we can
 * do without a client to re-render. */
- (void)windowDidResize:(NSNotification *)note
{
    NSWindow *win = [note object];
    GsView   *v   = _gsview_in(win);
    if (!_gsview_is_live(v)) return;            /* closed / stale slot */
    if (GS.wins[v->win_id].client_fd < 0) return;  /* nobody to re-render */

    /* NSNumbers of equal value are isEqual:, so cancel matches the
     * previously-scheduled fire even though it is a different instance. */
    NSNumber *boxed = [NSNumber numberWithInt:v->win_id];
    [NSObject cancelPreviousPerformRequestsWithTarget:self
                                             selector:@selector(_emitResize:)
                                               object:boxed];
    [self performSelector:@selector(_emitResize:)
               withObject:boxed
               afterDelay:GS_RESIZE_DEBOUNCE_SEC];
}

/* Fired once the resize drag settles (main thread). Read the plot view's
 * settled size and send RESIZE. Size is in points, matching how the
 * initial NEWWIN size is treated, so fonts/margins stay consistent
 * between the first frame and every resize. */
- (void)_emitResize:(NSNumber *)boxedWinId
{
    int id = [boxedWinId intValue];
    if (id < 0 || id >= GS.n_wins) return;
    GsWindow *w = &GS.wins[id];
    if (!w->alive || w->client_fd < 0 || w->view == nil) return;

    NSSize   sz = [w->view bounds].size;
    uint32_t pw = (uint32_t)(sz.width  + 0.5);
    uint32_t ph = (uint32_t)(sz.height + 0.5);
    if (pw == 0 || ph == 0) return;

    _resize_send(id, pw, ph);
}

/* Red-button close = explicit discard by the user. Logically invalidate
 * the slot: alive=0 / window=nil / view=nil / free last_png / client_fd=-1.
 * n_wins is NOT decremented (array index == win_id must stay stable, so
 * closed slots remain as holes). Every reader (_update_window,
 * _slider_send, the save actions) treats such a slot as "gone" by
 * checking alive and the niled refs.
 *
 * Closing also tears down the client connection so an INTERACTIVE client
 * (Perl Driver::GS show_interactive) stops blocking. Its run loop reads the
 * socket and returns on EOF; until server11 the fd was only ever closed at
 * process exit, so the loop blocked until Cmd-Q. Now we shutdown() the fd
 * when this was its last live window, which delivers EOF to the client and
 * also wakes our own per-connection reader (it then runs its normal
 * disconnect teardown and performs the single close()).
 *
 * Tab / window-group semantics fall out naturally: each tab is its own
 * NSWindow, so closing one tab fires windowWillClose: for that window only,
 * and closing a whole tab group fires it once per member. One connection can
 * back several windows ("one connection, many windows"), so we only shut the
 * fd down when no OTHER live window still uses it - closing one tab must not
 * sever its siblings. Cmd-Q still closes everything via process exit.
 *
 * We use shutdown(SHUT_RDWR), never close(), here:
 *   - it does not free the fd number, so there is no reuse race with a
 *     concurrent accept(); the owning reader thread still does the one close()
 *   - SHUT_WR delivers EOF to the client (run loop returns)
 *   - SHUT_RD makes our reader's blocked read() return 0 promptly
 * client_fd is flipped to -1 under write_lock first, so the slider/savereq
 * reverse-send paths never grab the fd we are about to shut down. */
- (void)windowWillClose:(NSNotification *)note
{
    NSWindow *closing = [note object];

    int fd_to_shutdown = -1;

    pthread_mutex_lock(&GS.wins_lock);
    for (int i = 0; i < GS.n_wins; i++) {
        GsWindow *w = &GS.wins[i];
        if (!w->alive || w->window != closing) continue;

        int fd = w->client_fd;        /* remember before we clear it */

        pthread_mutex_lock(&w->write_lock);
        w->client_fd = -1;            /* stop reverse sends (discard even if alive) */
        free(w->save_path);           /* drop any pending vector-save destination */
        w->save_path = NULL;
        pthread_mutex_unlock(&w->write_lock);

        w->alive  = 0;
        w->window = nil;              /* MRC: setReleasedWhenClosed:NO, but */
        w->view   = nil;              /*   nil it so save actions don't grab stale */
        NSData *old = w->last_png;
        w->last_png = nil;
        [old release];

        /* Tear the connection down only if this was its last live window.
         * (fd < 0 already means the client had exited - nothing to signal,
         * e.g. a plain show() window after its script finished.) */
        if (fd >= 0) {
            int still_used = 0;
            for (int j = 0; j < GS.n_wins; j++) {
                if (j == i) continue;
                if (GS.wins[j].alive && GS.wins[j].client_fd == fd) {
                    still_used = 1;
                    break;
                }
            }
            if (!still_used) fd_to_shutdown = fd;
        }
        break;                        /* window is unique: one slot, done */
    }
    pthread_mutex_unlock(&GS.wins_lock);

    /* Outside the lock (shutdown is a syscall; no window state involved). */
    if (fd_to_shutdown >= 0)
        shutdown(fd_to_shutdown, SHUT_RDWR);
}

@end


/* ------------------------------------------------------------------ */
/* Menu bar - App menu (Quit) + File menu (Save as PNG / PDF / SVG)    */
/* ------------------------------------------------------------------ */

/* --- Build provenance (2026-06-20 added) -----------------------------
 * __DATE__/__TIME__ are compiler-expanded at actual compile time, so
 * this string is a ground-truth answer to "is this binary really from
 * the source I think it is, and when was it actually compiled" - no
 * trust in shell history, PATH order, or memory of which `make` ran
 * last required. This was added after a real debugging session where
 * `which giza_server` (and the implicit PATH/auto-start fallback in
 * Driver::GS::_ensure_server) kept resolving to an old MacPorts/install
 * copy at /usr/local/bin/giza_server instead of the freshly-built one
 * in this repo, silently making every source edit a no-op for hours.
 * Shown both at startup (stderr, visible immediately) and in the App
 * menu (visible at any time without restarting). */
#define GIZA_SERVER_BUILD_STAMP (__DATE__ " " __TIME__)

static void _build_menu_bar(void)
{
    NSMenu *menubar = [[NSMenu alloc] init];
    [NSApp setMainMenu:menubar];

    /* --- App menu (first item; shows the app name) --- */
    NSMenuItem *app_item = [[NSMenuItem alloc] init];
    [menubar addItem:app_item];
    NSMenu *app_menu = [[NSMenu alloc] init];
    [app_item setSubmenu:app_menu];

    /* Disabled (non-clickable) info line showing build provenance.
     * setEnabled:NO makes it visible-but-inert, the same "selectable but
     * silent never happens" idea this file already uses for Save items
     * - except here inverted on purpose: this one item is SUPPOSED to be
     * inert, it exists only to be read. */
    NSMenuItem *build_item = [[NSMenuItem alloc]
        initWithTitle:[NSString stringWithFormat:@"Build: %s",
                       GIZA_SERVER_BUILD_STAMP]
               action:nil keyEquivalent:@""];
    [build_item setEnabled:NO];
    [app_menu addItem:build_item];
    [app_menu addItem:[NSMenuItem separatorItem]];

    [app_menu addItemWithTitle:@"Quit giza_server"
                        action:@selector(terminate:)
                 keyEquivalent:@"q"];   /* Cmd-Q works automatically */

    /* --- File menu --- */
    NSMenuItem *file_item = [[NSMenuItem alloc] init];
    [menubar addItem:file_item];
    NSMenu *file_menu = [[NSMenu alloc] initWithTitle:@"File"];
    [file_item setSubmenu:file_menu];

    /* target=nil: the action travels the responder chain to the matching
     * method on [NSApp delegate]. Enable/disable is decided by that same
     * delegate's validateMenuItem: (all three gray out when no live
     * window exists). PNG is viewer-side; PDF/SVG show a guidance dialog. */
    [file_menu addItemWithTitle:@"Save as PNG…"
                         action:@selector(savePNG:)
                  keyEquivalent:@"s"];   /* Cmd-S          */
    [file_menu addItemWithTitle:@"Save as PDF…"
                         action:@selector(savePDF:)
                  keyEquivalent:@"S"];   /* Cmd-Shift-S (uppercase S = with Shift) */
    [file_menu addItemWithTitle:@"Save as SVG…"
                         action:@selector(saveSVG:)
                  keyEquivalent:@""];    /* no shortcut */
}

/* ------------------------------------------------------------------ */
/* Public init - called from main thread before worker starts          */
/* ------------------------------------------------------------------ */

void giza_server_cocoa_init(void)
{
    /* Print build provenance immediately, before anything else, so it's
     * the first thing visible if the process is started from a terminal
     * (see _build_menu_bar's comment for why this exists). */
    fprintf(stderr, "giza_server (cocoa): build %s\n", GIZA_SERVER_BUILD_STAMP);
    fflush(stderr);

    memset(&GS, 0, sizeof(GS));
    pthread_mutex_init(&GS.wins_lock, NULL);
    GS.listen_fd = -1;

    gsp_resolve_sock_path(GS.sock_path, sizeof(GS.sock_path));

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    GsAppDelegate *delegate = [[GsAppDelegate alloc] init];
    [NSApp setDelegate:delegate];
    _build_menu_bar();

    /* finishLaunching is called internally by [NSApp run]; not done here */
}


/* ------------------------------------------------------------------ */
/* Window creation - must run on main thread                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int  suggested_w, suggested_h;
    int  client_fd;
    uint32_t group_id;        /* client PID for tab grouping; 0 = none */
    char title[256];
    int  result_id;           /* filled in by main-thread block       */
    dispatch_semaphore_t sem;
    float sld_init[2];        /* normalized [0,1] initial slider positions (from NEWWIN) */
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
    w->id        = idx;
    w->alive     = 1;
    w->client_fd = req->client_fd;
    w->group_id  = req->group_id;
    w->seq_out   = 0;
    pthread_mutex_init(&w->write_lock, NULL);
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
    /* Set a delegate so red-button closes are detected and the GS.wins[]
     * slot is invalidated. The delegate is the same GsAppDelegate as NSApp's. */
    [win setDelegate:(GsAppDelegate *)[NSApp delegate]];

    NSRect   cb       = win.contentView.bounds;
    CGFloat  slider_h = 28.0;   /* horizontal slider strip height (bottom) */
    CGFloat  slider_w = 28.0;   /* vertical slider strip width (right)     */

    /* GsView occupies the area minus the bottom and right strips */
    NSRect view_frame = NSMakeRect(0, slider_h,
                                   cb.size.width  - slider_w,
                                   cb.size.height - slider_h);
    GsView *view = [[GsView alloc] initWithFrame:view_frame];
    view->win_id = idx;
    [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [win.contentView addSubview:view];

    /* Horizontal slider (tag=0): bottom strip. Normalized [0,1]; the
    * client assigns its meaning (e.g. time offset) in its render
    * callback. Initial value from NEWWIN init records (default 0.0). */
    NSRect h_frame = NSMakeRect(8, 4,
                                cb.size.width - slider_w - 16,
                                slider_h - 8);
    NSSlider *hslider = [[NSSlider alloc] initWithFrame:h_frame];
    [hslider setMinValue:0.0];
    [hslider setMaxValue:1.0];
    [hslider setDoubleValue:req->sld_init[0]];
    [hslider setTag:0];
    [hslider setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
    [hslider setContinuous:YES];
    [hslider setTarget:view];
    [hslider setAction:@selector(sliderChanged:)];
    [win.contentView addSubview:hslider];

    /* Vertical slider (tag=1): right strip. Normalized [0,1], min=bottom /
     * max=top (NSSlider default). Client assigns meaning (e.g. gain);
     * initial value from NEWWIN init records (default 0.0). */
    NSRect v_frame = NSMakeRect(cb.size.width - slider_w + 4, slider_h + 4,
                                slider_w - 8,
                                cb.size.height - slider_h - 8);
    NSSlider *vslider = [[NSSlider alloc] initWithFrame:v_frame];
    [vslider setMinValue:0.0];
    [vslider setMaxValue:1.0];
    [vslider setDoubleValue:req->sld_init[1]];
    [vslider setTag:1];
    [vslider setAutoresizingMask:NSViewMinXMargin | NSViewHeightSizable];
    [vslider setContinuous:YES];
    [vslider setTarget:view];
    [vslider setAction:@selector(sliderChanged:)];
    [win.contentView addSubview:vslider];




    w->window = win;
    w->view   = view;

    /* ---- Tab grouping --------------------------------------------------
     * Windows opened by the same client process (group_id == client PID)
     * are gathered into one tab group, mirroring pdlcairo_viewer's
     * per-batch tabs. We add the new window explicitly to an existing live
     * group member with -addTabbedWindow:ordered: (deterministic; does not
     * depend on the user's "prefer tabs" system setting). group_id == 0
     * (e.g. C clients with no PID grouping) keeps standalone windows. */
    NSWindow *tab_host = nil;
    if (req->group_id != 0) {
        if (@available(macOS 10.12, *)) {
            win.tabbingIdentifier =
                [NSString stringWithFormat:@"giza_grp_%u", req->group_id];
            /* Host = the LAST live member of the group; inserting the new
             * window above it appends at the end of the tab bar, giving
             * natural creation order (1,2,3). Picking the first member and
             * inserting above it would stack every new tab just after tab 1
             * (1,3,2,...), so we keep scanning instead of breaking. */
            pthread_mutex_lock(&GS.wins_lock);
            for (int i = 0; i < GS.n_wins; ++i) {
                GsWindow *o = &GS.wins[i];
                if (i != idx && o->alive && o->window &&
                    o->group_id == req->group_id) {
                    tab_host = o->window;
                }
            }
            pthread_mutex_unlock(&GS.wins_lock);
        }
    }

    [[NSProcessInfo processInfo] disableAutomaticTermination:@"window open"];

    if (tab_host) {
        if (@available(macOS 10.12, *))
            [tab_host addTabbedWindow:win ordered:NSWindowAbove];
    }
    [win setAcceptsMouseMovedEvents:YES];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    req->result_id = idx;
    dispatch_semaphore_signal(req->sem);
}

/* Synchronously create a window from a non-main thread               */


static int _create_window_sync(int w_px, int h_px, const char *title,
                               int client_fd, uint32_t group_id,
                               const float sld_init[2])
{
    NewWinReq *req = calloc(1, sizeof(*req));
    req->suggested_w = w_px;
    req->suggested_h = h_px;
    req->client_fd   = client_fd;
    req->group_id    = group_id;
    req->sld_init[0] = sld_init ? sld_init[0] : 0.0f;
    req->sld_init[1] = sld_init ? sld_init[1] : 0.0f;
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
/* PNG update - decode NSImage from memory, push to view               */
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
    GsWindow *wptr   = w;          /* to update last_png for saving */

    /* The C array can't be captured by the block; convert to NSString and retain */
    NSString *ns_title = (title && title[0])
        ? [[NSString stringWithUTF8String:title] retain]
        : nil;

    dispatch_async(dispatch_get_main_queue(), ^{
        /* Re-check on the main thread: windowWillClose: may have run in
         * the meantime and invalidated the slot (red-button close). */
        if (!wptr->alive || wptr->view != view) {
            free(buf);
            if (ns_title) [ns_title release];
            return;
        }

        NSData  *data = [NSData dataWithBytesNoCopy:buf
                                             length:png_len
                                       freeWhenDone:YES];
        NSImage *img  = [[NSImage alloc] initWithData:data];
        if (img) {
            [view setImage:img];
            [img release];

            /* Keep an owned copy of the PNG bytes for saving. The `data`
             * above is NoCopy(freeWhenDone:YES) and can't be kept. This is
             * the main thread, serialized with savePNG:/windowWillClose:,
             * so no lock is needed. */
            NSData *keep = [[NSData alloc] initWithBytes:[data bytes]
                                                 length:[data length]];
            NSData *old = wptr->last_png;
            wptr->last_png = keep;   /* alloc'd = +1 retain, no extra retain */
            [old release];
        } else {
            fprintf(stderr, "giza_server: PNG decode failed\n");
        }
        if (ns_title) {
            [window setTitle:ns_title];
            [ns_title release];
        }
    });




}

/* --- Phase 2: 3D mode ---
 * GSP_MSG_3D_FRAME payload pass-through: unlike _update_window (which
 * decodes PNG bytes into an NSImage), the 3D payload is opaque binary
 * here and only gets parsed in GsView's drawRect:/_draw3DFrame. This
 * function just owns a copy and hands it to the view on the main thread,
 * mirroring _update_window's lock-and-dispatch shape.
 *
 * REVIEW NOTE (2026-06-21, label flicker fix): no longer calls
 * clear3DLabels/add3DLabel: - labels are now part of `payload` itself
 * (see gsp_3d.h, GS3D.pm's _send_3d_frame), so the single set3DPayload:
 * call below replaces lines+points+labels together, atomically, exactly
 * like it already did for lines+points alone. The previous design's
 * separate clear/add steps (driven by now-retired GSP_MSG_3D_LABEL
 * messages) are what let drawRect: observe a half-updated label set;
 * folding labels into this one NSData removes that window entirely. */
static void _update_window_3d(int win_id, const char *payload, size_t len)
{
    if (win_id < 0 || win_id >= GS.n_wins) return;
    GsWindow *w = &GS.wins[win_id];
    if (!w->alive) return;

    NSData *data = [[NSData alloc] initWithBytes:payload length:len];
    GsView *view = w->view;
    GsWindow *wptr = w;

    dispatch_async(dispatch_get_main_queue(), ^{
        /* Re-check on the main thread: windowWillClose: may have run in
         * the meantime (red-button close), same guard as _update_window. */
        if (!wptr->alive || wptr->view != view) { [data release]; return; }
        [view set3DPayload:data];
        [data release];
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

static int _send_msg_locked(int win_id, int fd, uint8_t type, uint32_t seq,
                            const void *payload, uint32_t plen)
{
    if (win_id >= 0 && win_id < GS.n_wins) {
        pthread_mutex_lock(&GS.wins[win_id].write_lock);
        int r = _send_msg(fd, type, seq, payload, plen);
        pthread_mutex_unlock(&GS.wins[win_id].write_lock);
        return r;
    }
    return _send_msg(fd, type, seq, payload, plen);   /* PONG etc.: no window yet */
}

static void _slider_send(int win_id, uint8_t slider_id, float value)
{
    if (win_id < 0 || win_id >= GS.n_wins) return;
    GsWindow *w = &GS.wins[win_id];
    if (!w->alive || w->client_fd < 0) return;
    gsp_slider_t body;
    body.slider_id = slider_id;
    body.value     = value;
    pthread_mutex_lock(&w->write_lock);
    /* Re-check client_fd under write_lock: the disconnect teardown and
     * windowWillClose: both flip it to -1 under the same lock, so we
     * never grab a closed fd here. */
    if (w->client_fd >= 0)
        _send_msg(w->client_fd, GSP_MSG_SLIDER, w->seq_out++,
                  &body, sizeof(body));
    pthread_mutex_unlock(&w->write_lock);
}

/* Ask the live client of `win_id` to re-render its current figure to a
 * vector format and send it back as GSP_MSG_SAVEDATA.  Same fd-sharing
 * discipline as _slider_send: the SAVEREQ write is serialized with the
 * ACK path through the per-window write_lock. */
static void _savereq_send(int win_id, uint8_t fmt)
{
    if (win_id < 0 || win_id >= GS.n_wins) return;
    GsWindow *w = &GS.wins[win_id];
    if (!w->alive) return;
    pthread_mutex_lock(&w->write_lock);
    if (w->client_fd >= 0)
        _send_msg(w->client_fd, GSP_MSG_SAVEREQ, w->seq_out++, &fmt, 1);
    pthread_mutex_unlock(&w->write_lock);
}

/* Tell the live client of `win_id` its window was resized to (w,h) px so
 * it can re-render at the new geometry. Same fd discipline as
 * _slider_send: serialized with the ACK path through write_lock, and a
 * no-op if the client has gone. last_sent_* suppresses duplicate final
 * sizes and pure window moves; it is touched only on the main thread
 * (windowDidResize:/_emitResize:), so the unlocked read is race-free. */
static void _resize_send(int win_id, uint32_t width_px, uint32_t height_px)
{
    if (win_id < 0 || win_id >= GS.n_wins) return;
    GsWindow *w = &GS.wins[win_id];
    if (!w->alive || w->client_fd < 0) return;
    if (width_px == w->last_sent_w && height_px == w->last_sent_h) return;

    gsp_resize_t body;
    body.width_px  = width_px;
    body.height_px = height_px;
    pthread_mutex_lock(&w->write_lock);
    /* Re-check under write_lock: disconnect teardown / windowWillClose:
     * flip client_fd to -1 under the same lock. */
    if (w->client_fd >= 0 &&
        _send_msg(w->client_fd, GSP_MSG_RESIZE, w->seq_out++,
                  &body, sizeof(body)) == 0) {
        w->last_sent_w = width_px;
        w->last_sent_h = height_px;
    }
    pthread_mutex_unlock(&w->write_lock);
}

static void _cursor_send(int win_id, float fx, float fy, uint8_t buttons, uint8_t type)
{
    if (win_id < 0 || win_id >= GS.n_wins) return;
    GsWindow *w = &GS.wins[win_id];
    if (!w->alive || w->client_fd < 0) return;
    gsp_cursor_t body;
    body.x       = fx;
    body.y       = fy;
    body.buttons = buttons;
    pthread_mutex_lock(&w->write_lock);
    if (w->client_fd >= 0)
        _send_msg(w->client_fd, type, w->seq_out++, &body, sizeof(body));
    pthread_mutex_unlock(&w->write_lock);
}

static void _zoom_send(int win_id, float zoom, float pan_x, float pan_y)
{
    if (win_id < 0 || win_id >= GS.n_wins) return;
    GsWindow *w = &GS.wins[win_id];
    if (!w->alive || w->client_fd < 0) return;
    gsp_zoom_t body;
    body.zoom  = zoom;
    body.pan_x = pan_x;
    body.pan_y = pan_y;
    pthread_mutex_lock(&w->write_lock);
    if (w->client_fd >= 0)
        _send_msg(w->client_fd, GSP_MSG_ZOOM, w->seq_out++, &body, sizeof(body));
    pthread_mutex_unlock(&w->write_lock);
}

/* --- Phase 2: 3D mode (トラックボール回転, 2026-06-21更新) ---
 * Same fd-sharing discipline as _slider_send/_zoom_send: serialized with
 * the ACK path through write_lock, no-op if the client has gone. Called
 * from GsView's mouseDragged: (main thread) for rotate-drag input.
 * dx/dy are raw drag pixel deltas now (not angles) - see gsp_3d.h and
 * GS3D.pm's _apply_drag_rotation for why. */
static void _3d_input_send(int win_id, float dx, float dy, float dzoom,
                           uint8_t type, uint8_t key)
{
    if (win_id < 0 || win_id >= GS.n_wins) return;
    GsWindow *w = &GS.wins[win_id];
    if (!w->alive || w->client_fd < 0) return;
    gsp_3d_input_t body;
    body.dx     = dx;
    body.dy     = dy;
    body.dzoom  = dzoom;
    body.type   = type;
    body.key    = key;
    pthread_mutex_lock(&w->write_lock);
    if (w->client_fd >= 0)
        _send_msg(w->client_fd, GSP_MSG_3D_INPUT, w->seq_out++, &body, sizeof(body));
    pthread_mutex_unlock(&w->write_lock);
}

/* Handle GSP_MSG_SAVEDATA from a client: find the window on this fd that
 * has a pending save (save_path set by the main thread in _vectorSave:)
 * and write the vector bytes to it.  Called from the per-connection
 * reader thread.  payload = [uint8 fmt][vector bytes...]. */
static void _save_vector_for_fd(int fd, const char *payload, size_t plen)
{
    if (plen < 1) {
        fprintf(stderr, "giza_server: SAVEDATA too short\n");
        return;
    }
    uint8_t      fmt   = (uint8_t)payload[0];
    const char  *bytes = payload + 1;
    size_t       blen  = plen - 1;

    /* Steal the destination path under the locks, then write outside them
     * (file I/O must not be held under write_lock). */
    char   *path   = NULL;
    uint8_t want   = 0;
    pthread_mutex_lock(&GS.wins_lock);
    for (int i = 0; i < GS.n_wins; i++) {
        GsWindow *w = &GS.wins[i];
        if (w->client_fd != fd) continue;
        pthread_mutex_lock(&w->write_lock);
        if (w->save_path) {
            path = w->save_path;       /* take ownership */
            want = w->save_fmt;
            w->save_path = NULL;
        }
        pthread_mutex_unlock(&w->write_lock);
        if (path) break;
    }
    pthread_mutex_unlock(&GS.wins_lock);

    if (!path) {
        fprintf(stderr, "giza_server: SAVEDATA with no pending save (ignored)\n");
        return;
    }
    if (fmt != want)
        fprintf(stderr, "giza_server: SAVEDATA format mismatch "
                "(got %u, expected %u) - writing anyway\n", fmt, want);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "giza_server: cannot open %s: %s\n",
                path, strerror(errno));
        free(path);
        dispatch_async(dispatch_get_main_queue(), ^{ NSBeep(); });
        return;
    }
    size_t wrote = blen ? fwrite(bytes, 1, blen, f) : 0;
    fclose(f);
    if (wrote != blen) {
        fprintf(stderr, "giza_server: short write to %s\n", path);
        dispatch_async(dispatch_get_main_queue(), ^{ NSBeep(); });
    }
    free(path);
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

    /* Tab grouping key: the connecting client's PID. All windows opened by
     * the same process land in one tab group. Unavailable -> 0 (ungrouped). */
    uint32_t peer_pid = 0;
#ifdef LOCAL_PEERPID
    {
        pid_t pid = 0;
        socklen_t pl = sizeof(pid);
        if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &pl) == 0 && pid > 0)
            peer_pid = (uint32_t)pid;
    }
#endif

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
            _send_msg_locked(current_win, fd, GSP_MSG_PONG, seq_out++, NULL, 0);
            break;

        case GSP_MSG_NEWWIN: {
            gsp_newwin_t nw = {0, 0};
            if (payload && hdr.length >= sizeof(nw))
                memcpy(&nw, payload, sizeof(nw));
             /* Optional trailing gsp_slider_t[] after the 8-byte w/h:
              * normalized [0,1] initial thumb positions, keyed by id.
              * A plain 8-byte NEWWIN keeps both sliders at 0.0. */
             float sld_init[2] = { 0.0f, 0.0f };
             if (payload && hdr.length > sizeof(nw)) {
                 size_t n = (hdr.length - sizeof(nw)) / sizeof(gsp_slider_t);
                 const uint8_t *rp = (const uint8_t *)payload + sizeof(nw);
                 for (size_t i = 0; i < n; i++) {
                     gsp_slider_t si;
                     memcpy(&si, rp + i * sizeof(gsp_slider_t), sizeof(si));
                     if (si.slider_id < 2) {
                         float v = si.value;
                         if (v < 0.0f) v = 0.0f;
                         if (v > 1.0f) v = 1.0f;
                         sld_init[si.slider_id] = v;
                     }
                 }
             }
            const char *ttl = title_buf[0] ? title_buf : "giza";
            current_win = _create_window_sync(nw.width_px, nw.height_px,
                                              ttl, fd, peer_pid, sld_init);
            _send_msg_locked(current_win, fd, GSP_MSG_ACK, seq_out++, NULL, 0);
            break;
        }

        case GSP_MSG_TITLE:

            /* fire-and-forget: NO ACK (matches GTK backend & protocol spec) */
            if (payload) {
                size_t len = hdr.length < 255 ? hdr.length : 255;
                memcpy(title_buf, payload, len);
                title_buf[len] = '\0';
                if (current_win >= 0 && GS.wins[current_win].alive) {
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
            if (current_win < 0) {
                 static const float dflt_init[2] = { 0.0f, 0.0f };
                 current_win = _create_window_sync(800, 600, "giza", fd,
                                                   peer_pid, dflt_init);
             }
            if (current_win >= 0) {
                _update_window(current_win, payload, hdr.length, title_buf);
                payload = NULL;
            }
            _send_msg_locked(current_win, fd, GSP_MSG_ACK, seq_out++, NULL, 0);
            break;
        }

        /* --- Phase 2: 3D mode ---
         * No auto-open fallback here (unlike PNG above): a 3D window
         * must come from Driver::GS3D::run()'s explicit NEWWIN first, so
         * current_win is always valid by the time a FRAME arrives. If a
         * FRAME somehow arrives with current_win < 0, drop it silently
         * rather than auto-creating a 2D-flavoured window that would
         * never get its is_3d flag set.
         *
         * REVIEW NOTE (2026-06-21, label flicker fix): labels are now
         * embedded in this FRAME payload itself (see gsp_3d.h's revised
         * layout and GS3D.pm's _send_3d_frame) rather than sent as
         * separate GSP_MSG_3D_LABEL messages (0x26, now retired). The
         * old split-message design let drawRect: observe a half-built
         * label set (clear3DLabels having run but not all of that
         * frame's add3DLabel: calls yet) because each LABEL was its own
         * dispatch_async block competing with the redraw's timing.
         * _update_window_3d/set3DPayload: now hand the *entire* payload
         * (lines + points + labels) to the view as one NSData, so
         * _draw3DFrame parses and draws a fully-consistent snapshot -
         * no separate clear/add steps, no partial-state window. */
        case GSP_MSG_3D_FRAME: {
            if (current_win >= 0) {
                GS.wins[current_win].is_3d = 1;
                _update_window_3d(current_win, payload, hdr.length);
            }
            _send_msg_locked(current_win, fd, GSP_MSG_ACK, seq_out++, NULL, 0);
            break;
        }

        /* 0x26 (旧GSP_MSG_3D_LABEL) は欠番。ラベルはFRAMEペイロードに
         * 統合されたため、このメッセージタイプはもう送受信されない
         * (gsp_3d.hのコメント参照)。古いクライアント/サーバーが万一
         * これを送ってきても、switchのdefault節で「未知の型」として
         * 安全に読み飛ばされる。 */


        case GSP_MSG_CLOSE:
            _send_msg_locked(current_win, fd, GSP_MSG_ACK, seq_out++, NULL, 0);
            free(payload);
            goto disconnect;

        case GSP_MSG_SAVEDATA:
            /* client replied to a SAVEREQ: write the vector bytes to the
             * path the user chose.  fire-and-forget (no ACK). */
            _save_vector_for_fd(fd, payload, hdr.length);
            break;

        default:
            fprintf(stderr, "giza_server: unknown msg type 0x%02X\n",
                    hdr.type);
            break;
        }

        free(payload);
    }

disconnect:
    /* Keep this client's windows (persistence) but invalidate client_fd.
     * A single current_win would miss the "one connection, many windows"
     * case, and after close() the fd number may be reused by another
     * connection - so BEFORE close() we scan all windows by fd and reset
     * them to -1. Writing under write_lock prevents the slider reverse
     * send (_slider_send) from grabbing a closed fd. */
    pthread_mutex_lock(&GS.wins_lock);
    for (int i = 0; i < GS.n_wins; i++) {
        if (GS.wins[i].client_fd == fd) {
            pthread_mutex_lock(&GS.wins[i].write_lock);
            GS.wins[i].client_fd = -1;
            free(GS.wins[i].save_path);   /* drop any pending vector-save */
            GS.wins[i].save_path = NULL;
            pthread_mutex_unlock(&GS.wins[i].write_lock);
        }
    }
    pthread_mutex_unlock(&GS.wins_lock);

    close(fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Accept loop - runs in worker thread                                 */
/* ------------------------------------------------------------------ */

int giza_server_worker_main(void)
{
    /* Ignore SIGPIPE so a client disconnect doesn't kill the process */
    signal(SIGPIPE, SIG_IGN);
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
