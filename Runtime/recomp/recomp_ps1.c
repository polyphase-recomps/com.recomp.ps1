/*
 * com.recomp.ps1 recomp mode: a PS1 game recompiled from its disc by N64Recomp (PS1 mode),
 * run with the runtime's PsyQ libraries.
 *
 * The libraries (Runtime/port/guest) are a wasm2c module, "ps1hle" (Ps1Recomp.cmake). Its
 * linear memory is the guest memory of Source/Wasm/ps1w.h - PS1 RAM, scratchpad and the
 * libraries' own data in one 8 MB buffer, folded by PS1W_OFFSET - and the recompiled code
 * uses the same buffer (recomp.h), so a PS1 address means the same thing on both sides.
 * The game calls the libraries through generated wrappers (tools/recomp/gen_hle_wrappers.py:
 * o32 arguments -> the module's exports); a library calls a function pointer the game gave
 * it through ps1w_guest_callback (Source/Wasm/ps1w_ops.h).
 *
 * The game registers as a Ps1wModule, so every host that runs a wasm2c guest - the standalone
 * program (headless dumps included), Ps1Player in the editor - runs it unchanged.
 *
 * Code lookup: each disc file N64Recomp recompiled is one section (ps1_syms.py). A section
 * is live once the game has read that file's sectors to its load address (the disc read
 * hook); calls by address go to the live section's function there.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp.h"
#include "librecomp/sections.h"

#include "ps1w.h"
#include "ps1w_module.h"
#include "ps1hle_guest.h"
#include "../port/include/port_host.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

void host_log(const char *fmt, ...);
void host_crashed(void);
extern void (*ps1w_disc_read_hook)(uint32_t lba, uint32_t count, uint32_t dst);

/* the section table and which disc file each section is: generated (ps1_sections.cpp,
 * section_files.c) or made at boot by the live recompiler (ps1_live.cpp) */
const SectionTableEntry *ps1r_section_table(size_t *count);
const char *ps1r_section_file(unsigned index);
unsigned ps1r_section_file_count(void);
void ps1r_mods_reset(void); /* ps1_mod_hooks.c (ps1_mods.py) */
#ifdef PS1R_LIVE
int ps1r_live_load(void); /* 0 on failure (logged) */
void ps1r_set_data_dir(const char *dir);
#endif

w2c_ps1hle ps1r_hle;
uint8_t *ps1r_mem;

static void fatal(const char *fmt, ...)
{
    char text[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    host_log("recomp: %s", text);
    host_crashed();
}

/* ---- the context of the game code that called into the libraries (callbacks run on it) -- */
#define MAX_DEPTH 64
static recomp_context *sCallers[MAX_DEPTH];
static int sDepth;

void ps1r_enter(recomp_context *ctx)
{
    if (sDepth < MAX_DEPTH) sCallers[sDepth] = ctx;
    sDepth++;
}

void ps1r_leave(void)
{
    if (sDepth > 0) sDepth--;
}

/* a library function the runtime doesn't have: logged once per name, returns 0 */
void ps1r_missing(const char *name, recomp_context *ctx)
{
    static const char *sLogged[256];
    static int sLoggedCount;
    int i;

    for (i = 0; i < sLoggedCount && sLogged[i] != name; i++)
    {
    }
    if (i == sLoggedCount && sLoggedCount < 256)
    {
        sLogged[sLoggedCount++] = name;
        host_log("recomp: the game called %s (a0=%08X), which the runtime does not implement", name,
                 (unsigned)ctx->r4);
    }
    ctx->r2 = 0;
}

/* ---- sections ------------------------------------------------------------------------------ */
#define MAX_SECTIONS 64
#define LOOKUP_CACHE 4096

static const SectionTableEntry *sSections;
static size_t sSectionsNum;
static uint8_t sLoaded[MAX_SECTIONS];
static unsigned sFileLba[MAX_SECTIONS], sFileSectors[MAX_SECTIONS];
static struct
{
    uint32_t vram;
    recomp_func_t *func;
} sCache[LOOKUP_CACHE];
static int32_t sSectionAddresses[MAX_SECTIONS];
int32_t *section_addresses = sSectionAddresses;

static void sections_init(void)
{
    size_t i;

    sSections = ps1r_section_table(&sSectionsNum);
    if (sSectionsNum > MAX_SECTIONS) fatal("%u sections, more than %u", (unsigned)sSectionsNum, MAX_SECTIONS);
    memset(sLoaded, 0, sizeof(sLoaded));
    memset(sCache, 0, sizeof(sCache));
    for (i = 0; i < sSectionsNum; i++)
    {
        const size_t index = sSections[i].index;
        char path[96];
        unsigned lba = 0, size = 0;

        sSectionAddresses[index < MAX_SECTIONS ? index : 0] = (int32_t)sSections[i].ram_addr;
        sFileLba[i] = sFileSectors[i] = 0;
        if (index >= ps1r_section_file_count()) continue;
        snprintf(path, sizeof(path), "/%s;1", ps1r_section_file((unsigned)index));
        if (port_disc_find(path, &lba, &size))
        {
            sFileLba[i] = lba;
            sFileSectors[i] = (size + 2047) / 2048;
        }
        else
        {
            host_log("recomp: %s is not on this disc: its code never runs", ps1r_section_file((unsigned)index));
        }
    }
}

static void section_loaded(size_t i)
{
    const SectionTableEntry *s = &sSections[i];
    size_t j;

    if (sLoaded[i]) return;
    /* whatever was loaded over the same addresses is gone */
    for (j = 0; j < sSectionsNum; j++)
    {
        const SectionTableEntry *o = &sSections[j];
        if (j != i && sLoaded[j] && o->ram_addr < s->ram_addr + s->size && s->ram_addr < o->ram_addr + o->size)
        {
            sLoaded[j] = 0;
        }
    }
    sLoaded[i] = 1;
    memset(sCache, 0, sizeof(sCache));
    if (getenv("PS1_RECOMP_TRACE"))
    {
        host_log("recomp: %s loaded at %08X", ps1r_section_file((unsigned)s->index), (unsigned)s->ram_addr);
    }
}

/* The game read `count` sectors from `lba` to guest address `dst`: a section's file read to
 * where the section runs makes that section the live one there. */
static void on_disc_read(uint32_t lba, uint32_t count, uint32_t dst)
{
    size_t i;

    for (i = 0; i < sSectionsNum; i++)
    {
        const unsigned first = sFileLba[i], n = sFileSectors[i];
        if (n == 0 || lba + count <= first || lba >= first + n) continue;
        if ((uint32_t)(dst - (lba - first) * 2048u) == sSections[i].ram_addr) section_loaded(i);
    }
}

static recomp_func_t *find_function(uint32_t vram)
{
    size_t i;

    for (i = 0; i < sSectionsNum; i++)
    {
        const SectionTableEntry *s = &sSections[i];
        size_t lo = 0, hi;

        if (!sLoaded[i] || vram < s->ram_addr || vram >= s->ram_addr + s->size) continue;
        hi = s->num_funcs;
        while (lo < hi)
        {
            const size_t mid = (lo + hi) / 2;
            const uint32_t at = s->ram_addr + s->funcs[mid].offset;
            if (at == vram) return s->funcs[mid].func;
            if (at < vram) lo = mid + 1;
            else hi = mid;
        }
        fatal("call to %08X: inside %s but not a function start", vram, ps1r_section_file((unsigned)s->index));
    }
    return NULL;
}

recomp_func_t *get_function(int32_t vram_signed)
{
    /* KUSEG / KSEG0 / KSEG1 run the same code */
    const uint32_t vram = ((uint32_t)vram_signed & 0x1FFFFFFFu) | 0x80000000u;
    const uint32_t slot = (vram >> 2) & (LOOKUP_CACHE - 1);
    recomp_func_t *func;

    if (sCache[slot].vram == vram && sCache[slot].func != NULL) return sCache[slot].func;
    func = find_function(vram);
    if (func == NULL) fatal("call to %08X: no loaded code there", vram);
    sCache[slot].vram = vram;
    sCache[slot].func = func;
    return func;
}

/* ---- a library calling a function the game gave it ------------------------------------------ */
static uint32_t sCallbackTarget;

static uint64_t callback_trampoline(void *instance, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    const uint32_t target = sCallbackTarget;
    recomp_context ctx;

    (void)instance;
    /* on the stack of the game code that called into the library */
    if (sDepth > 0 && sDepth <= MAX_DEPTH) ctx = *sCallers[sDepth - 1];
    else fatal("callback to %08X outside a library call", target);
    ctx.r4 = (gpr)(int32_t)a0;
    ctx.r5 = (gpr)(int32_t)a1;
    ctx.r6 = (gpr)(int32_t)a2;
    ctx.r7 = (gpr)(int32_t)a3;
    ctx.r31 = 0;
    get_function((int32_t)target)(ps1r_mem, &ctx);
    return (uint32_t)ctx.r2;
}

/* Mod code (ps1_mods.py: ps1_game_calls.c) calling a game function: o32 arguments, on the
 * stack of the game code it was called from (a hook, or a library call) */
uint32_t ps1r_call_game(uint32_t vram, int nargs, const uint32_t *args)
{
    uint8_t *const rdram = ps1r_mem; /* (MEM_W) */
    recomp_context ctx;
    int i;

    if (sDepth > 0 && sDepth <= MAX_DEPTH) ctx = *sCallers[sDepth - 1];
    else fatal("mod code called %08X outside the game", vram);
    /* a frame of its own below the caller's: the arguments past a3 at sp + 16 */
    ctx.r29 = (gpr)(int32_t)(((uint32_t)ctx.r29 - 16u - 4u * (uint32_t)(nargs > 4 ? nargs : 4) - 8u) & ~7u);
    for (i = 0; i < nargs; i++)
    {
        if (i < 4) (&ctx.r4)[i] = (gpr)(int32_t)args[i];
        else MEM_W(16 + 4 * (i - 4), ctx.r29) = (int32_t)args[i];
    }
    ctx.r31 = 0;
    get_function((int32_t)vram)(ps1r_mem, &ctx);
    return (uint32_t)ctx.r2;
}

void *ps1w_guest_callback(uint32_t address)
{
    if (address < 0x80000000u && address >= 0x00200000u)
    {
        fatal("call through an empty function table slot (%u)", (unsigned)address);
    }
    sCallbackTarget = address;
    return (void *)callback_trampoline;
}

/* ---- generated-code hooks ---------------------------------------------------------------------- */
void switch_error(const char *func, uint32_t vram, uint32_t jtbl)
{
    fatal("jump table %08X in %s (%08X) went out of range", jtbl, func, vram);
}

void do_break(uint32_t vram)
{
    fatal("break instruction at %08X", vram);
}

void pause_self(uint8_t *rdram)
{
    (void)rdram;
}

/* syscall(a0): 1 EnterCriticalSection (returns 1), 2 ExitCriticalSection */
void recomp_syscall_handler(uint8_t *rdram, recomp_context *ctx, int32_t instruction_vram)
{
    (void)rdram;
    (void)instruction_vram;
    if ((uint32_t)ctx->r4 == 1) ctx->r2 = 1;
}

static uint32_t sCop0[32];

gpr cop0_status_read(recomp_context *ctx)
{
    (void)ctx;
    return (gpr)(int32_t)sCop0[12];
}

void cop0_status_write(recomp_context *ctx, gpr value)
{
    (void)ctx;
    sCop0[12] = (uint32_t)value;
}

uint32_t ps1_cop0_read(recomp_context *ctx, int reg)
{
    (void)ctx;
    return sCop0[reg & 31];
}

void ps1_cop0_write(recomp_context *ctx, int reg, uint32_t value)
{
    (void)ctx;
    sCop0[reg & 31] = value;
}

void ps1_rfe(recomp_context *ctx)
{
    (void)ctx;
    sCop0[12] = (sCop0[12] & ~0xFu) | ((sCop0[12] >> 2) & 0xFu);
}

/* The GTE is the libraries' (port/guest/gte.c): the game's own cop2 instructions and libgte
 * see one register file. */
void ps1_gte_command(recomp_context *ctx, uint32_t instruction)
{
    (void)ctx;
    w2c_ps1hle_port_gte_op(&ps1r_hle, instruction & 0x01FFFFFFu);
}

uint32_t ps1_gte_read_data(recomp_context *ctx, int reg)
{
    (void)ctx;
    return w2c_ps1hle_port_gte_read_data(&ps1r_hle, (uint32_t)reg);
}

void ps1_gte_write_data(recomp_context *ctx, int reg, uint32_t value)
{
    (void)ctx;
    w2c_ps1hle_port_gte_write_data(&ps1r_hle, (uint32_t)reg, value);
}

uint32_t ps1_gte_read_ctrl(recomp_context *ctx, int reg)
{
    (void)ctx;
    return w2c_ps1hle_port_gte_read_ctrl(&ps1r_hle, (uint32_t)reg);
}

void ps1_gte_write_ctrl(recomp_context *ctx, int reg, uint32_t value)
{
    (void)ctx;
    w2c_ps1hle_port_gte_write_ctrl(&ps1r_hle, (uint32_t)reg, value);
}

/* ---- setjmp / longjmp ------------------------------------------------------------------------
 * The game's jmp_buf (a guest address) names a slot: the host jmp_buf of the recompiled
 * function that called setjmp, and the guest registers to restore when longjmp comes back. */
#define MAX_JMP 16
static struct
{
    uint32_t guest_buf;
    jmp_buf host;
    recomp_context regs;
    int depth;
} sJmp[MAX_JMP];

static int jmp_slot(uint32_t guest_buf, int create)
{
    int i, free_slot = -1;

    for (i = 0; i < MAX_JMP; i++)
    {
        if (sJmp[i].guest_buf == guest_buf) return i;
        if (sJmp[i].guest_buf == 0 && free_slot < 0) free_slot = i;
    }
    if (!create) return -1;
    if (free_slot < 0) fatal("more than %d setjmp buffers in use", MAX_JMP);
    sJmp[free_slot].guest_buf = guest_buf;
    return free_slot;
}

jmp_buf *ps1_setjmp_begin(uint8_t *rdram, recomp_context *ctx)
{
    const int slot = jmp_slot((uint32_t)ctx->r4, 1);

    (void)rdram;
    sJmp[slot].regs = *ctx;
    sJmp[slot].depth = sDepth;
    return &sJmp[slot].host;
}

void ps1_setjmp_resume(uint8_t *rdram, recomp_context *ctx, int value)
{
    const int slot = jmp_slot((uint32_t)ctx->r4, 0);
    const recomp_context *saved;

    (void)rdram;
    if (slot < 0) fatal("longjmp to an unknown jmp_buf");
    saved = &sJmp[slot].regs;
    /* what setjmp keeps: s0-s8, sp, gp (the rest is the caller's to lose) */
    ctx->r16 = saved->r16; ctx->r17 = saved->r17; ctx->r18 = saved->r18; ctx->r19 = saved->r19;
    ctx->r20 = saved->r20; ctx->r21 = saved->r21; ctx->r22 = saved->r22; ctx->r23 = saved->r23;
    ctx->r28 = saved->r28; ctx->r29 = saved->r29; ctx->r30 = saved->r30;
    ctx->r4 = saved->r4;
    ctx->r2 = (gpr)(int32_t)value;
    sDepth = sJmp[slot].depth;
}

void longjmp_recomp(uint8_t *rdram, recomp_context *ctx)
{
    const int slot = jmp_slot((uint32_t)ctx->r4, 0);
    int value = (int)(int32_t)ctx->r5;

    (void)rdram;
    if (slot < 0) fatal("longjmp to a jmp_buf no setjmp filled (%08X)", (unsigned)ctx->r4);
    longjmp(sJmp[slot].host, value ? value : 1);
}

void setjmp_recomp(uint8_t *rdram, recomp_context *ctx)
{
    (void)rdram;
    (void)ctx;
    fatal("setjmp called by address (the recompiler inlines direct calls)");
}

/* ---- boot --------------------------------------------------------------------------------------- */
static uint32_t rd32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* SYSTEM.CNF: BOOT = cdrom:\NAME;1 and STACK = hex */
static void read_system_cnf(char *exe, size_t exe_cap, uint32_t *stack)
{
    static uint8_t sector[2048];
    unsigned lba, size;
    const char *p;

    snprintf(exe, exe_cap, "/PSX.EXE;1");
    *stack = 0x801FFFF0u;
    if (!port_disc_find("/SYSTEM.CNF;1", &lba, &size) || !port_disc_read(lba, 1, sector)) return;
    sector[sizeof(sector) - 1] = 0;
    if ((p = strstr((const char *)sector, "BOOT")) != NULL && (p = strchr(p, ':')) != NULL)
    {
        size_t n = 0;
        p++;
        while (*p == '\\' || *p == '/') p++;
        exe[n++] = '/';
        while (*p && *p != '\r' && *p != '\n' && *p != ' ' && n + 1 < exe_cap) exe[n++] = *p++;
        exe[n] = 0;
    }
    if ((p = strstr((const char *)sector, "STACK")) != NULL && (p = strchr(p, '=')) != NULL)
    {
        *stack = (uint32_t)strtoul(p + 1, NULL, 16);
    }
}

static recomp_context sCtx;

static void run(void)
{
    static uint8_t header[2048];
    char exe[64];
    uint32_t stack, pc0, gp0, t_addr, t_size, b_addr, b_size;
    unsigned lba, size;

#ifdef PS1R_LIVE
    /* Recomp (live): the game's code comes from this disc, recompiled now */
    if (!ps1r_live_load()) fatal("the game could not be recompiled from this disc (see the log)");
#endif
    sections_init();
    memset(sJmp, 0, sizeof(sJmp));
    ps1r_mods_reset();
    read_system_cnf(exe, sizeof(exe), &stack);
    if (!port_disc_find(exe, &lba, &size) || !port_disc_read(lba, 1, header)) fatal("cannot read %s", exe);
    pc0 = rd32(header + 0x10);
    gp0 = rd32(header + 0x14);
    t_addr = rd32(header + 0x18);
    t_size = rd32(header + 0x1C);
    b_addr = rd32(header + 0x28);
    b_size = rd32(header + 0x2C);
    host_log("recomp: %s: entry %08X, text %08X+%X, bss %08X+%X, stack %08X", exe, (unsigned)pc0, (unsigned)t_addr,
             (unsigned)t_size, (unsigned)b_addr, (unsigned)b_size, (unsigned)stack);
    if (!port_disc_read(lba + 1, (t_size + 2047) / 2048, ps1r_mem + PS1W_OFFSET(t_addr))) fatal("cannot read %s", exe);
    on_disc_read(lba + 1, (t_size + 2047) / 2048, t_addr);
    /* the file starts with its header, the section with the file: */
    on_disc_read(lba, 1, t_addr - 2048);
    if (b_size) memset(ps1r_mem + PS1W_OFFSET(b_addr), 0, b_size);
    w2c_ps1hle_port_recomp_set_bss_end(&ps1r_hle, b_size ? b_addr + b_size : t_addr + t_size);
    w2c_ps1hle_InitGeom(&ps1r_hle);

    memset(&sCtx, 0, sizeof(sCtx));
    sCtx.r29 = (gpr)(int32_t)stack;
    sCtx.r30 = (gpr)(int32_t)stack;
    if (gp0) sCtx.r28 = (gpr)(int32_t)gp0;
    sDepth = 0;
    get_function((int32_t)pc0)(ps1r_mem, &sCtx);
    host_log("recomp: the game's entry point returned");
}

#if defined(_WIN32)
/* A crash in recompiled code or the libraries: where (for llvm-symbolizer --obj=<exe>) and
 * which guest address was in use. */
static LONG WINAPI crash_report(EXCEPTION_POINTERS *info)
{
    const EXCEPTION_RECORD *r = info->ExceptionRecord;
    const DWORD code = r->ExceptionCode;
    HMODULE base = NULL;

    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    /* the module the fault is in (the program, or the editor addon the game is linked into) */
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)r->ExceptionAddress, &base))
    {
        base = NULL;
    }
    host_log("recomp: crash %08lX at %p (module+0x%llX), access %p, ra=%08X sp=%08X", (unsigned long)code,
             r->ExceptionAddress, (unsigned long long)((uint8_t *)r->ExceptionAddress - (uint8_t *)base),
             r->NumberParameters > 1 ? (void *)r->ExceptionInformation[1] : NULL,
             sDepth > 0 && sDepth <= MAX_DEPTH ? (unsigned)sCallers[sDepth - 1]->r31 : 0u,
             sDepth > 0 && sDepth <= MAX_DEPTH ? (unsigned)sCallers[sDepth - 1]->r29 : 0u);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void instantiate(struct w2c_env *env)
{
#if defined(_WIN32)
    static int sHandler;
    if (!sHandler)
    {
        sHandler = 1;
        AddVectoredExceptionHandler(0, crash_report);
    }
#endif
    wasm2c_ps1hle_instantiate(&ps1r_hle, env);
    ps1r_mem = ps1r_hle.w2c_memory.data;
    ps1w_disc_read_hook = on_disc_read;
}

static void release(void)
{
    ps1w_disc_read_hook = NULL;
    wasm2c_ps1hle_free(&ps1r_hle);
    ps1r_mem = NULL;
}

static wasm_rt_memory_t *memory(void)
{
    return &ps1r_hle.w2c_memory;
}

#ifndef PS1R_NAME
#error "Ps1Recomp.cmake defines PS1R_NAME, PS1R_PACKAGE, PS1R_TITLE, PS1R_DISC_NAME, PS1R_MODULE"
#endif
const Ps1wModule PS1R_MODULE = {
    PS1R_NAME, PS1R_PACKAGE, PS1R_TITLE, PS1R_DISC_NAME, instantiate, release, run, memory,
#ifdef PS1R_LIVE
    ps1r_set_data_dir,
#else
    NULL,
#endif
};
