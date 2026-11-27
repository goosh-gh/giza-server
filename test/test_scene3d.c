/* test/test_scene3d.c -- the stage B scene (viewer/gsp_scene3d.c) without a window.
 *
 * Expected values are worked out by hand from the formulas in gsp_scene3d.h
 * (the same as PDL::Graphics::Cairo::Driver::GS3D), not taken from the code:
 * one triangle (0,0,0) (10,0,0) (0,10,0) seen with the identity rotation,
 * orthographic, zoom 1, in a 100x100 buffer lands at screen (50,50) (40,50)
 * (50,60) (sx = 50 - x, sy = y + 50). Exit 0 = all passed.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gsp_scene3d.h"
#include "gs3d_drag_reference.h"   /* tools/gs3d_drag_reference.pl, from GS3D */

static int failed = 0, run = 0;
#define CHECK(cond, ...) do { run++; if (cond) printf("ok %d - ", run); \
    else { printf("not ok %d - ", run); failed++; } printf(__VA_ARGS__); printf("\n"); } while (0)

enum { W = 100, H = 100 };
static uint8_t px[W * H * 4];
static float zb[W * H];
static uint32_t ids[W * H];

/* A GSP_MSG_3D_MESH payload. nrm may be NULL (no GSP_MESH_NORMALS). */
static uint8_t *mesh_payload(uint8_t id, uint8_t flags, uint32_t nv, uint32_t nf, const float *pos,
                             const float *nrm, const uint8_t *rgba, const uint32_t *face, size_t *len)
{
    if (nrm) flags |= GSP_MESH_NORMALS;
    size_t n = 10 + nv * 12 + (nrm ? nv * 12 : 0) + nv * 4 + nf * 12;
    uint8_t *b = malloc(n), *p = b;
    p[0] = id; p[1] = flags; memcpy(p + 2, &nv, 4); memcpy(p + 6, &nf, 4); p += 10;
    memcpy(p, pos, nv * 12); p += nv * 12;
    if (nrm) { memcpy(p, nrm, nv * 12); p += nv * 12; }
    memcpy(p, rgba, nv * 4); p += nv * 4;
    memcpy(p, face, nf * 12);
    *len = n;
    return b;
}

static void set_view(gsp_scene3d_t *sc, float w, float x, float y, float z, float zoom, int persp)
{
    gsp_3d_view_t v;
    memset(&v, 0, sizeof v);
    v.q[0] = w; v.q[1] = x; v.q[2] = y; v.q[3] = z; v.zoom = zoom; v.perspective = (uint8_t)persp;
    gsp_scene3d_set_view(sc, &v, sizeof v);
}

/* the test triangle at height z, colour rgb, as mesh `id` */
static int add_tri(gsp_scene3d_t *sc, uint8_t id, uint8_t flags, float dx, float z,
                   uint8_t r, uint8_t g, uint8_t b, int with_normals)
{
    float pos[9] = { dx, 0, z,  dx + 10, 0, z,  dx, 10, z };
    float nrm[9] = { 0, 0, 1,  0, 0, 1,  0, 0, 1 };
    uint8_t rgba[12] = { r, g, b, 255,  r, g, b, 255,  r, g, b, 255 };
    uint32_t face[3] = { 0, 1, 2 };
    size_t len;
    uint8_t *p = mesh_payload(id, flags, 3, 1, pos, with_normals ? nrm : NULL, rgba, face, &len);
    int rc = gsp_scene3d_set_mesh(sc, p, len);
    free(p);
    return rc;
}

/* A GSP_MSG_3D_OVERLAY payload, built in order: ovl_new (depth_eps and the
 * point count), points, ovl_next (opens the line count), lines, ovl_next
 * (opens the label count), labels. Each item updates the open count. */
typedef struct { uint8_t *b; size_t n, cap, count_at; uint32_t count; } ovl_t;
static void ovl_put(ovl_t *o, const void *p, size_t n)
{
    if (o->n + n > o->cap) { o->cap = (o->n + n) * 2 + 64; o->b = realloc(o->b, o->cap); }
    memcpy(o->b + o->n, p, n); o->n += n;
}
static ovl_t ovl_new(float eps)
{
    ovl_t o = { 0 };
    uint32_t z = 0;
    ovl_put(&o, &eps, 4); o.count_at = o.n; ovl_put(&o, &z, 4);
    return o;
}
static void ovl_next(ovl_t *o)
{
    uint32_t z = 0; o->count = 0; o->count_at = o->n; ovl_put(o, &z, 4);
}
static void ovl_counted(ovl_t *o) { o->count++; memcpy(o->b + o->count_at, &o->count, 4); }
static void ovl_point(ovl_t *o, float x, float y, float z, float r, uint8_t flags)
{
    gsp_3d_ovl_point_t p = { { x, y, z }, r, { 255, 255, 255, 255 }, flags };
    ovl_put(o, &p, sizeof p); ovl_counted(o);
}
static void ovl_line(ovl_t *o, float x0, float y0, float z0, float x1, float y1, float z1,
                     float w, uint8_t flags)
{
    gsp_3d_ovl_line_t l = { { x0, y0, z0 }, { x1, y1, z1 }, w, { 255, 80, 80, 255 }, flags };
    ovl_put(o, &l, sizeof l); ovl_counted(o);
}
static void ovl_label(ovl_t *o, float x, float y, float z, float dx, float dy, uint8_t flags, const char *t)
{
    gsp_3d_ovl_label_t l = { { x, y, z }, dx, dy, { 255, 255, 200 }, flags, (uint8_t)strlen(t) };
    ovl_put(o, &l, sizeof l); ovl_put(o, t, strlen(t)); ovl_counted(o);
}

static int near_u8(int got, int want) { return abs(got - want) <= 1; }
#define PIX(x, y) (px + ((size_t)(y) * W + (x)) * 4)

int main(void)
{
    /* I = 0.25 + 0.75 * (n . L), n = (0,0,1), L = (0.3,0.4,1)/|.| -> 0.920820;
     * colour 200,100,50 -> 184.16, 92.08, 46.04 */
    const int R0 = 184, G0 = 92, B0 = 46;

    /* ---- one triangle: drawn, id, colour, background ---- */
    {
        gsp_scene3d_t *sc = gsp_scene3d_new();
        set_view(sc, 1, 0, 0, 0, 1, 0);
        CHECK(add_tri(sc, 0, 0, 0, 0, 200, 100, 50, 1) == 0, "mesh accepted");
        long n = gsp_scene3d_render(sc, W, H, px, zb, ids);
        CHECK(n == 1, "one triangle drawn (got %ld)", n);
        CHECK(ids[53 * W + 46] == 0, "id buffer at (46,53): face 0 of mesh 0");
        uint8_t *o = PIX(46, 53);
        CHECK(near_u8(o[0], R0) && near_u8(o[1], G0) && near_u8(o[2], B0) && o[3] == 255,
              "shaded colour %d,%d,%d,%d (want %d,%d,%d,255 +-1)", o[0], o[1], o[2], o[3], R0, G0, B0);
        CHECK(ids[10 * W + 10] == GSP_SCENE3D_NO_ID && PIX(10, 10)[3] == 0, "background: no id, transparent");
        CHECK(ids[52 * W + 48] == 0 && ids[52 * W + 38] == GSP_SCENE3D_NO_ID,
              "inside at (48,52), outside at (38,52)");
        uint8_t mi = 9; uint32_t vx = 9;
        /* (46.5,53.5): squared distance 24.5 to v0 (50,50), 54.5 to v1 and v2 */
        CHECK(gsp_scene3d_pick(sc, ids, W, H, 46, 53, &mi, &vx) == 0 && mi == 0 && vx == 0,
              "pick at (46,53): mesh 0 vertex 0 (got %u %u)", mi, vx);
        CHECK(gsp_scene3d_pick(sc, ids, W, H, 10, 10, &mi, &vx) == -1, "pick on the background: none");

        /* colours replaced */
        uint8_t col[5 + 12] = { 0 };
        uint32_t nv = 3; memcpy(col + 1, &nv, 4);
        for (int k = 0; k < 3; k++) { col[5 + 4*k] = 0; col[6 + 4*k] = 255; col[7 + 4*k] = 0; col[8 + 4*k] = 255; }
        CHECK(gsp_scene3d_set_colors(sc, col, sizeof col) == 0, "colours accepted");
        gsp_scene3d_render(sc, W, H, px, zb, ids);
        o = PIX(46, 53);
        CHECK(o[0] == 0 && near_u8(o[1], 234) && o[2] == 0, "new colour 0,%d,0 (want 0,234,0 +-1)", o[1]);
        nv = 4; memcpy(col + 1, &nv, 4);
        CHECK(gsp_scene3d_set_colors(sc, col, sizeof col) == -1, "colours with the wrong count refused");
        gsp_scene3d_free(sc);
    }

    /* ---- winding: the same triangle with GSP_MESH_WINDING_CW is a back face ---- */
    {
        gsp_scene3d_t *sc = gsp_scene3d_new();
        set_view(sc, 1, 0, 0, 0, 1, 0);
        add_tri(sc, 0, GSP_MESH_WINDING_CW, 0, 0, 200, 100, 50, 1);
        CHECK(gsp_scene3d_render(sc, W, H, px, zb, ids) == 0 && ids[53 * W + 46] == GSP_SCENE3D_NO_ID,
              "clockwise winding: culled");
        gsp_scene3d_free(sc);
    }

    /* ---- depth: the larger z wins whatever the order ---- */
    {
        gsp_scene3d_t *sc = gsp_scene3d_new();
        set_view(sc, 1, 0, 0, 0, 1, 0);
        add_tri(sc, 0, 0, 0, 0, 200, 100, 50, 1);
        add_tri(sc, 1, 0, 0, 5, 10, 20, 30, 1);
        gsp_scene3d_render(sc, W, H, px, zb, ids);
        CHECK(ids[53 * W + 46] == (1u << 24), "mesh 1 at z=5 in front of mesh 0 at z=0");
        add_tri(sc, 1, 0, 0, -5, 10, 20, 30, 1);
        gsp_scene3d_render(sc, W, H, px, zb, ids);
        CHECK(ids[53 * W + 46] == 0, "mesh 1 moved to z=-5: mesh 0 in front");
        gsp_scene3d_free(sc);
    }

    /* ---- off screen: x = 200..210 -> sx = -150..-160 ---- */
    {
        gsp_scene3d_t *sc = gsp_scene3d_new();
        set_view(sc, 1, 0, 0, 0, 1, 0);
        add_tri(sc, 0, 0, 200, 0, 200, 100, 50, 1);
        CHECK(gsp_scene3d_render(sc, W, H, px, zb, ids) == 0, "off-screen triangle culled");
        gsp_scene3d_free(sc);
    }

    /* ---- normals computed when not sent: same colour as with (0,0,1) ---- */
    {
        gsp_scene3d_t *sc = gsp_scene3d_new();
        set_view(sc, 1, 0, 0, 0, 1, 0);
        add_tri(sc, 0, 0, 0, 0, 200, 100, 50, 0);
        gsp_scene3d_render(sc, W, H, px, zb, ids);
        uint8_t *o = PIX(46, 53);
        CHECK(near_u8(o[0], R0) && near_u8(o[1], G0) && near_u8(o[2], B0), "computed normals: same shading");
        gsp_scene3d_free(sc);
    }

    /* ---- default view (Cz from above, perspective): screen position of (10,20,30)
     *      R = diag(-1,-1,1): (x',y',z') = (-10,-20,30); s = 600/(600-30+200) = 0.7792208
     *      sx = 50 + 10*s = 57.792208, sy = 50 - 20*s = 34.415584 ---- */
    {
        gsp_scene3d_t *sc = gsp_scene3d_new();
        float pos[9] = { 10, 20, 30,  11, 20, 30,  10, 21, 30 };
        uint8_t rgba[12] = { 0 };
        uint32_t face[3] = { 0, 1, 2 };
        size_t len;
        uint8_t *p = mesh_payload(0, 0, 3, 1, pos, NULL, rgba, face, &len);
        gsp_scene3d_set_mesh(sc, p, len);
        free(p);
        gsp_scene3d_render(sc, W, H, px, zb, ids);
        CHECK(fabs(sc->mesh[0].sx[0] - 57.792208) < 1e-4 && fabs(sc->mesh[0].sy[0] - 34.415584) < 1e-4,
              "perspective, default pose: (%.6f, %.6f)", sc->mesh[0].sx[0], sc->mesh[0].sy[0]);

        /* drag dx=10 from (0,0,0,1): ry = (cos(-.05),0,sin(-.05),0); ry*q = (0, sin(-.05), 0, cos(-.05)) */
        gsp_scene3d_drag(sc, 10, 0);
        CHECK(fabs(sc->view.q[0]) < 1e-6 && fabs(sc->view.q[1] - sin(-0.05)) < 1e-6
              && fabs(sc->view.q[2]) < 1e-6 && fabs(sc->view.q[3] - cos(0.05)) < 1e-6,
              "drag dx=10: q = (%.6f, %.6f, %.6f, %.6f)", sc->view.q[0], sc->view.q[1], sc->view.q[2], sc->view.q[3]);
        gsp_scene3d_free(sc);
    }

    /* ---- malformed payloads change nothing; nv = 0 deletes ---- */
    {
        gsp_scene3d_t *sc = gsp_scene3d_new();
        set_view(sc, 1, 0, 0, 0, 1, 0);
        add_tri(sc, 0, 0, 0, 0, 200, 100, 50, 1);
        float pos[9] = { 0 }; uint8_t rgba[12] = { 0 }; uint32_t bad[3] = { 0, 1, 3 };
        size_t len;
        uint8_t *p = mesh_payload(0, 0, 3, 1, pos, NULL, rgba, bad, &len);
        CHECK(gsp_scene3d_set_mesh(sc, p, len) == -1, "face index >= nv refused");
        uint32_t good[3] = { 0, 1, 2 };
        free(p);
        p = mesh_payload(0, 0, 3, 1, pos, NULL, rgba, good, &len);
        CHECK(gsp_scene3d_set_mesh(sc, p, len - 1) == -1, "truncated payload refused");
        free(p);
        CHECK(gsp_scene3d_render(sc, W, H, px, zb, ids) == 1, "the earlier mesh is still drawn");
        uint8_t del[6] = { 0, 0, 0, 0, 0, 0 };
        CHECK(gsp_scene3d_set_mesh(sc, del, sizeof del) == 0 && sc->mesh[0].nv == 0, "nv = 0 deletes the mesh");
        uint8_t far[6] = { GSP_SCENE3D_MAX_MESH, 0, 0, 0, 0, 0 };
        CHECK(gsp_scene3d_set_mesh(sc, far, sizeof far) == -1, "mesh_id out of range refused");
        gsp_3d_view_t v; memset(&v, 0, sizeof v);
        CHECK(gsp_scene3d_set_view(sc, &v, sizeof v) == -1, "zero quaternion refused");
        gsp_scene3d_free(sc);
    }

    /* ---- the same 120 inputs (drags, wheel, 'p', 'r') as GS3D: poses agree ----
     * Expected poses come from PDL::Graphics::Cairo::Driver::GS3D itself
     * (tools/gs3d_drag_reference.pl), so this checks stage B against stage A. */
    {
        gsp_scene3d_t *sc = gsp_scene3d_new();
        size_t nsteps = sizeof drag_steps / sizeof drag_steps[0], c = 0;
        double worst = 0;
        int bad_mask = 0;
        for (size_t i = 0; i < nsteps; i++) {
            gsp_3d_input_t in = { drag_steps[i].dx, drag_steps[i].dy, drag_steps[i].dzoom,
                                  drag_steps[i].type, drag_steps[i].key };
            int mask = gsp_scene3d_input(sc, &in);
            if (!(mask & GSP_SCENE3D_POSE_CHANGED)) bad_mask++;
            if (c < sizeof drag_checks / sizeof drag_checks[0] && drag_checks[c].step == (int)(i + 1)) {
                const struct drag_check *k = &drag_checks[c++];
                for (int j = 0; j < 4; j++) {
                    double e = fabs(sc->view.q[j] - k->q[j]);
                    if (e > worst) worst = e;
                }
                double ez = fabs(sc->view.zoom - k->zoom);
                if (ez > worst) worst = ez;
                if (sc->view.perspective != k->persp) worst = 1;
            }
        }
        CHECK(c == sizeof drag_checks / sizeof drag_checks[0] && worst < 1e-5,
              "120 inputs: quaternion, zoom and projection agree with GS3D at %zu checkpoints (max diff %.2g)", c, worst);
        CHECK(bad_mask == 0, "every drag, wheel, 'p' and 'r' reports a pose change");
        gsp_3d_input_t key_c = { 0, 0, 0, 2, 'c' };
        CHECK(gsp_scene3d_input(sc, &key_c) == GSP_SCENE3D_FORWARD, "key 'c' is passed on to the client");
        gsp_scene3d_free(sc);
    }

    /* ---- overlay (GSP_MSG_3D_OVERLAY): projected, hidden by the mesh ----
     * The test triangle at z = 0 covers model x, y >= 0, x + y <= 10, which is
     * screen (50 - x, 50 + y). Depth z > 0 is in front of it. */
    {
        gsp_scene3d_t *sc = gsp_scene3d_new();
        set_view(sc, 1, 0, 0, 0, 1, 0);
        add_tri(sc, 0, 0, 0, 0, 200, 100, 50, 1);
        gsp_scene3d_render(sc, W, H, px, zb, ids);
        ovl_t o = ovl_new(1.0f);
        ovl_point(&o, 2, 2, 5, 4, 0);                       /* in front: drawn */
        ovl_point(&o, 30, 30, -5, 4, 0);                    /* behind, but no mesh there: drawn */
        ovl_point(&o, 2, 2, -5, 4, 0);                      /* behind the mesh: hidden */
        ovl_point(&o, 3, 3, -5, 4, GSP_OVL_ON_TOP);         /* behind but on top: drawn */
        ovl_point(&o, 1, 1, -0.5f, 4, 0);                   /* within depth_eps 1: drawn */
        ovl_point(&o, 10, 20, 0, 3, GSP_OVL_SCREEN);        /* window pixels */
        ovl_next(&o);
        ovl_line(&o, -5, 2, -5, 15, 2, -5, 1, 0);           /* behind: cut at x 0..8 */
        ovl_line(&o, -5, 4, -5, 15, 4, -5, 1, GSP_OVL_ON_TOP);
        ovl_line(&o, 0, 0, 0, 20, 0, 0, 1, GSP_OVL_ON_TOP | GSP_OVL_ARROW);
        ovl_line(&o, 10, 30, 0, 20, 30, 0, 2, GSP_OVL_SCREEN | GSP_OVL_BAND);
        ovl_next(&o);
        ovl_label(&o, 2, 2, 5, 6, -6, 0, "Cz");             /* at (48,52) + (6,-6) */
        ovl_label(&o, 2, 2, -5, 6, -6, 0, "hidden");
        ovl_label(&o, 12, 20, 0, 0, 0, GSP_OVL_SCREEN, "\xE9\x9B\xBB\xE6\xA5\xB5");  /* "電極" */
        ovl_label(&o, 12, 40, 0, 0, 0, GSP_OVL_SCREEN, "bad\xFF");
        ovl_label(&o, 12, 60, 0, 0, 0, GSP_OVL_SCREEN, "\xED\xA0\x80");             /* a surrogate */
        CHECK(gsp_scene3d_set_overlay(sc, o.b, o.n) == 0, "overlay accepted (%zu bytes)", o.n);
        gsp_ovl_draw_t d;
        memset(&d, 0, sizeof d);
        CHECK(gsp_scene3d_overlay_draw(sc, W, H, zb, &d) == 0, "overlay drawn");
        CHECK(d.ndisc == 5, "5 of 6 points drawn (got %zu)", d.ndisc);
        /* 3D far to near: (30,30,-5) and (3,3,-5) [same depth: by order], (1,1,-0.5), (2,2,5); then the screen one */
        CHECK(d.ndisc == 5 && d.disc[0].x == 20 && d.disc[0].y == 80 && d.disc[1].x == 47
              && d.disc[2].x == 49 && d.disc[3].x == 48 && d.disc[3].y == 52 && d.disc[3].r == 4,
              "points far to near: (%g,%g) (%g) (%g) (%g,%g r %g)", d.disc[0].x, d.disc[0].y,
              d.disc[1].x, d.disc[2].x, d.disc[3].x, d.disc[3].y, d.disc[3].r);
        CHECK(d.ndisc == 5 && d.disc[4].x == 10 && d.disc[4].y == 20 && d.disc[4].r == 3,
              "screen point last, at (10,20) r 3");
        /* line 1: screen x 55 -> 35 at y 52; hidden where the triangle covers x 0..8 (screen 50..42) */
        CHECK(d.nseg == 2 + 1 + 3 + 1, "segments: 2 pieces of the hidden line, 1 on top, 1 + 2 arrow, 1 band (got %zu)", d.nseg);
        CHECK(d.nseg >= 2 && fabs(d.seg[0].x0 - 55) < 0.01 && fabs(d.seg[0].x1 - 50) < 1.5
              && fabs(d.seg[1].x0 - 42) < 1.5 && fabs(d.seg[1].x1 - 35) < 0.01 && d.seg[0].y0 == 52,
              "hidden line cut: %g..%g and %g..%g", d.seg[0].x0, d.seg[0].x1, d.seg[1].x0, d.seg[1].x1);
        CHECK(d.nseg >= 3 && d.seg[2].x0 == 55 && d.seg[2].x1 == 35 && !d.seg[2].band, "on-top line whole");
        CHECK(d.nseg >= 6 && d.seg[3].x0 == 50 && d.seg[3].x1 == 30
              && fabs(d.seg[4].x1 - 42) < 1e-4 && fabs(d.seg[4].y1 - 45) < 1e-4
              && fabs(d.seg[5].x1 - 42) < 1e-4 && fabs(d.seg[5].y1 - 55) < 1e-4,
              "arrow head at (30,50): wings to (42,45) and (42,55)");
        CHECK(d.nseg == 7 && d.seg[6].band && d.seg[6].x0 == 10 && d.seg[6].x1 == 20
              && d.seg[6].y0 == 30 && d.seg[6].width == 2, "band in window pixels");
        CHECK(d.ntext == 2, "labels: 2 drawn (not the hidden one, not the bad UTF-8 ones) (got %zu)", d.ntext);
        CHECK(d.ntext >= 1 && d.text[0].x == 54 && d.text[0].y == 46 && strcmp(d.text[0].text, "Cz") == 0,
              "label at its point plus (6,-6)");
        CHECK(d.ntext >= 2 && strcmp(d.text[1].text, "\xE9\x9B\xBB\xE6\xA5\xB5") == 0
              && d.text[1].x == 12 && d.text[1].y == 20, "UTF-8 label in window pixels");

        /* zoom 2 doubles a 3D point's radius, not a screen point's */
        set_view(sc, 1, 0, 0, 0, 2, 0);
        gsp_scene3d_overlay_draw(sc, W, H, NULL, &d);
        CHECK(d.ndisc == 6 && d.disc[5].r == 3 && d.disc[4].r == 8, "zoom 2: radius 8 for 3D, 3 for screen");
        CHECK(d.nseg == 1 + 1 + 3 + 1, "zb NULL: nothing hidden (got %zu segments)", d.nseg);

        /* malformed overlays change nothing */
        CHECK(gsp_scene3d_set_overlay(sc, o.b, o.n - 1) == -1, "truncated overlay refused");
        uint8_t *x = malloc(o.n + 1); memcpy(x, o.b, o.n); x[o.n] = 0;
        CHECK(gsp_scene3d_set_overlay(sc, x, o.n + 1) == -1, "a byte too many refused");
        free(x);
        CHECK(sc->ovl.np == 6 && sc->ovl.nl == 4 && sc->ovl.nt == 5, "the earlier overlay is kept");
        ovl_t e = ovl_new(1.0f); ovl_next(&e); ovl_next(&e);
        CHECK(gsp_scene3d_set_overlay(sc, e.b, e.n) == 0, "empty overlay accepted");
        gsp_scene3d_overlay_draw(sc, W, H, zb, &d);
        CHECK(d.nseg == 0 && d.ndisc == 0 && d.ntext == 0, "empty overlay draws nothing");
        ovl_t big = ovl_new(1.0f);
        uint32_t huge = 0x10000000u; memcpy(big.b + 4, &huge, 4);
        ovl_next(&big); ovl_next(&big);
        CHECK(gsp_scene3d_set_overlay(sc, big.b, big.n) == -1, "a point count past the end refused");
        /* perspective: a point behind the eye (z >= 800 at zoom 1) is not drawn */
        set_view(sc, 1, 0, 0, 0, 1, 1);
        ovl_t pe = ovl_new(1.0f);
        ovl_point(&pe, 0, 0, 900, 4, GSP_OVL_ON_TOP);
        ovl_point(&pe, 0, 0, 100, 4, GSP_OVL_ON_TOP);
        ovl_next(&pe); ovl_next(&pe);
        gsp_scene3d_set_overlay(sc, pe.b, pe.n);
        gsp_scene3d_overlay_draw(sc, W, H, zb, &d);
        CHECK(d.ndisc == 1, "perspective: the point behind the eye is skipped (got %zu)", d.ndisc);
        free(o.b); free(e.b); free(big.b); free(pe.b);
        gsp_ovl_draw_free(&d);
        gsp_scene3d_free(sc);
    }

    printf("1..%d\n", run);
    return failed ? 1 : 0;
}
