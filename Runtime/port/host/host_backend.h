/*
 * Between a platform host (win32/, later wii/...) and the guest backend that runs
 * the game code: native/ (the guest linked in as 32-bit x86, PS1 memory mapped at
 * its real addresses) or wasm/ (the guest translated from WebAssembly by wasm2c,
 * PS1 memory in a buffer).
 */
#ifndef HOST_BACKEND_H
#define HOST_BACKEND_H

#include <stddef.h>

/* ---- guest backend ------------------------------------------------------------- */
/* Prepares guest memory and fault handling; returns 0 on failure (already logged). */
int ps1_backend_init(void);
/* Runs the game (boot, then main) on the calling thread; returns if main returns. */
void ps1_backend_run(void);
/* Stack size the game thread needs. */
size_t ps1_backend_stack_size(void);
/* Called after a fatal error was logged: reports what it can and stops. */
void ps1_backend_fatal(void);

/* ---- platform host ------------------------------------------------------------- */
void host_log(const char *fmt, ...);
/* Marks the game as crashed for an embedding editor and ends the process. */
void host_crashed(void);

#endif /* HOST_BACKEND_H */
