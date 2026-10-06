/*
 * Included by tools/wasm_to_c.py into the wasm2c -impl.h after its arithmetic
 * helpers: R3000 behaviour instead of wasm traps.
 */

/* Integer division never traps on the R3000. Division by zero gives quotient -1
 * (+1 for a negative dividend; all ones unsigned) and the dividend as remainder;
 * INT_MIN / -1 gives INT_MIN remainder 0. Applied to 64-bit too (compiler helpers). */
#undef DIV_S
#define DIV_S(ut, min, x, y)                                     \
  ((UNLIKELY((y) == 0))                  ? (ut)((x) < 0 ? 1 : -1) \
   : (UNLIKELY((x) == min && (y) == -1)) ? (ut)(x)               \
                                         : (ut)((x) / (y)))
#undef REM_S
#define REM_S(ut, min, x, y)                                 \
  ((UNLIKELY((y) == 0))                  ? (ut)(x)           \
   : (UNLIKELY((x) == min && (y) == -1)) ? 0                 \
                                         : (ut)((x) % (y)))
#undef DIV_U
#define DIV_U(x, y) ((UNLIKELY((y) == 0)) ? ~((x) - (x)) : ((x) / (y)))
#undef REM_U
#define REM_U(x, y) ((UNLIKELY((y) == 0)) ? (x) : ((x) % (y)))

/* Indirect calls: decomps call through function pointers whose type differs from
 * the function's (K&R pointers, tables of mixed handlers). The MIPS calling
 * convention tolerates that and so do the host ABIs (arguments in registers, the
 * callee ignores extras), so only check that the slot holds a function. */
void ps1w_bad_indirect_call(uint32_t index);
#undef CHECK_CALL_INDIRECT
#define CHECK_CALL_INDIRECT(table, ft, x) \
  (LIKELY((x) < table.size && table.data[x].func) || (ps1w_bad_indirect_call(x), 0))

/* the module instance of a table slot, NULL past the table (tools/wasm_to_c.py) */
#define PS1W_FUNCREF_INSTANCE(table, x) ((x) < (table).size ? (table).data[x].module_instance : (void *)0)

/* Recomp mode (Runtime/cmake/Ps1Recomp.cmake): the PsyQ libraries are this module and the
 * game is recompiled code, so a function pointer the game hands to a library (VSyncCallback,
 * DecDCToutCallback ...) is a PS1 code address, not a table slot. Such a call runs the
 * recompiled function at that address (Runtime/recomp/recomp_ps1.c). */
#ifdef PS1W_RECOMP
void *ps1w_guest_callback(uint32_t address);
#undef CALL_INDIRECT
#define CALL_INDIRECT(table, t, ft, x, ...)                                              \
  ((LIKELY((x) < table.size && table.data[x].func)) ? ((t)table.data[x].func)(__VA_ARGS__) \
                                                     : ((t)ps1w_guest_callback(x))(__VA_ARGS__))
#endif
