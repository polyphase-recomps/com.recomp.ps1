/*
 * PsyQ libcd streaming (StSetRing / StSetStream / StGetNext / StFreeRing ...) on the
 * disc image, for games that play STR movies with their own player (libcd + libpress).
 *
 * CdRead2 with CdlModeStream starts the stream at the CdlSetloc position (libetc.c
 * calls port_cd_stream_*). Sectors "arrive" at the drive's speed (75 or 150 a second)
 * counted in vblanks; StGetNext reads what has arrived: video sectors are put
 * together into frames, XA audio sectors are decoded and queued for the mixer
 * (libsnd.c port_cd_audio_queue) unless the CD audio is muted. One frame is handed out
 * at a time, from a buffer of this file rather than the game's ring; StFreeRing
 * releases it. A game that spins on StGetNext waiting for the drive (as on the PS1)
 * waits a vblank every so often, so time passes and no CPU is burnt.
 */
#include <port_host.h>

#define RAW_SECTOR 2352
#define VIDEO_PAYLOAD 2016
#define MAX_FRAME (192 * 1024)
#define POLLS_PER_VBLANK 32
#define END_GAP 256 /* sectors without video before the stream reports its end */

#define CdlModeSpeed 0x80
#define CdlModeSF 0x08

void port_cd_audio_queue(const short *samples, int frames);
void port_cd_audio_clear(void);

typedef struct
{
    int s1[2], s2[2];
    double phase;
} XaState;

static struct
{
    int active;
    unsigned lba;
    unsigned startVblank;
    unsigned long consumed; /* sectors read since the start */
    int speed2x;
    unsigned long startFrame, endFrame;
    /* the frame being put together / handed out */
    unsigned char *frame;
    unsigned long header[8];
    int number, chunks, have;
    int ready, held;
    int gap;
    /* polling */
    unsigned lastPollVblank;
    int polls;
    XaState xa;
} sSt;

static int sMuted;
static int sFilterOn, sFilterFile, sFilterChan;

/* ---- XA ADPCM --------------------------------------------------------------------------- */
static const int kXaF0[4] = {0, 60, 115, 98};
static const int kXaF1[4] = {0, 0, -52, -55};

#define XA_OUT_FRAMES 9472

static void decode_xa(XaState *st, const unsigned char *data, int stereo, int rate)
{
    static short pcm[18 * 8 * 28];
    /* one sector resampled to 44100 Hz: up to 4032 mono samples at 18900 Hz = 9408 frames */
    static short out[XA_OUT_FRAMES * 2];
    int count, g, u, s, outFrames = 0;
    double step = (double)rate / 44100.0;

    for (g = 0; g < 18; g++)
    {
        const unsigned char *grp = data + g * 128;

        for (u = 0; u < 8; u++)
        {
            int param = grp[4 + u], shift = param & 15, filter = (param >> 4) & 3;
            int ch = stereo ? (u & 1) : 0;

            if (shift > 12) shift = 9;
            for (s = 0; s < 28; s++)
            {
                int nib = (grp[16 + s * 4 + u / 2] >> ((u & 1) * 4)) & 15;
                int v = (short)(nib << 12) >> shift;

                v += (st->s1[ch] * kXaF0[filter] + st->s2[ch] * kXaF1[filter] + 32) >> 6;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                st->s2[ch] = st->s1[ch];
                st->s1[ch] = v;
                if (stereo)
                    pcm[((g * 4 + u / 2) * 28 + s) * 2 + ch] = (short)v;
                else
                    pcm[(g * 8 + u) * 28 + s] = (short)v;
            }
        }
    }
    count = stereo ? 18 * 4 * 28 : 18 * 8 * 28;
    while (st->phase < count && outFrames < XA_OUT_FRAMES)
    {
        int i = (int)st->phase;

        if (stereo)
        {
            out[outFrames * 2] = pcm[i * 2];
            out[outFrames * 2 + 1] = pcm[i * 2 + 1];
        }
        else
        {
            out[outFrames * 2] = out[outFrames * 2 + 1] = pcm[i];
        }
        outFrames++;
        st->phase += step;
    }
    st->phase -= count;
    port_cd_audio_queue(out, outFrames);
}

/* ---- the drive ---------------------------------------------------------------------------- */
static void reset_frame(void)
{
    sSt.number = -1;
    sSt.have = 0;
    sSt.ready = 0;
    sSt.held = 0;
}

void port_cd_stream_start(unsigned lba, int mode)
{
    int i;

    if (!sSt.frame) sSt.frame = (unsigned char *)port_alloc(MAX_FRAME);
    sSt.active = 1;
    sSt.lba = lba;
    sSt.startVblank = port_vblank_count();
    sSt.consumed = 0;
    sSt.speed2x = (mode & CdlModeSpeed) != 0;
    sSt.gap = 0;
    sSt.polls = 0;
    for (i = 0; i < (int)sizeof sSt.xa; i++) ((char *)&sSt.xa)[i] = 0;
    reset_frame();
}

void port_cd_stream_stop(void)
{
    if (sSt.active) port_cd_audio_clear();
    sSt.active = 0;
    reset_frame();
}

void port_cd_set_mute(int muted)
{
    sMuted = muted;
    if (muted) port_cd_audio_clear();
}

void port_cd_set_filter(int on, int file, int chan)
{
    sFilterOn = on;
    sFilterFile = file;
    sFilterChan = chan;
}

static unsigned long arrived(void)
{
    unsigned elapsed = port_vblank_count() - sSt.startVblank;

    /* 75 sectors a second at single speed, 60 vblanks a second; a few in the drive's buffer */
    return (unsigned long)elapsed * (sSt.speed2x ? 150 : 75) / 60 + 8;
}

/* Reads arrived sectors until a frame is complete. Returns 1 when one is ready. */
static int pump(void)
{
    static unsigned char sector[RAW_SECTOR];
    unsigned long until = arrived();

    while (!sSt.ready && sSt.consumed < until)
    {
        const unsigned char *sub, *data;

        if (!port_disc_read_raw(sSt.lba, sector)) break;
        sSt.lba++;
        sSt.consumed++;
        sub = sector + 16;
        data = sector + 24;
        if (sub[2] & 4)
        {
            /* XA audio (form 2): the channel the filter selects, while not muted */
            if (!sMuted && (!sFilterOn || (sub[0] == sFilterFile && sub[1] == sFilterChan)))
            {
                decode_xa(&sSt.xa, data, sub[3] & 1, (sub[3] & 4) ? 18900 : 37800);
            }
            continue;
        }
        if ((data[0] | (data[1] << 8)) != 0x0160)
        {
            if (++sSt.gap >= END_GAP)
            {
                /* past the movie: report frame 0, which ends a player's stream */
                int k;

                for (k = 0; k < 8; k++) sSt.header[k] = 0;
                for (k = 0; k < 64; k++) sSt.frame[k] = 0;
                sSt.ready = 1;
                sSt.gap = 0;
            }
            continue;
        }
        sSt.gap = 0;
        {
            int chunk = data[4] | (data[5] << 8), chunks = data[6] | (data[7] << 8);
            int number = data[8] | (data[9] << 8) | (data[10] << 16) | (data[11] << 24);
            int k;

            if (number != sSt.number)
            {
                sSt.number = number;
                sSt.have = 0;
                for (k = 0; k < 32; k++) ((unsigned char *)sSt.header)[k] = data[k];
            }
            sSt.chunks = chunks;
            if ((unsigned long)(chunk + 1) * VIDEO_PAYLOAD <= MAX_FRAME)
            {
                for (k = 0; k < VIDEO_PAYLOAD; k++) sSt.frame[chunk * VIDEO_PAYLOAD + k] = data[32 + k];
                sSt.have++;
            }
            if (chunk == chunks - 1)
            {
                if (sSt.have == chunks && (unsigned long)number >= sSt.startFrame &&
                    (sSt.endFrame == 0xFFFFFFFFu || (unsigned long)number <= sSt.endFrame))
                {
                    sSt.ready = 1;
                }
                else
                {
                    sSt.number = -1; /* incomplete or filtered out: drop it */
                }
            }
        }
    }
    return sSt.ready;
}

/* ---- libcd streaming API ------------------------------------------------------------------- */
void StSetRing(unsigned long *ring_addr, unsigned long ring_size)
{
    reset_frame();
}

void StUnSetRing(void)
{
    port_cd_stream_stop();
}

void StClearRing(void)
{
    reset_frame();
}

void StSetStream(unsigned long mode, unsigned long start_frame, unsigned long end_frame, void (*func1)(),
                 void (*func2)())
{
    sSt.startFrame = start_frame;
    sSt.endFrame = end_frame;
    reset_frame();
}

void StSetEmulate(unsigned long *addr, unsigned long mode, unsigned long start_frame, unsigned long end_frame,
                  void (*func1)(), void (*func2)())
{
}

unsigned long StGetNext(unsigned long **addr, unsigned long **header)
{
    unsigned now;

    if (!sSt.active || sSt.held) return 1;
    if (!pump())
    {
        /* the drive has not delivered yet: a game spinning here lets time pass */
        now = port_vblank_count();
        if (now != sSt.lastPollVblank)
        {
            sSt.lastPollVblank = now;
            sSt.polls = 0;
        }
        else if (++sSt.polls >= POLLS_PER_VBLANK)
        {
            sSt.polls = 0;
            sSt.lastPollVblank = port_wait_vblank();
        }
        if (!pump()) return 1;
    }
    sSt.held = 1;
    *addr = (unsigned long *)sSt.frame;
    *header = sSt.header;
    return 0;
}

unsigned long StGetNextS(unsigned long **addr, unsigned long **header)
{
    return StGetNext(addr, header);
}

unsigned long StFreeRing(unsigned long *base)
{
    if (sSt.held)
    {
        sSt.held = 0;
        sSt.ready = 0;
        sSt.number = -1;
        sSt.have = 0;
    }
    return 0;
}

void StRingStatus(short *free_sectors, short *over_sectors)
{
    if (free_sectors) *free_sectors = sSt.held ? 0 : 32;
    if (over_sectors) *over_sectors = 0;
}

int StGetBackloc(void *loc)
{
    return 0;
}

int StSetChannel(unsigned long channel)
{
    return 1;
}

void StSetMask(unsigned long mask, unsigned long start, unsigned long end) {}

void StCdInterrupt(void) {}
