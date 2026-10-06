/*
 * The section table types N64Recomp's generated recomp_overlays.inl uses (it includes
 * "librecomp/sections.h"); com.recomp.ps1's recomp runtime. Field names and order follow
 * what the generator emits.
 */
#ifndef PS1_RECOMP_SECTIONS_H
#define PS1_RECOMP_SECTIONS_H

#include <stddef.h>
#include <stdint.h>

#include "recomp.h"

#define ARRLEN(x) (sizeof(x) / sizeof((x)[0]))

typedef struct
{
    recomp_func_t *func;
    uint32_t offset;   /* from the section's start */
    uint32_t rom_size; /* bytes of code */
} FuncEntry;

typedef struct
{
    uint32_t offset;
    uint32_t target_section_offset;
    uint16_t target_section;
    uint8_t type;
} RelocEntry;

typedef struct
{
    uint32_t rom_addr;
    uint32_t ram_addr;
    uint32_t size;
    FuncEntry *funcs;
    size_t num_funcs;
    RelocEntry *relocs;
    size_t num_relocs;
    size_t index;
} SectionTableEntry;

#endif /* PS1_RECOMP_SECTIONS_H */
