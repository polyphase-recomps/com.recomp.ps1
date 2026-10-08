/*
 * Interface between the guest side (game code + PsyQ replacement, compiled
 * against the PsyQ headers) and the host side (platform). Only plain C types.
 *
 * The guest is either linked into a native 32-bit Windows program (host/native)
 * or compiled to WebAssembly and translated to C with wasm2c (host/wasm), where
 * every function below that the guest does not define becomes a module import.
 * Pointers passed to the host are guest addresses; data is little-endian.
 */
#ifndef PORT_HOST_H
#define PORT_HOST_H

/* ---- memory map --------------------------------------------------------------
 * PS1 main RAM is at its real address. In the native build the area above it (up
 * to the program image at 0x80800000) holds the game thread's stack. */
#define PORT_RAM_BASE      0x80000000u
#define PORT_RAM_SIZE      0x00200000u
#define PORT_STACK_TOP     0x80700000u
#define PORT_STACK_BOTTOM  0x80400000u
#define PORT_SCRATCH_BASE  0x1F800000u
#define PORT_SCRATCH_SIZE  0x00000400u

/* ---- logging (guest: guest/log.c) --------------------------------------------- */
void port_log(const char *fmt, ...);
void port_fatal(const char *fmt, ...);

/* ---- logging (host) ----------------------------------------------------------- */
void port_host_log(const char *msg);
/* Reports a fatal error and stops the game; does not return. */
void port_host_fatal(const char *msg);

/* ---- disc --------------------------------------------------------------------- */
/* Reads `count` 2048-byte data sectors starting at absolute sector `lba`. Returns 0 on failure. */
int port_disc_read(unsigned lba, unsigned count, void *dst);
/* Reads one raw 2352-byte sector (for XA/STR). Returns 0 on failure. */
int port_disc_read_raw(unsigned lba, void *dst);
/* Looks a path up in the ISO9660 directory ("\\DIR\\FILE.EXT;1"). Returns 0 if missing. */
int port_disc_find(const char *path, unsigned *lba, unsigned *size);

/* ---- timing -------------------------------------------------------------------- */
/* Blocks the game thread until the next host video frame; returns the vblank count. */
unsigned port_wait_vblank(void);
unsigned port_vblank_count(void);
/* Fast-forward: run the game up to `multiplier` times real time (1 = normal). Hosts
 * hand out vblanks faster, drop the audio and may skip drawing frames meanwhile. */
void port_set_speed(int multiplier);
/* Recomp mode, mod code only: runs the game's function at `vram` (up to 4 arguments) and returns
 * its result; 0xFFFFFFFF, logged, when no function of the game starts there. */
unsigned port_game_call(unsigned vram, unsigned nargs, unsigned a0, unsigned a1, unsigned a2, unsigned a3);
/* Host side (not a guest import): the speed in effect, after the host's own timeout. */
int port_speed(void);

/* ---- input --------------------------------------------------------------------- */
/* PsyQ PadRead() layout (pressed = 1): pad 1 in bits 0-15, pad 2 in bits 16-31. */
unsigned port_pad_state(void);

/* ---- video --------------------------------------------------------------------- */
/* Called by the GPU when the display should show a new picture. rgba is width*height*4. */
void port_present(const unsigned char *rgba, int width, int height);

/* Debug: integer values of --debug-NAME a,b,c; returns how many were given. */
int port_debug_values(const char *name, int *out, int max);
/* Debug: true while the frame selected with --trace-gpu is being drawn. */
int port_trace_gpu(void);
/* Debug: offered the whole 1024x512 VRAM after each present (written out with --dump-vram). */
void port_debug_vram(const unsigned short *vram);

/* ---- audio --------------------------------------------------------------------- */
/* Queues interleaved stereo 16-bit samples at 44100 Hz. */
void port_audio_push(const short *samples, int frames);

/* ---- memory ------------------------------------------------------------------- */
/* Zeroed allocation that the guest never frees (native: host heap; wasm: guest heap). */
void *port_alloc(unsigned long size);

/* ---- save files ------------------------------------------------------------------
 * Paths are relative to the save folder and use '/' (e.g. "card0/BASLUS-01032").
 * Folders are created as needed. */
/* Size in bytes, or -1 if the file does not exist. */
int port_file_size(const char *path);
/* Reads up to `len` bytes at `offset`; returns the count read, or -1 if missing. */
int port_file_read(const char *path, unsigned offset, void *dst, unsigned len);
/* Writes into an existing file (create = 0) or a new, truncated one (create = 1).
 * Returns the count written, or -1. */
int port_file_write(const char *path, unsigned offset, const void *src, unsigned len, int create);
/* Returns 0 if the file was deleted. */
int port_file_delete(const char *path);
/* The index-th file of folder `dir` in name order: copies its name and returns its
 * size, or -1 past the end. */
int port_file_list(const char *dir, unsigned index, char *name, unsigned name_cap);

#endif /* PORT_HOST_H */
