/*
 * Recomp (live) mode: the game's code is recompiled from the player's disc when it boots, by
 * N64Recomp's LiveRecomp in PS1 mode (sljit: MIPS straight to x86-64 / ARM64, no C compiler),
 * instead of being compiled into the build from N64Recomp's C output. A release then ships the
 * runtime and the game's recompiler data (its syms.toml and game.json), and no game code.
 *
 * This file stands in for what the C output brings in the other mode: the section table
 * (ps1_sections.cpp) and which disc file each section is (section_files.c). It builds the
 * recompiler's input the way tools/recomp/ps1_syms.py does - the files [ps1] files names, read
 * from the disc, one after the other at 2048-byte boundaries, words big-endian - and the
 * context N64Recomp's tool builds from the symbol file:
 *   - the PsyQ functions (size 0, <name>_recomp) are the runtime's (ps1r_hle_table);
 *   - functions only calls reveal (statics) are added and recompiled until none are left.
 * The boot executable must be the one game.json names (rom.sha1): the symbols describe it.
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <setjmp.h>
#include <sstream>
#include <string>
#include <vector>

#include "recompiler/context.h"
#include "recompiler/live_recompiler.h"
#include "toml++/toml.hpp"

#include "librecomp/sections.h"
#include "recomp.h"

extern "C" {
#include "../port/include/port_host.h"
void host_log(const char *fmt, ...);

typedef struct
{
    const char *name;
    void (*func)(uint8_t *, recomp_context *);
} Ps1rHleEntry;
extern const Ps1rHleEntry ps1r_hle_table[];
extern int32_t *section_addresses;
uint32_t ps1_cop0_read(recomp_context *ctx, int reg);
void ps1_cop0_write(recomp_context *ctx, int reg, uint32_t value);
void ps1_rfe(recomp_context *ctx);
void ps1_gte_command(recomp_context *ctx, uint32_t instruction);
uint32_t ps1_gte_read_data(recomp_context *ctx, int reg);
void ps1_gte_write_data(recomp_context *ctx, int reg, uint32_t value);
uint32_t ps1_gte_read_ctrl(recomp_context *ctx, int reg);
void ps1_gte_write_ctrl(recomp_context *ctx, int reg, uint32_t value);
/* the mods' hooks (ps1_mods.py: ps1_mod_hooks.c) */
typedef struct
{
    const char *func;
    uint32_t vram; /* 0: the function's entry */
    const char *text;
    int (*fn)(uint8_t *, recomp_context *);
} Ps1rModHook;
extern const Ps1rModHook ps1r_mod_hooks[];
extern const unsigned ps1r_mod_hook_count;
}

#ifndef PS1R_DATA_DIR
#define PS1R_DATA_DIR ""
#endif

namespace
{
std::string sDataDir = PS1R_DATA_DIR;

bool fail(const std::string &message)
{
    host_log("recomp (live): %s", message.c_str());
    return false;
}

std::string read_text(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    if (in) text << in.rdbuf();
    return text.str();
}

std::string json_string(const std::string &json, const char *key)
{
    const std::string quoted = std::string("\"") + key + "\"";
    size_t at = json.find(quoted);
    if (at == std::string::npos) return "";
    at = json.find(':', at + quoted.size());
    if (at == std::string::npos) return "";
    at = json.find('"', at);
    if (at == std::string::npos) return "";
    const size_t end = json.find('"', at + 1);
    return end == std::string::npos ? "" : json.substr(at + 1, end - at - 1);
}

std::string sha1_hex(const std::vector<uint8_t> &data)
{
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    auto block = [&](const uint8_t *p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };
    const size_t full = data.size() / 64 * 64;
    for (size_t i = 0; i < full; i += 64) block(data.data() + i);
    uint8_t tail[128] = {0};
    const size_t rest = data.size() - full;
    if (rest) memcpy(tail, data.data() + full, rest);
    tail[rest] = 0x80;
    const size_t tail_size = (rest + 9 <= 64) ? 64 : 128;
    const uint64_t bits = (uint64_t)data.size() * 8;
    for (int i = 0; i < 8; i++) tail[tail_size - 1 - i] = (uint8_t)(bits >> (i * 8));
    block(tail);
    if (tail_size == 128) block(tail + 64);
    char out[41];
    for (int i = 0; i < 5; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
}

/* a file of the disc, whole */
bool read_disc_file(const std::string &name, std::vector<uint8_t> &out)
{
    unsigned lba = 0, size = 0;
    if (!port_disc_find(("/" + name + ";1").c_str(), &lba, &size)) return false;
    out.assign(((size_t)size + 2047) / 2048 * 2048, 0);
    if (!port_disc_read(lba, (size + 2047) / 2048, out.data())) return false;
    out.resize(size);
    return true;
}

struct LiveGame
{
    N64Recomp::LiveGeneratorOutput output;
    std::vector<std::string> files;
    std::vector<std::vector<FuncEntry>> funcs;
    std::vector<SectionTableEntry> sections;
};
std::unique_ptr<LiveGame> sGame;

bool compile(N64Recomp::Context &context, const N64Recomp::LiveGeneratorInputs &base, N64Recomp::LiveGeneratorOutput &output)
{
    std::unordered_map<std::string, recomp_func_t *> runtime;
    for (const Ps1rHleEntry *e = ps1r_hle_table; e->name != nullptr; e++) runtime[e->name] = e->func;

    for (int pass = 0; pass < 16; pass++)
    {
        N64Recomp::LiveGeneratorInputs inputs = base;
        for (size_t i = 0; i < context.functions.size(); i++)
        {
            const N64Recomp::Function &func = context.functions[i];
            if (!func.words.empty()) continue;
            auto it = runtime.find(func.name);
            if (it == runtime.end()) return fail("no runtime function " + func.name);
            inputs.external_functions[i] = it->second;
        }
        N64Recomp::LiveGenerator generator{context.functions.size(), inputs};
        std::vector<std::vector<uint32_t>> statics(context.sections.size());
        for (size_t i = 0; i < context.functions.size(); i++)
        {
            const N64Recomp::Function &func = context.functions[i];
            if (func.ignored || func.words.empty()) continue;
            std::ostringstream unused;
            if (!N64Recomp::recompile_function_live(generator, context, i, unused, statics, false))
            {
                char where[16];
                snprintf(where, sizeof(where), "0x%08X", func.vram);
                return fail("could not recompile " + func.name + " (" + where + ")");
            }
        }
        size_t found = 0;
        for (size_t s = 0; s < statics.size(); s++)
        {
            N64Recomp::Section &section = context.sections[s];
            std::set<uint32_t> fresh;
            for (uint32_t vram : statics[s])
            {
                if (context.find_function_by_vram_section(vram, s) == (size_t)-1) fresh.insert(vram);
            }
            for (uint32_t vram : fresh)
            {
                uint32_t end = section.ram_addr + section.size;
                for (uint32_t a : section.function_addrs) if (a > vram && a < end) end = a;
                for (uint32_t a : fresh) if (a > vram && a < end) end = a;
                const uint32_t rom_addr = vram - section.ram_addr + section.rom_addr;
                const uint32_t *words = reinterpret_cast<const uint32_t *>(context.rom.data() + rom_addr);
                char name[64];
                snprintf(name, sizeof(name), "static_%zu_%08X", s, vram);
                const size_t index = context.functions.size();
                context.functions.emplace_back(vram, rom_addr, std::vector<uint32_t>(words, words + (end - vram) / 4), name,
                                               (uint16_t)s, false);
                context.functions_by_vram[vram].push_back(index);
                context.functions_by_name[name] = index;
                context.section_functions[s].push_back(index);
                section.function_addrs.push_back(vram);
                found++;
            }
        }
        if (found == 0)
        {
            output = generator.finish();
            return output.good ? true : fail("sljit could not finish the code");
        }
    }
    return fail("statics kept appearing after 16 passes");
}
} // namespace

extern "C" void ps1r_set_data_dir(const char *dir)
{
    sDataDir = dir != nullptr ? dir : "";
}

static int live_load();

// N64Recomp prints its notes ("[Info] ...") to stdout and stderr through fmt, which throws when
// a write fails - as it does in a packaged game, a program with no console: those streams go to
// NUL there. Anything the recompiler throws ends the load (logged), not the program.
extern "C" int ps1r_live_load(void)
{
#if defined(_WIN32)
    if (_fileno(stdout) < 0) freopen("NUL", "w", stdout);
    if (_fileno(stderr) < 0) freopen("NUL", "w", stderr);
#endif
    try
    {
        return live_load();
    }
    catch (const std::exception &e)
    {
        return fail(std::string("the recompiler stopped: ") + e.what());
    }
    catch (...)
    {
        return fail("the recompiler stopped (an exception)");
    }
}

static int live_load()
{
    const auto start = std::chrono::steady_clock::now();
    if (const char *env = getenv("PS1_RECOMP_DIR")) sDataDir = env;
    if (sDataDir.empty()) return fail("no recompiler data folder (ps1r_set_data_dir, PS1_RECOMP_DIR)");
    const std::filesystem::path base = std::filesystem::path(std::u8string(sDataDir.begin(), sDataDir.end()));
    const std::filesystem::path syms_path = base / "syms.toml";

    // the files to recompile, from the symbol file's [ps1] table
    auto game = std::make_unique<LiveGame>();
    try
    {
        toml::table syms = toml::parse_file(syms_path.string());
        if (const toml::array *files = syms["ps1"]["files"].as_array())
        {
            for (const auto &f : *files)
            {
                if (auto v = f.value<std::string>()) game->files.push_back(*v);
            }
        }
    }
    catch (const toml::parse_error &e)
    {
        return fail(std::string("cannot read ") + syms_path.string() + ": " + e.what());
    }
    if (game->files.empty()) return fail(syms_path.string() + " names no files ([ps1] files)");

    // the recompiler's ROM: the files, each at a multiple of 2048, words big-endian
    std::vector<uint8_t> rom;
    for (size_t i = 0; i < game->files.size(); i++)
    {
        std::vector<uint8_t> data;
        if (!read_disc_file(game->files[i], data)) return fail(game->files[i] + " is not on this disc");
        if (i == 0)
        {
            const std::string json = read_text(base / "game.json");
            const std::string want = json_string(json, "sha1");
            const std::string sha1 = sha1_hex(data);
            if (!want.empty() && sha1 != want)
            {
                return fail(game->files[0] + " on this disc (sha1 " + sha1 + ") is not the one the game's symbols describe (" + want + ")");
            }
        }
        data.resize((data.size() + 3) / 4 * 4, 0);
        for (size_t w = 0; w < data.size(); w += 4)
        {
            rom.push_back(data[w + 3]);
            rom.push_back(data[w + 2]);
            rom.push_back(data[w + 1]);
            rom.push_back(data[w + 0]);
        }
        rom.resize((rom.size() + 2047) / 2048 * 2048, 0);
    }

    N64Recomp::Context context{};
    if (!N64Recomp::Context::from_symbol_file(syms_path, std::move(rom), context, true))
    {
        return fail("could not load " + syms_path.string());
    }
    context.ps1 = true;

    // the mods' hooks, as N64Recomp's [[patches.hook]] (the C output has them from the config)
    std::unordered_map<std::string, int (*)(uint8_t *, recomp_context *)> text_hooks;
    for (unsigned i = 0; i < ps1r_mod_hook_count; i++)
    {
        const Ps1rModHook &hook = ps1r_mod_hooks[i];
        auto found = context.functions_by_name.find(hook.func);
        if (found == context.functions_by_name.end()) return fail(std::string("a mod hooks ") + hook.func + ", which the symbols don't have");
        N64Recomp::Function &func = context.functions[found->second];
        int32_t index = -1;
        if (hook.vram != 0)
        {
            if (hook.vram < func.vram || hook.vram >= func.vram + func.words.size() * 4) return fail(std::string("a mod hook is outside ") + hook.func);
            index = (int32_t)((hook.vram - func.vram) / 4);
        }
        func.function_hooks[index] = hook.text;
        std::string name(hook.text);
        if (name.rfind("if (", 0) == 0) name = name.substr(4);
        name = name.substr(0, name.find('('));
        text_hooks[name] = hook.fn;
    }

    N64Recomp::live_recompiler_init();
    N64Recomp::LiveGeneratorInputs inputs{};
    inputs.cop0_status_write = cop0_status_write;
    inputs.cop0_status_read = cop0_status_read;
    inputs.switch_error = switch_error;
    inputs.do_break = do_break;
    inputs.get_function = get_function;
    inputs.syscall_handler = recomp_syscall_handler;
    inputs.pause_self = pause_self;
    inputs.local_section_addresses = section_addresses;
    inputs.ps1 = true;
    inputs.ps1_gte_command = ps1_gte_command;
    inputs.ps1_gte_read_data = ps1_gte_read_data;
    inputs.ps1_gte_write_data = ps1_gte_write_data;
    inputs.ps1_gte_read_ctrl = ps1_gte_read_ctrl;
    inputs.ps1_gte_write_ctrl = ps1_gte_write_ctrl;
    inputs.ps1_cop0_read = ps1_cop0_read;
    inputs.ps1_cop0_write = ps1_cop0_write;
    inputs.ps1_rfe = ps1_rfe;
    inputs.ps1_setjmp_begin = reinterpret_cast<void *(*)(uint8_t *, recomp_context *)>(ps1_setjmp_begin);
    inputs.ps1_setjmp_fn = reinterpret_cast<void *>(&_setjmp);
    inputs.ps1_setjmp_resume = ps1_setjmp_resume;
    inputs.text_hooks = std::move(text_hooks);
    if (!compile(context, inputs, game->output)) return 0;

    // the section table the runtime looks functions up in (as recomp_overlays.inl has it)
    game->funcs.resize(context.sections.size());
    for (size_t s = 0; s < context.sections.size(); s++)
    {
        const N64Recomp::Section &section = context.sections[s];
        std::vector<FuncEntry> &entries = game->funcs[s];
        for (size_t index : context.section_functions[s])
        {
            const N64Recomp::Function &func = context.functions[index];
            if (func.words.empty()) continue;
            entries.push_back({game->output.functions[index], func.rom - section.rom_addr, (uint32_t)(func.words.size() * 4)});
        }
        if (entries.empty()) continue;
        std::stable_sort(entries.begin(), entries.end(), [](const FuncEntry &a, const FuncEntry &b) { return a.offset < b.offset; });
        SectionTableEntry entry{};
        entry.rom_addr = section.rom_addr;
        entry.ram_addr = section.ram_addr;
        entry.size = section.size;
        entry.funcs = entries.data();
        entry.num_funcs = entries.size();
        entry.index = s;
        game->sections.push_back(entry);
    }
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    host_log("recomp (live): recompiled %zu functions from the disc (%zu KB of code) in %lld ms", context.functions.size(),
             game->output.code_size >> 10, ms);
    sGame = std::move(game);
    return 1;
}

extern "C" const SectionTableEntry *ps1r_section_table(size_t *count)
{
    *count = sGame ? sGame->sections.size() : 0;
    return sGame ? sGame->sections.data() : nullptr;
}

extern "C" const char *ps1r_section_file(unsigned index)
{
    return sGame && index < sGame->files.size() ? sGame->files[index].c_str() : "";
}

extern "C" unsigned ps1r_section_file_count(void)
{
    return sGame ? (unsigned)sGame->files.size() : 0;
}
