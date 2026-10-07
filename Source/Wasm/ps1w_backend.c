/*
 * wasm2c guest backend: the registry of translated games (ps1w_module.h), the one
 * running instance, and its imports - the port_host.h functions - which turn guest
 * addresses into pointers into the guest memory buffer (ps1w.h) and call the host.
 * The host (Polyphase's Ps1Player, or a standalone platform host) provides the
 * port_host.h functions plus host_log() and host_crashed().
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ps1w.h"
#include "ps1w_module.h"
#include "ps1w_bridge.h"
#include "../../Runtime/port/include/port_host.h"

typedef uint32_t u32;

void host_log(const char *fmt, ...);
void host_crashed(void); /* does not return */
void ps1w_rt_reset(void);

/* imports come from the "env" module; nothing to keep per instance */
struct w2c_env
{
    int unused;
};

/* ---- registry ----------------------------------------------------------------------- */
#define MAX_MODULES 8
static const Ps1wModule *sModules[MAX_MODULES];
static int sModuleCount;

void ps1w_register_module(const Ps1wModule *module)
{
    if (sModuleCount < MAX_MODULES) sModules[sModuleCount++] = module;
}

int ps1w_module_count(void)
{
    return sModuleCount;
}

const Ps1wModule *ps1w_module_at(int index)
{
    return index >= 0 && index < sModuleCount ? sModules[index] : NULL;
}

const Ps1wModule *ps1w_find_module(const char *package)
{
    int i;

    for (i = 0; i < sModuleCount; i++)
    {
        if (package == NULL || package[0] == 0 || strcmp(sModules[i]->package, package) == 0) return sModules[i];
    }
    return NULL;
}

/* ---- the running instance ------------------------------------------------------------ */
static const Ps1wModule *sModule;
static struct w2c_env sEnv;
static uint8_t *sMem;

static void *guest_ptr(u32 addr, u32 len)
{
    u32 offset = PS1W_OFFSET(addr);

    if ((unsigned long long)offset + len > PS1W_MEM_BYTES)
    {
        host_log("guest buffer %08X+%X runs past guest memory", (unsigned)addr, (unsigned)len);
        host_crashed();
    }
    return sMem + offset;
}

void *ps1w_guest_ptr(unsigned addr, unsigned len)
{
    u32 offset = PS1W_OFFSET(addr);

    if (sMem == NULL || (unsigned long long)offset + len > PS1W_MEM_BYTES) return NULL;
    return sMem + offset;
}

static const char *guest_str(u32 addr)
{
    return (const char *)guest_ptr(addr, 1);
}

static void store_u32(u32 addr, u32 value)
{
    unsigned char *p = (unsigned char *)guest_ptr(addr, 4);

    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

void ps1w_report_trap(const char *what)
{
    host_log("CRASH: wasm trap: %s", what);
    host_crashed();
}

int ps1w_instantiate(const Ps1wModule *module)
{
    if (sModule) ps1w_free();
    wasm_rt_init();
    ps1w_rt_reset();
    module->instantiate(&sEnv);
    sModule = module;
    sMem = module->memory()->data;
    return sMem != NULL;
}

void ps1w_run(void)
{
    if (sModule) sModule->run();
}

void ps1w_free(void)
{
    if (sModule) sModule->free();
    sModule = NULL;
    sMem = NULL;
}

/* ---- imports: logging --------------------------------------------------------------- */
void w2c_env_port_host_log(struct w2c_env *env, u32 msg)
{
    port_host_log(guest_str(msg));
}

void w2c_env_port_host_fatal(struct w2c_env *env, u32 msg)
{
    port_host_fatal(guest_str(msg));
}

/* ---- imports: disc ------------------------------------------------------------------ */
/* Recomp mode: told about every disc read into guest memory (overlays loading). */
void (*ps1w_disc_read_hook)(u32 lba, u32 count, u32 dst);
/* a module with threads of its own (recomp mode's BIOS threads): called before the host leaves
 * the game with longjmp (stop: 2, crash: 1); it gets back to the thread whose stack that jumps to */
int (*ps1w_unwind_hook)(int code);

u32 w2c_env_port_disc_read(struct w2c_env *env, u32 lba, u32 count, u32 dst)
{
    u32 ok = (u32)port_disc_read(lba, count, guest_ptr(dst, count * 2048));

    if (ok && ps1w_disc_read_hook) ps1w_disc_read_hook(lba, count, dst);
    return ok;
}

u32 w2c_env_port_disc_read_raw(struct w2c_env *env, u32 lba, u32 dst)
{
    return (u32)port_disc_read_raw(lba, guest_ptr(dst, 2352));
}

u32 w2c_env_port_disc_find(struct w2c_env *env, u32 path, u32 lba, u32 size)
{
    unsigned l = 0, s = 0;
    int ok = port_disc_find(guest_str(path), &l, &s);

    if (ok)
    {
        store_u32(lba, l);
        store_u32(size, s);
    }
    return (u32)ok;
}

/* ---- imports: timing, input --------------------------------------------------------- */
u32 w2c_env_port_wait_vblank(struct w2c_env *env)
{
    return port_wait_vblank();
}

u32 w2c_env_port_vblank_count(struct w2c_env *env)
{
    return port_vblank_count();
}

/* Fast-forward (port_set_speed): only every speed-th frame is drawn and presented. */
static u32 sFrameIndex; /* frames presented so far */

static int frame_skipped(void)
{
    const int speed = port_speed();

    return speed > 1 && (sFrameIndex % (u32)speed) != 0;
}

void w2c_env_port_set_speed(struct w2c_env *env, u32 multiplier)
{
    port_set_speed((int)multiplier);
}

/* ---- script bridge (port_bridge.h) ---- */
void w2c_env_port_bridge_publish(struct w2c_env *env, u32 vars, u32 nvars, u32 requests, u32 nrequests)
{
    ps1w_host_bridge_publish(vars, (int)nvars, requests, (int)nrequests);
}

u32 w2c_env_port_bridge_poll(struct w2c_env *env, u32 name, u32 name_cap, u32 args, u32 max_args, u32 nargs)
{
    char buf[128];
    int values[16];
    int n = 0, id, i;
    u32 cap = name_cap < sizeof(buf) ? name_cap : (u32)sizeof(buf);

    if (cap == 0) return 0;
    id = ps1w_host_bridge_poll(buf, cap, values, (int)max_args < 16 ? (int)max_args : 16, &n);
    if (id <= 0) return 0;
    memcpy(guest_ptr(name, cap), buf, strlen(buf) + 1);
    for (i = 0; i < n; i++) store_u32(args + (u32)i * 4, (u32)values[i]);
    store_u32(nargs, (u32)n);
    return (u32)id;
}

void w2c_env_port_bridge_done(struct w2c_env *env, u32 id, u32 result)
{
    ps1w_host_bridge_done((int)id, (int)result);
}

u32 w2c_env_port_pad_state(struct w2c_env *env)
{
    return port_pad_state();
}

/* ---- imports: video, audio, debug ----------------------------------------------------- */
void w2c_env_port_present(struct w2c_env *env, u32 rgba, u32 width, u32 height)
{
    port_present((const unsigned char *)guest_ptr(rgba, width * height * 4), (int)width, (int)height);
}

#if PS1W_BIG_ENDIAN
static void *swapped16(u32 addr, u32 count)
{
    static unsigned short *buf;
    static u32 cap;
    const unsigned char *src = (const unsigned char *)guest_ptr(addr, count * 2);
    u32 i;

    if (count > cap)
    {
        free(buf);
        buf = (unsigned short *)malloc(count * 2);
        cap = count;
    }
    for (i = 0; i < count; i++) buf[i] = (unsigned short)(src[i * 2] | (src[i * 2 + 1] << 8));
    return buf;
}
#endif

void w2c_env_port_audio_push(struct w2c_env *env, u32 samples, u32 frames)
{
#if PS1W_BIG_ENDIAN
    port_audio_push((const short *)swapped16(samples, frames * 2), (int)frames);
#else
    port_audio_push((const short *)guest_ptr(samples, frames * 4), (int)frames);
#endif
}

void w2c_env_port_debug_vram(struct w2c_env *env, u32 vram)
{
#if PS1W_BIG_ENDIAN
    port_debug_vram((const unsigned short *)swapped16(vram, 1024 * 512));
#else
    port_debug_vram((const unsigned short *)guest_ptr(vram, 1024 * 512 * 2));
#endif
}

u32 w2c_env_port_debug_values(struct w2c_env *env, u32 name, u32 out, u32 max)
{
    int values[16];
    int n, i;

    n = port_debug_values(guest_str(name), values, (int)max < 16 ? (int)max : 16);
    for (i = 0; i < n; i++) store_u32(out + (u32)i * 4, (u32)values[i]);
    return (u32)n;
}

u32 w2c_env_port_trace_gpu(struct w2c_env *env)
{
    return (u32)port_trace_gpu();
}

/* ---- imports: save files ------------------------------------------------------------ */
u32 w2c_env_port_file_size(struct w2c_env *env, u32 path)
{
    return (u32)port_file_size(guest_str(path));
}

u32 w2c_env_port_file_read(struct w2c_env *env, u32 path, u32 offset, u32 dst, u32 len)
{
    return (u32)port_file_read(guest_str(path), offset, guest_ptr(dst, len), len);
}

u32 w2c_env_port_file_write(struct w2c_env *env, u32 path, u32 offset, u32 src, u32 len, u32 create)
{
    return (u32)port_file_write(guest_str(path), offset, guest_ptr(src, len), len, (int)create);
}

u32 w2c_env_port_file_delete(struct w2c_env *env, u32 path)
{
    return (u32)port_file_delete(guest_str(path));
}

u32 w2c_env_port_file_list(struct w2c_env *env, u32 dir, u32 index, u32 name, u32 cap)
{
    return (u32)port_file_list(guest_str(dir), index, (char *)guest_ptr(name, cap), cap);
}

/* ---- imports: GPU (ps1w_gpu.c runs it on the host) ------------------------------------- */
#include <stdarg.h>
#include <stdio.h>
#include "../../Runtime/port/include/port_gpu.h"

/* gpu.c logs through port_log (GPU tracing) */
void port_log(const char *fmt, ...)
{
    char buf[1024];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    host_log("%s", buf);
}

static u32 load_u32(u32 addr)
{
    const unsigned char *p = (const unsigned char *)guest_ptr(addr, 4);

    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

/* native copies of guest data handed to the GPU */
static void *scratch(size_t bytes)
{
    static void *buf;
    static size_t cap;

    if (bytes > cap)
    {
        free(buf);
        cap = bytes < 65536 ? 65536 : bytes;
        buf = malloc(cap);
        if (buf == NULL)
        {
            host_log("out of memory (%u bytes)", (unsigned)cap);
            host_crashed();
        }
    }
    return buf;
}

/* Optional profiling hook a host can set: called around the GPU imports
 * (which: 0 = draw_otag, 1 = present; begin: 1 before, 0 after). */
void (*ps1w_profile_hook)(int which, int begin);

/* PsyQ DrawOTag: walk the ordering table in guest memory (24-bit links to the next
 * packet, 0xFFFFFF ends it), each packet's words to the GPU. */
void w2c_env_gpu_draw_otag(struct w2c_env *env, u32 ot)
{
    static int sNoGpu = -1;
    unsigned long words[256];
    u32 addr = ot;
    int guard = 0;

    if (sNoGpu < 0)
    {
        /* --debug-nogpu 1: no drawing (profiling) */
        int v[1];

        sNoGpu = port_debug_values("nogpu", v, 1) == 1 && v[0];
    }
    if (sNoGpu || frame_skipped()) return;
    if (ps1w_profile_hook) ps1w_profile_hook(0, 1);

    while (guard++ < 200000)
    {
        u32 tag = load_u32(addr);
        u32 len = tag >> 24, i;

        if (len > 0)
        {
            for (i = 0; i < len; i++) words[i] = load_u32(addr + 4 + i * 4);
            gpu_gp0(words, (int)len);
        }
        if ((tag & 0xFFFFFF) == 0xFFFFFF) break;
        addr = (tag & 0xFFFFFF) | 0x80000000u;
    }
    if (ps1w_profile_hook) ps1w_profile_hook(0, 0);
}

void w2c_env_gpu_gp0(struct w2c_env *env, u32 words, u32 count)
{
    unsigned long *w = (unsigned long *)scratch(count * sizeof(unsigned long));
    u32 i;

    for (i = 0; i < count; i++) w[i] = load_u32(words + i * 4);
    gpu_gp0(w, (int)count);
}

void w2c_env_gpu_load_image(struct w2c_env *env, u32 x, u32 y, u32 w, u32 h, u32 src)
{
    const u32 n = w * h;
    const unsigned char *p = (const unsigned char *)guest_ptr(src, n * 2);
    unsigned short *pixels = (unsigned short *)scratch(n * 2);
    u32 i;

    for (i = 0; i < n; i++) pixels[i] = (unsigned short)(p[i * 2] | (p[i * 2 + 1] << 8));
    gpu_load_image((int)x, (int)y, (int)w, (int)h, pixels);
}

void w2c_env_gpu_store_image(struct w2c_env *env, u32 x, u32 y, u32 w, u32 h, u32 dst)
{
    const u32 n = w * h;
    unsigned char *p = (unsigned char *)guest_ptr(dst, n * 2);
    unsigned short *pixels = (unsigned short *)scratch(n * 2);
    u32 i;

    gpu_store_image((int)x, (int)y, (int)w, (int)h, pixels);
    for (i = 0; i < n; i++)
    {
        p[i * 2] = (unsigned char)pixels[i];
        p[i * 2 + 1] = (unsigned char)(pixels[i] >> 8);
    }
}

void w2c_env_gpu_move_image(struct w2c_env *env, u32 sx, u32 sy, u32 dx, u32 dy, u32 w, u32 h)
{
    gpu_move_image((int)sx, (int)sy, (int)dx, (int)dy, (int)w, (int)h);
}

void w2c_env_gpu_fill(struct w2c_env *env, u32 x, u32 y, u32 w, u32 h, u32 r, u32 g, u32 b)
{
    gpu_fill((int)x, (int)y, (int)w, (int)h, (int)r, (int)g, (int)b);
}

void w2c_env_gpu_set_draw_area(struct w2c_env *env, u32 x0, u32 y0, u32 x1, u32 y1)
{
    gpu_set_draw_area((int)x0, (int)y0, (int)x1, (int)y1);
}

void w2c_env_gpu_set_draw_offset(struct w2c_env *env, u32 x, u32 y)
{
    gpu_set_draw_offset((int)x, (int)y);
}

void w2c_env_gpu_set_texpage(struct w2c_env *env, u32 tpage)
{
    gpu_set_texpage((unsigned short)tpage);
}

void w2c_env_gpu_set_texwindow(struct w2c_env *env, u32 x, u32 y, u32 w, u32 h)
{
    gpu_set_texwindow((int)x, (int)y, (int)w, (int)h);
}

void w2c_env_gpu_set_dither(struct w2c_env *env, u32 on)
{
    gpu_set_dither((int)on);
}

void w2c_env_gpu_set_display(struct w2c_env *env, u32 x, u32 y, u32 w, u32 h, u32 rgb24)
{
    gpu_set_display((int)x, (int)y, (int)w, (int)h, (int)rgb24);
}

void w2c_env_gpu_set_display_enabled(struct w2c_env *env, u32 on)
{
    gpu_set_display_enabled((int)on);
}

void w2c_env_gpu_present(struct w2c_env *env)
{
    const int skipped = frame_skipped();

    sFrameIndex++;
    if (skipped) return;
    if (ps1w_profile_hook) ps1w_profile_hook(1, 1);
    gpu_present();
    if (ps1w_profile_hook) ps1w_profile_hook(1, 0);
}
