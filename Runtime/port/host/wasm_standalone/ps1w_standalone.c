/*
 * host_backend.h for the standalone programs built with the wasm2c guest (win32/,
 * ogc/): runs the game the build generated (ps1w_default_module, written by
 * Ps1Game.cmake) through the shared backend in Source/Wasm.
 */
#include "ps1w_module.h"
#include "ps1w_bridge.h"
#include "../host_backend.h"

extern const Ps1wModule *const ps1w_default_module;

int ps1_backend_init(void)
{
    return ps1w_instantiate(ps1w_default_module);
}

void ps1_backend_run(void)
{
    ps1w_run();
}

size_t ps1_backend_stack_size(void)
{
    /* wasm2c functions keep the wasm locals on the C stack */
    return (size_t)16 << 20;
}

void ps1_backend_fatal(void)
{
    host_crashed();
}

/* Script bridge: only Polyphase (Ps1GuestHost) serves scripts; here the game's tables
 * are ignored and no requests ever come. */
void ps1w_host_bridge_publish(unsigned vars, int nvars, unsigned requests, int nrequests)
{
    (void)vars, (void)nvars, (void)requests, (void)nrequests;
}

int ps1w_host_bridge_poll(char *name, unsigned name_cap, int *args, int max_args, int *nargs)
{
    (void)name, (void)name_cap, (void)args, (void)max_args;
    *nargs = 0;
    return 0;
}

void ps1w_host_bridge_done(int id, int result)
{
    (void)id, (void)result;
}
