/* giza-server-xlib-3d.c  --  Phase 2: Xlib ビューアの 3D フレーム描画
 *
 * 2026-07-12 全面書き直し。旧版(改名前の dtheta/dphi を使い、XDrawLine/
 * XFillArc で X GC 直描きしていたドラフト)は破棄した。破棄理由:
 *
 *   1. gsp_3d_input_t のフィールドが 2026-06-21 に dtheta/dphi -> dx/dy へ
 *      改名されており(角度差分 -> 生ピクセル差分)、旧版はコンパイルすら
 *      通らなかった。
 *   2. send_fn のシグネチャが giza-server-xlib.c の実際の送信関数
 *      (_send_hdr + _write_exact, win->write_lock 保護)と噛み合わなかった。
 *   3. ラベルが FRAME ペイロード末尾に同梱される現行仕様(Phase 1 の
 *      ちらつき対策)を知らず、旧 GSP_MSG_3D_LABEL 前提だった。
 *   4. X GC 直描きは giza-server-xlib.c の cairo ベースの描画
 *      (_repaint_container / タブバー / レターボックス)と混在し、
 *      cairo のクリアが後から乗って 3D を消す危険があった。
 *
 * 2026-08-01 (giza-server19, Cocoa パリティ化): 三角セクション(flags bit3
 * =0x08)の z-buffer Gouraud ラスタライズと、水平 span の thick-fill(flags
 * bit2=0x04)を追加。どちらも 2026-07-23 に Cocoa 版(giza-server-cocoa.m)へ
 * 入った機能で、この 07-12 版には無く Linux が 3D で見劣りしていた分。
 * ラスタライザ核 gsp_edge / gsp_raster_tris は Cocoa 版とバイト同一で流用
 * (依存が stdint のみの純 C なのでヘッドレスで単体テストできる)。Cocoa は
 * CGImage で blit するが、ここでは cairo image surface へ焼いて paint する。
 *
 * 現行方針: cairo だけで描く。呼び出し側(_repaint_container)が用意した
 * cairo_t にタブ本体の矩形(ox,oy,vw,vh)へオフセットして描画するので、
 * タブバーやウィンドウ枠と自然に共存する。サーバ状態には一切触れない
 * (純粋な描画関数)。入力の逆送信は xlib.c 側の _3d_input_send_xlib が持つ。
 *
 * ---- Y 軸の規約(重要) ----
 * Driver::GS3D の _project() は sy = y*scale + cy (cy = height/2) を返す。
 * これは「Y が下向きに増える」画像座標系で、cairo / X11 と同一である。
 * したがってここでは Y を反転しない。
 * Cocoa 版(giza-server-cocoa.m の _draw3DFrame)が view_h - y と反転して
 * いるのは、NSView の isFlipped が NO(Y が上向き)だからで、Cocoa 固有の
 * 事情である。あれを素朴に移植すると上下逆さになるので注意。
 *
 * ---- ペイロード レイアウト(gsp_3d.h、Driver::GS3D::_send_3d_frame)----
 *   gsp_3d_frame_hdr_t (13 bytes): n_lines(u16) n_points(u16) flags(u8)
 *                                  cx(f32) cy(f32)
 *   n_lines  * gsp_3d_line_t  (24 bytes: x0,y0,x1,y1,depth f32 / r,g,b,a u8)
 *   n_points * gsp_3d_point_t (20 bytes: x,y,depth,size f32 / r,g,b,a u8)
 *   uint16_t n_labels
 *   n_labels * (point_idx u16, x f32, y f32, r,g,b u8, len u8, text[len])
 *   [flags bit3=0x08 のとき] uint16_t n_tris, n_tris * gsp_3d_tri_t (45 bytes)
 *
 * レコードはサーバ側(Driver::GS3D の $depth->qsorti)で既に奥から手前へ
 * ソート済みなので、ここでは並べ替えず配列順に描く(画家のアルゴリズム)。
 * 三角(z-buffer)だけは描画順に依らず z テストで正しく重なる。
 *
 * 境界チェック: text の長さはワイヤから来るデータなので、壊れた長さで
 * バッファ外を歩かないよう 1 レコードごとに検査する(Cocoa 版と同じ)。
 */

#include <cairo/cairo.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gsp_3d.h"

void gsp3d_draw_cairo(cairo_t *cr, double ox, double oy, double vw, double vh,
                      const unsigned char *buf, size_t len);

/* --- Phase 2 (案③): z-buffer 三角ラスタライザ --------------------------
 *
 * gsp_edge / gsp_raster_tris は giza-server-cocoa.m の同名関数とバイト同一。
 * Gouraud 補間 + z テストで RGBA バッファへ塗る純 C 核(Cocoa/cairo 非依存、
 * stdint のみ)なので、両バックエンドで同じテストが通る。z は大きいほど手前
 * (gsp_3d_line の depth と同符号)。この核を Cocoa と別実装にすると「Mac では
 * 出るが Linux では歪む」類の再現困難バグの温床になるので、意図的に同一に
 * 保つ(片方を直したら他方にも同じ差分を当てる)。 */
static inline float gsp_edge(float ax,float ay,float bx,float by,float cx,float cy){
    return (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
}
static void gsp_raster_tris(uint8_t *px, float *zb, int w, int h,
                            const gsp_3d_tri_t *tris, int n)
{
    for (int i = 0; i < n; i++) {
        const gsp_3d_vert_t *V = tris[i].v;
        float x0=V[0].x,y0=V[0].y,z0=V[0].z;
        float x1=V[1].x,y1=V[1].y,z1=V[1].z;
        float x2=V[2].x,y2=V[2].y,z2=V[2].z;
        float area = gsp_edge(x0,y0,x1,y1,x2,y2);
        if (area == 0.0f) continue;                      /* degenerate */
        float inv = 1.0f/area;
        float mnx=x0<x1?(x0<x2?x0:x2):(x1<x2?x1:x2);
        float mxx=x0>x1?(x0>x2?x0:x2):(x1>x2?x1:x2);
        float mny=y0<y1?(y0<y2?y0:y2):(y1<y2?y1:y2);
        float mxy=y0>y1?(y0>y2?y0:y2):(y1>y2?y1:y2);
        int minx=(int)mnx; if(minx<0)minx=0;
        int maxx=(int)(mxx+1.0f); if(maxx>w-1)maxx=w-1;
        int miny=(int)mny; if(miny<0)miny=0;
        int maxy=(int)(mxy+1.0f); if(maxy>h-1)maxy=h-1;
        for (int py=miny; py<=maxy; py++){
            for (int pxx=minx; pxx<=maxx; pxx++){
                float fx=pxx+0.5f, fy=py+0.5f;
                float w0=gsp_edge(x1,y1,x2,y2,fx,fy);    /* weight for v0 */
                float w1=gsp_edge(x2,y2,x0,y0,fx,fy);    /* weight for v1 */
                float w2=gsp_edge(x0,y0,x1,y1,fx,fy);    /* weight for v2 */
                int inside = (area>0) ? (w0>=0&&w1>=0&&w2>=0)
                                      : (w0<=0&&w1<=0&&w2<=0);
                if (!inside) continue;
                float l0=w0*inv,l1=w1*inv,l2=w2*inv;
                float z = l0*z0 + l1*z1 + l2*z2;
                int idx = py*w + pxx;
                if (z <= zb[idx]) continue;              /* z-test: larger = nearer */
                zb[idx] = z;
                float r=l0*V[0].r+l1*V[1].r+l2*V[2].r;
                float g=l0*V[0].g+l1*V[1].g+l2*V[2].g;
                float b=l0*V[0].b+l1*V[1].b+l2*V[2].b;
                uint8_t *o = px + idx*4;
                o[0]=(uint8_t)(r<0?0:(r>255?255:r));     /* R,G,B,A tight layout */
                o[1]=(uint8_t)(g<0?0:(g>255?255:g));
                o[2]=(uint8_t)(b<0?0:(b>255?255:b));
                o[3]=255;
            }
        }
    }
}

/* Cocoa の _raster_and_blit の cairo 版。三角を w×h の RGBA バッファへ焼き、
 * cairo image surface(ARGB32)へ変換して現在の user space の原点(0,0)に paint
 * する。呼び出し側で既に cairo_translate(ox,oy) 済みなので、バッファのピクセル
 * (x,y) は最終的にコンテナの (ox+x, oy+y) に落ちる = 線/点/ラベルと同じ座標系。
 *
 * Y は反転しない(バッファ行 0 = 画像 top = ペイロード y=0、cairo も Y 下向き)。
 * Cocoa 版は NSView が Y 上向きなので blit を上下反転していたが、ここでは不要。 */
static void _raster_and_blit_cairo(cairo_t *cr, const void *tri_src, int n,
                                   int w, int h)
{
    if (w <= 0 || h <= 0 || n <= 0) return;

    /* 三角セクションは可変長ラベルの後ろに来るのでアラインメント保証が無い。
     * float メンバを触る前にアラインされた配列へコピーする(Cocoa 版と同じ)。 */
    gsp_3d_tri_t *tris = (gsp_3d_tri_t*)malloc((size_t)n * sizeof(gsp_3d_tri_t));
    if (!tris) return;
    memcpy(tris, tri_src, (size_t)n * sizeof(gsp_3d_tri_t));

    uint8_t *px = (uint8_t*)calloc((size_t)w*h*4, 1);    /* 透明(a=0)背景 */
    float   *zb = (float*)malloc((size_t)w*h*sizeof(float));
    if (!px || !zb) { free(px); free(zb); free(tris); return; }
    for (int i=0;i<w*h;i++) zb[i] = -1e30f;
    gsp_raster_tris(px, zb, w, h, tris, n);

    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(img) == CAIRO_STATUS_SUCCESS) {
        unsigned char *dst    = cairo_image_surface_get_data(img);
        int            stride = cairo_image_surface_get_stride(img);
        /* CAIRO_FORMAT_ARGB32 はホストエンディアンの uint32 0xAARRGGBB(=事前
         * 乗算アルファ)。uint32 語で書けばエンディアン非依存。三角は a=0 か
         * a=255 のどちらかなので事前乗算は R,G,B をそのまま使えば正しい
         * (a=0 の画素は calloc で R=G=B=0 = 事前乗算済みと一致)。 */
        for (int y = 0; y < h; y++) {
            uint32_t     *drow = (uint32_t*)(dst + (size_t)y * stride);
            const uint8_t*srow = px + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                uint8_t R = srow[x*4+0], G = srow[x*4+1],
                        B = srow[x*4+2], A = srow[x*4+3];
                drow[x] = ((uint32_t)A << 24) | ((uint32_t)R << 16)
                        | ((uint32_t)G <<  8) |  (uint32_t)B;
            }
        }
        cairo_surface_mark_dirty(img);
        cairo_set_source_surface(cr, img, 0.0, 0.0);
        cairo_paint(cr);
        cairo_set_source_rgb(cr, 0, 0, 0);   /* source を surface から外す */
    }
    cairo_surface_destroy(img);
    free(px); free(zb); free(tris);
}

void
gsp3d_draw_cairo(cairo_t *cr, double ox, double oy, double vw, double vh,
                 const unsigned char *buf, size_t len)
{
    if (!cr || vw <= 0 || vh <= 0) return;

    cairo_save(cr);

    /* タブ本体の矩形だけに描く。2D 側のレターボックス処理と同じ考え方で、
     * はみ出しがタブバーを侵食しないようクリップする。 */
    cairo_rectangle(cr, ox, oy, vw, vh);
    cairo_clip(cr);

    /* 背景: 2D ビューアの白と区別できる暗色。フレーム未着でも「3D窓だ」と
     * 一目で分かる(Cocoa 版と同じ配色)。 */
    cairo_set_source_rgb(cr, 0.08, 0.08, 0.12);
    cairo_paint(cr);

    if (!buf || len < sizeof(gsp_3d_frame_hdr_t)) {
        cairo_restore(cr);
        return;                     /* まだフレームが来ていない: 暗色のみ */
    }

    /* ペイロードの座標は 0..width / 0..height(クライアントが NEWWIN で
     * 要求したサイズ)なので、タブ本体の原点へ平行移動するだけでよい。 */
    cairo_translate(cr, ox, oy);

    gsp_3d_frame_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    const unsigned char *p   = buf + sizeof(hdr);
    const unsigned char *end = buf + len;

    size_t need = sizeof(hdr)
                + (size_t)hdr.n_lines  * sizeof(gsp_3d_line_t)
                + (size_t)hdr.n_points * sizeof(gsp_3d_point_t);
    if (len < need) {               /* 切れた/壊れたペイロード: 何も描かない */
        cairo_restore(cr);
        return;
    }

    /* ---- 三角セクション(flags bit3=0x08): z-buffer cortex。最背面に最初に
     * 描き、軸/電極/ラベルを上に重ねる。物理配置はラベルの後ろなので、別カーソル
     * tp でそこまで歩いてから焼く(下の線/点/ラベル解析は p のまま不変)。 */
    if (hdr.flags & 0x08) {
        const unsigned char *tp = buf + need;            /* = n_labels の位置 */
        if (tp + 2 <= end) {
            uint16_t n_labels; memcpy(&n_labels, tp, 2); tp += 2;
            for (uint16_t i = 0; i < n_labels && tp + 14 <= end; i++) {
                uint8_t l = tp[13];                       /* 固定部 14B, len は offset 13 */
                tp += 14 + (size_t)l;
            }
            if (tp + 2 <= end) {
                uint16_t n_tris; memcpy(&n_tris, tp, 2); tp += 2;
                if (n_tris > 0 &&
                    tp + (size_t)n_tris * sizeof(gsp_3d_tri_t) <= end) {
                    _raster_and_blit_cairo(cr, tp, (int)n_tris,
                                           (int)vw, (int)vh);
                }
            }
        }
    }

    /* ---- 線分(軸・矢じり・ワイヤフレーム。奥から手前の順で到着)----
     *
     * thick-fill(flags bit2=0x04): メッシュのスキャンライン塗りは水平 span
     * (y0==y1)を並べるが、拡大で行間隔が 1px を超えると 1px stroke では隙間が
     * 残り横縞になる。このフラグ時、水平線に限り depth を「帯の高さ(px)」として
     * 読み替え、その高さの塗り矩形で描いて行を隙間なくタイルする。非水平線
     * (軸/ワイヤ)と flag off 時は従来通り 1px stroke。 */
    int thick_fill = (hdr.flags & 0x04) != 0;
    cairo_set_line_width(cr, 1.0);
    for (uint16_t i = 0; i < hdr.n_lines; i++) {
        gsp_3d_line_t ln;
        memcpy(&ln, p, sizeof(ln));
        p += sizeof(ln);

        cairo_set_source_rgba(cr, ln.r / 255.0, ln.g / 255.0,
                                  ln.b / 255.0, ln.a / 255.0);

        float dy = ln.y1 - ln.y0; if (dy < 0.0f) dy = -dy;
        if (thick_fill && dy < 0.5f) {           /* 水平 span -> 塗り帯 */
            double bh = ln.depth; if (bh < 1.0) bh = 1.0; if (bh > 64.0) bh = 64.0;
            double xL = (ln.x0 < ln.x1) ? ln.x0 : ln.x1;
            double xR = (ln.x0 < ln.x1) ? ln.x1 : ln.x0;
            double bw = xR - xL; if (bw < 1.0) bw = 1.0;
            /* Y 下向き: span 行 y0 から下へ bh だけ塗る(Cocoa は view_h-y0-h で
             * Y 上向きに同じ [y0, y0+bh] を塗っていた。反転規約の違いだけ)。 */
            cairo_rectangle(cr, xL, ln.y0, bw, bh);
            cairo_fill(cr);
        } else {                                 /* 従来通り 1px stroke */
            cairo_move_to(cr, ln.x0, ln.y0);     /* Y 反転しない(上記参照) */
            cairo_line_to(cr, ln.x1, ln.y1);
            cairo_stroke(cr);
        }
    }

    /* ---- 点(電極)---- */
    for (uint16_t i = 0; i < hdr.n_points; i++) {
        gsp_3d_point_t pt;
        memcpy(&pt, p, sizeof(pt));
        p += sizeof(pt);

        double r = pt.size < 1.0f ? 1.0 : (double)pt.size;
        cairo_set_source_rgba(cr, pt.r / 255.0, pt.g / 255.0,
                                  pt.b / 255.0, pt.a / 255.0);
        cairo_new_sub_path(cr);              /* arc の前に現在点を切る */
        cairo_arc(cr, pt.x, pt.y, r, 0.0, 2 * 3.14159265358979323846);
        cairo_fill(cr);
    }

    /* ---- ラベル(同一ペイロード内。フレームは 1 枚のアトミックな
     *      スナップショットなので、ちらつきの原因だった「途中まで
     *      追加された状態」が存在しない)---- */
    if ((size_t)(end - p) >= sizeof(uint16_t)) {
        uint16_t n_labels;
        memcpy(&n_labels, p, sizeof(n_labels));
        p += sizeof(n_labels);

        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);

        for (uint16_t i = 0; i < n_labels; i++) {
            /* 固定部 14 bytes: point_idx(u16) x(f32) y(f32) r,g,b(u8) len(u8) */
            if (p + 14 > end) break;         /* 切れている: 中断(クラッシュさせない) */

            float   lx, ly;
            uint8_t lr, lg, lb, tlen;
            memcpy(&lx, p + 2, 4);
            memcpy(&ly, p + 6, 4);
            lr   = p[10];
            lg   = p[11];
            lb   = p[12];
            tlen = p[13];
            p += 14;

            if (p + tlen > end) break;        /* text が切れている: 中断 */

            char text[256];
            size_t n = tlen < sizeof(text) - 1 ? tlen : sizeof(text) - 1;
            memcpy(text, p, n);
            text[n] = '\0';
            p += tlen;

            /* 位置は Driver::GS3D 側で既にオフセット済み(点の右上)。
             * ここでは素直にベースラインとして使う。 */
            cairo_set_source_rgb(cr, lr / 255.0, lg / 255.0, lb / 255.0);
            cairo_move_to(cr, lx, ly);
            cairo_show_text(cr, text);        /* UTF-8 をそのまま渡せる */
        }
    }

    cairo_restore(cr);
}
