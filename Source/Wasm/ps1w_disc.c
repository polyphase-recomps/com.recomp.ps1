/*
 * port_disc_* for every host (plain stdio), from either
 *  - a disc image: raw 2352-byte .bin, or a 2048-byte .iso, or
 *  - an extracted disc folder (Runtime/tools/extract_disc.py): the disc's files plus
 *    disc.idx, which maps the original sector ranges to them, so games that address
 *    the disc by sector still work and modded files are read in place of the originals.
 * Called from the game thread only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Runtime/port/include/port_host.h"

void host_log(const char *fmt, ...);
void ps1w_disc_close(void);

#define RAW 2352
#define PATH_LEN 260

/* Some SD drivers DMA whole sectors straight into the destination when they can,
 * which needs 32-byte alignment; guest buffers have none, so reads go through this. */
#define BOUNCE_SIZE (64 * 1024)
static unsigned char sBounceRaw[BOUNCE_SIZE + 32];
static unsigned char *sBounce;

/* ---- image ------------------------------------------------------------------------ */
static FILE *sImage;
static int sImageRaw = 1;

/* ---- extracted folder --------------------------------------------------------------- */
typedef struct Extent
{
    unsigned first, count; /* sectors */
    unsigned offset;       /* sector offset into the file (r only) */
    char raw;              /* 'r': 2352-byte sectors, 'd': 2048-byte data */
    char *path;            /* relative to sRoot */
} Extent;

static char sRoot[PATH_LEN];
static Extent *sExtents;
static int sExtentCount;

#define OPEN_FILES 4
static struct
{
    const Extent *extent;
    FILE *f;
    unsigned use;
} sOpen[OPEN_FILES];
static unsigned sUseCounter;

static int read_file(FILE *f, long offset, void *dst, unsigned size)
{
    unsigned char *out = (unsigned char *)dst;

    if (fseek(f, offset, SEEK_SET) != 0) return 0;
    while (size > 0)
    {
        unsigned n = size < BOUNCE_SIZE ? size : BOUNCE_SIZE;
        size_t got = fread(sBounce, 1, n, f);

        /* past the end of a (modded, shorter) file: zeros, like empty sectors */
        if (got < n) memset(sBounce + got, 0, n - got);
        memcpy(out, sBounce, n);
        out += n;
        size -= n;
    }
    return 1;
}

static void close_all(void)
{
    int i;

    for (i = 0; i < OPEN_FILES; i++)
    {
        if (sOpen[i].f) fclose(sOpen[i].f);
        sOpen[i].f = NULL;
        sOpen[i].extent = NULL;
    }
}

static FILE *open_extent(const Extent *e)
{
    char path[PATH_LEN * 2];
    int i, slot = 0;

    for (i = 0; i < OPEN_FILES; i++)
    {
        if (sOpen[i].f && sOpen[i].extent->path == e->path)
        {
            sOpen[i].use = ++sUseCounter;
            return sOpen[i].f;
        }
        if (sOpen[i].use < sOpen[slot].use) slot = i;
    }
    if (sOpen[slot].f) fclose(sOpen[slot].f);
    snprintf(path, sizeof(path), "%s%s", sRoot, e->path);
    sOpen[slot].f = fopen(path, "rb");
    sOpen[slot].extent = e;
    sOpen[slot].use = ++sUseCounter;
    if (sOpen[slot].f == NULL) host_log("disc: cannot open %s", path);
    return sOpen[slot].f;
}

static const Extent *find_extent(unsigned lba)
{
    int lo = 0, hi = sExtentCount - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        const Extent *e = &sExtents[mid];

        if (lba < e->first) hi = mid - 1;
        else if (lba >= e->first + e->count) lo = mid + 1;
        else return e;
    }
    return NULL;
}

static int compare_extents(const void *a, const void *b)
{
    const Extent *x = (const Extent *)a, *y = (const Extent *)b;

    return x->first < y->first ? -1 : x->first > y->first;
}

static int open_folder(const char *dir)
{
    char path[PATH_LEN * 2], line[PATH_LEN + 64];
    FILE *f;
    int cap = 0;
    size_t n = strlen(dir);

    snprintf(sRoot, sizeof(sRoot), "%s%s", dir, (n && dir[n - 1] != '/' && dir[n - 1] != '\\') ? "/" : "");
    snprintf(path, sizeof(path), "%sdisc.idx", sRoot);
    f = fopen(path, "r");
    if (f == NULL) return 0;
    while (fgets(line, sizeof(line), f))
    {
        unsigned first, count;
        char mode, name[PATH_LEN], *at, *end;

        if (line[0] == '#' || sscanf(line, "%u %u %c %259[^\r\n]", &first, &count, &mode, name) != 4) continue;
        if (sExtentCount == cap)
        {
            cap = cap ? cap * 2 : 256;
            sExtents = (Extent *)realloc(sExtents, cap * sizeof(Extent));
        }
        sExtents[sExtentCount].first = first;
        sExtents[sExtentCount].count = count;
        sExtents[sExtentCount].raw = mode == 'r';
        sExtents[sExtentCount].offset = 0;
        at = strrchr(name, '@');
        if (at)
        {
            sExtents[sExtentCount].offset = (unsigned)strtoul(at + 1, &end, 10);
            *at = 0;
        }
        /* extents of one file share its path string (open_extent compares pointers) */
        {
            int k;
            char *shared = NULL;

            for (k = 0; k < sExtentCount && !shared; k++)
            {
                if (strcmp(sExtents[k].path, name) == 0) shared = sExtents[k].path;
            }
            if (!shared)
            {
                shared = (char *)malloc(strlen(name) + 1);
                strcpy(shared, name);
            }
            sExtents[sExtentCount].path = shared;
        }
        sExtentCount++;
    }
    fclose(f);
    qsort(sExtents, sExtentCount, sizeof(Extent), compare_extents);
    host_log("disc: %s (extracted, %d extents)", sRoot, sExtentCount);
    return sExtentCount > 0;
}

/* ---- opening ------------------------------------------------------------------------- */
int ps1w_disc_open(const char *path)
{
    char idx[PATH_LEN * 2];
    FILE *probe;
    long size;
    size_t n = strlen(path);

    if (sBounce == NULL) sBounce = (unsigned char *)(((size_t)sBounceRaw + 31) & ~(size_t)31);
    ps1w_disc_close();
    /* a folder (or its disc.idx) means an extracted disc */
    if (n > 8 && strcmp(path + n - 8, "disc.idx") == 0)
    {
        snprintf(idx, sizeof(idx), "%.*s", (int)(n - 8), path);
        return open_folder(idx);
    }
    snprintf(idx, sizeof(idx), "%s%sdisc.idx", path, (n && path[n - 1] != '/' && path[n - 1] != '\\') ? "/" : "");
    probe = fopen(idx, "r");
    if (probe)
    {
        fclose(probe);
        return open_folder(path);
    }
    sImage = fopen(path, "rb");
    if (sImage == NULL) return 0;
    fseek(sImage, 0, SEEK_END);
    size = ftell(sImage);
    sImageRaw = (size % RAW) == 0;
    host_log("disc: %s (%ld bytes, %s sectors)", path, size, sImageRaw ? "2352-byte" : "2048-byte");
    return 1;
}

void ps1w_disc_close(void)
{
    int i;

    if (sImage) fclose(sImage);
    sImage = NULL;
    close_all();
    for (i = 0; i < sExtentCount; i++)
    {
        int k, shared = 0;

        for (k = 0; k < i && !shared; k++) shared = sExtents[k].path == sExtents[i].path;
        if (!shared) free(sExtents[i].path);
    }
    free(sExtents);
    sExtents = NULL;
    sExtentCount = 0;
}

/* ---- reads ------------------------------------------------------------------------------- */
static int read_data_sector(unsigned lba, unsigned char *dst)
{
    const Extent *e;
    FILE *f;

    if (sImage)
    {
        return read_file(sImage, sImageRaw ? (long)lba * RAW + 24 : (long)lba * 2048, dst, 2048);
    }
    e = find_extent(lba);
    if (e == NULL)
    {
        memset(dst, 0, 2048); /* empty sector (not kept by the extraction) */
        return 1;
    }
    f = open_extent(e);
    if (f == NULL) return 0;
    if (e->raw) return read_file(f, (long)(e->offset + lba - e->first) * RAW + 24, dst, 2048);
    return read_file(f, (long)(lba - e->first) * 2048, dst, 2048);
}

int port_disc_read(unsigned lba, unsigned count, void *dst)
{
    unsigned i;

    if (sImage && !sImageRaw) return read_file(sImage, (long)lba * 2048, dst, count * 2048);
    for (i = 0; i < count; i++)
    {
        if (!read_data_sector(lba + i, (unsigned char *)dst + i * 2048)) return 0;
    }
    return 1;
}

int port_disc_read_raw(unsigned lba, void *dst)
{
    static const unsigned char kSync[12] = { 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0 };
    unsigned char *out = (unsigned char *)dst;
    const Extent *e;

    if (sImage) return sImageRaw && read_file(sImage, (long)lba * RAW, dst, RAW);
    e = find_extent(lba);
    if (e && e->raw)
    {
        FILE *f = open_extent(e);

        return f && read_file(f, (long)(e->offset + lba - e->first) * RAW, dst, RAW);
    }
    /* a data sector: Mode 2 Form 1 around it */
    memset(out, 0, RAW);
    memcpy(out, kSync, 12);
    out[15] = 2;
    out[18] = out[22] = 0x08;
    return read_data_sector(lba, out + 24);
}

/* ---- names -------------------------------------------------------------------------------- */
static unsigned rd_le32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static int same_name(const char *a, const char *b, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++)
    {
        char x = a[i], y = b[i];

        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (x != y) return 0;
    }
    return 1;
}

static int find_in_dir(unsigned dir_lba, unsigned dir_size, const char *name, unsigned *lba, unsigned *size, int *is_dir)
{
    unsigned char sector[2048];
    unsigned offset;

    for (offset = 0; offset < dir_size; offset += 2048)
    {
        unsigned pos = 0;

        if (!port_disc_read(dir_lba + offset / 2048, 1, sector)) return 0;
        while (pos < 2048 && sector[pos] != 0)
        {
            unsigned len = sector[pos], name_len = sector[pos + 32], n = 0;
            const char *entry = (const char *)&sector[pos + 33];

            while (n < name_len && entry[n] != ';') n++;
            if (strlen(name) == n && same_name(entry, name, n))
            {
                *lba = rd_le32(&sector[pos + 2]);
                *size = rd_le32(&sector[pos + 10]);
                *is_dir = (sector[pos + 25] & 2) != 0;
                return 1;
            }
            pos += len;
        }
    }
    return 0;
}

static int find_in_index(const char *path, unsigned *lba, unsigned *size)
{
    char want[PATH_LEN];
    int i, n = 0;

    while (*path == '\\' || *path == '/') path++;
    while (path[n] && path[n] != ';' && n < PATH_LEN - 1)
    {
        want[n] = path[n];
        n++;
    }
    want[n] = 0;
    for (i = 0; i < sExtentCount; i++)
    {
        const Extent *e = &sExtents[i];

        if (e->offset == 0 && strlen(e->path) == (size_t)n && same_name(e->path, want, (unsigned)n))
        {
            *lba = e->first;
            *size = e->count * 2048;
            if (!e->raw)
            {
                /* a data file's real size (a mod may have changed it) */
                FILE *f = open_extent(e);

                if (f && fseek(f, 0, SEEK_END) == 0) *size = (unsigned)ftell(f);
            }
            return 1;
        }
    }
    return 0;
}

int port_disc_find(const char *path, unsigned *lba, unsigned *size)
{
    unsigned char pvd[2048];
    unsigned cur_lba, cur_size;
    char part[64];
    const char *p = path;
    int is_dir = 1;

    if (sExtents)
    {
        if (find_in_index(path, lba, size)) return 1;
        host_log("disc: '%s' not found", path);
        return 0;
    }
    if (!port_disc_read(16, 1, pvd)) return 0;
    cur_lba = rd_le32(&pvd[156 + 2]);
    cur_size = rd_le32(&pvd[156 + 10]);
    while (*p)
    {
        int n = 0;

        while (*p == '\\' || *p == '/') p++;
        while (*p && *p != '\\' && *p != '/' && n < (int)sizeof(part) - 1) part[n++] = *p++;
        part[n] = 0;
        if (n == 0) break;
        if (strchr(part, ';')) *strchr(part, ';') = 0;
        if (!is_dir || !find_in_dir(cur_lba, cur_size, part, &cur_lba, &cur_size, &is_dir))
        {
            host_log("disc: '%s' not found (component '%s')", path, part);
            return 0;
        }
    }
    *lba = cur_lba;
    *size = cur_size;
    return 1;
}
