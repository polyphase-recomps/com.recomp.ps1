/**
 * @file Ps1Dependencies.cpp
 * @brief Sets up the PS1 game packages from the editor (see Ps1Dependencies.h).
 */

#include "Ps1Dependencies.h"

#if EDITOR && PLATFORM_WINDOWS

#include "Engine.h"
#include "Log.h"
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"

#include "Wasm/ps1w_module.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
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
// The "rom" block of a game package's Assets/game.json: which Native/local.cmake
// variables hold the disc image and the decomp, and how to recognise them.
struct RomConfig
{
    std::string variable;          // e.g. DW_DISC
    std::string description;
    std::string id;                // boot executable name found on the disc, e.g. SLUS_010.32
    std::string sourceVariable;    // e.g. DW_DECOMP_DIR ("" = none)
    std::string sourceDescription;
    std::string sourceCheck;       // a file the source folder must have, relative to it
};

struct GamePackage
{
    std::string id;
    std::string title;      // game.json "title", else the id
    std::string menu;       // game.json "menu": its Tools > Recomp submenu, else the title
    std::string packageDir; // ...\Packages\<id>\ (backslashes, trailing)
    std::string nativeDir;  // ...\Packages\<id>\Native\ (backslashes, trailing)
    RomConfig rom;
    bool hasDecomp = false;  // Native/ builds the game from its decomp (Ps1Game.cmake)
    std::string recompName;  // Recomp/game.json "name" ("" = no Recomp/: Decomp mode only)
};

struct GameStatus
{
    std::string id;
    std::string title;
    bool loaded = false;     // the translated game is in the addon this editor loaded
    bool translated = false; // the translated game is in com.recomp.ps1/Source/Guest
    bool extracted = false;  // the disc is extracted into the package's Assets/Disc
    std::string mode;        // what is in the addon: "decomp", "recomp", "recomp-live" ("" = nothing)
};

std::mutex sLock;
std::vector<std::string> sPending; // background setup output, written by Tick
std::deque<std::string> sRecent;   // the last lines of output, for the modal
std::thread sThread;
std::atomic<bool> sRunning{false};
std::atomic<bool> sFinished{false}; // a background setup ended; Tick refreshes the status
std::string sRunningId;             // the game being set up in the background ("" = all)
std::chrono::steady_clock::time_point sRunStart;
std::set<std::string> sProcessed;   // set up in this session: the editor must restart
std::set<std::string> sFailed;      // the last setup of these failed
std::set<std::string> sCancelled;   // the last setup of these was cancelled
int sStatusGeneration = 0;          // bumped when a setup ends, so cached status refreshes
std::atomic<bool> sCancel{false};   // Cancel(): stop the background run
HANDLE sJob = nullptr;              // the processes of the background run (under sLock)

std::vector<GameStatus> sStatus; // for the Target Options panel, refreshed on demand
bool sStatusValid = false;

EditorUIHooks* sHooks = nullptr;
uint64_t sHookId = 0;

// ---- files ---------------------------------------------------------------------------
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

bool ReadText(const std::string& path, std::string& text)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream buffer;

    if (!file)
    {
        return false;
    }
    buffer << file.rdbuf();
    text = buffer.str();
    return true;
}

bool FileContains(const std::string& path, const char* text)
{
    std::string content;
    return ReadText(path, content) && content.find(text) != std::string::npos;
}

bool Exists(const std::string& path)
{
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::string& path)
{
    const DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ForwardSlashes(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

// An absolute path without "..", forward slashes ("" stays "").
std::string Normalize(const std::string& path)
{
    char full[MAX_PATH];
    if (path.empty() || GetFullPathNameA(path.c_str(), sizeof(full), full, nullptr) == 0)
    {
        return path;
    }
    return ForwardSlashes(full);
}

std::string Trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    const size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// ---- game.json (flat string values, and the one nested "rom" object) ------------------
bool JsonString(const std::string& text, const char* key, std::string& value)
{
    const std::string quoted = std::string("\"") + key + "\"";
    size_t p = text.find(quoted);
    while (p != std::string::npos)
    {
        size_t q = text.find_first_not_of(" \t\r\n", p + quoted.size());
        if (q != std::string::npos && text[q] == ':')
        {
            q = text.find_first_not_of(" \t\r\n", q + 1);
            if (q != std::string::npos && text[q] == '"')
            {
                value.clear();
                for (size_t i = q + 1; i < text.size() && text[i] != '"'; ++i)
                {
                    if (text[i] == '\\' && i + 1 < text.size())
                    {
                        ++i;
                        value += text[i] == 'n' ? '\n' : text[i];
                    }
                    else
                    {
                        value += text[i];
                    }
                }
                return true;
            }
        }
        p = text.find(quoted, p + 1);
    }
    return false;
}

std::string JsonObject(const std::string& text, const char* key)
{
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t p = text.find(quoted);
    if (p == std::string::npos)
    {
        return std::string();
    }
    const size_t open = text.find('{', p);
    int depth = 0;
    bool inString = false;
    for (size_t i = open; open != std::string::npos && i < text.size(); ++i)
    {
        const char c = text[i];
        if (inString)
        {
            if (c == '\\') ++i;
            else if (c == '"') inString = false;
        }
        else if (c == '"') inString = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return text.substr(open, i - open + 1);
    }
    return std::string();
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
        const std::string recomp = packages + fd.cFileName + "\\Recomp\\";
        GamePackage game;
        game.hasDecomp = FileContains(native + "CMakeLists.txt", "Ps1Game.cmake") && Exists(native + "build.ps1");
        std::string json;
        if (FileContains(recomp + "CMakeLists.txt", "Ps1Recomp.cmake") && ReadText(recomp + "game.json", json))
        {
            JsonString(json, "name", game.recompName);
        }
        if (!game.hasDecomp && game.recompName.empty())
        {
            continue;
        }
        game.id = fd.cFileName;
        game.packageDir = packages + fd.cFileName + "\\";
        game.nativeDir = native;
        game.title = game.id;
        if (ReadText(packages + fd.cFileName + "\\Assets\\game.json", json))
        {
            JsonString(json, "title", game.title);
            JsonString(json, "menu", game.menu);
            const std::string rom = JsonObject(json, "rom");
            JsonString(rom, "variable", game.rom.variable);
            JsonString(rom, "description", game.rom.description);
            JsonString(rom, "id", game.rom.id);
            JsonString(rom, "sourceVariable", game.rom.sourceVariable);
            JsonString(rom, "sourceDescription", game.rom.sourceDescription);
            JsonString(rom, "sourceCheck", game.rom.sourceCheck);
        }
        if (game.menu.empty())
        {
            game.menu = game.title;
        }
        games.push_back(game);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return games;
}

bool FindGame(const std::string& id, GamePackage& out)
{
    for (const GamePackage& game : FindGamePackages())
    {
        if (game.id == id)
        {
            out = game;
            return true;
        }
    }
    return false;
}

// ---- status ----------------------------------------------------------------------------
// The game is in the addon's source when a Source/Guest/<name>/<name>_guest_module.c (Decomp)
// or <name>_guest_register.cpp (Recomp: <name> = <game>_recomp) of com.recomp.ps1 names this
// package id.
bool TranslatedOnDisk(const std::string& id)
{
    const std::string guests = ProjectDir() + "Packages\\com.recomp.ps1\\Source\\Guest\\";
    const std::string quoted = "\"" + id + "\"";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((guests + "*").c_str(), &fd);
    bool found = false;

    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
        {
            const std::string base = guests + fd.cFileName + "\\" + fd.cFileName;
            found = FileContains(base + "_guest_module.c", quoted.c_str()) ||
                    FileContains(base + "_guest_register.cpp", quoted.c_str());
        }
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

// com.recomp.ps1/Source/Guest/<name>_recomp\ : the Recomp build of a game in the addon
std::string RecompGuestDir(const GamePackage& game)
{
    return ProjectDir() + "Packages\\com.recomp.ps1\\Source\\Guest\\" + game.recompName + "_recomp\\";
}

GameStatus GetGameStatus(const GamePackage& game)
{
    GameStatus status;
    status.id = game.id;
    status.title = game.title;
    status.loaded = ps1w_find_module(game.id.c_str()) != nullptr;
    status.translated = status.loaded || TranslatedOnDisk(game.id);
    status.extracted = Exists(game.packageDir + "Assets\\Disc\\disc.idx");
    std::string mode;
    if (!game.recompName.empty() && ReadText(RecompGuestDir(game) + "mode.txt", mode))
    {
        status.mode = Trim(mode);
    }
    else if (status.translated)
    {
        status.mode = "decomp";
    }
    return status;
}

std::vector<GameStatus> GetStatus()
{
    std::vector<GameStatus> status;
    for (const GamePackage& game : FindGamePackages())
    {
        status.push_back(GetGameStatus(game));
    }
    return status;
}

bool WasProcessed(const std::string& id)
{
    std::lock_guard<std::mutex> guard(sLock);
    return sProcessed.count(id) != 0;
}

bool IsReady(const GameStatus& s)
{
    return s.loaded && s.extracted && !WasProcessed(s.id);
}

// Processed, but this editor still runs the addon from before: restart and reopen.
bool NeedsRestart(const GameStatus& s)
{
    return s.translated && s.extracted && (!s.loaded || WasProcessed(s.id));
}

const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kWarn(1.0f, 0.7f, 0.3f, 1.0f);
const ImVec4 kBad(1.0f, 0.5f, 0.4f, 1.0f);
const ImVec4 kNotice(1.0f, 0.85f, 0.35f, 1.0f);

// Colored text that wraps (TextColored doesn't).
void WrappedText(const ImVec4& color, const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

const char* kRestartHint = "Restart the editor and reopen the project for it to take effect: the translated game "
                           "is compiled into com.recomp.ps1 when the editor loads its addons, and the extracted "
                           "disc files are picked up when the project opens.";

// ---- Native/local.cmake ------------------------------------------------------------------
// The value of `set(NAME value ...)` in a CMake file, "" if it isn't set there.
std::string CMakeSetValue(const std::string& text, const std::string& name)
{
    std::istringstream lines(text);
    std::string line;
    std::string value;
    while (std::getline(lines, line))
    {
        const std::string t = Trim(line);
        const std::string head = "set(" + name;
        if (t.compare(0, head.size(), head) != 0 || t.size() <= head.size() ||
            (t[head.size()] != ' ' && t[head.size()] != '\t'))
        {
            continue;
        }
        const std::string rest = Trim(t.substr(head.size()));
        if (!rest.empty() && rest[0] == '"')
        {
            const size_t end = rest.find('"', 1);
            value = rest.substr(1, end == std::string::npos ? std::string::npos : end - 1);
        }
        else
        {
            value = rest.substr(0, rest.find_first_of(" \t)"));
        }
    }
    return value;
}

// The value the build uses: Native/local.cmake, else what the last configure cached.
std::string CurrentValue(const GamePackage& game, const std::string& name)
{
    std::string text;
    if (name.empty())
    {
        return std::string();
    }
    if (ReadText(game.nativeDir + "local.cmake", text))
    {
        const std::string value = CMakeSetValue(text, name);
        if (!value.empty()) return value;
    }
    if (ReadText(game.nativeDir + "build\\wasm-RelWithDebInfo\\CMakeCache.txt", text))
    {
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line))
        {
            if (line.compare(0, name.size() + 1, name + ":") == 0)
            {
                const size_t eq = line.find('=');
                if (eq != std::string::npos) return Trim(line.substr(eq + 1));
            }
        }
    }
    return std::string();
}

// Sets `set(NAME "value")` in Native/local.cmake, replacing an existing set(NAME ...) line
// and keeping everything else.
bool WriteLocalValue(const GamePackage& game, const std::string& name, const std::string& value)
{
    const std::string path = game.nativeDir + "local.cmake";
    const std::string setLine = "set(" + name + " \"" + ForwardSlashes(value) + "\")";
    std::string text;
    std::string out;
    bool replaced = false;

    if (!ReadText(path, text))
    {
        text = "# Local paths for building this game (not in git). Written by the editor's\n"
               "# Tools > Recomp > ... > Pre Process Rom; see local.cmake.example.\n";
    }
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = Trim(line);
        const std::string head = "set(" + name;
        if (t.compare(0, head.size(), head) == 0 && t.size() > head.size() &&
            (t[head.size()] == ' ' || t[head.size()] == '\t'))
        {
            if (!replaced) out += setLine + "\n";
            replaced = true;
            continue;
        }
        out += line + "\n";
    }
    if (!replaced)
    {
        out += setLine + "\n";
    }
    std::error_code ec;
    std::filesystem::create_directories(game.nativeDir, ec); // a package with only Recomp/
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << out;
    return file.good();
}

// ---- disc image check --------------------------------------------------------------------
struct RomCheck
{
    std::string path;       // the image to build from (a .cue resolves to its .bin)
    bool exists = false;
    bool sectorsOk = false;
    bool idFound = false;
    bool wrongGame = false; // its SYSTEM.CNF boots another executable: refused
    std::string boot;       // the executable its SYSTEM.CNF boots ("" if it couldn't be read)
    std::string message;
};

bool EndsWithNoCase(const std::string& s, const char* suffix)
{
    const size_t n = strlen(suffix);
    return s.size() >= n && _stricmp(s.c_str() + s.size() - n, suffix) == 0;
}

// The image a .cue sheet names first (track 1, the data track), next to it; "" if none.
std::string BinFromCue(const std::string& cuePath)
{
    std::string cue;
    if (!ReadText(cuePath, cue))
    {
        return std::string();
    }
    std::istringstream lines(cue);
    std::string line;
    while (std::getline(lines, line))
    {
        line = Trim(line);
        if (line.size() < 5 || _strnicmp(line.c_str(), "FILE", 4) != 0)
        {
            continue;
        }
        std::string name;
        const size_t open = line.find('"');
        const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
        if (close != std::string::npos)
        {
            name = line.substr(open + 1, close - open - 1);
        }
        else
        {
            name = Trim(line.substr(4));
            name = name.substr(0, name.find_first_of(" \t"));
        }
        const size_t slash = cuePath.find_last_of("/\\");
        return (slash == std::string::npos ? std::string() : cuePath.substr(0, slash + 1)) + name;
    }
    return std::string();
}

uint32_t Le32(const unsigned char* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

// The executable a raw (2352-byte sector) PlayStation disc image boots, read like the
// console does: the ISO 9660 root directory's SYSTEM.CNF, its BOOT line. "" if there is
// no file system or no SYSTEM.CNF.
std::string BootExecutable(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    // the 2048 bytes of user data of a sector (Mode 2 Form 1 on PS1 discs: after 24 bytes)
    auto readSector = [&file](uint32_t lba, unsigned char* out) {
        unsigned char raw[2352];
        file.seekg(std::streamoff(lba) * 2352);
        if (!file.read(reinterpret_cast<char*>(raw), sizeof(raw)))
        {
            file.clear();
            return false;
        }
        memcpy(out, raw + (raw[15] == 1 ? 16 : 24), 2048);
        return true;
    };
    unsigned char sector[2048];

    if (!file || !readSector(16, sector) || sector[0] != 1 || memcmp(sector + 1, "CD001", 5) != 0)
    {
        return std::string();
    }
    const uint32_t rootLba = Le32(sector + 156 + 2);
    const uint32_t rootSize = Le32(sector + 156 + 10);
    uint32_t cnfLba = 0, cnfSize = 0;
    for (uint32_t s = 0; s < (rootSize + 2047) / 2048 && s < 64 && cnfSize == 0; ++s)
    {
        if (!readSector(rootLba + s, sector))
        {
            break;
        }
        for (uint32_t off = 0; off + 33 < 2048 && sector[off] != 0; off += sector[off])
        {
            const uint32_t nameLen = sector[off + 32];
            if (off + 33 + nameLen > 2048)
            {
                break;
            }
            if (nameLen >= 10 && _strnicmp(reinterpret_cast<const char*>(sector + off + 33), "SYSTEM.CNF", 10) == 0)
            {
                cnfLba = Le32(sector + off + 2);
                cnfSize = Le32(sector + off + 10);
                break;
            }
        }
    }
    if (cnfSize == 0 || !readSector(cnfLba, sector))
    {
        return std::string();
    }
    std::istringstream lines(std::string(reinterpret_cast<const char*>(sector), std::min<uint32_t>(cnfSize, 2048)));
    std::string line;
    while (std::getline(lines, line))
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || _stricmp(Trim(line.substr(0, eq)).c_str(), "BOOT") != 0)
        {
            continue;
        }
        // BOOT = cdrom:\SLUS_010.32;1
        std::string exe = Trim(line.substr(eq + 1));
        const size_t sep = exe.find_last_of(":\\/");
        if (sep != std::string::npos)
        {
            exe = exe.substr(sep + 1);
        }
        return Trim(exe.substr(0, exe.find(';')));
    }
    return std::string();
}

RomCheck CheckRom(const std::string& input, const std::string& id)
{
    RomCheck check;
    check.path = Trim(input);
    if (check.path.size() >= 2 && check.path.front() == '"' && check.path.back() == '"')
    {
        check.path = check.path.substr(1, check.path.size() - 2);
    }
    if (check.path.empty())
    {
        check.message = "Pick your disc image.";
        return check;
    }
    if (EndsWithNoCase(check.path, ".cue"))
    {
        const std::string bin = BinFromCue(check.path);
        if (bin.empty())
        {
            check.message = "Can't read that .cue file: pick the .bin instead.";
            return check;
        }
        check.path = bin;
    }

    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(check.path.c_str(), GetFileExInfoStandard, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        check.message = "File not found.";
        return check;
    }
    check.exists = true;
    const uint64_t size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    check.sectorsOk = size > 0 && size % 2352 == 0;
    if (!check.sectorsOk)
    {
        check.message = "Not a raw .bin with 2352-byte sectors (an .iso or a compressed image won't work).";
        return check;
    }
    char sizeText[64];
    snprintf(sizeText, sizeof(sizeText), "%.0f MB", double(size) / (1024.0 * 1024.0));
    check.boot = BootExecutable(check.path);
    if (id.empty())
    {
        check.idFound = true;
    }
    else if (!check.boot.empty())
    {
        check.idFound = _stricmp(check.boot.c_str(), id.c_str()) == 0;
        check.wrongGame = !check.idFound;
    }
    else
    {
        // no readable SYSTEM.CNF: look for the boot executable's name near the start
        std::ifstream file(check.path, std::ios::binary);
        std::vector<char> head(size_t(std::min<uint64_t>(size, 4u << 20)));
        file.read(head.data(), std::streamsize(head.size()));
        check.idFound = std::search(head.begin(), head.end(), id.begin(), id.end()) != head.end();
    }
    if (check.wrongGame)
    {
        check.message = "This disc boots " + check.boot + ": it is another game (or another version). This one "
                        "needs the disc that boots " + id + ".";
    }
    else if (check.idFound)
    {
        check.message = "Found " + (id.empty() ? std::string("a disc image") : id) + ", " + sizeText + ".";
    }
    else
    {
        check.message = "This doesn't look like the right game (no " + id + " on it). It will be tried anyway.";
    }
    return check;
}

// ---- running build.ps1 ---------------------------------------------------------------------
void Emit(const std::string& line, bool background)
{
    {
        std::lock_guard<std::mutex> guard(sLock);
        if (background)
        {
            sPending.push_back(line);
        }
        sRecent.push_back(line);
        while (sRecent.size() > 300)
        {
            sRecent.pop_front();
        }
    }
    if (!background)
    {
        LogDebug("%s", line.c_str());
    }
}

// Runs `cmd` in `dir` and emits its output line by line. Its exit code, or -1 when it
// can't be started. A background run's processes go in a job, which Cancel() ends.
int RunProcess(const std::string& cmd, const std::string& dir, bool background)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;

    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        Emit("[ps1] cannot create a pipe for the setup", background);
        return -1;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    HANDLE job = nullptr;
    if (background)
    {
        // closing the job ends everything the run started (PowerShell, CMake, Ninja, clang)
        job = CreateJobObjectA(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job != nullptr && !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        {
            CloseHandle(job);
            job = nullptr;
        }
    }

    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | (job != nullptr ? CREATE_SUSPENDED : 0), nullptr, dir.c_str(), &si, &pi))
    {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        if (job != nullptr)
        {
            CloseHandle(job);
        }
        Emit("[ps1] cannot start: " + cmd, background);
        return -1;
    }
    CloseHandle(writePipe);
    if (job != nullptr)
    {
        if (AssignProcessToJobObject(job, pi.hProcess))
        {
            std::lock_guard<std::mutex> guard(sLock);
            sJob = job;
        }
        else
        {
            CloseHandle(job);
            job = nullptr;
        }
        ResumeThread(pi.hThread);
        if (sCancel && job != nullptr)
        {
            TerminateJobObject(job, 1);
        }
    }

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
    if (job != nullptr)
    {
        {
            std::lock_guard<std::mutex> guard(sLock);
            sJob = nullptr;
        }
        CloseHandle(job);
    }
    return static_cast<int>(code);
}

// Records how a game's run ended.
void SetResult(const std::string& id, bool ok)
{
    std::lock_guard<std::mutex> guard(sLock);
    sFailed.erase(id);
    sCancelled.erase(id);
    if (sCancel)
    {
        sCancelled.insert(id);
    }
    else if (ok)
    {
        sProcessed.insert(id);
    }
    else
    {
        sFailed.insert(id);
    }
}

// ---- Build mode ---------------------------------------------------------------------------
enum class Pipeline
{
    Decomp,     // Native/build.ps1: the decomp translated by wasm2c (Source/Guest/<name>)
    Recomp,     // build_recomp.ps1: recompiled from the disc (Source/Guest/<name>_recomp)
    RecompLive  // build_recomp.ps1 -Live: LiveRecomp, recompiled when the game boots
};

std::string sMode = "auto"; // the Build mode (kModeOption), under sLock

const char* PipelineName(Pipeline p)
{
    return p == Pipeline::Recomp ? "Recomp" : p == Pipeline::RecompLive ? "Recomp (live)" : "Decomp";
}

// auto: as the game is in the addon now (Recomp / Recomp (live) once published so), else its
// decomp, else Recomp (live) - it needs no disc to build
Pipeline ChoosePipeline(const GamePackage& game, const std::string& mode)
{
    if (game.recompName.empty() || mode == "decomp")
    {
        return Pipeline::Decomp;
    }
    if (mode == "recomp")
    {
        return Pipeline::Recomp;
    }
    if (mode == "live")
    {
        return Pipeline::RecompLive;
    }
    std::string published;
    if (ReadText(RecompGuestDir(game) + "mode.txt", published))
    {
        published = Trim(published);
        if (published == "recomp") return Pipeline::Recomp;
        if (published == "recomp-live") return Pipeline::RecompLive;
    }
    return game.hasDecomp ? Pipeline::Decomp : Pipeline::RecompLive;
}

std::string CurrentMode()
{
    std::lock_guard<std::mutex> guard(sLock);
    return sMode;
}

// Runs build_recomp.ps1 for a game package's Recomp/ folder.
bool RunRecompSetup(const GamePackage& game, Pipeline pipeline, bool background)
{
    std::string args = "-Package \"" + game.packageDir.substr(0, game.packageDir.size() - 1) + "\"";
    if (pipeline == Pipeline::RecompLive)
    {
        args += " -Live";
    }
    else
    {
        // the disc image Pre Process Rom set (else build_recomp.ps1 finds one)
        const std::string disc = Normalize(CurrentValue(game, game.rom.variable));
        if (!disc.empty() && Exists(disc))
        {
            args += " -Disc \"" + disc + "\"";
        }
    }
#if defined(_DEBUG)
    args += " -DebugCrt"; // the addon is compiled with the editor's (debug) C runtime
#endif
    const std::string script =
        ProjectDir() + "Packages\\com.recomp.ps1\\Runtime\\tools\\recomp\\build_recomp.ps1";
    const std::string cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" " + args;

    Emit("[ps1] " + game.id + ": " + PipelineName(pipeline) + " (build_recomp.ps1 " + args + ")", background);
    const int code = RunProcess(cmd, game.packageDir, background);
    if (sCancel)
    {
        Emit("[ps1] " + game.id + ": cancelled", background);
    }
    else
    {
        Emit(std::string("[ps1] ") + game.id + (code == 0 ? ": built" : ": BUILD FAILED"), background);
    }
    SetResult(game.id, code == 0);
    return code == 0 && !sCancel;
}

// Runs one game package's build.ps1; its output goes to the log.
bool RunSetup(const GamePackage& game, const std::string& mode, bool background)
{
    const Pipeline pipeline = ChoosePipeline(game, mode);
    if (pipeline != Pipeline::Decomp)
    {
        return RunRecompSetup(game, pipeline, background);
    }
    if (!game.hasDecomp)
    {
        Emit("[ps1] " + game.id + " has no decomp build (Native/): use Build mode Recomp or Recomp (live)", background);
        SetResult(game.id, false);
        return false;
    }
    // only what the addon needs: the translated game and the extracted disc (target
    // ps1_addon), not the standalone test program
    const std::string args = "-Guest wasm -Target ps1_addon";
    const std::string cmd =
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + game.nativeDir + "build.ps1\" " + args;

    Emit("[ps1] pre-processing " + game.id + " (" + game.nativeDir + "build.ps1 " + args + ")", background);
    const int code = RunProcess(cmd, game.nativeDir, background);
    if (sCancel)
    {
        Emit("[ps1] " + game.id + ": cancelled", background);
    }
    else
    {
        Emit(std::string("[ps1] ") + game.id + (code == 0 ? ": pre-processed" : ": PRE-PROCESSING FAILED"), background);
    }
    // one module per game package: the game's Recomp build leaves the addon
    if (code == 0 && !sCancel && !game.recompName.empty())
    {
        std::error_code ec;
        const std::string dir = RecompGuestDir(game);
        if (std::filesystem::exists(dir, ec))
        {
            std::filesystem::remove_all(dir, ec);
            Emit("[ps1] " + game.id + ": removed its Recomp build from the addon (Source/Guest/" + game.recompName +
                     "_recomp)",
                 background);
        }
    }
    SetResult(game.id, code == 0);
    return code == 0 && !sCancel;
}

// Extracts the disc image `disc` again over the package's Assets/Disc, replacing modded
// files (the build's own extraction only adds missing ones).
bool RestoreDisc(const GamePackage& game, const std::string& disc, bool background)
{
    const std::string out = game.packageDir + "Assets\\Disc";
    const std::string script = ProjectDir() + "Packages\\com.recomp.ps1\\Runtime\\tools\\extract_disc.py";

    Emit("[ps1] restoring the original disc files in " + out, background);
    if (RunProcess("python.exe -u \"" + script + "\" --force \"" + disc + "\" \"" + out + "\"", game.packageDir,
                   background) == 0)
    {
        return true;
    }
    Emit(sCancel ? "[ps1] " + game.id + ": cancelled"
                 : "[ps1] " + game.id + ": extracting the disc FAILED (is Python installed and on PATH?)",
         background);
    SetResult(game.id, false);
    return false;
}

bool SetupGames(const std::vector<GamePackage>& games, const std::string& mode, bool background)
{
    bool ok = true;
    for (const GamePackage& game : games)
    {
        if (sCancel)
        {
            return false;
        }
        ok = RunSetup(game, mode, background) && ok;
    }
    return ok;
}

// Pre-processes `games` on a background thread. With `restoreDisc` (one game), its disc
// `disc` is extracted again first.
void StartAsync(const std::vector<GamePackage>& games, const std::string& runningId, bool restoreDisc = false,
                const std::string& disc = std::string())
{
    if (sRunning)
    {
        LogWarning("[ps1] a pre-processing run is already going");
        return;
    }
    if (sThread.joinable())
    {
        sThread.join();
    }
    {
        std::lock_guard<std::mutex> guard(sLock);
        sRecent.clear();
        sRunningId = runningId;
        for (const GamePackage& game : games)
        {
            sFailed.erase(game.id);
            sCancelled.erase(game.id);
        }
    }
    sRunStart = std::chrono::steady_clock::now();
    sCancel = false;
    sRunning = true;
    const std::string mode = CurrentMode();
    sThread = std::thread([games, restoreDisc, disc, mode]() {
        bool ok = true;
        if (restoreDisc && games.size() == 1)
        {
            ok = RestoreDisc(games[0], disc, true);
        }
        ok = ok && SetupGames(games, mode, true);
        if (sCancel)
        {
            Emit("[ps1] Pre-processing cancelled.", true);
        }
        else
        {
            Emit(ok ? std::string("[ps1] Pre-processing done. ") + kRestartHint
                    : std::string("[ps1] Pre-processing failed (see above)"),
                 true);
        }
        sRunning = false;
        sFinished = true;
    });
}

// ---- the Pre Process Rom modal ---------------------------------------------------------------
struct PreprocessModal
{
    GamePackage game;
    std::string title;      // also the ImGui popup id
    char rom[1024] = "";
    char source[1024] = "";
    std::string checkedRom; // what `check` was computed for
    RomCheck check;
    std::string checkedSource;
    bool sourceOk = false;
    int statusGeneration = -1;
    GameStatus status;
    std::string processedFrom; // the disc image the build is configured with
    bool restore = false;      // extract the disc again over Assets/Disc first
    bool windowRegistered = false;
};

std::vector<std::unique_ptr<PreprocessModal>> sModals; // one per game, kept for the menu's userData

void CopyTo(char* buf, size_t size, const std::string& value)
{
    snprintf(buf, size, "%s", value.c_str());
}

PreprocessModal* ModalFor(const std::string& id)
{
    for (auto& modal : sModals)
    {
        if (modal->game.id == id) return modal.get();
    }
    GamePackage game;
    if (!FindGame(id, game))
    {
        return nullptr;
    }
    auto modal = std::make_unique<PreprocessModal>();
    modal->game = game;
    modal->title = "Pre Process Rom: " + game.title;
    sModals.push_back(std::move(modal));
    return sModals.back().get();
}

void Refresh(PreprocessModal& m)
{
    GamePackage game;
    if (FindGame(m.game.id, game))
    {
        m.game = game; // game.json may have changed
    }
    m.status = GetGameStatus(m.game);
    m.processedFrom = Normalize(CurrentValue(m.game, m.game.rom.variable));
    m.statusGeneration = sStatusGeneration;
}

void TextStatus(bool ok, const char* text)
{
    ImGui::TextColored(ok ? kGood : kWarn, "%s %s",
                       ok ? "[x]" : "[ ]", text);
}

bool DrawPreprocessModal(void* userData)
{
    PreprocessModal& m = *static_cast<PreprocessModal*>(userData);
    const GamePackage& game = m.game;
    const bool running = sRunning;
    bool runningThis = false;
    bool failed = false;
    bool processed = false;
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> guard(sLock);
        runningThis = running && (sRunningId.empty() || sRunningId == game.id);
        failed = sFailed.count(game.id) != 0;
        processed = sProcessed.count(game.id) != 0;
        cancelled = sCancelled.count(game.id) != 0;
    }
    if (m.statusGeneration != sStatusGeneration)
    {
        Refresh(m);
    }

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 620.0f);
    ImGui::TextWrapped("%s comes as code only: no game data. Point this at your own copy of the game; "
                       "Pre Process translates it into the PS1 runtime addon and extracts its files into "
                       "Packages/%s/Assets/Disc (where texture and other file mods go).",
                       game.title.c_str(), game.id.c_str());
    ImGui::Spacing();

    if (game.rom.variable.empty())
    {
        WrappedText(kBad, "Packages/" + game.id + "/Assets/game.json has no \"rom\" block naming the "
                          "local.cmake variable of the disc image, so the paths can't be set here. Set them in "
                          "Native/local.cmake by hand.");
    }
    else
    {
        // disc image
        ImGui::TextUnformatted("Disc image");
        if (!game.rom.description.empty())
        {
            ImGui::TextDisabled("%s", game.rom.description.c_str());
        }
        ImGui::SetNextItemWidth(520.0f);
        ImGui::InputText("##rom", m.rom, sizeof(m.rom));
        ImGui::SameLine();
        if (ImGui::Button("Browse...##rom") && sHooks != nullptr && sHooks->ShowOpenFileDialog != nullptr)
        {
            char picked[1024] = "";
            if (sHooks->ShowOpenFileDialog("Your disc image (.bin / .cue)", "Disc image|*.bin;*.cue", nullptr, picked,
                                           sizeof(picked)))
            {
                CopyTo(m.rom, sizeof(m.rom), picked);
                m.checkedRom = "\x01"; // check it again, even when it's the same path
            }
        }
        if (m.checkedRom != m.rom)
        {
            m.checkedRom = m.rom;
            m.check = CheckRom(m.rom, game.rom.id);
        }
        if (m.check.exists && m.check.sectorsOk)
        {
            WrappedText(m.check.idFound ? kGood : m.check.wrongGame ? kBad : kWarn, m.check.message);
            if (ForwardSlashes(m.check.path) != ForwardSlashes(Trim(m.rom)))
            {
                ImGui::TextDisabled("Uses %s", m.check.path.c_str());
            }
        }
        else
        {
            WrappedText(kBad, m.check.message);
        }

        // decomp
        if (!game.rom.sourceVariable.empty())
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Decomp folder");
            if (!game.rom.sourceDescription.empty())
            {
                ImGui::TextDisabled("%s", game.rom.sourceDescription.c_str());
            }
            ImGui::SetNextItemWidth(520.0f);
            ImGui::InputText("##source", m.source, sizeof(m.source));
            ImGui::SameLine();
            if (ImGui::Button("Browse...##source") && sHooks != nullptr && sHooks->ShowSelectFolderDialog != nullptr)
            {
                char picked[1024] = "";
                if (sHooks->ShowSelectFolderDialog("The decomp folder", picked, sizeof(picked)))
                {
                    CopyTo(m.source, sizeof(m.source), picked);
                }
            }
            if (m.checkedSource != m.source)
            {
                m.checkedSource = m.source;
                const std::string dir = Trim(m.source);
                m.sourceOk = !dir.empty() && IsDirectory(dir) &&
                             (game.rom.sourceCheck.empty() || Exists(dir + "/" + game.rom.sourceCheck));
            }
            if (m.sourceOk)
            {
                ImGui::TextColored(kGood, "Found.");
            }
            else
            {
                ImGui::TextColored(kBad, "Not found%s%s.",
                                   game.rom.sourceCheck.empty() ? "" : " (no ",
                                   game.rom.sourceCheck.empty() ? "" : (game.rom.sourceCheck + ")").c_str());
            }
        }
        ImGui::TextDisabled("Saved to Packages/%s/Native/local.cmake (not in git) when you pre-process.",
                            game.id.c_str());
    }

    // what is already processed
    ImGui::Separator();
    ImGui::TextUnformatted("Status");
    const std::string built = m.status.mode == "recomp"        ? " (Recomp)"
                              : m.status.mode == "recomp-live" ? " (Recomp (live))"
                              : m.status.mode == "decomp"      ? " (Decomp)"
                                                               : "";
    TextStatus(m.status.translated, (m.status.loaded ? "Game built into com.recomp.ps1" + built + ", loaded in this editor"
                                                     : "Game built into com.recomp.ps1" + built).c_str());
    TextStatus(m.status.extracted, ("Disc extracted to Packages/" + game.id + "/Assets/Disc").c_str());
    if (!m.processedFrom.empty() && (m.status.translated || m.status.extracted))
    {
        ImGui::TextDisabled("Configured with %s", m.processedFrom.c_str());
    }
    if (!runningThis)
    {
        if (processed || NeedsRestart(m.status))
        {
            ImGui::TextColored(kGood, "Pre-processed.");
            WrappedText(kNotice, kRestartHint);
        }
        else if (failed)
        {
            WrappedText(kBad, "Pre-processing failed: see the output below and the log.");
        }
        else if (cancelled)
        {
            WrappedText(kWarn, "Cancelled.");
        }
        else if (m.status.loaded && m.status.extracted)
        {
            WrappedText(kGood, "Ready. Pre Process again after changing the ROM, the decomp or the patches.");
        }
    }

    // progress
    if (runningThis || failed || processed || cancelled)
    {
        std::vector<std::string> tail;
        {
            std::lock_guard<std::mutex> guard(sLock);
            const size_t n = std::min<size_t>(sRecent.size(), 14);
            tail.assign(sRecent.end() - n, sRecent.end());
        }
        if (runningThis)
        {
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                                sRunStart).count();
            ImGui::Text("Pre-processing... %d:%02d (the first run takes several minutes)", int(secs / 60),
                        int(secs % 60));
        }
        ImGui::BeginChild("##output", ImVec2(620.0f, 200.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const std::string& line : tail)
        {
            ImGui::TextUnformatted(line.c_str());
        }
        if (runningThis)
        {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }
    ImGui::PopTextWrapPos();

    // buttons
    ImGui::Separator();
    if (!game.rom.variable.empty())
    {
        ImGui::Checkbox("Restore the original disc files", &m.restore);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Extracts every file of the disc again over Packages/%s/Assets/Disc,\n"
                              "replacing modded files. Without it, files already there are kept.",
                              game.id.c_str());
        }
    }
    const Pipeline pipeline = ChoosePipeline(game, CurrentMode());
    const bool needsSource = pipeline == Pipeline::Decomp && !game.rom.sourceVariable.empty();
    if (!game.recompName.empty())
    {
        ImGui::TextDisabled("Build mode (Target Options): %s", PipelineName(pipeline));
    }
    const bool canRun = !running && !game.rom.variable.empty() && m.check.exists && m.check.sectorsOk &&
                        !m.check.wrongGame && (!needsSource || m.sourceOk);
    if (!canRun) ImGui::BeginDisabled();
    if (ImGui::Button(m.status.translated || m.status.extracted ? "Pre Process again" : "Pre Process",
                      ImVec2(160.0f, 0.0f)))
    {
        bool saved = WriteLocalValue(game, game.rom.variable, m.check.path);
        if (!game.rom.sourceVariable.empty() && m.sourceOk)
        {
            saved = WriteLocalValue(game, game.rom.sourceVariable, Trim(m.source)) && saved;
        }
        if (!saved)
        {
            LogError("[ps1] cannot write %slocal.cmake", game.nativeDir.c_str());
        }
        StartAsync({game}, game.id, m.restore, m.check.path);
    }
    if (!canRun) ImGui::EndDisabled();
    if (runningThis)
    {
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
        {
            Ps1Dependencies::Cancel();
        }
    }
    if (running && !runningThis)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("Another game is being pre-processed.");
    }
    ImGui::SameLine();
    bool keepOpen = true;
    if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
    {
        keepOpen = false; // a running pre-process carries on; its result goes to the log
    }
    return keepOpen;
}

void OpenModal(PreprocessModal& m)
{
    // fields start from what the build is set to now
    CopyTo(m.rom, sizeof(m.rom), Normalize(CurrentValue(m.game, m.game.rom.variable)));
    CopyTo(m.source, sizeof(m.source), Normalize(CurrentValue(m.game, m.game.rom.sourceVariable)));
    m.checkedRom = "\x01"; // re-check
    m.checkedSource = "\x01";
    Refresh(m);
    if (m.rom[0] == 0)
    {
        m.processedFrom.clear();
    }

    if (sHooks == nullptr)
    {
        return;
    }
    if (sHooks->OpenModal != nullptr)
    {
        sHooks->OpenModal(sHookId, m.title.c_str(), DrawPreprocessModal, &m);
        return;
    }
    // older engines: a dockable window instead
    if (!m.windowRegistered && sHooks->RegisterWindow != nullptr)
    {
        sHooks->RegisterWindow(sHookId, m.title.c_str(), m.title.c_str(),
            [](void* userData) {
                PreprocessModal* modal = static_cast<PreprocessModal*>(userData);
                if (!DrawPreprocessModal(userData) && sHooks != nullptr && sHooks->CloseWindow != nullptr)
                {
                    sHooks->CloseWindow(modal->title.c_str());
                }
            },
            &m);
        m.windowRegistered = true;
    }
    if (sHooks->OpenWindow != nullptr)
    {
        sHooks->OpenWindow(m.title.c_str());
    }
}
}

bool Ps1Dependencies::SetupAll()
{
    if (sRunning)
    {
        LogWarning("[ps1] pre-processing is still running in the background; packaging waits for it");
    }
    if (sThread.joinable())
    {
        sThread.join();
    }
    Tick();
    sCancel = false;
    const bool ok = SetupGames(FindGamePackages(), CurrentMode(), false);
    sStatusValid = false;
    ++sStatusGeneration;
    return ok;
}

void Ps1Dependencies::SetupAllAsync()
{
    StartAsync(FindGamePackages(), std::string());
}

bool Ps1Dependencies::IsRunning()
{
    return sRunning;
}

void Ps1Dependencies::SetBuildMode(const char* mode)
{
    std::lock_guard<std::mutex> guard(sLock);
    sMode = (mode != nullptr && mode[0] != 0) ? mode : "auto";
}

void Ps1Dependencies::Cancel()
{
    if (!sRunning)
    {
        return;
    }
    sCancel = true;
    std::lock_guard<std::mutex> guard(sLock);
    if (sJob != nullptr)
    {
        TerminateJobObject(sJob, 1);
    }
}

void Ps1Dependencies::Shutdown()
{
    Cancel();
    if (sThread.joinable())
    {
        sThread.join();
    }
    Tick();
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
        if (line.find("cancelled") != std::string::npos)
            LogWarning("%s", line.c_str());
        else if (line.find("FAILED") != std::string::npos || line.find("failed") != std::string::npos ||
            line.find("error:") != std::string::npos || line.find(": error") != std::string::npos)
            LogError("%s", line.c_str());
        else
            LogDebug("%s", line.c_str());
    }
    if (sFinished.exchange(false))
    {
        sStatusValid = false;
        ++sStatusGeneration;
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
            LogWarning("[ps1] %s is not pre-processed (%s%s%s): Tools > Recomp > %s > Pre Process Rom",
                       game.id.c_str(), game.translated ? "" : "game not translated into com.recomp.ps1",
                       (!game.translated && !game.extracted) ? ", " : "", game.extracted ? "" : "disc not extracted",
                       game.title.c_str());
        }
        else if (!game.loaded)
        {
            LogWarning("[ps1] %s is pre-processed but not loaded: restart the editor and reopen the project",
                       game.id.c_str());
        }
    }
}

void Ps1Dependencies::RegisterMenus(EditorUIHooks* hooks, uint64_t hookId)
{
    sHooks = hooks;
    sHookId = hookId;
    if (hooks == nullptr || hooks->AddMenuItem == nullptr)
    {
        return;
    }
    for (const GamePackage& game : FindGamePackages())
    {
        PreprocessModal* modal = ModalFor(game.id);
        if (modal == nullptr)
        {
            continue;
        }
        const std::string item = "Recomp/" + game.menu + "/Pre Process Rom";
        hooks->AddMenuItem(hookId, "Tools", item.c_str(),
                           [](void* userData) { OpenModal(*static_cast<PreprocessModal*>(userData)); }, modal,
                           nullptr);
    }
}

void Ps1Dependencies::OpenPreprocess(const char* gameId)
{
    if (PreprocessModal* modal = ModalFor(gameId != nullptr ? gameId : ""))
    {
        OpenModal(*modal);
    }
}

void Ps1Dependencies::DrawTargetOptions(const PolyphaseBuildContext* ctx)
{
    char value[8] = "";
    const bool hasValue = ctx->GetProfileSetting != nullptr &&
                          ctx->GetProfileSetting(kSetupOption, value, sizeof(value)) != 0;
    bool setup = !hasValue || value[0] != '0';

    if (ImGui::Checkbox("Pre-process before packaging", &setup) && ctx->SetProfileSetting != nullptr)
    {
        ctx->SetProfileSetting(kSetupOption, setup ? "1" : "0");
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Before packaging, brings each PS1 game package up to date with its ROM, decomp and\n"
                          "patches (only what changed is rebuilt). A failure cancels the packaging.\n"
                          "Pick the ROM with Pre Process Rom below.");
    }

    // Build mode: what each game in the addon is made from
    char modeValue[16] = "";
    if (ctx->GetProfileSetting != nullptr)
    {
        ctx->GetProfileSetting(kModeOption, modeValue, sizeof(modeValue));
    }
    const char* modeValues[] = {"auto", "decomp", "recomp", "live"};
    int mode = 0;
    for (int i = 0; i < 4; i++)
    {
        if (strcmp(modeValue, modeValues[i]) == 0) mode = i;
    }
    Ps1Dependencies::SetBuildMode(modeValues[mode]);
    const char* modes[] = {"Auto (as each game was last built)", "Decomp", "Recomp (from your disc)",
                           "Recomp (live: releases without game code)"};
    if (ImGui::Combo("Build mode", &mode, modes, 4) && ctx->SetProfileSetting != nullptr)
    {
        ctx->SetProfileSetting(kModeOption, modeValues[mode]);
        Ps1Dependencies::SetBuildMode(modeValues[mode]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Decomp: the game is compiled from its decompilation (Native/), translated by wasm2c.\n"
                          "Every target.\n"
                          "Recomp: the game is recompiled from your disc by N64Recomp (the package's Recomp/\n"
                          "config) on the runtime's PsyQ libraries. For games whose decomp is unfinished.\n"
                          "Windows x64 only; one recompiled game per project.\n"
                          "Recomp (live): no game code in the build at all. LiveRecomp recompiles the\n"
                          "player's own disc when the game boots, from the symbols shipped in the package.\n"
                          "For PC releases.\n"
                          "Auto: as the game was last built (decomp for a game never built in a Recomp mode).\n"
                          "Games without Recomp/ always build in Decomp mode.");
    }

    const bool running = sRunning;
    if (!sStatusValid && !running)
    {
        sStatus = GetStatus();
        sStatusValid = true;
    }
    for (const GameStatus& game : sStatus)
    {
        ImGui::PushID(game.id.c_str());
        if (ImGui::Button("Pre Process Rom..."))
        {
            Ps1Dependencies::OpenPreprocess(game.id.c_str());
        }
        ImGui::SameLine();
        if (IsReady(game))
        {
            ImGui::Text("%s: ready (%s)", game.title.c_str(),
                        game.mode == "recomp" ? "Recomp" : game.mode == "recomp-live" ? "Recomp (live)" : "Decomp");
        }
        else if (NeedsRestart(game))
        {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f),
                               "%s: pre-processed, restart the editor and reopen the project", game.title.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", kRestartHint);
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s: not pre-processed (%s%s%s)", game.title.c_str(),
                               game.translated ? "" : "game not translated",
                               (!game.translated && !game.extracted) ? ", " : "",
                               game.extracted ? "" : "disc not extracted");
        }
        ImGui::PopID();
    }
    if (running)
    {
        ImGui::TextDisabled("pre-processing, see the log...");
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

void Ps1Dependencies::Cancel()
{
}

void Ps1Dependencies::Shutdown()
{
}

void Ps1Dependencies::Tick()
{
}

void Ps1Dependencies::CheckReady()
{
}

void Ps1Dependencies::RegisterMenus(EditorUIHooks*, uint64_t)
{
}

void Ps1Dependencies::SetBuildMode(const char*)
{
}

void Ps1Dependencies::OpenPreprocess(const char*)
{
}

void Ps1Dependencies::DrawTargetOptions(const PolyphaseBuildContext*)
{
}

#endif
