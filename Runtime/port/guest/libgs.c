/*
 * PsyQ libgs replacement: double buffering, ordering tables, coordinate systems,
 * view/light setup, sprites and TMD model drawing.
 *
 * Globals that the game also touches (GsWSMATRIX, PSDIDX, GsOUT_PACKET_P, ...) are
 * only declared here; they resolve to their original PS1 addresses like the rest
 * of the game's data.
 */
#include <sys/types.h>
#include <libgte.h>
#include <libgpu.h>
#include <libgs.h>

#include <inline_n.h>
#include <port_gte.h>
#include <port_host.h>

/* Not part of the original symbol map: owned here. */
short PSDBASEX[2], PSDBASEY[2];
MATRIX GsLSMATRIX;
static MATRIX sLightColor;
static int sOffsetByGpu;
static long sProjection = 1000;
static long sNearClip;

extern MATRIX GsLIGHTWSMATRIX;
extern MATRIX GsIDMATRIX;
extern MATRIX GsIDMATRIX2;
extern MATRIX GsWSMATRIX;
extern MATRIX GsWSMATRIX_ORG;
extern PACKET *GsOUT_PACKET_P;
extern short PSDIDX;
extern u_long PSDCNT;
extern _GsPOSITION POSITION;
extern DRAWENV GsDRAWENV;
extern DISPENV GsDISPENV;
extern long HWD0, VWD0;
extern int GsLIGHT_MODE;
extern RECT CLIP2;

#define OT_SIZE(ot) (1 << (ot)->length)

static void identity(MATRIX *m)
{
    int i, j;

    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
        {
            m->m[i][j] = (short)(i == j ? 4096 : 0);
        }
        m->t[i] = 0;
    }
}

static void zero_matrix(MATRIX *m)
{
    int i, j;

    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
        {
            m->m[i][j] = 0;
        }
        m->t[i] = 0;
    }
}

/* ---- graphics system / double buffer ---------------------------------------------------------- */
void GsInitGraph(unsigned short x, unsigned short y, unsigned short intmode, unsigned short dith,
                 unsigned short varmmode)
{
    HWD0 = x;
    VWD0 = y;
    sOffsetByGpu = (intmode & 4) != 0;
    PSDIDX = 0;
    PSDCNT = 1;
    POSITION.offx = 0;
    POSITION.offy = 0;
    ResetGraph(0);
    SetDefDrawEnv(&GsDRAWENV, 0, 0, x, y);
    GsDRAWENV.dtd = (u_char)dith;
    GsDRAWENV.isbg = 0;
    SetDefDispEnv(&GsDISPENV, 0, 0, x, y);
    CLIP2.x = 0;
    CLIP2.y = 0;
    CLIP2.w = (short)x;
    CLIP2.h = (short)y;
    PSDBASEX[0] = PSDBASEX[1] = 0;
    PSDBASEY[0] = PSDBASEY[1] = 0;
    if (!sOffsetByGpu) SetGeomOffset(0, 0);
}

void GsDefDispBuff(unsigned short x0, unsigned short y0, unsigned short x1, unsigned short y1)
{
    PSDBASEX[0] = (short)x0;
    PSDBASEY[0] = (short)y0;
    PSDBASEX[1] = (short)x1;
    PSDBASEY[1] = (short)y1;
    PSDIDX = 0;
    GsDISPENV.disp.x = PSDBASEX[0];
    GsDISPENV.disp.y = PSDBASEY[0];
    PutDispEnv(&GsDISPENV);
    GsDRAWENV.clip.x = PSDBASEX[1];
    GsDRAWENV.clip.y = PSDBASEY[1];
    GsDRAWENV.clip.w = (short)HWD0;
    GsDRAWENV.clip.h = (short)VWD0;
    GsDRAWENV.ofs[0] = PSDBASEX[1];
    GsDRAWENV.ofs[1] = PSDBASEY[1];
    PutDrawEnv(&GsDRAWENV);
}

int GsGetActiveBuff(void)
{
    return PSDIDX;
}

void GsSwapDispBuff(void)
{
    int next;

    GsDISPENV.disp.x = PSDBASEX[PSDIDX];
    GsDISPENV.disp.y = PSDBASEY[PSDIDX];
    GsDISPENV.disp.w = (short)HWD0;
    GsDISPENV.disp.h = (short)VWD0;
    PutDispEnv(&GsDISPENV);
    /* libgs's swap also lifts the display mask (some games never call SetDispMask) */
    SetDispMask(1);

    next = PSDIDX ? 0 : 1;
    PSDIDX = (short)next;
    PSDCNT++;
    if (PSDCNT == 0) PSDCNT = 1;

    GsDRAWENV.clip.x = (short)(PSDBASEX[next] + CLIP2.x);
    GsDRAWENV.clip.y = (short)(PSDBASEY[next] + CLIP2.y);
    GsDRAWENV.clip.w = CLIP2.w;
    GsDRAWENV.clip.h = CLIP2.h;
    if (sOffsetByGpu)
    {
        GsDRAWENV.ofs[0] = (short)(PSDBASEX[next] + POSITION.offx);
        GsDRAWENV.ofs[1] = (short)(PSDBASEY[next] + POSITION.offy);
    }
    else
    {
        GsDRAWENV.ofs[0] = PSDBASEX[next];
        GsDRAWENV.ofs[1] = PSDBASEY[next];
    }
    PutDrawEnv(&GsDRAWENV);
}

void GsSetOrign(long x, long y)
{
    POSITION.offx = (short)x;
    POSITION.offy = (short)y;
    if (!sOffsetByGpu)
    {
        SetGeomOffset(x, y);
    }
}

void GsSetOffset(long x, long y)
{
    GsSetOrign(x, y);
}

void GsSetDrawBuffClip(void)
{
    PutDrawEnv(&GsDRAWENV);
}

void GsSetDrawBuffOffset(void)
{
    PutDrawEnv(&GsDRAWENV);
}

void GsSetClip(RECT *clip)
{
    CLIP2 = *clip;
}

/* ---- ordering tables ----------------------------------------------------------------------------- */
void GsClearOt(unsigned short offset, unsigned short point, GsOT *otp)
{
    int n = OT_SIZE(otp);

    otp->offset = offset;
    otp->point = point;
    ClearOTagR((u_long *)otp->org, n);
    otp->tag = otp->org + n - 1;
}

void GsDrawOt(GsOT *ot)
{
    DrawOTag((u_long *)ot->tag);
}

GsOT *GsSortOt(GsOT *src, GsOT *dst)
{
    int n = OT_SIZE(dst);
    int idx = (int)src->point;
    u_long *at;
    u_long *last;

    if (idx >= n) idx = n - 1;
    at = (u_long *)&dst->org[idx];
    last = (u_long *)&src->org[0];
    /* src chain runs from src->tag down to src->org[0]; splice it in after `at`. */
    *last = (*last & 0xFF000000) | (*at & 0x00FFFFFF);
    *at = (*at & 0xFF000000) | ((u_long)src->tag & 0x00FFFFFF);
    return dst;
}

GsOT *GsCutOt(GsOT *src, GsOT *dst)
{
    return GsSortOt(src, dst);
}

void GsSetWorkBase(PACKET *base)
{
    GsOUT_PACKET_P = base;
}

PACKET *GsGetWorkBase(void)
{
    return GsOUT_PACKET_P;
}

void GsSortClear(unsigned char r, unsigned char g, unsigned char b, GsOT *ot)
{
    u_long *p = (u_long *)GsOUT_PACKET_P;

    p[0] = 3 << 24;
    p[1] = 0x02000000 | ((u_long)b << 16) | ((u_long)g << 8) | r;
    p[2] = ((u_long)(u_short)GsDRAWENV.clip.y << 16) | (u_short)GsDRAWENV.clip.x;
    p[3] = ((u_long)(u_short)GsDRAWENV.clip.h << 16) | (u_short)GsDRAWENV.clip.w;
    addPrim(ot->tag, p);
    GsOUT_PACKET_P = (PACKET *)(p + 4);
}

/* Copies the primitive into the packet area (callers pass primitives on the stack). */
void GsSortPoly(void *pp, GsOT *ot, unsigned short pri)
{
    u_long *src = (u_long *)pp;
    u_long *dst = (u_long *)GsOUT_PACKET_P;
    int words = (int)(src[0] >> 24) + 1;
    int i;

    for (i = 0; i < words; i++) dst[i] = src[i];
    addPrim(ot->org + pri, dst);
    GsOUT_PACKET_P = (PACKET *)(dst + words);
}

/* ---- 2D primitives --------------------------------------------------------------------------------- */
static u_long *put_drmode(u_long *p, u_long tpage, GsOT *ot, unsigned short pri)
{
    p[0] = 2 << 24;
    p[1] = 0xE1000000 | (tpage & 0x9FF);
    p[2] = 0;
    addPrim(ot->org + pri, p);
    return p + 3;
}

/* 2D origin of libgs's sprites and boxes. With GsOFSGTE libgs adds the origin
 * (POSITION: the screen centre after GsInit3D, or GsSetOrign's) to their coordinates
 * (BGs and lines it leaves alone); the drawing area's own offset covers the buffer.
 * With GsOFSGPU the GPU's drawing offset holds the origin already. */
short port_gs_origin_x(void)
{
    return sOffsetByGpu ? 0 : POSITION.offx;
}

short port_gs_origin_y(void)
{
    return sOffsetByGpu ? 0 : POSITION.offy;
}

static u_long sprite_tpage(GsSPRITE *sp)
{
    u_long tpage = sp->tpage & ~0x1E0;

    tpage |= ((sp->attribute >> 24) & 3) << 7;
    tpage |= ((sp->attribute >> 28) & 3) << 5;
    return tpage;
}

static u_long sprite_code(GsSPRITE *sp, u_long base)
{
    u_long code = base;

    if (sp->attribute & (1 << 30)) code |= 2;
    if (sp->attribute & (1 << 6)) code |= 1;
    return code;
}

static u_long rgb_word(u_long code, int r, int g, int b)
{
    return (code << 24) | ((u_long)b << 16) | ((u_long)g << 8) | (u_long)r;
}

void GsSortFastSprite(GsSPRITE *sp, GsOT *ot, unsigned short pri)
{
    u_long *p = (u_long *)GsOUT_PACKET_P;
    u_long *s;

    if (sp->attribute & 0x80000000) return;
    s = p;
    s[0] = 4 << 24;
    s[1] = rgb_word(sprite_code(sp, 0x64), sp->r, sp->g, sp->b);
    s[2] = ((u_long)(u_short)(sp->y + port_gs_origin_y()) << 16) | (u_short)(sp->x + port_gs_origin_x());
    s[3] = ((u_long)getClut(sp->cx, sp->cy) << 16) | ((u_long)sp->v << 8) | sp->u;
    s[4] = ((u_long)sp->h << 16) | sp->w;
    addPrim(ot->org + pri, s);
    p = put_drmode(p + 5, sprite_tpage(sp), ot, pri);
    GsOUT_PACKET_P = (PACKET *)p;
}

void GsSortFastSpriteB(GsSPRITE *sp, GsOT *ot, unsigned short pri, unsigned short flip)
{
    GsSortFastSprite(sp, ot, pri);
}

void GsSortSprite(GsSPRITE *sp, GsOT *ot, unsigned short pri)
{
    u_long *p = (u_long *)GsOUT_PACKET_P;
    long sx, sy, c, s;
    long cx[4], cy[4];
    int i, u0, v0, u1, v1;
    long ox, oy;

    if (sp->attribute & 0x80000000) return;
    if (sp->rotate == 0 && sp->scalex == 4096 && sp->scaley == 4096)
    {
        GsSortFastSprite(sp, ot, pri);
        return;
    }

    sx = sp->scalex;
    sy = sp->scaley;
    c = rcos(sp->rotate / 360);
    s = rsin(sp->rotate / 360);
    cx[0] = -sp->mx;          cy[0] = -sp->my;
    cx[1] = sp->w - sp->mx;   cy[1] = -sp->my;
    cx[2] = -sp->mx;          cy[2] = sp->h - sp->my;
    cx[3] = sp->w - sp->mx;   cy[3] = sp->h - sp->my;
    ox = sp->x + sp->mx + port_gs_origin_x();
    oy = sp->y + sp->my + port_gs_origin_y();

    p[0] = 9 << 24;
    p[1] = rgb_word(sprite_code(sp, 0x2C), sp->r, sp->g, sp->b);
    u0 = sp->u;
    v0 = sp->v;
    u1 = u0 + sp->w - 1;
    v1 = v0 + sp->h - 1;
    if (u1 > 255) u1 = 255;
    if (v1 > 255) v1 = 255;
    for (i = 0; i < 4; i++)
    {
        long x = (cx[i] * sx) >> 12;
        long y = (cy[i] * sy) >> 12;
        long rx = (x * c - y * s) >> 12;
        long ry = (x * s + y * c) >> 12;
        int u = (i & 1) ? u1 : u0;
        int v = (i & 2) ? v1 : v0;
        u_long uv = ((u_long)v << 8) | (u_long)u;

        p[2 + i * 2] = ((u_long)(u_short)(oy + ry) << 16) | (u_short)(ox + rx);
        if (i == 0) uv |= (u_long)getClut(sp->cx, sp->cy) << 16;
        if (i == 1) uv |= sprite_tpage(sp) << 16;
        p[3 + i * 2] = uv;
    }
    addPrim(ot->org + pri, p);
    GsOUT_PACKET_P = (PACKET *)(p + 10);
}

void GsSortSpriteB(GsSPRITE *sp, GsOT *ot, unsigned short pri, unsigned short flip)
{
    GsSortSprite(sp, ot, pri);
}

void GsSortFlipSprite(GsSPRITE *sp, GsOT *ot, unsigned short pri)
{
    GsSortSprite(sp, ot, pri);
}

void GsSortBoxFill(GsBOXF *bp, GsOT *ot, unsigned short pri)
{
    u_long *p = (u_long *)GsOUT_PACKET_P;
    u_long code = 0x60;

    if (bp->attribute & 0x80000000) return;
    if (bp->attribute & (1 << 30)) code |= 2;
    p[0] = 3 << 24;
    p[1] = rgb_word(code, bp->r, bp->g, bp->b);
    p[2] = ((u_long)(u_short)(bp->y + port_gs_origin_y()) << 16) | (u_short)(bp->x + port_gs_origin_x());
    p[3] = ((u_long)bp->h << 16) | bp->w;
    addPrim(ot->org + pri, p);
    p += 4;
    if (code & 2)
    {
        p = put_drmode(p, ((bp->attribute >> 28) & 3) << 5, ot, pri);
    }
    GsOUT_PACKET_P = (PACKET *)p;
}

void GsSortLine(GsLINE *lp, GsOT *ot, unsigned short pri)
{
    u_long *p = (u_long *)GsOUT_PACKET_P;
    u_long code = 0x40;

    if (lp->attribute & (1 << 30)) code |= 2;
    p[0] = 3 << 24;
    p[1] = rgb_word(code, lp->r, lp->g, lp->b);
    p[2] = ((u_long)(u_short)lp->y0 << 16) | (u_short)lp->x0;
    p[3] = ((u_long)(u_short)lp->y1 << 16) | (u_short)lp->x1;
    addPrim(ot->org + pri, p);
    GsOUT_PACKET_P = (PACKET *)(p + 4);
}

void GsGetTimInfo(unsigned long *im, GsIMAGE *tim)
{
    u_long *p;

    tim->pmode = im[0];
    p = im + 1;
    if (tim->pmode & 8)
    {
        tim->cx = ((short *)p)[2];
        tim->cy = ((short *)p)[3];
        tim->cw = ((u_short *)p)[4];
        tim->ch = ((u_short *)p)[5];
        tim->clut = p + 3;
        p += p[0] / 4;
    }
    tim->px = ((short *)p)[2];
    tim->py = ((short *)p)[3];
    tim->pw = ((u_short *)p)[4];
    tim->ph = ((u_short *)p)[5];
    tim->pixel = p + 3;
}

/* ---- 3D setup ------------------------------------------------------------------------------------ */
void GsInit3D(void)
{
    InitGeom();
    if (sOffsetByGpu)
    {
        SetGeomOffset(0, 0);
    }
    else
    {
        /* libgs puts the 3D origin at the screen centre here; GsOFSGTE applies it as
         * the GTE's screen offset (sprites and boxes keep the drawing area's top-left) */
        POSITION.offx = (short)(HWD0 / 2);
        POSITION.offy = (short)(VWD0 / 2);
        SetGeomOffset(POSITION.offx, POSITION.offy);
    }
    identity(&GsIDMATRIX);
    identity(&GsIDMATRIX2);
    identity(&GsWSMATRIX);
    identity(&GsWSMATRIX_ORG);
    identity(&GsLSMATRIX);
    zero_matrix(&GsLIGHTWSMATRIX);
    zero_matrix(&sLightColor);
    SetColorMatrix(&sLightColor);
    SetBackColor(0, 0, 0);
    GsLIGHT_MODE = 0;
    GsSetProjection(1000);
}

void GsSetProjection(long h)
{
    sProjection = h;
    SetGeomScreen(h);
}

void GsSetNearClip(long clip)
{
    sNearClip = clip;
}

int GsSetFlatLight(int id, GsF_LIGHT *lt)
{
    long x = lt->vx, y = lt->vy, z = lt->vz;
    long len;

    if (id < 0 || id > 2) return -1;
    len = SquareRoot0(x * x + y * y + z * z);
    if (len == 0)
    {
        GsLIGHTWSMATRIX.m[id][0] = GsLIGHTWSMATRIX.m[id][1] = GsLIGHTWSMATRIX.m[id][2] = 0;
    }
    else
    {
        GsLIGHTWSMATRIX.m[id][0] = (short)(-x * 4096 / len);
        GsLIGHTWSMATRIX.m[id][1] = (short)(-y * 4096 / len);
        GsLIGHTWSMATRIX.m[id][2] = (short)(-z * 4096 / len);
    }
    sLightColor.m[0][id] = (short)(lt->r << 4);
    sLightColor.m[1][id] = (short)(lt->g << 4);
    sLightColor.m[2][id] = (short)(lt->b << 4);
    SetColorMatrix(&sLightColor);
    return 0;
}

void GsSetLightMode(int mode)
{
    GsLIGHT_MODE = mode;
}

void GsSetAmbient(long r, long g, long b)
{
    SetBackColor(r >> 4, g >> 4, b >> 4);
}

void GsSetFogParam(GsFOGPARAM *fog)
{
    port_gte_write_ctrl(27, fog->dqa);
    port_gte_write_ctrl(28, fog->dqb);
    SetFarColor(fog->rfc, fog->gfc, fog->bfc);
}

void GsSetLightMatrix(MATRIX *mp)
{
    MATRIX m;

    MulMatrix0(&GsLIGHTWSMATRIX, mp, &m);
    SetLightMatrix(&m);
}

void GsSetLightMatrix2(MATRIX *mp)
{
    SetLightMatrix(mp);
}

void GsSetLsMatrix(MATRIX *mp)
{
    GsLSMATRIX = *mp;
    SetRotMatrix(mp);
    SetTransMatrix(mp);
}

/* ---- coordinate systems --------------------------------------------------------------------- */
/* As libgs: coord, super and flg only. param, workm and sub are left as they are (LSD
 * sets param before it calls this; clearing it sent every node's rotation and scale
 * through a NULL pointer). */
void GsInitCoordinate2(GsCOORDINATE2 *super, GsCOORDINATE2 *base)
{
    identity(&base->coord); /* libgs copies GsIDMATRIX */
    base->super = super;
    base->flg = 0;
    if (super != 0 && super != SCREEN)
    {
        super->sub = base;
    }
}

/* m3 = m1 * m2, translation included */
void GsMulCoord0(MATRIX *m1, MATRIX *m2, MATRIX *m3)
{
    MATRIX r;

    CompMatrix(m1, m2, &r);
    *m3 = r;
}

/* m2 = m1 * m2 */
void GsMulCoord2(MATRIX *m1, MATRIX *m2)
{
    MATRIX r;

    CompMatrix(m1, m2, &r);
    *m2 = r;
}

/* m1 = m1 * m2 */
void GsMulCoord3(MATRIX *m1, MATRIX *m2)
{
    MATRIX r;

    CompMatrix(m1, m2, &r);
    *m1 = r;
}

/* Local-to-world of `c` into c->workm (and `lw`), with libgs's cache (as in Sony's
 * GsGetLws): workm is kept between frames. A coordinate computed this frame has
 * flg == PSDCNT; a game marks one it changed with flg = 0. Walking up from `c`, the
 * first coordinate computed this frame, or the root (its workm = coord when it is
 * dirty or current), gives the starting matrix; with a root from an earlier frame the
 * cached workm above the highest dirty coordinate is used, and with nothing dirty
 * c's own cached workm, as it is. From there each coordinate below gets
 * workm = super's workm x coord and flg = PSDCNT. Games rely on this: LSD writes
 * workm.t of nodes itself and draws with the cached matrices. */
#define COORD_CHAIN_MAX 100
static void update_workm_lw(GsCOORDINATE2 *c, MATRIX *lw)
{
    GsCOORDINATE2 *chain[COORD_CHAIN_MAX + 1];
    int n = 0, dirty = -1;

    for (;;)
    {
        chain[n] = c;
        if (c->super == 0 || c->super == SCREEN)
        {
            if (c->flg == PSDCNT || c->flg == 0)
            {
                c->workm = c->coord;
                c->flg = PSDCNT;
                *lw = c->workm;
            }
            else if (dirty < 0)
            {
                *lw = chain[0]->workm;
                n = 0;
            }
            else
            {
                n = dirty + 1;
                *lw = chain[n]->workm;
            }
            break;
        }
        if (c->flg == PSDCNT)
        {
            *lw = c->workm;
            break;
        }
        if (c->flg == 0) dirty = n;
        if (n == COORD_CHAIN_MAX)
        {
            /* deeper than libgs allows (its own table holds 100): start here */
            *lw = c->workm;
            break;
        }
        c = c->super;
        n++;
    }
    while (n > 0)
    {
        n--;
        GsMulCoord3(lw, &chain[n]->coord);
        chain[n]->workm = *lw;
        chain[n]->flg = PSDCNT;
    }
}

static void update_workm(GsCOORDINATE2 *c, int depth)
{
    MATRIX lw;

    (void)depth;
    update_workm_lw(c, &lw);
}

void GsGetLw(GsCOORDINATE2 *m, MATRIX *out)
{
    update_workm(m, 0);
    *out = m->workm;
}

void GsGetLs(GsCOORDINATE2 *m, MATRIX *out)
{
    update_workm(m, 0);
    if (m->super == SCREEN)
    {
        *out = m->workm;
        return;
    }
    CompMatrix(&GsWSMATRIX, &m->workm, out);
}

void GsGetLws(GsCOORDINATE2 *m, MATRIX *outw, MATRIX *outs)
{
    update_workm(m, 0);
    *outw = m->workm;
    CompMatrix(&GsWSMATRIX, &m->workm, outs);
}

/* World matrix of a view's parent coordinate system, inverted and applied. */
static void apply_view_super(GsCOORDINATE2 *super)
{
    MATRIX inv, r;
    int i, j;

    if (super == 0 || super == SCREEN) return;
    update_workm(super, 0);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            inv.m[i][j] = super->workm.m[j][i];
    for (i = 0; i < 3; i++)
    {
        inv.t[i] = -(long)(((long long)inv.m[i][0] * super->workm.t[0] + (long long)inv.m[i][1] * super->workm.t[1] +
                            (long long)inv.m[i][2] * super->workm.t[2]) >> 12);
    }
    CompMatrix(&GsWSMATRIX, &inv, &r);
    GsWSMATRIX = r;
}

int GsSetRefView2(GsRVIEW2 *pv)
{
    long dx = pv->vrx - pv->vpx;
    long dy = pv->vry - pv->vpy;
    long dz = pv->vrz - pv->vpz;
    long h;
    SVECTOR ry = {0, 0, 0, 0};
    SVECTOR rx = {0, 0, 0, 0};
    MATRIX my, mx, mz, m;
    VECTOR vp, t;

    if (dx == 0 && dy == 0 && dz == 0) return -1;
    while (dx > 0x7FFF || dx < -0x7FFF || dy > 0x7FFF || dy < -0x7FFF || dz > 0x7FFF || dz < -0x7FFF)
    {
        dx >>= 1;
        dy >>= 1;
        dz >>= 1;
    }
    h = SquareRoot0(dx * dx + dz * dz);
    ry.vy = (short)ratan2(-dx, dz);
    rx.vx = (short)ratan2(dy, h);
    RotMatrix(&ry, &my);
    RotMatrix(&rx, &mx);
    MulMatrix0(&mx, &my, &m);
    if (pv->rz != 0)
    {
        SVECTOR rz = {0, 0, 0, 0};

        rz.vz = (short)(pv->rz / 360);
        RotMatrix(&rz, &mz);
        MulMatrix0(&mz, &m, &m);
    }
    vp.vx = pv->vpx;
    vp.vy = pv->vpy;
    vp.vz = pv->vpz;
    ApplyMatrixLV(&m, &vp, &t);
    m.t[0] = -t.vx;
    m.t[1] = -t.vy;
    m.t[2] = -t.vz;
    GsWSMATRIX = m;
    apply_view_super(pv->super);
    GsWSMATRIX_ORG = GsWSMATRIX;
    /* PSDCNT (the coordinate cache's frame) advances in GsSwapDispBuff only, as in libgs */
    return 0;
}

int GsSetRefView2L(GsRVIEW2 *pv)
{
    return GsSetRefView2(pv);
}

int GsSetView2(GsVIEW2 *pv)
{
    GsWSMATRIX = pv->view;
    apply_view_super(pv->super);
    GsWSMATRIX_ORG = GsWSMATRIX;
    PSDCNT++;
    if (PSDCNT == 0) PSDCNT = 1;
    return 0;
}

/* ---- TMD models ------------------------------------------------------------------------------------ */
/* Words after the header of a TMD primitive packet of this flag/mode, or -1 for a type
 * this runtime does not know. Mapped TMDs keep a run's packet count where the first
 * packet's olen/ilen were (GsMapModelingData), so lengths come from the type. */
static int tmd_packet_words(int flag, int mode)
{
    int quad = (mode & 8) != 0, tex = (mode & 4) != 0, iip = (mode & 0x10) != 0;
    int unlit = (flag & 1) != 0, grad = (flag & 4) != 0;
    int nv = quad ? 4 : 3, ncol, halves;

    switch (mode & 0xE0)
    {
    case 0x20: /* polygon */
        if (tex && !unlit)
            ncol = 0;
        else if (unlit)
            ncol = iip ? nv : 1;
        else
            ncol = grad ? nv : 1;
        if (unlit)
            halves = nv;
        else if (iip)
            halves = nv * 2;
        else
            halves = nv + 1;
        return (tex ? nv : 0) + ncol + (halves + 1) / 2;
    case 0x40: /* line: colour(s), two vertex indices */
        return (iip ? 2 : 1) + 1;
    case 0x60: /* 3D sprite: vertex + tpage, uv + clut, and the size when it is free */
        return (mode & 0x18) == 0 ? 3 : 2;
    default:
        return -1;
    }
}

/* Relocates the TMD's object table (offsets -> addresses), as libgs does. */
void GsMapModelingData(unsigned long *p)
{
    u_long nobj, i;
    u_long *table;
    u_long base;

    if (p[0] & 1) return;
    nobj = p[1];
    table = p + 2;
    base = (u_long)table;
    for (i = 0; i < nobj; i++)
    {
        u_long *o = table + i * 7;

        o[0] += base;
        o[2] += base;
        o[4] += base;
    }
    p[0] |= 1;
}

/* Links object `n` of a mapped TMD to `objp`. With PS1_LIBGS_RUN_COUNTS (a game whose
 * libgs did this, set in its package's DEFINES: LSD) it also does what that libgs's
 * GsLinkObject4 does for the GsSortObject4 fast path: it groups the object's primitives
 * into runs of one mode (the ABE bit aside) and writes each run's packet count over the
 * first packet's olen/ilen. Other games' libgs left the packets as they are. */
void GsLinkObject4(unsigned long tmd_base, GsDOBJ2 *objp, int n)
{
    u_long *o = (u_long *)tmd_base + n * 7;

    objp->tmd = o;
#if PS1_LIBGS_RUN_COUNTS
    {
        u_long *prim = (u_long *)o[4], *first = prim;
        u_long left = o[5], count = 0;
        int runMode = -1;

        while (left-- > 0)
        {
            int flag = (int)((prim[0] >> 16) & 0xFF), mode = (int)(prim[0] >> 24);
            int words = tmd_packet_words(flag, mode);

            if (runMode >= 0 && (mode & 0xFD) != runMode)
            {
                *(u_short *)first = (u_short)count;
                count = 0;
                first = prim;
            }
            runMode = mode & 0xFD;
            if (words < 0) break; /* a type libgs does not know: it stops there too */
            prim += 1 + words;
            count++;
        }
        if (runMode >= 0) *(u_short *)first = (u_short)count;
    }
#endif
}

#define LM_NORMAL 0
#define LM_FOG 1
#define LM_OFF 2

static int sTmdSkipped;

/* Lit colour for one normal; rgbc = base colour with the GPU code in the top byte. */
static u_long light_color(const SVECTOR *n, u_long rgbc, int lmode)
{
    gte_ldv0(n);
    port_gte_write_data(6, (long)rgbc);
    port_gte_op(lmode == LM_FOG ? GTE_CMD_NCDS : GTE_CMD_NCCS);
    return (u_long)port_gte_read_data(22);
}

/*
 * Draws `n` TMD polygon packets starting at `prim`. Each packet is decoded from its
 * own header, so one routine serves every GsTMDfast / GsTMDdiv variant.
 * `force_lmode` >= 0 overrides the light mode chosen from the object attribute.
 */
static PACKET *tmd_draw(u_long *prim, SVECTOR *vtx, SVECTOR *nrm, PACKET *pk, int n, int shift, GsOT *ot, int lmode,
                        u_long attr)
{
    u_long *p = prim;
    int otn = OT_SIZE(ot);

    while (n-- > 0)
    {
        u_long hdr = p[0];
        int flag = (hdr >> 16) & 0xFF;
        int mode = (hdr >> 24) & 0xFF;
        int ilen = tmd_packet_words(flag, mode);
        u_long *d = p + 1;
        int quad, tex, iip, unlit, grad, nv, ncol, i;
        u_long uvw[4];
        u_long col[4];
        u_short nidx[4], vidx[4];
        long sxy[4];
        long opz, otz, flg;
        u_long code;
        u_long *out;
        int words;

        if (ilen < 0) break; /* unknown packet type: the rest cannot be walked */
        p += 1 + ilen;
        if ((mode & 0xE0) != 0x20) continue;

        quad = (mode & 8) != 0;
        tex = (mode & 4) != 0;
        iip = (mode & 0x10) != 0;
        unlit = (flag & 1) != 0;
        grad = (flag & 4) != 0;
        nv = quad ? 4 : 3;

        if (tex)
        {
            for (i = 0; i < nv; i++) uvw[i] = *d++;
        }
        if (tex && !unlit)
            ncol = 0;
        else if (unlit)
            ncol = iip ? nv : 1;
        else
            ncol = grad ? nv : 1;
        for (i = 0; i < ncol; i++) col[i] = *d++;
        {
            u_short *s = (u_short *)d;

            if (unlit)
            {
                for (i = 0; i < nv; i++) vidx[i] = s[i];
            }
            else if (iip)
            {
                for (i = 0; i < nv; i++)
                {
                    nidx[i] = s[i * 2];
                    vidx[i] = s[i * 2 + 1];
                }
            }
            else
            {
                nidx[0] = s[0];
                for (i = 0; i < nv; i++) vidx[i] = s[1 + i];
            }
        }

        gte_ldv3(&vtx[vidx[0]], &vtx[vidx[1]], &vtx[vidx[2]]);
        gte_rtpt();
        flg = port_gte_read_ctrl(31);
        port_gte_op(GTE_CMD_NCLIP);
        opz = port_gte_read_data(24);
        if (opz <= 0 && !(flag & 2)) continue;
        sxy[0] = port_gte_read_data(12);
        sxy[1] = port_gte_read_data(13);
        sxy[2] = port_gte_read_data(14);
        if (quad)
        {
            gte_ldv0(&vtx[vidx[3]]);
            gte_rtps();
            flg |= port_gte_read_ctrl(31);
            sxy[3] = port_gte_read_data(14);
            port_gte_op(GTE_CMD_AVSZ4);
        }
        else
        {
            port_gte_op(GTE_CMD_AVSZ3);
        }
        /* Vertex behind the projection plane or projection overflow. */
        if (flg & 0x00060000)
        {
            sTmdSkipped++;
            continue;
        }
        otz = port_gte_read_data(7) >> shift;
        if (otz <= 0 || otz >= otn) continue;
        {
            short minx = 32767, maxx = -32768, miny = 32767, maxy = -32768;

            for (i = 0; i < nv; i++)
            {
                short x = (short)(sxy[i] & 0xFFFF), y = (short)(sxy[i] >> 16);

                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
            if (maxx - minx > 1023 || maxy - miny > 511) continue;
        }

        /* Output colours */
        code = 0x20 | (quad ? 8 : 0) | (tex ? 4 : 0) | (mode & 3);
        if (attr & GsALON) code |= 2;
        {
            int gouraud_out = iip || (grad && !unlit) || (unlit && iip);
            u_long base = (tex ? 0x808080 : (col[0] & 0xFFFFFF)) | (code << 24);

            if (!unlit && lmode != LM_OFF)
            {
                if (iip)
                {
                    for (i = 0; i < nv; i++)
                    {
                        u_long b = tex ? base : ((grad ? col[i] : col[0]) & 0xFFFFFF) | (code << 24);

                        col[i] = light_color(&nrm[nidx[i]], b, lmode);
                    }
                }
                else if (grad)
                {
                    for (i = 0; i < nv; i++)
                    {
                        col[i] = light_color(&nrm[nidx[0]], (col[i] & 0xFFFFFF) | (code << 24), lmode);
                    }
                }
                else
                {
                    col[0] = light_color(&nrm[nidx[0]], base, lmode);
                }
            }
            else if (tex && !unlit)
            {
                col[0] = base;
                if (iip)
                {
                    for (i = 1; i < nv; i++) col[i] = base;
                }
            }
            if (gouraud_out)
            {
                code |= 0x10;
                if (ncol == 1 && !(iip && !unlit))
                {
                    for (i = 1; i < nv; i++) col[i] = col[0];
                }
            }
        }

        out = (u_long *)pk;
        words = 1;
        for (i = 0; i < nv; i++)
        {
            if (i == 0 || (code & 0x10))
            {
                out[words++] = (col[i] & 0xFFFFFF) | (code << 24);
            }
            out[words++] = (u_long)sxy[i];
            if (tex)
            {
                u_long w = uvw[i];

                if (i == 1 && (attr & GsALON))
                {
                    u_long tp = (w >> 16) & ~0x60;

                    tp |= ((attr >> 28) & 3) << 5;
                    w = (w & 0xFFFF) | (tp << 16);
                }
                if (i >= 2) w &= 0xFFFF;
                out[words++] = w;
            }
        }
        out[0] = (u_long)(words - 1) << 24;
        addPrim(ot->org + otz, out);
        pk = (PACKET *)(out + words);
    }
    return pk;
}

static int obj_lmode(u_long attr)
{
    if (attr & GsLOFF) return LM_OFF;
    if (attr & GsFOG) return LM_FOG;
    if (GsLIGHT_MODE == 1) return LM_FOG;
    return LM_NORMAL;
}

void GsSortObject4(GsDOBJ2 *objp, GsOT *ot, int shift, u_long *scratch)
{
    struct TMD_STRUCT *t;

    if (objp == 0 || objp->tmd == 0) return;
    if (objp->attribute & GsDOFF) return;
    t = (struct TMD_STRUCT *)objp->tmd;
    GsOUT_PACKET_P = tmd_draw(t->primtop, (SVECTOR *)t->vertop, (SVECTOR *)t->nortop, GsOUT_PACKET_P, (int)t->primn,
                              shift, ot, obj_lmode(objp->attribute), objp->attribute);
}

void GsSortObject4J(GsDOBJ2 *objp, GsOT *ot, int shift, u_long *scratch)
{
    GsSortObject4(objp, ot, shift, scratch);
}

#define TMD_FN(name, lm)                                                                                            \
    PACKET *name(u_long *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, int n, int shift, GsOT *ot, u_long *scratch)  \
    {                                                                                                             \
        return tmd_draw(op, vp, np, pk, n, shift, ot, lm, 0);                                                     \
    }
#define TMD_FN_NN(name)                                                                                           \
    PACKET *name(u_long *op, SVECTOR *vp, PACKET *pk, int n, int shift, GsOT *ot, u_long *scratch)                \
    {                                                                                                             \
        return tmd_draw(op, vp, 0, pk, n, shift, ot, LM_OFF, 0);                                                  \
    }

TMD_FN(GsTMDfastF3L, LM_NORMAL)
TMD_FN(GsTMDfastG3L, LM_NORMAL)
TMD_FN(GsTMDfastF4L, LM_NORMAL)
TMD_FN(GsTMDfastF4NL, LM_OFF)
TMD_FN(GsTMDfastG4L, LM_NORMAL)
TMD_FN_NN(GsTMDfastNF4)
TMD_FN(GsTMDfastTF3L, LM_NORMAL)
TMD_FN(GsTMDfastTF3NL, LM_OFF)
TMD_FN_NN(GsTMDfastTNF3)
TMD_FN(GsTMDfastTF4L, LM_NORMAL)
TMD_FN(GsTMDfastTF4NL, LM_OFF)
TMD_FN_NN(GsTMDfastTNF4)
TMD_FN(GsTMDfastTG3L, LM_NORMAL)
TMD_FN(GsTMDfastTG3NL, LM_OFF)
TMD_FN_NN(GsTMDfastTNG3)
TMD_FN(GsTMDfastTG4L, LM_NORMAL)
TMD_FN(GsTMDfastTG4NL, LM_OFF)
TMD_FN_NN(GsTMDfastTNG4)
TMD_FN(GsTMDdivTF3NL, LM_OFF)
TMD_FN_NN(GsTMDdivTNF3)
TMD_FN(GsTMDdivTG3NL, LM_OFF)
TMD_FN_NN(GsTMDdivTNG3)
TMD_FN(GsTMDdivTF4L, LM_NORMAL)
TMD_FN(GsTMDdivTF4NL, LM_OFF)
TMD_FN_NN(GsTMDdivTNF4)
TMD_FN(GsTMDdivTG4NL, LM_OFF)
TMD_FN_NN(GsTMDdivTNG4)
