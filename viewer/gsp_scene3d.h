/* gsp_scene3d.h -- stage B 3D scene, shared by the Cocoa and Xlib backends.
 *
 * Holds the meshes a client sent with GSP_MSG_3D_MESH, the view (pose, zoom,
 * pivot, projection), and draws them into an RGBA buffer with a z-buffer and
 * an id buffer (which face is under each pixel). Pure C: no window system, no
 * cairo, no Cocoa. The backends only blit the buffer.
 *
 * The formulas follow PDL::Graphics::Cairo::Driver::GS3D (stage A), so a
 * stage B window looks the same as the 3D_FRAME one:
 *   rotation   R from the quaternion as GS3D::_q_to_rot; p' = R (p - pivot)
 *   projection perspective: f = 600*zoom, s = f / (f - z' + 200),
 *              sx = w/2 - x'*s*zoom, sy = y'*s*zoom + h/2
 *              orthographic: sx = w/2 - x'*zoom, sy = y'*zoom + h/2
 *              depth = z' (larger = nearer); buffer row 0 = top
 *   culling    z of the face normal of (x', y') times the winding sign > 0,
 *              and the screen bounding box overlapping the window
 *   shading    n' = R n; I = ambient + (1 - ambient) * clamp(n' . L, 0, 1),
 *              L = normalised (0.3, 0.4, 1.0), ambient 0.25;
 *              colour = clamp(I * rgb / 255, 0, 1) * 255
 *   drag       GS3D::_apply_drag_rotation: q <- rx * ry * q, ry about world Y
 *              by -dx * 0.01, rx about world X by +dy * 0.01, normalised
 */
#ifndef GSP_SCENE3D_H
#define GSP_SCENE3D_H
#include <stddef.h>
#include <stdint.h>
#include "gsp_3d.h"

#define GSP_SCENE3D_MAX_MESH 8
#define GSP_SCENE3D_NO_ID    0xFFFFFFFFu   /* id buffer: nothing drawn */

typedef struct {
    uint32_t  nv, nf;          /* nv == 0: slot empty */
    uint8_t   flags;           /* GSP_MESH_* */
    float    *pos;             /* [nv][3] */
    float    *nrm;             /* [nv][3] unit outward */
    uint8_t  *rgba;            /* [nv][4] */
    uint32_t *face;            /* [nf][3] */
    /* filled by gsp_scene3d_render, kept for pick / hover */
    float    *sx, *sy;         /* [nv] screen position of the last frame */
    /* scratch of gsp_scene3d_render */
    double   *rx, *ry;         /* [nv] rotated x, y (for culling) */
    float    *sz;              /* [nv] depth */
    uint8_t  *lit;             /* [nv][3] shaded colour */
} gsp_mesh3d_t;

/* The overlay of GSP_MSG_3D_OVERLAY, unpacked (see gsp_3d.h). */
typedef struct { float pos[3], radius; uint8_t rgba[4], flags; } gsp_ovl_point_t;
typedef struct { float p0[3], p1[3], width; uint8_t rgba[4], flags; } gsp_ovl_line_t;
typedef struct { float pos[3], dx, dy; uint8_t rgb[3], flags; const char *text; } gsp_ovl_label_t;

typedef struct {
    float            eps;
    uint32_t         np, nl, nt;
    gsp_ovl_point_t *pts;
    gsp_ovl_line_t  *lines;
    gsp_ovl_label_t *labels;
    char            *text;     /* the labels' texts, each NUL-terminated */
} gsp_overlay3d_t;

typedef struct {
    gsp_3d_view_t view;
    double        light[3];    /* unnormalised direction, view space */
    double        ambient;
    gsp_mesh3d_t  mesh[GSP_SCENE3D_MAX_MESH];
    gsp_overlay3d_t ovl;
    gsp_3d_tri_t *tris;        /* scratch for one frame */
    uint32_t     *tri_id;
    size_t        tri_cap;
} gsp_scene3d_t;

/* What a backend draws for the overlay, in window pixels (x right, y down),
 * in this order: segments, discs, texts. A segment with band set is a filled
 * rectangle x0..x1, y0..y0 + width; otherwise a stroke of that width. A text
 * is drawn with its baseline starting at (x, y). */
typedef struct { float x0, y0, x1, y1, width; uint8_t rgba[4]; uint8_t band; } gsp_ovl_seg_t;
typedef struct { float x, y, r; uint8_t rgba[4]; } gsp_ovl_disc_t;
typedef struct { float x, y; uint8_t rgb[3]; const char *text; } gsp_ovl_text_t;
typedef struct {
    gsp_ovl_seg_t  *seg;  size_t nseg,  segcap;
    gsp_ovl_disc_t *disc; size_t ndisc, disccap;
    gsp_ovl_text_t *text; size_t ntext, textcap;
} gsp_ovl_draw_t;

/* A new scene: default view (Cz from above, zoom 1, pivot 0, perspective),
 * GS3D's light and ambient. Free with gsp_scene3d_free. */
gsp_scene3d_t *gsp_scene3d_new(void);
void gsp_scene3d_free(gsp_scene3d_t *sc);

/* Apply a payload. Return 0, or -1 if it is malformed (nothing changed). */
int gsp_scene3d_set_mesh(gsp_scene3d_t *sc, const void *payload, size_t len);
int gsp_scene3d_set_colors(gsp_scene3d_t *sc, const void *payload, size_t len);
int gsp_scene3d_set_view(gsp_scene3d_t *sc, const void *payload, size_t len);
int gsp_scene3d_set_overlay(gsp_scene3d_t *sc, const void *payload, size_t len);

/* The overlay as drawing commands for a w x h window, after
 * gsp_scene3d_render drew the meshes into zb (same w, h). zb NULL: nothing is
 * hidden. The texts point into the scene (valid until the next overlay).
 * Reuses out's arrays; start from a zeroed gsp_ovl_draw_t and release it with
 * gsp_ovl_draw_free. Returns 0, or -1 if memory ran out. */
int gsp_scene3d_overlay_draw(const gsp_scene3d_t *sc, int w, int h, const float *zb,
                             gsp_ovl_draw_t *out);
void gsp_ovl_draw_free(gsp_ovl_draw_t *d);

/* Rotate as a drag of (dx, dy) screen pixels does in GS3D (dy: up positive). */
void gsp_scene3d_drag(gsp_scene3d_t *sc, double dx, double dy);

/* Apply one GSP_MSG_3D_INPUT record as GS3D::run does: drag rotates,
 * zoom *= 1 + dzoom * 0.1 when |dzoom| > 0.01, key 'p'/'P' toggles the
 * projection, 'r'/'R' resets the pose and zoom (pivot kept). Returns a mask:
 * GSP_SCENE3D_POSE_CHANGED when the view changed (send GSP_MSG_3D_POSE),
 * GSP_SCENE3D_FORWARD for any other key (pass the record on to the client,
 * e.g. 'c' and 'u', which clear and undo the client's seeds). */
#define GSP_SCENE3D_POSE_CHANGED 0x01
#define GSP_SCENE3D_FORWARD      0x02
int gsp_scene3d_input(gsp_scene3d_t *sc, const gsp_3d_input_t *in);

/* Draw every mesh into px (w*h RGBA, row 0 = top), zb (w*h) and, when not
 * NULL, ids (w*h; (mesh_id << 24) | face index, or GSP_SCENE3D_NO_ID). The
 * buffers are cleared first (transparent, far, no id). Returns the number of
 * triangles drawn, or -1 if memory ran out. */
long gsp_scene3d_render(gsp_scene3d_t *sc, int w, int h,
                        uint8_t *px, float *zb, uint32_t *ids);

/* The vertex under pixel (x, y) of the last frame: of the face in the id
 * buffer there, its corner nearest on screen. Returns 0 and sets *mesh_id and
 * *vertex, or -1 when no face is there. */
int gsp_scene3d_pick(const gsp_scene3d_t *sc, const uint32_t *ids, int w, int h,
                     int x, int y, uint8_t *mesh_id, uint32_t *vertex);

#endif /* GSP_SCENE3D_H */
