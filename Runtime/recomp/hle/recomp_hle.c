/*
 * The PsyQ libraries as the recomp runtime's wasm2c module ("ps1hle", Ps1Recomp.cmake): the
 * pieces a decomp build gets from boot.c and overlays.c, which this module leaves out (the
 * game boots itself, from its own startup code, and its overlays are real disc loads).
 */
#include <port_host.h>

/* End of the executable's BSS: where the PS1 startup code starts the heap (libc2.c). The
 * runtime sets it after loading the executable. */
unsigned long port_boot_bss_end;

void port_recomp_set_bss_end(unsigned long end)
{
    port_boot_bss_end = end;
}

/* overlays.c's: overlay data is reloaded from the disc in recomp mode */
void port_overlays_snapshot(void)
{
}

void port_overlay_reset(int lib)
{
    (void)lib;
}

/* A buffer for building variadic argument lists (printf, sprintf: wasm32 passes them in
 * memory) from the MIPS registers and stack. */
static unsigned long sVarargs[32];

unsigned long port_recomp_varargs(void)
{
    return (unsigned long)sVarargs;
}
