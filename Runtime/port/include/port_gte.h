/*
 * Software model of the PlayStation Geometry Transformation Engine (COP2).
 *
 * gte_data / gte_ctrl are the 32 data and 32 control registers exactly as
 * the hardware exposes them through mfc2/mtc2 and cfc2/ctc2; port_gte_op()
 * executes one GTE command word. The inline macros of the PsyQ headers and
 * the libgte functions are built on top of this.
 */
#ifndef PORT_GTE_H
#define PORT_GTE_H

extern long gte_data[32];
extern long gte_ctrl[32];

/* Register accessors with the hardware's read/write side effects. */
long port_gte_read_data(int reg);
void port_gte_write_data(int reg, long value);
long port_gte_read_ctrl(int reg);
void port_gte_write_ctrl(int reg, long value);

/* Execute a GTE command (the 25-bit immediate of the cop2 instruction). */
void port_gte_op(unsigned long command);

/* Command words used by the PsyQ macros (sf/lm/mx/v/cv fields included). */
#define GTE_CMD_RTPS  0x0180001
#define GTE_CMD_RTPT  0x0280030
#define GTE_CMD_NCLIP 0x1400006
#define GTE_CMD_AVSZ3 0x158002D
#define GTE_CMD_AVSZ4 0x168002E
#define GTE_CMD_NCDS  0x0E80413
#define GTE_CMD_NCDT  0x0F80416
#define GTE_CMD_NCCS  0x108041B
#define GTE_CMD_NCCT  0x118043F
#define GTE_CMD_NCS   0x0C8041E
#define GTE_CMD_NCT   0x0D80420
#define GTE_CMD_CC    0x138041C
#define GTE_CMD_CDP   0x1280414
#define GTE_CMD_DPCS  0x0780010
#define GTE_CMD_DPCT  0x0F8002A
#define GTE_CMD_INTPL 0x0980011
#define GTE_CMD_SQR   0x0A00428
#define GTE_CMD_OP    0x170000C
#define GTE_CMD_GPF   0x190003D
#define GTE_CMD_GPL   0x1A0003E

#endif /* PORT_GTE_H */
