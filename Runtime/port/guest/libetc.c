/* PsyQ libetc (VSync, pads, callbacks) and libcd on the host disc image. */
#include <port_gpu.h>
#include <port_host.h>

/* ---- libetc ------------------------------------------------------------------------------- */
static unsigned sLastVsync;
void port_snd_render(int frames);
static int sPadReadsSinceVsync;

void ResetCallback(void) {}
void StopCallback(void) {}
void PadInit(int mode) {}
void PadStop(void) {}

long VSync(int mode)
{
    unsigned now, target;

    if (mode < 0)
    {
        return (long)port_vblank_count();
    }
    if (mode == 1)
    {
        return 0;
    }
    gpu_present();
    now = port_vblank_count();
    target = sLastVsync + (unsigned)(mode > 1 ? mode : 1);
    if ((int)(target - now) < 1 || (int)(target - now) > mode + 1)
    {
        target = now + 1;
    }
    while ((int)(port_vblank_count() - target) < 0)
    {
        port_wait_vblank();
    }
    sLastVsync = port_vblank_count();
    sPadReadsSinceVsync = 0;
    {
        /* audio follows the video clock: 735 samples per 60 Hz frame */
        static unsigned long long produced;
        unsigned long long target = (unsigned long long)sLastVsync * 735;

        if (produced + 4096 < target || produced > target) produced = target - 735;
        static int sNoSound = -1;

        if (sNoSound < 0)
        {
            /* --debug-nosnd 1: no audio mixing (profiling) */
            int v[1];

            sNoSound = port_debug_values("nosnd", v, 1) == 1 && v[0];
        }
        if (target > produced)
        {
            if (!sNoSound) port_snd_render((int)(target - produced));
            produced = target;
        }
    }
    return 0;
}

unsigned long PadRead(int id)
{
    /* Busy-wait loops on the pad (pause screen) get paced to the video rate. */
    if (++sPadReadsSinceVsync > 8)
    {
        port_wait_vblank();
        sPadReadsSinceVsync = 0;
    }
    return port_pad_state();
}

/* ---- libcd -------------------------------------------------------------------------------- */
typedef struct
{
    unsigned char minute, second, sector, track;
} CdlLOC;

typedef struct
{
    CdlLOC pos;
    unsigned long size;
    char name[16];
} CdlFILE;

#define CdlSetloc 0x02
#define CdlSetmode 0x0E

static int sLoc;
static unsigned char sMode;

static int bcd(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static int unbcd(int v)
{
    return (v >> 4) * 10 + (v & 15);
}

CdlLOC *CdIntToPos(int i, CdlLOC *p)
{
    i += 150;
    p->minute = (unsigned char)bcd(i / 4500);
    p->second = (unsigned char)bcd((i / 75) % 60);
    p->sector = (unsigned char)bcd(i % 75);
    return p;
}

int CdPosToInt(CdlLOC *p)
{
    return unbcd(p->minute) * 4500 + unbcd(p->second) * 75 + unbcd(p->sector) - 150;
}

int CdInit(void)
{
    return 1;
}

int CdControl(unsigned char com, unsigned char *param, unsigned char *result)
{
    if (com == CdlSetloc && param)
    {
        sLoc = CdPosToInt((CdlLOC *)param);
    }
    else if (com == CdlSetmode && param)
    {
        sMode = param[0];
    }
    if (result) result[0] = 0x02;
    return 1;
}

int CdControlB(unsigned char com, unsigned char *param, unsigned char *result)
{
    return CdControl(com, param, result);
}

int CdControlF(unsigned char com, unsigned char *param)
{
    return CdControl(com, param, 0);
}

int CdRead(int sectors, unsigned long *buf, int mode)
{
    int flag[1];

    if (port_debug_values("cdlog", flag, 1) == 1 && flag[0])
    {
        port_log("CdRead lba %d sectors %d -> %p (vblank %u)", sLoc, sectors, buf, port_vblank_count());
    }
    if (!port_disc_read((unsigned)sLoc, (unsigned)sectors, buf))
    {
        port_log("CdRead: failed at %d (%d sectors)", sLoc, sectors);
    }
    sLoc += sectors;
    return 1;
}

int CdRead2(long mode)
{
    return 1;
}

int CdReadSync(int mode, unsigned char *result)
{
    return 0;
}

int CdSync(int mode, unsigned char *result)
{
    return 2; /* CdlComplete */
}

CdlFILE *CdSearchFile(CdlFILE *fp, char *name)
{
    unsigned lba, size;
    const char *base;
    int i;

    if (!port_disc_find(name, &lba, &size))
    {
        port_log("CdSearchFile: %s not found", name);
        return 0;
    }
    CdIntToPos((int)lba, &fp->pos);
    fp->size = size;
    base = name;
    for (i = 0; name[i]; i++)
        if (name[i] == '\\') base = name + i + 1;
    for (i = 0; i < 15 && base[i]; i++) fp->name[i] = base[i];
    fp->name[i] = 0;
    return fp;
}

void CdInterrupt(void) {}

/* Streaming / MDEC (movies are skipped in the port). */
void StSetRing(unsigned long *ring_addr, unsigned long ring_size) {}
void StUnSetRing(void) {}
void StSetStream(unsigned long mode, unsigned long start_frame, unsigned long end_frame, void (*func1)(), void (*func2)()) {}
void StClearRing(void) {}
unsigned long StFreeRing(unsigned long *base) { return 0; }
unsigned long StGetNext(unsigned long **addr, unsigned long **header) { return 1; }
int StGetBackloc(void *loc) { return 0; }
void StCdInterrupt(void) {}
void DecDCTReset(int mode) {}
void DecDCTin(unsigned long *buf, int mode) {}
void DecDCTout(unsigned long *buf, int size) {}
int DecDCToutSync(int mode) { return 0; }
void *DecDCToutCallback(void (*func)()) { return 0; }
int DecDCTvlc2(unsigned long *bs, unsigned long *buf, void *table) { return 0; }
void DecDCTvlcBuild(void *table) {}
