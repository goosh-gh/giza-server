/* gsp_raster.h -- z-buffer triangle rasterizer core shared by the 3D code.
 *
 * gsp_edge / gsp_raster_tris are byte-identical to the copy in
 * giza-server-xlib-3d.c (Gouraud interpolation and a z test into an RGBA
 * buffer, z larger = nearer, buffer row 0 = top of the image). The copy in
 * giza-server-cocoa.m differs in one line: it writes row (h-1-py), because
 * Cocoa blits through a flipped CTM. gsp_raster_tris_ids is the xlib loop
 * with one more line: where a pixel passes the z test it also writes
 * tri_id[i] into idbuf, which the stage B scene (gsp_scene3d.c) uses to find
 * the face under the mouse. Pure C (stdint only).
 */
#ifndef GSP_RASTER_H
#define GSP_RASTER_H
#include <stdint.h>
#include "gsp_3d.h"

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

static void gsp_raster_tris_ids(uint8_t *px, float *zb, uint32_t *idbuf,
                                const uint32_t *tri_id, int w, int h,
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
                idbuf[idx] = tri_id[i];
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

#endif /* GSP_RASTER_H */
