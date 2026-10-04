/*
 * Wii / GameCube platform host (libogc) for PS1 games on the com.recomp.ps1 runtime.
 * Used with the wasm2c guest backend (Source/Wasm).
 *
 * Files live on the SD card (Wii: front slot; GameCube: SD Gecko), on the default
 * libfat device:
 *   /ps1/<game>/disc.idx + files the extracted disc (tools/extract_disc.py), or
 *   /ps1/<disc image>            the disc image named by the game package (raw .bin)
 *   /ps1/saves/<game>/card0/...  memory card files
 *   /ps1/<game>.log              log
 *
 * The game runs on its own thread and waits on the vblank the main thread signals.
 * Frames go to the screen through GX: RGB565 texture, 4:3 quad, copy to the XFB.
 * Exit: hold HOME (Wiimote / Classic) or Start+Z (GameCube pad) for a second.
 */
#include <gccore.h>
#include <ogc/cond.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/lwp_watchdog.h>
#include <asndlib.h>
#include <fat.h>
#include <malloc.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef HW_RVL
#include <wiiuse/wpad.h>
#endif

#include "port_host.h"
#include "ps1_game_config.h"
#include "../host_backend.h"

#define ROOT "/ps1/"
#define GAME_STACK_SIZE (256u << 10) /* measured peak about 6 KB (the stats line reports it) */
#define FIFO_SIZE (256 * 1024)

static char sSaveDir[256];
static FILE *sLog;

static volatile unsigned sVblank;
static mutex_t sVblankLock;
static cond_t sVblankCond;
static volatile unsigned sPad;
static volatile int sCrashed;

static GXRModeObj *sMode;
static void *sXfb[2];
static int sXfbIndex;

/* ---- logging -------------------------------------------------------------------- */
void host_log(const char *fmt, ...)
{
    char buf[1024];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    SYS_Report("[ps1] %s\n", buf);
    if (sLog)
    {
        fprintf(sLog, "%s\n", buf);
        fflush(sLog);
    }
}

void port_host_log(const char *msg)
{
    host_log("%s", msg);
}

void port_host_fatal(const char *msg)
{
    host_log("FATAL: %s", msg);
    ps1_backend_fatal();
    host_crashed();
}

void host_crashed(void)
{
    /* the main loop puts the log on screen; this thread stops here */
    sCrashed = 1;
    for (;;)
    {
        usleep(100000);
    }
}

/* ---- disc: ps1w_disc.c ------------------------------------------------------------ */
int ps1w_disc_open(const char *path);

/* ---- timing, input ----------------------------------------------------------------- */
static u64 sProfStart[2], sProfTicks[2];

static void profile_hook(int which, int begin)
{
    if (begin) sProfStart[which] = gettime();
    else sProfTicks[which] += gettime() - sProfStart[which];
}

extern void (*ps1w_profile_hook)(int which, int begin);

static const unsigned char *sStack;

/* bytes of the game thread's stack ever used (it grows down from the top) */
static unsigned stack_used(void)
{
    unsigned i = 0;

    if (sStack == NULL) return 0;
    while (i < GAME_STACK_SIZE && sStack[i] == 0xA5) i++;
    return GAME_STACK_SIZE - i;
}

static volatile unsigned sGameFrames;
static u64 sBusyTicks, sLastWake; /* game thread time spent working (not waiting) */

/* Fast-forward (port_set_speed): of every sSpeed waits only one blocks; the others
 * count as vblanks the game ran ahead (sExtraVblanks, game thread only). */
static volatile int sSpeed = 1;
static unsigned sFastWaits, sExtraVblanks;
/* fast-forward must be refreshed: after this many waits without a call it ends */
static unsigned sWaitsSinceSpeed;
#define SPEED_TIMEOUT 30

int port_speed(void)
{
    return sSpeed;
}

void port_set_speed(int multiplier)
{
    int speed = multiplier < 1 ? 1 : (multiplier > 8 ? 8 : multiplier);

    sWaitsSinceSpeed = 0;
    if (speed != sSpeed)
    {
        port_host_log(speed > 1 ? "fast-forward on" : "fast-forward off");
        sSpeed = speed;
    }
}

unsigned port_wait_vblank(void)
{
    unsigned seen;
    u64 now = gettime();

    if (sSpeed > 1 && ++sWaitsSinceSpeed > SPEED_TIMEOUT)
    {
        port_host_log("fast-forward expired (not refreshed)");
        port_set_speed(1);
    }
    if (sSpeed > 1 && (++sFastWaits % (unsigned)sSpeed) != 0)
    {
        sExtraVblanks++;
        return sVblank + sExtraVblanks;
    }
    sGameFrames++;
    if (sLastWake) sBusyTicks += now - sLastWake;

    LWP_MutexLock(sVblankLock);
    seen = sVblank;
    while (sVblank == seen)
    {
        LWP_CondWait(sVblankCond, sVblankLock);
    }
    seen = sVblank;
    LWP_MutexUnlock(sVblankLock);
    sLastWake = gettime();
    return seen + sExtraVblanks;
}

unsigned port_vblank_count(void)
{
    return sVblank + sExtraVblanks;
}

/* ---- test options -------------------------------------------------------------------
 * /ps1/<game>.args holds the Windows host's test options, whitespace separated:
 *   --debug-NAME a,b,c          values for port_debug_values (game test hooks)
 *   --script F:HEX[:DUR],...    pad bits held from vblank F for DUR vblanks (default 4) */
#define MAX_DEBUG_ARGS 8
#define MAX_SCRIPT 256
static char sDebugNames[MAX_DEBUG_ARGS][32];
static char sDebugValues[MAX_DEBUG_ARGS][64];
static int sDebugCount;
static struct { int frame, dur; unsigned buttons; } sScript[MAX_SCRIPT];
static int sScriptCount;

static void parse_script(const char *text)
{
    while (*text && sScriptCount < MAX_SCRIPT)
    {
        int d = 4, f;
        unsigned b;
        char *end;

        f = (int)strtol(text, &end, 10);
        if (*end != ':') break;
        b = (unsigned)strtoul(end + 1, &end, 16);
        if (*end == ':') d = (int)strtol(end + 1, &end, 10);
        sScript[sScriptCount].frame = f;
        sScript[sScriptCount].buttons = b;
        sScript[sScriptCount].dur = d;
        sScriptCount++;
        text = end;
        while (*text == ',') text++;
    }
}

static void load_args(const char *path)
{
    static char text[16384];
    char *tok, *save;
    FILE *f = fopen(path, "r");
    size_t n;

    if (f == NULL) return;
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    for (tok = strtok_r(text, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save))
    {
        char *value = strtok_r(NULL, " \t\r\n", &save);

        if (value == NULL) break;
        if (!strcmp(tok, "--script"))
        {
            parse_script(value);
        }
        else if (!strncmp(tok, "--debug-", 8) && sDebugCount < MAX_DEBUG_ARGS)
        {
            snprintf(sDebugNames[sDebugCount], sizeof(sDebugNames[0]), "%s", tok + 8);
            snprintf(sDebugValues[sDebugCount], sizeof(sDebugValues[0]), "%s", value);
            sDebugCount++;
        }
    }
    host_log("args: %s (%d debug values, %d script steps)", path, sDebugCount, sScriptCount);
}

int port_debug_values(const char *name, int *out, int max)
{
    int i, n = 0;

    for (i = 0; i < sDebugCount; i++)
    {
        if (!strcmp(sDebugNames[i], name))
        {
            const char *p = sDebugValues[i];
            char *end;

            while (*p && n < max)
            {
                out[n++] = (int)strtoul(p, &end, 0);
                if (end == p) break;
                p = end;
                while (*p == ',') p++;
            }
            return n;
        }
    }
    return 0;
}

unsigned port_pad_state(void)
{
    unsigned bits = sPad;
    int frame = (int)sVblank, i;

    for (i = 0; i < sScriptCount; i++)
    {
        if (frame >= sScript[i].frame && frame < sScript[i].frame + sScript[i].dur) bits |= sScript[i].buttons;
    }
    return bits;
}

#define PS_SELECT   (1u << 8)
#define PS_START    (1u << 11)
#define PS_UP       (1u << 12)
#define PS_RIGHT    (1u << 13)
#define PS_DOWN     (1u << 14)
#define PS_LEFT     (1u << 15)
#define PS_L2       (1u << 0)
#define PS_R2       (1u << 1)
#define PS_L1       (1u << 2)
#define PS_R1       (1u << 3)
#define PS_TRIANGLE (1u << 4)
#define PS_CIRCLE   (1u << 5)
#define PS_CROSS    (1u << 6)
#define PS_SQUARE   (1u << 7)

static int sQuit;
static int sQuitHeld; /* vblanks the exit combination has been held */

static unsigned poll_pads(void)
{
    unsigned bits = 0;
    int quit = 0;
    u16 b;
    s8 sx, sy;

    PAD_ScanPads();
    b = PAD_ButtonsHeld(0);
    sx = PAD_StickX(0);
    sy = PAD_StickY(0);
    if (b & PAD_BUTTON_UP || sy > 48) bits |= PS_UP;
    if (b & PAD_BUTTON_DOWN || sy < -48) bits |= PS_DOWN;
    if (b & PAD_BUTTON_LEFT || sx < -48) bits |= PS_LEFT;
    if (b & PAD_BUTTON_RIGHT || sx > 48) bits |= PS_RIGHT;
    if (b & PAD_BUTTON_A) bits |= PS_CROSS;
    if (b & PAD_BUTTON_B) bits |= PS_CIRCLE;
    if (b & PAD_BUTTON_X) bits |= PS_SQUARE;
    if (b & PAD_BUTTON_Y) bits |= PS_TRIANGLE;
    if (b & PAD_TRIGGER_L) bits |= PS_L1;
    if (b & PAD_TRIGGER_R) bits |= PS_R1;
    if (b & PAD_TRIGGER_Z) bits |= PS_SELECT;
    if (b & PAD_BUTTON_START) bits |= PS_START;
    if ((b & PAD_BUTTON_START) && (b & PAD_TRIGGER_Z)) quit = 1;
#ifdef HW_RVL
    {
        u32 w;
        expansion_t exp;

        WPAD_ScanPads();
        w = WPAD_ButtonsHeld(0);
        WPAD_Expansion(0, &exp);
        if (exp.type == WPAD_EXP_CLASSIC)
        {
            if (w & WPAD_CLASSIC_BUTTON_UP) bits |= PS_UP;
            if (w & WPAD_CLASSIC_BUTTON_DOWN) bits |= PS_DOWN;
            if (w & WPAD_CLASSIC_BUTTON_LEFT) bits |= PS_LEFT;
            if (w & WPAD_CLASSIC_BUTTON_RIGHT) bits |= PS_RIGHT;
            if (w & WPAD_CLASSIC_BUTTON_B) bits |= PS_CROSS;
            if (w & WPAD_CLASSIC_BUTTON_A) bits |= PS_CIRCLE;
            if (w & WPAD_CLASSIC_BUTTON_Y) bits |= PS_SQUARE;
            if (w & WPAD_CLASSIC_BUTTON_X) bits |= PS_TRIANGLE;
            if (w & WPAD_CLASSIC_BUTTON_FULL_L) bits |= PS_L1;
            if (w & WPAD_CLASSIC_BUTTON_FULL_R) bits |= PS_R1;
            if (w & WPAD_CLASSIC_BUTTON_ZL) bits |= PS_L2;
            if (w & WPAD_CLASSIC_BUTTON_ZR) bits |= PS_R2;
            if (w & WPAD_CLASSIC_BUTTON_PLUS) bits |= PS_START;
            if (w & WPAD_CLASSIC_BUTTON_MINUS) bits |= PS_SELECT;
            if (w & WPAD_CLASSIC_BUTTON_HOME) quit = 1;
        }
        else
        {
            /* Wiimote held sideways (d-pad on the left) */
            if (w & WPAD_BUTTON_RIGHT) bits |= PS_UP;
            if (w & WPAD_BUTTON_LEFT) bits |= PS_DOWN;
            if (w & WPAD_BUTTON_UP) bits |= PS_LEFT;
            if (w & WPAD_BUTTON_DOWN) bits |= PS_RIGHT;
            if (w & WPAD_BUTTON_2) bits |= PS_CROSS;
            if (w & WPAD_BUTTON_1) bits |= PS_CIRCLE;
            if (w & WPAD_BUTTON_B) bits |= PS_SQUARE;
            if (w & WPAD_BUTTON_A) bits |= PS_TRIANGLE;
            if (w & WPAD_BUTTON_PLUS) bits |= PS_START;
            if (w & WPAD_BUTTON_MINUS) bits |= PS_SELECT;
        }
        if (w & WPAD_BUTTON_HOME) quit = 1;
    }
#endif
    /* held for a second, so a stray press (Dolphin maps Enter to both Start and
     * HOME by default) does not end the game */
    sQuitHeld = quit ? sQuitHeld + 1 : 0;
    if (sQuitHeld >= 60) sQuit = 1;
    return bits;
}

/* ---- video --------------------------------------------------------------------------- */
#define TEX_MAX_W 640
#define TEX_MAX_H 512

static mutex_t sFrameLock;
static unsigned char *sFrame; /* RGBA from the game */
static int sFrameW, sFrameH;
static volatile unsigned sFrameSerial;
static u16 *sTex;             /* RGB565, 4x4 tiles */

void port_present(const unsigned char *rgba, int width, int height)
{
    if (width > TEX_MAX_W || height > TEX_MAX_H) return;
    LWP_MutexLock(sFrameLock);
    memcpy(sFrame, rgba, (size_t)width * height * 4);
    sFrameW = width;
    sFrameH = height;
    sFrameSerial++;
    LWP_MutexUnlock(sFrameLock);
}

static void video_init(void)
{
    static void *fifo;
    Mtx44 ortho;
    Mtx identity;
    f32 yscale;
    u32 xfb_height;

    VIDEO_Init();
    sMode = VIDEO_GetPreferredMode(NULL);
    VIDEO_Configure(sMode);
    sXfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(sMode));
    sXfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(sMode));
    VIDEO_ClearFrameBuffer(sMode, sXfb[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(sMode, sXfb[1], COLOR_BLACK);
    VIDEO_SetNextFramebuffer(sXfb[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (sMode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    fifo = memalign(32, FIFO_SIZE);
    memset(fifo, 0, FIFO_SIZE);
    GX_Init(fifo, FIFO_SIZE);
    GX_SetCopyClear((GXColor){0, 0, 0, 255}, GX_MAX_Z24);
    GX_SetViewport(0, 0, sMode->fbWidth, sMode->efbHeight, 0, 1);
    yscale = GX_GetYScaleFactor(sMode->efbHeight, sMode->xfbHeight);
    xfb_height = GX_SetDispCopyYScale(yscale);
    GX_SetScissor(0, 0, sMode->fbWidth, sMode->efbHeight);
    GX_SetDispCopySrc(0, 0, sMode->fbWidth, sMode->efbHeight);
    GX_SetDispCopyDst(sMode->fbWidth, xfb_height);
    GX_SetCopyFilter(sMode->aa, sMode->sample_pattern, GX_TRUE, sMode->vfilter);
    GX_SetFieldMode(sMode->field_rendering, sMode->viHeight == 2 * sMode->xfbHeight ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(0);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    guOrtho(ortho, 0, sMode->efbHeight, 0, sMode->fbWidth, 0, 1);
    GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
    guMtxIdentity(identity);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);

    sFrame = (unsigned char *)malloc(TEX_MAX_W * TEX_MAX_H * 4);
    sTex = (u16 *)memalign(32, TEX_MAX_W * TEX_MAX_H * 2);
    LWP_MutexInit(&sFrameLock, false);
}

/* RGBA -> RGB565 in the GX 4x4 tile order */
static void upload_frame(int *out_w, int *out_h)
{
    int w, h, tw, th, x, y;

    LWP_MutexLock(sFrameLock);
    w = sFrameW;
    h = sFrameH;
    tw = (w + 3) & ~3;
    th = (h + 3) & ~3;
    for (y = 0; y < th; y++)
    {
        const unsigned char *src = sFrame + (size_t)(y < h ? y : h - 1) * w * 4;
        u16 *row = sTex + (size_t)(y >> 2) * (tw >> 2) * 16 + (y & 3) * 4;

        for (x = 0; x < tw; x++)
        {
            const unsigned char *p = src + (x < w ? x : w - 1) * 4;

            row[(x >> 2) * 16 + (x & 3)] = (u16)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        }
    }
    LWP_MutexUnlock(sFrameLock);
    DCFlushRange(sTex, (u32)tw * th * 2);
    *out_w = tw;
    *out_h = th;
}

static void draw_frame(void)
{
    GXTexObj tex;
    int tw, th;
    f32 x0, y0, x1, y1, u1, v1;
    int fw = sMode->fbWidth, fh = sMode->efbHeight;

    upload_frame(&tw, &th);
    GX_InitTexObj(&tex, sTex, tw, th, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&tex, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&tex, GX_TEXMAP0);
    GX_InvalidateTexAll();
    /* 4:3 picture filling the height */
    x0 = 0;
    x1 = fw;
    y0 = 0;
    y1 = fh;
    u1 = (f32)sFrameW / tw;
    v1 = (f32)sFrameH / th;
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2f32(x0, y0); GX_TexCoord2f32(0, 0);
    GX_Position2f32(x1, y0); GX_TexCoord2f32(u1, 0);
    GX_Position2f32(x1, y1); GX_TexCoord2f32(u1, v1);
    GX_Position2f32(x0, y1); GX_TexCoord2f32(0, v1);
    GX_End();
    GX_DrawDone();
    sXfbIndex ^= 1;
    GX_CopyDisp(sXfb[sXfbIndex], GX_TRUE);
    GX_Flush();
    VIDEO_SetNextFramebuffer(sXfb[sXfbIndex]);
    VIDEO_Flush();
}

static void show_text_screen(const char *title)
{
    static int shown;
    char line[256];
    FILE *f;

    if (shown) return;
    shown = 1;
    CON_Init(sXfb[sXfbIndex], 20, 20, sMode->fbWidth, sMode->xfbHeight, sMode->fbWidth * VI_DISPLAY_PIX_SZ);
    printf("\x1b[2J\x1b[2;0H%s\n\n", title);
    /* last lines of the log */
    snprintf(line, sizeof(line), ROOT "%s.log", PS1_GAME_TITLE);
    f = fopen(line, "r");
    if (f)
    {
        static char lines[16][120];
        int n = 0, i;

        while (fgets(line, sizeof(line), f))
        {
            snprintf(lines[n % 16], sizeof(lines[0]), "%s", line);
            n++;
        }
        fclose(f);
        for (i = n > 16 ? n - 16 : 0; i < n; i++) printf("%s", lines[i % 16]);
    }
    printf("\nHold HOME / Start+Z to exit.\n");
}

/* ---- debug hooks ------------------------------------------------------------------- */
int port_trace_gpu(void)
{
    return 0;
}

void port_debug_vram(const unsigned short *vram) {}

/* ---- audio ----------------------------------------------------------------------------- */
#define AUDIO_RING 16384 /* frames */
#define AUDIO_BLOCK 1024 /* frames per ASND buffer */
static s16 sRing[AUDIO_RING * 2];
static volatile unsigned sRingWrite, sRingRead;
static s16 sBlocks[3][AUDIO_BLOCK * 2] ATTRIBUTE_ALIGN(32);
static int sBlockIndex;

void port_audio_push(const short *samples, int frames)
{
    int i;

    if (sSpeed > 1) return; /* fast-forward: drop the sound */
    for (i = 0; i < frames; i++)
    {
        unsigned w = sRingWrite;

        if (w - sRingRead >= AUDIO_RING) break;
        sRing[(w % AUDIO_RING) * 2] = samples[i * 2];
        sRing[(w % AUDIO_RING) * 2 + 1] = samples[i * 2 + 1];
        sRingWrite = w + 1;
    }
}

static s16 *fill_block(void)
{
    s16 *b = sBlocks[sBlockIndex];
    int i;

    sBlockIndex = (sBlockIndex + 1) % 3;
    for (i = 0; i < AUDIO_BLOCK; i++)
    {
        unsigned r = sRingRead;

        if (r != sRingWrite)
        {
            b[i * 2] = sRing[(r % AUDIO_RING) * 2];
            b[i * 2 + 1] = sRing[(r % AUDIO_RING) * 2 + 1];
            sRingRead = r + 1;
        }
        else
        {
            b[i * 2] = b[i * 2 + 1] = 0;
        }
    }
    DCFlushRange(b, sizeof(sBlocks[0]));
    return b;
}

static void audio_callback(s32 voice)
{
    ASND_AddVoice(voice, fill_block(), sizeof(sBlocks[0]));
}

static void audio_init(void)
{
    ASND_Init();
    ASND_Pause(0);
    ASND_SetVoice(0, VOICE_STEREO_16BIT, 44100, 0, fill_block(), sizeof(sBlocks[0]), 255, 255, audio_callback);
}

/* ---- save files ------------------------------------------------------------------------- */
static void save_path(const char *rel, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s", sSaveDir, rel);
}

static void make_parent_dirs(const char *path)
{
    char dir[256];
    size_t n;

    snprintf(dir, sizeof(dir), "%s", path);
    for (n = 1; dir[n]; n++)
    {
        if (dir[n] == '/')
        {
            dir[n] = 0;
            mkdir(dir, 0777);
            dir[n] = '/';
        }
    }
}

int port_file_size(const char *rel)
{
    char path[256];
    struct stat st;

    save_path(rel, path, sizeof(path));
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) return -1;
    return (int)st.st_size;
}

int port_file_read(const char *rel, unsigned offset, void *dst, unsigned len)
{
    char path[256];
    FILE *f;
    size_t got;

    save_path(rel, path, sizeof(path));
    f = fopen(path, "rb");
    if (f == NULL) return -1;
    fseek(f, (long)offset, SEEK_SET);
    got = fread(dst, 1, len, f);
    fclose(f);
    return (int)got;
}

int port_file_write(const char *rel, unsigned offset, const void *src, unsigned len, int create)
{
    char path[256];
    FILE *f;
    size_t put;

    save_path(rel, path, sizeof(path));
    make_parent_dirs(path);
    f = fopen(path, create ? "wb" : "r+b");
    if (f == NULL) return -1;
    fseek(f, (long)offset, SEEK_SET);
    put = fwrite(src, 1, len, f);
    fclose(f);
    return (int)put;
}

int port_file_delete(const char *rel)
{
    char path[256];

    save_path(rel, path, sizeof(path));
    return remove(path) == 0 ? 0 : -1;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int port_file_list(const char *rel_dir, unsigned index, char *name, unsigned name_cap)
{
    char path[256];
    static char names[64][64];
    unsigned count = 0;
    DIR *d;
    struct dirent *e;

    save_path(rel_dir, path, sizeof(path));
    d = opendir(path);
    if (d == NULL) return -1;
    while ((e = readdir(d)) != NULL && count < 64)
    {
        char full[320];
        struct stat st;

        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (e->d_name[0] == '.' || stat(full, &st) != 0 || S_ISDIR(st.st_mode)) continue;
        snprintf(names[count++], sizeof(names[0]), "%s", e->d_name);
    }
    closedir(d);
    /* name order, like the Windows host */
    qsort(names, count, sizeof(names[0]), compare_names);
    if (index >= count) return -1;
    snprintf(name, name_cap, "%s", names[index]);
    snprintf(path, sizeof(path), "%s%s/%s", sSaveDir, rel_dir, names[index]);
    {
        struct stat st;

        return stat(path, &st) == 0 ? (int)st.st_size : 0;
    }
}

/* ---- main ------------------------------------------------------------------------------- */
static void *game_thread(void *arg)
{
    ps1_backend_run();
    host_log("game returned from main()");
    sCrashed = 1;
    return NULL;
}

int main(int argc, char **argv)
{
    char path[256];
    lwp_t thread;
    void *stack;
    unsigned presented = 0;

    video_init();
    PAD_Init();
#ifdef HW_RVL
    WPAD_Init();
#endif
    ps1w_profile_hook = profile_hook;
    LWP_MutexInit(&sVblankLock, false);
    LWP_CondInit(&sVblankCond);

    if (!fatInitDefault())
    {
        show_text_screen("No SD card: put the disc image in /ps1/ on the SD card.");
        sCrashed = 1;
    }
    else
    {
        mkdir(ROOT, 0777);
        snprintf(path, sizeof(path), ROOT "%s.log", PS1_GAME_TITLE);
        sLog = fopen(path, "w");
        snprintf(sSaveDir, sizeof(sSaveDir), ROOT "saves/%s/", PS1_GAME_TITLE);
        snprintf(path, sizeof(path), ROOT "%s.args", PS1_GAME_TITLE);
        load_args(path);
        /* an extracted disc (/ps1/<title>/disc.idx, from extract_disc.py) or the image */
        snprintf(path, sizeof(path), ROOT "%s/disc.idx", PS1_GAME_TITLE);
        if (!ps1w_disc_open(path))
        {
            snprintf(path, sizeof(path), ROOT "%s", PS1_DEFAULT_DISC_NAME);
        }
        if (!ps1w_disc_open(path))
        {
            host_log("cannot open " ROOT "%s/ or %s", PS1_GAME_TITLE, path);
            sCrashed = 1;
        }
        else if (!ps1_backend_init())
        {
            sCrashed = 1;
        }
        else
        {
            audio_init();
            stack = memalign(32, GAME_STACK_SIZE);
            memset(stack, 0xA5, GAME_STACK_SIZE); /* for the stack high-water mark in the stats */
            sStack = (const unsigned char *)stack;
            LWP_CreateThread(&thread, game_thread, NULL, stack, GAME_STACK_SIZE, 48);
        }
    }

    while (!sQuit)
    {
        VIDEO_WaitVSync();
        sPad = poll_pads();
        LWP_MutexLock(sVblankLock);
        sVblank++;
        LWP_CondBroadcast(sVblankCond);
        LWP_MutexUnlock(sVblankLock);
        if (sCrashed)
        {
            show_text_screen(PS1_GAME_TITLE " stopped:");
            continue;
        }
        if (sFrameSerial != presented)
        {
            presented = sFrameSerial;
            draw_frame();
        }
        if (sVblank % 300 == 0)
        {
            /* speed check: the game waits for ~every vblank when it keeps up */
            host_log("stats: vblank %u, game frames %u, presents %u, game thread busy %u ms (draw %u, present %u), "
                     "stack %u KB",
                     sVblank, sGameFrames, sFrameSerial, (unsigned)ticks_to_millisecs(sBusyTicks),
                     (unsigned)ticks_to_millisecs(sProfTicks[0]), (unsigned)ticks_to_millisecs(sProfTicks[1]),
                     stack_used() / 1024);
        }
    }
    if (sLog) fclose(sLog);
    exit(0);
    return 0;
}
