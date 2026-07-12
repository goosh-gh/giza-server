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
 *
 * レコードはサーバ側(Driver::GS3D の $depth->qsorti)で既に奥から手前へ
 * ソート済みなので、ここでは並べ替えず配列順に描く(画家のアルゴリズム)。
 *
 * 境界チェック: text の長さはワイヤから来るデータなので、壊れた長さで
 * バッファ外を歩かないよう 1 レコードごとに検査する(Cocoa 版と同じ)。
 */

#include <cairo/cairo.h>
#include <stdint.h>
#include <string.h>

#include "gsp_3d.h"

void gsp3d_draw_cairo(cairo_t *cr, double ox, double oy, double vw, double vh,
                      const unsigned char *buf, size_t len);

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

    /* ---- 線分(軸・矢じり・ワイヤフレーム。奥から手前の順で到着)---- */
    cairo_set_line_width(cr, 1.0);
    for (uint16_t i = 0; i < hdr.n_lines; i++) {
        gsp_3d_line_t ln;
        memcpy(&ln, p, sizeof(ln));
        p += sizeof(ln);

        cairo_set_source_rgba(cr, ln.r / 255.0, ln.g / 255.0,
                                  ln.b / 255.0, ln.a / 255.0);
        cairo_move_to(cr, ln.x0, ln.y0);     /* Y 反転しない(上記参照) */
        cairo_line_to(cr, ln.x1, ln.y1);
        cairo_stroke(cr);
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
