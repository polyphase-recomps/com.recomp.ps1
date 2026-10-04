/*
 * A PS1 game translated by wasm2c, as seen by the runtime. tools/wasm_to_c.py writes
 * one descriptor per game (<name>_guest_module.c) next to the generated code; the
 * addon build also gets <name>_guest_register.cpp, which registers it at startup so
 * that Ps1Player finds it by package id.
 */
#ifndef PS1W_MODULE_H
#define PS1W_MODULE_H

#include "wasm-rt.h"

#ifdef __cplusplus
extern "C" {
#endif

struct w2c_env;

typedef struct Ps1wModule
{
    const char *name;      /* build name, e.g. "dw" */
    const char *package;   /* game package id, e.g. "com.recomp.digimonworld" */
    const char *title;     /* "Digimon World" */
    const char *disc_name; /* file name of the disc image the game was built for */
    void (*instantiate)(struct w2c_env *env);
    void (*free)(void);
    void (*run)(void); /* boot + main(); returns if main returns */
    wasm_rt_memory_t *(*memory)(void);
} Ps1wModule;

/* registry (ps1w_backend.c) */
void ps1w_register_module(const Ps1wModule *module);
/* by package id; NULL or "" gives the first registered game */
const Ps1wModule *ps1w_find_module(const char *package);

/* one game instance at a time (ps1w_backend.c) */
int ps1w_instantiate(const Ps1wModule *module);
/* runs the game on the calling thread; returns when main returns or the host unwinds it */
void ps1w_run(void);
void ps1w_free(void);

#ifdef __cplusplus
}
#endif

#endif /* PS1W_MODULE_H */
