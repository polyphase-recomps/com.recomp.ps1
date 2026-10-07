/* PsyQ libetc (VSync, pads, callbacks) and libcd on the host disc image. */
#include <port_gpu.h>
#include <port_host.h>

/* ---- libetc ------------------------------------------------------------------------------- */
static unsigned sLastVsync; /* VSync(n) counts from the last call */
static unsigned sLastWork;  /* the last vblank whose work was done (vblank_work) */
void port_snd_render(int frames);
static int sPadReadsSinceVsync;
static void (*sVSyncCallback)(void);
static void cd_vsync(void);
static void vblank_work(void);
static void pad_buffers(void);

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

#define AUDIO_CATCH_UP 22050 /* stereo frames at 44100 Hz */

/* The vertical blank interrupt's events (root counter 3, RCntCNT3 / EvSpINT), for every vblank
 * since the last call: from VSync, and (recomp mode) from a loop the game spins in waiting for
 * what such a handler sets. After a long stall only the last two are delivered. */
void DeliverEvent(unsigned long desc, long spec);
void port_vblank_interrupts(void)
{
    static unsigned sDelivered;
    static int sBusy;
    const unsigned now = port_vblank_count();

    if (sBusy) return; /* (a handler that waits) */
    sBusy = 1;
    if ((int)(now - sDelivered) > 2) sDelivered = now - 2;
    while ((int)(now - sDelivered) > 0)
    {
        sDelivered++;
        DeliverEvent(0xF2000003u, 0x0002);
    }
    sBusy = 0;
}

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
    vblank_work();
    return 0;
}

/* What the vblank brings (VSync's, after its wait): the interrupts, the drive, the callback, the
 * audio of the frames gone by. */
static void vblank_work(void)
{
    sLastWork = port_vblank_count();
    pad_buffers();
    port_vblank_interrupts();
    cd_vsync();
    if (sVSyncCallback) sVSyncCallback();
    {
        /* audio follows the video clock: 735 samples per 60 Hz frame. A game slower than
         * the clock (a slow host) gets the samples it owes made up, up to half a second;
         * beyond that (a stall, a load) the clock is followed from here. */
        static unsigned long long produced;
        unsigned long long target = (unsigned long long)sLastWork * 735;

        if (produced + AUDIO_CATCH_UP < target || produced > target) produced = target - 735;
        static int sNoSound = -1;

        if (sNoSound < 0)
        {
            /* --debug-nosnd 1: no audio mixing (profiling) */
            int v[1];

            sNoSound = port_debug_values("nosnd", v, 1) == 1 && v[0];
        }
        while (target > produced)
        {
            /* port_snd_render makes at most 4096 at a time */
            int frames = target - produced > 4096 ? 4096 : (int)(target - produced);

            if (!sNoSound) port_snd_render(frames);
            produced += (unsigned long long)frames;
        }
    }
}

/* Time passed without VSync: a game that waits its own way (a vblank counter of its interrupt
 * handler, recomp mode's loop checks) or the libraries waiting for the drive or the pad. The
 * PS1 shows the frame and plays the sound all the same: what VSync does at a vblank is done for
 * the last one that went by (once each). */
void port_vblank_catch_up(void)
{
    if (port_vblank_count() == sLastWork)
    {
        port_vblank_interrupts();
        return;
    }
    gpu_present();
    vblank_work();
}

unsigned long PadRead(int id)
{
    /* Busy-wait loops on the pad (pause screen) get paced to the video rate. */
    if (++sPadReadsSinceVsync > 8)
    {
        port_wait_vblank();
        sPadReadsSinceVsync = 0;
        port_vblank_catch_up();
    }
    return port_pad_state();
}

/* ---- the pads' buffers (BIOS InitPAD, libpad's PadInitDirect): the BIOS fills them at every
 * vblank. Pad 1 is a digital pad (no analog mode, no actuators); pad 2 is not there. */
static unsigned char *sPadBuf[2];
static long sPadLen[2];
static int sPadsOn;

static void pad_buffers(void)
{
    const unsigned v = port_pad_state() & 0xFFFF;
    int i;

    if (!sPadsOn) return;
    for (i = 0; i < 2; i++)
    {
        unsigned char *b = sPadBuf[i];

        if (b == 0) continue;
        if (i == 0)
        {
            b[0] = 0x00; /* received */
            b[1] = 0x41; /* digital pad, 1 halfword */
            b[2] = (unsigned char)~(v >> 8);
            b[3] = (unsigned char)~v;
        }
        else
        {
            b[0] = 0xFF; /* nothing there */
            b[1] = 0;
        }
    }
}

void InitPAD(unsigned char *buf1, long len1, unsigned char *buf2, long len2)
{
    sPadBuf[0] = buf1;
    sPadLen[0] = len1;
    sPadBuf[1] = buf2;
    sPadLen[1] = len2;
    if (buf1 && len1 > 0) buf1[0] = 0xFF;
    if (buf2 && len2 > 0) buf2[0] = 0xFF;
}

int StartPAD(void)
{
    sPadsOn = 1;
    pad_buffers();
    return 1;
}

void StopPAD(void)
{
    sPadsOn = 0;
}

void ChangeClearPAD(long mode) {}

/* libpad, direct mode */
void PadInitDirect(unsigned char *pad1, unsigned char *pad2)
{
    InitPAD(pad1, 34, pad2, 34);
}

int PadStartCom(void)
{
    return StartPAD();
}

void PadStopCom(void)
{
    StopPAD();
}

/* PadStateFindCTP1: a pad that has no extended modes */
int PadGetState(int port)
{
    return (port & 0x0F) == 0 && (port & 0x10) == 0 ? 2 : 0;
}

int PadInfoMode(int port, int term, int offs) { return 0; }
int PadInfoAct(int port, int acno, int term) { return 0; }
int PadInfoComb(int port, int term, int offs) { return 0; }
int PadSetActAlign(int port, unsigned char *data) { return 1; }
void PadSetAct(int port, unsigned char *data, int len) {}
int PadSetMainMode(int socket, int offs, int lock) { return 0; }
int PadChkVsync(void) { return 0; }

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
void port_cd_stream_tick(void);
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
    /* a CD stream keeps reading (and playing its XA audio) while the game is busy */
    port_cd_stream_tick();
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
