/*
 * FMV playback for the port: reads an STR file sector by sector from the disc,
 * decodes the MDEC (v2/v3) video frames and the XA ADPCM audio, and presents them
 * directly (bypassing the GPU), the way the game's MOV overlay does with
 * libcd streaming + DecDCT on the console.
 */
#include <port_host.h>
#include <ps1_game_config.h>

#define RAW_SECTOR 2352
#define MAX_W 640
#define MAX_H 480

extern double cos(double);
extern double sqrt(double);

/* ---- bitstream ------------------------------------------------------------------------- */
typedef struct
{
    const unsigned char *data;
    int size;   /* bytes */
    int pos;    /* bit position */
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
static unsigned sAcTable[65536];
static int sTablesReady;

static void build_tables(void)
{
    unsigned i;

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
        {
            sAcTable[(code << fill) | n] = (len << 16) | ((unsigned)kAc[i].run << 8) | kAc[i].level;
        }
    }
    sTablesReady = 1;
}

/* ---- MDEC ------------------------------------------------------------------------------ */
static const unsigned char kZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22,
    15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static const unsigned char kQuant[64] = {
    0x02, 0x10, 0x10, 0x13, 0x10, 0x13, 0x16, 0x16, 0x16, 0x16, 0x16, 0x16, 0x1A, 0x18, 0x1A, 0x1B,
    0x1B, 0x1B, 0x1A, 0x1A, 0x1A, 0x1A, 0x1B, 0x1B, 0x1B, 0x1D, 0x1D, 0x1D, 0x22, 0x22, 0x22, 0x1D,
    0x1D, 0x1D, 0x1B, 0x1B, 0x1D, 0x1D, 0x20, 0x20, 0x22, 0x22, 0x25, 0x26, 0x25, 0x23, 0x23, 0x22,
    0x23, 0x26, 0x26, 0x28, 0x28, 0x28, 0x30, 0x30, 0x2E, 0x2E, 0x38, 0x38, 0x3A, 0x45, 0x45, 0x53,
};

static double sIdct[8][8];

static void build_idct(void)
{
    int x, u;

    for (x = 0; x < 8; x++)
        for (u = 0; u < 8; u++)
            sIdct[x][u] = (u == 0 ? sqrt(0.125) : 0.5) * cos((2 * x + 1) * u * 3.14159265358979 / 16.0);
}

static void idct(const int *in, int *out)
{
    double tmp[64];
    int x, y, u, v;

    for (y = 0; y < 8; y++)
        for (u = 0; u < 8; u++)
        {
            double s = 0;

            for (v = 0; v < 8; v++) s += sIdct[y][v] * in[v * 8 + u];
            tmp[y * 8 + u] = s;
        }
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
        {
            double s = 0;

            for (u = 0; u < 8; u++) s += sIdct[x][u] * tmp[y * 8 + u];
            out[y * 8 + x] = (int)(s >= 0 ? s + 0.5 : s - 0.5);
        }
}

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

/* Decodes one 8x8 block; returns 0 on bitstream error. */
static int decode_block(Bits *b, int version, int qscale, int *pred, int luma, int *out)
{
    int coef[64];
    int n = 0, i;

    for (i = 0; i < 64; i++) coef[i] = 0;
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
        coef[0] = *pred * kQuant[0];
    }
    else
    {
        int dc = (int)take(b, 10);

        if (dc & 0x200) dc -= 0x400;
        coef[0] = dc * kQuant[0];
    }
    for (;;)
    {
        unsigned bits = peek(b, 16), e;
        int run, level;

        if ((bits >> 14) == 2)
        {
            b->pos += 2; /* end of block */
            break;
        }
        if ((bits >> 10) == 1)
        {
            b->pos += 6;
            run = (int)take(b, 6);
            level = (int)take(b, 10);
            if (level & 0x200) level -= 0x400;
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
        n += run + 1;
        if (n > 63) return 0;
        coef[kZigzag[n]] = (level * kQuant[n] * qscale + 4) / 8;
    }
    idct(coef, out);
    return 1;
}

static unsigned char clamp8(int v)
{
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Decodes a frame into rgba (width x height). Returns 0 on error. */
static int decode_frame(const unsigned char *frame, int size, int width, int height, unsigned char *rgba)
{
    Bits b;
    int qscale = frame[4] | (frame[5] << 8);
    int version = frame[6] | (frame[7] << 8);
    int mbw = (width + 15) / 16, mbh = (height + 15) / 16, mx, my;
    int predY = 0, predCb = 0, predCr = 0;

    b.data = frame + 8;
    b.size = size - 8;
    b.pos = 0;
    for (mx = 0; mx < mbw; mx++)
    {
        for (my = 0; my < mbh; my++)
        {
            int cr[64], cb[64], y[4][64], i, px, py;

            if (!decode_block(&b, version, qscale, &predCr, 0, cr) || !decode_block(&b, version, qscale, &predCb, 0, cb))
                return 0;
            for (i = 0; i < 4; i++)
                if (!decode_block(&b, version, qscale, &predY, 1, y[i])) return 0;
            for (py = 0; py < 16; py++)
            {
                for (px = 0; px < 16; px++)
                {
                    int X = mx * 16 + px, Y = my * 16 + py;
                    int yv = y[(py >> 3) * 2 + (px >> 3)][(py & 7) * 8 + (px & 7)] + 128;
                    int c = (py >> 1) * 8 + (px >> 1);
                    double r = yv + 1.402 * cr[c];
                    double g = yv - 0.3437 * cb[c] - 0.7143 * cr[c];
                    double bl = yv + 1.772 * cb[c];
                    unsigned char *o;

                    if (X >= width || Y >= height) continue;
                    o = rgba + (Y * width + X) * 4;
                    o[0] = clamp8((int)r);
                    o[1] = clamp8((int)g);
                    o[2] = clamp8((int)bl);
                    o[3] = 255;
                }
            }
        }
    }
    return 1;
}

/* ---- XA audio --------------------------------------------------------------------------- */
typedef struct
{
    int s1[2], s2[2];
    double phase;
    short last[2];
} XaState;

static const int kXaF0[4] = {0, 60, 115, 98};
static const int kXaF1[4] = {0, 0, -52, -55};

/* Decodes one 2304-byte XA form-2 payload (4-bit) and pushes it resampled to 44100 Hz. */
#define XA_OUT_FRAMES 9472

static void decode_xa(XaState *st, const unsigned char *data, int stereo, int rate)
{
    static short pcm[18 * 8 * 28];
    /* one sector resampled to 44100 Hz: up to 4032 mono samples at 18900 Hz = 9408 frames */
    static short out[XA_OUT_FRAMES * 2];
    int count = 0, g, u, s, outFrames = 0;
    double step = (double)rate / 44100.0;

    for (g = 0; g < 18; g++)
    {
        const unsigned char *grp = data + g * 128;

        for (u = 0; u < 8; u++)
        {
            int param = grp[4 + u], shift = param & 15, filter = (param >> 4) & 3;
            int ch = stereo ? (u & 1) : 0;

            if (shift > 12) shift = 9;
            for (s = 0; s < 28; s++)
            {
                int nib = (grp[16 + s * 4 + u / 2] >> ((u & 1) * 4)) & 15;
                int v = (short)(nib << 12) >> shift;

                v += (st->s1[ch] * kXaF0[filter] + st->s2[ch] * kXaF1[filter] + 32) >> 6;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                st->s2[ch] = st->s1[ch];
                st->s1[ch] = v;
                if (stereo)
                    pcm[((g * 4 + u / 2) * 28 + s) * 2 + ch] = (short)v;
                else
                    pcm[(g * 8 + u) * 28 + s] = (short)v;
            }
        }
    }
    count = stereo ? 18 * 4 * 28 : 18 * 8 * 28;
    /* resample by nearest-with-hold to 44100 Hz */
    while (st->phase < count && outFrames < XA_OUT_FRAMES)
    {
        int i = (int)st->phase;

        if (stereo)
        {
            out[outFrames * 2] = pcm[i * 2];
            out[outFrames * 2 + 1] = pcm[i * 2 + 1];
        }
        else
        {
            out[outFrames * 2] = out[outFrames * 2 + 1] = pcm[i];
        }
        outFrames++;
        st->phase += step;
    }
    st->phase -= count;
    port_audio_push(out, outFrames);
}

/* ---- player ----------------------------------------------------------------------------- */
unsigned long PadRead(int id);

/* STR files by movie id, from the game package (ps1_add_game MOVIES) */
static const char *const kMovies[] = PS1_MOVIES;

#if PORT_HOST_GPU
/* the GPU runs in the host (wasm2c build): its buffers are not in guest memory */
static unsigned char sMovieRgba[MAX_W * MAX_H * 4];
#else
unsigned char *gpu_present_buffer(unsigned long *size);
#endif

void port_play_movie(int movieId)
{
    static unsigned char sector[RAW_SECTOR];
    static unsigned char frame[128 * 1024];
#if PORT_HOST_GPU
    unsigned char *rgba = sMovieRgba;
#else
    unsigned long rgba_size;
    unsigned char *rgba = gpu_present_buffer(&rgba_size);
#endif
    unsigned lba, size, i, sectors;
    int curFrame = -1, framePos = 0, width = 0, height = 0;
    XaState xa;
    int presented = 0, dropped = 0;
    unsigned start;
    /* mod "holdskip=N": holding Cross for N seconds ends any movie, even one the game
     * does not let Start skip */
    int holdSkip = 0, holding = 0;
    unsigned holdFrom = 0;

    {
        int skip[1];

        if (port_debug_values("nomovie", skip, 1) == 1 && skip[0]) return;
        if (port_debug_values("holdskip", skip, 1) == 1 && skip[0] > 0) holdSkip = skip[0] * 60;
    }
    if (movieId < 0 || movieId >= PS1_MOVIE_COUNT || !port_disc_find(kMovies[movieId], &lba, &size)) return;
    if (!sTablesReady)
    {
        build_tables();
        build_idct();
    }
    for (i = 0; i < sizeof(xa); i++) ((char *)&xa)[i] = 0;
    sectors = (size + 2047) / 2048;
    port_log("movie %d: %s (%u sectors)", movieId, kMovies[movieId], sectors);
    start = port_vblank_count();
    for (i = 0; i < sectors; i++)
    {
        const unsigned char *sub, *data;

        if (!port_disc_read_raw(lba + i, sector)) break;
        sub = sector + 16;
        data = sector + 24;
        if (sub[2] & 4)
        {
            /* XA audio: channel 1 is the movie's soundtrack */
            if ((sub[3] & 0x30) == 0) decode_xa(&xa, data, sub[3] & 1, (sub[3] & 4) ? 18900 : 37800);
            continue;
        }
        if ((data[0] | (data[1] << 8)) != 0x0160) continue;
        {
            int chunk = data[4] | (data[5] << 8), chunks = data[6] | (data[7] << 8);
            int number = data[8] | (data[9] << 8) | (data[10] << 16) | (data[11] << 24);
            int bytes = data[12] | (data[13] << 8) | (data[14] << 16) | (data[15] << 24);

            if (number != curFrame)
            {
                curFrame = number;
                framePos = 0;
            }
            width = data[16] | (data[17] << 8);
            height = data[18] | (data[19] << 8);
            if (chunk * 2016 + 2016 <= (int)sizeof(frame))
            {
                int k;

                for (k = 0; k < 2016; k++) frame[chunk * 2016 + k] = data[32 + k];
                framePos++;
            }
            if (chunk == chunks - 1 && framePos == chunks && width <= MAX_W && height <= MAX_H)
            {
                /* 30 frames per second, two video fields per frame, on a fixed clock: frame n
                 * is due at vblank start + 2n. The audio is pushed as its sectors are read, so
                 * the video must keep that rate even when a frame decodes slowly (slow hosts):
                 * a frame more than one behind is not decoded (STR frames are independent). */
                const unsigned due = start + (unsigned)presented * 2;

                if ((int)(port_vblank_count() - due) <= 2)
                {
                    int pad = (240 - height) / 2, y;

                    for (y = 0; y < MAX_W * 240 * 4; y++) rgba[y] = 0;
                    for (y = 0; y < 320 * 240; y++) rgba[y * 4 + 3] = 255;
                    if (decode_frame(frame, bytes, width, height, rgba + (pad > 0 ? pad : 0) * width * 4))
                    {
                        port_present(rgba, width, 240);
                    }
                }
                else
                {
                    dropped++;
                }
                while ((int)(port_vblank_count() - (due + 2)) < 0)
                {
                    port_wait_vblank();
                }
                presented++;
                if (!(PS1_MOVIE_NOSKIP_MASK & (1u << movieId)) && presented > 30 && (PadRead(1) & 0x800)) break;
                if (holdSkip)
                {
                    if (!(PadRead(1) & 0x40)) holding = 0;
                    else if (!holding) holding = 1, holdFrom = port_vblank_count();
                    else if ((int)(port_vblank_count() - holdFrom) >= holdSkip)
                    {
                        port_log("movie %d: skipped (Cross held)", movieId);
                        break;
                    }
                }
            }
        }
    }
    port_log("movie %d: %d frames (%d not drawn to keep up)", movieId, presented, dropped);
}
