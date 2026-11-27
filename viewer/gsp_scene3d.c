/* gsp_scene3d.c -- stage B 3D scene (see gsp_scene3d.h for the formulas). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "gsp_scene3d.h"
#include "gsp_raster.h"

#define DRAG_SENSITIVITY 0.01
#define DRAG_DX_SIGN     (-1.0)
#define DRAG_DY_SIGN     (+1.0)

static void view_default(gsp_3d_view_t *v)
{
    memset(v, 0, sizeof *v);
    v->q[0] = 0; v->q[1] = 0; v->q[2] = 0; v->q[3] = 1;   /* Cz from above: diag(-1,-1,1) */
    v->zoom = 1;
    v->perspective = 1;
}

gsp_scene3d_t *gsp_scene3d_new(void)
{
    gsp_scene3d_t *sc = calloc(1, sizeof *sc);
    if (!sc) return NULL;
    view_default(&sc->view);
    sc->light[0] = 0.3; sc->light[1] = 0.4; sc->light[2] = 1.0;
    sc->ambient = 0.25;
    return sc;
}

static void mesh_clear(gsp_mesh3d_t *m)
{
    free(m->pos); free(m->nrm); free(m->rgba); free(m->face);
    free(m->sx); free(m->sy); free(m->rx); free(m->ry); free(m->sz); free(m->lit);
    memset(m, 0, sizeof *m);
}

void gsp_scene3d_free(gsp_scene3d_t *sc)
{
    if (!sc) return;
    for (int i = 0; i < GSP_SCENE3D_MAX_MESH; i++) mesh_clear(&sc->mesh[i]);
    free(sc->tris); free(sc->tri_id);
    free(sc);
}

static uint32_t rd_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Vertex normals as GS3D::_compute_vnormals: the (area-weighted) face normals
 * summed at each corner, normalised, then all flipped if they point inward on
 * average (against the vector from the centroid). */
static void compute_normals(gsp_mesh3d_t *m)
{
    uint32_t nv = m->nv;
    double *acc = calloc((size_t)nv * 3, sizeof(double));
    if (!acc) { memset(m->nrm, 0, (size_t)nv * 3 * sizeof(float)); return; }
    for (uint32_t f = 0; f < m->nf; f++) {
        const uint32_t *q = m->face + 3 * f;
        const float *a = m->pos + 3 * q[0], *b = m->pos + 3 * q[1], *c = m->pos + 3 * q[2];
        double e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        double e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
        double n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 3; j++) acc[3 * q[k] + j] += n[j];
    }
    double cg[3] = { 0, 0, 0 };
    for (uint32_t v = 0; v < nv; v++)
        for (int j = 0; j < 3; j++) cg[j] += m->pos[3 * v + j];
    for (int j = 0; j < 3; j++) cg[j] /= nv;
    double al = 0;
    for (uint32_t v = 0; v < nv; v++) {
        double *n = acc + 3 * v;
        double len = sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]) + 1e-12;
        for (int j = 0; j < 3; j++) n[j] /= len;
        for (int j = 0; j < 3; j++) al += n[j] * (m->pos[3 * v + j] - cg[j]);
    }
    double sgn = al < 0 ? -1 : 1;
    for (size_t i = 0; i < (size_t)nv * 3; i++) m->nrm[i] = (float)(acc[i] * sgn);
    free(acc);
}

int gsp_scene3d_set_mesh(gsp_scene3d_t *sc, const void *payload, size_t len)
{
    const uint8_t *p = payload;
    if (len < 2) return -1;
    uint8_t id = p[0], flags = p[1];
    if (id >= GSP_SCENE3D_MAX_MESH) return -1;
    if (len < 6) return -1;
    if (rd_u32(p + 2) == 0) {                                   /* nv = 0: delete, nothing follows */
        if (len != 6) return -1;
        mesh_clear(&sc->mesh[id]);
        return 0;
    }
    if (len < 10) return -1;
    uint32_t nv = rd_u32(p + 2), nf = rd_u32(p + 6);
    size_t per_v = 12 + ((flags & GSP_MESH_NORMALS) ? 12 : 0) + 4;
    uint64_t need = 10 + (uint64_t)nv * per_v + (uint64_t)nf * 12;
    if (need != (uint64_t)len) return -1;           /* also guards the size_t products below */
    const uint8_t *pp = p + 10;
    const uint8_t *pn = (flags & GSP_MESH_NORMALS) ? pp + (size_t)nv * 12 : NULL;
    const uint8_t *pc = pp + (size_t)nv * ((flags & GSP_MESH_NORMALS) ? 24 : 12);
    const uint8_t *pf = pc + (size_t)nv * 4;
    for (size_t i = 0; i < (size_t)nf * 3; i++)
        if (rd_u32(pf + 4 * i) >= nv) return -1;                  /* bad index: change nothing */

    gsp_mesh3d_t m;
    memset(&m, 0, sizeof m);
    m.nv = nv; m.nf = nf; m.flags = flags;
    m.pos  = malloc((size_t)nv * 12);
    m.nrm  = malloc((size_t)nv * 12);
    m.rgba = malloc((size_t)nv * 4);
    m.face = malloc((size_t)nf * 12 + 1);
    m.sx = malloc((size_t)nv * 4);  m.sy = malloc((size_t)nv * 4);  m.sz = malloc((size_t)nv * 4);
    m.rx = malloc((size_t)nv * 8);  m.ry = malloc((size_t)nv * 8);  m.lit = malloc((size_t)nv * 3);
    if (!m.pos || !m.nrm || !m.rgba || !m.face || !m.sx || !m.sy || !m.sz || !m.rx || !m.ry || !m.lit) {
        mesh_clear(&m); return -1;
    }
    memcpy(m.pos, pp, (size_t)nv * 12);
    memcpy(m.rgba, pc, (size_t)nv * 4);
    memcpy(m.face, pf, (size_t)nf * 12);
    if (pn) memcpy(m.nrm, pn, (size_t)nv * 12);
    else    compute_normals(&m);
    memset(m.sx, 0, (size_t)nv * 4); memset(m.sy, 0, (size_t)nv * 4);
    mesh_clear(&sc->mesh[id]);
    sc->mesh[id] = m;
    return 0;
}

int gsp_scene3d_set_colors(gsp_scene3d_t *sc, const void *payload, size_t len)
{
    const uint8_t *p = payload;
    if (len < 5) return -1;
    uint8_t id = p[0];
    if (id >= GSP_SCENE3D_MAX_MESH) return -1;
    uint32_t nv = rd_u32(p + 1);
    gsp_mesh3d_t *m = &sc->mesh[id];
    if (m->nv == 0 || nv != m->nv || len != 5 + (size_t)nv * 4) return -1;
    memcpy(m->rgba, p + 5, (size_t)nv * 4);
    return 0;
}

int gsp_scene3d_set_view(gsp_scene3d_t *sc, const void *payload, size_t len)
{
    gsp_3d_view_t v;
    if (len != sizeof v) return -1;
    memcpy(&v, payload, sizeof v);
    double n = sqrt((double)v.q[0]*v.q[0] + (double)v.q[1]*v.q[1] + (double)v.q[2]*v.q[2] + (double)v.q[3]*v.q[3]);
    if (!(n > 1e-12) || !(v.zoom > 0) || !isfinite(n)) return -1;
    sc->view = v;
    return 0;
}

static void q_mul(const double a[4], const double b[4], double out[4])
{
    out[0] = a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3];
    out[1] = a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2];
    out[2] = a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1];
    out[3] = a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0];
}

void gsp_scene3d_drag(gsp_scene3d_t *sc, double dx, double dy)
{
    double q[4] = { sc->view.q[0], sc->view.q[1], sc->view.q[2], sc->view.q[3] };
    double ay = DRAG_DX_SIGN * dx * DRAG_SENSITIVITY, ax = DRAG_DY_SIGN * dy * DRAG_SENSITIVITY;
    double ry[4] = { cos(ay / 2), 0, sin(ay / 2), 0 };
    double rx[4] = { cos(ax / 2), sin(ax / 2), 0, 0 };
    double t[4], r[4];
    q_mul(ry, q, t);
    q_mul(rx, t, r);
    double n = sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2] + r[3]*r[3]);
    if (n < 1e-12) { r[0] = 1; r[1] = r[2] = r[3] = 0; n = 1; }
    for (int i = 0; i < 4; i++) sc->view.q[i] = (float)(r[i] / n);
}

/* R from (w, x, y, z), rows as GS3D::_q_to_rot (normalised first). */
static void q_to_rot(double w, double x, double y, double z, double R[9])
{
    double n = sqrt(w*w + x*x + y*y + z*z);
    w /= n; x /= n; y /= n; z /= n;
    double x2 = x*x, y2 = y*y, z2 = z*z, xy = x*y, xz = x*z, yz = y*z, wx = w*x, wy = w*y, wz = w*z;
    R[0] = 1-2*(y2+z2); R[1] = 2*(xy-wz);   R[2] = 2*(xz+wy);
    R[3] = 2*(xy+wz);   R[4] = 1-2*(x2+z2); R[5] = 2*(yz-wx);
    R[6] = 2*(xz-wy);   R[7] = 2*(yz+wx);   R[8] = 1-2*(x2+y2);
}

static int ensure_tris(gsp_scene3d_t *sc, size_t n)
{
    if (n <= sc->tri_cap) return 0;
    gsp_3d_tri_t *t = realloc(sc->tris, n * sizeof *t);
    if (!t) return -1;
    sc->tris = t;
    uint32_t *id = realloc(sc->tri_id, n * sizeof *id);
    if (!id) return -1;
    sc->tri_id = id;
    sc->tri_cap = n;
    return 0;
}

long gsp_scene3d_render(gsp_scene3d_t *sc, int w, int h, uint8_t *px, float *zb, uint32_t *ids)
{
    size_t npx = (size_t)w * h;
    memset(px, 0, npx * 4);
    for (size_t i = 0; i < npx; i++) zb[i] = -1e30f;
    if (ids) for (size_t i = 0; i < npx; i++) ids[i] = GSP_SCENE3D_NO_ID;

    double R[9];
    q_to_rot(sc->view.q[0], sc->view.q[1], sc->view.q[2], sc->view.q[3], R);
    const double zoom = sc->view.zoom, cx = w / 2.0, cy = h / 2.0, fov = 600 * zoom;
    const double pv[3] = { sc->view.pivot[0], sc->view.pivot[1], sc->view.pivot[2] };
    double L[3] = { sc->light[0], sc->light[1], sc->light[2] };
    double ln = sqrt(L[0]*L[0] + L[1]*L[1] + L[2]*L[2]) + 1e-12;
    for (int j = 0; j < 3; j++) L[j] /= ln;
    const double amb = sc->ambient;

    size_t total = 0;
    for (int id = 0; id < GSP_SCENE3D_MAX_MESH; id++) total += sc->mesh[id].nf;
    if (ensure_tris(sc, total ? total : 1) < 0) return -1;

    size_t nt = 0;
    for (int id = 0; id < GSP_SCENE3D_MAX_MESH; id++) {
        gsp_mesh3d_t *m = &sc->mesh[id];
        if (m->nv == 0) continue;
        for (uint32_t v = 0; v < m->nv; v++) {
            const float *p = m->pos + 3 * v, *nn = m->nrm + 3 * v;
            double X = p[0] - pv[0], Y = p[1] - pv[1], Z = p[2] - pv[2];
            double x = R[0]*X + R[1]*Y + R[2]*Z;
            double y = R[3]*X + R[4]*Y + R[5]*Z;
            double z = R[6]*X + R[7]*Y + R[8]*Z;
            double sx, sy;
            if (sc->view.perspective) {
                double s = fov / (fov - z + 200);
                sx = cx - x * s * zoom; sy = y * s * zoom + cy;
            } else {
                sx = cx - x * zoom;     sy = y * zoom + cy;
            }
            m->rx[v] = x; m->ry[v] = y;
            m->sx[v] = (float)sx; m->sy[v] = (float)sy; m->sz[v] = (float)z;
            double nx = R[0]*nn[0] + R[1]*nn[1] + R[2]*nn[2];
            double ny = R[3]*nn[0] + R[4]*nn[1] + R[5]*nn[2];
            double nz = R[6]*nn[0] + R[7]*nn[1] + R[8]*nn[2];
            double nl = nx*L[0] + ny*L[1] + nz*L[2];
            double I = amb + (1 - amb) * (nl < 0 ? 0 : nl > 1 ? 1 : nl);
            for (int c = 0; c < 3; c++) {
                double val = I * (m->rgba[4 * v + c] / 255.0);
                val = val < 0 ? 0 : val > 1 ? 1 : val;
                m->lit[3 * v + c] = (uint8_t)(val * 255);
            }
        }
        const double wsign = (m->flags & GSP_MESH_WINDING_CW) ? -1 : 1;
        for (uint32_t f = 0; f < m->nf; f++) {
            const uint32_t *q = m->face + 3 * f;
            uint32_t a = q[0], b = q[1], c = q[2];
            double nz = (m->rx[b] - m->rx[a]) * (m->ry[c] - m->ry[a])
                      - (m->ry[b] - m->ry[a]) * (m->rx[c] - m->rx[a]);
            if (!(nz * wsign > 0)) continue;
            float x0 = m->sx[a], x1 = m->sx[b], x2 = m->sx[c];
            float y0 = m->sy[a], y1 = m->sy[b], y2 = m->sy[c];
            float mnx = fminf(x0, fminf(x1, x2)), mxx = fmaxf(x0, fmaxf(x1, x2));
            float mny = fminf(y0, fminf(y1, y2)), mxy = fmaxf(y0, fmaxf(y1, y2));
            if (mxx < 0 || mnx > w || mxy < 0 || mny > h) continue;
            gsp_3d_tri_t *t = &sc->tris[nt];
            for (int k = 0; k < 3; k++) {
                uint32_t v = q[k];
                t->v[k].x = m->sx[v]; t->v[k].y = m->sy[v]; t->v[k].z = m->sz[v];
                t->v[k].r = m->lit[3 * v]; t->v[k].g = m->lit[3 * v + 1]; t->v[k].b = m->lit[3 * v + 2];
            }
            sc->tri_id[nt] = ((uint32_t)id << 24) | f;
            nt++;
        }
    }
    if (ids) gsp_raster_tris_ids(px, zb, ids, sc->tri_id, w, h, sc->tris, (int)nt);
    else     gsp_raster_tris(px, zb, w, h, sc->tris, (int)nt);
    return (long)nt;
}

int gsp_scene3d_pick(const gsp_scene3d_t *sc, const uint32_t *ids, int w, int h,
                     int x, int y, uint8_t *mesh_id, uint32_t *vertex)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return -1;
    uint32_t id = ids[(size_t)y * w + x];
    if (id == GSP_SCENE3D_NO_ID) return -1;
    uint8_t mi = (uint8_t)(id >> 24);
    uint32_t f = id & 0x00FFFFFFu;
    if (mi >= GSP_SCENE3D_MAX_MESH) return -1;
    const gsp_mesh3d_t *m = &sc->mesh[mi];
    if (f >= m->nf) return -1;
    const uint32_t *q = m->face + 3 * f;
    double px = x + 0.5, py = y + 0.5, best = -1;
    uint32_t bv = q[0];
    for (int k = 0; k < 3; k++) {
        double dx = m->sx[q[k]] - px, dy = m->sy[q[k]] - py, d = dx*dx + dy*dy;
        if (best < 0 || d < best) { best = d; bv = q[k]; }
    }
    *mesh_id = mi;
    *vertex = bv;
    return 0;
}
