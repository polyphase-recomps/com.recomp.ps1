/*
 * Windows platform host for PS1 games built on the com.recomp.ps1 runtime.
 *
 *   game.exe [--disc path.bin] [--scale N]             windowed, keyboard / XInput
 *   game.exe --headless --frames N [--dump dir] [--every N] [--script F:HEX[:DUR],...]
 *   game.exe --shm NAME                                embedded: frames / input via shared memory (Polyphase addon)
 *
 * The game runs on its own thread through the guest backend (../native or ../wasm,
 * see ../host_backend.h); the main thread owns the window, input and pacing.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_host.h"
#include "port_shm.h"
#include "ps1_game_config.h"
#include "../host_backend.h"

static FILE *sWavFile;
static unsigned long sWavFrames;
static void wav_header(FILE *f, unsigned long frames);

/* ---- options / state ------------------------------------------------------------ */
static char sDiscPath[MAX_PATH];
static char sSaveDir[MAX_PATH];
static int sHeadless;
static int sFramesLimit = -1;
static const char *sDumpDir;
static int sDumpEvery = 30;
static int sPressFrom, sPressEvery;
static unsigned sPressButtons = 0x0800; /* START */
/* --script "F:HEX[:DUR],...": hold HEX from vblank F for DUR vblanks (default 4) */
#define MAX_SCRIPT 256
static struct { int frame, dur; unsigned buttons; } sScript[MAX_SCRIPT];
static int sScriptCount;

static void parse_script(const char *text)
{
    while (*text && sScriptCount < MAX_SCRIPT)
    {
        int f = 0, d = 4;
        unsigned b = 0;
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
        while (*text == ',' || *text == ' ') text++;
    }
}
static int sScale = 3;
static const char *sShmName;


static volatile LONG sVblank;
static HANDLE sVblankEvent;
static volatile LONG sPad;

static CRITICAL_SECTION sFrameLock;
static unsigned char *sFrame;
static int sFrameW, sFrameH;
static volatile LONG sFrameSerial;

static HWND sWindow;
static FILE *sLogFile;
static PortShm *sShm;
static HANDLE sShmMapping;

/* ---- logging ---------------------------------------------------------------------- */
void host_log(const char *fmt, ...)
{
    char buf[1024];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fprintf(stderr, "[ps1] %s\n", buf);
    fflush(stderr);
    if (sLogFile)
    {
        fprintf(sLogFile, "%s\n", buf);
        fflush(sLogFile);
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

/* (recomp mode's threads: leave the game from the start thread's stack - here, the end) */
void host_unwind(int code)
{
    if (code == 1) host_crashed();
    ExitProcess(0);
}

void host_crashed(void)
{
    if (sShm)
    {
        sShm->status = PORT_SHM_STATUS_CRASHED;
    }
    TerminateProcess(GetCurrentProcess(), 3);
}

/* ---- save files --------------------------------------------------------------------- */
static void save_path(const char *rel, char *out, size_t cap)
{
    size_t n;

    snprintf(out, cap, "%s%s", sSaveDir, rel);
    for (n = 0; out[n]; n++)
    {
        if (out[n] == '/') out[n] = '\\';
    }
}

/* Creates the folders leading to a file. */
static void make_parent_dirs(const char *path)
{
    char dir[MAX_PATH];
    size_t n;

    snprintf(dir, sizeof(dir), "%s", path);
    for (n = 3; dir[n]; n++)
    {
        if (dir[n] == '\\')
        {
            dir[n] = 0;
            CreateDirectoryA(dir, NULL);
            dir[n] = '\\';
        }
    }
}

int port_file_size(const char *rel)
{
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA info;

    save_path(rel, path, sizeof(path));
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        return -1;
    }
    return (int)info.nFileSizeLow;
}

int port_file_read(const char *rel, unsigned offset, void *dst, unsigned len)
{
    char path[MAX_PATH];
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
    char path[MAX_PATH];
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
    char path[MAX_PATH];

    save_path(rel, path, sizeof(path));
    return DeleteFileA(path) ? 0 : -1;
}

int port_file_list(const char *rel_dir, unsigned index, char *name, unsigned name_cap)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    unsigned i = 0;
    int size = -1;

    save_path(rel_dir, pattern, sizeof(pattern));
    strncat(pattern, "\\*", sizeof(pattern) - strlen(pattern) - 1);
    /* NTFS lists in name order */
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (i++ == index)
        {
            snprintf(name, name_cap, "%s", fd.cFileName);
            size = (int)fd.nFileSizeLow;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return size;
}

/* ---- disc: Source/Wasm/ps1w_disc.c (image or extracted folder) ---------------------------- */
int ps1w_disc_open(const char *path);

/* ---- timing ------------------------------------------------------------------------- */
static unsigned sLastPresentVblank;
static int sDumpFrom;
static int sDumpVram;
static int sDumpVramPending;
static unsigned sNextDump;

/* Fast-forward (port_set_speed): of every sSpeed waits only one blocks, the others
 * count a vblank at once. Headless runs are unpaced anyway. */
static volatile int sSpeed = 1;
static unsigned sFastWaits;
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
    if (sSpeed > 1 && ++sWaitsSinceSpeed > SPEED_TIMEOUT)
    {
        port_host_log("fast-forward expired (not refreshed)");
        port_set_speed(1);
    }
    if (sHeadless)
    {
        InterlockedIncrement(&sVblank);
        return (unsigned)sVblank;
    }
    if (sSpeed > 1 && (++sFastWaits % (unsigned)sSpeed) != 0)
    {
        InterlockedIncrement(&sVblank);
        return (unsigned)sVblank;
    }
    WaitForSingleObject(sVblankEvent, INFINITE);
    return (unsigned)sVblank;
}

unsigned port_vblank_count(void)
{
    return (unsigned)sVblank;
}

/* ---- script bridge (port_bridge.h) ----------------------------------------------------
 * Native builds: only Polyphase serves scripts, so the tables are ignored here. */
void port_bridge_publish(const void *vars, int nvars, const void *requests, int nrequests)
{
    (void)vars, (void)nvars, (void)requests, (void)nrequests;
}

int port_bridge_poll(char *name, unsigned name_cap, int *args, int max_args, int *nargs)
{
    (void)name, (void)name_cap, (void)args, (void)max_args;
    *nargs = 0;
    return 0;
}

void port_bridge_done(int id, int result)
{
    (void)id, (void)result;
}

/* ---- input ---------------------------------------------------------------------------- */
unsigned port_pad_state(void)
{
    if (sScriptCount > 0)
    {
        int frame = (int)sVblank, i;
        unsigned bits = 0;

        for (i = 0; i < sScriptCount; i++)
        {
            if (frame >= sScript[i].frame && frame < sScript[i].frame + sScript[i].dur) bits |= sScript[i].buttons;
        }
        if (sHeadless) return bits;
        return bits | (unsigned)sPad;
    }
    if (sHeadless && sPressEvery > 0)
    {
        int frame = (int)sVblank;

        if (frame >= sPressFrom && ((frame - sPressFrom) % sPressEvery) < 4)
        {
            return sPressButtons;
        }
        return 0;
    }
    return (unsigned)sPad;
}

#define PAD_SELECT   (1u << 8)
#define PAD_START    (1u << 11)
#define PAD_UP       (1u << 12)
#define PAD_RIGHT    (1u << 13)
#define PAD_DOWN     (1u << 14)
#define PAD_LEFT     (1u << 15)
#define PAD_L2       (1u << 0)
#define PAD_R2       (1u << 1)
#define PAD_L1       (1u << 2)
#define PAD_R1       (1u << 3)
#define PAD_TRIANGLE (1u << 4)
#define PAD_CIRCLE   (1u << 5)
#define PAD_CROSS    (1u << 6)
#define PAD_SQUARE   (1u << 7)

static unsigned poll_keyboard(void)
{
    static const struct { int key; unsigned bit; } map[] = {
        { VK_UP, PAD_UP }, { VK_DOWN, PAD_DOWN }, { VK_LEFT, PAD_LEFT }, { VK_RIGHT, PAD_RIGHT },
        { 'Z', PAD_CROSS }, { 'X', PAD_CIRCLE }, { 'A', PAD_SQUARE }, { 'S', PAD_TRIANGLE },
        { 'Q', PAD_L1 }, { 'W', PAD_R1 }, { '1', PAD_L2 }, { '2', PAD_R2 },
        { VK_RETURN, PAD_START }, { VK_BACK, PAD_SELECT }, { VK_RSHIFT, PAD_SELECT },
    };
    unsigned bits = 0;
    int i;

    if (GetForegroundWindow() != sWindow)
    {
        return 0;
    }
    for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++)
    {
        if (GetAsyncKeyState(map[i].key) & 0x8000)
        {
            bits |= map[i].bit;
        }
    }
    return bits;
}

static unsigned poll_xinput(void)
{
    XINPUT_STATE state;
    unsigned bits = 0;
    WORD b;

    if (XInputGetState(0, &state) != ERROR_SUCCESS)
    {
        return 0;
    }
    b = state.Gamepad.wButtons;
    if (b & XINPUT_GAMEPAD_DPAD_UP) bits |= PAD_UP;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) bits |= PAD_DOWN;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) bits |= PAD_LEFT;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) bits |= PAD_RIGHT;
    if (b & XINPUT_GAMEPAD_A) bits |= PAD_CROSS;
    if (b & XINPUT_GAMEPAD_B) bits |= PAD_CIRCLE;
    if (b & XINPUT_GAMEPAD_X) bits |= PAD_SQUARE;
    if (b & XINPUT_GAMEPAD_Y) bits |= PAD_TRIANGLE;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) bits |= PAD_L1;
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) bits |= PAD_R1;
    if (b & XINPUT_GAMEPAD_START) bits |= PAD_START;
    if (b & XINPUT_GAMEPAD_BACK) bits |= PAD_SELECT;
    if (state.Gamepad.bLeftTrigger > 64) bits |= PAD_L2;
    if (state.Gamepad.bRightTrigger > 64) bits |= PAD_R2;
    if (state.Gamepad.sThumbLX < -16000) bits |= PAD_LEFT;
    if (state.Gamepad.sThumbLX > 16000) bits |= PAD_RIGHT;
    if (state.Gamepad.sThumbLY > 16000) bits |= PAD_UP;
    if (state.Gamepad.sThumbLY < -16000) bits |= PAD_DOWN;
    return bits;
}

/* ---- video ---------------------------------------------------------------------------- */
static void write_ppm(const char *path, const unsigned char *rgba, int width, int height)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (f == NULL)
    {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (i = 0; i < width * height; i++)
    {
        fwrite(rgba + i * 4, 1, 3, f);
    }
    fclose(f);
}

void port_present(const unsigned char *rgba, int width, int height)
{
    unsigned frame = port_vblank_count();

    EnterCriticalSection(&sFrameLock);
    if (sFrame == NULL || width * height > sFrameW * sFrameH)
    {
        free(sFrame);
        sFrame = malloc((size_t)width * height * 4);
    }
    memcpy(sFrame, rgba, (size_t)width * height * 4);
    sFrameW = width;
    sFrameH = height;
    InterlockedIncrement(&sFrameSerial);
    LeaveCriticalSection(&sFrameLock);

    if (sShm && width <= PORT_SHM_MAX_W && height <= PORT_SHM_MAX_H)
    {
        int slot = (sShm->frame_index + 1) & 1;

        memcpy(sShm->frames[slot], rgba, (size_t)width * height * 4);
        sShm->width[slot] = width;
        sShm->height[slot] = height;
        MemoryBarrier();
        sShm->frame_index = slot;
        InterlockedIncrement((volatile LONG *)&sShm->frame_serial);
    }
    if (sDumpDir && frame != sLastPresentVblank && (int)frame >= sDumpFrom && frame >= sNextDump)
    {
        sNextDump = frame - (frame % sDumpEvery) + sDumpEvery;
        char path[MAX_PATH];

        snprintf(path, sizeof(path), "%s/frame_%05u.ppm", sDumpDir, frame);
        write_ppm(path, rgba, width, height);
        sDumpVramPending = sDumpVram;
    }
    sLastPresentVblank = frame;
    if (sHeadless && sFramesLimit >= 0 && (int)frame >= sFramesLimit)
    {
        host_log("reached %d frames", sFramesLimit);
        if (sWavFile) { wav_header(sWavFile, sWavFrames); fclose(sWavFile); sWavFile = NULL; }
        ExitProcess(0);
    }
}

static int sTraceGpuFrame = -1;
#define MAX_DEBUG_ARGS 8
static const char *sDebugNames[MAX_DEBUG_ARGS];
static const char *sDebugValues[MAX_DEBUG_ARGS];
static int sDebugCount;

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

int port_trace_gpu(void)
{
    return sTraceGpuFrame >= 0 && (int)port_vblank_count() >= sTraceGpuFrame && (int)port_vblank_count() <= sTraceGpuFrame + 3;
}

void port_debug_vram(const unsigned short *vram)
{
    static unsigned char rgba[1024 * 512 * 4];
    char path[MAX_PATH];
    int i;

    if (!sDumpVramPending) return;
    sDumpVramPending = 0;
    for (i = 0; i < 1024 * 512; i++)
    {
        unsigned short c = vram[i];

        rgba[i * 4] = (unsigned char)((c & 31) << 3);
        rgba[i * 4 + 1] = (unsigned char)(((c >> 5) & 31) << 3);
        rgba[i * 4 + 2] = (unsigned char)(((c >> 10) & 31) << 3);
        rgba[i * 4 + 3] = 255;
    }
    snprintf(path, sizeof(path), "%s/vram_%05u.ppm", sDumpDir, port_vblank_count());
    write_ppm(path, rgba, 1024, 512);
}

/* ---- audio ---------------------------------------------------------------------------- */
#define AUDIO_RING (44100 * 2)
static short sAudioRing[AUDIO_RING * 2];
static volatile LONG sAudioWrite, sAudioRead;
static HWAVEOUT sWaveOut;
#define AUDIO_BLOCKS 4
#define AUDIO_BLOCK_FRAMES 1470
static WAVEHDR sWaveHdr[AUDIO_BLOCKS];
static short sWaveData[AUDIO_BLOCKS][AUDIO_BLOCK_FRAMES * 2];

static void wav_header(FILE *f, unsigned long frames)
{
    unsigned long data = frames * 4, v;

    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    v = 0x00020001; fwrite(&v, 4, 1, f);        /* PCM, 2 channels */
    v = 44100; fwrite(&v, 4, 1, f);
    v = 44100 * 4; fwrite(&v, 4, 1, f);
    v = 0x00100004; fwrite(&v, 4, 1, f);        /* block align 4, 16 bits */
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

void port_audio_push(const short *samples, int frames)
{
    int i;

    if (sWavFile)
    {
        fwrite(samples, 4, (size_t)frames, sWavFile);
        sWavFrames += (unsigned long)frames;
        if ((sWavFrames & 0xFFFF) < (unsigned long)frames) wav_header(sWavFile, sWavFrames);
    }

    if (sSpeed > 1)
    {
        return; /* fast-forward: drop the sound (the .wav above keeps it) */
    }
    if (sShm)
    {
        for (i = 0; i < frames; i++)
        {
            unsigned w = sShm->audio_write;

            if (w - sShm->audio_read >= PORT_SHM_AUDIO_FRAMES)
            {
                break;
            }
            sShm->audio[(w % PORT_SHM_AUDIO_FRAMES) * 2] = samples[i * 2];
            sShm->audio[(w % PORT_SHM_AUDIO_FRAMES) * 2 + 1] = samples[i * 2 + 1];
            MemoryBarrier();
            sShm->audio_write = w + 1;
        }
        /* the embedded game also plays its audio itself (the ring above is optional
         * for hosts that want to mix it) */
    }
    for (i = 0; i < frames; i++)
    {
        LONG w = sAudioWrite;

        if (w - sAudioRead >= AUDIO_RING)
        {
            break;
        }
        sAudioRing[(w % AUDIO_RING) * 2] = samples[i * 2];
        sAudioRing[(w % AUDIO_RING) * 2 + 1] = samples[i * 2 + 1];
        sAudioWrite = w + 1;
    }
}

static void audio_fill_block(int index)
{
    int i;

    for (i = 0; i < AUDIO_BLOCK_FRAMES; i++)
    {
        LONG r = sAudioRead;

        if (r < sAudioWrite)
        {
            sWaveData[index][i * 2] = sAudioRing[(r % AUDIO_RING) * 2];
            sWaveData[index][i * 2 + 1] = sAudioRing[(r % AUDIO_RING) * 2 + 1];
            sAudioRead = r + 1;
        }
        else
        {
            sWaveData[index][i * 2] = sWaveData[index][i * 2 + 1] = 0;
        }
    }
}

static void audio_open(void)
{
    WAVEFORMATEX format;
    int i;

    memset(&format, 0, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 44100;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 44100 * 4;
    if (waveOutOpen(&sWaveOut, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
    {
        sWaveOut = NULL;
        return;
    }
    for (i = 0; i < AUDIO_BLOCKS; i++)
    {
        sWaveHdr[i].lpData = (LPSTR)sWaveData[i];
        sWaveHdr[i].dwBufferLength = sizeof(sWaveData[i]);
        waveOutPrepareHeader(sWaveOut, &sWaveHdr[i], sizeof(WAVEHDR));
        audio_fill_block(i);
        waveOutWrite(sWaveOut, &sWaveHdr[i], sizeof(WAVEHDR));
    }
}

static void audio_pump(void)
{
    int i;

    if (sWaveOut == NULL)
    {
        return;
    }
    for (i = 0; i < AUDIO_BLOCKS; i++)
    {
        if (sWaveHdr[i].dwFlags & WHDR_DONE)
        {
            audio_fill_block(i);
            waveOutWrite(sWaveOut, &sWaveHdr[i], sizeof(WAVEHDR));
        }
    }
}

/* ---- game thread ------------------------------------------------------------------------ */
static DWORD WINAPI game_thread(void *param)
{
    ps1_backend_run();
    host_log("game returned from main()");
    if (sShm) sShm->status = PORT_SHM_STATUS_EXITED;
    ExitProcess(0);
    return 0;
}

/* ---- window ------------------------------------------------------------------------------ */
/* Alt+Enter / F11: borderless fullscreen on the window's monitor and back. */
static void toggle_fullscreen(HWND hwnd)
{
    static WINDOWPLACEMENT saved = {sizeof(WINDOWPLACEMENT)};
    DWORD style = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);

    if (style & WS_OVERLAPPEDWINDOW)
    {
        MONITORINFO mi = {sizeof(mi)};

        if (GetWindowPlacement(hwnd, &saved) && GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
        {
            SetWindowLongA(hwnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    }
    else
    {
        SetWindowLongA(hwnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(hwnd, &saved);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;

        GetClientRect(hwnd, &rc);
        EnterCriticalSection(&sFrameLock);
        if (sFrame)
        {
            BITMAPINFO bmi;
            int w = rc.right, h = rc.bottom, dw, dh, dx, dy;

            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
            bmi.bmiHeader.biWidth = sFrameW;
            bmi.bmiHeader.biHeight = -sFrameH;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            /* keep 4:3 */
            dw = w; dh = w * 3 / 4;
            if (dh > h) { dh = h; dw = h * 4 / 3; }
            dx = (w - dw) / 2; dy = (h - dh) / 2;
            {
                /* black bars around the 4:3 picture */
                HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
                RECT bar;

                SetRect(&bar, 0, 0, w, dy); FillRect(dc, &bar, black);
                SetRect(&bar, 0, dy + dh, w, h); FillRect(dc, &bar, black);
                SetRect(&bar, 0, dy, dx, dy + dh); FillRect(dc, &bar, black);
                SetRect(&bar, dx + dw, dy, w, dy + dh); FillRect(dc, &bar, black);
            }
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, dx, dy, dw, dh, 0, 0, sFrameW, sFrameH, sFrame, &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        LeaveCriticalSection(&sFrameLock);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        if ((wp == VK_RETURN && (msg == WM_SYSKEYDOWN)) || wp == VK_F11)
        {
            toggle_fullscreen(hwnd);
            return 0;
        }
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void swap_rb_for_gdi(void)
{
    /* GDI wants BGRA; the guest produces RGBA. Done once per new frame. */
    int i;

    EnterCriticalSection(&sFrameLock);
    for (i = 0; i < sFrameW * sFrameH; i++)
    {
        unsigned char t = sFrame[i * 4];

        sFrame[i * 4] = sFrame[i * 4 + 2];
        sFrame[i * 4 + 2] = t;
    }
    LeaveCriticalSection(&sFrameLock);
}

/* No --disc: the image named by the game package (baked in at configure time), else
 * a file of the same name next to the executable. */
static void find_default_disc(void)
{
    char path[MAX_PATH], *slash;

    if (PS1_DEFAULT_DISC[0] && GetFileAttributesA(PS1_DEFAULT_DISC) != INVALID_FILE_ATTRIBUTES)
    {
        snprintf(sDiscPath, sizeof(sDiscPath), "%s", PS1_DEFAULT_DISC);
        return;
    }
    /* the disc extracted for the game package (ps1_add_game EXTRACT_TO) */
    snprintf(path, sizeof(path), "%s\\disc.idx", PS1_EXTRACTED_DISC);
    if (PS1_EXTRACTED_DISC[0] && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
    {
        snprintf(sDiscPath, sizeof(sDiscPath), "%s", PS1_EXTRACTED_DISC);
        return;
    }
    GetModuleFileNameA(NULL, path, sizeof(path));
    slash = strrchr(path, '\\');
    if (slash && PS1_DEFAULT_DISC_NAME[0])
    {
        snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "%s", PS1_DEFAULT_DISC_NAME);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        {
            snprintf(sDiscPath, sizeof(sDiscPath), "%s", path);
        }
    }
}

int main(int argc, char **argv)
{
    HANDLE thread;
    int i;
    LARGE_INTEGER freq, start, now;
    unsigned long long next_tick;
    LONG presented = 0;

    InitializeCriticalSection(&sFrameLock);
    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc) strncpy(sDiscPath, argv[++i], sizeof(sDiscPath) - 1);
        else if (!strcmp(argv[i], "--headless")) sHeadless = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) sFramesLimit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) sDumpDir = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) sDumpEvery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) parse_script(argv[++i]);
        else if (!strcmp(argv[i], "--dump-from") && i + 1 < argc) sDumpFrom = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-vram")) sDumpVram = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
        {
            sWavFile = fopen(argv[++i], "wb");
            if (sWavFile) wav_header(sWavFile, 0);
        }
        else if (!strcmp(argv[i], "--trace-gpu") && i + 1 < argc) sTraceGpuFrame = atoi(argv[++i]);
        else if (!strncmp(argv[i], "--debug-", 8) && i + 1 < argc && sDebugCount < MAX_DEBUG_ARGS)
        {
            sDebugNames[sDebugCount] = argv[i] + 8;
            sDebugValues[sDebugCount++] = argv[++i];
        }
        else if (!strcmp(argv[i], "--press-from") && i + 1 < argc) sPressFrom = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--press-every") && i + 1 < argc) sPressEvery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--press-buttons") && i + 1 < argc) sPressButtons = strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) sScale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shm") && i + 1 < argc) sShmName = argv[++i];
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) snprintf(sSaveDir, sizeof(sSaveDir), "%s\\", argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) sLogFile = fopen(argv[++i], "w");
        else
        {
            fprintf(stderr, "usage: dw [--disc image.bin] [--headless --frames N [--dump dir] [--every N]] [--shm name]\n");
            return 2;
        }
    }
    if (sDiscPath[0] == 0)
    {
        find_default_disc();
    }
    if (sSaveDir[0] == 0)
    {
        char exe[MAX_PATH], *slash;

        GetModuleFileNameA(NULL, exe, sizeof(exe));
        slash = strrchr(exe, '\\');
        if (slash) slash[1] = 0;
        snprintf(sSaveDir, sizeof(sSaveDir), "%ssaves\\", exe);
    }
    CreateDirectoryA(sSaveDir, NULL);
    if (sShmName)
    {
        sShmMapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, sShmName);
        if (sShmMapping)
        {
            sShm = (PortShm *)MapViewOfFile(sShmMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PortShm));
        }
        if (sShm == NULL)
        {
            host_log("cannot open shared memory '%s'", sShmName);
            return 1;
        }
        sHeadless = 0;
    }
    if (!ps1w_disc_open(sDiscPath))
    {
        host_log("cannot open disc '%s' (an image, or a folder extracted by extract_disc.py)", sDiscPath);
        if (sShm) sShm->status = PORT_SHM_STATUS_CRASHED;
        return 1;
    }
    if (!ps1_backend_init())
    {
        if (sShm) sShm->status = PORT_SHM_STATUS_CRASHED;
        return 1;
    }
    sVblankEvent = CreateEventA(NULL, FALSE, FALSE, NULL);

    if (!sHeadless && !sShm)
    {
        WNDCLASSA wc;
        RECT rc = { 0, 0, 320 * sScale, 240 * sScale };

        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "Ps1RecompWindow";
        RegisterClassA(&wc);
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        sWindow = CreateWindowA(wc.lpszClassName, PS1_GAME_TITLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
    }
    if (!sHeadless)
    {
        audio_open();
    }
    if (sShm) sShm->status = PORT_SHM_STATUS_RUNNING;

    thread = CreateThread(NULL, ps1_backend_stack_size(), game_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (sHeadless)
    {
        WaitForSingleObject(thread, INFINITE);
        return 0;
    }

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    next_tick = 0;
    timeBeginPeriod(1);
    for (;;)
    {
        MSG msg;
        unsigned long long elapsed;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                ExitProcess(0);
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (sShm)
        {
            if (sShm->command == PORT_SHM_CMD_QUIT || WaitForSingleObject(GetCurrentProcess(), 0) == WAIT_OBJECT_0)
            {
                ExitProcess(0);
            }
            sPad = (LONG)sShm->pad;
        }
        else
        {
            sPad = (LONG)(poll_keyboard() | poll_xinput());
        }
        QueryPerformanceCounter(&now);
        elapsed = (unsigned long long)(now.QuadPart - start.QuadPart) * 60000000ull / (unsigned long long)freq.QuadPart;
        if (elapsed >= next_tick)
        {
            /* NTSC: 59.94 Hz, close enough to 60 for pacing */
            InterlockedIncrement(&sVblank);
            SetEvent(sVblankEvent);
            next_tick += 1000000;
            if (elapsed > next_tick + 5000000) next_tick = elapsed; /* do not try to catch up after a stall */
        }
        if (sWindow && sFrameSerial != presented)
        {
            presented = sFrameSerial;
            swap_rb_for_gdi();
            InvalidateRect(sWindow, NULL, FALSE);
        }
        audio_pump();
        Sleep(1);
    }
}
