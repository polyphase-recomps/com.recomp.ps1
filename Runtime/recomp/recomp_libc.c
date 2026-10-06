/*
 * The C library functions of PsyQ's libc the recompiled game calls, done here on guest memory
 * (the libraries' module has no exports for them). Strings and buffers are guest addresses;
 * the PS1 semantics are the C ones.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp.h"

#define A0 ((uint32_t)ctx->r4)
#define A1 ((uint32_t)ctx->r5)
#define A2 ((uint32_t)ctx->r6)
#define RET(v) (ctx->r2 = (gpr)(int32_t)(uint32_t)(v))
/* (PS1_PTR evaluates its argument more than once: no side effects in it) */
#define G(addr) ((char *)PS1_PTR(addr))

/* guest buffers never wrap past the 8 MB window in practice; byte loops keep the folding exact */
void memcpy_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint32_t i;
    for (i = 0; i < A2; i++) *G(A0 + i) = *G(A1 + i);
    RET(A0);
}

void memmove_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint32_t i;
    if (A0 < A1)
        for (i = 0; i < A2; i++) *G(A0 + i) = *G(A1 + i);
    else
        for (i = A2; i > 0; i--) *G(A0 + i - 1) = *G(A1 + i - 1);
    RET(A0);
}

void memset_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint32_t i;
    for (i = 0; i < A2; i++) *G(A0 + i) = (char)A1;
    RET(A0);
}

void memcmp_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint32_t i;
    for (i = 0; i < A2; i++)
    {
        const int d = (unsigned char)*G(A0 + i) - (unsigned char)*G(A1 + i);
        if (d) { RET(d); return; }
    }
    RET(0);
}

void strlen_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint32_t n = 0;
    while (*G(A0 + n)) n++;
    RET(n);
}

void host_log(const char *fmt, ...);
#define TRACE(name) do { if (getenv("PS1_RECOMP_LIBC_TRACE")) host_log("%s(%08X, %08X '%.40s')", name, A0, A1, G(A1)); } while (0)

void strcpy_recomp(uint8_t *rdram, recomp_context *ctx)
{
    TRACE("strcpy");
    uint32_t i;
    for (i = 0;; i++)
    {
        const char c = *G(A1 + i);
        *G(A0 + i) = c;
        if (c == 0) break;
    }
    RET(A0);
}

void strncpy_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint32_t i = 0;
    for (; i < A2 && *G(A1 + i); i++) *G(A0 + i) = *G(A1 + i);
    for (; i < A2; i++) *G(A0 + i) = 0;
    RET(A0);
}

void strcat_recomp(uint8_t *rdram, recomp_context *ctx)
{
    TRACE("strcat");
    uint32_t end = 0, i;
    while (*G(A0 + end)) end++;
    for (i = 0;; i++)
    {
        const char c = *G(A1 + i);
        *G(A0 + end + i) = c;
        if (c == 0) break;
    }
    RET(A0);
}

void strcmp_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint32_t i = 0;
    for (;; i++)
    {
        const unsigned char a = (unsigned char)*G(A0 + i), b = (unsigned char)*G(A1 + i);
        if (a != b || a == 0) { RET((int)a - (int)b); return; }
    }
}

void strncmp_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint32_t i = 0;
    for (; i < A2; i++)
    {
        const unsigned char a = (unsigned char)*G(A0 + i), b = (unsigned char)*G(A1 + i);
        if (a != b || a == 0) { RET((int)a - (int)b); return; }
    }
    RET(0);
}

void strchr_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint32_t i = 0;
    for (;; i++)
    {
        const char c = *G(A0 + i);
        if (c == (char)A1) { RET(A0 + i); return; }
        if (c == 0) { RET(0); return; }
    }
}

void strrchr_recomp(uint8_t *rdram, recomp_context *ctx)
{
    uint32_t i = 0, found = 0;
    for (;; i++)
    {
        const char c = *G(A0 + i);
        if (c == (char)A1) found = A0 + i;
        if (c == 0) break;
    }
    RET(found);
}

void abs_recomp(uint8_t *rdram, recomp_context *ctx)
{
    const int32_t v = (int32_t)A0;
    (void)rdram;
    RET(v < 0 ? -v : v);
}
