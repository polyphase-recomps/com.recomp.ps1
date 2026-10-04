/**
 * @file Ps1Dependencies.cpp
 * @brief Sets up the PS1 game packages from the editor (see Ps1Dependencies.h).
 */

#include "Ps1Dependencies.h"

#if EDITOR && PLATFORM_WINDOWS

#include "Engine.h"
#include "Log.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"

#include "Wasm/ps1w_module.h"

#include "imgui.h"

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
struct GamePackage
{
    std::string id;
    std::string nativeDir; // ...\Packages\<id>\Native\ (backslashes, trailing)
};

struct GameStatus
{
    std::string id;
    bool translated;
    bool extracted;
};

std::mutex sLock;
std::vector<std::string> sPending; // background setup output, written by Tick
std::thread sThread;
std::atomic<bool> sRunning{false};
std::atomic<bool> sFinished{false}; // a background setup ended; Tick refreshes the status

std::vector<GameStatus> sStatus; // for the Target Options panel, refreshed on demand
bool sStatusValid = false;

std::string ProjectDir()
{
    std::string dir = GetEngineState()->mProjectDirectory;
    char full[MAX_PATH];

    if (GetFullPathNameA(dir.c_str(), sizeof(full), full, nullptr) != 0)
    {
        dir = full;
    }
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/')
    {
        dir += "\\";
    }
    return dir;
}

bool FileContains(const std::string& path, const char* text)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;

    if (!file)
    {
        return false;
    }
    buffer << file.rdbuf();
    return buffer.str().find(text) != std::string::npos;
}

std::vector<GamePackage> FindGamePackages()
{
    std::vector<GamePackage> games;
    const std::string packages = ProjectDir() + "Packages\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((packages + "*").c_str(), &fd);

    if (h == INVALID_HANDLE_VALUE)
    {
        return games;
    }
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
        {
            continue;
        }
        const std::string native = packages + fd.cFileName + "\\Native\\";
        if (FileContains(native + "CMakeLists.txt", "Ps1Game.cmake") &&
            GetFileAttributesA((native + "build.ps1").c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            games.push_back({fd.cFileName, native});
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return games;
}

std::vector<GameStatus> GetStatus()
{
    std::vector<GameStatus> status;

    for (const GamePackage& game : FindGamePackages())
    {
        const std::string idx = game.nativeDir + "..\\Assets\\Disc\\disc.idx";
        status.push_back({game.id, ps1w_find_module(game.id.c_str()) != nullptr,
                          GetFileAttributesA(idx.c_str()) != INVALID_FILE_ATTRIBUTES});
    }
    return status;
}

void Emit(const std::string& line, bool background)
{
    if (background)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sPending.push_back(line);
    }
    else
    {
        LogDebug("%s", line.c_str());
    }
}

// Runs one game package's build.ps1; its output goes to the log.
bool RunSetup(const GamePackage& game, bool background)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;

    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        Emit("[ps1] cannot create a pipe for the setup", background);
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // only what the addon needs: the translated game and the extracted disc (target
    // ps1_addon), not the standalone test program
    const std::string args = "-Guest wasm -Target ps1_addon";
    std::string cmd =
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + game.nativeDir + "build.ps1\" " + args;
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};

    Emit("[ps1] setting up " + game.id + " (" + game.nativeDir + "build.ps1 " + args + ")", background);
    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        game.nativeDir.c_str(), &si, &pi))
    {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        Emit("[ps1] cannot start PowerShell for the setup", background);
        return false;
    }
    CloseHandle(writePipe);

    std::string line;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0)
    {
        for (DWORD i = 0; i < got; ++i)
        {
            if (buf[i] == '\n')
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) Emit("[ps1] " + line, background);
                line.clear();
            }
            else
            {
                line += buf[i];
            }
        }
    }
    if (!line.empty())
    {
        Emit("[ps1] " + line, background);
    }
    CloseHandle(readPipe);

    DWORD code = 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    Emit(std::string("[ps1] ") + game.id + (code == 0 ? ": dependencies set up" : ": SETUP FAILED"), background);
    return code == 0;
}

bool SetupGames(bool background)
{
    bool ok = true;

    for (const GamePackage& game : FindGamePackages())
    {
        ok = RunSetup(game, background) && ok;
    }
    return ok;
}

const char* kDoneHint = "Reload Native Addons to load newly translated games, and reopen the project the first "
                        "time a disc was extracted so its files are packaged";
}

bool Ps1Dependencies::SetupAll()
{
    if (sRunning)
    {
        LogWarning("[ps1] Setup Dependencies is still running in the background; packaging waits for it");
    }
    if (sThread.joinable())
    {
        sThread.join();
    }
    Tick();
    const bool ok = SetupGames(false);
    sStatusValid = false;
    return ok;
}

void Ps1Dependencies::SetupAllAsync()
{
    if (sRunning)
    {
        LogWarning("[ps1] Setup Dependencies is already running");
        return;
    }
    if (sThread.joinable())
    {
        sThread.join();
    }
    sRunning = true;
    sThread = std::thread([]() {
        const bool ok = SetupGames(true);
        Emit(ok ? std::string("[ps1] Setup Dependencies done. ") + kDoneHint
                : std::string("[ps1] Setup Dependencies failed (see above)"),
             true);
        sRunning = false;
        sFinished = true;
    });
}

bool Ps1Dependencies::IsRunning()
{
    return sRunning;
}

void Ps1Dependencies::Tick()
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> guard(sLock);
        lines.swap(sPending);
    }
    for (const std::string& line : lines)
    {
        if (line.find("FAILED") != std::string::npos || line.find("failed") != std::string::npos ||
            line.find("error:") != std::string::npos || line.find(": error") != std::string::npos)
            LogError("%s", line.c_str());
        else
            LogDebug("%s", line.c_str());
    }
    if (sFinished.exchange(false))
    {
        sStatusValid = false;
        if (sThread.joinable())
        {
            sThread.join();
        }
    }
}

void Ps1Dependencies::CheckReady()
{
    for (const GameStatus& game : GetStatus())
    {
        if (!game.translated || !game.extracted)
        {
            LogWarning("[ps1] %s is not set up (%s%s%s): Packaging > Target Options > Setup Dependencies "
                       "(packaging also runs it)",
                       game.id.c_str(), game.translated ? "" : "no translated game in com.recomp.ps1",
                       (!game.translated && !game.extracted) ? ", " : "",
                       game.extracted ? "" : "no extracted disc");
        }
    }
}

void Ps1Dependencies::DrawTargetOptions(const PolyphaseBuildContext* ctx)
{
    char value[8] = "";
    const bool hasValue = ctx->GetProfileSetting != nullptr &&
                          ctx->GetProfileSetting(kSetupOption, value, sizeof(value)) != 0;
    bool setup = !hasValue || value[0] != '0';

    if (ImGui::Checkbox("Setup Dependencies before packaging", &setup) && ctx->SetProfileSetting != nullptr)
    {
        ctx->SetProfileSetting(kSetupOption, setup ? "1" : "0");
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Before packaging, translates each PS1 game package (Packages/<game>/Native)\n"
                          "into com.recomp.ps1 and extracts its disc files into the package's\n"
                          "Assets/Disc (moddable; existing files are kept). Only what changed is\n"
                          "rebuilt. A failure cancels the packaging.");
    }

    const bool running = sRunning;
    if (running)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Setup Dependencies Now"))
    {
        SetupAllAsync();
    }
    if (running)
    {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted("running, see the log...");
    }

    if (!sStatusValid && !running)
    {
        sStatus = GetStatus();
        sStatusValid = true;
    }
    for (const GameStatus& game : sStatus)
    {
        if (game.translated && game.extracted)
        {
            ImGui::Text("%s: ready", game.id.c_str());
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s: needs setup (%s%s%s)", game.id.c_str(),
                               game.translated ? "" : "game not translated or not loaded",
                               (!game.translated && !game.extracted) ? ", " : "",
                               game.extracted ? "" : "disc not extracted");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s.", kDoneHint);
            }
        }
    }
}

#else

// Setting up games needs the editor on Windows (PowerShell, Visual Studio).
bool Ps1Dependencies::SetupAll()
{
    return true;
}

void Ps1Dependencies::SetupAllAsync()
{
}

bool Ps1Dependencies::IsRunning()
{
    return false;
}

void Ps1Dependencies::Tick()
{
}

void Ps1Dependencies::CheckReady()
{
}

void Ps1Dependencies::DrawTargetOptions(const PolyphaseBuildContext*)
{
}

#endif
