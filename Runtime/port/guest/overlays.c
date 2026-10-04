/* Overlay data reset: see the overlay section comment in cmake/Ps1Game.cmake. */
#include <port_host.h>

/* generated per game (ovl_markers.c): data range [begin, end) of each overlay, in
 * the order of the game's own overlay ids (1-based); null when an overlay has no data */
extern char *const port_ovl_begins[];
extern char *const port_ovl_ends[];
extern const int port_ovl_count;

#define MAX_OVERLAYS 64
static char *sSnapshot[MAX_OVERLAYS];

/* Copies every overlay's initial data; called once before main(). */
void port_overlays_snapshot(void)
{
    int i;
    unsigned long n, k;

    for (i = 0; i < port_ovl_count && i < MAX_OVERLAYS; i++)
    {
        char *b = port_ovl_begins[i];

        if (b == 0 || port_ovl_ends[i] <= b) continue;
        n = (unsigned long)(port_ovl_ends[i] - b);
        sSnapshot[i] = (char *)port_alloc(n);
        for (k = 0; k < n; k++) sSnapshot[i][k] = b[k];
    }
}

/* Puts overlay `lib` (1-based) back to its initial data, as loading it from the disc
 * does on the console. */
void port_overlay_reset(int lib)
{
    int i = lib - 1;
    char *b;
    unsigned long n, k;

    if (i < 0 || i >= port_ovl_count || i >= MAX_OVERLAYS || sSnapshot[i] == 0) return;
    b = port_ovl_begins[i];
    n = (unsigned long)(port_ovl_ends[i] - b);
    for (k = 0; k < n; k++) b[k] = sSnapshot[i][k];
}
