/*
 * PsyQ libgs BG (cell map) sorting: GsSortBg / GsSortFastBg. A GsBG shows its GsMAP
 * (cells of cellw x cellh texels, repeating in both directions) in a w x h window at
 * (x, y), scrolled by (scrollx, scrolly), scaled and rotated around (mx, my) of the
 * window. Each visible cell becomes one textured quad.
 */
#include <sys/types.h>
#include <libgte.h>
#include <libgpu.h>
#include <libgs.h>

extern PACKET *GsOUT_PACKET_P;
int rcos(int a);
int rsin(int a);

#define BG_FLIP_H 1
#define BG_FLIP_V 2

static int wrap(int v, int n)
{
    v %= n;
    return v < 0 ? v + n : v;
}

static int floor_div(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

void GsSortBg(GsBG *bg, GsOT *ot, unsigned short pri)
{
    GsMAP *map = bg->map;
    u_long *p = (u_long *)GsOUT_PACKET_P;
    u_long code = 0x2C, mode;
    long c, s, sx, sy, ox, oy;
    int cw, ch, cx0, cy0, cx1, cy1, cx, cy;
    int plain;

    if ((bg->attribute & 0x80000000) || map == 0 || map->cellw == 0 || map->cellh == 0 || map->ncellw == 0 ||
        map->ncellh == 0)
    {
        return;
    }
    if (bg->attribute & (1 << 30)) code |= 2; /* semi-transparent */
    if (bg->attribute & (1 << 6)) code |= 1;  /* no brightness (colour) modulation */
    mode = (((bg->attribute >> 24) & 3) << 7) | (((bg->attribute >> 28) & 3) << 5);

    cw = map->cellw ? map->cellw : 256;
    ch = map->cellh ? map->cellh : 256;
    sx = bg->scalex ? bg->scalex : 4096;
    sy = bg->scaley ? bg->scaley : 4096;
    c = rcos(bg->rotate / 360);
    s = rsin(bg->rotate / 360);
    plain = bg->rotate == 0 && sx == 4096 && sy == 4096;
    ox = bg->x + bg->mx;
    oy = bg->y + bg->my;

    /* the cells (in map coordinates) the window covers */
    cx0 = floor_div(bg->scrollx, cw);
    cy0 = floor_div(bg->scrolly, ch);
    cx1 = floor_div(bg->scrollx + bg->w - 1, cw);
    cy1 = floor_div(bg->scrolly + bg->h - 1, ch);

    for (cy = cy0; cy <= cy1; cy++)
    {
        for (cx = cx0; cx <= cx1; cx++)
        {
            int index = map->index[wrap(cy, map->ncellh) * map->ncellw + wrap(cx, map->ncellw)];
            GsCELL *cell;
            long lx[4], ly[4];
            int u0, v0, u1, v1, i;

            if (index == 0xFFFF) continue;
            cell = &map->base[index];
            u0 = cell->u;
            v0 = cell->v;
            u1 = u0 + cw;
            v1 = v0 + ch;
            if (u1 > 255) u1 = 255;
            if (v1 > 255) v1 = 255;
            if (cell->flag & BG_FLIP_H) i = u0, u0 = u1, u1 = i;
            if (cell->flag & BG_FLIP_V) i = v0, v0 = v1, v1 = i;

            /* corners relative to the pivot: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right */
            lx[0] = lx[2] = (long)cx * cw - bg->scrollx - bg->mx;
            lx[1] = lx[3] = lx[0] + cw;
            ly[0] = ly[1] = (long)cy * ch - bg->scrolly - bg->my;
            ly[2] = ly[3] = ly[0] + ch;

            p[0] = 9 << 24;
            p[1] = (code << 24) | ((u_long)bg->b << 16) | ((u_long)bg->g << 8) | bg->r;
            for (i = 0; i < 4; i++)
            {
                long x = lx[i], y = ly[i];
                int u = (i & 1) ? u1 : u0;
                int v = (i & 2) ? v1 : v0;
                u_long uv = ((u_long)v << 8) | (u_long)u;

                if (!plain)
                {
                    long ax = (x * sx) >> 12, ay = (y * sy) >> 12;

                    x = (ax * c - ay * s) >> 12;
                    y = (ax * s + ay * c) >> 12;
                }
                p[2 + i * 2] = ((u_long)(u_short)(oy + y) << 16) | (u_short)(ox + x);
                if (i == 0) uv |= (u_long)cell->cba << 16;
                if (i == 1) uv |= (u_long)((cell->tpage & ~0x1E0) | mode) << 16;
                p[3 + i * 2] = uv;
            }
            addPrim(ot->org + pri, p);
            p += 10;
        }
    }
    GsOUT_PACKET_P = (PACKET *)p;
}

void GsSortFastBg(GsBG *bg, GsOT *ot, unsigned short pri)
{
    GsSortBg(bg, ot, pri);
}
