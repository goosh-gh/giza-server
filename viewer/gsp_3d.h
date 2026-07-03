/* gsp_3d.h  --  GSP protocol extension for 3D rendering (Phase 2)
 *
 * REVIEW NOTE (2026-06-20): 当初案は GSP_MSG_3D_FRAME=0x20 /
 * GSP_MSG_3D_INPUT=0x21 だったが、Driver::GS の実装を確認した結果、
 * 既存プロトコルで 0x20=GSP_MSG_CLOSE, 0x21=GSP_MSG_ACK が既に使われて
 * いることが判明した（既存メッセージは 0x01〜0x19 + 0xFF まで埋まって
 * いる）。0x21 と衝突すると _recv_ack_sys() のACK判定ロジックが破壊され、
 * デッドロックまたは誤動作する。番号帯を 0x24〜0x27 に変更し、0x1A〜0x23
 * は将来の2D機能拡張用に予約として空ける。
 *
 * REVIEW NOTE 2 (2026-06-21, ラベルちらつき修正): GSP_MSG_3D_LABEL は
 * 廃止した。当初は「FRAME送信→LABELを点数分だけ別メッセージで送る」
 * という設計だったが、実機検証で重大な不整合が見つかった: Cocoa側は
 * FRAMEとLABEL×Nをそれぞれ独立した dispatch_async ブロックとして
 * メインスレッドのキューに積んでいたため、setNeedsDisplay: による
 * 再描画(drawRect:)が、20個のLABELブロックが完全に実行され終わる前に
 * 割り込んで発生することがあった。点・線(_3d_payload)は1回の
 * NSData置き換えでアトミックに更新されるためちらつかなかったが、
 * ラベルは「clear→add×20」という21段階の途中状態がそのまま画面に
 * 出てしまい(labels.countが0,1,2,...,20と不規則に観測された)、
 * 「テキストだけちらつく」という症状になった。
 *
 * 修正方針: ラベルをFRAMEペイロード自体の末尾に埋め込み、別メッセージに
 * 分割しない。これにより点・線と同じ「1回の受信・1回のNSData置き換えで
 * 完全なフレームがアトミックに反映される」という保証がラベルにも及ぶ。
 * GSP_MSG_3D_LABEL (旧0x26) は欠番として予約し、再利用しない。
 *
 * 既存の gsp.h に追記するか #include "gsp_3d.h" で取り込む。
 */
#ifndef GSP_3D_H
#define GSP_3D_H
#include <stdint.h>

/* --------------------------------------------------------
 * 新規メッセージタイプ（0x1A〜0x23は予約、衝突回避のため0x24から開始）
 *
 * 0x26 (旧GSP_MSG_3D_LABEL) は欠番。ラベルはFRAMEペイロードに統合された
 * ため、このメッセージタイプは送受信のどちらからも使用しない。
 * -------------------------------------------------------- */
#define GSP_MSG_3D_FRAME  0x24   /* サーバ→クライアント: 描画フレーム（線・点・ラベル全て含む） */
#define GSP_MSG_3D_INPUT  0x25   /* クライアント→サーバ: マウス/キー入力 */
/* 0x26 は欠番（旧 GSP_MSG_3D_LABEL、廃止済み、再利用禁止） */
#define GSP_MSG_3D_CLEAR  0x27   /* サーバ→クライアント: 画面クリア（未使用、予約） */

/* --------------------------------------------------------
 * GSP_MSG_3D_FRAME ペイロード構造（2026-06-21版: ラベルを統合）
 *
 * ヘッダ (13 bytes):
 *   uint16_t n_lines    線分の本数        (2 bytes)
 *   uint16_t n_points   点の個数          (2 bytes)
 *   uint8_t  flags      bit0=深度ソート済み bit1=透視投影済み (1 byte)
 *   float    cx         スクリーン中心X (px) (4 bytes)
 *   float    cy         スクリーン中心Y (px) (4 bytes)
 *   合計: 2+2+1+4+4 = 13 bytes
 *
 * 線分レコード (24 bytes each) × n_lines:
 *   float  x0, y0       スクリーン座標 始点   (4+4)
 *   float  x1, y1       スクリーン座標 終点   (4+4)
 *   float  depth        中点Z深度（ソート参照用） (4)
 *   uint8_t r,g,b,a     RGBA 0-255            (1+1+1+1)
 *   合計: 4*5 + 1*4 = 24 bytes
 *
 * 点レコード (20 bytes each) × n_points:
 *   float  x, y         スクリーン座標         (4+4)
 *   float  depth        Z深度                  (4)
 *   float  size         半径 (px)              (4)
 *   uint8_t r,g,b,a     RGBA 0-255             (1+1+1+1)
 *   合計: 4*4 + 1*4 = 20 bytes
 *
 * ラベルセクション（新規、点レコードの直後）:
 *   uint16_t n_labels   ラベルの個数 (2 bytes)
 *   その後 n_labels 個のラベルレコード（可変長、gsp_3d_label_t参照）
 *
 * 全体のバイト数 = 13 + 24*n_lines + 20*n_points + 2 + (各ラベルの可変長合計)
 * -------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint16_t n_lines;
    uint16_t n_points;
    uint8_t  flags;
    float    cx;
    float    cy;
} gsp_3d_frame_hdr_t;    /* 13 bytes */

typedef struct __attribute__((packed)) {
    float   x0, y0;
    float   x1, y1;
    float   depth;
    uint8_t r, g, b, a;
} gsp_3d_line_t;          /* 24 bytes */

typedef struct __attribute__((packed)) {
    float   x, y;
    float   depth;
    float   size;
    uint8_t r, g, b, a;
} gsp_3d_point_t;         /* 20 bytes */

/* --------------------------------------------------------
 * GSP_MSG_3D_INPUT ペイロード (14 bytes)
 *   float×3 (12 bytes) + uint8_t×2 (2 bytes) = 14 bytes
 *
 * REVIEW NOTE (2026-06-21, トラックボール回転への変更):
 * 当初はオイラー角2軸合成(theta=Z軸回転, phi=Y軸回転)を前提に
 * dtheta/dphiという角度差分を送る設計だったが、実機検証でCz(頭頂、
 * 元の座標系でZ軸の真上の点)を画面の真上に持ってこられないことが
 * 判明した。Z軸回転はZ軸上の点自体を動かさないため、phi(Y軸回転)
 * だけがCzを動かせる唯一の自由度であり、これは原理的に「真横」方向
 * にしかCzを動かせない(2自由度しかない構造的制約、実装ミスではない)。
 *
 * 任意軸回転(トラックボール風: スクリーン上のどの方向にドラッグしても
 * その方向に回転する)を実現するため、サーバー(Perl)側は角度ではなく
 * 回転行列そのものを状態として保持する方式に変更した。このため
 * クライアントが送るのは「角度差分」ではなく「ドラッグの生のピクセル
 * 差分」になり、フィールド名をdtheta/dphiからdx/dyに変更した
 * (バイトレイアウト・サイズは不変、意味だけが変わる)。
 * -------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    float   dx;       /* ドラッグの水平差分 (画面px、トラックボール回転用) */
    float   dy;       /* ドラッグの垂直差分 (画面px、トラックボール回転用) */
    float   dzoom;    /* ズーム倍率差分 (>0 拡大, <0 縮小) */
    uint8_t type;     /* 0=drag 1=wheel 2=key */
    uint8_t key;      /* type==2の時: 'p'=透視切替 'r'=リセット */
} gsp_3d_input_t;    /* 14 bytes */

/* --------------------------------------------------------
 * ラベルレコード（FRAMEペイロード末尾、n_labels個。固定部14 bytes + 可変長text）
 *
 * REVIEW NOTE: 旧GSP_MSG_3D_LABELメッセージの固定部と同一構造。
 * メッセージとして独立送信する代わりにFRAMEペイロードに連結するだけの
 * 変更なので、レコード自体のバイトレイアウトは変えていない。
 *
 *   uint16_t  point_idx   対応する点インデックス。
 *                         0xFFFF をセンチネル値として「絶対座標指定」を表す
 *                         （point_idxはuint16_tなので-1は表現できないため、
 *                          uint16_tの最大値0xFFFFを代わりに予約する）
 *   float     x, y        (point_idx==0xFFFFの場合のみ使用)
 *   uint8_t   r,g,b        テキスト色
 *   uint8_t   len          テキスト長
 *   char      text[len]    UTF-8テキスト（フレキシブル配列、sizeof()には含まれない）
 *
 *   固定部合計: 2+4+4+1+1+1+1 = 14 bytes
 *
 *   注意: sizeof(gsp_3d_label_t) はコンパイラ依存でtext[]を含まない
 *   固定部のみを返す。受信側は必ず「固定部14バイトを読む→lenを見る→
 *   text分を追加で読む」という二段読みにすること（FRAMEペイロード内の
 *   オフセット計算でも同様に、固定部+len分を進める）。
 * -------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint16_t point_idx;
    float    x, y;
    uint8_t  r, g, b;
    uint8_t  len;
    /* char text[] follows -- NOT included in sizeof() */
} gsp_3d_label_t;   /* 14 bytes fixed part */

#endif /* GSP_3D_H */
