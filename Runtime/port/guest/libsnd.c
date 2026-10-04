/*
 * PsyQ libsnd / libspu on a software SPU.
 *
 * VAB banks (header kept by the game, body copied here), 24 SPU voices for the
 * game's own key-ons (SsUtKeyOnV), a separate pool for SEQ music, PS1 ADPCM decoding,
 * the SPU ADSR envelope and a SEQ (Sony MIDI) sequencer. Output is 44100 Hz stereo,
 * generated from VSync() via port_snd_render().
 */
#include <port_host.h>

#define SAMPLE_RATE 44100
#define NUM_VABS 16
#define NUM_SPU_VOICES 24
#define NUM_SEQ_VOICES 24
#define NUM_VOICES (NUM_SPU_VOICES + NUM_SEQ_VOICES)

/* ---- VAB --------------------------------------------------------------------------------- */
typedef struct
{
    unsigned char tones, mvol, prior, mode, mpan, reserved0;
    short attr;
    unsigned long reserved1, reserved2;
} ProgAtr;

typedef struct
{
    unsigned char prior, mode, vol, pan, center, shift, min, max;
    unsigned char vibW, vibT, porW, porT, pbmin, pbmax, reserved1, reserved2;
    unsigned short adsr1, adsr2;
    short prog, vag;
    short reserved[4];
} VagAtr;

typedef struct
{
    int open;
    const unsigned char *header; /* sticky VH in game memory */
    int mvol, mpan;
    const ProgAtr *progs;        /* 128 entries */
    const VagAtr *tones;         /* 16 per used program */
    short progTone[128];         /* first VagAtr index for a program, -1 if none */
    unsigned long vagOffset[256];
    unsigned long vagSize[256];
    unsigned char *body;         /* copied VB */
    unsigned long bodySize;
} Vab;

static Vab sVabs[NUM_VABS];

static int snd_debug(void)
{
    int flag[1];

    return port_debug_values("sndlog", flag, 1) == 1 && flag[0];
}

/* ---- voices ------------------------------------------------------------------------------- */
enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct
{
    int active;
    int keyed;            /* key on and not yet released */
    const unsigned char *data, *end, *loop;
    const unsigned char *block;
    short decoded[28];
    int index;            /* sample index in decoded block */
    int prevSample;
    int s1, s2;
    unsigned long pos;    /* 12-bit fraction */
    unsigned long pitch;  /* 0x1000 = 44100 Hz */
    int volL, volR;       /* 0..0x3FFF */
    int envState, envLevel, envCounter;
    unsigned short adsr1, adsr2;
    /* sequencer bookkeeping */
    int channel, note, seq;
    /* SsUtAutoVol */
    int autoFrom, autoTo, autoTime, autoElapsed, baseL, baseR;
} Voice;

static Voice sVoices[NUM_VOICES];
static int sMasterL = 127, sMasterR = 127;

static const int kFilter0[5] = {0, 60, 115, 98, 122};
static const int kFilter1[5] = {0, 0, -52, -55, -60};

static void decode_block(Voice *v)
{
    const unsigned char *b = v->block;
    int shift = b[0] & 15, filter = (b[0] >> 4) & 7, i;

    if (filter > 4) filter = 4;
    if (shift > 12) shift = 9;
    for (i = 0; i < 28; i++)
    {
        int nibble = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int s = (short)(nibble << 12) >> shift;

        s += (v->s1 * kFilter0[filter] + v->s2 * kFilter1[filter] + 32) >> 6;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        v->s2 = v->s1;
        v->s1 = s;
        v->decoded[i] = (short)s;
    }
    if (b[1] & 4) v->loop = b;
}

/* Moves to the next ADPCM block; returns 0 when the voice ends. */
static int next_block(Voice *v)
{
    const unsigned char *b = v->block;

    if (b[1] & 1)
    {
        if ((b[1] & 2) && v->loop)
        {
            v->block = v->loop;
        }
        else
        {
            return 0;
        }
    }
    else
    {
        v->block = b + 16;
        if (v->block >= v->end) return 0;
    }
    decode_block(v);
    return 1;
}

/* SPU envelope step (rate encoding as on the hardware). */
static void env_apply(Voice *v, int exp, int dec, int shift, int step)
{
    int s = dec ? (-8 + step) : (7 - step);
    int adsrStep = s << (shift < 11 ? 11 - shift : 0);
    int cycles = 1 << (shift > 11 ? shift - 11 : 0);

    if (exp && !dec && v->envLevel > 0x6000) cycles *= 4;
    if (exp && dec) adsrStep = (adsrStep * v->envLevel) >> 15;
    if (++v->envCounter >= cycles)
    {
        v->envCounter = 0;
        v->envLevel += adsrStep;
        if (v->envLevel > 0x7FFF) v->envLevel = 0x7FFF;
        if (v->envLevel < 0) v->envLevel = 0;
    }
}

static void env_tick(Voice *v)
{
    unsigned a1 = v->adsr1, a2 = v->adsr2;

    switch (v->envState)
    {
    case ENV_ATTACK:
        env_apply(v, (a1 >> 15) & 1, 0, (a1 >> 10) & 31, (a1 >> 8) & 3);
        if (v->envLevel >= 0x7FFF)
        {
            v->envState = ENV_DECAY;
            v->envCounter = 0;
        }
        break;
    case ENV_DECAY:
    {
        int sustain = (int)(((a1 & 15) + 1) * 0x800);

        env_apply(v, 1, 1, (a1 >> 4) & 15, 0);
        if (v->envLevel <= sustain)
        {
            v->envState = ENV_SUSTAIN;
            v->envCounter = 0;
        }
        break;
    }
    case ENV_SUSTAIN:
        env_apply(v, (a2 >> 15) & 1, (a2 >> 14) & 1, (a2 >> 8) & 31, (a2 >> 6) & 3);
        break;
    case ENV_RELEASE:
        env_apply(v, (a2 >> 5) & 1, 1, a2 & 31, 0);
        if (v->envLevel <= 0)
        {
            v->active = 0;
            v->envState = ENV_OFF;
        }
        break;
    default:
        break;
    }
}

static void voice_release(Voice *v)
{
    if (v->active && v->envState != ENV_RELEASE)
    {
        v->envState = ENV_RELEASE;
        v->envCounter = 0;
    }
    v->keyed = 0;
}

static unsigned long note_pitch(const VagAtr *t, int note, int fine)
{
    /* 0x1000 at the tone's centre note; shift/fine in 1/128 semitone */
    double semis = (note - t->center) + (fine - t->shift) / 128.0;
    double p = 4096.0;
    int whole = (int)(semis >= 0 ? semis : semis - 1);
    double frac = semis - whole;
    int i;

    if (whole > 0)
        for (i = 0; i < whole; i++) p *= 1.0594630943592953;
    else
        for (i = 0; i < -whole; i++) p /= 1.0594630943592953;
    p *= 1.0 + frac * (0.0594630943592953 + frac * 0.0); /* linear between semitones */
    if (p > 0x3FFF) p = 0x3FFF;
    return (unsigned long)p;
}

static int voice_start(Voice *v, const Vab *vab, const VagAtr *t, int note, int fine, int volL, int volR)
{
    int vag = t->vag;

    if (!vab->body || vag <= 0 || vag > 255 || vab->vagSize[vag] == 0) return 0;
    if (vab->vagOffset[vag] + vab->vagSize[vag] > vab->bodySize) return 0;
    v->data = vab->body + vab->vagOffset[vag];
    v->end = v->data + vab->vagSize[vag];
    v->block = v->data;
    v->loop = 0;
    v->s1 = v->s2 = 0;
    v->pos = 0;
    v->index = 0;
    v->prevSample = 0;
    decode_block(v);
    v->pitch = note_pitch(t, note, fine);
    v->volL = volL;
    v->volR = volR;
    v->baseL = volL;
    v->baseR = volR;
    v->autoTime = 0;
    v->adsr1 = t->adsr1;
    v->adsr2 = t->adsr2;
    v->envState = ENV_ATTACK;
    v->envLevel = 0;
    v->envCounter = 0;
    v->active = 1;
    v->keyed = 1;
    return 1;
}

static int render_voice_sample(Voice *v, int *outL, int *outR)
{
    int s;

    if (!v->active) return 0;
    /* linear interpolation between the previous and the current ADPCM sample */
    s = v->prevSample + (((v->decoded[v->index] - v->prevSample) * (int)(v->pos & 0xFFF)) >> 12);
    env_tick(v);
    if (!v->active) return 0;
    s = (s * v->envLevel) >> 15;
    *outL += (s * v->volL) >> 14;
    *outR += (s * v->volR) >> 14;
    v->pos += v->pitch;
    while (v->pos >= 0x1000)
    {
        v->pos -= 0x1000;
        v->prevSample = v->decoded[v->index];
        if (++v->index >= 28)
        {
            v->index = 0;
            if (!next_block(v))
            {
                v->active = 0;
                v->envState = ENV_OFF;
                v->keyed = 0;
                break;
            }
        }
    }
    return 1;
}

/* ---- tone helpers -------------------------------------------------------------------------- */
static const VagAtr *vab_tone(const Vab *vab, int prog, int tone)
{
    int first;

    if (prog < 0 || prog > 127) return 0;
    first = vab->progTone[prog];
    if (first < 0 || tone < 0 || tone >= vab->progs[prog].tones || tone >= 16) return 0;
    return &vab->tones[first + tone];
}

static void tone_volume(const Vab *vab, int prog, const VagAtr *t, int vol127, int pan127, int *l, int *r)
{
    /* everything in 0..127; result in 0..0x3FFF */
    long v = (long)vol127 * t->vol / 127 * vab->progs[prog].mvol / 127 * vab->mvol / 127;
    int pan = pan127 + (t->pan - 64) + (vab->progs[prog].mpan - 64);
    long left, right;

    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    left = pan <= 64 ? v : v * (127 - pan) / 63;
    right = pan >= 64 ? v : v * pan / 64;
    *l = (int)(left * 0x3FFF / 127);
    *r = (int)(right * 0x3FFF / 127);
}

/* ---- libsnd: VAB ------------------------------------------------------------------------------ */
void SsInit(void) {}
void SsEnd(void) {}
void SsQuit(void) {}
void SsStart(void) {}
void SsSetTableSize(char *table, short s_max, short t_max) {}
void SsSetTickMode(long tick_mode) {}

void SsSetMVol(short voll, short volr)
{
    sMasterL = voll;
    sMasterR = volr;
}

short SsVabOpenHeadSticky(unsigned char *addr, short vabid, unsigned long sbaddr)
{
    Vab *vab;
    int ps, i, used;
    unsigned long off;
    const unsigned short *sizes;

    if (vabid < 0 || vabid >= NUM_VABS) return -1;
    if (addr[0] != 'p' || addr[1] != 'B' || addr[2] != 'A' || addr[3] != 'V') return -1;
    vab = &sVabs[vabid];
    vab->open = 1;
    vab->header = addr;
    if (snd_debug()) port_log("SsVabOpenHeadSticky %d ps %d", vabid, *(const unsigned short *)(addr + 18));
    ps = *(const unsigned short *)(addr + 18);
    vab->mvol = addr[24];
    vab->mpan = addr[25];
    vab->progs = (const ProgAtr *)(addr + 32);
    vab->tones = (const VagAtr *)(addr + 32 + 128 * 16);
    used = 0;
    for (i = 0; i < 128; i++)
    {
        if (vab->progs[i].tones > 0 && used < ps)
        {
            vab->progTone[i] = (short)(used * 16);
            used++;
        }
        else
        {
            vab->progTone[i] = -1;
        }
    }
    sizes = (const unsigned short *)(addr + 32 + 128 * 16 + ps * 16 * 32);
    off = 0;
    for (i = 0; i < 256; i++)
    {
        vab->vagSize[i] = (unsigned long)sizes[i] << 3;
        vab->vagOffset[i] = off;
        off += vab->vagSize[i];
    }
    /* entry 0 is unused: VAG 1 starts at offset 0 */
    off = 0;
    for (i = 1; i < 256; i++)
    {
        vab->vagOffset[i] = off;
        off += vab->vagSize[i];
    }
    vab->bodySize = off;
    return vabid;
}

short SsVabTransBody(unsigned char *addr, short vabid)
{
    Vab *vab;
    unsigned long i;

    if (vabid < 0 || vabid >= NUM_VABS || !sVabs[vabid].open) return -1;
    vab = &sVabs[vabid];
    if (vab->body == 0 || vab->bodySize > 0)
    {
        /* banks are reloaded often; keep one buffer per slot, grown as needed */
        static unsigned long capacity[NUM_VABS];

        if (capacity[vabid] < vab->bodySize)
        {
            vab->body = (unsigned char *)port_alloc(vab->bodySize);
            capacity[vabid] = vab->bodySize;
        }
    }
    for (i = 0; i < vab->bodySize; i++) vab->body[i] = addr[i];
    return vabid;
}

short SsVabTransCompleted(short immediateFlag)
{
    return 1;
}

void SsVabClose(short vabid)
{
    int i;

    if (vabid < 0 || vabid >= NUM_VABS) return;
    for (i = 0; i < NUM_VOICES; i++)
    {
        if (sVoices[i].active && sVoices[i].data >= sVabs[vabid].body &&
            sVoices[i].data < sVabs[vabid].body + sVabs[vabid].bodySize)
        {
            sVoices[i].active = 0;
        }
    }
    sVabs[vabid].open = 0;
}

unsigned long SsUtGetVBaddrInSB(short vabid)
{
    return 0x1010 + (unsigned long)vabid * 0x8000;
}

/* ---- libsnd: direct voices ------------------------------------------------------------------ */
short SsUtKeyOnV(short voice, short vabId, short prog, short tone, short note, short fine, short voll, short volr)
{
    const VagAtr *t;
    int l, r;

    if (voice < 0 || voice >= NUM_SPU_VOICES || vabId < 0 || vabId >= NUM_VABS || !sVabs[vabId].open) return -1;
    t = vab_tone(&sVabs[vabId], prog, tone);
    if (!t) return -1;
    tone_volume(&sVabs[vabId], prog, t, 127, 64, &l, &r);
    l = l * voll / 127;
    r = r * volr / 127;
    sVoices[voice].seq = 0;
    return voice_start(&sVoices[voice], &sVabs[vabId], t, note, fine, l, r) ? voice : -1;
}

short SsUtKeyOffV(short voice)
{
    if (voice < 0 || voice >= NUM_SPU_VOICES) return -1;
    voice_release(&sVoices[voice]);
    return 0;
}

short SsUtAutoVol(short vc, short start_vol, short end_vol, short delta_time)
{
    Voice *v;

    if (vc < 0 || vc >= NUM_SPU_VOICES) return -1;
    v = &sVoices[vc];
    v->autoFrom = start_vol;
    v->autoTo = end_vol;
    v->autoTime = delta_time > 0 ? delta_time * (SAMPLE_RATE / 60) : 1;
    v->autoElapsed = 0;
    return 0;
}

void SsUtAllKeyOff(short mode)
{
    int i;

    for (i = 0; i < NUM_VOICES; i++)
    {
        sVoices[i].active = 0;
        sVoices[i].keyed = 0;
        sVoices[i].envState = ENV_OFF;
    }
}

void SsUtReverbOn(void) {}
void SsUtReverbOff(void) {}
void SsUtSetReverbType(short type) {}
void SsUtSetReverbDepth(short ldepth, short rdepth) {}

long SpuGetKeyStatus(unsigned long voice_bit)
{
    int i;

    for (i = 0; i < NUM_SPU_VOICES; i++)
    {
        if (voice_bit & (1ul << i))
        {
            Voice *v = &sVoices[i];

            if (!v->active) return 0;                      /* SPU_OFF */
            return v->keyed ? 1 : 2;                       /* SPU_ON / SPU_OFF_ENV_ON */
        }
    }
    return 0;
}

/* ---- reverb: a small Schroeder network standing in for the SPU's room presets ---- */
#define COMBS 4
#define ALLPASSES 2
static const int kCombLen[2][COMBS] = {{1557, 1617, 1491, 1422}, {1580, 1640, 1514, 1445}};
static const int kAllpassLen[2][ALLPASSES] = {{556, 441}, {579, 464}};
static float sCombBuf[2][COMBS][1700];
static float sAllpassBuf[2][ALLPASSES][600];
static int sCombPos[2][COMBS], sAllpassPos[2][ALLPASSES];
static float sCombStore[2][COMBS];
static int sReverbOn;
static int sReverbDepth = 0x7000;

static void reverb_process(int *l, int *r)
{
    float in = (float)(*l + *r) * 0.015f;
    int ch, i;

    for (ch = 0; ch < 2; ch++)
    {
        float out = 0;

        for (i = 0; i < COMBS; i++)
        {
            float *buf = sCombBuf[ch][i];
            int p = sCombPos[ch][i];
            float y = buf[p];

            sCombStore[ch][i] = y * 0.8f + sCombStore[ch][i] * 0.2f;
            buf[p] = in + sCombStore[ch][i] * 0.78f;
            if (++p >= kCombLen[ch][i]) p = 0;
            sCombPos[ch][i] = p;
            out += y;
        }
        for (i = 0; i < ALLPASSES; i++)
        {
            float *buf = sAllpassBuf[ch][i];
            int p = sAllpassPos[ch][i];
            float b = buf[p];

            buf[p] = out + b * 0.5f;
            out = b - out;
            if (++p >= kAllpassLen[ch][i]) p = 0;
            sAllpassPos[ch][i] = p;
        }
        if (ch == 0)
            *l += (int)(out * (float)sReverbDepth / 32768.0f);
        else
            *r += (int)(out * (float)sReverbDepth / 32768.0f);
    }
}

long SpuClearReverbWorkArea(long mode)
{
    int c, i, k;

    for (c = 0; c < 2; c++)
    {
        for (i = 0; i < COMBS; i++)
        {
            for (k = 0; k < 1700; k++) sCombBuf[c][i][k] = 0;
            sCombStore[c][i] = 0;
        }
        for (i = 0; i < ALLPASSES; i++)
            for (k = 0; k < 600; k++) sAllpassBuf[c][i][k] = 0;
    }
    return 1;
}

long SpuSetReverb(long on_off)
{
    sReverbOn = on_off != 0;
    return on_off;
}

void SpuSetReverbDepth(void *attr) {}
void SpuSetReverbModeParam(void *attr) {}
unsigned long SpuSetReverbVoice(long on_off, unsigned long voice_bit) { return voice_bit; }

/* ---- SEQ sequencer ------------------------------------------------------------------------------ */
typedef struct
{
    int open, playing;
    short vab;
    const unsigned char *data, *start, *pos;
    const unsigned char *loopPos;
    int loopCount;          /* remaining NRPN loops, -1 infinite */
    int playLoops;          /* SsSeqPlay count; 0 = infinite */
    unsigned long resolution, tempo;
    double tickAccum;
    long wait;              /* ticks until the next event */
    unsigned char running;
    int volL, volR;
    unsigned char program[16], volume[16], pan[16], expression[16];
    short bend[16];
    int nrpn;
} Seq;

#define NUM_SEQS 4
static Seq sSeqs[NUM_SEQS];

static unsigned long read_var(const unsigned char **p)
{
    unsigned long v = 0;
    int i;

    for (i = 0; i < 4; i++)
    {
        unsigned char b = *(*p)++;

        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

static void seq_reset_channels(Seq *s)
{
    int c;

    for (c = 0; c < 16; c++)
    {
        s->program[c] = 0;
        s->volume[c] = 127;
        s->pan[c] = 64;
        s->expression[c] = 127;
        s->bend[c] = 0;
    }
    s->running = 0;
    s->nrpn = 0;
}

static void seq_notes_off(int seqIndex, int channel, int note, int all)
{
    int i;

    for (i = NUM_SPU_VOICES; i < NUM_VOICES; i++)
    {
        Voice *v = &sVoices[i];

        if (v->active && v->seq == seqIndex + 1 && (all || (v->channel == channel && v->note == note && v->keyed)))
        {
            if (all == 2)
            {
                v->active = 0;
                v->keyed = 0;
            }
            else
            {
                voice_release(v);
            }
        }
    }
}

static Voice *seq_alloc_voice(void)
{
    int i, best = -1, bestLevel = 0x10000;

    for (i = NUM_SPU_VOICES; i < NUM_VOICES; i++)
    {
        if (!sVoices[i].active) return &sVoices[i];
    }
    /* steal the quietest released voice, else the quietest */
    for (i = NUM_SPU_VOICES; i < NUM_VOICES; i++)
    {
        int level = sVoices[i].envLevel + (sVoices[i].keyed ? 0x8000 : 0);

        if (level < bestLevel)
        {
            bestLevel = level;
            best = i;
        }
    }
    return best >= 0 ? &sVoices[best] : 0;
}

static void seq_note_on(int seqIndex, Seq *s, int channel, int note, int velocity)
{
    const Vab *vab;
    int prog = s->program[channel], t, count;

    if (s->vab < 0 || s->vab >= NUM_VABS || !sVabs[s->vab].open) return;
    vab = &sVabs[s->vab];
    if (vab->progTone[prog] < 0) return;
    count = vab->progs[prog].tones;
    for (t = 0; t < count && t < 16; t++)
    {
        const VagAtr *tone = &vab->tones[vab->progTone[prog] + t];
        Voice *v;
        int l, r, vol;

        if (note < tone->min || note > tone->max) continue;
        v = seq_alloc_voice();
        if (!v) return;
        vol = velocity * s->volume[channel] / 127 * s->expression[channel] / 127;
        tone_volume(vab, prog, tone, vol, s->pan[channel], &l, &r);
        l = l * s->volL / 127;
        r = r * s->volR / 127;
        if (voice_start(v, vab, tone, note, 0, l, r))
        {
            v->seq = seqIndex + 1;
            v->channel = channel;
            v->note = note;
            if (s->bend[channel])
            {
                int range = s->bend[channel] > 0 ? tone->pbmax : tone->pbmin;

                v->pitch = note_pitch(tone, note, s->bend[channel] * (range ? range : 2) * 128 / 8192);
            }
        }
    }
}

/* Runs events until the sequence waits; returns 0 when it ended. */
static int seq_events(int seqIndex, Seq *s)
{
    int guard = 0;

    while (s->wait <= 0 && guard++ < 10000)
    {
        unsigned char status = *s->pos;
        int channel;

        if (status & 0x80)
        {
            s->pos++;
            if (status < 0xF0) s->running = status;
        }
        else
        {
            status = s->running;
        }
        channel = status & 15;
        switch (status & 0xF0)
        {
        case 0x80:
        {
            int note = *s->pos++;

            s->pos++;
            seq_notes_off(seqIndex, channel, note, 0);
            break;
        }
        case 0x90:
        {
            int note = *s->pos++, vel = *s->pos++;

            if (vel) seq_note_on(seqIndex, s, channel, note, vel);
            else seq_notes_off(seqIndex, channel, note, 0);
            break;
        }
        case 0xA0:
            s->pos += 2;
            break;
        case 0xB0:
        {
            int cc = *s->pos++, val = *s->pos++;

            if (cc == 7) s->volume[channel] = (unsigned char)val;
            else if (cc == 10) s->pan[channel] = (unsigned char)val;
            else if (cc == 11) s->expression[channel] = (unsigned char)val;
            else if (cc == 99)
            {
                s->nrpn = val;
                if (val == 20)
                {
                    s->loopPos = 0; /* set after the delta time below */
                    s->loopCount = -2;
                }
                else if (val == 30 && s->loopPos)
                {
                    if (s->loopCount == -1 || s->loopCount > 0)
                    {
                        if (s->loopCount > 0) s->loopCount--;
                        s->pos = s->loopPos;
                        s->wait = 0;
                        continue;
                    }
                }
            }
            else if (cc == 6 && s->nrpn == 20)
            {
                s->loopCount = (val == 0 || val == 127) ? -1 : val;
            }
            break;
        }
        case 0xC0:
            s->program[channel] = *s->pos++;
            break;
        case 0xD0:
            s->pos++;
            break;
        case 0xE0:
        {
            int lo = *s->pos++, hi = *s->pos++;

            s->bend[channel] = (short)(((hi << 7) | lo) - 8192);
            break;
        }
        default:
            if (status == 0xFF)
            {
                int type = *s->pos++;

                if (type == 0x2F)
                {
                    return 0;
                }
                if (type == 0x51)
                {
                    s->pos++; /* length 3 */
                    s->tempo = ((unsigned long)s->pos[0] << 16) | ((unsigned long)s->pos[1] << 8) | s->pos[2];
                    s->pos += 3;
                }
                else
                {
                    unsigned long len = read_var(&s->pos);

                    s->pos += len;
                }
            }
            else
            {
                return 0; /* unknown */
            }
            break;
        }
        s->wait = (long)read_var(&s->pos);
        if (s->loopCount == -2)
        {
            /* the loop starts at the event after the NRPN 20 marker */
            s->loopPos = s->pos - 0;
            s->loopCount = -1;
        }
    }
    return 1;
}

static void seq_restart(Seq *s)
{
    s->pos = s->start;
    s->wait = (long)read_var(&s->pos);
    s->loopPos = 0;
    s->loopCount = -1;
}

short SsSeqOpen(unsigned long *addr, short vab_id)
{
    const unsigned char *p = (const unsigned char *)addr;
    int i;
    Seq *s = 0;

    for (i = 0; i < NUM_SEQS; i++)
    {
        if (!sSeqs[i].open)
        {
            s = &sSeqs[i];
            break;
        }
    }
    if (!s) return -1;
    if (p[0] != 'p' || p[1] != 'Q' || p[2] != 'E' || p[3] != 'S')
    {
        port_log("SsSeqOpen: not a SEQ (%02X %02X %02X %02X)", p[0], p[1], p[2], p[3]);
        return -1;
    }
    s->open = 1;
    s->playing = 0;
    s->vab = vab_id;
    if (snd_debug()) port_log("SsSeqOpen %d vab %d res %u tempo %u", i, vab_id, ((unsigned)p[8] << 8) | p[9],
             ((unsigned)p[10] << 16) | ((unsigned)p[11] << 8) | p[12]);
    s->data = p;
    s->resolution = ((unsigned long)p[8] << 8) | p[9];
    s->tempo = ((unsigned long)p[10] << 16) | ((unsigned long)p[11] << 8) | p[12];
    if (s->resolution == 0) s->resolution = 480;
    if (s->tempo == 0) s->tempo = 500000;
    s->start = p + 15;
    s->volL = s->volR = 127;
    s->tickAccum = 0;
    seq_reset_channels(s);
    seq_restart(s);
    return (short)i;
}

void SsSeqClose(short seq_access_num)
{
    if (seq_access_num < 0 || seq_access_num >= NUM_SEQS) return;
    seq_notes_off(seq_access_num, 0, 0, 2);
    sSeqs[seq_access_num].open = 0;
    sSeqs[seq_access_num].playing = 0;
}

void SsSeqPlay(short seq_access_num, char play_mode, short l_count)
{
    Seq *s;

    if (seq_access_num < 0 || seq_access_num >= NUM_SEQS || !sSeqs[seq_access_num].open) return;
    s = &sSeqs[seq_access_num];
    s->playing = play_mode != 0;
    s->playLoops = l_count;
    if (snd_debug()) port_log("SsSeqPlay %d mode %d loops %d", seq_access_num, play_mode, l_count);
}

void SsSeqStop(short seq_access_num)
{
    if (seq_access_num < 0 || seq_access_num >= NUM_SEQS) return;
    sSeqs[seq_access_num].playing = 0;
    seq_notes_off(seq_access_num, 0, 0, 1);
    if (sSeqs[seq_access_num].open)
    {
        seq_reset_channels(&sSeqs[seq_access_num]);
        seq_restart(&sSeqs[seq_access_num]);
    }
}

void SsSeqSetVol(short seq_access_num, short voll, short volr)
{
    if (seq_access_num < 0 || seq_access_num >= NUM_SEQS) return;
    sSeqs[seq_access_num].volL = voll;
    sSeqs[seq_access_num].volR = volr;
}

int port_vsnprintf(char *buf, int cap, const char *fmt, __builtin_va_list ap);
static int port_vsnprintf_wrap(char *buf, int cap, ...)
{
    __builtin_va_list ap;
    int n;

    __builtin_va_start(ap, cap);
    n = port_vsnprintf(buf, cap, "[%d e%d l%x p%x] ", ap);
    __builtin_va_end(ap);
    return n < cap ? n : cap - 1;
}

/* ---- mixer ------------------------------------------------------------------------------------- */
static void seq_advance(double seconds)
{
    int i;

    for (i = 0; i < NUM_SEQS; i++)
    {
        Seq *s = &sSeqs[i];

        if (!s->open || !s->playing) continue;
        s->tickAccum += seconds * 1000000.0 / (double)s->tempo * (double)s->resolution;
        while (s->tickAccum >= 1.0)
        {
            long ticks = (long)s->tickAccum;

            if (s->wait > ticks)
            {
                s->wait -= ticks;
                s->tickAccum -= ticks;
                break;
            }
            s->tickAccum -= s->wait;
            s->wait = 0;
            if (!seq_events(i, s))
            {
                if (s->playLoops == 0 || --s->playLoops > 0)
                {
                    seq_restart(s);
                }
                else
                {
                    s->playing = 0;
                    break;
                }
            }
        }
    }
}

/* Generates `frames` stereo samples and hands them to the host. */
void port_snd_render(int frames)
{
    static short out[4096 * 2];
    int done = 0;

    if (frames > 4096) frames = 4096;
    {
        static int counter;
        int flag[1];

        if (port_debug_values("sndlog", flag, 1) == 1 && ++counter % 60 == 0)
        {
            int k, act = 0, keyed = 0, seqv = 0;
            char buf[200];
            int n = 0;

            for (k = 0; k < NUM_VOICES; k++)
            {
                if (sVoices[k].active)
                {
                    act++;
                    if (sVoices[k].keyed) keyed++;
                    if (k >= NUM_SPU_VOICES) seqv++;
                    if (n < 180) n += port_vsnprintf_wrap(buf + n, 200 - n, k, sVoices[k].envState, sVoices[k].envLevel, (int)sVoices[k].pitch);
                }
            }
            buf[n] = 0;
            port_log("snd: active %d keyed %d seq %d | %s", act, keyed, seqv, buf);
        }
    }
    while (done < frames)
    {
        int chunk = frames - done, i, k;

        if (chunk > 64) chunk = 64;
        seq_advance((double)chunk / SAMPLE_RATE);
        for (i = 0; i < chunk; i++)
        {
            int l = 0, r = 0;

            for (k = 0; k < NUM_VOICES; k++)
            {
                Voice *v = &sVoices[k];

                if (!v->active) continue;
                if (v->autoTime > 0)
                {
                    int vol = v->autoFrom + (v->autoTo - v->autoFrom) * v->autoElapsed / v->autoTime;

                    v->volL = v->baseL * vol / 127;
                    v->volR = v->baseR * vol / 127;
                    if (++v->autoElapsed >= v->autoTime) v->autoTime = 0;
                }
                render_voice_sample(v, &l, &r);
            }
            if (sReverbOn) reverb_process(&l, &r);
            l = l * sMasterL / 127 / 2;
            r = r * sMasterR / 127 / 2;
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            out[(done + i) * 2] = (short)l;
            out[(done + i) * 2 + 1] = (short)r;
        }
        done += chunk;
    }
    port_audio_push(out, frames);
}
