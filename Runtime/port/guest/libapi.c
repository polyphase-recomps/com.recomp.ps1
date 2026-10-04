/* PsyQ libapi / libc pieces: heap (InitHeap3/malloc3/free3), rand, bzero, printf, sprintf. */
#include <port_host.h>

/* ---- heap -----------------------------------------------------------------------------------
 * First-fit allocator over the region the game hands to InitHeap3 (PS1 RAM above BSS).
 * Block header: size of the whole block (bytes, 8-aligned) with bit 0 = in use. */
typedef struct HeapBlock
{
    unsigned long size;
    unsigned long pad;
} HeapBlock;

static unsigned char *sHeapStart;
static unsigned char *sHeapEnd;

void InitHeap3(unsigned long *head, unsigned long size)
{
    HeapBlock *b;

    sHeapStart = (unsigned char *)(((unsigned long)head + 7) & ~7ul);
    sHeapEnd = (unsigned char *)(((unsigned long)head + size) & ~7ul);
    b = (HeapBlock *)sHeapStart;
    b->size = (unsigned long)(sHeapEnd - sHeapStart);
    port_log("heap: %p-%p (%lu KB)", sHeapStart, sHeapEnd, (unsigned long)(sHeapEnd - sHeapStart) / 1024);
}

void *malloc3(unsigned long size)
{
    unsigned char *p = sHeapStart;
    unsigned long need = ((size + 7) & ~7ul) + sizeof(HeapBlock);

    if (sHeapStart == 0) return 0;
    while (p < sHeapEnd)
    {
        HeapBlock *b = (HeapBlock *)p;
        unsigned long bsize = b->size & ~1ul;

        if (bsize == 0) break;
        if (!(b->size & 1))
        {
            /* merge following free blocks */
            while (p + bsize < sHeapEnd)
            {
                HeapBlock *n = (HeapBlock *)(p + bsize);

                if (n->size & 1 || (n->size & ~1ul) == 0) break;
                bsize += n->size;
            }
            b->size = bsize;
            if (bsize >= need)
            {
                if (bsize - need >= sizeof(HeapBlock) + 8)
                {
                    HeapBlock *rest = (HeapBlock *)(p + need);

                    rest->size = bsize - need;
                    b->size = need | 1;
                }
                else
                {
                    b->size = bsize | 1;
                }
                return p + sizeof(HeapBlock);
            }
        }
        p += bsize;
    }
    port_log("malloc3(%lu) failed", size);
    return 0;
}

void free3(void *ptr)
{
    HeapBlock *b;

    if (ptr == 0) return;
    b = (HeapBlock *)((unsigned char *)ptr - sizeof(HeapBlock));
    if ((unsigned char *)b < sHeapStart || (unsigned char *)b >= sHeapEnd || !(b->size & 1))
    {
        port_log("free3(%p): not a heap block", ptr);
        return;
    }
    b->size &= ~1ul;
}

void *calloc3(unsigned long n, unsigned long s)
{
    unsigned char *p = malloc3(n * s);
    unsigned long i;

    if (p)
        for (i = 0; i < n * s; i++) p[i] = 0;
    return p;
}

/* ---- rand -----------------------------------------------------------------------------------
 * The PS1 C library's generator (the ANSI example LCG, 15-bit results), so that every
 * host gives the game the same sequence instead of its own C library's. */
static unsigned long sRandNext = 1;

int rand(void)
{
    sRandNext = sRandNext * 1103515245u + 12345u;
    return (int)((sRandNext >> 16) & 0x7FFF);
}

void srand(unsigned int seed)
{
    sRandNext = seed;
}

void bzero(void *p, int n)
{
    unsigned char *d = p;

    while (n-- > 0) *d++ = 0;
}

/* ---- formatting ---------------------------------------------------------------------------- */
typedef __builtin_va_list va_list_t;

static int fmt_out(char *buf, int cap, int pos, char c)
{
    if (pos < cap - 1) buf[pos] = c;
    return pos + 1;
}

int port_vsnprintf(char *buf, int cap, const char *fmt, va_list_t ap)
{
    int pos = 0;

    for (; *fmt; fmt++)
    {
        char tmp[24];
        int left = 0, zero = 0, width = 0, prec = -1, len = 0, neg = 0, i;
        unsigned long v;
        const char *s;
        char c;

        if (*fmt != '%')
        {
            pos = fmt_out(buf, cap, pos, *fmt);
            continue;
        }
        fmt++;
        for (;; fmt++)
        {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == ' ' || *fmt == '+' || *fmt == '#') {}
            else break;
        }
        if (*fmt == '*')
        {
            width = __builtin_va_arg(ap, int);
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.')
        {
            fmt++;
            prec = 0;
            if (*fmt == '*')
            {
                prec = __builtin_va_arg(ap, int);
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'h') fmt++;
        c = *fmt;
        s = tmp;
        switch (c)
        {
        case 'd':
        case 'i':
        {
            long sv = __builtin_va_arg(ap, long);

            if (sv < 0)
            {
                neg = 1;
                v = (unsigned long)-sv;
            }
            else
            {
                v = (unsigned long)sv;
            }
            goto dec;
        }
        case 'u':
            v = __builtin_va_arg(ap, unsigned long);
        dec:
            i = sizeof tmp;
            tmp[--i] = 0;
            do
            {
                tmp[--i] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            s = tmp + i;
            break;
        case 'x':
        case 'X':
        case 'p':
            v = __builtin_va_arg(ap, unsigned long);
            i = sizeof tmp;
            tmp[--i] = 0;
            do
            {
                int d = (int)(v & 15);

                tmp[--i] = (char)(d < 10 ? '0' + d : (c == 'X' ? 'A' : 'a') + d - 10);
                v >>= 4;
            } while (v);
            s = tmp + i;
            break;
        case 'c':
            tmp[0] = (char)__builtin_va_arg(ap, int);
            tmp[1] = 0;
            break;
        case 's':
            s = __builtin_va_arg(ap, const char *);
            if (s == 0) s = "(null)";
            break;
        case '%':
            tmp[0] = '%';
            tmp[1] = 0;
            break;
        case 0:
            fmt--;
            tmp[0] = 0;
            break;
        default:
            tmp[0] = c;
            tmp[1] = 0;
            break;
        }
        while (s[len] && (c != 's' || prec < 0 || len < prec)) len++;
        if (neg) width--;
        if (!left && !(zero && c != 's'))
            for (i = len; i < width; i++) pos = fmt_out(buf, cap, pos, ' ');
        if (neg) pos = fmt_out(buf, cap, pos, '-');
        if (!left && zero && c != 's')
            for (i = len; i < width; i++) pos = fmt_out(buf, cap, pos, '0');
        for (i = 0; i < len; i++) pos = fmt_out(buf, cap, pos, s[i]);
        if (left)
            for (i = len; i < width; i++) pos = fmt_out(buf, cap, pos, ' ');
    }
    if (cap > 0) buf[pos < cap ? pos : cap - 1] = 0;
    return pos;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list_t ap;
    int n;

    __builtin_va_start(ap, fmt);
    n = port_vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    char buf[512];
    va_list_t ap;
    int n;

    __builtin_va_start(ap, fmt);
    n = port_vsnprintf(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    port_log("[game] %s", buf);
    return n;
}
