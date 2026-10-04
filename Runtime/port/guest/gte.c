/*
 * Software GTE (PlayStation COP2), modelled on the hardware behaviour documented
 * in psx-spx: fixed-point maths, saturation flags, the UNR division used by
 * perspective transformation, and the SXY/SZ/RGB FIFOs.
 */
#include <port_gte.h>

typedef long long s64;
typedef long s32;
typedef unsigned long u32;
typedef short s16;
typedef unsigned short u16;
typedef unsigned char u8;

long gte_data[32];
long gte_ctrl[32];

#define FLAG gte_ctrl[31]

/* ---- register views ------------------------------------------------------- */
#define VX(n) ((s16)(gte_data[(n) * 2] & 0xFFFF))
#define VY(n) ((s16)(gte_data[(n) * 2] >> 16))
#define VZ(n) ((s16)(gte_data[(n) * 2 + 1] & 0xFFFF))

#define IR0 gte_data[8]
#define IR1 gte_data[9]
#define IR2 gte_data[10]
#define IR3 gte_data[11]
#define MAC0 gte_data[24]
#define MAC1 gte_data[25]
#define MAC2 gte_data[26]
#define MAC3 gte_data[27]

#define TRX gte_ctrl[5]
#define TRY gte_ctrl[6]
#define TRZ gte_ctrl[7]
#define RBK gte_ctrl[13]
#define GBK gte_ctrl[14]
#define BBK gte_ctrl[15]
#define RFC gte_ctrl[21]
#define GFC gte_ctrl[22]
#define BFC gte_ctrl[23]
#define OFX gte_ctrl[24]
#define OFY gte_ctrl[25]
#define H ((u16)gte_ctrl[26])
#define DQA ((s16)gte_ctrl[27])
#define DQB gte_ctrl[28]
#define ZSF3 ((s16)gte_ctrl[29])
#define ZSF4 ((s16)gte_ctrl[30])

/* matrix element m (0=rotation, 1=light, 2=colour), row r, column c */
static s16 mat(int m, int r, int c)
{
    int index = r * 3 + c;
    u32 word = (u32)gte_ctrl[m * 8 + index / 2];

    return (s16)((index & 1) ? (word >> 16) : (word & 0xFFFF));
}

/* ---- flags and saturation -------------------------------------------------- */
static s64 mac_check(int n, s64 value)
{
    static const u32 pos[4] = { 1u << 16, 1u << 30, 1u << 29, 1u << 28 };
    static const u32 neg[4] = { 1u << 15, 1u << 27, 1u << 26, 1u << 25 };

    if (n == 0)
    {
        if (value > 0x7FFFFFFFLL) FLAG |= pos[0];
        else if (value < -0x80000000LL) FLAG |= neg[0];
        return value;
    }
    if (value > 0x7FFFFFFFFFFLL) FLAG |= pos[n];
    else if (value < -0x80000000000LL) FLAG |= neg[n];
    /* sign-extend to 44 bits like the hardware accumulators */
    return (value << 20) >> 20;
}

static s32 ir_sat(int n, s64 value, int lm)
{
    static const u32 bits[4] = { 1u << 12, 1u << 24, 1u << 23, 1u << 22 };
    s64 lo = (n == 0) ? 0 : (lm ? 0 : -0x8000);
    s64 hi = (n == 0) ? 0x1000 : 0x7FFF;

    if (value < lo) { FLAG |= bits[n]; return (s32)lo; }
    if (value > hi) { FLAG |= bits[n]; return (s32)hi; }
    return (s32)value;
}

/* IR3 saturation flag in RTPS/RTPT depends on the unshifted value. */
static s32 ir3_rtp(s64 mac3, int sf, int lm)
{
    s32 shifted = (s32)(mac3 >> (sf ? 12 : 0));
    s32 lo = lm ? 0 : -0x8000;
    s64 test = mac3 >> 12;

    if (test < -0x8000 || test > 0x7FFF) FLAG |= 1u << 22;
    if (shifted < lo) return lo;
    if (shifted > 0x7FFF) return 0x7FFF;
    return shifted;
}

static u8 color_sat(int n, s32 value)
{
    static const u32 bits[3] = { 1u << 21, 1u << 20, 1u << 19 };

    if (value < 0) { FLAG |= bits[n]; return 0; }
    if (value > 0xFF) { FLAG |= bits[n]; return 0xFF; }
    return (u8)value;
}

static void push_sz(s64 value)
{
    if (value < 0) { FLAG |= 1u << 18; value = 0; }
    else if (value > 0xFFFF) { FLAG |= 1u << 18; value = 0xFFFF; }
    gte_data[16] = gte_data[17];
    gte_data[17] = gte_data[18];
    gte_data[18] = gte_data[19];
    gte_data[19] = (s32)value;
}

static void push_sxy(s64 x, s64 y)
{
    if (x < -0x400) { FLAG |= 1u << 14; x = -0x400; } else if (x > 0x3FF) { FLAG |= 1u << 14; x = 0x3FF; }
    if (y < -0x400) { FLAG |= 1u << 13; y = -0x400; } else if (y > 0x3FF) { FLAG |= 1u << 13; y = 0x3FF; }
    gte_data[12] = gte_data[13];
    gte_data[13] = gte_data[14];
    gte_data[14] = (s32)((x & 0xFFFF) | ((y & 0xFFFF) << 16));
}

static void push_rgb(s32 r, s32 g, s32 b)
{
    u32 code = (u32)gte_data[6] & 0xFF000000u;

    gte_data[20] = gte_data[21];
    gte_data[21] = gte_data[22];
    gte_data[22] = (s32)(code | ((u32)color_sat(2, b) << 16) | ((u32)color_sat(1, g) << 8) | color_sat(0, r));
}

static void finish_flag(void)
{
    if (FLAG & 0x7F87E000) FLAG |= 0x80000000;
    else FLAG &= 0x7FFFFFFF;
}

/* ---- UNR division (H / SZ3) ------------------------------------------------ */
static u8 sUnrTable[0x101];
static int sUnrReady;

static void unr_init(void)
{
    int i;

    for (i = 0; i < 0x100; i++)
    {
        int v = ((0x40000 / (i + 0x100) + 1) / 2) - 0x101;
        sUnrTable[i] = (u8)(v < 0 ? 0 : v);
    }
    sUnrTable[0x100] = 0;
    sUnrReady = 1;
}

static u32 gte_divide(u16 h, u16 sz3)
{
    u32 n, d, u;
    int z;

    if (h >= sz3 * 2)
    {
        FLAG |= 1u << 17;
        return 0x1FFFF;
    }
    if (!sUnrReady) unr_init();
    z = 0;
    while (z < 16 && !(sz3 & (0x8000 >> z))) z++;
    n = (u32)h << z;
    d = (u32)sz3 << z;
    u = sUnrTable[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - (d * u)) >> 8;
    d = (0x0000080 + (d * u)) >> 8;
    n = (u32)(((unsigned long long)n * d + 0x8000) >> 16);
    if (n > 0x1FFFF) n = 0x1FFFF;
    return n;
}

/* ---- commands ----------------------------------------------------------------- */
static void rtp(int v, int sf, int lm, int last)
{
    int shift = sf ? 12 : 0;
    s64 vx = VX(v), vy = VY(v), vz = VZ(v);
    s64 m1, m2, m3, mac0;
    u32 q;

    m1 = mac_check(1, ((s64)TRX << 12) + mat(0, 0, 0) * vx + mat(0, 0, 1) * vy + mat(0, 0, 2) * vz);
    m2 = mac_check(2, ((s64)TRY << 12) + mat(0, 1, 0) * vx + mat(0, 1, 1) * vy + mat(0, 1, 2) * vz);
    m3 = mac_check(3, ((s64)TRZ << 12) + mat(0, 2, 0) * vx + mat(0, 2, 1) * vy + mat(0, 2, 2) * vz);
    MAC1 = (s32)(m1 >> shift);
    MAC2 = (s32)(m2 >> shift);
    MAC3 = (s32)(m3 >> shift);
    IR1 = ir_sat(1, MAC1, lm);
    IR2 = ir_sat(2, MAC2, lm);
    IR3 = ir3_rtp(m3, sf, lm);
    push_sz(m3 >> 12);
    q = gte_divide(H, (u16)gte_data[19]);
    push_sxy(mac_check(0, (s64)q * (s16)IR1 + OFX) >> 16, mac_check(0, (s64)q * (s16)IR2 + OFY) >> 16);
    if (last)
    {
        mac0 = mac_check(0, (s64)q * DQA + DQB);
        MAC0 = (s32)mac0;
        IR0 = ir_sat(0, mac0 >> 12, 1);
    }
}

/* MVMVA-style product: result = tv + m * v (all 3 rows) */
static void mat_vec(int m, s64 vx, s64 vy, s64 vz, s64 tx, s64 ty, s64 tz, int sf, int lm)
{
    int shift = sf ? 12 : 0;

    MAC1 = (s32)(mac_check(1, (tx << 12) + mat(m, 0, 0) * vx + mat(m, 0, 1) * vy + mat(m, 0, 2) * vz) >> shift);
    MAC2 = (s32)(mac_check(2, (ty << 12) + mat(m, 1, 0) * vx + mat(m, 1, 1) * vy + mat(m, 1, 2) * vz) >> shift);
    MAC3 = (s32)(mac_check(3, (tz << 12) + mat(m, 2, 0) * vx + mat(m, 2, 1) * vy + mat(m, 2, 2) * vz) >> shift);
    IR1 = ir_sat(1, MAC1, lm);
    IR2 = ir_sat(2, MAC2, lm);
    IR3 = ir_sat(3, MAC3, lm);
}

/* Colour stage shared by NC*, CC, CDP: IR = LR * IR + BK, then optional depth cue. */
static void colour_from_ir(int sf, int lm, int depth_cue, int use_rgbc)
{
    s64 r = (u8)(gte_data[6] & 0xFF), g = (u8)((gte_data[6] >> 8) & 0xFF), b = (u8)((gte_data[6] >> 16) & 0xFF);
    int shift = sf ? 12 : 0;

    mat_vec(2, (s16)IR1, (s16)IR2, (s16)IR3, RBK, GBK, BBK, sf, lm);
    if (use_rgbc)
    {
        s64 m1 = (r * (s16)IR1) << 4, m2 = (g * (s16)IR2) << 4, m3 = (b * (s16)IR3) << 4;

        if (depth_cue)
        {
            s64 f1 = mac_check(1, ((s64)RFC << 12) - m1) >> shift;
            s64 f2 = mac_check(2, ((s64)GFC << 12) - m2) >> shift;
            s64 f3 = mac_check(3, ((s64)BFC << 12) - m3) >> shift;

            m1 += (s64)ir_sat(1, f1, 0) * (s16)IR0;
            m2 += (s64)ir_sat(2, f2, 0) * (s16)IR0;
            m3 += (s64)ir_sat(3, f3, 0) * (s16)IR0;
        }
        MAC1 = (s32)(mac_check(1, m1) >> shift);
        MAC2 = (s32)(mac_check(2, m2) >> shift);
        MAC3 = (s32)(mac_check(3, m3) >> shift);
        IR1 = ir_sat(1, MAC1, lm);
        IR2 = ir_sat(2, MAC2, lm);
        IR3 = ir_sat(3, MAC3, lm);
    }
    push_rgb(MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

static void normal_colour(int v, int sf, int lm, int colour, int depth_cue)
{
    mat_vec(1, VX(v), VY(v), VZ(v), 0, 0, 0, sf, lm);
    colour_from_ir(sf, lm, depth_cue, colour);
}

static void depth_cue_colour(s64 r, s64 g, s64 b, int sf, int lm)
{
    int shift = sf ? 12 : 0;
    s64 f1 = mac_check(1, ((s64)RFC << 12) - r) >> shift;
    s64 f2 = mac_check(2, ((s64)GFC << 12) - g) >> shift;
    s64 f3 = mac_check(3, ((s64)BFC << 12) - b) >> shift;

    MAC1 = (s32)(mac_check(1, r + (s64)ir_sat(1, f1, 0) * (s16)IR0) >> shift);
    MAC2 = (s32)(mac_check(2, g + (s64)ir_sat(2, f2, 0) * (s16)IR0) >> shift);
    MAC3 = (s32)(mac_check(3, b + (s64)ir_sat(3, f3, 0) * (s16)IR0) >> shift);
    IR1 = ir_sat(1, MAC1, lm);
    IR2 = ir_sat(2, MAC2, lm);
    IR3 = ir_sat(3, MAC3, lm);
    push_rgb(MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
}

void port_gte_op(unsigned long command)
{
    int sf = (command >> 19) & 1;
    int lm = (command >> 10) & 1;
    int shift = sf ? 12 : 0;
    int i;

    FLAG = 0;
    switch (command & 0x3F)
    {
    case 0x01: /* RTPS */
        rtp(0, sf, lm, 1);
        break;
    case 0x30: /* RTPT */
        rtp(0, sf, lm, 0);
        rtp(1, sf, lm, 0);
        rtp(2, sf, lm, 1);
        break;
    case 0x06: /* NCLIP */
    {
        s64 x0 = (s16)gte_data[12], y0 = (s16)(gte_data[12] >> 16);
        s64 x1 = (s16)gte_data[13], y1 = (s16)(gte_data[13] >> 16);
        s64 x2 = (s16)gte_data[14], y2 = (s16)(gte_data[14] >> 16);

        MAC0 = (s32)mac_check(0, x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1);
        break;
    }
    case 0x2D: /* AVSZ3 */
    {
        s64 v = (s64)ZSF3 * ((u16)gte_data[17] + (u16)gte_data[18] + (u16)gte_data[19]);

        MAC0 = (s32)mac_check(0, v);
        v >>= 12;
        if (v < 0) { FLAG |= 1u << 18; v = 0; } else if (v > 0xFFFF) { FLAG |= 1u << 18; v = 0xFFFF; }
        gte_data[7] = (s32)v;
        break;
    }
    case 0x2E: /* AVSZ4 */
    {
        s64 v = (s64)ZSF4 * ((u16)gte_data[16] + (u16)gte_data[17] + (u16)gte_data[18] + (u16)gte_data[19]);

        MAC0 = (s32)mac_check(0, v);
        v >>= 12;
        if (v < 0) { FLAG |= 1u << 18; v = 0; } else if (v > 0xFFFF) { FLAG |= 1u << 18; v = 0xFFFF; }
        gte_data[7] = (s32)v;
        break;
    }
    case 0x12: /* MVMVA */
    {
        int mx = (command >> 17) & 3, vsel = (command >> 15) & 3, cv = (command >> 13) & 3;
        s64 vx, vy, vz, tx = 0, ty = 0, tz = 0;

        if (vsel == 3) { vx = (s16)IR1; vy = (s16)IR2; vz = (s16)IR3; }
        else { vx = VX(vsel); vy = VY(vsel); vz = VZ(vsel); }
        if (cv == 0) { tx = TRX; ty = TRY; tz = TRZ; }
        else if (cv == 1) { tx = RBK; ty = GBK; tz = BBK; }
        else if (cv == 2) { tx = RFC; ty = GFC; tz = BFC; }
        if (mx == 3)
        {
            /* "garbage" matrix; rarely used, approximate with zero */
            MAC1 = MAC2 = MAC3 = 0;
            IR1 = IR2 = IR3 = 0;
            break;
        }
        mat_vec(mx, vx, vy, vz, tx, ty, tz, sf, lm);
        break;
    }
    case 0x28: /* SQR */
        MAC1 = (s32)(mac_check(1, (s64)(s16)IR1 * (s16)IR1) >> shift);
        MAC2 = (s32)(mac_check(2, (s64)(s16)IR2 * (s16)IR2) >> shift);
        MAC3 = (s32)(mac_check(3, (s64)(s16)IR3 * (s16)IR3) >> shift);
        IR1 = ir_sat(1, MAC1, lm);
        IR2 = ir_sat(2, MAC2, lm);
        IR3 = ir_sat(3, MAC3, lm);
        break;
    case 0x0C: /* OP: outer product of the rotation diagonal with IR */
    {
        s64 d1 = mat(0, 0, 0), d2 = mat(0, 1, 1), d3 = mat(0, 2, 2);

        MAC1 = (s32)(mac_check(1, d2 * (s16)IR3 - d3 * (s16)IR2) >> shift);
        MAC2 = (s32)(mac_check(2, d3 * (s16)IR1 - d1 * (s16)IR3) >> shift);
        MAC3 = (s32)(mac_check(3, d1 * (s16)IR2 - d2 * (s16)IR1) >> shift);
        IR1 = ir_sat(1, MAC1, lm);
        IR2 = ir_sat(2, MAC2, lm);
        IR3 = ir_sat(3, MAC3, lm);
        break;
    }
    case 0x13: normal_colour(0, sf, lm, 1, 1); break;                                     /* NCDS */
    case 0x16: for (i = 0; i < 3; i++) normal_colour(i, sf, lm, 1, 1); break;             /* NCDT */
    case 0x1B: normal_colour(0, sf, lm, 1, 0); break;                                     /* NCCS */
    case 0x3F: for (i = 0; i < 3; i++) normal_colour(i, sf, lm, 1, 0); break;             /* NCCT */
    case 0x1E: normal_colour(0, sf, lm, 0, 0); break;                                     /* NCS */
    case 0x20: for (i = 0; i < 3; i++) normal_colour(i, sf, lm, 0, 0); break;             /* NCT */
    case 0x1C: colour_from_ir(sf, lm, 0, 1); break;                                       /* CC */
    case 0x14: colour_from_ir(sf, lm, 1, 1); break;                                       /* CDP */
    case 0x10: /* DPCS */
    {
        s64 r = (s64)(u8)(gte_data[6] & 0xFF) << 16, g = (s64)(u8)(gte_data[6] >> 8) << 16, b = (s64)(u8)(gte_data[6] >> 16) << 16;

        depth_cue_colour(r, g, b, sf, lm);
        break;
    }
    case 0x2A: /* DPCT: three times on the RGB FIFO head */
        for (i = 0; i < 3; i++)
        {
            u32 c = (u32)gte_data[20];

            depth_cue_colour((s64)(c & 0xFF) << 16, (s64)((c >> 8) & 0xFF) << 16, (s64)((c >> 16) & 0xFF) << 16, sf, lm);
        }
        break;
    case 0x29: /* DCPL */
    {
        s64 r = (u8)(gte_data[6] & 0xFF), g = (u8)(gte_data[6] >> 8), b = (u8)(gte_data[6] >> 16);

        depth_cue_colour((r * (s16)IR1) << 4, (g * (s16)IR2) << 4, (b * (s16)IR3) << 4, sf, lm);
        break;
    }
    case 0x11: /* INTPL */
        depth_cue_colour((s64)(s16)IR1 << 12, (s64)(s16)IR2 << 12, (s64)(s16)IR3 << 12, sf, lm);
        break;
    case 0x3D: /* GPF */
        MAC1 = (s32)(mac_check(1, (s64)(s16)IR0 * (s16)IR1) >> shift);
        MAC2 = (s32)(mac_check(2, (s64)(s16)IR0 * (s16)IR2) >> shift);
        MAC3 = (s32)(mac_check(3, (s64)(s16)IR0 * (s16)IR3) >> shift);
        IR1 = ir_sat(1, MAC1, lm);
        IR2 = ir_sat(2, MAC2, lm);
        IR3 = ir_sat(3, MAC3, lm);
        push_rgb(MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
        break;
    case 0x3E: /* GPL */
        MAC1 = (s32)(mac_check(1, ((s64)MAC1 << shift) + (s64)(s16)IR0 * (s16)IR1) >> shift);
        MAC2 = (s32)(mac_check(2, ((s64)MAC2 << shift) + (s64)(s16)IR0 * (s16)IR2) >> shift);
        MAC3 = (s32)(mac_check(3, ((s64)MAC3 << shift) + (s64)(s16)IR0 * (s16)IR3) >> shift);
        IR1 = ir_sat(1, MAC1, lm);
        IR2 = ir_sat(2, MAC2, lm);
        IR3 = ir_sat(3, MAC3, lm);
        push_rgb(MAC1 >> 4, MAC2 >> 4, MAC3 >> 4);
        break;
    default:
        break;
    }
    finish_flag();
}

/* ---- register access with hardware side effects ----------------------------------- */
long port_gte_read_data(int reg)
{
    switch (reg)
    {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11:
        return (s16)gte_data[reg];
    case 7: case 16: case 17: case 18: case 19:
        return (u16)gte_data[reg];
    case 15:
        return gte_data[14];
    case 28: case 29:
    {
        s32 r = (s16)IR1 >> 7, g = (s16)IR2 >> 7, b = (s16)IR3 >> 7;

        r = r < 0 ? 0 : r > 0x1F ? 0x1F : r;
        g = g < 0 ? 0 : g > 0x1F ? 0x1F : g;
        b = b < 0 ? 0 : b > 0x1F ? 0x1F : b;
        return r | (g << 5) | (b << 10);
    }
    case 31:
    {
        u32 v = (u32)gte_data[30];
        int n = 0;

        if ((s32)v < 0) v = ~v;
        while (n < 32 && !(v & 0x80000000u)) { v <<= 1; n++; }
        return n;
    }
    default:
        return gte_data[reg];
    }
}

void port_gte_write_data(int reg, long value)
{
    switch (reg)
    {
    case 15: /* SXYP pushes the FIFO */
        gte_data[12] = gte_data[13];
        gte_data[13] = gte_data[14];
        gte_data[14] = value;
        break;
    case 28: /* IRGB */
        gte_data[28] = value & 0x7FFF;
        IR1 = (value & 0x1F) << 7;
        IR2 = ((value >> 5) & 0x1F) << 7;
        IR3 = ((value >> 10) & 0x1F) << 7;
        break;
    case 29: case 31:
        break;
    default:
        gte_data[reg] = value;
        break;
    }
}

long port_gte_read_ctrl(int reg)
{
    switch (reg)
    {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30:
        return (s16)gte_ctrl[reg];
    default:
        return gte_ctrl[reg];
    }
}

void port_gte_write_ctrl(int reg, long value)
{
    gte_ctrl[reg] = (reg == 31) ? (value & 0x7FFFF000) : value;
    if (reg == 31) finish_flag();
}
