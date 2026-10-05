/*
 * Guest backend for the game linked in as 32-bit x86 code: PS1 RAM and the
 * scratchpad are mapped at their real addresses, the game runs on a stack inside
 * the PS1 window, and a fault handler gives PS1 behaviour to what Windows faults on
 * (accesses through NULL and other low addresses, integer division by zero).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port_host.h"
#include "../host_backend.h"

void port_game_entry(void); /* guest side: boots the game */

/* ---- crash reporting ---------------------------------------------------------------- */
static void print_stack(CONTEXT *ctx)
{
    HANDLE process = GetCurrentProcess();
    STACKFRAME frame;
    char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)buffer;
    int depth;

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(process, NULL, TRUE);
    memset(&frame, 0, sizeof(frame));
    frame.AddrPC.Offset = ctx->Eip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Ebp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Esp; frame.AddrStack.Mode = AddrModeFlat;
    for (depth = 0; depth < 32; depth++)
    {
        DWORD64 displacement = 0;
        DWORD line_displacement = 0;
        IMAGEHLP_LINE64 line;

        if (!StackWalk(IMAGE_FILE_MACHINE_I386, process, GetCurrentThread(), &frame, ctx, NULL,
                       SymFunctionTableAccess, SymGetModuleBase, NULL) || frame.AddrPC.Offset == 0)
        {
            break;
        }
        memset(buffer, 0, sizeof(buffer));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        line.SizeOfStruct = sizeof(line);
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
        {
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line))
            {
                const char *file = strrchr(line.FileName, '\\');
                host_log("  #%d %s (%s:%lu)", depth, symbol->Name, file ? file + 1 : line.FileName, line.LineNumber);
            }
            else
            {
                host_log("  #%d %s+0x%llX", depth, symbol->Name, displacement);
            }
        }
        else
        {
            host_log("  #%d %08lX", depth, (unsigned long)frame.AddrPC.Offset);
        }
    }
}

/*
 * PS1 programs read through NULL and other low pointers without crashing: on the
 * console address 0 is a mirror of main RAM (KUSEG), as is 0xA0000000 (KSEG1).
 * Windows cannot map the NULL page, so a faulting access is retried with the base
 * register moved into the RAM mapping at 0x80000000, single-stepped, and the
 * register put back afterwards (unless the instruction overwrote it).
 */
static DWORD sGameThreadId;
int port_x86_emulate_access(CONTEXT *ctx, unsigned char *mirror, int *was_skipped);
int port_x86_emulate_divide(CONTEXT *ctx, int overflow);
static DWORD sFixEip;
static int sFixReg = -1;
static DWORD sFixOrig;
static unsigned sFixTried;
static unsigned sFixCount;

/*
 * --debug-watch ADDR: a hardware data breakpoint (DR0) on the 4 bytes at ADDR, set on
 * the game thread at start; every write to them is logged with the code address and
 * the new value (symbolize with llvm-symbolizer --obj=<exe>). For memory corruption.
 */
#define WATCH_ARM_CODE 0xE0440002u
static DWORD sWatchAddr;
static unsigned sWatchCount;

static DWORD *context_reg(CONTEXT *ctx, int i)
{
    switch (i)
    {
    case 0: return &ctx->Eax;
    case 1: return &ctx->Ecx;
    case 2: return &ctx->Edx;
    case 3: return &ctx->Ebx;
    case 4: return &ctx->Esi;
    case 5: return &ctx->Edi;
    default: return &ctx->Ebp;
    }
}

static DWORD mirror_delta(DWORD v)
{
    if (v < PORT_RAM_SIZE) return PORT_RAM_BASE;
    if (v >= 0xA0000000u && v < 0xA0000000u + PORT_RAM_SIZE) return (DWORD)(PORT_RAM_BASE - 0xA0000000u);
    return 0;
}

static LONG CALLBACK lowmem_handler(EXCEPTION_POINTERS *info)
{
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    CONTEXT *ctx = info->ContextRecord;
    int i;

    if (GetCurrentThreadId() != sGameThreadId)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (rec->ExceptionCode == WATCH_ARM_CODE)
    {
        ctx->Dr0 = sWatchAddr;
        ctx->Dr7 = (ctx->Dr7 & ~0xF0003u) | 1u | (1u << 16) | (3u << 18); /* L0, write, 4 bytes */
        ctx->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && (ctx->Dr6 & 1))
    {
        if (sWatchCount++ < 2000)
        {
            host_log("watch %08lX = %08lX, written at %08lX (esp %08lX)", sWatchAddr, *(DWORD *)sWatchAddr,
                     ctx->Eip, ctx->Esp);
        }
        ctx->Dr6 = 0;
        if (sFixReg < 0) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && sFixReg >= 0)
    {
        DWORD *r = context_reg(ctx, sFixReg);

        if (*r == sFixOrig + mirror_delta(sFixOrig))
        {
            *r = sFixOrig;
        }
        sFixReg = -1;
        ctx->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rec->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO || rec->ExceptionCode == EXCEPTION_INT_OVERFLOW)
    {
        DWORD eip = ctx->Eip;

        if (port_x86_emulate_divide(ctx, rec->ExceptionCode == EXCEPTION_INT_OVERFLOW))
        {
            if (sFixCount++ < 64) host_log("integer %s at %08lX given the R3000 result",
                                           rec->ExceptionCode == EXCEPTION_INT_OVERFLOW ? "overflow" : "divide by zero", eip);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    {
        DWORD addr = (DWORD)rec->ExceptionInformation[1];

        if (mirror_delta(addr) == 0)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (sFixReg >= 0)
        {
            /* the previous candidate did not help: undo it before trying another */
            *context_reg(ctx, sFixReg) = sFixOrig;
            sFixReg = -1;
            ctx->EFlags &= ~0x100u;
        }
        if (ctx->Eip != sFixEip)
        {
            sFixEip = ctx->Eip;
            sFixTried = 0;
        }
        /* Candidates: registers pointing just below the faulting address, zero first. */
        for (i = 0; i < 2 * 7; i++)
        {
            int reg = i % 7;
            DWORD v = *context_reg(ctx, reg);

            if (sFixTried & (1u << reg)) continue;
            if (i < 7 && v != 0) continue;
            if (mirror_delta(v) == 0 || addr - v >= 0x10000u) continue;
            sFixTried |= 1u << reg;
            sFixReg = reg;
            sFixOrig = v;
            *context_reg(ctx, reg) = v + mirror_delta(v);
            ctx->EFlags |= 0x100u;
            if (sFixCount++ < 64)
            {
                host_log("low-memory %s at %08lX (address %08lX), retried through RAM mirror",
                         rec->ExceptionInformation[0] ? "write" : "read", ctx->Eip, addr);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* Absolute operand (no register to move): perform the access on the mirror. */
        {
            int skipped;
            DWORD eip = ctx->Eip;

            if (port_x86_emulate_access(ctx, (unsigned char *)(addr + mirror_delta(addr)), &skipped))
            {
                sFixEip = 0;
                if (sFixCount++ < 64 || skipped)
                {
                    host_log("low-memory %s at %08lX (address %08lX), %s", rec->ExceptionInformation[0] ? "write" : "read",
                             (unsigned long)eip, addr, skipped ? "instruction skipped" : "emulated on RAM mirror");
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    CONTEXT ctx;

    if (code < 0xC0000000 && code != 0xE0440001)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    host_log("CRASH: exception 0x%08lX at %p (address %p)", code, info->ExceptionRecord->ExceptionAddress,
             (code == EXCEPTION_ACCESS_VIOLATION) ? (void *)info->ExceptionRecord->ExceptionInformation[1] : NULL);
    ctx = *info->ContextRecord;
    host_log("  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX", ctx.Eax, ctx.Ebx,
             ctx.Ecx, ctx.Edx, ctx.Esi, ctx.Edi, ctx.Ebp, ctx.Esp);
    {
        int dump[1];

        if (port_debug_values("crash-dump", dump, 1) == 1)
        {
            const unsigned char *m = (const unsigned char *)(unsigned long)dump[0];
            int row;

            for (row = 0; row < 8; row++)
            {
                host_log("  %08lX: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                         (unsigned long)(m + row * 16), m[row * 16], m[row * 16 + 1], m[row * 16 + 2], m[row * 16 + 3],
                         m[row * 16 + 4], m[row * 16 + 5], m[row * 16 + 6], m[row * 16 + 7], m[row * 16 + 8],
                         m[row * 16 + 9], m[row * 16 + 10], m[row * 16 + 11], m[row * 16 + 12], m[row * 16 + 13],
                         m[row * 16 + 14], m[row * 16 + 15]);
            }
        }
    }
    print_stack(&ctx);
    host_crashed();
    return EXCEPTION_CONTINUE_SEARCH;
}

void *port_alloc(unsigned long size)
{
    void *p = calloc(1, size ? size : 1);

    if (p == NULL)
    {
        host_log("FATAL: out of memory (%lu bytes)", size);
        ps1_backend_fatal();
    }
    return p;
}

void ps1_backend_fatal(void)
{
    /* goes through crash_filter for the stack trace */
    RaiseException(0xE0440001, EXCEPTION_NONCONTINUABLE, 0, NULL);
    ExitProcess(1);
}

/* ---- game thread ------------------------------------------------------------------------ */
static void run_on_stack(void (*fn)(void), void *top, void *bottom)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    void *old_base = tib->StackBase, *old_limit = tib->StackLimit;

    tib->StackBase = top;
    tib->StackLimit = bottom;
    __asm__ volatile(
        "movl %%esp, %%ebx\n\t"
        "movl %0, %%esp\n\t"
        "call *%1\n\t"
        "movl %%ebx, %%esp\n\t"
        :
        : "r"(top), "r"(fn)
        : "ebx", "eax", "ecx", "edx", "memory");
    tib->StackBase = old_base;
    tib->StackLimit = old_limit;
}

void ps1_backend_run(void)
{
    sGameThreadId = GetCurrentThreadId();
    {
        int watch[1];

        if (port_debug_values("watch", watch, 1) == 1)
        {
            /* the handler sets the debug registers of the thread that raised this */
            sWatchAddr = (DWORD)watch[0];
            host_log("watching writes to %08lX", sWatchAddr);
            RaiseException(WATCH_ARM_CODE, 0, 0, NULL);
        }
    }
    run_on_stack(port_game_entry, (void *)(PORT_STACK_TOP - 64), (void *)PORT_STACK_BOTTOM);
}

size_t ps1_backend_stack_size(void)
{
    /* the game itself runs on the stack inside the PS1 window */
    return 1 << 20;
}

int ps1_backend_init(void)
{
    void *ram = VirtualAlloc((void *)PORT_RAM_BASE, PORT_STACK_TOP - PORT_RAM_BASE + 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void *scratch = VirtualAlloc((void *)PORT_SCRATCH_BASE, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    if (ram != (void *)PORT_RAM_BASE || scratch != (void *)PORT_SCRATCH_BASE)
    {
        host_log("could not map PS1 memory (ram %p scratch %p)", ram, scratch);
        return 0;
    }
    AddVectoredExceptionHandler(1, crash_filter);
    AddVectoredExceptionHandler(1, lowmem_handler);
    return 1;
}
