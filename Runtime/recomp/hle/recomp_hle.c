/*
 * The PsyQ libraries as the recomp runtime's wasm2c module ("ps1hle", Ps1Recomp.cmake): the
 * pieces a decomp build gets from boot.c and overlays.c, which this module leaves out (the
 * game boots itself, from its own startup code, and its overlays are real disc loads).
 */
#include <port_host.h>

/* End of the executable's BSS: where the PS1 startup code starts the heap (libc2.c). The
 * runtime sets it after loading the executable. */
unsigned long port_boot_bss_end;

void port_recomp_set_bss_end(unsigned long end)
{
    port_boot_bss_end = end;
}

/* overlays.c's: overlay data is reloaded from the disc in recomp mode */
void port_overlays_snapshot(void)
{
}

void port_overlay_reset(int lib)
{
    (void)lib;
}

/* A buffer for building variadic argument lists (printf, sprintf: wasm32 passes them in
 * memory) from the MIPS registers and stack. */
static unsigned long sVarargs[32];

unsigned long port_recomp_varargs(void)
{
    return (unsigned long)sVarargs;
}

/* libgs's globals. The runtime's libgs (libgs.c) uses the game's own (its data symbols put them
 * at the game's addresses); a game that doesn't use libgs has none, so these stand in. Sizes
 * are at least the PsyQ types' (MATRIX, DRAWENV, DISPENV, RECT, ...). */
#define PS1R_LIBGS_GLOBAL(name, size) __attribute__((weak, aligned(8))) unsigned char name[size]
PS1R_LIBGS_GLOBAL(GsWSMATRIX, 32);
PS1R_LIBGS_GLOBAL(GsWSMATRIX_ORG, 32);
PS1R_LIBGS_GLOBAL(GsLIGHTWSMATRIX, 32);
PS1R_LIBGS_GLOBAL(GsIDMATRIX, 32);
PS1R_LIBGS_GLOBAL(GsIDMATRIX2, 32);
PS1R_LIBGS_GLOBAL(GsOUT_PACKET_P, 4);
PS1R_LIBGS_GLOBAL(GsDRAWENV, 96);
PS1R_LIBGS_GLOBAL(GsDISPENV, 24);
PS1R_LIBGS_GLOBAL(POSITION, 16);
PS1R_LIBGS_GLOBAL(CLIP2, 8);
PS1R_LIBGS_GLOBAL(PSDCNT, 4);
PS1R_LIBGS_GLOBAL(PSDIDX, 4);
PS1R_LIBGS_GLOBAL(VWD0, 4);
PS1R_LIBGS_GLOBAL(HWD0, 4);
PS1R_LIBGS_GLOBAL(GsLIGHT_MODE, 4);

/* ---- startup and BIOS pieces a game's own code calls ---------------------------------------- */
/* PsyQ's start code runs the C++ constructors through __main: a C game has none */
void __main(void)
{
}

/* the instruction cache: recompiled code has none to flush */
void FlushCache(void)
{
}

static long sVideoMode; /* MODE_NTSC */

long SetVideoMode(long mode)
{
    long old = sVideoMode;

    sVideoMode = mode;
    return old;
}

long GetVideoMode(void)
{
    return sVideoMode;
}

/* ---- libc2 functions the runtime's libraries don't use themselves ---------------------------- */
void bcopy(const void *src, void *dst, int n)
{
    const unsigned char *s = (const unsigned char *)src;
    unsigned char *d = (unsigned char *)dst;

    if (d < s)
        while (n-- > 0) *d++ = *s++;
    else
        while (n-- > 0) d[n] = s[n];
}

void *memchr(const void *p, int c, unsigned long n)
{
    const unsigned char *s = (const unsigned char *)p;

    for (; n > 0; n--, s++)
        if (*s == (unsigned char)c) return (void *)s;
    return 0;
}

char *strstr(const char *s, const char *find)
{
    int i;

    for (; *s; s++)
    {
        for (i = 0; find[i] && s[i] == find[i]; i++)
        {
        }
        if (!find[i]) return (char *)s;
    }
    return *find ? 0 : (char *)s;
}

char *strncat(char *dst, const char *src, unsigned long n)
{
    char *d = dst;

    while (*d) d++;
    while (n-- > 0 && *src) *d++ = *src++;
    *d = 0;
    return dst;
}

int toupper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

int tolower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}
