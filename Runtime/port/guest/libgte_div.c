/*
 * PsyQ libgte recursive clip-division: RCpolyF3 .. RCpolyGT4. The caller has projected
 * a large polygon, filled a DIVPOLYGON3/4 with its corners (RVECTOR: model vertex,
 * screen XY, UV, colour), the division depth, the clip area, the CLUT/TPAGE, the
 * colour/code word and the OT slot, and left the GTE's rotation/translation as they
 * were for the projection.
 *
 * A piece no larger than the clip area (pih x piv) is drawn as it is, one wholly off
 * the screen is dropped, and one that is larger, or has a corner behind the screen or
 * at the GTE's screen limit, is cut in four (in model space: the new points are
 * projected with the GTE) until the depth `ndiv` is used up, where whatever is still
 * valid is drawn. Every piece becomes a primitive of the polygon's type, written from `s` on
 * and linked into the OT slot. Returns the next free packet address.
 */
#include <sys/types.h>
#include <libgte.h>
#include <libgpu.h>

#include <port_gte.h>

#define MAX_NDIV 5

enum { KIND_F, KIND_FT, KIND_G, KIND_GT };

typedef struct
{
    long mx, my, mz; /* model */
    int u, v;        /* texture */
    int r, g, b;     /* colour */
    short x, y;      /* screen */
    long z;          /* SZ */
} DivPoint;

typedef struct
{
    int kind, quad;
    long x0, y0, x1, y1; /* screen */
    long maxw, maxh;     /* the largest piece drawn whole (the clip area's size) */
    u_long code;
    u_short clut, tpage;
    u_long *ot;
    u_long *out;
} DivJob;

static void project(DivPoint *p)
{
    port_gte_write_data(0, (long)(((unsigned long)(unsigned short)p->my << 16) | (unsigned short)p->mx));
    port_gte_write_data(1, p->mz);
    port_gte_op(0x0180001); /* RTPS */
    {
        long sxy = port_gte_read_data(14);

        p->x = (short)(sxy & 0xFFFF);
        p->y = (short)(sxy >> 16);
        p->z = port_gte_read_data(19);
    }
}

static void mid(const DivPoint *a, const DivPoint *b, DivPoint *o)
{
    o->mx = (a->mx + b->mx) >> 1;
    o->my = (a->my + b->my) >> 1;
    o->mz = (a->mz + b->mz) >> 1;
    o->u = (a->u + b->u + 1) >> 1;
    o->v = (a->v + b->v + 1) >> 1;
    o->r = (a->r + b->r + 1) >> 1;
    o->g = (a->g + b->g + 1) >> 1;
    o->b = (a->b + b->b + 1) >> 1;
    project(o);
}

static int bad_point(const DivPoint *p)
{
    return p->z <= 0 || p->x <= -1024 || p->x >= 1023 || p->y <= -1024 || p->y >= 1023;
}

static u_long *emit(DivJob *j, const DivPoint *const *pt)
{
    u_long *p = j->out;
    int vcount = j->quad ? 4 : 3, i, w = 1;
    int gouraud = j->kind == KIND_G || j->kind == KIND_GT, textured = j->kind == KIND_FT || j->kind == KIND_GT;

    p[w++] = (j->code & 0xFF000000) |
             (gouraud ? ((u_long)pt[0]->b << 16) | ((u_long)pt[0]->g << 8) | (u_long)pt[0]->r : (j->code & 0xFFFFFF));
    for (i = 0; i < vcount; i++)
    {
        if (i > 0 && gouraud) p[w++] = ((u_long)pt[i]->b << 16) | ((u_long)pt[i]->g << 8) | (u_long)pt[i]->r;
        p[w++] = ((u_long)(u_short)pt[i]->y << 16) | (u_short)pt[i]->x;
        if (textured)
        {
            u_long uv = ((u_long)(pt[i]->v & 0xFF) << 8) | (u_long)(pt[i]->u & 0xFF);

            if (i == 0) uv |= (u_long)j->clut << 16;
            if (i == 1) uv |= (u_long)j->tpage << 16;
            p[w++] = uv;
        }
    }
    p[0] = (u_long)(w - 1) << 24;
    addPrim(j->ot, p);
    j->out = p + w;
    return j->out;
}

static void divide(DivJob *j, const DivPoint *const *pt, int depth)
{
    int count = j->quad ? 4 : 3, i, bad = 0;
    int left = 1, right = 1, top = 1, bottom = 1;
    long minx = 0x7FFF, maxx = -0x8000, miny = 0x7FFF, maxy = -0x8000;

    for (i = 0; i < count; i++)
    {
        const DivPoint *p = pt[i];

        if (bad_point(p)) bad = 1;
        if (p->x >= j->x0) left = 0;
        if (p->x < j->x1) right = 0;
        if (p->y >= j->y0) top = 0;
        if (p->y < j->y1) bottom = 0;
        if (p->x < minx) minx = p->x;
        if (p->x > maxx) maxx = p->x;
        if (p->y < miny) miny = p->y;
        if (p->y > maxy) maxy = p->y;
    }
    if (!bad && (left || right || top || bottom)) return; /* wholly outside */
    if (!bad && (depth <= 0 || (maxx - minx <= j->maxw && maxy - miny <= j->maxh)))
    {
        emit(j, pt);
        return;
    }
    if (depth <= 0) return; /* still behind the screen at the last level: dropped */
    if (j->quad)
    {
        DivPoint m01, m23, m02, m13, c;
        const DivPoint *q[4];

        mid(pt[0], pt[1], &m01);
        mid(pt[2], pt[3], &m23);
        mid(pt[0], pt[2], &m02);
        mid(pt[1], pt[3], &m13);
        mid(&m01, &m23, &c);
        q[0] = pt[0], q[1] = &m01, q[2] = &m02, q[3] = &c;
        divide(j, q, depth - 1);
        q[0] = &m01, q[1] = pt[1], q[2] = &c, q[3] = &m13;
        divide(j, q, depth - 1);
        q[0] = &m02, q[1] = &c, q[2] = pt[2], q[3] = &m23;
        divide(j, q, depth - 1);
        q[0] = &c, q[1] = &m13, q[2] = &m23, q[3] = pt[3];
        divide(j, q, depth - 1);
    }
    else
    {
        DivPoint m01, m12, m20;
        const DivPoint *t[3];

        mid(pt[0], pt[1], &m01);
        mid(pt[1], pt[2], &m12);
        mid(pt[2], pt[0], &m20);
        t[0] = pt[0], t[1] = &m01, t[2] = &m20;
        divide(j, t, depth - 1);
        t[0] = &m01, t[1] = pt[1], t[2] = &m12;
        divide(j, t, depth - 1);
        t[0] = &m20, t[1] = &m12, t[2] = pt[2];
        divide(j, t, depth - 1);
        t[0] = &m01, t[1] = &m12, t[2] = &m20;
        divide(j, t, depth - 1);
    }
}

static u_long *subdivide(u_long *out, int kind, int quad, u_long ndiv, long pih, long piv, u_short clut,
                         u_short tpage, CVECTOR rgbc, u_long *ot, const RVECTOR *const *corner)
{
    DivJob j;
    DivPoint pts[4];
    const DivPoint *pt[4];
    long ofx = port_gte_read_ctrl(24) >> 16, ofy = port_gte_read_ctrl(25) >> 16;
    int i, count = quad ? 4 : 3;

    j.kind = kind;
    j.quad = quad;
    j.code = ((u_long)rgbc.cd << 24) | ((u_long)rgbc.b << 16) | ((u_long)rgbc.g << 8) | rgbc.r;
    j.clut = clut;
    j.tpage = tpage;
    j.ot = ot;
    j.out = out;
    if (pih <= 0) pih = 320;
    if (piv <= 0) piv = 240;
    j.maxw = pih;
    j.maxh = piv;
    /* screen coordinates are around the GTE offset: the screen centre when the game
     * projects straight to the screen, 0 when the drawing offset centres them */
    if (ofx >= pih / 2 - 1)
        j.x0 = 0, j.x1 = pih;
    else
        j.x0 = ofx - pih / 2, j.x1 = ofx + pih / 2;
    if (ofy >= piv / 2 - 1)
        j.y0 = 0, j.y1 = piv;
    else
        j.y0 = ofy - piv / 2, j.y1 = ofy + piv / 2;

    for (i = 0; i < count; i++)
    {
        pts[i].mx = corner[i]->v.vx;
        pts[i].my = corner[i]->v.vy;
        pts[i].mz = corner[i]->v.vz;
        pts[i].u = corner[i]->uv[0];
        pts[i].v = corner[i]->uv[1];
        pts[i].r = corner[i]->c.r;
        pts[i].g = corner[i]->c.g;
        pts[i].b = corner[i]->c.b;
        /* the corners keep the caller's projection */
        pts[i].x = corner[i]->sxy.vx;
        pts[i].y = corner[i]->sxy.vy;
        pts[i].z = 1;
        pt[i] = &pts[i];
    }
    if (ndiv > MAX_NDIV) ndiv = MAX_NDIV;
    divide(&j, pt, (int)ndiv);
    return j.out;
}

#define DIV3(name, kind)                                                                         \
    u_long *name(void *s, DIVPOLYGON3 *d)                                                        \
    {                                                                                            \
        const RVECTOR *c[3];                                                                     \
        c[0] = &d->r0, c[1] = &d->r1, c[2] = &d->r2;                                             \
        return subdivide((u_long *)s, kind, 0, d->ndiv, (long)d->pih, (long)d->piv, d->clut,     \
                         d->tpage, d->rgbc, d->ot, c);                                           \
    }
#define DIV4(name, kind)                                                                         \
    u_long *name(void *s, DIVPOLYGON4 *d)                                                        \
    {                                                                                            \
        const RVECTOR *c[4];                                                                     \
        c[0] = &d->r0, c[1] = &d->r1, c[2] = &d->r2, c[3] = &d->r3;                              \
        return subdivide((u_long *)s, kind, 1, d->ndiv, (long)d->pih, (long)d->piv, d->clut,     \
                         d->tpage, d->rgbc, d->ot, c);                                           \
    }

DIV3(RCpolyF3, KIND_F)
DIV3(RCpolyFT3, KIND_FT)
DIV3(RCpolyG3, KIND_G)
DIV3(RCpolyGT3, KIND_GT)
DIV4(RCpolyF4, KIND_F)
DIV4(RCpolyFT4, KIND_FT)
DIV4(RCpolyG4, KIND_G)
DIV4(RCpolyGT4, KIND_GT)
