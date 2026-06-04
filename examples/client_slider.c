/* client_slider.c — minimal bidirectional GSP client.
 * Sends sin(k*x); receives SLIDER from server; redraws. No libgiza. */
#include <cairo/cairo.h>
#include <cairo/cairo-pdf.h>
#include <cairo/cairo-svg.h>
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

static int      g_fd;
static uint32_t g_seq = 0;

static int writen(int fd, const void *b, size_t n){
    size_t d=0; while(d<n){ ssize_t r=write(fd,(const char*)b+d,n-d);
        if(r<=0)return -1; d+=(size_t)r;} return 0; }
static int readn(int fd, void *b, size_t n){
    size_t d=0; while(d<n){ ssize_t r=read(fd,(char*)b+d,n-d);
        if(r<=0)return -1; d+=(size_t)r;} return 0; }

static int send_msg(uint8_t type, const void *payload, uint32_t plen){
    gsp_header_t h;
    h.magic=GSP_MAGIC; h.version=GSP_VERSION; h.type=type;
    h.flags=0; h.length=plen; h.seq=g_seq++;
    if(writen(g_fd,&h,sizeof(h))<0) return -1;
    if(plen && writen(g_fd,payload,plen)<0) return -1;
    return 0;
}
static int drain(uint32_t len){
    char t[256]; while(len){ uint32_t c=len<256?len:256;
        if(readn(g_fd,t,c)<0)return -1; len-=c;} return 0; }
static int recv_ack(void){               /* 次が ACK のはずのとき */
    gsp_header_t h;
    if(readn(g_fd,&h,sizeof(h))<0) return -1;
    if(h.magic!=GSP_MAGIC){ fprintf(stderr,"bad magic\n"); return -1; }
    return drain(h.length);
} 

/* ヘッダ16Bを非ブロックで1個だけ覗く。
 * 1=読めた / 0=今は無い / -1=エラーか切断 */
static int try_peek_header(gsp_header_t *h){
    ssize_t r = recv(g_fd, h, sizeof(*h), MSG_DONTWAIT);
    if (r == (ssize_t)sizeof(*h)) return 1;
    if (r == 0) return -1;                       /* 切断 */
    if (r < 0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
    return -1;                                   /* 中途半端 or 本物のエラー */
}

/* Cairo PNG → growable memory buffer */
typedef struct { unsigned char *buf; size_t len, cap; } PngBuf;
static cairo_status_t png_w(void *cl, const unsigned char *d, unsigned int n){
    PngBuf *p=cl;
    if(p->len+n > p->cap){ p->cap=(p->len+n)*2; p->buf=realloc(p->buf,p->cap); }
    memcpy(p->buf+p->len,d,n); p->len+=n; return CAIRO_STATUS_SUCCESS;
}


/* SLIDER ペイロード(id+float)を1個読み、k/amp を更新。
 * 戻り値 1=状態変化あり / 0=変化なし(未知id) / -1=エラー */
static int apply_slider(uint32_t plen, double *k, double *amp){
    gsp_slider_t body;
    if(plen < sizeof(body)){ if(drain(plen)<0) return -1; return 0; }
    if(readn(g_fd,&body,sizeof(body))<0) return -1;
    if(drain(plen-sizeof(body))<0) return -1;
    if(body.slider_id==0){ *k   = body.value; return 1; }
    if(body.slider_id==1){ *amp = body.value; return 1; }
    return 0;   /* 未知のスライダid は無視 */
}

/* 共有の描画ロジック（PNG送信とベクタ保存の両方から呼ぶ）*/
static void draw_plot(cairo_t *cr, double k, double amp){
    cairo_set_source_rgb(cr,1,1,1); cairo_paint(cr);
    cairo_set_source_rgb(cr,0.8,0.8,0.8); cairo_set_line_width(cr,1);
    cairo_move_to(cr,0,H/2.0); cairo_line_to(cr,W,H/2.0); cairo_stroke(cr);
    cairo_set_source_rgb(cr,0.1,0.3,0.9); cairo_set_line_width(cr,2);
    for(int i=0;i<W;i++){
        double x=(double)i/W*10.0, y=amp*sin(k*x);
        double py=H/2.0 - y*(H/2.0-40);
        if(i==0) cairo_move_to(cr,i,py); else cairo_line_to(cr,i,py);
    }
    cairo_stroke(cr);
}

static int render_and_send(double k, double amp);   /* fwd */

/* SAVEREQ への応答: 現在の k/amp を PDF/SVG に描いて SAVEDATA で返送。
 * Cairo の Pdf/SvgSurface はパス必須なので tmp ファイル経由。 */
static int save_vector(uint8_t fmt, double k, double amp){
    /* Honor $TMPDIR (set per-user on macOS as /var/folders/.../T/),
     * falling back to /tmp on systems that leave it unset (Linux).
     * Mirrors Perl File::Spec->tmpdir used by Driver::GS. */
    const char *td = getenv("TMPDIR");
    if (!td || !*td) td = "/tmp";
    size_t tl = strlen(td);
    char tmpl[512];
    snprintf(tmpl, sizeof(tmpl), "%s%sgiza_gs_save_XXXXXX",
             td, (tl && td[tl-1] == '/') ? "" : "/");
    int tfd=mkstemp(tmpl);
    if(tfd<0){ perror("mkstemp"); return 0; }   /* 失敗しても接続は継続 */
    close(tfd);                                  /* Cairo がパスで書く */

    cairo_surface_t *s = (fmt==GSP_SAVE_FMT_SVG)
        ? cairo_svg_surface_create(tmpl, W, H)
        : cairo_pdf_surface_create(tmpl, W, H);
    cairo_t *cr=cairo_create(s);
    draw_plot(cr,k,amp);
    cairo_destroy(cr);
    cairo_surface_finish(s);                     /* ファイル確定 */
    cairo_surface_destroy(s);

    /* slurp */
    FILE *f=fopen(tmpl,"rb");
    if(!f){ unlink(tmpl); return 0; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    if(n<0){ fclose(f); unlink(tmpl); return 0; }
    /* payload = [uint8 fmt][vector bytes] */
    char *buf=malloc(1+(size_t)n);
    if(!buf){ fclose(f); unlink(tmpl); return 0; }
    buf[0]=(char)fmt;
    size_t got = n ? fread(buf+1,1,(size_t)n,f) : 0;
    fclose(f); unlink(tmpl);

    int rc=send_msg(GSP_MSG_SAVEDATA, buf, (uint32_t)(1+got));
    free(buf);
    return rc;   /* 0=ok, -1=write error */
}

/* PNG送信後の ACK 待ち。待つ間に割り込む SAVEREQ は取りこぼさず処理し、
 * SLIDER 等はドレインして ACK が来るまで待ち続ける。 */
static int recv_ack_x(double k, double amp){
    for(;;){
        gsp_header_t h;
        if(readn(g_fd,&h,sizeof(h))<0) return -1;
        if(h.magic!=GSP_MAGIC){ fprintf(stderr,"bad magic\n"); return -1; }
        if(h.type==GSP_MSG_ACK) return drain(h.length);
        if(h.type==GSP_MSG_SAVEREQ){
            uint8_t fmt=GSP_SAVE_FMT_PDF;
            if(h.length>=1){
                if(readn(g_fd,&fmt,1)<0) return -1;
                if(drain(h.length-1)<0) return -1;
            }
            if(save_vector(fmt,k,amp)<0) return -1;
            continue;                            /* まだ ACK を待つ */
        }
        if(drain(h.length)<0) return -1;         /* SLIDER 等: 吸って継続 */
    }
}

static int render_and_send(double k, double amp){
    cairo_surface_t *s=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,W,H);
    cairo_t *cr=cairo_create(s);
    draw_plot(cr,k,amp);
    cairo_destroy(cr); cairo_surface_flush(s);

    PngBuf pb={0}; pb.cap=64*1024; pb.buf=malloc(pb.cap);
    cairo_surface_write_to_png_stream(s,png_w,&pb);
    cairo_surface_destroy(s);
    int rc=send_msg(GSP_MSG_PNG, pb.buf, (uint32_t)pb.len);
    free(pb.buf);
    if(rc<0) return -1;
    return recv_ack_x(k,amp);
}

int main(void){
    char path[256];
    snprintf(path,sizeof(path),
             GIZA_SERVER_SOCK_DIR "/" GIZA_SERVER_SOCK_NAME,(int)getuid());
    g_fd=socket(AF_UNIX,SOCK_STREAM,0);
    struct sockaddr_un a; memset(&a,0,sizeof(a)); a.sun_family=AF_UNIX;
    strncpy(a.sun_path,path,sizeof(a.sun_path)-1);
    if(connect(g_fd,(struct sockaddr*)&a,sizeof(a))<0){ perror("connect"); return 1; }
    printf("connected: %s\n",path);

    gsp_newwin_t nw={W,H};
    send_msg(GSP_MSG_NEWWIN,&nw,sizeof(nw));
    if(recv_ack()<0){ fprintf(stderr,"no NEWWIN ack\n"); return 1; }

    double k=1.0, amp=1.0;
    if(render_and_send(k,amp)<0){ fprintf(stderr,"initial send failed\n"); return 1; }
    printf("initial frame k=%.2f amp=%.2f — move the sliders\n",k,amp);

for(;;){
        gsp_header_t h;
        if(readn(g_fd,&h,sizeof(h))<0){ printf("server closed\n"); break; }
        if(h.magic!=GSP_MAGIC){ fprintf(stderr,"bad magic\n"); break; }

        int changed = 0;

        if(h.type==GSP_MSG_SLIDER){
            int r = apply_slider(h.length, &k, &amp);
            if(r<0) break;
            if(r>0) changed = 1;
        } else if(h.type==GSP_MSG_SAVEREQ){
            uint8_t fmt=GSP_SAVE_FMT_PDF;
            if(h.length>=1){
                if(readn(g_fd,&fmt,1)<0) break;
                if(h.length>1 && drain(h.length-1)<0) break;
            }
            if(save_vector(fmt,k,amp)<0) break;
        } else {
            if(drain(h.length)<0) break;
        }

        /* coalesce: 溜まっている後続を吸い出す */
        for(;;){
            gsp_header_t h2;
            int p = try_peek_header(&h2);
            if(p<=0){ if(p<0){ printf("server closed\n"); goto done; } break; }
            if(h2.magic!=GSP_MAGIC){ fprintf(stderr,"bad magic (coalesce)\n"); goto done; }
            if(h2.type==GSP_MSG_SLIDER){
                int r = apply_slider(h2.length, &k, &amp);
                if(r<0) goto done;
                if(r>0) changed = 1;
            } else if(h2.type==GSP_MSG_SAVEREQ){
                uint8_t fmt=GSP_SAVE_FMT_PDF;
                if(h2.length>=1){
                    if(readn(g_fd,&fmt,1)<0) goto done;
                    if(h2.length>1 && drain(h2.length-1)<0) goto done;
                }
                if(save_vector(fmt,k,amp)<0) goto done;
            } else {
                if(drain(h2.length)<0) goto done;
            }
        }

        if(changed){
            printf("slider -> k=%.3f amp=%.3f\n", k, amp);
            if(render_and_send(k,amp)<0){ printf("send failed\n"); break; }
        }
    }
done:
    close(g_fd);
    return 0;

}
