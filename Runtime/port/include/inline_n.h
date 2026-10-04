/*
 * PORT: replaces the decomp's include/gcc/inline_n.h (MIPS cop2 inline asm)
 * with calls into the software GTE. Only the macro forms the game uses are
 * provided; using another one is a compile error, which is the point.
 */
#ifndef PORT_INLINE_N_H
#define PORT_INLINE_N_H

#include <port_gte.h>

static __inline void port_gte_lwc2(int reg, const void *p)
{
    port_gte_write_data(reg, *(const long *)p);
}

static __inline void port_gte_swc2(int reg, void *p)
{
    *(long *)p = port_gte_read_data(reg);
}

#define gte_ldv0(r0) (port_gte_lwc2(0, (const char *)(r0)), port_gte_lwc2(1, (const char *)(r0) + 4))
#define gte_ldv1(r0) (port_gte_lwc2(2, (const char *)(r0)), port_gte_lwc2(3, (const char *)(r0) + 4))
#define gte_ldv2(r0) (port_gte_lwc2(4, (const char *)(r0)), port_gte_lwc2(5, (const char *)(r0) + 4))
#define gte_ldv3(r0, r1, r2) (gte_ldv0(r0), gte_ldv1(r1), gte_ldv2(r2))
#define gte_ldv3c(r0) (gte_ldv0(r0), gte_ldv1((const char *)(r0) + 8), gte_ldv2((const char *)(r0) + 16))

#define gte_ldlzc(r0) port_gte_write_data(30, (long)(r0))
#define gte_stlzc(r0) port_gte_swc2(31, (void *)(r0))
#define gte_ldrgb(r0) port_gte_lwc2(6, (const void *)(r0))
#define gte_ldIR0(r0) port_gte_write_data(8, (long)(r0))
#define gte_lddp(r0) port_gte_write_data(8, (long)(r0))

#define gte_rtps() port_gte_op(GTE_CMD_RTPS)
#define gte_rtpt() port_gte_op(GTE_CMD_RTPT)
#define gte_nclip() port_gte_op(GTE_CMD_NCLIP)
#define gte_avsz3() port_gte_op(GTE_CMD_AVSZ3)
#define gte_avsz4() port_gte_op(GTE_CMD_AVSZ4)
#define gte_ncs() port_gte_op(GTE_CMD_NCS)
#define gte_nccs() port_gte_op(GTE_CMD_NCCS)
#define gte_ncds() port_gte_op(GTE_CMD_NCDS)
#define gte_dpcs() port_gte_op(GTE_CMD_DPCS)

#define gte_stsxy(r0) port_gte_swc2(14, (void *)(r0))
#define gte_stsxy3(r0, r1, r2) (port_gte_swc2(12, (void *)(r0)), port_gte_swc2(13, (void *)(r1)), port_gte_swc2(14, (void *)(r2)))
#define gte_stsxy01(r0, r1) (port_gte_swc2(12, (void *)(r0)), port_gte_swc2(13, (void *)(r1)))
#define gte_stsz(r0) port_gte_swc2(19, (void *)(r0))
#define gte_stotz(r0) port_gte_swc2(7, (void *)(r0))
#define gte_stopz(r0) port_gte_swc2(24, (void *)(r0))
#define gte_stflg(r0) (*(long *)(r0) = port_gte_read_ctrl(31))
#define gte_stszotz(r0) (*(long *)(r0) = port_gte_read_data(19) >> 2)
#define gte_strgb(r0) port_gte_swc2(22, (void *)(r0))
#define gte_stdp(r0) port_gte_swc2(8, (void *)(r0))

#define gte_SetRotMatrix(r0)                                         \
    (port_gte_write_ctrl(0, ((const long *)(r0))[0]),                \
     port_gte_write_ctrl(1, ((const long *)(r0))[1]),                \
     port_gte_write_ctrl(2, ((const long *)(r0))[2]),                \
     port_gte_write_ctrl(3, ((const long *)(r0))[3]),                \
     port_gte_write_ctrl(4, ((const long *)(r0))[4]))
#define gte_SetTransMatrix(r0)                                       \
    (port_gte_write_ctrl(5, ((const long *)(r0))[5]),                \
     port_gte_write_ctrl(6, ((const long *)(r0))[6]),                \
     port_gte_write_ctrl(7, ((const long *)(r0))[7]))

#endif /* PORT_INLINE_N_H */
