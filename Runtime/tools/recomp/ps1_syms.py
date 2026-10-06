"""PS1 game -> N64Recomp input (com.recomp.ps1 recomp mode).

From a splat-based decomp's configs and the game's own files (the boot executable and its
overlay files, taken from the player's disc), makes what N64Recomp (PS1 mode) recompiles:

  <out>/syms.toml   one [[section]] per file (its whole image: code, rodata, data), with the
                    functions in its code; a [ps1] table naming the files in ROM order
  <out>/rom.bin     the files one after the other (each at a multiple of 2048 bytes), every
                    32-bit word stored big-endian: N64Recomp reads N64 ROMs that way and
                    byte-swaps each instruction, so PS1 (little-endian) words come out right
  <out>/section_files.c  the disc file of each section, for the runtime
  <out>/data_symbols.txt the game's data symbols (tools/recomp/make_hle_symbols.py)
  <out>/psyq.txt    the PsyQ library functions (the runtime implements them; see
                    runtime/recomp/ps1_hle_funcs.h)

Functions are the symbols that fall inside a file's code (splat `c` / `asm` subsegments of
`code` segments); each runs up to the next one, or to the end of the code. Code that only
calls reveal (static functions) N64Recomp finds itself. PsyQ library functions (`asm`
subsegments named psyq/...) are listed as <name>_recomp with size 0: never recompiled,
called by name, implemented by the runtime.

    python ps1_syms.py --decomp <dw_decomp> --disc-dir <folder with the disc's files> --out <dir>
                       --file SLUS_010.32=config/us/main.yaml --file BTL_REL.BIN=config/us/btl.yaml ...

The first --file is the boot executable (PS-X EXE). The ROM layout (file order, 2048-byte
alignment) is what the runtime rebuilds from the player's disc in live mode.
"""
import argparse
import os
import re
import struct
import sys

import yaml

ALIGN = 2048
# game functions that would clash with the host's C library or C itself get "_game"
RESERVED = set("""main exit abort atexit malloc free calloc realloc memcpy memset memmove memcmp memchr
strcpy strncpy strlen strcmp strncmp strcat strncat strchr strrchr strstr strtol strtoul atoi atol
sprintf printf vsprintf snprintf puts putchar rand srand abs labs qsort bsearch setjmp longjmp
sin cos tan sqrt atan atan2 pow floor ceil fabs toupper tolower isdigit isalpha isspace
open close read write lseek""".split())
CODE_TYPES = {"c", "asm", "hasm"}
SYMBOL_RE = re.compile(r"^\s*([A-Za-z_.$][\w.$]*)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;")


def read_symbols(paths):
    syms = []
    for path in paths:
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = SYMBOL_RE.match(line)
                if m:
                    syms.append((m.group(1), int(m.group(2), 0)))
    return syms


def code_ranges(config):
    """(vram_start, vram_end, is_psyq) ranges of code in a splat config, and the file's
    offset -> vram delta."""
    ranges = []
    delta = None
    segments = config.get("segments", [])
    # segment boundaries: each segment runs to the next one's start
    starts = []
    for seg in segments:
        if isinstance(seg, dict):
            starts.append(seg.get("start"))
        elif isinstance(seg, list) and seg:
            starts.append(seg[0])
    for i, seg in enumerate(segments):
        if not isinstance(seg, dict) or "vram" not in seg:
            continue
        seg_delta = seg["vram"] - seg["start"]
        if delta is None:
            delta = seg_delta
        elif delta != seg_delta:
            raise SystemExit(f"{seg.get('name')}: vram - file offset differs between segments of one file")
        seg_end = next((s for s in starts[i + 1:] if s is not None), None)
        if seg.get("type") != "code":
            continue
        subs = seg.get("subsegments", [])
        for j, sub in enumerate(subs):
            if not isinstance(sub, list) or len(sub) < 2:
                continue
            start, kind = sub[0], str(sub[1])
            name = str(sub[2]) if len(sub) > 2 else ""
            end = subs[j + 1][0] if j + 1 < len(subs) and isinstance(subs[j + 1], list) else seg_end
            if kind in CODE_TYPES and end is not None and end > start:
                ranges.append((start + seg_delta, end + seg_delta, kind == "asm" and name.startswith("psyq/")))
    return ranges, delta


def config_symbol_paths(config, decomp):
    opts = config.get("options", {})
    paths = opts.get("symbol_addrs_path", [])
    if isinstance(paths, str):
        paths = [paths]
    return [os.path.join(decomp, p) for p in paths]


def merge(ranges):
    """Adjacent ranges of the same kind as one (functions run across file boundaries)."""
    out = []
    for r in sorted(ranges):
        if out and out[-1][1] == r[0] and out[-1][2] == r[2]:
            out[-1] = (out[-1][0], r[1], r[2])
        else:
            out.append(r)
    return out


def jal_targets(data, delta, ranges):
    """Targets of the jal instructions in a file's game code (not its PsyQ code)."""
    out = set()
    for lo, hi, is_psyq in ranges:
        if is_psyq:
            continue
        for vram in range(lo, hi, 4):
            off = vram - delta
            if off + 4 > len(data):
                break
            word = struct.unpack_from("<I", data, off)[0]
            if word >> 26 == 3:
                out.add((vram & 0xF0000000) | ((word & 0x03FFFFFF) << 2))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--decomp", required=True)
    ap.add_argument("--disc-dir", required=True, help="folder with the disc's files (extract_disc.py output)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--file", action="append", required=True, help="DISCFILE=splat yaml (relative to --decomp)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    rom = bytearray()
    sections = []
    psyq = set()
    files = []
    used_names = set()
    data_syms = {}
    # calls from game code into PsyQ code at addresses the symbols don't name
    loaded = []
    for spec in args.file:
        disc_name, yaml_path = spec.split("=", 1)
        with open(os.path.join(args.decomp, yaml_path), encoding="utf-8") as f:
            config = yaml.safe_load(f)
        with open(os.path.join(args.disc_dir, disc_name), "rb") as f:
            data = f.read()
        ranges, delta = code_ranges(config)
        loaded.append((ranges, delta, jal_targets(data, delta, ranges)))
    all_targets = set().union(*(t for _, _, t in loaded))
    all_ranges = [r for ranges, _, _ in loaded for r in ranges]

    for spec in args.file:
        disc_name, yaml_path = spec.split("=", 1)
        with open(os.path.join(args.decomp, yaml_path), encoding="utf-8") as f:
            config = yaml.safe_load(f)
        with open(os.path.join(args.disc_dir, disc_name), "rb") as f:
            data = f.read()
        data += b"\0" * ((-len(data)) % 4)
        ranges, delta = code_ranges(config)
        if delta is None:
            raise SystemExit(f"{yaml_path}: no segment with a vram")
        ranges = merge(ranges)
        rom_base = len(rom)
        files.append(disc_name)
        # big-endian words (see above)
        rom += b"".join(struct.pack(">I", w) for w in struct.unpack(f"<{len(data) // 4}I", data))
        rom += b"\0" * ((-len(rom)) % ALIGN)

        vram_start = delta  # file offset 0
        vram_end = delta + len(data)
        syms = read_symbols(config_symbol_paths(config, args.decomp))
        funcs = {}
        for name, addr in syms:
            if not any(lo <= addr < hi for lo, hi, _ in all_ranges):
                data_syms.setdefault(name, addr)
            if addr & 3:
                continue
            for lo, hi, is_psyq in ranges:
                if lo <= addr < hi:
                    funcs.setdefault(addr, (name, is_psyq, hi))
                    break
        for target in all_targets:
            for lo, hi, is_psyq in ranges:
                if is_psyq and lo <= target < hi and target not in funcs:
                    funcs[target] = (f"psyq_{target:08X}", True, hi)
        # every code range starts a function (a symbol-less start is named after its address)
        for lo, hi, is_psyq in ranges:
            if lo not in funcs:
                funcs[lo] = (f"func_{lo:08X}", is_psyq, hi)
        addrs = sorted(funcs)
        entries = []
        for i, addr in enumerate(addrs):
            name, is_psyq, range_end = funcs[addr]
            end = min(addrs[i + 1] if i + 1 < len(addrs) else range_end, range_end)
            if is_psyq:
                psyq.add(name)
                entries.append((f"{name}_recomp", addr, 0))
                continue
            # overlays reuse names (and addresses): each C function needs its own
            if name in RESERVED:
                name = f"{name}_game"
            unique = name if name not in used_names else f"{name}_{os.path.splitext(disc_name)[0].lower()}"
            while unique in used_names:
                unique += "_"
            used_names.add(unique)
            entries.append((unique, addr, end - addr))
        section_name = os.path.splitext(disc_name)[0].lower().replace(".", "_")
        sections.append((section_name, rom_base, vram_start, vram_end - vram_start, entries))
        game_funcs = sum(1 for e in entries if e[2] > 0)
        print(f"{disc_name}: vram 0x{vram_start:08X}-0x{vram_end:08X}, {game_funcs} functions, "
              f"{len(entries) - game_funcs} PsyQ, rom 0x{rom_base:X}")

    with open(os.path.join(args.out, "rom.bin"), "wb") as f:
        f.write(rom)
    with open(os.path.join(args.out, "psyq.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(sorted(psyq)) + "\n")
    with open(os.path.join(args.out, "syms.toml"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# Made by com.recomp.ps1 Runtime/tools/recomp/ps1_syms.py: the game's executable and\n"
                "# overlays as N64Recomp (PS1 mode) sections. PsyQ functions (size 0, <name>_recomp)\n"
                "# are the runtime's.\n")
        f.write("[ps1]\nfiles = [" + ", ".join(f'"{n}"' for n in files) + f"]\nalign = {ALIGN}\n\n")
        for name, rom_base, vram, size, entries in sections:
            f.write(f'[[section]]\nname = "{name}"\nrom = 0x{rom_base:08X}\nvram = 0x{vram:08X}\nsize = 0x{size:X}\n\nfunctions = [\n')
            for fname, addr, fsize in entries:
                f.write(f'    {{ name = "{fname}", vram = 0x{addr:08X}, size = 0x{fsize:X} }},\n')
            f.write("]\n\n")
    # the game's data symbols (the runtime's libraries use PsyQ's globals at these addresses)
    with open(os.path.join(args.out, "data_symbols.txt"), "w", encoding="utf-8", newline="\n") as f:
        for name, addr in sorted(data_syms.items(), key=lambda kv: (kv[1], kv[0])):
            f.write(f"{name} = 0x{addr:08X};\n")
    # which disc file each section is (the runtime makes a section live when its file is read)
    with open(os.path.join(args.out, "section_files.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write("/* Made by ps1_syms.py: the disc file of each recompiled section, in section order. */\n")
        f.write("static const char *const sFiles[] = {" + ", ".join(f'"{n}"' for n in files) + "};\n")
        f.write("const char *ps1r_section_file(unsigned index) { return index < %d ? sFiles[index] : \"\"; }\n" % len(files))
        f.write("unsigned ps1r_section_file_count(void) { return %d; }\n" % len(files))
    print(f"rom.bin {len(rom)} bytes, {len(psyq)} PsyQ functions -> {args.out}")


if __name__ == "__main__":
    sys.exit(main())
