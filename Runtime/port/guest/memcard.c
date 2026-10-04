/*
 * PsyQ libmcrd on host save files. Each card is a folder of the save folder (card0
 * for channel 0x00, card1 for 0x10); each memory card file is a host file whose
 * size is its block count * 8 KB. Card 1 is present, card 2 is empty.
 *
 * Read/write/exist/accept are "asynchronous" like the original: they register a
 * command whose result MemCardSync() reports once.
 */
#include <port_host.h>

#define McFuncExist 1
#define McFuncAccept 2
#define McFuncReadFile 3
#define McFuncWriteFile 4

#define McErrNone 0
#define McErrCardNotExist 1
#define McErrNewCard 3
#define McErrFileNotExist 5
#define McErrAlreadyExist 6
#define McErrBlockFull 7

#define BLOCK_SIZE 8192
#define CARD_BLOCKS 15
#define PATH_MAX_LEN 64

/* PsyQ struct DIRENTRY (sys/file.h). */
struct DIRENTRY
{
    char name[20];
    long attr;
    long size;
    struct DIRENTRY *next;
    long head;
    char system[4];
};

static int sPending;
static unsigned long sPendingCmd;
static unsigned long sPendingResult;

static int card_present(long chan)
{
    return (chan & 0x10) == 0;
}

static const char *card_dir(long chan)
{
    return (chan & 0x10) ? "card1" : "card0";
}

static void card_path(long chan, const char *name, char *out)
{
    const char *dir = card_dir(chan);
    int n = 0;

    while (*dir && n < PATH_MAX_LEN - 2) out[n++] = *dir++;
    out[n++] = '/';
    while (*name && n < PATH_MAX_LEN - 1) out[n++] = *name++;
    out[n] = 0;
}

static int used_blocks(long chan)
{
    char name[PATH_MAX_LEN];
    unsigned i;
    int size, blocks = 0;

    for (i = 0; (size = port_file_list(card_dir(chan), i, name, sizeof name)) >= 0; i++)
    {
        blocks += (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    }
    return blocks;
}

static void post(unsigned long cmd, unsigned long result)
{
    sPending = 1;
    sPendingCmd = cmd;
    sPendingResult = result;
}

void MemCardInit(long flg)
{
    sPending = 0;
}

void MemCardEnd(void) {}
void MemCardStart(void) {}
void MemCardStop(void) {}

long MemCardExist(long chan)
{
    post(McFuncExist, card_present(chan) ? McErrNone : McErrCardNotExist);
    return 1;
}

long MemCardAccept(long chan)
{
    post(McFuncAccept, card_present(chan) ? McErrNone : McErrCardNotExist);
    return 1;
}

long MemCardSync(long mode, unsigned long *cmds, unsigned long *rslt)
{
    if (!sPending) return -1;
    if (cmds) *cmds = sPendingCmd;
    if (rslt) *rslt = sPendingResult;
    sPending = 0;
    return 1;
}

long MemCardReadFile(long chan, char *name, long *adrs, long ofs, long bytes)
{
    char path[PATH_MAX_LEN];
    unsigned char *dst = (unsigned char *)adrs;
    int got;
    long i;

    if (!card_present(chan))
    {
        post(McFuncReadFile, McErrCardNotExist);
        return 1;
    }
    card_path(chan, name, path);
    got = port_file_read(path, (unsigned)ofs, dst, (unsigned)bytes);
    if (got < 0)
    {
        post(McFuncReadFile, McErrFileNotExist);
        return 1;
    }
    for (i = got; i < bytes; i++) dst[i] = 0;
    post(McFuncReadFile, McErrNone);
    return 1;
}

long MemCardWriteFile(long chan, char *name, long *adrs, long ofs, long bytes)
{
    char path[PATH_MAX_LEN];

    if (!card_present(chan))
    {
        post(McFuncWriteFile, McErrCardNotExist);
        return 1;
    }
    card_path(chan, name, path);
    if (port_file_size(path) < 0 || port_file_write(path, (unsigned)ofs, adrs, (unsigned)bytes, 0) < 0)
    {
        post(McFuncWriteFile, McErrFileNotExist);
        return 1;
    }
    post(McFuncWriteFile, McErrNone);
    return 1;
}

long MemCardCreateFile(long chan, char *name, long blocks)
{
    static char zero[BLOCK_SIZE];
    char path[PATH_MAX_LEN];
    long i;

    if (!card_present(chan)) return McErrCardNotExist;
    card_path(chan, name, path);
    if (port_file_size(path) >= 0) return McErrAlreadyExist;
    if (used_blocks(chan) + blocks > CARD_BLOCKS) return McErrBlockFull;
    for (i = 0; i < blocks; i++)
    {
        if (port_file_write(path, (unsigned)(i * BLOCK_SIZE), zero, BLOCK_SIZE, i == 0) < 0) return McErrCardNotExist;
    }
    port_log("memcard: created %s (%d blocks)", path, (int)blocks);
    return McErrNone;
}

long MemCardDeleteFile(long chan, char *name)
{
    char path[PATH_MAX_LEN];

    if (!card_present(chan)) return McErrCardNotExist;
    card_path(chan, name, path);
    if (port_file_delete(path) != 0) return McErrFileNotExist;
    return McErrNone;
}

long MemCardFormat(long chan)
{
    char name[PATH_MAX_LEN], path[PATH_MAX_LEN];

    if (!card_present(chan)) return McErrCardNotExist;
    /* always take the first entry: the list shrinks as files go */
    while (port_file_list(card_dir(chan), 0, name, sizeof name) >= 0)
    {
        card_path(chan, name, path);
        if (port_file_delete(path) != 0) break;
    }
    return McErrNone;
}

long MemCardUnformat(long chan)
{
    return MemCardFormat(chan);
}

static int wild_match(const char *pat, const char *s)
{
    for (; *pat; pat++, s++)
    {
        if (*pat == '*') return 1;
        if (*s == 0) return 0;
        if (*pat != '?' && *pat != *s) return 0;
    }
    return *s == 0;
}

long MemCardGetDirentry(long chan, char *name, struct DIRENTRY *pdir, long *files, long ofs, long max)
{
    char entry[PATH_MAX_LEN];
    unsigned i;
    int size, k;
    long found = 0, index = 0;

    *files = 0;
    if (!card_present(chan)) return McErrCardNotExist;
    for (i = 0; (size = port_file_list(card_dir(chan), i, entry, sizeof entry)) >= 0; i++)
    {
        if (!wild_match(name, entry)) continue;
        if (index++ < ofs) continue;
        if (found >= max) break;
        for (k = 0; k < (int)sizeof(struct DIRENTRY); k++) ((char *)&pdir[found])[k] = 0;
        for (k = 0; k < (int)sizeof(pdir[found].name) - 1 && entry[k]; k++) pdir[found].name[k] = entry[k];
        pdir[found].size = size;
        found++;
    }
    *files = found;
    return McErrNone;
}
