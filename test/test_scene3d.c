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

    printf("1..%d\n", run);
    return failed ? 1 : 0;
}
