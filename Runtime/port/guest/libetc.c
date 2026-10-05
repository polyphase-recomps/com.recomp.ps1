/* PsyQ libetc (VSync, pads, callbacks) and libcd on the host disc image. */
#include <port_gpu.h>
#include <port_host.h>

/* ---- libetc ------------------------------------------------------------------------------- */
static unsigned sLastVsync;
void port_snd_render(int frames);
static int sPadReadsSinceVsync;
static void (*sVSyncCallback)(void);
static void cd_vsync(void);

/* Runs `func` at every VSync (the PS1 runs it from the vertical blank interrupt). */
int VSyncCallback(void (*func)(void))
{
    sVSyncCallback = func;
    return 0;
}

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
    cd_vsync();
    if (sVSyncCallback) sVSyncCallback();
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
#define CdlReadN 0x06
#define CdlStop 0x08
#define CdlPause 0x09
#define CdlInit 0x0A
#define CdlMute 0x0B
#define CdlDemute 0x0C
#define CdlSetfilter 0x0D
#define CdlSetmode 0x0E
#define CdlSeekL 0x15
#define CdlSeekP 0x16
#define CdlReadS 0x1B
#define CdlModeStream 0x100
#define CdlModeSF 0x08
#define CdlComplete 0x02

static int sLoc;
static unsigned char sMode;
static int sStreamMode; /* CdlModeStream, which does not fit the mode byte */
static void (*sSyncCallback)(unsigned char, unsigned char *);
static int sSyncPending;

/* libcd_stream.c */
void port_cd_stream_start(unsigned lba, int mode);
void port_cd_stream_stop(void);
void port_cd_set_mute(int muted);
void port_cd_set_filter(int on, int file, int chan);
static unsigned char sFilter[2];

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
    if ((com == CdlSetloc || com == CdlSeekL || com == CdlSeekP) && param)
    {
        sLoc = CdPosToInt((CdlLOC *)param);
    }
    else if (com == CdlSetmode && param)
    {
        sMode = param[0];
        port_cd_set_filter((sMode & CdlModeSF) != 0, sFilter[0], sFilter[1]);
    }
    else if (com == CdlSetfilter && param)
    {
        sFilter[0] = param[0];
        sFilter[1] = param[1];
        port_cd_set_filter((sMode & CdlModeSF) != 0, sFilter[0], sFilter[1]);
    }
    else if (com == CdlMute || com == CdlDemute)
    {
        port_cd_set_mute(com == CdlMute);
    }
    else if (com == CdlPause || com == CdlStop || com == CdlInit)
    {
        port_cd_stream_stop();
    }
    else if ((com == CdlReadS || com == CdlReadN) && sStreamMode)
    {
        port_cd_stream_start((unsigned)sLoc, sMode);
    }
    if (result) result[0] = 0x02;
    /* the command completes at once; CdSyncCallback hears of it at the next VSync */
    if (sSyncCallback) sSyncPending = 1;
    return 1;
}

/* Called from VSync: delivers a pending CdSyncCallback. */
static void cd_vsync(void)
{
    if (sSyncPending && sSyncCallback)
    {
        static unsigned char result[8] = {0x02};

        sSyncPending = 0;
        sSyncCallback(CdlComplete, result);
    }
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

/* Starts reading at the CdlSetloc position in `mode`; with CdlModeStream, a stream
 * that libcd_stream.c serves. */
int CdRead2(long mode)
{
    unsigned char m = (unsigned char)mode;

    sStreamMode = (mode & CdlModeStream) != 0;
    CdControl(CdlSetmode, &m, 0);
    if (sStreamMode) port_cd_stream_start((unsigned)sLoc, (int)mode);
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

/* Streaming (St*) is in libcd_stream.c, the MDEC (DecDCT*) in libpress.c. */

void CdFlush(void) {}

int CdSetDebug(int level)
{
    return 0;
}

void (*CdSyncCallback(void (*func)(unsigned char, unsigned char *)))(unsigned char, unsigned char *)
{
    void (*old)(unsigned char, unsigned char *) = sSyncCallback;

    sSyncCallback = func;
    return old;
}

void (*CdReadyCallback(void (*func)(unsigned char, unsigned char *)))(unsigned char, unsigned char *)
{
    return 0;
}
