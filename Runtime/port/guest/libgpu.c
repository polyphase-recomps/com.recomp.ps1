/* PsyQ libgpu replacement on top of the software GPU. */
#include <sys/types.h>
#include <libgpu.h>

#include <port_gpu.h>
#include <port_host.h>

static int sGraphDebug;
static DRAWENV sDrawEnv; /* the last PutDrawEnv / PutDispEnv (GetDrawEnv / GetDispEnv) */
static DISPENV sDispEnv;

int ResetGraph(int mode)
{
    if (mode == 0)
    {
        gpu_set_display_enabled(0);
    }
    return 0;
}

int SetGraphDebug(int level)
{
    int old = sGraphDebug;

    sGraphDebug = level;
    return old;
}

int GetGraphDebug(void)
{
    return sGraphDebug;
}

/* the GPU's type: 0, the retail console's (1 and 2 are development boards' VRAM layouts) */
int GetGraphType(void)
{
    return 0;
}

/* interlaced odd/even line: the display is drawn progressively */
int GetODE(void)
{
    return 0;
}

void SetDispMask(int mask)
{
    gpu_set_display_enabled(mask != 0);
}

int DrawSync(int mode)
{
    return 0;
}

void DrawOTag(u_long *p)
{
    if (port_trace_gpu())
    {
        port_log("DrawOTag %p at vblank %u", p, port_vblank_count());
    }
    gpu_draw_otag((const unsigned long *)p);
}

void AddPrim(void *ot, void *p)
{
    if (port_trace_gpu())
    {
        port_log("AddPrim ot=%p p=%p code=%08lX", ot, p, ((unsigned long *)p)[1]);
    }
    addPrim(ot, p);
}

u_long *ClearOTagR(u_long *ot, int n)
{
    int i;

    for (i = 1; i < n; i++)
    {
        setaddr(&ot[i], &ot[i - 1]);
        setlen(&ot[i], 0);
    }
    if (n > 0)
    {
        termPrim(&ot[0]);
        setlen(&ot[0], 0);
    }
    return ot;
}

u_long *ClearOTag(u_long *ot, int n)
{
    int i;

    for (i = 0; i < n - 1; i++)
    {
        setaddr(&ot[i], &ot[i + 1]);
        setlen(&ot[i], 0);
    }
    if (n > 0)
    {
        termPrim(&ot[n - 1]);
        setlen(&ot[n - 1], 0);
    }
    return ot;
}

int LoadImage(RECT *rect, u_long *p)
{
    gpu_load_image(rect->x, rect->y, rect->w, rect->h, p);
    return 0;
}

int StoreImage(RECT *rect, u_long *p)
{
    gpu_store_image(rect->x, rect->y, rect->w, rect->h, p);
    return 0;
}

int MoveImage(RECT *rect, int x, int y)
{
    gpu_move_image(rect->x, rect->y, x, y, rect->w, rect->h);
    return 0;
}

int ClearImage(RECT *rect, u_char r, u_char g, u_char b)
{
    gpu_fill(rect->x, rect->y, rect->w, rect->h, r, g, b);
    return 0;
}

u_short LoadClut(u_long *clut, int x, int y)
{
    gpu_load_image(x, y, 256, 1, clut);
    return (u_short)getClut(x, y);
}

u_short GetClut(int x, int y)
{
    return (u_short)getClut(x, y);
}

u_short GetTPage(int tp, int abr, int x, int y)
{
    return (u_short)getTPage(tp, abr, x, y);
}

DISPENV *SetDefDispEnv(DISPENV *env, int x, int y, int w, int h)
{
    env->disp.x = (short)x;
    env->disp.y = (short)y;
    env->disp.w = (short)w;
    env->disp.h = (short)h;
    env->screen.x = env->screen.y = 0;
    env->screen.w = env->screen.h = 0;
    env->isinter = 0;
    env->isrgb24 = 0;
    env->pad0 = env->pad1 = 0;
    return env;
}

DRAWENV *SetDefDrawEnv(DRAWENV *env, int x, int y, int w, int h)
{
    env->clip.x = (short)x;
    env->clip.y = (short)y;
    env->clip.w = (short)w;
    env->clip.h = (short)h;
    env->ofs[0] = (short)x;
    env->ofs[1] = (short)y;
    env->tw.x = env->tw.y = env->tw.w = env->tw.h = 0;
    env->tpage = 10;
    env->dtd = 1;
    env->dfe = 0;
    env->isbg = 0;
    env->r0 = env->g0 = env->b0 = 0;
    return env;
}

DISPENV *GetDispEnv(DISPENV *env)
{
    *env = sDispEnv;
    return env;
}

DRAWENV *GetDrawEnv(DRAWENV *env)
{
    *env = sDrawEnv;
    return env;
}

DISPENV *PutDispEnv(DISPENV *env)
{
    int h = env->disp.h;

    sDispEnv = *env;
    gpu_set_display(env->disp.x, env->disp.y, env->disp.w, h, env->isrgb24);
    return env;
}

DRAWENV *PutDrawEnv(DRAWENV *env)
{
    sDrawEnv = *env;
    gpu_set_draw_area(env->clip.x, env->clip.y, env->clip.x + env->clip.w - 1, env->clip.y + env->clip.h - 1);
    gpu_set_draw_offset(env->ofs[0], env->ofs[1]);
    gpu_set_texwindow(env->tw.x, env->tw.y, env->tw.w, env->tw.h);
    gpu_set_texpage(env->tpage);
    gpu_set_dither(env->dtd);
    if (env->isbg)
    {
        gpu_fill(env->clip.x, env->clip.y, env->clip.w, env->clip.h, env->r0, env->g0, env->b0);
    }
    return env;
}

void SetDrawLoad(DR_LOAD *p, RECT *rect)
{
    int words = (rect->w * rect->h + 1) / 2;

    if (words > 13) words = 13;
    setlen(p, 3 + words);
    p->code[0] = 0xA0000000;
    p->code[1] = ((u_long)(u_short)rect->y << 16) | (u_short)rect->x;
    p->code[2] = ((u_long)(u_short)rect->h << 16) | (u_short)rect->w;
}

void SetDrawOffset(DR_OFFSET *p, u_short *ofs)
{
    setlen(p, 2);
    p->code[0] = 0xE5000000 | (((u_long)ofs[1] & 0x7FF) << 11) | ((u_long)ofs[0] & 0x7FF);
    p->code[1] = 0;
}

/* the GPU's draw-mode words (GP0 E1 texpage, E2 texture window, E3/E4 drawing area) */
static u_long draw_mode_word(int dfe, int dtd, int tpage)
{
    return 0xE1000000 | (dtd ? 0x200 : 0) | (dfe ? 0x400 : 0) | ((u_long)tpage & 0x9FF);
}

static u_long tex_window_word(RECT *tw)
{
    if (tw == 0) return 0;
    return 0xE2000000 | ((u_long)((u_char)tw->y >> 3) << 15) | ((u_long)((u_char)tw->x >> 3) << 10) |
           ((u_long)((u_char)-tw->h >> 3) << 5) | (u_long)((u_char)-tw->w >> 3);
}

void SetDrawMode(DR_MODE *p, int dfe, int dtd, int tpage, RECT *tw)
{
    setlen(p, 2);
    p->code[0] = draw_mode_word(dfe, dtd, tpage);
    p->code[1] = tex_window_word(tw);
}

void SetDrawTPage(DR_TPAGE *p, int dfe, int dtd, int tpage)
{
    setlen(p, 1);
    p->code[0] = draw_mode_word(dfe, dtd, tpage);
}

void SetTexWindow(DR_TWIN *p, RECT *tw)
{
    setlen(p, 2);
    p->code[0] = tex_window_word(tw);
    p->code[1] = 0;
}

void SetDrawArea(DR_AREA *p, RECT *r)
{
    int x1 = r->x + r->w - 1, y1 = r->y + r->h - 1;

    if (x1 > 1023) x1 = 1023;
    if (y1 > 511) y1 = 511;
    setlen(p, 2);
    p->code[0] = 0xE3000000 | (((u_long)r->y & 0x3FF) << 10) | ((u_long)r->x & 0x3FF);
    p->code[1] = 0xE4000000 | (((u_long)y1 & 0x3FF) << 10) | ((u_long)x1 & 0x3FF);
}

/* the ordering table's links (a primitive's tag: the next one's address, its length) */
void *NextPrim(void *p) { return nextPrim(p); }
int IsEndPrim(void *p) { return isendprim(p); }
void TermPrim(void *p) { termPrim(p); }
void CatPrim(void *p0, void *p1) { catPrim(p0, p1); }
void AddPrims(void *ot, void *p0, void *p1) { addPrims(ot, p0, p1); }

void SetLineF2(LINE_F2 *p) { setLineF2(p); }
void SetLineF3(LINE_F3 *p) { setLineF3(p); }
void SetLineF4(LINE_F4 *p) { setLineF4(p); }
void SetPolyF4(POLY_F4 *p) { setPolyF4(p); }
void SetPolyFT3(POLY_FT3 *p) { setPolyFT3(p); }
void SetPolyFT4(POLY_FT4 *p) { setPolyFT4(p); }
void SetPolyG4(POLY_G4 *p) { setPolyG4(p); }
void SetPolyGT4(POLY_GT4 *p) { setPolyGT4(p); }
void SetSemiTrans(void *p, int abe) { setSemiTrans(p, abe); }
void SetShadeTex(void *p, int tge) { setShadeTex(p, tge); }
void SetPolyF3(POLY_F3 *p) { setPolyF3(p); }
void SetPolyG3(POLY_G3 *p) { setPolyG3(p); }
void SetPolyGT3(POLY_GT3 *p) { setPolyGT3(p); }
void SetLineG2(LINE_G2 *p) { setLineG2(p); }
void SetLineG3(LINE_G3 *p) { setLineG3(p); }
void SetLineG4(LINE_G4 *p) { setLineG4(p); }
void SetSprt(SPRT *p) { setSprt(p); }
void SetSprt8(SPRT_8 *p) { setSprt8(p); }
void SetSprt16(SPRT_16 *p) { setSprt16(p); }
void SetTile(TILE *p) { setTile(p); }
void SetTile1(TILE_1 *p) { setTile1(p); }
void SetTile8(TILE_8 *p) { setTile8(p); }
void SetTile16(TILE_16 *p) { setTile16(p); }

/* ---- TIM ------------------------------------------------------------------------------- */
static u_long *sTimCursor;

int OpenTIM(u_long *addr)
{
    sTimCursor = addr;
    return 0;
}

TIM_IMAGE *ReadTIM(TIM_IMAGE *tim)
{
    u_long *p = sTimCursor;
    u_long flags;

    if (p == 0 || (p[0] & 0xFF) != 0x10)
    {
        return 0;
    }
    flags = p[1];
    tim->mode = flags;
    p += 2;
    if (flags & 8)
    {
        tim->crect = (RECT *)(p + 1);
        tim->caddr = p + 3;
        p += p[0] / 4;
    }
    else
    {
        tim->crect = 0;
        tim->caddr = 0;
    }
    tim->prect = (RECT *)(p + 1);
    tim->paddr = p + 3;
    p += p[0] / 4;
    sTimCursor = p;
    return tim;
}
