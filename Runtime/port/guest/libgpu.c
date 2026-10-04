/* PsyQ libgpu replacement on top of the software GPU. */
#include <sys/types.h>
#include <libgpu.h>

#include <port_gpu.h>
#include <port_host.h>

static int sGraphDebug;

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

DISPENV *PutDispEnv(DISPENV *env)
{
    int h = env->disp.h;

    gpu_set_display(env->disp.x, env->disp.y, env->disp.w, h, env->isrgb24);
    return env;
}

DRAWENV *PutDrawEnv(DRAWENV *env)
{
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

void SetLineF2(LINE_F2 *p) { setLineF2(p); }
void SetLineF3(LINE_F3 *p) { setLineF3(p); }
void SetLineF4(LINE_F4 *p) { setLineF4(p); }
void SetPolyF4(POLY_F4 *p) { setPolyF4(p); }
void SetPolyFT3(POLY_FT3 *p) { setPolyFT3(p); }
void SetPolyFT4(POLY_FT4 *p) { setPolyFT4(p); }
void SetPolyG4(POLY_G4 *p) { setPolyG4(p); }
void SetPolyGT4(POLY_GT4 *p) { setPolyGT4(p); }
void SetSemiTrans(void *p, int abe) { setSemiTrans(p, abe); }

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
