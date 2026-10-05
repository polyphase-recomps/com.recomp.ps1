/*
 * PsyQ libpress (MDEC) replacement: DecDCTvlc turns an STR frame's bitstream (v2/v3)
 * into the MDEC's run-level codes, DecDCTin decodes those into pixels (16-bit or 24-bit,
 * macroblock after macroblock, each 16 x 16 row by row, as the MDEC outputs them) and
 * DecDCTout hands the pixels out in the sizes the game asks for, calling the
 * DecDCToutCallback after each, like the MDEC's output DMA. Everything completes at
 * once; a callback that asks for the next piece gets it after it returns (no
 * recursion).
 */
#include <port_host.h>

extern double cos(double);
extern double sqrt(double);

/* ---- bitstream ------------------------------------------------------------------------- */
typedef struct
{
    const unsigned char *data;
    int size; /* bytes */
    int pos;  /* bit position: 16-bit little-endian words, most significant bit first */
} Bits;

static unsigned peek(Bits *b, int n)
{
    unsigned v = 0;
    int i, p = b->pos;

    for (i = 0; i < n; i++, p++)
    {
        int word = p >> 4, bit = 15 - (p & 15);
        unsigned w = 0;

        if (word * 2 + 1 < b->size) w = b->data[word * 2] | (b->data[word * 2 + 1] << 8);
        v = (v << 1) | ((w >> bit) & 1);
    }
    return v;
}

static unsigned take(Bits *b, int n)
{
    unsigned v = peek(b, n);

    b->pos += n;
    return v;
}

/* ---- AC coefficient VLC (MPEG-1 table B.14 without the sign bit) ---------------------- */
typedef struct
{
    const char *code;
    unsigned char run, level;
} Vlc;

static const Vlc kAc[] = {
    {"11", 0, 1}, {"011", 1, 1}, {"0100", 0, 2}, {"0101", 2, 1}, {"00101", 0, 3}, {"00111", 3, 1},
    {"00110", 4, 1}, {"000110", 1, 2}, {"000111", 5, 1}, {"000101", 6, 1}, {"000100", 7, 1},
    {"0000110", 0, 4}, {"0000100", 2, 2}, {"0000111", 8, 1}, {"0000101", 9, 1},
    {"00100110", 0, 5}, {"00100001", 0, 6}, {"00100101", 1, 3}, {"00100100", 3, 2}, {"00100111", 10, 1},
    {"00100011", 11, 1}, {"00100010", 12, 1}, {"00100000", 13, 1},
    {"0000001010", 0, 7}, {"0000001100", 1, 4}, {"0000001011", 2, 3}, {"0000001111", 4, 2},
    {"0000001001", 5, 2}, {"0000001110", 14, 1}, {"0000001101", 15, 1}, {"0000001000", 16, 1},
    {"000000011101", 0, 8}, {"000000011000", 0, 9}, {"000000010011", 0, 10}, {"000000010000", 0, 11},
    {"000000011011", 1, 5}, {"000000010100", 2, 4}, {"000000011100", 3, 3}, {"000000010010", 4, 3},
    {"000000011110", 6, 2}, {"000000010101", 7, 2}, {"000000010001", 8, 2}, {"000000011111", 17, 1},
    {"000000011010", 18, 1}, {"000000011001", 19, 1}, {"000000010111", 20, 1}, {"000000010110", 21, 1},
    {"0000000011010", 0, 12}, {"0000000011001", 0, 13}, {"0000000011000", 0, 14}, {"0000000010111", 0, 15},
    {"0000000010110", 1, 6}, {"0000000010101", 1, 7}, {"0000000010100", 2, 5}, {"0000000010011", 3, 4},
    {"0000000010010", 5, 3}, {"0000000010001", 9, 2}, {"0000000010000", 10, 2}, {"0000000011111", 22, 1},
    {"0000000011110", 23, 1}, {"0000000011101", 24, 1}, {"0000000011100", 25, 1}, {"0000000011011", 26, 1},
    {"00000000011111", 0, 16}, {"00000000011110", 0, 17}, {"00000000011101", 0, 18},
    {"00000000011100", 0, 19}, {"00000000011011", 0, 20}, {"00000000011010", 0, 21},
    {"00000000011001", 0, 22}, {"00000000011000", 0, 23}, {"00000000010111", 0, 24},
    {"00000000010110", 0, 25}, {"00000000010101", 0, 26}, {"00000000010100", 0, 27},
    {"00000000010011", 0, 28}, {"00000000010010", 0, 29}, {"00000000010001", 0, 30},
    {"00000000010000", 0, 31},
    {"000000000011000", 0, 32}, {"000000000010111", 0, 33}, {"000000000010110", 0, 34},
    {"000000000010101", 0, 35}, {"000000000010100", 0, 36}, {"000000000010011", 0, 37},
    {"000000000010010", 0, 38}, {"000000000010001", 0, 39}, {"000000000010000", 0, 40},
    {"000000000011111", 1, 8}, {"000000000011110", 1, 9}, {"000000000011101", 1, 10},
    {"000000000011100", 1, 11}, {"000000000011011", 1, 12}, {"000000000011010", 1, 13},
    {"000000000011001", 1, 14},
    {"0000000000010011", 1, 15}, {"0000000000010010", 1, 16}, {"0000000000010001", 1, 17},
    {"0000000000010000", 1, 18}, {"0000000000010100", 6, 3}, {"0000000000011010", 11, 2},
    {"0000000000011001", 12, 2}, {"0000000000011000", 13, 2}, {"0000000000010111", 14, 2},
    {"0000000000010110", 15, 2}, {"0000000000010101", 16, 2}, {"0000000000011111", 27, 1},
    {"0000000000011110", 28, 1}, {"0000000000011101", 29, 1}, {"0000000000011100", 30, 1},
    {"0000000000011011", 31, 1},
};

/* lookup by the next 16 bits: (length << 16) | (run << 8) | level; 0 = invalid */
static unsigned *sAcTable;
static float sIdct[8][8];

static const unsigned char kZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22,
    15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* the MDEC's default quantisation tables (zig-zag order), set by DecDCTReset */
static const unsigned char kQuant[64] = {
    0x02, 0x10, 0x10, 0x13, 0x10, 0x13, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x1A, 0x18, 0x1A, 0x1B,
    0x1B, 0x1B, 0x1A, 0x1A, 0x1A, 0x1A, 0x1B, 0x1B, 0x1B, 0x1D, 0x1D, 0x1D, 0x22, 0x22, 0x22, 0x1D,
    0x1D, 0x1D, 0x1B, 0x1B, 0x1D, 0x1D, 0x20, 0x20, 0x22, 0x22, 0x25, 0x26, 0x25, 0x23, 0x23, 0x22,
    0x23, 0x26, 0x26, 0x28, 0x28, 0x28, 0x30, 0x30, 0x2E, 0x2E, 0x38, 0x38, 0x3A, 0x45, 0x45, 0x53,
};

typedef struct
{
    unsigned char iq_y[64];
    unsigned char iq_c[64];
    short dct[64];
} DecEnv;

static DecEnv sEnv;
static int sReady;

static void build_tables(void)
{
    unsigned i;
    int x, u;

    sAcTable = (unsigned *)port_alloc(65536 * sizeof(unsigned));
    for (i = 0; i < sizeof(kAc) / sizeof(kAc[0]); i++)
    {
        const char *c = kAc[i].code;
        unsigned len = 0, code = 0, fill, n;

        while (c[len])
        {
            code = (code << 1) | (unsigned)(c[len] - '0');
            len++;
        }
        fill = 16 - len;
        for (n = 0; n < (1u << fill); n++)
            sAcTable[(code << fill) | n] = (len << 16) | ((unsigned)kAc[i].run << 8) | kAc[i].level;
    }
    for (x = 0; x < 8; x++)
        for (u = 0; u < 8; u++)
            sIdct[x][u] = (float)((u == 0 ? sqrt(0.125) : 0.5) * cos((2 * x + 1) * u * 3.14159265358979 / 16.0));
    for (i = 0; i < 64; i++) sEnv.iq_y[i] = sEnv.iq_c[i] = kQuant[i];
    sReady = 1;
}

/* ---- DecDCTvlc: bitstream -> run-level codes ---------------------------------------------- */
static int read_dc_size(Bits *b, int luma)
{
    static const char *const kLuma[9] = {"100", "00", "01", "101", "110", "1110", "11110", "111110", "1111110"};
    static const char *const kChroma[9] = {"00", "01", "10", "110", "1110", "11110", "111110", "1111110",
                                          "11111110"};
    const char *const *t = luma ? kLuma : kChroma;
    int s;

    for (s = 0; s < 9; s++)
    {
        const char *c = t[s];
        int len = 0;
        unsigned code = 0;

        while (c[len])
        {
            code = (code << 1) | (unsigned)(c[len] - '0');
            len++;
        }
        if (peek(b, len) == code)
        {
            b->pos += len;
            return s;
        }
    }
    return -1;
}

static unsigned sVlcMax = 0xFFFFFFFFu; /* DecDCTvlcSize */

/* One block's codes; returns the halfwords written, 0 on a bitstream error. */
static int vlc_block(Bits *b, int version, int qscale, int *pred, int luma, unsigned short *out)
{
    int n = 0;

    if (version >= 3)
    {
        int size = read_dc_size(b, luma), diff = 0;

        if (size < 0) return 0;
        if (size > 0)
        {
            int v = (int)take(b, size);

            diff = (v & (1 << (size - 1))) ? v : v - (1 << size) + 1;
        }
        *pred += diff * 4;
        out[n++] = (unsigned short)((qscale << 10) | (*pred & 0x3FF));
    }
    else
    {
        out[n++] = (unsigned short)((qscale << 10) | take(b, 10));
    }
    for (;;)
    {
        unsigned bits = peek(b, 16), e;
        int run, level;

        if ((bits >> 14) == 2)
        {
            b->pos += 2;
            break;
        }
        if ((bits >> 10) == 1)
        {
            b->pos += 6;
            run = (int)take(b, 6);
            level = (int)take(b, 10);
        }
        else
        {
            e = sAcTable[bits];
            if (e == 0) return 0;
            b->pos += (int)(e >> 16);
            run = (int)((e >> 8) & 0xFF);
            level = (int)(e & 0xFF);
            if (take(b, 1)) level = -level;
        }
        if (n >= 63) return 0;
        out[n++] = (unsigned short)((run << 10) | (level & 0x3FF));
    }
    out[n++] = 0xFE00;
    return n;
}

/* The frame header: [0] halfwords of codes / 2, [1] 0x3800, [2] qscale, [3] version.
 * Output: one word (0x38000000 | words of codes), then the codes. Returns 0 (done). */
int DecDCTvlc(unsigned long *bs, unsigned long *buf)
{
    const unsigned char *h = (const unsigned char *)bs;
    unsigned short *out = (unsigned short *)(buf + 1);
    int qscale = h[4] | (h[5] << 8), version = h[6] | (h[7] << 8);
    int words = h[0] | (h[1] << 8);
    unsigned total = 0, limit = (unsigned)words * 2 + 64;
    int predY = 0, predCb = 0, predCr = 0;
    Bits b;

    if (!sReady) build_tables();
    b.data = h + 8;
    b.size = words * 4 + 4096; /* the codes never take more bits than the bitstream holds */
    b.pos = 0;
    /* macroblocks until the code count from the header is reached */
    while (total < (unsigned)words * 2 && total < limit && total < sVlcMax * 2)
    {
        int blk, n;

        for (blk = 0; blk < 6; blk++)
        {
            int *pred = blk == 0 ? &predCr : blk == 1 ? &predCb : &predY;

            n = vlc_block(&b, version, qscale, pred, blk >= 2, out + total);
            if (n == 0) goto done;
            total += (unsigned)n;
        }
    }
done:
    if (total & 1) out[total++] = 0xFE00;
    buf[0] = 0x38000000u | (total / 2);
    return 0;
}

int DecDCTvlc2(unsigned long *bs, unsigned long *buf, void *table)
{
    return DecDCTvlc(bs, buf);
}

void DecDCTvlcBuild(void *table) {}

int DecDCTvlcSize(int size)
{
    unsigned old = sVlcMax;

    sVlcMax = size > 0 ? (unsigned)size : 0xFFFFFFFFu;
    return (int)old;
}

int DecDCTvlcSize2(int size)
{
    return DecDCTvlcSize(size);
}

int DecDCTBufSize(unsigned long *bs)
{
    const unsigned char *h = (const unsigned char *)bs;

    return (h[0] | (h[1] << 8)) + 1;
}

/* ---- MDEC: codes -> pixels ---------------------------------------------------------------- */
static unsigned char *sPixels;   /* decoded macroblocks, output order */
static unsigned long sPixelCap, sPixelSize, sPixelPos;
static void (*sOutCallback)(void);
static void (*sInCallback)(void);
static int sDelivering;
static unsigned long *sOutBuf;
static long sOutWords;
static int sOutPending;

void DecDCTReset(int mode)
{
    if (!sReady) build_tables();
    if (mode == 0)
    {
        int i;

        for (i = 0; i < 64; i++) sEnv.iq_y[i] = sEnv.iq_c[i] = kQuant[i];
    }
    sPixelSize = sPixelPos = 0;
    sOutPending = 0;
}

void *DecDCTGetEnv(void *env)
{
    int i;

    for (i = 0; i < (int)sizeof sEnv; i++) ((unsigned char *)env)[i] = ((unsigned char *)&sEnv)[i];
    return env;
}

void *DecDCTPutEnv(void *env)
{
    int i;

    for (i = 0; i < (int)sizeof sEnv; i++) ((unsigned char *)&sEnv)[i] = ((unsigned char *)env)[i];
    return env;
}

static void idct_block(const int *coef, int nonzero, int *out)
{
    float tmp[64];
    int x, y, u, v;

    if (nonzero <= 1)
    {
        /* DC only: a flat block */
        float dc = coef[0] * sIdct[0][0] * sIdct[0][0];
        int value = (int)(dc >= 0 ? dc + 0.5f : dc - 0.5f);

        for (x = 0; x < 64; x++) out[x] = value;
        return;
    }
    for (y = 0; y < 8; y++)
        for (u = 0; u < 8; u++)
        {
            float s = 0;

            for (v = 0; v < 8; v++) s += sIdct[y][v] * coef[v * 8 + u];
            tmp[y * 8 + u] = s;
        }
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
        {
            float s = 0;

            for (u = 0; u < 8; u++) s += sIdct[x][u] * tmp[y * 8 + u];
            out[y * 8 + x] = (int)(s >= 0 ? s + 0.5f : s - 0.5f);
        }
}

/* Decodes one block's codes; returns the halfwords used, 0 at the end of the data. */
static int iq_block(const unsigned short *in, int avail, const unsigned char *iq, int *out)
{
    int coef[64], n = 0, k, used = 0, nonzero = 0, qscale;

    if (avail <= 0) return 0;
    for (k = 0; k < 64; k++) coef[k] = 0;
    /* skip padding before the DC */
    while (used < avail && in[used] == 0xFE00) used++;
    if (used >= avail) return 0;
    qscale = in[used] >> 10;
    {
        int dc = in[used] & 0x3FF;

        if (dc & 0x200) dc -= 0x400;
        coef[0] = dc * iq[0];
        nonzero = dc != 0;
    }
    used++;
    while (used < avail)
    {
        unsigned short c = in[used++];
        int level;

        if (c == 0xFE00) break;
        n += (c >> 10) + 1;
        if (n > 63) break;
        level = c & 0x3FF;
        if (level & 0x200) level -= 0x400;
        coef[kZigzag[n]] = (level * iq[n] * qscale + 4) / 8;
        nonzero++;
    }
    idct_block(coef, nonzero, out);
    return used;
}

static unsigned char clamp8(int v)
{
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* mode bit 0: 24-bit output, else 16-bit (Sony's samples pass 3 and 2) */
void DecDCTin(unsigned long *buf, int mode)
{
    const unsigned short *codes = (const unsigned short *)(buf + 1);
    int avail = (int)((buf[0] & 0xFFFF) * 2), pos = 0;
    int depth24 = mode & 1;
    unsigned long mbBytes = depth24 ? 16 * 16 * 3 : 16 * 16 * 2;

    if (!sReady) build_tables();
    /* port_alloc'd memory is never freed: one buffer for the largest frame the MDEC
     * makes in practice, 640 x 480 (1200 macroblocks), at the depth asked for */
    {
        unsigned long need = 1200ul * mbBytes;

        if (need > sPixelCap)
        {
            sPixels = (unsigned char *)port_alloc(need);
            sPixelCap = need;
        }
    }
    sPixelSize = sPixelPos = 0;
    for (;;)
    {
        int blocks[6][64], b, n, px, py;
        unsigned char *o;

        for (b = 0; b < 6; b++)
        {
            n = iq_block(codes + pos, avail - pos, b < 2 ? sEnv.iq_c : sEnv.iq_y, blocks[b]);
            if (n == 0) goto end;
            pos += n;
        }
        if (sPixelSize + mbBytes > sPixelCap) break;
        o = sPixels + sPixelSize;
        for (py = 0; py < 16; py++)
        {
            for (px = 0; px < 16; px++)
            {
                int yv = blocks[2 + (py >> 3) * 2 + (px >> 3)][(py & 7) * 8 + (px & 7)] + 128;
                int c = (py >> 1) * 8 + (px >> 1);
                int cr = blocks[0][c], cb = blocks[1][c];
                int r = clamp8(yv + ((1436 * cr) >> 10));
                int g = clamp8(yv - ((352 * cb + 731 * cr) >> 10));
                int bl = clamp8(yv + ((1815 * cb) >> 10));

                if (depth24)
                {
                    *o++ = (unsigned char)r;
                    *o++ = (unsigned char)g;
                    *o++ = (unsigned char)bl;
                }
                else
                {
                    unsigned short p = (unsigned short)((r >> 3) | ((g >> 3) << 5) | ((bl >> 3) << 10));

                    *o++ = (unsigned char)p;
                    *o++ = (unsigned char)(p >> 8);
                }
            }
        }
        sPixelSize += mbBytes;
    }
end:
    if (sInCallback) sInCallback();
}

static void deliver(void)
{
    while (sOutPending)
    {
        unsigned char *dst = (unsigned char *)sOutBuf;
        unsigned long bytes = (unsigned long)sOutWords * 4, i;

        sOutPending = 0;
        for (i = 0; i < bytes; i++) dst[i] = sPixelPos < sPixelSize ? sPixels[sPixelPos++] : 0;
        /* the callback may ask for the next piece: the loop delivers it */
        if (sOutCallback) sOutCallback();
    }
}

void DecDCTout(unsigned long *buf, int size)
{
    sOutBuf = buf;
    sOutWords = size;
    sOutPending = 1;
    if (sDelivering) return;
    sDelivering = 1;
    deliver();
    sDelivering = 0;
}

int DecDCTinSync(int mode)
{
    return 0;
}

int DecDCToutSync(int mode)
{
    return 0;
}

int DecDCTinCallback(void (*func)(void))
{
    sInCallback = func;
    return 0;
}

int DecDCToutCallback(void (*func)(void))
{
    sOutCallback = func;
    return 0;
}
