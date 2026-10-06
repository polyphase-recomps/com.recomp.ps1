/**
 * @file Ps1Launcher.cpp
 * @brief PS1 games as com.recomp.mod.base launchers (see Ps1Launcher.h).
 */

#include "Ps1Launcher.h"

#include "Engine.h"
#include "Log.h"

#include "Ps1Player.h"
#include "Wasm/ps1w_module.h"

#include "ModBaseLauncher.h"
#include "ModBaseUtil.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

namespace
{
std::string ProjectPath(const std::string& relative)
{
    return GetEngineState()->mProjectDirectory + relative;
}

bool IsAbsolute(const std::string& path)
{
    return path.size() > 1 && (path[1] == ':' || path[0] == '/' || path[0] == '\\');
}

bool FileExists(const std::string& path)
{
    if (FILE* f = fopen(path.c_str(), "rb"))
    {
        fclose(f);
        return true;
    }
    return false;
}

std::string DirectoryOf(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

bool EndsWithNoCase(const std::string& s, const char* suffix)
{
    const size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
    {
        char a = s[s.size() - n + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

std::string Upper(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    }
    return s;
}

// ---- reading a disc image (ISO 9660 in 2048- or 2352-byte sectors) ---------------------------
class DiscImage
{
public:
    bool Open(const std::string& path)
    {
        mFile.open(path, std::ios::binary);
        if (!mFile) return false;
        // a raw image (MODE2/2352: the data after a 24-byte header) or a plain .iso
        for (const auto& layout : {std::make_pair(2352u, 24u), std::make_pair(2048u, 0u)})
        {
            mSectorSize = layout.first;
            mDataOffset = layout.second;
            uint8_t pvd[2048];
            if (ReadSector(16, pvd) && pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0)
            {
                mRootLba = Le32(pvd + 156 + 2);
                mRootSize = Le32(pvd + 156 + 10);
                return true;
            }
        }
        return false;
    }

    // A file by its path on the disc ("SYSTEM.CNF", "DIR/FILE.BIN"; no ";1")
    bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
    {
        uint32_t lba = mRootLba, size = mRootSize;
        bool isDir = true;
        size_t pos = 0;
        while (pos <= path.size())
        {
            size_t next = path.find_first_of("/\\", pos);
            if (next == std::string::npos) next = path.size();
            const std::string name = Upper(path.substr(pos, next - pos));
            pos = next + 1;
            if (name.empty()) continue;
            if (!isDir || !Find(lba, size, name, lba, size, isDir)) return false;
        }
        if (isDir) return false;
        out.resize(size);
        std::vector<uint8_t> sector(2048);
        for (uint32_t i = 0; i * 2048u < size; i++)
        {
            if (!ReadSector(lba + i, sector.data())) return false;
            const uint32_t n = std::min<uint32_t>(2048u, size - i * 2048u);
            memcpy(out.data() + i * 2048u, sector.data(), n);
        }
        return true;
    }

private:
    static uint32_t Le32(const uint8_t* p)
    {
        return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    bool ReadSector(uint32_t lba, uint8_t* out)
    {
        mFile.clear();
        mFile.seekg(std::streamoff(lba) * mSectorSize + mDataOffset);
        mFile.read(reinterpret_cast<char*>(out), 2048);
        return mFile.gcount() == 2048;
    }

    bool Find(uint32_t dirLba, uint32_t dirSize, const std::string& name, uint32_t& lba, uint32_t& size, bool& isDir)
    {
        uint8_t sector[2048];
        for (uint32_t s = 0; s * 2048u < dirSize; s++)
        {
            if (!ReadSector(dirLba + s, sector)) return false;
            for (uint32_t off = 0; off < 2048u;)
            {
                const uint8_t len = sector[off];
                if (len == 0 || off + len > 2048u) break;
                const uint8_t nameLen = sector[off + 32];
                std::string entry(reinterpret_cast<const char*>(sector + off + 33), nameLen);
                const size_t semi = entry.find(';');
                if (semi != std::string::npos) entry.resize(semi);
                if (Upper(entry) == name)
                {
                    lba = Le32(sector + off + 2);
                    size = Le32(sector + off + 10);
                    isDir = (sector[off + 25] & 2) != 0;
                    return true;
                }
                off += len;
            }
        }
        return false;
    }

    std::ifstream mFile;
    uint32_t mSectorSize = 2352, mDataOffset = 24;
    uint32_t mRootLba = 0, mRootSize = 0;
};

// The .bin a .cue names (its first FILE line), else the path itself.
std::string ImageOf(const std::string& path)
{
    if (!EndsWithNoCase(path, ".cue")) return path;
    const std::string text = RecompUtil::ReadText(path);
    const size_t file = text.find("FILE");
    if (file == std::string::npos) return path;
    const size_t open = text.find('"', file);
    const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
    if (close == std::string::npos) return path;
    const std::string name = text.substr(open + 1, close - open - 1);
    return IsAbsolute(name) ? name : DirectoryOf(path) + name;
}

// What SYSTEM.CNF boots ("SLUS_010.32", or "DIR/NAME.EXE"), "" if unreadable.
std::string BootName(const std::vector<uint8_t>& cnf)
{
    const std::string text(cnf.begin(), cnf.end());
    size_t p = text.find("BOOT");
    if (p == std::string::npos || (p = text.find(':', p)) == std::string::npos) return std::string();
    p++;
    while (p < text.size() && (text[p] == '\\' || text[p] == '/')) p++;
    std::string name;
    while (p < text.size() && text[p] != ';' && text[p] != '\r' && text[p] != '\n' && text[p] != ' ')
    {
        name += text[p] == '\\' ? '/' : text[p];
        p++;
    }
    return name;
}

// What a game package says about its disc: the boot executable and its SHA-1 (either "")
struct DiscFacts
{
    std::string title;
    std::string boot;
    std::string sha1;
};

DiscFacts FactsFor(const std::string& package, const char* title)
{
    DiscFacts facts;
    facts.title = title != nullptr ? title : package;
    const std::string assets = ProjectPath("Packages/" + package + "/Assets/");
    facts.boot = RecompUtil::JsonString(RecompUtil::ReadText(assets + "game.json"), "id");
    // the recompiler data a Recomp (live) build ships: the executable its symbols describe
    const std::string live = RecompUtil::ReadText(assets + "Recomp/Live/game.json");
    facts.sha1 = RecompUtil::JsonString(live, "sha1");
    if (facts.boot.empty()) facts.boot = RecompUtil::JsonString(live, "file");
    return facts;
}

std::string SavePath(const std::string& package)
{
    return ProjectPath("Saves/" + package + ".disc.txt");
}

// ---- one game ------------------------------------------------------------------------------
class Ps1GameLauncher : public RecompGameLauncher
{
public:
    explicit Ps1GameLauncher(const Ps1wModule* module) : mModule(module) {}

    const char* RuntimeId() const override { return "ps1"; }
    std::string GamePackage() const override { return mModule->package; }
    std::string GameTitle() const override { return mModule->title != nullptr ? mModule->title : mModule->package; }

    bool CheckRom(const std::string& path, std::string& message) override
    {
        std::string resolved;
        return Ps1Launcher::CheckDisc(GamePackage(), path, message, resolved);
    }

    bool SetRomLocation(const std::string& path, std::string& message) override
    {
        std::string resolved;
        if (!Ps1Launcher::CheckDisc(GamePackage(), path, message, resolved)) return false;
        std::ofstream file(SavePath(GamePackage()), std::ios::binary | std::ios::trunc);
        file << resolved;
        if (!file.good())
        {
            message = "Could not remember the disc (cannot write " + SavePath(GamePackage()) + ")";
            return false;
        }
        return true;
    }

    std::string GetRomLocation() override { return Ps1Launcher::ChosenDisc(GamePackage()); }

    void ClearRomLocation() override { remove(SavePath(GamePackage()).c_str()); }

    // the disc extracted into the package (development builds, or a game that ships it)
    bool HasShippedData() override
    {
        return FileExists(ProjectPath("Packages/" + GamePackage() + "/Assets/Disc/disc.idx"));
    }

    bool StartGame(std::string& message) override
    {
        const std::string chosen = GetRomLocation();
        if (!chosen.empty())
        {
            std::string resolved;
            if (!Ps1Launcher::CheckDisc(GamePackage(), chosen, message, resolved))
            {
                mMessage = message;
                return false;
            }
        }
        else if (!HasShippedData())
        {
            message = mMessage = "Choose your " + GameTitle() + " disc image first (.bin/.cue or .iso)";
            return false;
        }
        mStarted = true;
        Ps1Player::RestartGame(GamePackage());
        message = mMessage = "Starting " + GameTitle();
        return true;
    }

    bool IsStarted() override { return mStarted; }
    std::string LastMessage() override { return mMessage; }

private:
    const Ps1wModule* mModule;
    bool mStarted = false;
    std::string mMessage;
};

std::vector<std::unique_ptr<Ps1GameLauncher>> sLaunchers;
} // namespace

void Ps1Launcher::RegisterAll()
{
    UnregisterAll();
    for (int i = 0; i < ps1w_module_count(); i++)
    {
        const Ps1wModule* module = ps1w_module_at(i);
        if (module == nullptr || module->package == nullptr) continue;
        sLaunchers.push_back(std::make_unique<Ps1GameLauncher>(module));
        Recomp_RegisterLauncher(sLaunchers.back().get());
    }
}

void Ps1Launcher::UnregisterAll()
{
    for (auto& launcher : sLaunchers)
    {
        Recomp_UnregisterLauncher(launcher.get());
    }
    sLaunchers.clear();
}

std::string Ps1Launcher::ChosenDisc(const std::string& package)
{
    std::string text = RecompUtil::ReadText(SavePath(package));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

bool Ps1Launcher::CheckDisc(const std::string& package, const std::string& path, std::string& message,
                            std::string& resolved)
{
    const Ps1wModule* module = ps1w_find_module(package.c_str());
    const DiscFacts facts = FactsFor(package, module != nullptr ? module->title : nullptr);
    std::string full = IsAbsolute(path) ? path : ProjectPath(path);
    resolved = ImageOf(full);

    std::vector<uint8_t> cnf, exe;
    std::string boot;
    std::unique_ptr<DiscImage> image;
    const bool folder = FileExists(resolved + "/SYSTEM.CNF") || FileExists(resolved + "/disc.idx");
    if (folder)
    {
        // an extracted disc (extract_disc.py)
        const std::string text = RecompUtil::ReadText(resolved + "/SYSTEM.CNF");
        cnf.assign(text.begin(), text.end());
    }
    else
    {
        if (!FileExists(resolved))
        {
            message = "File not found: " + resolved;
            return false;
        }
        image = std::make_unique<DiscImage>();
        if (!image->Open(resolved) || !image->ReadFile("SYSTEM.CNF", cnf))
        {
            message = RecompUtil::FileName(resolved) + " is not a PS1 disc image (.bin with 2352-byte sectors, or .iso)";
            return false;
        }
    }
    boot = BootName(cnf);
    if (boot.empty())
    {
        message = RecompUtil::FileName(resolved) + ": its SYSTEM.CNF names no executable";
        return false;
    }
    if (!facts.boot.empty() && Upper(RecompUtil::FileName(boot)) != Upper(facts.boot))
    {
        message = RecompUtil::FileName(resolved) + " is another game (it boots " + boot + "), not " + facts.title + " (" +
                  facts.boot + ")";
        return false;
    }
    if (!facts.sha1.empty())
    {
        bool read = false;
        if (folder)
        {
            std::ifstream file(resolved + "/" + boot, std::ios::binary);
            exe.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            read = !exe.empty();
        }
        else
        {
            read = image->ReadFile(boot, exe);
        }
        const std::string sha1 = read ? RecompUtil::Sha1Hex(exe.data(), exe.size()) : std::string();
        if (sha1 != facts.sha1)
        {
            message = RecompUtil::FileName(resolved) + " is " + facts.title + ", but another version or region (" + boot +
                      " sha1 " + (sha1.empty() ? "unreadable" : sha1) + "): this build needs " + facts.sha1;
            return false;
        }
    }
    message = facts.title + " (" + boot + ")";
    return true;
}
