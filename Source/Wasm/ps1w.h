/*
 * Guest memory of the wasm2c build.
 *
 * The guest module is linked so that every address it uses has the PS1 meaning:
 * PS1 RAM at 0x80000000 (the symbol-file data and the boot executable live there),
 * the module's own data, stack and heap right after it, up to PS1W_HEAP_END, and the
 * scratchpad at 0x1F800000. PS1W_OFFSET folds those onto one 8 MB buffer the way the
 * PS1 mirrors its RAM:
 *
 *   0x000000-0x1FFFFF  PS1 RAM            (0x80xxxxxx, 0x00xxxxxx, 0xA0xxxxxx)
 *   0x200000-0x7EFFFF  module data, stack, heap
 *   0x7F0000-0x7FFFFF  scratchpad (+ I/O) (0x1F80xxxx: the only addresses with bit 28 set)
 *
 * NULL and other low pointers therefore read PS1 RAM, as on the console. The
 * buffer is little-endian on every host; big-endian hosts swap on load/store.
 * 8 MB keeps the GameCube (24 MB) in reach; Ps1Game.cmake links the module with
 * --initial-memory = PS1W_HEAP_END so its heap stays below the scratchpad.
 */
#ifndef PS1W_H
#define PS1W_H

#define PS1W_WINDOW_MASK 0x007FFFFFu
#define PS1W_SCRATCH_OFFSET 0x007F0000u
#define PS1W_HEAP_END 0x807F0000u
/* KSEG0/KSEG1/KUSEG keep their low 23 bits; 0x1F8xxxxx moves to PS1W_SCRATCH_OFFSET */
#define PS1W_OFFSET(a) \
    (((uint32_t)(a) & PS1W_WINDOW_MASK) | ((0u - (((uint32_t)(a) >> 28) & 1u)) & PS1W_SCRATCH_OFFSET))
/* room for an access of up to 16 bytes at the last address */
#define PS1W_MEM_BYTES (PS1W_WINDOW_MASK + 1u + 16u)

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define PS1W_BIG_ENDIAN 1
#else
#define PS1W_BIG_ENDIAN 0
#endif

/* libogc/newlib has no signals: wasm-rt.h's sigsetjmp/siglongjmp (used outside
 * Windows) are plain setjmp/longjmp there */
#if defined(GEKKO)
#include <setjmp.h>
#define sigsetjmp(b, s) setjmp(b)
#define siglongjmp(b, v) longjmp(b, v)
#endif

#endif /* PS1W_H */
