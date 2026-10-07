/**
 * @file Ps1GuestHost.cpp
 * @brief port_host.h on top of the engine, for the wasm2c guest (see Ps1GuestHost.h).
 */

#include "Ps1GuestHost.h"

#include "Log.h"
#include "System/System.h"

#include "Wasm/ps1w_bridge.h"
#include "Wasm/ps1w_module.h"
#include "../Runtime/port/include/port_bridge.h"

#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <deque>
#include <map>

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#elif PLATFORM_DOLPHIN
#include <ogc/lwp.h>
#include <malloc.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

extern "C" {
#include "../Runtime/port/include/port_host.h"
int ps1w_disc_open(const char* path);
void ps1w_disc_close(void);
void host_log(const char* fmt, ...);
void host_crashed(void);
// (Wasm/ps1w_backend.c) recomp mode's threads: back to the start thread before leaving
extern int (*ps1w_unwind_hook)(int code);
}

namespace
{
// ---- state ---------------------------------------------------------------------------
const Ps1wModule* sModule = nullptr;
volatile Ps1GuestHost::State sState = Ps1GuestHost::State::Stopped;
volatile bool sStopRequested = false;
std::jmp_buf sExitJump;
std::string sSaveDir;

volatile uint32_t sVblank = 0;
// The game's vblanks: one per wait, so that the host's coming several at once (an editor below
// 60 fps adds two or more per frame) still reach the game one at a time, as the PS1's do - a game
// that waits for "a vblank since the last frame" runs a frame for each. It keeps within
// kMaxVblankLag of the host's: a game slower than the clock sees the time it missed.
volatile uint32_t sGuestVblank = 0;
constexpr uint32_t kMaxVblankLag = 4;
volatile uint32_t sPad = 0;
std::string sOptions;

// Fast-forward (port_set_speed): of every sSpeed vblank waits only one blocks; the
// others return at once and count as vblanks the game ran ahead (game thread only).
volatile int sSpeed = 1;
volatile uint32_t sExtraVblanks = 0;
uint32_t sFastWaits = 0;
// waits since the game last asked for its speed: fast-forward has to be refreshed (a
// game loop that stops asking, e.g. a battle started from a scene, falls back to 1x)
uint32_t sWaitsSinceSpeed = 0;
const uint32_t kSpeedTimeout = 30;

MutexObject* sLock = nullptr; // frame, log
std::vector<uint8_t> sFrame;  // written by the game thread
std::vector<uint8_t> sFrameOut; // handed to the main thread
int sFrameW = 0, sFrameH = 0;
volatile uint32_t sFrameSerial = 0;
std::vector<std::string> sLogLines;

// script bridge (under sLock): the game's tables, queued requests and finished ones
struct PendingRequest
{
    int id;
    std::string name;
    std::vector<int> args;
};
std::vector<Ps1GuestHost::BridgeVar> sBridgeVars;
std::vector<Ps1GuestHost::BridgeRequestInfo> sBridgeRequests;
std::deque<PendingRequest> sBridgeQueue;
std::map<int, int> sBridgeResults;
int sBridgeNextId = 1;
const size_t kBridgeQueueMax = 64;
const size_t kBridgeResultsMax = 256;

void ClearBridge()
{
    sBridgeVars.clear();
    sBridgeRequests.clear();
    sBridgeQueue.clear();
    sBridgeResults.clear();
}

const uint32_t kAudioRing = 32768; // frames, power of two
int16_t sAudio[kAudioRing * 2];
volatile uint32_t sAudioWrite = 0, sAudioRead = 0;

#if PLATFORM_WINDOWS
HANDLE sThread = nullptr;
HANDLE sVblankEvent = nullptr;
#elif PLATFORM_DOLPHIN
lwp_t sThread = LWP_THREAD_NULL;
void* sThreadStack = nullptr;
// the game thread peaks at about 6 KB of stack (measured on Wii); keep plenty spare
const uint32_t kStackSize = 256u << 10;
#endif

void Lock()
{
    SYS_LockMutex(sLock);
}

void Unlock()
{
    SYS_UnlockMutex(sLock);
}

// Leaves the guest when the player stops it; called from the imports it blocks in.
void CheckStop()
{
    if (sStopRequested)
    {
        // (a recompiled game's thread: on the start thread's stack first, which comes back to
        // host_unwind)
        if (ps1w_unwind_hook != nullptr && ps1w_unwind_hook(2))
        {
            return;
        }
        std::longjmp(sExitJump, 2);
    }
}

void SleepBriefly()
{
#if PLATFORM_WINDOWS
    WaitForSingleObject(sVblankEvent, 20);
#elif PLATFORM_DOLPHIN
    usleep(500);
#else
    SYS_Sleep(1);
#endif
}

void GameThreadBody()
{
    switch (setjmp(sExitJump))
    {
    case 0:
#if defined(_MSC_VER) && defined(_M_X64)
        // Stop and crash come back here with longjmp without unwinding (as on the other hosts):
        // a recompiled game's code (LiveRecomp) has no unwind data, which an unwinding longjmp
        // fails on (STATUS_BAD_FUNCTION_TABLE), and no frame in between holds anything to destroy
        reinterpret_cast<_JUMP_BUFFER*>(&sExitJump)->Frame = 0;
#endif
        ps1w_run();
        host_log("game returned from main()");
        sState = Ps1GuestHost::State::Exited;
        break;
    case 1:
        sState = Ps1GuestHost::State::Crashed;
        break;
    default:
        sState = Ps1GuestHost::State::Stopped;
        break;
    }
}

#if PLATFORM_WINDOWS
DWORD WINAPI GameThread(void*)
{
    GameThreadBody();
    return 0;
}
#elif PLATFORM_DOLPHIN
void* GameThread(void*)
{
    GameThreadBody();
    return nullptr;
}
#endif

std::string SavePath(const char* rel)
{
    return sSaveDir + rel;
}

void MakeDir(const std::string& path)
{
#if PLATFORM_WINDOWS
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0777);
#endif
}

void MakeParentDirs(const std::string& path)
{
    for (size_t i = 1; i < path.size(); ++i)
    {
        if (path[i] == '/' || path[i] == '\\')
        {
            MakeDir(path.substr(0, i));
        }
    }
}
}

// ---- host functions used by the wasm2c runtime ---------------------------------------
extern "C" void host_log(const char* fmt, ...)
{
    char buf[1024];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (sLock)
    {
        Lock();
        sLogLines.push_back(buf);
        Unlock();
    }
}

extern "C" void host_crashed(void)
{
    // only ever reached on the game thread (traps, fatal errors, bad guest pointers)
    if (ps1w_unwind_hook != nullptr && ps1w_unwind_hook(1))
    {
        return;
    }
    std::longjmp(sExitJump, 1);
}

// recomp mode's threads: leaving the game from the start thread's stack (CheckStop, host_crashed)
extern "C" void host_unwind(int code)
{
    std::longjmp(sExitJump, code);
}

// ---- script bridge, game thread side (Wasm/ps1w_bridge.h) ------------------------------
namespace
{
uint32_t ReadLE32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

std::string GuestString(uint32_t addr)
{
    std::string out;
    for (uint32_t i = 0; addr != 0 && i < 256; ++i)
    {
        const uint8_t* p = (const uint8_t*)ps1w_guest_ptr(addr + i, 1);
        if (p == nullptr || *p == 0)
        {
            break;
        }
        out.push_back((char)*p);
    }
    return out;
}

int ElementSize(int type)
{
    switch (type)
    {
    case PB_U16:
    case PB_S16: return 2;
    case PB_U32:
    case PB_S32: return 4;
    default: return 1;
    }
}
}

extern "C" void ps1w_host_bridge_publish(unsigned vars, int nvars, unsigned requests, int nrequests)
{
    std::vector<Ps1GuestHost::BridgeVar> parsedVars;
    std::vector<Ps1GuestHost::BridgeRequestInfo> parsedRequests;

    // wasm32 layout: PortBridgeVar = 6 words, PortBridgeRequest = 3 words
    for (int i = 0; i < nvars; ++i)
    {
        const uint8_t* e = (const uint8_t*)ps1w_guest_ptr(vars + (uint32_t)i * 24, 24);
        if (e == nullptr)
        {
            break;
        }
        Ps1GuestHost::BridgeVar v;
        v.name = GuestString(ReadLE32(e));
        v.addr = ReadLE32(e + 4);
        v.type = (int)ReadLE32(e + 8);
        v.count = (int)ReadLE32(e + 12);
        v.stride = (int)ReadLE32(e + 16);
        v.help = GuestString(ReadLE32(e + 20));
        if (v.type == PB_STR && v.stride == 0)
        {
            v.stride = v.count; // one string of `count` bytes
            v.count = 1;
        }
        if (v.stride == 0)
        {
            v.stride = ElementSize(v.type);
        }
        parsedVars.push_back(v);
    }
    for (int i = 0; i < nrequests; ++i)
    {
        const uint8_t* e = (const uint8_t*)ps1w_guest_ptr(requests + (uint32_t)i * 12, 12);
        if (e == nullptr)
        {
            break;
        }
        Ps1GuestHost::BridgeRequestInfo r;
        r.name = GuestString(ReadLE32(e));
        r.help = GuestString(ReadLE32(e + 8));
        parsedRequests.push_back(r);
    }

    Lock();
    sBridgeVars.swap(parsedVars);
    sBridgeRequests.swap(parsedRequests);
    Unlock();
}

extern "C" int ps1w_host_bridge_poll(char* name, unsigned nameCap, int* args, int maxArgs, int* nargs)
{
    *nargs = 0;
    Lock();
    if (sBridgeQueue.empty())
    {
        Unlock();
        return 0;
    }
    PendingRequest request = sBridgeQueue.front();
    sBridgeQueue.pop_front();
    Unlock();

    snprintf(name, nameCap, "%s", request.name.c_str());
    for (size_t i = 0; i < request.args.size() && (int)i < maxArgs; ++i)
    {
        args[i] = request.args[i];
        *nargs = (int)i + 1;
    }
    return request.id;
}

extern "C" void ps1w_host_bridge_done(int id, int result)
{
    Lock();
    if (sBridgeResults.size() >= kBridgeResultsMax)
    {
        sBridgeResults.erase(sBridgeResults.begin()); // nobody collected the oldest
    }
    sBridgeResults[id] = result;
    Unlock();
}

// ---- port_host.h ---------------------------------------------------------------------
extern "C" void port_host_log(const char* msg)
{
    host_log("%s", msg);
}

extern "C" void port_host_fatal(const char* msg)
{
    host_log("FATAL: %s", msg);
    host_crashed();
}

extern "C" void port_set_speed(int multiplier);

extern "C" unsigned port_wait_vblank(void)
{
    if (sSpeed > 1 && ++sWaitsSinceSpeed > kSpeedTimeout)
    {
        host_log("fast-forward expired (not refreshed)");
        port_set_speed(1);
    }
    if (sSpeed > 1 && (++sFastWaits % (uint32_t)sSpeed) != 0)
    {
        CheckStop();
        sExtraVblanks = sExtraVblanks + 1;
        return sGuestVblank + sExtraVblanks;
    }

    if (sVblank - sGuestVblank > kMaxVblankLag)
    {
        sGuestVblank = sVblank - kMaxVblankLag;
    }
    while (sVblank == sGuestVblank)
    {
        CheckStop();
        SleepBriefly();
    }
    sGuestVblank = sGuestVblank + 1;
    return sGuestVblank + sExtraVblanks;
}

extern "C" unsigned port_vblank_count(void)
{
    CheckStop();
    return sGuestVblank + sExtraVblanks;
}

extern "C" int port_speed(void)
{
    return sSpeed;
}

extern "C" void port_set_speed(int multiplier)
{
    const int speed = multiplier < 1 ? 1 : (multiplier > 8 ? 8 : multiplier);

    sWaitsSinceSpeed = 0;
    if (speed != sSpeed)
    {
        host_log("fast-forward %s (x%d)", speed > 1 ? "on" : "off", speed);
        sSpeed = speed;
    }
}

extern "C" unsigned port_pad_state(void)
{
    CheckStop();
    return sPad;
}

extern "C" void port_present(const unsigned char* rgba, int width, int height)
{
    if (width <= 0 || height <= 0 || width > 1024 || height > 512)
    {
        return;
    }
    Lock();
    sFrame.assign(rgba, rgba + size_t(width) * size_t(height) * 4);
    sFrameW = width;
    sFrameH = height;
    sFrameSerial++;
    Unlock();
}

extern "C" int port_debug_values(const char* name, int* out, int max)
{
    // sOptions: "name=v1,v2 name2=v" (spaces or ';' between entries)
    const size_t nameLen = strlen(name);
    size_t pos = 0;

    while (pos < sOptions.size())
    {
        while (pos < sOptions.size() && (sOptions[pos] == ' ' || sOptions[pos] == ';' || sOptions[pos] == '\t'))
        {
            ++pos;
        }
        size_t end = pos;
        while (end < sOptions.size() && sOptions[end] != ' ' && sOptions[end] != ';' && sOptions[end] != '\t')
        {
            ++end;
        }
        if (end - pos > nameLen && sOptions.compare(pos, nameLen, name) == 0 && sOptions[pos + nameLen] == '=')
        {
            int count = 0;
            const char* p = sOptions.c_str() + pos + nameLen + 1;
            const char* stop = sOptions.c_str() + end;

            while (p < stop && count < max)
            {
                out[count++] = (int)strtol(p, nullptr, 0);
                while (p < stop && *p != ',')
                {
                    ++p;
                }
                if (p < stop)
                {
                    ++p;
                }
            }
            return count;
        }
        pos = end;
    }
    return 0;
}

extern "C" int port_trace_gpu(void)
{
    return 0;
}

extern "C" void port_debug_vram(const unsigned short* vram)
{
}

// Engine audio streams take 16-bit little-endian PCM on every platform (the Wii backend
// opens its voices as VOICE_STEREO_16BIT_LE).
static inline int16_t ToLittleEndian(int16_t v)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return (int16_t)(((uint16_t)v >> 8) | ((uint16_t)v << 8));
#else
    return v;
#endif
}

extern "C" void port_audio_push(const short* samples, int frames)
{
    if (sSpeed > 1)
    {
        return; // fast-forward: the sound would be sped up or pile up, drop it
    }
    for (int i = 0; i < frames; ++i)
    {
        uint32_t w = sAudioWrite;

        if (w - sAudioRead >= kAudioRing)
        {
            break; // the player is not draining (paused): drop
        }
        sAudio[(w % kAudioRing) * 2] = ToLittleEndian(samples[i * 2]);
        sAudio[(w % kAudioRing) * 2 + 1] = ToLittleEndian(samples[i * 2 + 1]);
        sAudioWrite = w + 1;
    }
}

extern "C" int port_file_size(const char* rel)
{
    FILE* f = fopen(SavePath(rel).c_str(), "rb");
    long size;

    if (f == nullptr)
    {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
    return (int)size;
}

extern "C" int port_file_read(const char* rel, unsigned offset, void* dst, unsigned len)
{
    FILE* f = fopen(SavePath(rel).c_str(), "rb");
    size_t got;

    if (f == nullptr)
    {
        return -1;
    }
    fseek(f, (long)offset, SEEK_SET);
    got = fread(dst, 1, len, f);
    fclose(f);
    return (int)got;
}

extern "C" int port_file_write(const char* rel, unsigned offset, const void* src, unsigned len, int create)
{
    const std::string path = SavePath(rel);
    FILE* f;
    size_t put;

    MakeParentDirs(path);
    f = fopen(path.c_str(), create ? "wb" : "r+b");
    if (f == nullptr)
    {
        return -1;
    }
    fseek(f, (long)offset, SEEK_SET);
    put = fwrite(src, 1, len, f);
    fclose(f);
    return (int)put;
}

extern "C" int port_file_delete(const char* rel)
{
    return remove(SavePath(rel).c_str()) == 0 ? 0 : -1;
}

extern "C" int port_file_list(const char* relDir, unsigned index, char* name, unsigned nameCap)
{
    std::vector<std::string> names;
    const std::string dir = SavePath(relDir);

#if PLATFORM_WINDOWS
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);

    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                names.push_back(fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR* d = opendir(dir.c_str());

    if (d != nullptr)
    {
        struct dirent* e;

        while ((e = readdir(d)) != nullptr)
        {
            struct stat st;
            const std::string full = dir + "/" + e->d_name;

            if (e->d_name[0] != '.' && stat(full.c_str(), &st) == 0 && !S_ISDIR(st.st_mode))
            {
                names.push_back(e->d_name);
            }
        }
        closedir(d);
    }
#endif
    // name order on every platform
    std::sort(names.begin(), names.end());
    if (index >= names.size())
    {
        return -1;
    }
    snprintf(name, nameCap, "%s", names[index].c_str());
    return port_file_size((std::string(relDir) + "/" + names[index]).c_str());
}

// ---- player side ----------------------------------------------------------------------
bool Ps1GuestHost::Start(const Ps1wModule* module, const std::string& discPath, const std::string& saveDir)
{
    if (sState == State::Running)
    {
        LogError("Ps1Player: a PS1 game is already running");
        return false;
    }
    if (sLock == nullptr)
    {
        sLock = SYS_CreateMutex();
    }
    if (!ps1w_disc_open(discPath.c_str()))
    {
        LogError("Ps1Player: cannot open the disc image '%s'", discPath.c_str());
        return false;
    }
    sSaveDir = saveDir;
    if (!sSaveDir.empty() && sSaveDir.back() != '/' && sSaveDir.back() != '\\')
    {
        sSaveDir += "/";
    }
    MakeParentDirs(sSaveDir);

    sStopRequested = false;
    sVblank = 0;
    sGuestVblank = 0;
    sSpeed = 1;
    sExtraVblanks = 0;
    sFastWaits = 0;
    sWaitsSinceSpeed = 0;
    sPad = 0;
    sFrameSerial = 0;
    if (sLock)
    {
        Lock();
        ClearBridge();
        Unlock();
    }
    sAudioWrite = sAudioRead = 0;
    if (!ps1w_instantiate(module))
    {
        LogError("Ps1Player: cannot set up %s", module->title);
        ps1w_disc_close();
        return false;
    }
    sModule = module;
    sState = State::Running;

#if PLATFORM_WINDOWS
    if (sVblankEvent == nullptr)
    {
        sVblankEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    }
    // wasm2c code keeps the wasm locals on the C stack: reserve plenty
    sThread = CreateThread(nullptr, 16u << 20, GameThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (sThread == nullptr)
#elif PLATFORM_DOLPHIN
    if (sThreadStack == nullptr)
    {
        sThreadStack = memalign(32, kStackSize);
    }
    if (sThreadStack == nullptr ||
        LWP_CreateThread(&sThread, GameThread, nullptr, sThreadStack, kStackSize, 48) != 0)
#else
    if (true)
#endif
    {
        LogError("Ps1Player: cannot start the game thread on this platform");
        sState = State::Stopped;
        ps1w_free();
        ps1w_disc_close();
        return false;
    }
    LogDebug("Ps1Player: started %s (%s)", module->title, discPath.c_str());
    return true;
}

void Ps1GuestHost::Stop()
{
    if (sModule == nullptr)
    {
        return;
    }
    sStopRequested = true;
#if PLATFORM_WINDOWS
    if (sThread)
    {
        SetEvent(sVblankEvent);
        if (WaitForSingleObject(sThread, 3000) != WAIT_OBJECT_0)
        {
            // stuck in a loop that never calls the host: no clean way out
            LogWarning("Ps1Player: the game thread did not stop; terminating it");
            TerminateThread(sThread, 1);
        }
        CloseHandle(sThread);
        sThread = nullptr;
    }
#elif PLATFORM_DOLPHIN
    if (sThread != LWP_THREAD_NULL)
    {
        LWP_JoinThread(sThread, nullptr);
        sThread = LWP_THREAD_NULL;
    }
#endif
    if (sLock)
    {
        Lock();
        ClearBridge();
        Unlock();
    }
    ps1w_free();
    ps1w_disc_close();
    FlushLog();
    sModule = nullptr;
    sState = State::Stopped;
}

Ps1GuestHost::State Ps1GuestHost::GetState()
{
    return sState;
}

void Ps1GuestHost::AddVblanks(uint32_t count)
{
    if (count == 0)
    {
        return;
    }
    sVblank = sVblank + count;
#if PLATFORM_WINDOWS
    if (sVblankEvent)
    {
        SetEvent(sVblankEvent);
    }
#endif
}

void Ps1GuestHost::SetPad(uint32_t bits)
{
    sPad = bits;
}

int Ps1GuestHost::GetSpeed()
{
    return sSpeed;
}

void Ps1GuestHost::SetOptions(const std::string& options)
{
    sOptions = options;
}

bool Ps1GuestHost::GetFrame(uint32_t& lastSerial, const uint8_t*& rgba, int& width, int& height)
{
    if (sLock == nullptr || sFrameSerial == lastSerial)
    {
        return false;
    }
    Lock();
    sFrameOut.swap(sFrame);
    width = sFrameW;
    height = sFrameH;
    lastSerial = sFrameSerial;
    Unlock();
    if (sFrameOut.size() < size_t(width) * size_t(height) * 4)
    {
        return false;
    }
    rgba = sFrameOut.data();
    return true;
}

uint32_t Ps1GuestHost::PeekAudio(const int16_t*& frames, uint32_t maxFrames)
{
    const uint32_t r = sAudioRead;
    uint32_t available = sAudioWrite - r;
    const uint32_t untilWrap = kAudioRing - (r % kAudioRing);

    available = (std::min)(available, (std::min)(untilWrap, maxFrames));
    frames = &sAudio[(r % kAudioRing) * 2];
    return available;
}

void Ps1GuestHost::ConsumeAudio(uint32_t frames)
{
    sAudioRead = sAudioRead + frames;
}

void Ps1GuestHost::FlushLog()
{
    std::vector<std::string> lines;

    if (sLock == nullptr)
    {
        return;
    }
    Lock();
    lines.swap(sLogLines);
    Unlock();
    for (const std::string& line : lines)
    {
        if (line.compare(0, 6, "FATAL:") == 0 || line.compare(0, 6, "CRASH:") == 0)
        {
            LogError("[ps1] %s", line.c_str());
        }
        else
        {
            LogDebug("[ps1] %s", line.c_str());
        }
    }
}

// ---- script bridge, main thread side ----------------------------------------------------
std::vector<Ps1GuestHost::BridgeVar> Ps1GuestHost::BridgeVariables()
{
    std::vector<BridgeVar> out;
    if (sLock)
    {
        Lock();
        out = sBridgeVars;
        Unlock();
    }
    return out;
}

std::vector<Ps1GuestHost::BridgeRequestInfo> Ps1GuestHost::BridgeRequests()
{
    std::vector<BridgeRequestInfo> out;
    if (sLock)
    {
        Lock();
        out = sBridgeRequests;
        Unlock();
    }
    return out;
}

static bool FindBridgeVar(const std::string& name, Ps1GuestHost::BridgeVar& out)
{
    bool found = false;
    if (sLock == nullptr || sState != Ps1GuestHost::State::Running)
    {
        return false;
    }
    Lock();
    for (const Ps1GuestHost::BridgeVar& v : sBridgeVars)
    {
        if (v.name == name)
        {
            out = v;
            found = true;
            break;
        }
    }
    Unlock();
    return found;
}

bool Ps1GuestHost::BridgeGet(const std::string& name, int index, int64_t& value)
{
    BridgeVar v;
    if (!FindBridgeVar(name, v) || v.type == PB_STR || index < 0 || index >= v.count)
    {
        return false;
    }
    const int size = ElementSize(v.type);
    const uint8_t* p =
        (const uint8_t*)ps1w_guest_ptr(v.addr + (uint32_t)index * (uint32_t)v.stride, (unsigned)size);
    if (p == nullptr)
    {
        return false;
    }
    // guest memory is little endian on every host
    switch (v.type)
    {
    case PB_U8: value = p[0]; break;
    case PB_S8: value = (int8_t)p[0]; break;
    case PB_U16: value = (uint16_t)(p[0] | (p[1] << 8)); break;
    case PB_S16: value = (int16_t)(uint16_t)(p[0] | (p[1] << 8)); break;
    case PB_U32: value = ReadLE32(p); break;
    default: value = (int32_t)ReadLE32(p); break;
    }
    return true;
}

bool Ps1GuestHost::BridgeGetString(const std::string& name, int index, std::string& value)
{
    BridgeVar v;
    if (!FindBridgeVar(name, v) || v.type != PB_STR || index < 0 || index >= v.count)
    {
        return false;
    }
    const char* p =
        (const char*)ps1w_guest_ptr(v.addr + (uint32_t)index * (uint32_t)v.stride, (unsigned)v.stride);
    if (p == nullptr)
    {
        return false;
    }
    size_t n = 0;
    while (n < (size_t)v.stride && p[n] != 0)
    {
        ++n;
    }
    value.assign(p, n);
    return true;
}

int Ps1GuestHost::BridgeRequest(const std::string& name, const std::vector<int>& args)
{
    if (sLock == nullptr || sState != State::Running)
    {
        return 0;
    }
    Lock();
    if (sBridgeQueue.size() >= kBridgeQueueMax)
    {
        Unlock();
        return 0;
    }
    const int id = sBridgeNextId++;
    if (sBridgeNextId <= 0)
    {
        sBridgeNextId = 1;
    }
    sBridgeQueue.push_back({id, name, args});
    Unlock();
    return id;
}

bool Ps1GuestHost::BridgeResult(int id, int& result)
{
    bool done = false;
    if (sLock == nullptr)
    {
        return false;
    }
    Lock();
    auto it = sBridgeResults.find(id);
    if (it != sBridgeResults.end())
    {
        result = it->second;
        sBridgeResults.erase(it);
        done = true;
    }
    Unlock();
    return done;
}
