/*
 * Game entry: stands in for the PS1 BIOS loading the boot executable. The original
 * executable is copied to its load address so that data the game reaches by
 * PS1 address (tables the decomp leaves to the linker, libgs/libgte globals,
 * the file table) holds its real contents, then main() runs natively.
 */
#include <port_gte.h>
#include <port_host.h>
#include <ps1_game_config.h>

int ps1_game_main(void);
void InitGeom(void);
void port_overlays_snapshot(void);

#define EXE_HEADER_SIZE 0x800

static unsigned long rd32(const unsigned char *p)
{
    return p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static void load_executable(void)
{
    static unsigned char header[EXE_HEADER_SIZE];
    unsigned lba, size;
    unsigned long t_addr, t_size, b_addr, b_size;
    unsigned char *dst;
    unsigned long i;

    if (!port_disc_find(PS1_BOOT_EXE, &lba, &size))
    {
        port_fatal("%s not found on the disc", PS1_BOOT_EXE_NAME);
    }
    if (!port_disc_read(lba, 1, header))
    {
        port_fatal("cannot read the executable header");
    }
    t_addr = rd32(header + 0x18);
    t_size = rd32(header + 0x1C);
    b_addr = rd32(header + 0x28);
    b_size = rd32(header + 0x2C);
    port_log("%s: text %08lX+%lX bss %08lX+%lX", PS1_BOOT_EXE_NAME, t_addr, t_size, b_addr, b_size);
    if (t_addr < PORT_RAM_BASE || t_addr + t_size > PORT_RAM_BASE + PORT_RAM_SIZE)
    {
        port_fatal("unexpected load address %08lX", t_addr);
    }
    if (!port_disc_read(lba + 1, (unsigned)((t_size + 2047) / 2048), (void *)t_addr))
    {
        port_fatal("cannot read the executable");
    }
    dst = (unsigned char *)b_addr;
    for (i = 0; i < b_size; i++) dst[i] = 0;
}

void port_game_entry(void)
{
    load_executable();
    port_overlays_snapshot();
    InitGeom();
    port_log("entering main()");
    ps1_game_main();
}
