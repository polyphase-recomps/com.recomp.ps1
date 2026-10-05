/*
 * PsyQ libc / libapi pieces whose names clash with the host C library: malloc, free,
 * calloc, realloc and itoa (Psy-Q's itoa takes one argument and returns a static
 * string). They are ps1_malloc ... here; a game that calls them maps the names in its
 * compile definitions (malloc=ps1_malloc ...; see the LSD package). InitHeap is
 * Psy-Q's own name and needs no mapping.
 *
 * They share libapi.c's heap (InitHeap3/malloc3) in PS1 RAM, where PS1 code expects
 * its memory (24-bit ordering-table links, DMA-style addresses). On the PS1 the
 * startup code gives the heap everything between the end of BSS and the stack; a game
 * that never calls InitHeap gets that here on its first allocation (the game stack is
 * not in PS1 RAM in this port, so the heap reaches the top of PS1 RAM: 2 MB, or what
 * the game package gives, ps1_add_game RAM_SIZE).
 */
#include <port_host.h>
#include <ps1_game_config.h>

void InitHeap3(unsigned long *head, unsigned long size);
void *malloc3(unsigned long size);
void free3(void *ptr);
void *calloc3(unsigned long n, unsigned long s);

extern unsigned long port_boot_bss_end; /* boot.c */
static int sHeapReady;

void InitHeap(unsigned long *head, unsigned long size)
{
    InitHeap3(head, size);
    sHeapReady = 1;
}

static void default_heap(void)
{
    if (!sHeapReady)
    {
        unsigned long start = (port_boot_bss_end + 15) & ~15ul;
        unsigned long end = PORT_RAM_BASE + PS1_RAM_SIZE;

        if (start < PORT_RAM_BASE || start >= end) start = PORT_RAM_BASE + 0x100000;
        InitHeap((unsigned long *)start, end - start);
    }
}

void *ps1_malloc(unsigned long size)
{
    default_heap();
    return malloc3(size);
}

void ps1_free(void *p)
{
    free3(p);
}

void *ps1_calloc(unsigned long n, unsigned long s)
{
    default_heap();
    return calloc3(n, s);
}

void *ps1_realloc(void *p, unsigned long size)
{
    unsigned char *q;
    unsigned long old, i;

    if (!p) return ps1_malloc(size);
    /* libapi.c's block header: 8 bytes before the data, size (with flag bit 0) first */
    old = (((unsigned long *)p)[-2] & ~1ul) - 8;
    if (old >= size) return p;
    q = ps1_malloc(size);
    if (!q) return 0;
    for (i = 0; i < old; i++) q[i] = ((unsigned char *)p)[i];
    ps1_free(p);
    return q;
}

char *ps1_itoa(int n)
{
    static char buf[16];
    char *p = buf + sizeof buf - 1;
    unsigned v = n < 0 ? 0u - (unsigned)n : (unsigned)n;

    *p = 0;
    do
    {
        *--p = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    if (n < 0) *--p = '-';
    return p;
}
