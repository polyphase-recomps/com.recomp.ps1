/*
 * The contract N64Recomp's C output (PS1 mode, [input] arch = "ps1") is compiled against:
 * com.recomp.ps1's recomp runtime. Modelled on N64Recomp's include/recomp.h (MIT), with the
 * PlayStation's memory: little-endian (no byte swizzles) and addresses folded like the wasm
 * guest's (Source/Wasm/ps1w.h): RAM and its mirrors, the scratchpad and the I/O page all
 * land in one 8 MB buffer, which the recompiled code shares with the runtime's PsyQ
 * libraries (the wasm2c module). `rdram` is that buffer.
 *
 * Registers keep N64Recomp's layout (64-bit, 32-bit values sign-extended), so the same
 * recomp_context works for LiveRecomp too.
 */
#ifndef PS1_RECOMP_H
#define PS1_RECOMP_H

#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>

#include "ps1w.h"

#if defined(__clang__)
#define RECOMP_FUNC extern inline __attribute__((weak, noinline))
#elif defined(_MSC_VER)
#define RECOMP_FUNC __declspec(noinline)
#else
#define RECOMP_FUNC __attribute__((noipa))
#endif

typedef uint64_t gpr;

#define SIGNED(val) ((int64_t)(val))
#define ADD32(a, b) ((gpr)(int32_t)((a) + (b)))
#define SUB32(a, b) ((gpr)(int32_t)((a) - (b)))
#define S32(val) ((int32_t)(val))
#define U32(val) ((uint32_t)(val))
#define S64(val) ((int64_t)(val))
#define U64(val) ((uint64_t)(val))

/* a guest address -> its byte in the shared buffer */
#define PS1_PTR(addr) (rdram + PS1W_OFFSET((uint32_t)(addr)))

#define MEM_W(offset, reg) (*(int32_t *)PS1_PTR((reg) + (offset)))
#define MEM_H(offset, reg) (*(int16_t *)PS1_PTR((reg) + (offset)))
#define MEM_HU(offset, reg) (*(uint16_t *)PS1_PTR((reg) + (offset)))
#define MEM_B(offset, reg) (*(int8_t *)PS1_PTR((reg) + (offset)))
#define MEM_BU(offset, reg) (*(uint8_t *)PS1_PTR((reg) + (offset)))

/* unaligned word access, little-endian (MIPS LE lwl/lwr/swl/swr) */
static inline gpr do_lwl(uint8_t *rdram, gpr initial_value, gpr offset, gpr reg)
{
    const uint32_t address = (uint32_t)(offset + reg);
    const uint32_t shift = (address & 3) * 8;
    const uint32_t word = (uint32_t)MEM_W(0, address & ~3u);
    const uint32_t keep = 0x00FFFFFFu >> shift;
    return (gpr)(int32_t)(((uint32_t)initial_value & keep) | (word << (24 - shift)));
}

static inline gpr do_lwr(uint8_t *rdram, gpr initial_value, gpr offset, gpr reg)
{
    const uint32_t address = (uint32_t)(offset + reg);
    const uint32_t shift = (address & 3) * 8;
    const uint32_t word = (uint32_t)MEM_W(0, address & ~3u);
    const uint32_t keep = shift ? (0xFFFFFFFFu << (32 - shift)) : 0u;
    return (gpr)(int32_t)(((uint32_t)initial_value & keep) | (word >> shift));
}

static inline void do_swl(uint8_t *rdram, gpr offset, gpr reg, gpr val)
{
    const uint32_t address = (uint32_t)(offset + reg);
    const uint32_t shift = (address & 3) * 8;
    const uint32_t keep = shift == 24 ? 0u : (0xFFFFFFFFu << (shift + 8));
    uint32_t *word = (uint32_t *)PS1_PTR(address & ~3u);
    *word = (*word & keep) | ((uint32_t)val >> (24 - shift));
}

static inline void do_swr(uint8_t *rdram, gpr offset, gpr reg, gpr val)
{
    const uint32_t address = (uint32_t)(offset + reg);
    const uint32_t shift = (address & 3) * 8;
    const uint32_t keep = shift ? (0xFFFFFFFFu >> (32 - shift)) : 0u;
    uint32_t *word = (uint32_t *)PS1_PTR(address & ~3u);
    *word = (*word & keep) | ((uint32_t)val << shift);
}

/* R3000 division: never traps (zero divisor and INT_MIN / -1 give defined results) */
static inline void PS1_DIV(uint64_t *lo, uint64_t *hi, uint32_t a, uint32_t b)
{
    const int32_t x = (int32_t)a, y = (int32_t)b;
    if (y == 0)
    {
        *lo = (uint64_t)(int64_t)(x < 0 ? 1 : -1);
        *hi = (uint64_t)(int64_t)x;
    }
    else if (x == INT32_MIN && y == -1)
    {
        *lo = (uint64_t)(int64_t)x;
        *hi = 0;
    }
    else
    {
        *lo = (uint64_t)(int64_t)(x / y);
        *hi = (uint64_t)(int64_t)(x % y);
    }
}

static inline void PS1_DIVU(uint64_t *lo, uint64_t *hi, uint32_t a, uint32_t b)
{
    *lo = (uint64_t)(int64_t)(int32_t)(b ? a / b : 0xFFFFFFFFu);
    *hi = (uint64_t)(int64_t)(int32_t)(b ? a % b : a);
}

typedef union
{
    double d;
    struct
    {
        float fl;
        float fh;
    };
    struct
    {
        uint32_t u32l;
        uint32_t u32h;
    };
    uint64_t u64;
} fpr;

typedef struct
{
    gpr r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15, r16, r17, r18, r19, r20, r21, r22,
        r23, r24, r25, r26, r27, r28, r29, r30, r31;
    fpr f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22,
        f23, f24, f25, f26, f27, f28, f29, f30, f31;
    uint64_t hi, lo;
    uint32_t *f_odd;
    uint32_t status_reg;
    uint8_t mips3_float_mode;
} recomp_context;

#ifdef __cplusplus
extern "C" {
#endif

typedef void(recomp_func_t)(uint8_t *rdram, recomp_context *ctx);
typedef void(recomp_func_ext_t)(uint8_t *rdram, recomp_context *ctx, uintptr_t arg);

recomp_func_t *get_function(int32_t vram);
#define LOOKUP_FUNC(val) get_function((int32_t)(val))

extern int32_t *section_addresses;
#define LO16(x) ((x) & 0xFFFF)
#define HI16(x) (((x) >> 16) + (((x) >> 15) & 1))
#define RELOC_HI16(section_index, offset) HI16(section_addresses[section_index] + (offset))
#define RELOC_LO16(section_index, offset) LO16(section_addresses[section_index] + (offset))

void switch_error(const char *func, uint32_t vram, uint32_t jtbl);
void do_break(uint32_t vram);
void recomp_syscall_handler(uint8_t *rdram, recomp_context *ctx, int32_t instruction_vram);
void pause_self(uint8_t *rdram);
void cop0_status_write(recomp_context *ctx, gpr value);
gpr cop0_status_read(recomp_context *ctx);

/* cop0 (system control) and the GTE (cop2): the runtime's (recomp_ps1.c) */
uint32_t ps1_cop0_read(recomp_context *ctx, int reg);
void ps1_cop0_write(recomp_context *ctx, int reg, uint32_t value);
void ps1_rfe(recomp_context *ctx);
void ps1_gte_command(recomp_context *ctx, uint32_t instruction);
uint32_t ps1_gte_read_data(recomp_context *ctx, int reg);
void ps1_gte_write_data(recomp_context *ctx, int reg, uint32_t value);
uint32_t ps1_gte_read_ctrl(recomp_context *ctx, int reg);
void ps1_gte_write_ctrl(recomp_context *ctx, int reg, uint32_t value);

/* setjmp / longjmp of the game (PsyQ libc): the call site of setjmp is generated inline
 * (a host setjmp in the recompiled function that called it); longjmp jumps back to it. */
jmp_buf *ps1_setjmp_begin(uint8_t *rdram, recomp_context *ctx);
void ps1_setjmp_resume(uint8_t *rdram, recomp_context *ctx, int value);

#ifdef __cplusplus
}
#endif

#endif /* PS1_RECOMP_H */
