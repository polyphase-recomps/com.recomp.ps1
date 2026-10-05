/**
 * @file Ps1Player.cpp
 * @brief Launches a com.recomp.ps1 game executable in embedded mode and streams its frames
 *        to a fullscreen Quad.
 */

#include "Ps1Player.h"

#include "AssetManager.h"
#include "Engine.h"
#include "Input/Input.h"
#include "Input/InputTypes.h"
#include "Nodes/Widgets/Quad.h"
#include "Nodes/Widgets/Text.h"
#include "Plugins/PolyphaseEngineAPI.h"

#include "../Runtime/port/include/port_shm.h"
#include "Ps1GuestHost.h"
#include "Ps1Provider.h"
#include "Ps1Widgets.h"
#include "Wasm/ps1w_module.h"

// com.recomp.mod.base: mod settings, resolution scaler, shared menus
#include "ModBaseDisplay.h"
#include "ModBaseSettings.h"
#include "ModBaseWidgets.h"

#if PLATFORM_WII
#include <ogc/system.h>
#include <wiiuse/wpad.h>
#endif

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

FORCE_LINK_DEF(Ps1Player);
DEFINE_NODE(Ps1Player, Node3D);

PolyphaseEngineAPI* Ps1Player::sAPI = nullptr;

static std::vector<Ps1Player*> sLivePlayers;

// PsyQ PadRead() bits (pressed = 1)
enum : unsigned int
{
    PAD_L2 = 1u << 0,
    PAD_R2 = 1u << 1,
    PAD_L1 = 1u << 2,
    PAD_R1 = 1u << 3,
    PAD_TRIANGLE = 1u << 4,
    PAD_CIRCLE = 1u << 5,
    PAD_CROSS = 1u << 6,
    PAD_SQUARE = 1u << 7,
    PAD_SELECT = 1u << 8,
    PAD_START = 1u << 11,
    PAD_UP = 1u << 12,
    PAD_RIGHT = 1u << 13,
    PAD_DOWN = 1u << 14,
    PAD_LEFT = 1u << 15,
};

// Input diagnostics: logs every change of the controller state at three levels so a
// run on hardware shows where a press gets lost -- the raw Wii Remote (WPAD, Wii
// builds), the engine's gamepads, and the PS1 pad bits handed to the game -- plus a
// status line every 5 seconds while nothing changes.
static const bool kLogInput = true;

static void AppendNames(std::string& out, uint32_t mask, const char* const* names, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (mask & (1u << i))
        {
            if (!out.empty() && out.back() != '[') out += ' ';
            out += names[i];
        }
    }
}

static void LogInputState(unsigned int ps1Bits, float deltaTime)
{
    static const char* const kEngineNames[] = {
        "A", "B", "C", "X", "Y", "Z", "L1", "R1", "L2", "R2", "THUMBL", "THUMBR", "START", "SELECT",
        "LEFT", "RIGHT", "UP", "DOWN", "LS_LEFT", "LS_RIGHT", "LS_UP", "LS_DOWN", "RS_LEFT", "RS_RIGHT",
        "RS_UP", "RS_DOWN", "HOME"};
    static const char* const kPs1Names[] = {
        "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE", "SELECT", "?9", "?10", "START",
        "UP", "RIGHT", "DOWN", "LEFT"};
    static const char* const kTypeNames[] = {"Standard", "GameCube", "Wiimote", "WiiClassic", "DS4", "DualSense"};
    static_assert(sizeof(kEngineNames) / sizeof(kEngineNames[0]) == GAMEPAD_BUTTON_COUNT, "engine button names");

    struct Snapshot
    {
        uint32_t wpadHeld[4];
        int32_t wpadErr[4];
        int32_t wpadExp[4];
        uint32_t engineButtons[INPUT_MAX_GAMEPADS];
        bool connected[INPUT_MAX_GAMEPADS];
        int32_t type[INPUT_MAX_GAMEPADS];
        unsigned int ps1;
        bool operator!=(const Snapshot& o) const { return memcmp(this, &o, sizeof(*this)) != 0; }
    };
    static Snapshot sLast;
    static bool sHaveLast = false;
    static float sSinceLog = 0.0f;

    Snapshot now;
    memset(&now, 0, sizeof(now));
#if PLATFORM_WII
    for (int32_t chan = 0; chan < 4; ++chan)
    {
        WPADData* data = WPAD_Data(chan);
        now.wpadErr[chan] = data ? data->err : -999;
        now.wpadHeld[chan] = WPAD_ButtonsHeld(chan);
        struct expansion_t exp;
        WPAD_Expansion(chan, &exp);
        now.wpadExp[chan] = exp.type;
    }
#endif
    for (int32_t pad = 0; pad < INPUT_MAX_GAMEPADS; ++pad)
    {
        now.connected[pad] = INP_IsGamepadConnected(pad);
        now.type[pad] = (int32_t)INP_GetGamepadType(pad);
        for (int32_t button = 0; button < GAMEPAD_BUTTON_COUNT; ++button)
        {
            if (INP_IsGamepadButtonDown(button, pad)) now.engineButtons[pad] |= 1u << button;
        }
    }
    now.ps1 = ps1Bits;

    sSinceLog += deltaTime;
    const bool changed = !sHaveLast || now != sLast;
    if (!changed && sSinceLog < 5.0f)
    {
        return;
    }
    sLast = now;
    sHaveLast = true;
    sSinceLog = 0.0f;

    std::string line = changed ? "[ps1 input]" : "[ps1 input, no change]";
#if PLATFORM_WII
    for (int32_t chan = 0; chan < 4; ++chan)
    {
        char buf[96];
        snprintf(buf, sizeof(buf), " wpad%d(err=%d exp=%d held=%08x)", chan, now.wpadErr[chan], now.wpadExp[chan],
                 now.wpadHeld[chan]);
        line += buf;
    }
#endif
    for (int32_t pad = 0; pad < INPUT_MAX_GAMEPADS; ++pad)
    {
        if (!now.connected[pad] && now.engineButtons[pad] == 0 && pad > 0) continue;
        const int32_t type = now.type[pad];
        char buf[96];
        snprintf(buf, sizeof(buf), " pad%d(%s %s [", pad, now.connected[pad] ? "connected" : "DISCONNECTED",
                 (type >= 0 && type < 6) ? kTypeNames[type] : "?");
        std::string names = buf;
        AppendNames(names, now.engineButtons[pad], kEngineNames, GAMEPAD_BUTTON_COUNT);
        line += names + "])";
    }
    std::string ps1 = " -> ps1[";
    AppendNames(ps1, now.ps1, kPs1Names, 16);
    line += ps1 + "]";
    LogDebug("%s", line.c_str());
}

Ps1Player::Ps1Player()
{
    // Paths left empty come from the game package's game.json; explicit relative
    // paths are resolved against the project directory.
    mGame = "com.recomp.digimonworld";
}

Ps1Player::~Ps1Player()
{
}

void Ps1Player::SetEngineAPI(PolyphaseEngineAPI* api)
{
    sAPI = api;
}

void Ps1Player::ShutdownAll()
{
    std::vector<Ps1Player*> players = sLivePlayers;
    for (Ps1Player* player : players)
    {
        player->StopGame();
    }
}

void Ps1Player::Create()
{
    Node3D::Create();
    SetName("Ps1Player");
    EnsureDisplayQuad();
    sLivePlayers.push_back(this);
}

void Ps1Player::Destroy()
{
    StopGame();
    for (size_t i = 0; i < sLivePlayers.size(); ++i)
    {
        if (sLivePlayers[i] == this)
        {
            sLivePlayers.erase(sLivePlayers.begin() + i);
            break;
        }
    }

    // Only touch the quad through the WeakPtr: an auto-created child is already gone here.
    if (Quad* quad = mBoundQuad.Get())
    {
        quad->SetTexture(nullptr);
    }
    mDisplayQuad = nullptr;
    mBoundQuad = WeakPtr<Quad>();

    mFrameTexture = nullptr;
    Node3D::Destroy();
}

void Ps1Player::GatherProperties(std::vector<Property>& outProps)
{
    Node3D::GatherProperties(outProps);
    outProps.push_back(Property(DatumType::String, "Game", this, &mGame));
    outProps.push_back(Property(DatumType::String, "Game Executable", this, &mExePath));
    outProps.push_back(Property(DatumType::String, "Disc Image", this, &mDiscPath));
    outProps.push_back(Property(DatumType::String, "Save Folder", this, &mSaveDir));
}

void Ps1Player::SaveStream(Stream& stream, Platform platform)
{
    Node3D::SaveStream(stream, platform);
    stream.WriteString(mGame);
    stream.WriteString(mExePath);
    stream.WriteString(mDiscPath);
    stream.WriteString(mSaveDir);
}

void Ps1Player::LoadStream(Stream& stream, Platform platform, uint32_t version)
{
    Node3D::LoadStream(stream, platform, version);
    stream.ReadString(mGame);
    stream.ReadString(mExePath);
    stream.ReadString(mDiscPath);
    stream.ReadString(mSaveDir);
}

std::string Ps1Player::ResolvePath(const std::string& path) const
{
    const bool isAbsolute = path.size() > 1 && (path[1] == ':' || path[0] == '/' || path[0] == '\\');
    std::string full = isAbsolute ? path : GetEngineState()->mProjectDirectory + path;
#if PLATFORM_WINDOWS
    for (char& c : full)
    {
        if (c == '/')
        {
            c = '\\';
        }
    }
    // packaged games have a relative project directory; the game process gets absolute paths
    char absolute[MAX_PATH];
    if (GetFullPathNameA(full.c_str(), sizeof(absolute), absolute, nullptr) != 0)
    {
        full = absolute;
    }
#endif
    return full;
}

// Value of a top-level string field in a flat JSON object (game.json); false if absent.
static bool ReadJsonString(const std::string& text, const char* key, std::string& out)
{
    const std::string quoted = std::string("\"") + key + "\"";
    size_t pos = text.find(quoted);
    if (pos == std::string::npos)
    {
        return false;
    }
    pos = text.find(':', pos + quoted.size());
    if (pos == std::string::npos)
    {
        return false;
    }
    pos = text.find('"', pos + 1);
    if (pos == std::string::npos)
    {
        return false;
    }
    out.clear();
    for (++pos; pos < text.size() && text[pos] != '"'; ++pos)
    {
        char c = text[pos];
        if (c == '\\' && pos + 1 < text.size())
        {
            c = text[++pos];
        }
        out.push_back(c);
    }
    return true;
}

// "holdskip=2 pausemenu=1" with name=value set (replaced, or added)
static std::string SetOption(const std::string& options, const std::string& name, int value)
{
    std::istringstream words(options);
    std::string word, out;
    while (words >> word)
    {
        const std::string key = word.substr(0, word.find('='));
        if (key != name)
        {
            out += (out.empty() ? "" : " ") + word;
        }
    }
    return out + (out.empty() ? "" : " ") + name + "=" + std::to_string(value);
}

void Ps1Player::ResolveGameDefaults(std::string& exe, std::string& disc, std::string& saves) const
{
    exe = mExePath;
    disc = mDiscPath;
    saves = mSaveDir;
    mGameOptions.clear();
    if (mGame.empty())
    {
        return;
    }

    // game.json lives in the game package's Assets/ (shipped with packaged games) or, in
    // older layouts, the package root. exe and disc are relative to its folder, saves to
    // the project.
    std::string packageDir = "Packages/" + mGame + "/Assets/";
    std::ifstream file(ResolvePath(packageDir + "game.json"), std::ios::binary);
    if (!file)
    {
        packageDir = "Packages/" + mGame + "/";
        file.open(ResolvePath(packageDir + "game.json"), std::ios::binary);
    }
    if (!file)
    {
        LogWarning("Ps1Player: no game.json in %sAssets", ResolvePath(packageDir).c_str());
        return;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    std::string value;
    if (exe.empty() && ReadJsonString(text, "exe", value) && !value.empty())
    {
        exe = packageDir + value;
    }
    if (disc.empty() && ReadJsonString(text, "disc", value) && !value.empty())
    {
        disc = packageDir + value;
    }
    if (saves.empty() && ReadJsonString(text, "saves", value) && !value.empty())
    {
        saves = value;
    }
    // game options / mods, "name=v1,v2 ..." (see Ps1GuestHost::SetOptions)
    ReadJsonString(text, "options", mGameOptions);
    // the end user's mod settings (com.recomp.mod.base) override game.json's options
    for (const auto& option : ModSettings::Get().StartupOptions(mGame))
    {
        mGameOptions = SetOption(mGameOptions, option.first, option.second);
    }
}

// "holdskip=2 x=1,2" -> " --debug-holdskip 2 --debug-x 1,2" (the Windows host's options)
[[maybe_unused]] static std::string OptionsToArgs(const std::string& options)
{
    std::string args;
    size_t pos = 0;
    while (pos < options.size())
    {
        while (pos < options.size() && (options[pos] == ' ' || options[pos] == ';'))
        {
            ++pos;
        }
        size_t end = pos;
        while (end < options.size() && options[end] != ' ' && options[end] != ';')
        {
            ++end;
        }
        const size_t eq = options.find('=', pos);
        if (eq != std::string::npos && eq < end)
        {
            args += " --debug-" + options.substr(pos, eq - pos) + " " + options.substr(eq + 1, end - eq - 1);
        }
        pos = end;
    }
    return args;
}

bool Ps1Player::StartGame()
{
    // A game translated into the addon (Source/Guest) runs in-process on any platform;
    // otherwise, on Windows, the game package's executable runs as a child process.
    if (const Ps1wModule* module = ps1w_find_module(mGame.c_str()))
    {
        return StartGuest(module);
    }
    return StartProcess();
}

void Ps1Player::StopGame()
{
    if (mInProcess)
    {
        StopGuest();
    }
    else
    {
        StopProcess();
    }
    mStartAttempted = false;
}

#if PLATFORM_WINDOWS

bool Ps1Player::StartProcess()
{
    std::string exePath, discPath, saveDir;
    ResolveGameDefaults(exePath, discPath, saveDir);
    if (exePath.empty())
    {
        LogError("Ps1Player: no executable (set Game to a package with a game.json, or set Game Executable)");
        return false;
    }
    if (saveDir.empty())
    {
        saveDir = "Saves/" + (mGame.empty() ? std::string("ps1") : mGame);
    }

    char name[96];
    static LONG sCounter = 0;
    snprintf(name, sizeof(name), "Local\\Ps1RecompPort_%lu_%ld", GetCurrentProcessId(), InterlockedIncrement(&sCounter));

    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)sizeof(PortShm), name);
    if (mapping == nullptr)
    {
        LogError("Ps1Player: CreateFileMapping failed (%lu)", GetLastError());
        return false;
    }
    PortShm* shm = (PortShm*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PortShm));
    if (shm == nullptr)
    {
        CloseHandle(mapping);
        LogError("Ps1Player: MapViewOfFile failed (%lu)", GetLastError());
        return false;
    }
    memset((void*)shm, 0, offsetof(PortShm, frames));

    const std::string exe = ResolvePath(exePath);
    const std::string saves = ResolvePath(saveDir);
    mLogPath = saves + "\\game.log";
    // Create every missing level of the save folder.
    for (size_t i = 3; i <= saves.size(); ++i)
    {
        if (i == saves.size() || saves[i] == '\\')
        {
            CreateDirectoryA(saves.substr(0, i).c_str(), nullptr);
        }
    }

    // Without --disc the game falls back to the disc path baked in at build time, then to a
    // disc image of the same name next to its executable.
    std::string cmd = "\"" + exe + "\" --shm " + name;
    if (!discPath.empty() && GetFileAttributesA(ResolvePath(discPath).c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        cmd += " --disc \"" + ResolvePath(discPath) + "\"";
    }
    cmd += " --saves \"" + saves + "\" --log \"" + mLogPath + "\"";
    cmd += OptionsToArgs(mGameOptions);
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    std::string workDir = exe.substr(0, exe.find_last_of('\\'));

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, workDir.c_str(), &si, &pi))
    {
        LogError("Ps1Player: could not start '%s' (%lu). Build it with Packages/%s/Native/build.ps1.", exe.c_str(),
                 GetLastError(), mGame.c_str());
        UnmapViewOfFile(shm);
        CloseHandle(mapping);
        return false;
    }

    // The game process must not outlive the editor.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    mProcess = pi.hProcess;
    mJob = job;
    mMapping = mapping;
    mShm = shm;
    mLastSerial = 0;
    mReportedExit = false;
    LogDebug("Ps1Player: started %s", cmd.c_str());
    return true;
}

void Ps1Player::StopProcess()
{
    if (mShm)
    {
        mShm->command = PORT_SHM_CMD_QUIT;
    }
    if (mProcess)
    {
        if (WaitForSingleObject((HANDLE)mProcess, 500) != WAIT_OBJECT_0)
        {
            TerminateProcess((HANDLE)mProcess, 0);
        }
        CloseHandle((HANDLE)mProcess);
        mProcess = nullptr;
    }
    if (mJob)
    {
        CloseHandle((HANDLE)mJob);
        mJob = nullptr;
    }
    if (mShm)
    {
        UnmapViewOfFile(mShm);
        mShm = nullptr;
    }
    if (mMapping)
    {
        CloseHandle((HANDLE)mMapping);
        mMapping = nullptr;
    }
}

bool Ps1Player::HasProcessExited() const
{
    return mProcess && WaitForSingleObject((HANDLE)mProcess, 0) == WAIT_OBJECT_0;
}

#else

// Other platforms only run games translated into the addon (Source/Guest, see
// Runtime/README.md); the game package's executable is a Windows program.
bool Ps1Player::StartProcess()
{
    LogError("Ps1Player: %s is not built into this addon (build its Native/ with -Guest wasm, then repackage)",
             mGame.c_str());
    return false;
}

void Ps1Player::StopProcess()
{
}

bool Ps1Player::HasProcessExited() const
{
    return false;
}

#endif

unsigned int Ps1Player::ReadPad() const
{
    unsigned int bits = 0;

    if (sAPI && sAPI->IsKeyDown)
    {
        auto down = [](int32_t key) { return sAPI->IsKeyDown(key); };

        if (down(POLYPHASE_KEY_UP))        bits |= PAD_UP;
        if (down(POLYPHASE_KEY_DOWN))      bits |= PAD_DOWN;
        if (down(POLYPHASE_KEY_LEFT))      bits |= PAD_LEFT;
        if (down(POLYPHASE_KEY_RIGHT))     bits |= PAD_RIGHT;
        if (down(POLYPHASE_KEY_Z))         bits |= PAD_CROSS;
        if (down(POLYPHASE_KEY_X))         bits |= PAD_CIRCLE;
        if (down(POLYPHASE_KEY_A))         bits |= PAD_SQUARE;
        if (down(POLYPHASE_KEY_S))         bits |= PAD_TRIANGLE;
        if (down(POLYPHASE_KEY_Q))         bits |= PAD_L1;
        if (down(POLYPHASE_KEY_W))         bits |= PAD_R1;
        if (down(POLYPHASE_KEY_1))         bits |= PAD_L2;
        if (down(POLYPHASE_KEY_2))         bits |= PAD_R2;
        if (down(POLYPHASE_KEY_ENTER))     bits |= PAD_START;
        if (down(POLYPHASE_KEY_BACKSPACE)) bits |= PAD_SELECT;
    }

    const int32_t gamepad = 0;
    if (INP_IsGamepadConnected(gamepad))
    {
        auto pdown = [gamepad](int32_t button) { return INP_IsGamepadButtonDown(button, gamepad); };
        const float lx = INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_X, gamepad);
        const float ly = INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_Y, gamepad);

        if (pdown(GAMEPAD_UP) || ly > 0.5f)    bits |= PAD_UP;
        if (pdown(GAMEPAD_DOWN) || ly < -0.5f) bits |= PAD_DOWN;
        if (pdown(GAMEPAD_LEFT) || lx < -0.5f) bits |= PAD_LEFT;
        if (pdown(GAMEPAD_RIGHT) || lx > 0.5f) bits |= PAD_RIGHT;
        if (pdown(GAMEPAD_A))      bits |= PAD_CROSS;
        if (pdown(GAMEPAD_B))      bits |= PAD_CIRCLE;
        if (pdown(GAMEPAD_X))      bits |= PAD_SQUARE;
        if (pdown(GAMEPAD_Y))      bits |= PAD_TRIANGLE;
        if (pdown(GAMEPAD_L1))     bits |= PAD_L1;
        if (pdown(GAMEPAD_R1))     bits |= PAD_R1;
        if (pdown(GAMEPAD_L2))     bits |= PAD_L2;
        if (pdown(GAMEPAD_R2))     bits |= PAD_R2;
        if (pdown(GAMEPAD_START))  bits |= PAD_START;
        if (pdown(GAMEPAD_SELECT)) bits |= PAD_SELECT;

        // Controllers without the full PS1 set. A Wiimote (upright) has A B 1 2 + -:
        // the Nunchuk's C/Z become L1/R1. A GameCube pad's L/R clicks are L1/R1 (their
        // analog travel would otherwise press L2/R2 first) and Z stands in for Select.
        // The Classic Controller's ZL is L2.
        switch (INP_GetGamepadType(gamepad))
        {
        case GamepadType::Wiimote:
            if (pdown(GAMEPAD_C)) bits |= PAD_L1;
            if (pdown(GAMEPAD_Z)) bits |= PAD_R1;
            break;
        case GamepadType::GameCube:
            if (pdown(GAMEPAD_Z)) bits |= PAD_SELECT;
            break;
        case GamepadType::WiiClassic:
            if (pdown(GAMEPAD_Z)) bits |= PAD_L2;
            break;
        default:
            if (INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTRIGGER, gamepad) > 0.3f) bits |= PAD_L2;
            if (INP_GetGamepadAxisValue(GAMEPAD_AXIS_RTRIGGER, gamepad) > 0.3f) bits |= PAD_R2;
            break;
        }
    }
    return bits;
}

void Ps1Player::EnsureDisplayQuad()
{
    if (mDisplayQuad != nullptr)
    {
        return;
    }
    mDisplayQuad = CreateChild<Quad>("PS1 Display");
    mDisplayQuad->SetAnchorMode(AnchorMode::FullStretch);
    mDisplayQuad->SetSize(1.0f, 1.0f);
    mDisplayQuad->SetObjectFit(ObjectFit::Fill);
    mBoundQuad = ResolveWeakPtr<Quad>(mDisplayQuad);
}

void Ps1Player::UpdateDisplayTexture(const uint8_t* pixels, unsigned int width, unsigned int height)
{
    if (width == 0 || height == 0 || width > PORT_SHM_MAX_W || height > PORT_SHM_MAX_H)
    {
        return;
    }

    EnsureDisplayQuad();

    Texture* texture = mFrameTexture.Get<Texture>();
    if (texture == nullptr || texture->GetWidth() != width || texture->GetHeight() != height)
    {
        // Must be a transient asset: Quad drops textures the AssetManager does not know.
        texture = NewTransientAsset<Texture>();
        texture->SetName("T_Ps1Frame");
        texture->SetMipmapped(false);
        texture->SetFilterType(Recomp_DisplayFilterLinear() ? FilterType::Linear : FilterType::Nearest);
        texture->SetWrapMode(WrapMode::Clamp);
        texture->Init(width, height, (uint8_t*)pixels);
        texture->Create();
        mFrameTexture = texture;
    }

    if (Quad* quad = mBoundQuad.Get())
    {
        quad->SetTexture(texture);
        // the resolution scaler (mod settings "Screen" / "Filter") places our own display;
        // a Quad the user bound keeps the layout they gave it
        if (quad == mDisplayQuad && Recomp_DisplayApply(quad, texture, (int)width, (int)height, 4.0f / 3.0f))
        {
            mFrameTexture = nullptr; // filter changed: a new texture next frame
        }
    }
    texture->UpdatePixels(pixels, size_t(width) * size_t(height) * 4);
    Ps1Provider::Get().SetFrame((int)width, (int)height);
    Recomp_DisplayApplyWindow((int)width, (int)height);
}

// ---- HOME menu ------------------------------------------------------------------------------
enum : uint32_t
{
    MENU_OPEN = 1u << 0,
    MENU_UP = 1u << 1,
    MENU_DOWN = 1u << 2,
    MENU_ACCEPT = 1u << 3,
    MENU_BACK = 1u << 4,
};

static const char* HomeLabel(int action)
{
    switch (action)
    {
    case 0: return "Resume";
    case 1: return "Reset Game";
#if PLATFORM_WII
    case 2: return "Exit to Homebrew Channel";
    case 3: return "Wii Menu";
#elif PLATFORM_DOLPHIN
    case 2: return "Exit";
#else
    case 2: return "Quit Game";
#endif
    default: return "?";
    }
}

uint32_t Ps1Player::ReadMenuButtons() const
{
    uint32_t bits = 0;

    for (int32_t pad = 0; pad < INPUT_MAX_GAMEPADS; ++pad)
    {
        // pad 0 also answers for the keyboard mappings of the engine's InputMap
        if (pad > 0 && !INP_IsGamepadConnected(pad))
        {
            continue;
        }
        auto down = [pad](int32_t button) { return INP_IsGamepadButtonDown(button, pad); };
        const float ly = INP_GetGamepadAxisValue(GAMEPAD_AXIS_LTHUMB_Y, pad);

        if (down(GAMEPAD_HOME)) bits |= MENU_OPEN;
        // no HOME on a GameCube pad
        if (INP_GetGamepadType(pad) == GamepadType::GameCube && down(GAMEPAD_Z) && down(GAMEPAD_START))
            bits |= MENU_OPEN;
        if (down(GAMEPAD_UP) || ly > 0.5f) bits |= MENU_UP;
        if (down(GAMEPAD_DOWN) || ly < -0.5f) bits |= MENU_DOWN;
        if (down(GAMEPAD_A)) bits |= MENU_ACCEPT;
        if (down(GAMEPAD_B)) bits |= MENU_BACK;
    }

    if (sAPI && sAPI->IsKeyDown)
    {
        if (sAPI->IsKeyDown(POLYPHASE_KEY_HOME)) bits |= MENU_OPEN;
        if (sAPI->IsKeyDown(POLYPHASE_KEY_UP)) bits |= MENU_UP;
        if (sAPI->IsKeyDown(POLYPHASE_KEY_DOWN)) bits |= MENU_DOWN;
        if (sAPI->IsKeyDown(POLYPHASE_KEY_ENTER)) bits |= MENU_ACCEPT;
        if (sAPI->IsKeyDown(POLYPHASE_KEY_BACKSPACE)) bits |= MENU_BACK;
    }
    return bits;
}

// a HOME menu line, centred, `top` pixels from the middle of the screen
static WeakPtr<Text> MakeHomeText(Node* parent, const char* name, float size, glm::vec4 color)
{
    Text* text = parent->CreateChild<Text>(name);
    text->SetAnchorMode(AnchorMode::MidHorizontalStretch);
    text->SetTextSize(size);
    text->SetHorizontalJustification(Justification::Center);
    text->SetVerticalJustification(Justification::Center);
    text->SetColor(color);
    return ResolveWeakPtr<Text>(text);
}

static void PlaceHomeText(Text* text, float top, float height)
{
    text->SetOffset(0.0f, top);
    text->SetSize(1.0f, height);
}

void Ps1Player::BuildHomeMenu()
{
    // Created after the display quad, so drawn over it.
    Quad* backdrop = CreateChild<Quad>("PS1 Home Backdrop");
    backdrop->SetAnchorMode(AnchorMode::FullStretch);
    backdrop->SetSize(1.0f, 1.0f);
    backdrop->SetColor(glm::vec4(0.0f, 0.0f, 0.0f, 0.8f));
    mHomeBackdrop = ResolveWeakPtr<Quad>(backdrop);

    mHomeTitle = MakeHomeText(this, "PS1 Home Title", 34.0f, glm::vec4(1.0f));
    mHomeHint = MakeHomeText(this, "PS1 Home Hint", 18.0f, glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
#if PLATFORM_DOLPHIN
    const char* hint = "A: select    B / HOME: back";
#else
    const char* hint = "A / Enter: select    B / Home: back";
#endif
    if (Text* text = mHomeHint.Get())
    {
        text->SetText(hint);
    }
}

// The entries for this opening (UIs come and go with scenes) and their layout: a
// centred column in pixels around the middle of the screen.
void Ps1Player::LayoutHomeMenu()
{
    mHomeEntries.clear();
    mHomeEntries.push_back({HomeAction::Resume, {}});
    for (Ps1MenuController* panel : Ps1MenuController::GetAll())
    {
        if (panel->IsInHomeMenu())
        {
            mHomeEntries.push_back({HomeAction::Panel, ResolveWeakPtr<Ps1MenuController>(panel)});
        }
    }
    for (RecompMenuController* panel : RecompMenuController::GetAll())
    {
        if (panel->IsInHomeMenu())
        {
            HomeEntry entry{HomeAction::Panel, {}};
            entry.recompPanel = ResolveWeakPtr<RecompMenuController>(panel);
            mHomeEntries.push_back(entry);
        }
    }
    mHomeEntries.push_back({HomeAction::Reset, {}});
#if PLATFORM_WII
    mHomeEntries.push_back({HomeAction::Exit, {}});
    mHomeEntries.push_back({HomeAction::WiiMenu, {}});
#elif PLATFORM_DOLPHIN || !EDITOR
    mHomeEntries.push_back({HomeAction::Exit, {}});
#endif

    while (mHomeItems.size() < mHomeEntries.size())
    {
        mHomeItems.push_back(MakeHomeText(this, "PS1 Home Item", 26.0f, glm::vec4(1.0f)));
    }

    const float titleHeight = 56.0f;
    const float lineHeight = 44.0f;
    const float hintHeight = 40.0f;
    const float total = titleHeight + 16.0f + lineHeight * mHomeEntries.size() + 16.0f + hintHeight;
    float y = -total * 0.5f;

    if (Text* title = mHomeTitle.Get())
    {
        PlaceHomeText(title, y, titleHeight);
        title->SetText(mGameTitle.empty() ? std::string("HOME Menu") : mGameTitle);
    }
    y += titleHeight + 16.0f;
    for (size_t i = 0; i < mHomeEntries.size(); ++i)
    {
        if (Text* item = mHomeItems[i].Get())
        {
            PlaceHomeText(item, y, lineHeight);
        }
        y += lineHeight;
    }
    y += 16.0f;
    if (Text* hint = mHomeHint.Get())
    {
        PlaceHomeText(hint, y, hintHeight);
    }
}

void Ps1Player::RefreshHomeMenu()
{
    for (size_t i = 0; i < mHomeEntries.size() && i < mHomeItems.size(); ++i)
    {
        if (Text* text = mHomeItems[i].Get())
        {
            const HomeEntry& entry = mHomeEntries[i];
            std::string label;
            if (entry.action == HomeAction::Panel)
            {
                Ps1MenuController* panel = entry.panel.Get();
                RecompMenuController* recomp = entry.recompPanel.Get();
                label = panel    ? (panel->IsOpen() ? "Hide " : "Show ") + panel->GetTitle()
                        : recomp ? (recomp->IsOpen() ? "Hide " : "Show ") + recomp->GetTitle()
                                 : "?";
            }
            else
            {
                label = HomeLabel((int)entry.action);
            }
            const bool selected = (int)i == mHomeSelection;
            text->SetText(selected ? "> " + label + " <" : label);
            text->SetColor(selected ? glm::vec4(1.0f, 0.85f, 0.2f, 1.0f) : glm::vec4(1.0f));
        }
    }
}

void Ps1Player::ShowHomeMenu(bool show)
{
    if (show && !mHomeBackdrop.Get())
    {
        BuildHomeMenu();
    }
    mHomeOpen = show;
    if (show)
    {
        LayoutHomeMenu();
        mHomeSelection = 0;
        RefreshHomeMenu();
    }
    else
    {
        mHoldPadUntilRelease = true;
        mVblankTime = 0.0f; // no catch-up burst of vblanks after the pause
    }

    if (Quad* backdrop = mHomeBackdrop.Get()) backdrop->SetVisible(show);
    if (Text* title = mHomeTitle.Get()) title->SetVisible(show);
    if (Text* hint = mHomeHint.Get()) hint->SetVisible(show);
    for (size_t i = 0; i < mHomeItems.size(); ++i)
    {
        if (Text* text = mHomeItems[i].Get()) text->SetVisible(show && i < mHomeEntries.size());
    }
}

void Ps1Player::RunHomeAction(const HomeEntry& entry)
{
    switch (entry.action)
    {
    case HomeAction::Resume:
        break;
    case HomeAction::Panel:
        if (Ps1MenuController* panel = entry.panel.Get())
        {
            panel->Toggle(); // an interactive UI takes the gamepad until B closes it
        }
        else if (RecompMenuController* recomp = entry.recompPanel.Get())
        {
            recomp->Toggle();
        }
        break;
    case HomeAction::Reset:
        LogDebug("Ps1Player: HOME menu: reset game");
        StopGame(); // Tick starts it again
        mReportedExit = false;
        break;
    case HomeAction::Exit:
        LogDebug("Ps1Player: HOME menu: exit");
        StopGame(); // saves are written by now
#if !EDITOR
        Quit(); // the engine shuts down; on consoles main() returns to the loader
#endif          // (the editor offers no Exit item, and Quit is not exported to addons)
        break;
    case HomeAction::WiiMenu:
#if PLATFORM_WII
        LogDebug("Ps1Player: HOME menu: Wii Menu");
        StopGame();
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
#endif
        break;
    }
}

bool Ps1Player::UpdateHomeMenu()
{
    const uint32_t buttons = ReadMenuButtons();
    const uint32_t pressed = buttons & ~mMenuButtonsPrev;
    mMenuButtonsPrev = buttons;

    if (!mHomeOpen)
    {
        if (pressed & MENU_OPEN)
        {
            ShowHomeMenu(true);
        }
        return mHomeOpen;
    }

    const int count = (int)mHomeEntries.size();
    if (pressed & (MENU_OPEN | MENU_BACK))
    {
        ShowHomeMenu(false);
        return false;
    }
    if ((pressed & MENU_UP) && count > 0)
    {
        mHomeSelection = (mHomeSelection + count - 1) % count;
        RefreshHomeMenu();
    }
    if ((pressed & MENU_DOWN) && count > 0)
    {
        mHomeSelection = (mHomeSelection + 1) % count;
        RefreshHomeMenu();
    }
    if ((pressed & MENU_ACCEPT) && count > 0)
    {
        const HomeEntry entry = mHomeEntries[mHomeSelection];
        ShowHomeMenu(false);
        RunHomeAction(entry);
    }
    return mHomeOpen;
}

void Ps1Player::Tick(float deltaTime)
{
    Node3D::Tick(deltaTime);

    if (!mStartAttempted)
    {
        mStartAttempted = true;
        StartGame();
    }

    // HOME menu first: it pauses the game, and its Reset / Exit stop it.
    UpdateHomeMenu();

    // mod settings: written to the game once it runs, kept, saved
    ModSettings::Get().Tick(&Ps1Provider::Get());

    if (mInProcess)
    {
        TickGuest(deltaTime);
        return;
    }
#if PLATFORM_WINDOWS
    if (mShm == nullptr)
    {
        return;
    }

    if (mShm->status == PORT_SHM_STATUS_CRASHED || mShm->status == PORT_SHM_STATUS_EXITED || HasProcessExited())
    {
        if (!mReportedExit)
        {
            mReportedExit = true;
            LogError("Ps1Player: the game process %s (log: %s)",
                     mShm->status == PORT_SHM_STATUS_CRASHED ? "crashed" : "exited", mLogPath.c_str());
        }
        return;
    }

    mShm->pad = (mHomeOpen || Ps1Bind::IsInputBlocked()) ? 0u : ReadPad();

    const unsigned int serial = mShm->frame_serial;
    if (serial != mLastSerial)
    {
        mLastSerial = serial;
        const unsigned int slot = mShm->frame_index & 1;
        UpdateDisplayTexture(mShm->frames[slot], mShm->width[slot], mShm->height[slot]);
    }
#endif
}

// ---- in-process game (wasm2c guest, all platforms) ---------------------------------------
bool Ps1Player::StartGuest(const Ps1wModule* module)
{
    // Which build is running (a GameCube .dol has no Wii Remote support at all), and
    // when it was built, to tell an old copy on the SD card from a new one.
#if PLATFORM_WII
    const char* platform = "Wii";
#elif PLATFORM_GAMECUBE
    const char* platform = "GameCube";
#elif PLATFORM_WINDOWS
    const char* platform = "Windows";
#else
    const char* platform = "other";
#endif
#if defined(HW_RVL)
    const char* hardware = " (libogc HW_RVL: Wii)";
#elif defined(HW_DOL)
    const char* hardware = " (libogc HW_DOL: GameCube)";
#else
    const char* hardware = "";
#endif
    LogDebug("Ps1Player: %s build%s of com.recomp.ps1, built %s %s", platform, hardware, __DATE__, __TIME__);
    mGameTitle = (module->title != nullptr) ? module->title : mGame;
    Ps1Bind::SetInputBlocked(false); // a script's block from a previous play doesn't carry over
    Recomp_SetInputBlocked(false);
    Ps1Provider::Get().SetGame(module->package != nullptr ? module->package : mGame);

    std::string exePath, discPath, saveDir;
    ResolveGameDefaults(exePath, discPath, saveDir);

    // The disc: the Disc Image property (an image or an extracted folder), else the disc
    // extracted into the game package (Assets/Disc, where mods go), else game.json's
    // image (editor), else an image named as the one the game was built for in the
    // usual places of a packaged game.
    std::vector<std::string> candidates;
    if (!mDiscPath.empty())
    {
        candidates.push_back(ResolvePath(mDiscPath));
        candidates.push_back(ResolvePath(mDiscPath + "/disc.idx"));
    }
    candidates.push_back(ResolvePath("Packages/" + std::string(module->package) + "/Assets/Disc/disc.idx"));
    candidates.push_back(ResolvePath("Assets/Disc/disc.idx"));
    if (!discPath.empty() && discPath != mDiscPath)
    {
        candidates.push_back(ResolvePath(discPath));
    }
    const std::string discName = module->disc_name;
    candidates.push_back(ResolvePath(discName));
    candidates.push_back(ResolvePath("Assets/" + discName));
    candidates.push_back(ResolvePath("Packages/" + std::string(module->package) + "/Assets/" + discName));
#if PLATFORM_DOLPHIN
    candidates.push_back("sd:/ps1/" + discName);
    candidates.push_back("usb:/ps1/" + discName);
    candidates.push_back("/ps1/" + discName);
#endif
    std::string disc;
    for (const std::string& c : candidates)
    {
        if (FILE* f = fopen(c.c_str(), "rb"))
        {
            fclose(f);
            disc = c;
            break;
        }
    }
    if (disc.empty())
    {
        LogError("Ps1Player: no disc for %s (build its Native/ to extract '%s' into the package, or set Disc "
                 "Image). Tried:",
                 module->title, discName.c_str());
        for (const std::string& c : candidates)
        {
            LogError("  %s", c.c_str());
        }
        return false;
    }

    if (saveDir.empty())
    {
        saveDir = "Saves/" + std::string(module->title);
    }
    Ps1GuestHost::SetOptions(mGameOptions);
    if (!mGameOptions.empty())
    {
        LogDebug("Ps1Player: game options: %s", mGameOptions.c_str());
    }
    if (!Ps1GuestHost::Start(module, disc, ResolvePath(saveDir)))
    {
        return false;
    }
    mInProcess = true;
    mLastSerial = 0;
    mVblankTime = 0.0f;
    mReportedExit = false;
    mAudioStream = (sAPI && sAPI->Audio_OpenStream) ? sAPI->Audio_OpenStream(44100, 2, 16) : 0;
    return true;
}

void Ps1Player::TickGuest(float deltaTime)
{
    Ps1GuestHost::FlushLog();
    const Ps1GuestHost::State state = Ps1GuestHost::GetState();
    if (state != Ps1GuestHost::State::Running)
    {
        if (!mReportedExit)
        {
            mReportedExit = true;
            LogError("Ps1Player: the game %s", state == Ps1GuestHost::State::Crashed ? "crashed" : "exited");
        }
        return;
    }

    // HOME menu open: the game is paused (it waits for its next vblank) with no input.
    if (mHomeOpen)
    {
        Ps1GuestHost::SetPad(0);
        return;
    }

    unsigned int pad = ReadPad();
    if (Ps1Bind::IsInputBlocked())
    {
        // an open interactive UI (cheats menu) navigates with the gamepad: the game gets
        // nothing, and not the press that closes it either
        pad = 0;
        mHoldPadUntilRelease = true;
    }
    else if (mHoldPadUntilRelease)
    {
        // the press that closed the menu (A / B) must not reach the game
        if (pad == 0)
        {
            mHoldPadUntilRelease = false;
        }
        pad = 0;
    }
    Ps1GuestHost::SetPad(pad);
    if (kLogInput)
    {
        LogInputState(pad, deltaTime);
    }

    // PS1 vblanks at 60 Hz of elapsed time (a few at most per tick after a stall)
    mVblankTime += deltaTime;
    uint32_t vblanks = 0;
    while (mVblankTime >= 1.0f / 60.0f && vblanks < 4)
    {
        mVblankTime -= 1.0f / 60.0f;
        vblanks++;
    }
    if (mVblankTime > 1.0f / 60.0f)
    {
        mVblankTime = 0.0f;
    }
    Ps1GuestHost::AddVblanks(vblanks);

    const uint8_t* pixels = nullptr;
    int width = 0, height = 0;
    if (Ps1GuestHost::GetFrame(mLastSerial, pixels, width, height))
    {
        UpdateDisplayTexture(pixels, (unsigned int)width, (unsigned int)height);
    }

    // audio: hand everything queued to the engine stream, as far as it takes it
    for (int i = 0; i < 8; ++i)
    {
        const int16_t* frames = nullptr;
        const uint32_t count = Ps1GuestHost::PeekAudio(frames, 2048);
        if (count == 0)
        {
            break;
        }
        if (mAudioStream && sAPI->Audio_SubmitStreamBuffer)
        {
            if (sAPI->Audio_SubmitStreamBuffer(mAudioStream, (const uint8_t*)frames, count * 4) <= 0)
            {
                break; // stream queue full: keep the rest for the next tick
            }
        }
        Ps1GuestHost::ConsumeAudio(count);
    }
}

void Ps1Player::StopGuest()
{
    Ps1GuestHost::Stop();
    if (mAudioStream && sAPI && sAPI->Audio_CloseStream)
    {
        sAPI->Audio_CloseStream(mAudioStream);
    }
    mAudioStream = 0;
    mInProcess = false;
}
