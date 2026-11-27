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

static void overlay_clear(gsp_overlay3d_t *o)
{
    free(o->pts); free(o->lines); free(o->labels); free(o->text);
    memset(o, 0, sizeof *o);
}

void gsp_scene3d_free(gsp_scene3d_t *sc)
{
    if (!sc) return;
    for (int i = 0; i < GSP_SCENE3D_MAX_MESH; i++) mesh_clear(&sc->mesh[i]);
    overlay_clear(&sc->ovl);
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

int gsp_scene3d_input(gsp_scene3d_t *sc, const gsp_3d_input_t *in)
{
    int out = 0;
    if (in->type == 2) {
        if (in->key == 'p' || in->key == 'P') {
            sc->view.perspective = !sc->view.perspective;
            return GSP_SCENE3D_POSE_CHANGED;
        }
        if (in->key == 'r' || in->key == 'R') {
            sc->view.q[0] = 0; sc->view.q[1] = 0; sc->view.q[2] = 0; sc->view.q[3] = 1;
            sc->view.zoom = 1;
            return GSP_SCENE3D_POSE_CHANGED;
        }
        out |= GSP_SCENE3D_FORWARD;
    }
    if (in->dx != 0 || in->dy != 0) {
        gsp_scene3d_drag(sc, in->dx, in->dy);
        out |= GSP_SCENE3D_POSE_CHANGED;
    }
    if (fabs(in->dzoom) > 0.01) {
        sc->view.zoom = (float)(sc->view.zoom * (1 + in->dzoom * 0.1));
        out |= GSP_SCENE3D_POSE_CHANGED;
    }
    return out;
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

/* ------------------------------------------------------------------
 * Overlay (GSP_MSG_3D_OVERLAY): points, lines and labels over the meshes.
 * ------------------------------------------------------------------ */

/* 1 if s[0..n) is well-formed UTF-8 (no overlongs, surrogates or > U+10FFFF). */
static int utf8_ok(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t k;
        uint32_t cp;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { k = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { k = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { k = 3; cp = c & 0x07; }
        else return 0;
        for (size_t j = 1; j <= k; j++) {
            if (i + j >= n || (s[i + j] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (s[i + j] & 0x3F);
        }
        if ((k == 1 && cp < 0x80) || (k == 2 && cp < 0x800) || (k == 3 && cp < 0x10000)) return 0;
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return 0;
        i += k + 1;
    }
    return 1;
}

int gsp_scene3d_set_overlay(gsp_scene3d_t *sc, const void *payload, size_t len)
{
    const uint8_t *p = payload, *end = p + len;
    float eps;
    uint32_t np, nl, nt;
    if (len < 4 + 4) return -1;
    memcpy(&eps, p, 4); p += 4;
    if (!isfinite(eps)) return -1;
    memcpy(&np, p, 4); p += 4;
    if ((uint64_t)np * sizeof(gsp_3d_ovl_point_t) > (uint64_t)(end - p)) return -1;
    const uint8_t *pp = p; p += (size_t)np * sizeof(gsp_3d_ovl_point_t);
    if (end - p < 4) return -1;
    memcpy(&nl, p, 4); p += 4;
    if ((uint64_t)nl * sizeof(gsp_3d_ovl_line_t) > (uint64_t)(end - p)) return -1;
    const uint8_t *pl = p; p += (size_t)nl * sizeof(gsp_3d_ovl_line_t);
    if (end - p < 4) return -1;
    memcpy(&nt, p, 4); p += 4;
    if ((uint64_t)nt * sizeof(gsp_3d_ovl_label_t) > (uint64_t)(end - p)) return -1;
    const uint8_t *pt = p;
    size_t text_bytes = 0;                            /* walk the labels: lengths must add up */
    for (uint32_t i = 0; i < nt; i++) {
        if ((size_t)(end - p) < sizeof(gsp_3d_ovl_label_t)) return -1;
        gsp_3d_ovl_label_t lb;
        memcpy(&lb, p, sizeof lb);
        p += sizeof lb;
        if ((size_t)(end - p) < lb.len) return -1;
        p += lb.len;
        text_bytes += (size_t)lb.len + 1;
    }
    if (p != end) return -1;

    gsp_overlay3d_t o;
    memset(&o, 0, sizeof o);
    o.eps = eps; o.np = np; o.nl = nl; o.nt = nt;
    o.pts    = malloc((np ? np : 1) * sizeof *o.pts);
    o.lines  = malloc((nl ? nl : 1) * sizeof *o.lines);
    o.labels = malloc((nt ? nt : 1) * sizeof *o.labels);
    o.text   = malloc(text_bytes ? text_bytes : 1);
    if (!o.pts || !o.lines || !o.labels || !o.text) { overlay_clear(&o); return -1; }
    for (uint32_t i = 0; i < np; i++) {
        gsp_3d_ovl_point_t w;
        memcpy(&w, pp + (size_t)i * sizeof w, sizeof w);
        memcpy(o.pts[i].pos, w.pos, sizeof w.pos);
        o.pts[i].radius = w.radius;
        memcpy(o.pts[i].rgba, w.rgba, 4);
        o.pts[i].flags = w.flags;
    }
    for (uint32_t i = 0; i < nl; i++) {
        gsp_3d_ovl_line_t w;
        memcpy(&w, pl + (size_t)i * sizeof w, sizeof w);
        memcpy(o.lines[i].p0, w.p0, sizeof w.p0);
        memcpy(o.lines[i].p1, w.p1, sizeof w.p1);
        o.lines[i].width = w.width;
        memcpy(o.lines[i].rgba, w.rgba, 4);
        o.lines[i].flags = w.flags;
    }
    char *tp = o.text;
    p = pt;
    for (uint32_t i = 0; i < nt; i++) {
        gsp_3d_ovl_label_t w;
        memcpy(&w, p, sizeof w);
        p += sizeof w;
        memcpy(o.labels[i].pos, w.pos, sizeof w.pos);
        o.labels[i].dx = w.dx; o.labels[i].dy = w.dy;
        memcpy(o.labels[i].rgb, w.rgb, 3);
        o.labels[i].flags = w.flags;
        size_t n = utf8_ok(p, w.len) ? w.len : 0;  /* cairo and Cocoa reject bad UTF-8 */
        memcpy(tp, p, n);
        tp[n] = '\0';
        o.labels[i].text = tp;
        tp += (size_t)w.len + 1;
        p += w.len;
    }
    overlay_clear(&sc->ovl);
    sc->ovl = o;
    return 0;
}

typedef struct {
    double R[9], pv[3], zoom, cx, cy, fov;
    int    persp, w, h;
    const float *zb;
    double eps;
} ovl_ctx_t;

/* Screen position and depth of an overlay position; 0 if it cannot be
 * projected (behind the eye, or not finite). */
static int ovl_project(const ovl_ctx_t *c, const float p[3], uint8_t flags,
                       double *sx, double *sy, double *z)
{
    if (flags & GSP_OVL_SCREEN) {
        *sx = p[0]; *sy = p[1]; *z = 0;
        return isfinite(*sx) && isfinite(*sy);
    }
    double X = p[0] - c->pv[0], Y = p[1] - c->pv[1], Z = p[2] - c->pv[2];
    double x = c->R[0]*X + c->R[1]*Y + c->R[2]*Z;
    double y = c->R[3]*X + c->R[4]*Y + c->R[5]*Z;
    double zz = c->R[6]*X + c->R[7]*Y + c->R[8]*Z;
    if (c->persp) {
        double d = c->fov - zz + 200;
        if (!(d > 1e-6)) return 0;
        double s = c->fov / d;
        *sx = c->cx - x * s * c->zoom; *sy = y * s * c->zoom + c->cy;
    } else {
        *sx = c->cx - x * c->zoom;     *sy = y * c->zoom + c->cy;
    }
    *z = zz;
    return isfinite(*sx) && isfinite(*sy) && isfinite(zz);
}

/* 1 if a projected item at (sx, sy) with depth z is not hidden by a mesh. */
static int ovl_visible(const ovl_ctx_t *c, double sx, double sy, double z, uint8_t flags)
{
    if (!c->zb || (flags & (GSP_OVL_SCREEN | GSP_OVL_ON_TOP))) return 1;
    if (sx < 0 || sy < 0 || sx >= c->w || sy >= c->h) return 1;
    float m = c->zb[(size_t)(int)sy * c->w + (int)sx];
    return !(m > z + c->eps);
}

#define GROW(arr, n, cap) do { if ((n) >= (cap)) {                          \
        size_t nc_ = (cap) ? 2 * (cap) : 64;                                 \
        void *t_ = realloc((arr), nc_ * sizeof *(arr));                      \
        if (!t_) return -1;                                                  \
        (arr) = t_; (cap) = nc_; } } while (0)

static int add_seg(gsp_ovl_draw_t *d, double x0, double y0, double x1, double y1,
                   double width, const uint8_t rgba[4], int band)
{
    GROW(d->seg, d->nseg, d->segcap);
    gsp_ovl_seg_t *s = &d->seg[d->nseg++];
    s->x0 = (float)x0; s->y0 = (float)y0; s->x1 = (float)x1; s->y1 = (float)y1;
    s->width = (float)width; memcpy(s->rgba, rgba, 4); s->band = (uint8_t)band;
    return 0;
}

typedef struct { uint32_t i; double z; } zidx_t;
static int cmp_far_first(const void *a, const void *b)
{
    const zidx_t *x = a, *y = b;
    if (x->z < y->z) return -1;
    if (x->z > y->z) return 1;
    return (x->i > y->i) - (x->i < y->i);
}

int gsp_scene3d_overlay_draw(const gsp_scene3d_t *sc, int w, int h, const float *zb,
                             gsp_ovl_draw_t *d)
{
    d->nseg = d->ndisc = d->ntext = 0;
    const gsp_overlay3d_t *o = &sc->ovl;
    ovl_ctx_t c;
    q_to_rot(sc->view.q[0], sc->view.q[1], sc->view.q[2], sc->view.q[3], c.R);
    for (int j = 0; j < 3; j++) c.pv[j] = sc->view.pivot[j];
    c.zoom = sc->view.zoom; c.cx = w / 2.0; c.cy = h / 2.0; c.fov = 600 * c.zoom;
    c.persp = sc->view.perspective; c.w = w; c.h = h; c.zb = zb; c.eps = o->eps;

    /* Lines: 3D ones are sampled every pixel and cut where the mesh hides them. */
    for (uint32_t i = 0; i < o->nl; i++) {
        const gsp_ovl_line_t *ln = &o->lines[i];
        double x0, y0, z0, x1, y1, z1;
        if (!ovl_project(&c, ln->p0, ln->flags, &x0, &y0, &z0) ||
            !ovl_project(&c, ln->p1, ln->flags, &x1, &y1, &z1)) continue;
        double lw = ln->width > 0 ? ln->width : 1;
        if (ln->flags & GSP_OVL_BAND) {
            if (add_seg(d, x0, y0, x1, y0, lw, ln->rgba, 1) < 0) return -1;
            continue;
        }
        double L = hypot(x1 - x0, y1 - y0);
        int n = (int)ceil(L);
        if (n < 1) n = 1;
        if (n > 8192) n = 8192;
        int run = -1;
        for (int k = 0; k <= n + 1; k++) {
            int vis = 0;
            if (k <= n) {
                double t = (double)k / n;
                vis = ovl_visible(&c, x0 + t * (x1 - x0), y0 + t * (y1 - y0),
                                  z0 + t * (z1 - z0), ln->flags);
            }
            if (vis && run < 0) run = k;
            if (!vis && run >= 0) {
                double ta = run == 0 ? 0 : (run - 0.5) / n;
                double tb = k - 1 == n ? 1 : (k - 0.5) / n;
                if (add_seg(d, x0 + ta * (x1 - x0), y0 + ta * (y1 - y0),
                               x0 + tb * (x1 - x0), y0 + tb * (y1 - y0), lw, ln->rgba, 0) < 0)
                    return -1;
                run = -1;
            }
        }
        if ((ln->flags & GSP_OVL_ARROW) && L > 1e-9 && ovl_visible(&c, x1, y1, z1, ln->flags)) {
            double ux = (x1 - x0) / L, uy = (y1 - y0) / L, px = -uy, py = ux;
            double bx = x1 - ux * 12, by = y1 - uy * 12;
            if (add_seg(d, x1, y1, bx + px * 5, by + py * 5, lw, ln->rgba, 0) < 0 ||
                add_seg(d, x1, y1, bx - px * 5, by - py * 5, lw, ln->rgba, 0) < 0) return -1;
        }
    }

    /* Points: 3D ones far to near, then screen ones in order. */
    if (o->np) {
        zidx_t *ord = malloc(o->np * sizeof *ord);
        if (!ord) return -1;
        uint32_t n3 = 0, ns = 0;
        for (uint32_t i = 0; i < o->np; i++)
            if (!(o->pts[i].flags & GSP_OVL_SCREEN)) {
                double sx, sy, z;
                ord[n3].i = i;
                ord[n3].z = ovl_project(&c, o->pts[i].pos, o->pts[i].flags, &sx, &sy, &z) ? z : -1e30;
                n3++;
            }
        qsort(ord, n3, sizeof *ord, cmp_far_first);
        for (uint32_t i = 0; i < o->np; i++)
            if (o->pts[i].flags & GSP_OVL_SCREEN) { ord[n3 + ns].i = i; ord[n3 + ns].z = 0; ns++; }
        for (uint32_t k = 0; k < n3 + ns; k++) {
            const gsp_ovl_point_t *pt = &o->pts[ord[k].i];
            double sx, sy, z;
            if (!ovl_project(&c, pt->pos, pt->flags, &sx, &sy, &z)) continue;
            if (!ovl_visible(&c, sx, sy, z, pt->flags)) continue;
            if (d->ndisc >= d->disccap) {
                size_t nc = d->disccap ? 2 * d->disccap : 64;
                void *t = realloc(d->disc, nc * sizeof *d->disc);
                if (!t) { free(ord); return -1; }
                d->disc = t; d->disccap = nc;
            }
            gsp_ovl_disc_t *ds = &d->disc[d->ndisc++];
            ds->x = (float)sx; ds->y = (float)sy;
            ds->r = (float)(pt->radius * ((pt->flags & GSP_OVL_SCREEN) ? 1 : c.zoom));
            memcpy(ds->rgba, pt->rgba, 4);
        }
        free(ord);
    }

    /* Labels: at the position's screen point plus (dx, dy), hidden with it. */
    for (uint32_t i = 0; i < o->nt; i++) {
        const gsp_ovl_label_t *lb = &o->labels[i];
        double sx, sy, z;
        if (!lb->text[0]) continue;
        if (!ovl_project(&c, lb->pos, lb->flags, &sx, &sy, &z)) continue;
        if (!ovl_visible(&c, sx, sy, z, lb->flags)) continue;
        GROW(d->text, d->ntext, d->textcap);
        gsp_ovl_text_t *t = &d->text[d->ntext++];
        t->x = (float)(sx + lb->dx); t->y = (float)(sy + lb->dy);
        memcpy(t->rgb, lb->rgb, 3);
        t->text = lb->text;
    }
    return 0;
}

void gsp_ovl_draw_free(gsp_ovl_draw_t *d)
{
    if (!d) return;
    free(d->seg); free(d->disc); free(d->text);
    memset(d, 0, sizeof *d);
}
