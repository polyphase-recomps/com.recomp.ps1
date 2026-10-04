/*
 * Software PlayStation GPU.
 *
 * Implements the GP0 drawing commands the PsyQ libraries emit (polygons,
 * lines, rectangles, fills, VRAM transfers, environment commands) over a
 * 1024x512 15-bit VRAM, with texture pages and CLUTs, modulation, the four
 * semi-transparency modes and the drawing area/offset. Display output turns
 * the display area into RGBA for the host.
 */
/* Also compiled into the host for the wasm2c guest (Source/Wasm/ps1w_gpu.c, which
 * includes the headers first): no byte-order assumptions about VRAM or packets. */
#ifndef PORT_GPU_H
#include <port_gpu.h>
#endif
#ifndef PORT_HOST_H
#include <port_host.h>
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef long s32;
typedef short s16;

unsigned short gpu_vram[512][1024];

static struct
{
    int clip_x0, clip_y0, clip_x1, clip_y1; /* inclusive */
    int off_x, off_y;
    int tex_x, tex_y, tex_mode, abr;          /* texpage */
    int tw_mask_x, tw_mask_y, tw_off_x, tw_off_y; /* texture window (in texels) */
    int dither;
    int set_mask, check_mask;
    int disp_x, disp_y, disp_w, disp_h, disp_rgb24, disp_on;
} G = { 0, 0, 1023, 511, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 320, 240, 0, 1 };

/* ---- pixel helpers --------------------------------------------------------------------- */
static u16 blend(u16 back, u16 front, int mode)
{
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;
    int fr = front & 31, fg = (front >> 5) & 31, fb = (front >> 10) & 31;
    int r, g, b;

    switch (mode)
    {
    case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
    case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
    default: r = br + (fr >> 2); g = bg + (fg >> 2); b = bb + (fb >> 2); break;
    }
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 31 ? 31 : g;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (u16)(r | (g << 5) | (b << 10) | (front & 0x8000));
}

/* --debug-gpustats 1: per-second counts (commands by GP0 group, pixels written) */
static unsigned sStatCmds[8], sStatPixels, sStatSemi, sStatTexels;

static void plot(int x, int y, u16 color, int semi, int mode)
{
    u16 *dst;

    sStatPixels++;
    sStatSemi += semi != 0;

    if (x < G.clip_x0 || x > G.clip_x1 || y < G.clip_y0 || y > G.clip_y1)
    {
        return;
    }
    dst = &gpu_vram[y & 511][x & 1023];
    if (G.check_mask && (*dst & 0x8000))
    {
        return;
    }
    if (semi)
    {
        color = blend(*dst, color, mode);
    }
    *dst = color | (G.set_mask ? 0x8000 : 0);
}

static u16 sample(int u, int v, int tex_x, int tex_y, int mode, int clut_x, int clut_y)
{
    u &= 0xFF;
    v &= 0xFF;
    u = (u & ~G.tw_mask_x) | (G.tw_off_x & G.tw_mask_x);
    v = (v & ~G.tw_mask_y) | (G.tw_off_y & G.tw_mask_y);
    switch (mode)
    {
    case 0:
    {
        u16 word = gpu_vram[(tex_y + v) & 511][(tex_x + (u >> 2)) & 1023];
        int index = (word >> ((u & 3) * 4)) & 0xF;

        return gpu_vram[clut_y & 511][(clut_x + index) & 1023];
    }
    case 1:
    {
        u16 word = gpu_vram[(tex_y + v) & 511][(tex_x + (u >> 1)) & 1023];
        int index = (word >> ((u & 1) * 8)) & 0xFF;

        return gpu_vram[clut_y & 511][(clut_x + index) & 1023];
    }
    default:
        return gpu_vram[(tex_y + v) & 511][(tex_x + u) & 1023];
    }
}

static u16 modulate(u16 texel, int r, int g, int b)
{
    int tr = ((texel & 31) * r) >> 7, tg = (((texel >> 5) & 31) * g) >> 7, tb = (((texel >> 10) & 31) * b) >> 7;

    if (tr > 31) tr = 31;
    if (tg > 31) tg = 31;
    if (tb > 31) tb = 31;
    return (u16)(tr | (tg << 5) | (tb << 10) | (texel & 0x8000));
}

static u16 rgb24_to_15(int r, int g, int b)
{
    return (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

/* The GPU's 4x4 ordered dither, applied to 8-bit colour before truncation to 5 bits. */
static const signed char kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static u16 rgb24_to_15_dither(int r, int g, int b, int x, int y)
{
    int d = kDither[y & 3][x & 3];

    return rgb24_to_15(clamp255(r + d), clamp255(g + d), clamp255(b + d));
}

static u16 modulate_dither(u16 texel, int r, int g, int b, int x, int y)
{
    int d = kDither[y & 3][x & 3];
    int tr = clamp255((((texel & 31) << 3) * r >> 7) + d);
    int tg = clamp255(((((texel >> 5) & 31) << 3) * g >> 7) + d);
    int tb = clamp255(((((texel >> 10) & 31) << 3) * b >> 7) + d);

    return (u16)(rgb24_to_15(tr, tg, tb) | (texel & 0x8000));
}

/* ---- triangles --------------------------------------------------------------------------- */
typedef struct Vtx
{
    int x, y;
    int r, g, b;
    int u, v;
} Vtx;

typedef struct PolyState
{
    int textured, raw, gouraud, semi, mode;
    int tex_x, tex_y, tex_mode, clut_x, clut_y;
} PolyState;

/* An attribute interpolated across a triangle, exactly as
 *   value = floor((w0 * a0 + w1 * a1 + w2 * a2) / area)
 * (barycentric weights w, all >= 0 inside), stepped along x without dividing: the
 * quotient and remainder advance by the per-pixel step's own quotient and remainder. */
typedef struct Interp
{
    int q, r;   /* current value and remainder, 0 <= r < area */
    int dq, dr; /* per pixel step */
    int row;    /* numerator at the first pixel (min_x) of the current row */
    int dx;     /* numerator step per pixel */
    int dy;     /* numerator step per row */
} Interp;

static void floor_div(int n, int d, int *q, int *r)
{
    *q = n / d;
    *r = n % d;
    if (*r < 0)
    {
        *r += d;
        (*q)--;
    }
}

static void interp_init(Interp *it, int a0, int a1, int a2, int ex0, int ex1, int ex2, int ey0, int ey1, int ey2,
                        int w0, int w1, int w2, int area)
{
    it->row = w0 * a0 + w1 * a1 + w2 * a2;
    it->dy = ey0 * a0 + ey1 * a1 + ey2 * a2;
    it->dx = ex0 * a0 + ex1 * a1 + ex2 * a2;
    floor_div(it->dx, area, &it->dq, &it->dr);
}

/* value at pixel min_x + skip of the current row; then on to the next row */
static void interp_row(Interp *it, int skip, int area)
{
    floor_div(it->row + it->dx * skip, area, &it->q, &it->r);
    it->row += it->dy;
}

/* First x >= x0 where the edge function w(x) = w + e * (x - x0) is >= 0 (e > 0). */
static int edge_first(int w, int e)
{
    return w >= 0 ? 0 : (-w + e - 1) / e;
}

/* Last offset where w + e * offset >= 0 (e < 0), or -1 if none. */
static int edge_last(int w, int e)
{
    return w < 0 ? -1 : w / -e;
}

/* Narrows [first, last] (pixel offsets in the row) to where edge w + e * offset >= 0. */
static void edge_span(int w, int e, int *first, int *last)
{
    if (e > 0)
    {
        int f = edge_first(w, e);

        if (f > *first) *first = f;
    }
    else if (e < 0)
    {
        int l = edge_last(w, e);

        if (l < *last) *last = l;
    }
    else if (w < 0)
    {
        *last = -1;
    }
}

#define INTERP_STEP(it, area)          \
    do                                 \
    {                                  \
        (it).q += (it).dq;             \
        (it).r += (it).dr;             \
        if ((it).r >= (area))          \
        {                              \
            (it).r -= (area);          \
            (it).q++;                  \
        }                              \
    } while (0)

static void draw_triangle(const Vtx *v0, const Vtx *v1, const Vtx *v2, const PolyState *ps)
{
    int min_x, max_x, min_y, max_y, x, y, area;
    int ex0, ex1, ex2, ey0, ey1, ey2, w0row, w1row, w2row, excl0, excl1, excl2, fast, unmodulated;
    const u16 *clut_row, *clut;
    Interp ir, ig, ib, iu, iv;

    /* reject huge polygons like the hardware does (this also keeps every product below in
     * 32 bits: edge functions within the bounding box stay under 2^21, numerators 2^30) */
    if (v0->x - v1->x > 1023 || v1->x - v0->x > 1023 || v0->x - v2->x > 1023 || v2->x - v0->x > 1023 ||
        v1->x - v2->x > 1023 || v2->x - v1->x > 1023 || v0->y - v1->y > 511 || v1->y - v0->y > 511 ||
        v0->y - v2->y > 511 || v2->y - v0->y > 511 || v1->y - v2->y > 511 || v2->y - v1->y > 511)
    {
        return;
    }
    area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0)
    {
        return;
    }
    if (area < 0)
    {
        const Vtx *t = v1;
        v1 = v2;
        v2 = t;
        area = -area;
    }
    min_x = v0->x; max_x = v0->x; min_y = v0->y; max_y = v0->y;
    if (v1->x < min_x) min_x = v1->x; if (v1->x > max_x) max_x = v1->x;
    if (v2->x < min_x) min_x = v2->x; if (v2->x > max_x) max_x = v2->x;
    if (v1->y < min_y) min_y = v1->y; if (v1->y > max_y) max_y = v1->y;
    if (v2->y < min_y) min_y = v2->y; if (v2->y > max_y) max_y = v2->y;
    if (min_x < G.clip_x0) min_x = G.clip_x0;
    if (min_y < G.clip_y0) min_y = G.clip_y0;
    if (max_x > G.clip_x1) max_x = G.clip_x1;
    if (max_y > G.clip_y1) max_y = G.clip_y1;
    if (min_x > max_x || min_y > max_y)
    {
        return;
    }

    /* edge functions (pixel centres at integer coordinates: the PS1 has no half-pixel
     * offset) and their steps along x and y */
    ex0 = -(v2->y - v1->y); ey0 = v2->x - v1->x;
    ex1 = -(v0->y - v2->y); ey1 = v0->x - v2->x;
    ex2 = -(v1->y - v0->y); ey2 = v1->x - v0->x;
    w0row = ey0 * (min_y - v1->y) + ex0 * (min_x - v1->x);
    w1row = ey1 * (min_y - v2->y) + ex1 * (min_x - v2->x);
    w2row = ey2 * (min_y - v0->y) + ex2 * (min_x - v0->x);
    /* top-left style tie rule so shared edges are drawn once: exclude right edges (going
     * down) and bottom edges (going left); the interior is on the positive side */
    excl0 = v2->y - v1->y > 0 || (v2->y == v1->y && v2->x - v1->x < 0);
    excl1 = v0->y - v2->y > 0 || (v0->y == v2->y && v0->x - v2->x < 0);
    excl2 = v1->y - v0->y > 0 || (v1->y == v0->y && v1->x - v0->x < 0);

    if (ps->gouraud)
    {
        interp_init(&ir, v0->r, v1->r, v2->r, ex0, ex1, ex2, ey0, ey1, ey2, w0row, w1row, w2row, area);
        interp_init(&ig, v0->g, v1->g, v2->g, ex0, ex1, ex2, ey0, ey1, ey2, w0row, w1row, w2row, area);
        interp_init(&ib, v0->b, v1->b, v2->b, ex0, ex1, ex2, ey0, ey1, ey2, w0row, w1row, w2row, area);
    }
    if (ps->textured)
    {
        interp_init(&iu, v0->u, v1->u, v2->u, ex0, ex1, ex2, ey0, ey1, ey2, w0row, w1row, w2row, area);
        interp_init(&iv, v0->v, v1->v, v2->v, ex0, ex1, ex2, ey0, ey1, ey2, w0row, w1row, w2row, area);
    }
    fast = ps->textured && !ps->gouraud && !G.check_mask && !G.set_mask && G.tw_mask_x == 0 && G.tw_mask_y == 0;
    /* modulating by 128 (or raw texturing) leaves the texel as it is */
    unmodulated = ps->raw || (v0->r == 128 && v0->g == 128 && v0->b == 128);
    clut_row = gpu_vram[ps->clut_y & 511];
    clut = clut_row + ps->clut_x; /* 4-bit: clut_x + 15 stays within the row */

    for (y = min_y; y <= max_y; y++, w0row += ey0, w1row += ey1, w2row += ey2)
    {
        /* this row's span inside all three edges (ties are still checked per pixel) */
        int first = 0, last = max_x - min_x, w0, w1, w2, skip;

        edge_span(w0row, ex0, &first, &last);
        edge_span(w1row, ex1, &first, &last);
        edge_span(w2row, ex2, &first, &last);
        skip = first <= last ? first : 0;
        if (ps->gouraud)
        {
            interp_row(&ir, skip, area);
            interp_row(&ig, skip, area);
            interp_row(&ib, skip, area);
        }
        if (ps->textured)
        {
            interp_row(&iu, skip, area);
            interp_row(&iv, skip, area);
        }
        if (first > last)
        {
            continue;
        }
        w0 = w0row + ex0 * skip;
        w1 = w1row + ex1 * skip;
        w2 = w2row + ex2 * skip;
        if (fast)
        {
            /* textured, flat colour, no mask bits, no texture window: the common case
             * (backgrounds, sprites as quads, most models), done without the generic
             * helpers. Same results as the loop below: the span is inside the clip area
             * and every edge function is >= 0 in it, only ties need checking. */
            u16 *dst = gpu_vram[y & 511];
            const int x_end = min_x + last;

            for (x = min_x + first; x <= x_end; x++)
            {
                if (!((w0 == 0 && excl0) || (w1 == 0 && excl1) || (w2 == 0 && excl2)))
                {
                    const int u = iu.q & 0xFF, v = iv.q & 0xFF;
                    const u16 *trow = gpu_vram[(ps->tex_y + v) & 511];
                    u16 texel;

                    switch (ps->tex_mode)
                    {
                    case 0:
                        texel = clut[(trow[(ps->tex_x + (u >> 2)) & 1023] >> ((u & 3) * 4)) & 0xF];
                        break;
                    case 1:
                        texel = clut_row[(ps->clut_x + ((trow[(ps->tex_x + (u >> 1)) & 1023] >> ((u & 1) * 8)) & 0xFF)) & 1023];
                        break;
                    default:
                        texel = trow[(ps->tex_x + u) & 1023];
                        break;
                    }
                    if (texel != 0)
                    {
                        u16 color = unmodulated ? texel : modulate(texel, v0->r, v0->g, v0->b);

                        if (ps->semi && (texel & 0x8000)) color = blend(dst[x], color, ps->mode);
                        dst[x] = color;
                    }
                }
                w0 += ex0;
                w1 += ex1;
                w2 += ex2;
                INTERP_STEP(iu, area);
                INTERP_STEP(iv, area);
            }
            continue;
        }
        for (x = min_x + first; x <= min_x + last; x++)
        {
            int inside = (w0 | w1 | w2) >= 0 && !(w0 == 0 && excl0) && !(w1 == 0 && excl1) && !(w2 == 0 && excl2);

            if (inside)
            {
                int r, g, b;
                u16 color;

                if (ps->gouraud)
                {
                    r = ir.q; g = ig.q; b = ib.q;
                }
                else
                {
                    r = v0->r; g = v0->g; b = v0->b;
                }
                if (ps->textured)
                {
                    u16 texel = sample(iu.q, iv.q, ps->tex_x, ps->tex_y, ps->tex_mode, ps->clut_x, ps->clut_y);

                    if (texel != 0)
                    {
                        if (ps->raw)
                            color = texel;
                        else if (G.dither && ps->gouraud)
                            color = modulate_dither(texel, r, g, b, x, y);
                        else
                            color = modulate(texel, r, g, b);
                        plot(x, y, color, ps->semi && (texel & 0x8000), ps->mode);
                    }
                }
                else
                {
                    plot(x, y, (G.dither && ps->gouraud) ? rgb24_to_15_dither(r, g, b, x, y) : rgb24_to_15(r, g, b),
                         ps->semi, ps->mode);
                }
            }
            w0 += ex0;
            w1 += ex1;
            w2 += ex2;
            if (ps->gouraud)
            {
                INTERP_STEP(ir, area);
                INTERP_STEP(ig, area);
                INTERP_STEP(ib, area);
            }
            if (ps->textured)
            {
                INTERP_STEP(iu, area);
                INTERP_STEP(iv, area);
            }
        }
    }
}

static int sx11(u32 word) /* signed 11-bit x */
{
    return ((s32)(word << 21)) >> 21;
}

static int sy11(u32 word) /* signed 11-bit y from the high half */
{
    return ((s32)((word >> 16) << 21)) >> 21;
}

static void apply_texpage(PolyState *ps, u16 tpage)
{
    ps->tex_x = (tpage & 0xF) * 64;
    ps->tex_y = ((tpage >> 4) & 1) * 256;
    ps->tex_mode = (tpage >> 7) & 3;
    ps->mode = (tpage >> 5) & 3;
}

static int cmd_polygon(const u32 *w)
{
    u32 cmd = w[0] >> 24;
    int quad = (cmd & 0x08) != 0, gouraud = (cmd & 0x10) != 0, textured = (cmd & 0x04) != 0;
    int nverts = quad ? 4 : 3, i, idx = 1;
    Vtx v[4];
    PolyState ps;

    ps.textured = textured;
    ps.raw = (cmd & 0x01) != 0;
    ps.gouraud = gouraud;
    ps.semi = (cmd & 0x02) != 0;
    ps.mode = G.abr;
    ps.tex_x = G.tex_x; ps.tex_y = G.tex_y; ps.tex_mode = G.tex_mode;
    ps.clut_x = ps.clut_y = 0;
    for (i = 0; i < nverts; i++)
    {
        u32 color = (i == 0 || !gouraud) ? w[0] : w[idx++];
        u32 pos = w[idx++];

        v[i].r = color & 0xFF;
        v[i].g = (color >> 8) & 0xFF;
        v[i].b = (color >> 16) & 0xFF;
        v[i].x = sx11(pos) + G.off_x;
        v[i].y = sy11(pos) + G.off_y;
        v[i].u = v[i].v = 0;
        if (textured)
        {
            u32 uv = w[idx++];

            v[i].u = uv & 0xFF;
            v[i].v = (uv >> 8) & 0xFF;
            if (i == 0)
            {
                ps.clut_x = ((uv >> 16) & 0x3F) * 16;
                ps.clut_y = (uv >> 22) & 0x1FF;
            }
            else if (i == 1)
            {
                u16 tpage = (u16)(uv >> 16);

                apply_texpage(&ps, tpage);
                /* a textured polygon also updates the draw mode texpage */
                G.tex_x = ps.tex_x; G.tex_y = ps.tex_y; G.tex_mode = ps.tex_mode; G.abr = ps.mode;
            }
        }
    }
    if (!gouraud)
    {
        for (i = 1; i < nverts; i++)
        {
            v[i].r = v[0].r; v[i].g = v[0].g; v[i].b = v[0].b;
        }
    }
    draw_triangle(&v[0], &v[1], &v[2], &ps);
    if (quad)
    {
        draw_triangle(&v[1], &v[2], &v[3], &ps);
    }
    return idx;
}

/* ---- lines ------------------------------------------------------------------------------- */
static void draw_line(int x0, int y0, int x1, int y1, u32 c0, u32 c1, int gouraud, int semi)
{
    int dx = x1 - x0, dy = y1 - y0, steps, i;

    steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps > 1023)
    {
        return;
    }
    for (i = 0; i <= steps; i++)
    {
        int x = steps ? x0 + dx * i / steps : x0, y = steps ? y0 + dy * i / steps : y0;
        int r = c0 & 0xFF, g = (c0 >> 8) & 0xFF, b = (c0 >> 16) & 0xFF;

        if (gouraud && steps)
        {
            r += (((int)(c1 & 0xFF) - r) * i) / steps;
            g += (((int)((c1 >> 8) & 0xFF) - g) * i) / steps;
            b += (((int)((c1 >> 16) & 0xFF) - b) * i) / steps;
        }
        plot(x, y, rgb24_to_15(r, g, b), semi, G.abr);
    }
}

static int cmd_line(const u32 *w, int remaining)
{
    u32 cmd = w[0] >> 24;
    int gouraud = (cmd & 0x10) != 0, poly = (cmd & 0x08) != 0, semi = (cmd & 0x02) != 0;
    int idx = 1, x0, y0;
    u32 c0 = w[0], c1;

    x0 = sx11(w[idx]) + G.off_x;
    y0 = sy11(w[idx]) + G.off_y;
    idx++;
    for (;;)
    {
        int x1, y1;

        if (idx >= remaining) break;
        if (gouraud)
        {
            if (poly && (w[idx] & 0xF000F000) == 0x50005000) { idx++; break; }
            c1 = w[idx++];
        }
        else c1 = c0;
        if (idx >= remaining) break;
        if (poly && (w[idx] & 0xF000F000) == 0x50005000) { idx++; break; }
        x1 = sx11(w[idx]) + G.off_x;
        y1 = sy11(w[idx]) + G.off_y;
        idx++;
        draw_line(x0, y0, x1, y1, c0, c1, gouraud, semi);
        x0 = x1; y0 = y1; c0 = c1;
        if (!poly) break;
    }
    return idx;
}

/* ---- rectangles -------------------------------------------------------------------------- */
static int cmd_rect(const u32 *w)
{
    u32 cmd = w[0] >> 24;
    int textured = (cmd & 0x04) != 0, raw = (cmd & 0x01) != 0, semi = (cmd & 0x02) != 0;
    int size = (cmd >> 3) & 3, idx = 1, x, y, width, height, u0 = 0, v0 = 0, clut_x = 0, clut_y = 0, i, j;
    int r = w[0] & 0xFF, g = (w[0] >> 8) & 0xFF, b = (w[0] >> 16) & 0xFF;

    x = sx11(w[idx]) + G.off_x;
    y = sy11(w[idx]) + G.off_y;
    idx++;
    if (textured)
    {
        u0 = w[idx] & 0xFF;
        v0 = (w[idx] >> 8) & 0xFF;
        clut_x = ((w[idx] >> 16) & 0x3F) * 16;
        clut_y = (w[idx] >> 22) & 0x1FF;
        idx++;
    }
    switch (size)
    {
    case 0: width = w[idx] & 0x3FF; height = (w[idx] >> 16) & 0x1FF; idx++; break;
    case 1: width = height = 1; break;
    case 2: width = height = 8; break;
    default: width = height = 16; break;
    }
    for (j = 0; j < height; j++)
    {
        for (i = 0; i < width; i++)
        {
            if (textured)
            {
                u16 texel = sample(u0 + i, v0 + j, G.tex_x, G.tex_y, G.tex_mode, clut_x, clut_y);

                if (texel == 0) continue;
                plot(x + i, y + j, raw ? texel : modulate(texel, r, g, b), semi && (texel & 0x8000), G.abr);
            }
            else
            {
                plot(x + i, y + j, rgb24_to_15(r, g, b), semi, G.abr);
            }
        }
    }
    return idx;
}

/* ---- VRAM transfers -------------------------------------------------------------------------- */
void gpu_fill(int x, int y, int w, int h, int r, int g, int b)
{
    u16 color = rgb24_to_15(r, g, b);
    int i, j;

    for (j = 0; j < h; j++)
    {
        for (i = 0; i < w; i++)
        {
            gpu_vram[(y + j) & 511][(x + i) & 1023] = color;
        }
    }
}

void gpu_load_image(int x, int y, int w, int h, const void *src)
{
    const u16 *p = src;
    int i, j;

    for (j = 0; j < h; j++)
    {
        for (i = 0; i < w; i++)
        {
            gpu_vram[(y + j) & 511][(x + i) & 1023] = *p++;
        }
    }
}

void gpu_store_image(int x, int y, int w, int h, void *dst)
{
    u16 *p = dst;
    int i, j;

    for (j = 0; j < h; j++)
    {
        for (i = 0; i < w; i++)
        {
            *p++ = gpu_vram[(y + j) & 511][(x + i) & 1023];
        }
    }
}

void gpu_move_image(int sx, int sy, int dx, int dy, int w, int h)
{
    /* row by row through a row buffer, in the order that keeps overlapping source rows
     * intact until they are copied */
    u16 row[1024];
    int i, k, j, step = dy > sy ? -1 : 1;

    for (k = 0, j = step < 0 ? h - 1 : 0; k < h; k++, j += step)
    {
        for (i = 0; i < w; i++) row[i] = gpu_vram[(sy + j) & 511][(sx + i) & 1023];
        for (i = 0; i < w; i++) gpu_vram[(dy + j) & 511][(dx + i) & 1023] = row[i];
    }
}

/* ---- environment ---------------------------------------------------------------------------- */
void gpu_set_draw_area(int x0, int y0, int x1, int y1)
{
    G.clip_x0 = x0 < 0 ? 0 : x0;
    G.clip_y0 = y0 < 0 ? 0 : y0;
    G.clip_x1 = x1 > 1023 ? 1023 : x1;
    G.clip_y1 = y1 > 511 ? 511 : y1;
}

void gpu_set_draw_offset(int x, int y)
{
    G.off_x = x;
    G.off_y = y;
}

void gpu_set_texpage(unsigned short tpage)
{
    G.tex_x = (tpage & 0xF) * 64;
    G.tex_y = ((tpage >> 4) & 1) * 256;
    G.abr = (tpage >> 5) & 3;
    G.tex_mode = (tpage >> 7) & 3;
    G.dither = (tpage >> 9) & 1;
}

void gpu_set_texwindow(int x, int y, int w, int h)
{
    /* PsyQ RECT in texels; the hardware works in 8-pixel units with masks */
    if (w == 0 || h == 0)
    {
        G.tw_mask_x = G.tw_mask_y = G.tw_off_x = G.tw_off_y = 0;
        return;
    }
    G.tw_mask_x = (~(w - 1)) & 0xFF;
    G.tw_mask_y = (~(h - 1)) & 0xFF;
    G.tw_off_x = x & G.tw_mask_x;
    G.tw_off_y = y & G.tw_mask_y;
    if (w >= 256) G.tw_mask_x = 0;
    if (h >= 256) G.tw_mask_y = 0;
}

void gpu_set_dither(int on)
{
    G.dither = on;
}

void gpu_gp0(const unsigned long *words, int count)
{
    const u32 *w = (const u32 *)words;
    int pos = 0;

    static int trace;

    trace = port_trace_gpu();
    while (pos < count)
    {
        u32 cmd = w[pos] >> 24;

        sStatCmds[cmd >> 5]++;
        int used = 1;

        if (trace)
        {
            port_log("gp0 %02lX: %08lX %08lX %08lX %08lX %08lX %08lX %08lX", cmd, w[pos], count - pos > 1 ? w[pos + 1] : 0,
                     count - pos > 2 ? w[pos + 2] : 0, count - pos > 3 ? w[pos + 3] : 0, count - pos > 4 ? w[pos + 4] : 0,
                     count - pos > 5 ? w[pos + 5] : 0, count - pos > 6 ? w[pos + 6] : 0);
        }

        if (cmd >= 0x20 && cmd < 0x40)
        {
            used = cmd_polygon(&w[pos]);
        }
        else if (cmd >= 0x40 && cmd < 0x60)
        {
            used = cmd_line(&w[pos], count - pos);
        }
        else if (cmd >= 0x60 && cmd < 0x80)
        {
            used = cmd_rect(&w[pos]);
        }
        else switch (cmd)
        {
        case 0x02: /* fill rectangle (ignores clipping and offset) */
        {
            int x = w[pos + 1] & 0x3F0, y = (w[pos + 1] >> 16) & 0x1FF;
            int width = ((w[pos + 2] & 0x3FF) + 0xF) & ~0xF, height = (w[pos + 2] >> 16) & 0x1FF;

            gpu_fill(x, y, width, height, w[pos] & 0xFF, (w[pos] >> 8) & 0xFF, (w[pos] >> 16) & 0xFF);
            used = 3;
            break;
        }
        case 0x80: /* VRAM to VRAM */
            gpu_move_image(w[pos + 1] & 0x3FF, (w[pos + 1] >> 16) & 0x1FF, w[pos + 2] & 0x3FF, (w[pos + 2] >> 16) & 0x1FF,
                           ((w[pos + 3] - 1) & 0x3FF) + 1, (((w[pos + 3] >> 16) - 1) & 0x1FF) + 1);
            used = 4;
            break;
        case 0xA0: /* CPU to VRAM with inline data */
        {
            int x = w[pos + 1] & 0x3FF, y = (w[pos + 1] >> 16) & 0x1FF;
            int width = ((w[pos + 2] - 1) & 0x3FF) + 1, height = (((w[pos + 2] >> 16) - 1) & 0x1FF) + 1;
            int words_needed = (width * height + 1) / 2;

            if (pos + 3 + words_needed <= count)
            {
                /* two pixels per word, low half first (by value: any byte order) */
                int k;

                for (k = 0; k < width * height; k++)
                {
                    u32 word = w[pos + 3 + k / 2];

                    gpu_vram[(y + k / width) & 511][(x + k % width) & 1023] = (u16)((k & 1) ? word >> 16 : word);
                }
            }
            used = 3 + words_needed;
            break;
        }
        case 0xC0:
            used = 3;
            break;
        case 0xE1:
            gpu_set_texpage((u16)(w[pos] & 0x3FFF));
            break;
        case 0xE2:
        {
            u32 v = w[pos];
            int mx = v & 0x1F, my = (v >> 5) & 0x1F, ox = (v >> 10) & 0x1F, oy = (v >> 15) & 0x1F;

            G.tw_mask_x = mx * 8;
            G.tw_mask_y = my * 8;
            G.tw_off_x = (ox * 8) & G.tw_mask_x;
            G.tw_off_y = (oy * 8) & G.tw_mask_y;
            break;
        }
        case 0xE3:
            G.clip_x0 = w[pos] & 0x3FF;
            G.clip_y0 = (w[pos] >> 10) & 0x1FF;
            break;
        case 0xE4:
            G.clip_x1 = w[pos] & 0x3FF;
            G.clip_y1 = (w[pos] >> 10) & 0x1FF;
            break;
        case 0xE5:
            G.off_x = ((s32)(w[pos] << 21)) >> 21;
            G.off_y = ((s32)((w[pos] >> 11) << 21)) >> 21;
            break;
        case 0xE6:
            G.set_mask = w[pos] & 1;
            G.check_mask = (w[pos] >> 1) & 1;
            break;
        default:
            break;
        }
        pos += used;
    }
}

void gpu_draw_otag(const unsigned long *ot)
{
    const u32 *p = (const u32 *)ot;
    int guard = 0;

    while (p != 0 && guard++ < 200000)
    {
        u32 tag = p[0];
        int len = tag >> 24;

        if (len > 0)
        {
            gpu_gp0((const unsigned long *)&p[1], len);
        }
        if ((tag & 0xFFFFFF) == 0xFFFFFF)
        {
            break;
        }
        p = (const u32 *)((tag & 0xFFFFFF) | 0x80000000u);
    }
}

/* ---- display --------------------------------------------------------------------------------- */
void gpu_set_display(int x, int y, int w, int h, int rgb24)
{
    G.disp_x = x;
    G.disp_y = y;
    G.disp_w = w > 0 ? w : 320;
    G.disp_h = h > 0 ? h : 240;
    G.disp_rgb24 = rgb24;
}

void gpu_set_display_enabled(int on)
{
    G.disp_on = on;
}

#if PORT_GPU_IN_HOST
/* RGBA staging for port_present, as large as the display (host build: malloc). */
#include <stdlib.h>
static unsigned char *sPresentRgba;
static unsigned long sPresentSize;

static unsigned char *present_staging(unsigned long size)
{
    if (size > sPresentSize)
    {
        free(sPresentRgba);
        sPresentRgba = (unsigned char *)malloc(size);
        sPresentSize = sPresentRgba ? size : 0;
    }
    return sPresentRgba;
}
#else
/* RGBA staging for port_present; the movie player borrows it (port_play_movie runs
 * while the game, and so the GPU, waits). */
static unsigned char sPresentRgba[640 * 512 * 4];

unsigned char *gpu_present_buffer(unsigned long *size)
{
    *size = sizeof sPresentRgba;
    return sPresentRgba;
}

static unsigned char *present_staging(unsigned long size)
{
    return sPresentRgba;
}
#endif

void gpu_present(void)
{
    unsigned char *rgba;
    int w = G.disp_w > 640 ? 640 : G.disp_w, h = G.disp_h > 512 ? 512 : G.disp_h, x, y;

    rgba = present_staging((unsigned long)w * h * 4);
    if (rgba == 0) return;

    for (y = 0; y < h; y++)
    {
        unsigned char *out = &rgba[(y * w) * 4];

        if (!G.disp_on)
        {
            for (x = 0; x < w * 4; x++) out[x] = (x & 3) == 3 ? 255 : 0;
            continue;
        }
        if (G.disp_rgb24)
        {
            /* the row as bytes, little-endian like the PS1 VRAM (by value: any host) */
            const u16 *row = gpu_vram[(G.disp_y + y) & 511];
            int base = G.disp_x * 2;

#define VRAM_BYTE(o) ((u8)((o) & 1 ? row[((o) >> 1) & 1023] >> 8 : row[((o) >> 1) & 1023]))
            for (x = 0; x < w; x++)
            {
                int o = (base + x * 3) % 2048;

                out[x * 4] = VRAM_BYTE(o);
                out[x * 4 + 1] = VRAM_BYTE((o + 1) % 2048);
                out[x * 4 + 2] = VRAM_BYTE((o + 2) % 2048);
                out[x * 4 + 3] = 255;
            }
#undef VRAM_BYTE
        }
        else
        {
            for (x = 0; x < w; x++)
            {
                u16 c = gpu_vram[(G.disp_y + y) & 511][(G.disp_x + x) & 1023];

                out[x * 4] = (u8)(((c & 31) << 3) | ((c & 31) >> 2));
                out[x * 4 + 1] = (u8)((((c >> 5) & 31) << 3) | (((c >> 5) & 31) >> 2));
                out[x * 4 + 2] = (u8)((((c >> 10) & 31) << 3) | (((c >> 10) & 31) >> 2));
                out[x * 4 + 3] = 255;
            }
        }
    }
    port_present(rgba, w, h);
    port_debug_vram(&gpu_vram[0][0]);
    {
        static int enabled = -1, frames;
        int v[1];

        if (enabled < 0) enabled = port_debug_values("gpustats", v, 1) == 1 && v[0];
        if (enabled && ++frames == 60)
        {
            port_log("gpustats/60 frames: polys %u lines %u rects %u (fill/copy %u %u %u) env %u; pixels %u (semi %u)",
                     sStatCmds[1], sStatCmds[2], sStatCmds[3], sStatCmds[0], sStatCmds[4] + sStatCmds[6],
                     sStatCmds[5], sStatCmds[7], sStatPixels, sStatSemi);
            frames = 0;
            sStatPixels = sStatSemi = sStatTexels = 0;
            for (v[0] = 0; v[0] < 8; v[0]++) sStatCmds[v[0]] = 0;
        }
    }
}
