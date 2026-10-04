/* PsyQ libgte replacement: matrix/vector maths and GTE wrappers. */
#include <sys/types.h>
#include <libgte.h>

#include <inline_n.h>
#include <port_gte.h>

extern double sin(double);
extern double cos(double);
extern double atan2(double, double);
extern double sqrt(double);

#define PI2 6.283185307179586

static short sat16(long long v)
{
    return (short)(v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v);
}

int rsin(int a)
{
    a &= 4095;
    return (int)(sin(a * PI2 / 4096.0) * 4096.0 + (sin(a * PI2 / 4096.0) >= 0 ? 0.5 : -0.5));
}

int rcos(int a)
{
    return rsin(a + 1024);
}

long ratan2(long y, long x)
{
    double r;

    if (x == 0 && y == 0)
    {
        return 0;
    }
    r = atan2((double)y, (double)x) * 4096.0 / PI2;
    return (long)(r + (r >= 0 ? 0.5 : -0.5));
}

long SquareRoot0(long a)
{
    unsigned long r;

    if (a <= 0)
    {
        return 0;
    }
    r = (unsigned long)sqrt((double)a);
    while (r * r > (unsigned long)a) r--;
    while ((r + 1) * (r + 1) <= (unsigned long)a) r++;
    return (long)r;
}

/* ---- matrices ------------------------------------------------------------------------------ */
static void mul3(short out[3][3], short a[3][3], short b[3][3])
{
    short tmp[3][3];
    int i, j;

    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
        {
            tmp[i][j] = sat16(((long long)a[i][0] * b[0][j] + (long long)a[i][1] * b[1][j] + (long long)a[i][2] * b[2][j]) >> 12);
        }
    }
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            out[i][j] = tmp[i][j];
}

MATRIX *MulMatrix0(MATRIX *m0, MATRIX *m1, MATRIX *m2)
{
    mul3(m2->m, m0->m, m1->m);
    return m2;
}

MATRIX *MulMatrix(MATRIX *m0, MATRIX *m1)
{
    mul3(m0->m, m0->m, m1->m);
    return m0;
}

MATRIX *MulMatrix2(MATRIX *m0, MATRIX *m1)
{
    mul3(m1->m, m0->m, m1->m);
    return m1;
}

MATRIX *CompMatrix(MATRIX *m0, MATRIX *m1, MATRIX *m2)
{
    long t[3];
    int i;

    for (i = 0; i < 3; i++)
    {
        t[i] = (long)((((long long)m0->m[i][0] * m1->t[0] + (long long)m0->m[i][1] * m1->t[1] +
                        (long long)m0->m[i][2] * m1->t[2]) >> 12) + m0->t[i]);
    }
    mul3(m2->m, m0->m, m1->m);
    for (i = 0; i < 3; i++) m2->t[i] = t[i];
    return m2;
}

MATRIX *TransposeMatrix(MATRIX *m0, MATRIX *m1)
{
    short t[3][3];
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            t[i][j] = m0->m[j][i];
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            m1->m[i][j] = t[i][j];
    return m1;
}

MATRIX *TransMatrix(MATRIX *m, VECTOR *v)
{
    m->t[0] = v->vx;
    m->t[1] = v->vy;
    m->t[2] = v->vz;
    return m;
}

MATRIX *ScaleMatrix(MATRIX *m, VECTOR *v)
{
    int i;

    for (i = 0; i < 3; i++)
    {
        m->m[i][0] = sat16(((long long)m->m[i][0] * v->vx) >> 12);
        m->m[i][1] = sat16(((long long)m->m[i][1] * v->vy) >> 12);
        m->m[i][2] = sat16(((long long)m->m[i][2] * v->vz) >> 12);
    }
    return m;
}

MATRIX *ScaleMatrixL(MATRIX *m, VECTOR *v)
{
    int i;

    for (i = 0; i < 3; i++)
    {
        m->m[0][i] = sat16(((long long)m->m[0][i] * v->vx) >> 12);
        m->m[1][i] = sat16(((long long)m->m[1][i] * v->vy) >> 12);
        m->m[2][i] = sat16(((long long)m->m[2][i] * v->vz) >> 12);
    }
    return m;
}

static void axis(short out[3][3], int which, int angle)
{
    short s = (short)rsin(angle), c = (short)rcos(angle);
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            out[i][j] = (short)(i == j ? 4096 : 0);
    switch (which)
    {
    case 0: out[1][1] = c; out[1][2] = -s; out[2][1] = s; out[2][2] = c; break;
    case 1: out[0][0] = c; out[0][2] = s; out[2][0] = -s; out[2][2] = c; break;
    default: out[0][0] = c; out[0][1] = -s; out[1][0] = s; out[1][1] = c; break;
    }
}

static MATRIX *rot3(SVECTOR *r, MATRIX *m, int a, int b, int c)
{
    short ma[3][3], mb[3][3], mc[3][3];
    int angles[3];

    angles[0] = r->vx;
    angles[1] = r->vy;
    angles[2] = r->vz;
    axis(ma, a, angles[a]);
    axis(mb, b, angles[b]);
    axis(mc, c, angles[c]);
    mul3(ma, ma, mb);
    mul3(m->m, ma, mc);
    return m;
}

MATRIX *RotMatrix(SVECTOR *r, MATRIX *m) { return rot3(r, m, 0, 1, 2); }
MATRIX *RotMatrixYXZ(SVECTOR *r, MATRIX *m) { return rot3(r, m, 1, 0, 2); }
MATRIX *RotMatrixZYX(SVECTOR *r, MATRIX *m) { return rot3(r, m, 2, 1, 0); }

MATRIX *RotMatrixX(long r, MATRIX *m)
{
    short a[3][3];

    axis(a, 0, (int)r);
    mul3(m->m, a, m->m);
    return m;
}

MATRIX *RotMatrixY(long r, MATRIX *m)
{
    short a[3][3];

    axis(a, 1, (int)r);
    mul3(m->m, a, m->m);
    return m;
}

MATRIX *RotMatrixZ(long r, MATRIX *m)
{
    short a[3][3];

    axis(a, 2, (int)r);
    mul3(m->m, a, m->m);
    return m;
}

/* ---- vectors ------------------------------------------------------------------------------ */
VECTOR *ApplyMatrix(MATRIX *m, SVECTOR *v0, VECTOR *v1)
{
    long x = v0->vx, y = v0->vy, z = v0->vz;
    int i;
    long out[3];

    for (i = 0; i < 3; i++)
    {
        out[i] = (long)(((long long)m->m[i][0] * x + (long long)m->m[i][1] * y + (long long)m->m[i][2] * z) >> 12);
    }
    v1->vx = out[0]; v1->vy = out[1]; v1->vz = out[2];
    return v1;
}

SVECTOR *ApplyMatrixSV(MATRIX *m, SVECTOR *v0, SVECTOR *v1)
{
    long x = v0->vx, y = v0->vy, z = v0->vz;
    short out[3];
    int i;

    for (i = 0; i < 3; i++)
    {
        out[i] = sat16(((long long)m->m[i][0] * x + (long long)m->m[i][1] * y + (long long)m->m[i][2] * z) >> 12);
    }
    v1->vx = out[0]; v1->vy = out[1]; v1->vz = out[2];
    return v1;
}

VECTOR *ApplyMatrixLV(MATRIX *m, VECTOR *v0, VECTOR *v1)
{
    long long x = v0->vx, y = v0->vy, z = v0->vz;
    long out[3];
    int i;

    for (i = 0; i < 3; i++)
    {
        out[i] = (long)((m->m[i][0] * x + m->m[i][1] * y + m->m[i][2] * z) >> 12);
    }
    v1->vx = out[0]; v1->vy = out[1]; v1->vz = out[2];
    return v1;
}

VECTOR *ApplyTransposeMatrixLV(MATRIX *m, VECTOR *v0, VECTOR *v1)
{
    long long x = v0->vx, y = v0->vy, z = v0->vz;
    long out[3];
    int i;

    for (i = 0; i < 3; i++)
    {
        out[i] = (long)((m->m[0][i] * x + m->m[1][i] * y + m->m[2][i] * z) >> 12);
    }
    v1->vx = out[0]; v1->vy = out[1]; v1->vz = out[2];
    return v1;
}

/* ---- GTE state ----------------------------------------------------------------------------- */
void SetRotMatrix(MATRIX *m)
{
    gte_SetRotMatrix(m);
}

void SetTransMatrix(MATRIX *m)
{
    gte_SetTransMatrix(m);
}

void SetLightMatrix(MATRIX *m)
{
    const long *w = (const long *)m;
    int i;

    for (i = 0; i < 5; i++) port_gte_write_ctrl(8 + i, w[i]);
}

void SetColorMatrix(MATRIX *m)
{
    const long *w = (const long *)m;
    int i;

    for (i = 0; i < 5; i++) port_gte_write_ctrl(16 + i, w[i]);
}

void SetBackColor(long rbk, long gbk, long bbk)
{
    port_gte_write_ctrl(13, rbk << 4);
    port_gte_write_ctrl(14, gbk << 4);
    port_gte_write_ctrl(15, bbk << 4);
}

void SetFarColor(long rfc, long gfc, long bfc)
{
    port_gte_write_ctrl(21, rfc << 4);
    port_gte_write_ctrl(22, gfc << 4);
    port_gte_write_ctrl(23, bfc << 4);
}

void SetGeomOffset(long ofx, long ofy)
{
    port_gte_write_ctrl(24, ofx << 16);
    port_gte_write_ctrl(25, ofy << 16);
}

void SetGeomScreen(long h)
{
    port_gte_write_ctrl(26, h);
}

void SetFogNear(long a, long h)
{
    long dqa, dqb;

    if (h == 0) h = 1;
    dqa = -((a * 64) / h);
    dqb = 0x1400000;
    if (dqa < -0x8000) dqa = -0x8000;
    if (dqa > 0x7FFF) dqa = 0x7FFF;
    port_gte_write_ctrl(27, dqa);
    port_gte_write_ctrl(28, dqb);
}

void InitGeom(void)
{
    port_gte_write_ctrl(29, 0x155);
    port_gte_write_ctrl(30, 0x100);
    port_gte_write_ctrl(26, 1000);
    port_gte_write_ctrl(27, -0x1062);
    port_gte_write_ctrl(28, 0x1400000);
}

#define MATRIX_STACK 20
static long sMatrixStack[MATRIX_STACK][8];
static int sMatrixTop;

void PushMatrix(void)
{
    int i;

    if (sMatrixTop >= MATRIX_STACK) return;
    for (i = 0; i < 8; i++) sMatrixStack[sMatrixTop][i] = gte_ctrl[i];
    sMatrixTop++;
}

void PopMatrix(void)
{
    int i;

    if (sMatrixTop <= 0) return;
    sMatrixTop--;
    for (i = 0; i < 8; i++) gte_ctrl[i] = sMatrixStack[sMatrixTop][i];
}

/* ---- perspective transforms ---------------------------------------------------------------- */
long RotTransPers(SVECTOR *v0, long *sxy, long *p, long *flag)
{
    gte_ldv0(v0);
    gte_rtps();
    gte_stsxy(sxy);
    *p = port_gte_read_data(8);
    *flag = port_gte_read_ctrl(31);
    return port_gte_read_data(19) >> 2;
}

long RotTransPers3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, long *sxy0, long *sxy1, long *sxy2, long *p, long *flag)
{
    gte_ldv3(v0, v1, v2);
    gte_rtpt();
    gte_stsxy3(sxy0, sxy1, sxy2);
    *p = port_gte_read_data(8);
    *flag = port_gte_read_ctrl(31);
    return port_gte_read_data(19) >> 2;
}

long RotTransPers4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, long *sxy0, long *sxy1, long *sxy2, long *sxy3,
                   long *p, long *flag)
{
    long f;

    gte_ldv3(v0, v1, v2);
    gte_rtpt();
    gte_stsxy3(sxy0, sxy1, sxy2);
    f = port_gte_read_ctrl(31);
    gte_ldv0(v3);
    gte_rtps();
    gte_stsxy(sxy3);
    *p = port_gte_read_data(8);
    *flag = f | port_gte_read_ctrl(31);
    return port_gte_read_data(19) >> 2;
}

void RotTrans(SVECTOR *v0, VECTOR *v1, long *flag)
{
    MATRIX m;
    int i;

    for (i = 0; i < 8; i++) ((long *)&m)[i] = gte_ctrl[i];
    ApplyMatrix(&m, v0, v1);
    v1->vx += m.t[0];
    v1->vy += m.t[1];
    v1->vz += m.t[2];
    *flag = 0;
}

long RotNclip3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, long *sxy0, long *sxy1, long *sxy2, long *p, long *otz, long *flag)
{
    long opz;

    gte_ldv3(v0, v1, v2);
    gte_rtpt();
    *flag = port_gte_read_ctrl(31);
    port_gte_op(GTE_CMD_NCLIP);
    opz = port_gte_read_data(24);
    if (opz > 0)
    {
        gte_stsxy3(sxy0, sxy1, sxy2);
        *p = port_gte_read_data(8);
        port_gte_op(GTE_CMD_AVSZ3);
        *otz = port_gte_read_data(7);
    }
    return opz;
}

long RotNclip4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, long *sxy0, long *sxy1, long *sxy2, long *sxy3,
               long *p, long *otz, long *flag)
{
    long opz, f;

    gte_ldv3(v0, v1, v2);
    gte_rtpt();
    f = port_gte_read_ctrl(31);
    port_gte_op(GTE_CMD_NCLIP);
    opz = port_gte_read_data(24);
    if (opz > 0)
    {
        gte_stsxy3(sxy0, sxy1, sxy2);
        gte_ldv0(v3);
        gte_rtps();
        gte_stsxy(sxy3);
        *p = port_gte_read_data(8);
        f |= port_gte_read_ctrl(31);
        port_gte_op(GTE_CMD_AVSZ4);
        *otz = port_gte_read_data(7);
    }
    *flag = f;
    return opz;
}

/* ---- lighting ------------------------------------------------------------------------------ */
void NormalColorCol(SVECTOR *v0, CVECTOR *v1, CVECTOR *v2)
{
    gte_ldv0(v0);
    port_gte_write_data(6, *(long *)v1);
    port_gte_op(GTE_CMD_NCCS);
    *(long *)v2 = port_gte_read_data(22);
}

void NormalColorCol3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, CVECTOR *v3, CVECTOR *v4, CVECTOR *v5, CVECTOR *v6)
{
    gte_ldv3(v0, v1, v2);
    port_gte_write_data(6, *(long *)v3);
    port_gte_op(GTE_CMD_NCCT);
    *(long *)v4 = port_gte_read_data(20);
    *(long *)v5 = port_gte_read_data(21);
    *(long *)v6 = port_gte_read_data(22);
}

void NormalColorDpq(SVECTOR *v0, CVECTOR *v1, long p, CVECTOR *v2)
{
    gte_ldv0(v0);
    port_gte_write_data(6, *(long *)v1);
    port_gte_write_data(8, p);
    port_gte_op(GTE_CMD_NCDS);
    *(long *)v2 = port_gte_read_data(22);
}
