/*
 * Guest pieces that only the wasm build needs (the native build gets them from the
 * host side): the guest heap for port_alloc, and exit(). The rest of the C library
 * the game uses (string functions, math) comes from wasi-libc.
 */
#include <port_host.h>

/* wasi-libc's heap, not the game's (a game may map calloc to ps1_calloc, libc2.c) */
#undef calloc
void *calloc(unsigned long n, unsigned long size);

void *port_alloc(unsigned long size)
{
    void *p = calloc(1, size ? size : 1);

    if (p == 0) port_fatal("out of memory (%lu bytes)", size);
    return p;
}

void exit(int code)
{
    port_fatal("game called exit(%d)", code);
    for (;;)
    {
    }
}

/* wasm-ld wraps the exported entry point in a "command" that runs the destructors
 * on return; there are none, and wasi-libc's version would bring its exit() along. */
void __wasm_call_dtors(void)
{
}
