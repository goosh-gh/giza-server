/* client_mouse.c — minimal bidirectional GSP client for mouse events.
 * Draws a scatter plot; receives CURSOR / PICK / ZOOM from the server
 * (giza-server14); prints the cursor position, reports the nearest data
 * point on a click, and highlights the picked point. No libgiza.
 *
 * Companion to client_slider.c — same GSP + Cairo plumbing, exercising
 * the server->client mouse channels instead of the slider channel.
 *
 * Coordinates arrive as IMAGE FRACTIONS in [0,1]: (0,0) = top-left of the
 * rendered image, (1,1) = bottom-right. This client maps them to its own
 * data coordinates using the same plot rectangle it draws into.
 */
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include "../viewer/giza-server-protocol.h"

#define W 800
#define H 600

/* Plot rectangle inside the image (pixels). The image-fraction coords the
 * server sends are relative to the whole image, so we invert through the
 * same margins to recover data coordinates. */
#define PLOT_L 60.0
#define PLOT_R 760.0
#define PLOT_T 40.0
#define PLOT_B 540.0

/* Data domain shown in the plot rectangle. */
#define XMIN 0.0
#define XMAX 10.0
#define YMIN -1.2
#define YMAX 1.2

#define NPTS 24

static int      g_fd;
static uint32_t g_seq = 0;

/* Scatter data (filled in main). */
static double g_dx[NPTS], g_dy[NPTS];
static int    g_picked = -1;   /* index of the highlighted point, -1 = none */

static int writen(int fd, const void *b, size_t n){
    size_t d=0;
    while(d<n){ ssize_t r=write(fd,(const char*)b+d,n-d);
        if(r<=0) return -1;
        d+=(size_t)r;
    }
    return 0;
}
static int readn(int fd, void *b, size_t n){
    size_t d=0;
    while(d<n){ ssize_t r=read(fd,(char*)b+d,n-d);
        if(r<=0) return -1;
        d+=(size_t)r;
    }
    return 0;
}

static int send_msg(uint8_t type, const void *payload, uint32_t plen){
    gsp_header_t h;
    h.magic=GSP_MAGIC; h.version=GSP_VERSION; h.type=type;
    h.flags=0; h.length=plen; h.seq=g_seq++;
    if(writen(g_fd,&h,sizeof(h))<0) return -1;
    if(plen && writen(g_fd,payload,plen)<0) return -1;
    return 0;
}
static int drain(uint32_t len){
    char t[256];
    while(len){ uint32_t c=len<256?len:256;
        if(readn(g_fd,t,c)<0) return -1;
        len-=c;
    }
    return 0;
}
static int recv_ack(void){               /* next message must be ACK */
    gsp_header_t h;
    if(readn(g_fd,&h,sizeof(h))<0) return -1;
    if(h.magic!=GSP_MAGIC){ fprintf(stderr,"bad magic\n"); return -1; }
    return drain(h.length);
}

/* Peek one 16B header non-blocking. 1=got it / 0=nothing now / -1=err/closed */
static int try_peek_header(gsp_header_t *h){
    ssize_t r = recv(g_fd, h, sizeof(*h), MSG_DONTWAIT);
    if (r == (ssize_t)sizeof(*h)) return 1;
    if (r == 0) return -1;                       /* closed */
    if (r < 0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
    return -1;
}

/* Cairo PNG -> growable memory buffer */
typedef struct { unsigned char *buf; size_t len, cap; } PngBuf;
static cairo_status_t png_w(void *cl, const unsigned char *d, unsigned int n){
    PngBuf *p=cl;
    if(p->len+n > p->cap){ p->cap=(p->len+n)*2; p->buf=realloc(p->buf,p->cap); }
    memcpy(p->buf+p->len,d,n); p->len+=n; return CAIRO_STATUS_SUCCESS;
}

/* ---- coordinate mapping -------------------------------------------- */

/* data (x,y) -> pixel (px,py) inside the image */
static void data_to_px(double dx, double dy, double *px, double *py){
    *px = PLOT_L + (dx - XMIN) / (XMAX - XMIN) * (PLOT_R - PLOT_L);
    /* pixel y grows downward; data y grows upward */
    *py = PLOT_B - (dy - YMIN) / (YMAX - YMIN) * (PLOT_B - PLOT_T);
}

/* image fraction (fx,fy) in [0,1] -> data (x,y).
 * fy is top-down (0 = top of image), matching the server convention. */
static void frac_to_data(double fx, double fy, double *dx, double *dy){
    double px = fx * W;
    double py = fy * H;
    *dx = XMIN + (px - PLOT_L) / (PLOT_R - PLOT_L) * (XMAX - XMIN);
    *dy = YMIN + (PLOT_B - py) / (PLOT_B - PLOT_T) * (YMAX - YMIN);
}

/* nearest data point index to data coords (dx,dy) */
static int nearest_point(double dx, double dy){
    int best=-1; double bd=1e30;
    for(int i=0;i<NPTS;i++){
        double ex=g_dx[i]-dx, ey=g_dy[i]-dy;
        double d=ex*ex+ey*ey;
        if(d<bd){ bd=d; best=i; }
    }
    return best;
}

/* ---- drawing -------------------------------------------------------- */

static void draw_plot(cairo_t *cr){
    cairo_set_source_rgb(cr,1,1,1); cairo_paint(cr);

    /* axes box */
    cairo_set_source_rgb(cr,0.6,0.6,0.6); cairo_set_line_width(cr,1);
    cairo_rectangle(cr, PLOT_L, PLOT_T, PLOT_R-PLOT_L, PLOT_B-PLOT_T);
    cairo_stroke(cr);

    /* zero line */
    double zx0,zy0,zx1,zy1;
    data_to_px(XMIN,0.0,&zx0,&zy0); data_to_px(XMAX,0.0,&zx1,&zy1);
    cairo_set_source_rgb(cr,0.85,0.85,0.85);
    cairo_move_to(cr,zx0,zy0); cairo_line_to(cr,zx1,zy1); cairo_stroke(cr);

    /* points */
    for(int i=0;i<NPTS;i++){
        double px,py; data_to_px(g_dx[i],g_dy[i],&px,&py);
        if(i==g_picked){
            cairo_set_source_rgb(cr,0.9,0.2,0.2);
            cairo_arc(cr,px,py,8.0,0,2*M_PI); cairo_fill(cr);
        } else {
            cairo_set_source_rgb(cr,0.1,0.3,0.9);
            cairo_arc(cr,px,py,4.0,0,2*M_PI); cairo_fill(cr);
        }
    }

    /* title */
    cairo_set_source_rgb(cr,0.2,0.2,0.2);
    cairo_select_font_face(cr,"sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr,16);
    cairo_move_to(cr,PLOT_L,PLOT_T-14);
    cairo_show_text(cr,"move the mouse (cursor), click a point (pick), "
                       "scroll/pinch to zoom");
}

static int render_and_send(void);   /* fwd */

/* ACK wait after a PNG send. We do not save vectors in this demo, so any
 * non-ACK message that arrives in the gap is simply drained until ACK. */
static int recv_ack_x(void){
    for(;;){
        gsp_header_t h;
        if(readn(g_fd,&h,sizeof(h))<0) return -1;
        if(h.magic!=GSP_MAGIC){ fprintf(stderr,"bad magic\n"); return -1; }
        if(h.type==GSP_MSG_ACK) return drain(h.length);
        if(drain(h.length)<0) return -1;        /* CURSOR/PICK/ZOOM etc.: skip */
    }
}

static int render_and_send(void){
    cairo_surface_t *s=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,W,H);
    cairo_t *cr=cairo_create(s);
    draw_plot(cr);
    cairo_destroy(cr); cairo_surface_flush(s);

    PngBuf pb={0}; pb.cap=64*1024; pb.buf=malloc(pb.cap);
    cairo_surface_write_to_png_stream(s,png_w,&pb);
    cairo_surface_destroy(s);
    int rc=send_msg(GSP_MSG_PNG, pb.buf, (uint32_t)pb.len);
    free(pb.buf);
    if(rc<0) return -1;
    return recv_ack_x();
}

/* ---- event handlers ------------------------------------------------- */

/* Read a CURSOR/PICK payload (gsp_cursor_t). Returns 1 on a PICK that
 * changed the highlighted point (needs redraw), 0 otherwise, -1 on error. */
static int handle_cursor(uint8_t type, uint32_t plen){
    gsp_cursor_t body;
    if(plen < sizeof(body)){ if(drain(plen)<0) return -1; return 0; }
    if(readn(g_fd,&body,sizeof(body))<0) return -1;
    if(drain(plen-sizeof(body))<0) return -1;

    double dx,dy; frac_to_data(body.x, body.y, &dx, &dy);
    if(type==GSP_MSG_CURSOR){
        printf("\rcursor: frac=(%.3f,%.3f) data=(%6.3f,%6.3f)   ",
               body.x, body.y, dx, dy);
        fflush(stdout);
        return 0;
    }
    /* PICK */
    int i = nearest_point(dx,dy);
    printf("\nPICK btn=%u -> nearest point #%d at data=(%.3f,%.3f)\n",
           body.buttons, i, g_dx[i], g_dy[i]);
    if(i != g_picked){ g_picked = i; return 1; }   /* redraw to highlight */
    return 0;
}

static int handle_zoom(uint32_t plen){
    gsp_zoom_t body;
    if(plen < sizeof(body)){ if(drain(plen)<0) return -1; return 0; }
    if(readn(g_fd,&body,sizeof(body))<0) return -1;
    if(drain(plen-sizeof(body))<0) return -1;
    printf("\nZOOM zoom=%.2f pan=(%.2f,%.2f)\n", body.zoom, body.pan_x, body.pan_y);
    return 0;
}

int main(void){
    /* sample scatter: a noisy-ish sine */
    for(int i=0;i<NPTS;i++){
        g_dx[i] = (double)i / (NPTS-1) * (XMAX-XMIN) + XMIN;
        g_dy[i] = sin(g_dx[i]);
    }

    char path[256];
    gsp_resolve_sock_path(path, sizeof(path));
    g_fd=socket(AF_UNIX,SOCK_STREAM,0);
    struct sockaddr_un a; memset(&a,0,sizeof(a)); a.sun_family=AF_UNIX;
    strncpy(a.sun_path,path,sizeof(a.sun_path)-1);
    if(connect(g_fd,(struct sockaddr*)&a,sizeof(a))<0){ perror("connect"); return 1; }
    printf("connected: %s\n",path);

    gsp_newwin_t nw={W,H};
    send_msg(GSP_MSG_NEWWIN,&nw,sizeof(nw));
    if(recv_ack()<0){ fprintf(stderr,"no NEWWIN ack\n"); return 1; }

    if(render_and_send()<0){ fprintf(stderr,"initial send failed\n"); return 1; }
    printf("initial frame sent — interact with the window\n");

    for(;;){
        gsp_header_t h;
        if(readn(g_fd,&h,sizeof(h))<0){ printf("\nserver closed\n"); break; }
        if(h.magic!=GSP_MAGIC){ fprintf(stderr,"bad magic\n"); break; }

        int redraw = 0;

        if(h.type==GSP_MSG_CURSOR || h.type==GSP_MSG_PICK){
            int r = handle_cursor(h.type, h.length);
            if(r<0) break;
            if(r>0) redraw = 1;
        } else if(h.type==GSP_MSG_ZOOM){
            if(handle_zoom(h.length)<0) break;
        } else {
            if(drain(h.length)<0) break;       /* SLIDER/RESIZE/etc.: ignore */
        }

        /* coalesce: drain any backlog so cursor spam does not pile up */
        for(;;){
            gsp_header_t h2;
            int p = try_peek_header(&h2);
            if(p<=0){ if(p<0){ printf("\nserver closed\n"); goto done; } break; }
            if(h2.magic!=GSP_MAGIC){ fprintf(stderr,"bad magic (coalesce)\n"); goto done; }
            if(h2.type==GSP_MSG_CURSOR || h2.type==GSP_MSG_PICK){
                int r = handle_cursor(h2.type, h2.length);
                if(r<0) goto done;
                if(r>0) redraw = 1;
            } else if(h2.type==GSP_MSG_ZOOM){
                if(handle_zoom(h2.length)<0) goto done;
            } else {
                if(drain(h2.length)<0) goto done;
            }
        }

        if(redraw){
            if(render_and_send()<0){ printf("\nsend failed\n"); break; }
        }
    }
done:
    close(g_fd);
    return 0;
}
