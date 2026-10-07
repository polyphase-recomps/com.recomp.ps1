/*
 * PS1 BIOS kernel pieces for games that use the memory card through the BIOS rather
 * than libmcrd: events (OpenEvent / TestEvent ...), libcard (InitCARD, _card_info,
 * _card_load ...), and the file calls on "bu00:" / "bu10:" (open, read, write, lseek,
 * close, erase, format, firstfile / nextfile).
 *
 * Cards are the folders memcard.c uses (card0 = slot 1, present; card1 = slot 2,
 * empty), so saves made either way are the same files. Card operations finish at
 * once: the event a game waits for is already delivered when the call returns.
 *
 * open/read/write/lseek/close clash with the host C library's names, so they are
 * ps1_open ... here; a game that calls them maps the names in its compile
 * definitions (open=ps1_open read=ps1_read ...; see the LSD package).
 */
#include <port_host.h>

#define CARD_BLOCK_SIZE 8192
#define CARD_BLOCKS 15
#define PATH_CAP 64
#define MAX_FILES 8
#define MAX_EVENTS 32

/* kernel.h */
#define DescSW 0xF4000000u
#define SwCARD (DescSW | 0x01)
#define EvSpIOE 0x0004
#define EvSpTIMOUT 0x0100
#define FREAD 0x0001
#define FWRITE 0x0002
#define FCREAT 0x0200

/* ---- kernel ------------------------------------------------------------------------- */
void SetMem(int megabytes) {}

int EnterCriticalSection(void)
{
    return 1;
}

void ExitCriticalSection(void) {}

typedef struct
{
    unsigned long desc;
    long spec;
    long mode;
    long (*func)(void);
    int open, enabled, fired;
} Event;

#define EV_MD_INTR 0x1000 /* EvMdINTR: the BIOS calls the handler */

static Event sEvents[MAX_EVENTS];

long OpenEvent(unsigned long desc, long spec, long mode, long (*func)())
{
    int i;

    for (i = 0; i < MAX_EVENTS; i++)
    {
        if (!sEvents[i].open)
        {
            sEvents[i].desc = desc;
            sEvents[i].spec = spec;
            sEvents[i].mode = mode;
            sEvents[i].func = (long (*)(void))func;
            sEvents[i].open = 1;
            sEvents[i].enabled = 0;
            sEvents[i].fired = 0;
            return (long)(0xF1000000u | (unsigned long)i);
        }
    }
    return -1;
}

static Event *event(long ev)
{
    unsigned long i = (unsigned long)ev & 0xFFFF;

    return ((unsigned long)ev & 0xFF000000u) == 0xF1000000u && i < MAX_EVENTS && sEvents[i].open ? &sEvents[i] : 0;
}

long CloseEvent(long ev)
{
    Event *e = event(ev);

    if (e) e->open = 0;
    return 1;
}

long EnableEvent(long ev)
{
    Event *e = event(ev);

    if (e) e->enabled = 1;
    return 1;
}

long DisableEvent(long ev)
{
    Event *e = event(ev);

    if (e) e->enabled = 0;
    return 1;
}

long TestEvent(long ev)
{
    Event *e = event(ev);

    if (e && e->fired)
    {
        e->fired = 0;
        return 1;
    }
    return 0;
}

long WaitEvent(long ev)
{
    return TestEvent(ev);
}

void DeliverEvent(unsigned long desc, long spec)
{
    int i;

    for (i = 0; i < MAX_EVENTS; i++)
    {
        if (sEvents[i].open && sEvents[i].enabled && sEvents[i].desc == desc && sEvents[i].spec == spec)
        {
            if ((sEvents[i].mode & EV_MD_INTR) && sEvents[i].func) sEvents[i].func();
            else sEvents[i].fired = 1;
        }
    }
}

/* ---- libcard ------------------------------------------------------------------------- */
static int card_present(long chan)
{
    return (chan & 0x10) == 0;
}

void InitCARD(long padEnable) {}
long StartCARD(void)
{
    return 1;
}
long StopCARD(void)
{
    return 1;
}
void _bu_init(void) {}

long _card_info(long chan)
{
    DeliverEvent(SwCARD, card_present(chan) ? EvSpIOE : EvSpTIMOUT);
    return 1;
}

long _card_load(long chan)
{
    DeliverEvent(SwCARD, card_present(chan) ? EvSpIOE : EvSpTIMOUT);
    return 1;
}

long _card_clear(long chan)
{
    return 1;
}

long _card_auto(long val)
{
    return 1;
}

/* ---- BIOS files ---------------------------------------------------------------------- */
typedef struct
{
    int used;
    char path[PATH_CAP];
    unsigned long pos;
    int size;
} File;

static File sFiles[MAX_FILES];

/* "bu00:NAME" -> "card0/NAME"; 0 for another device or an empty slot */
static int card_path(const char *name, char *out, int needFile)
{
    int n = 0, i;

    if (!name || name[0] != 'b' || name[1] != 'u' || !name[2] || !name[3] || name[4] != ':') return 0;
    if (name[2] != '0') return 0; /* slot 2: no card */
    for (i = 0; "card0/"[i]; i++) out[n++] = "card0/"[i];
    name += 5;
    if (needFile && !*name) return 0;
    while (*name && n < PATH_CAP - 1) out[n++] = *name++;
    out[n] = 0;
    return 1;
}

static int used_blocks(void)
{
    char name[PATH_CAP];
    unsigned i;
    int size, blocks = 0;

    for (i = 0; (size = port_file_list("card0", i, name, sizeof name)) >= 0; i++)
        blocks += (size + CARD_BLOCK_SIZE - 1) / CARD_BLOCK_SIZE;
    return blocks;
}

int ps1_open(const char *name, int mode)
{
    char path[PATH_CAP];
    int fd, size;

    if (!card_path(name, path, 1)) return -1;
    for (fd = 0; fd < MAX_FILES && sFiles[fd].used; fd++)
    {
    }
    if (fd == MAX_FILES) return -1;
    size = port_file_size(path);
    if (mode & FCREAT)
    {
        static char zero[CARD_BLOCK_SIZE];
        int blocks = (mode >> 16) & 0xFFFF, i;

        if (blocks < 1) blocks = 1;
        if (size >= 0 || used_blocks() + blocks > CARD_BLOCKS) return -1;
        for (i = 0; i < blocks; i++)
            if (port_file_write(path, (unsigned)(i * CARD_BLOCK_SIZE), zero, CARD_BLOCK_SIZE, i == 0) < 0) return -1;
        size = blocks * CARD_BLOCK_SIZE;
        port_log("card: created %s (%d blocks)", path, blocks);
    }
    else if (size < 0)
    {
        return -1;
    }
    sFiles[fd].used = 1;
    for (size = 0; path[size]; size++) sFiles[fd].path[size] = path[size];
    sFiles[fd].path[size] = 0;
    sFiles[fd].pos = 0;
    sFiles[fd].size = port_file_size(path);
    return fd;
}

static File *file(int fd)
{
    return fd >= 0 && fd < MAX_FILES && sFiles[fd].used ? &sFiles[fd] : 0;
}

int ps1_read(int fd, void *buf, int n)
{
    File *f = file(fd);
    int got;

    if (!f || n < 0) return -1;
    got = port_file_read(f->path, (unsigned)f->pos, buf, (unsigned)n);
    if (got < 0) return -1;
    f->pos += (unsigned long)got;
    return got;
}

int ps1_write(int fd, const void *buf, int n)
{
    File *f = file(fd);
    int put;

    if (!f || n < 0) return -1;
    /* the file keeps the size it was created with */
    if ((long)f->pos + n > f->size) n = f->size - (int)f->pos;
    if (n <= 0) return 0;
    put = port_file_write(f->path, (unsigned)f->pos, buf, (unsigned)n, 0);
    if (put < 0) return -1;
    f->pos += (unsigned long)put;
    return put;
}

int ps1_lseek(int fd, int offset, int whence)
{
    File *f = file(fd);
    long pos;

    if (!f) return -1;
    pos = whence == 0 ? offset : whence == 1 ? (long)f->pos + offset : (long)f->size + offset;
    if (pos < 0) pos = 0;
    f->pos = (unsigned long)pos;
    return (int)pos;
}

int ps1_close(int fd)
{
    File *f = file(fd);

    if (!f) return -1;
    f->used = 0;
    return fd;
}

long erase(char *name)
{
    char path[PATH_CAP];

    if (!card_path(name, path, 1)) return 0;
    return port_file_delete(path) == 0;
}

long format(char *name)
{
    char entry[PATH_CAP], path[PATH_CAP];
    int n;

    if (!card_path(name, path, 0)) return 0;
    while (port_file_list("card0", 0, entry, sizeof entry) >= 0)
    {
        for (n = 0; "card0/"[n]; n++) path[n] = "card0/"[n];
        for (int i = 0; entry[i] && n < PATH_CAP - 1; i++) path[n++] = entry[i];
        path[n] = 0;
        if (port_file_delete(path) != 0) break;
    }
    return 1;
}

/* struct DIRENTRY (sys/file.h) */
struct DIRENTRY
{
    char name[20];
    long attr;
    long size;
    struct DIRENTRY *next;
    long head;
    char system[4];
};

static char sFindPattern[PATH_CAP];
static unsigned sFindIndex;

static int match(const char *pat, const char *s)
{
    for (; *pat; pat++, s++)
    {
        if (*pat == '*') return 1;
        if (!*s) return 0;
        if (*pat != '?' && *pat != *s) return 0;
    }
    return !*s;
}

struct DIRENTRY *nextfile(struct DIRENTRY *dir)
{
    char entry[PATH_CAP];
    int size, k;

    while ((size = port_file_list("card0", sFindIndex++, entry, sizeof entry)) >= 0)
    {
        if (!match(sFindPattern, entry)) continue;
        for (k = 0; k < (int)sizeof *dir; k++) ((char *)dir)[k] = 0;
        for (k = 0; k < 19 && entry[k]; k++) dir->name[k] = entry[k];
        dir->size = size;
        return dir;
    }
    return 0;
}

struct DIRENTRY *firstfile(char *name, struct DIRENTRY *dir)
{
    char path[PATH_CAP];
    int i;

    if (!card_path(name, path, 0)) return 0;
    for (i = 0; path[6 + i] && i < PATH_CAP - 1; i++) sFindPattern[i] = path[6 + i];
    sFindPattern[i] = 0;
    if (!i) sFindPattern[0] = '*', sFindPattern[1] = 0;
    sFindIndex = 0;
    return nextfile(dir);
}
