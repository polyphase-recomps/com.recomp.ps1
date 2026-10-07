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

Functions are the symbols that fall inside a file's code (splat `c` / `asm` / `hasm`
subsegments of `code` segments); each runs up to the next one, or to the end of the code.
Code that only calls reveal (static functions) N64Recomp finds itself. PsyQ library code
(subsegments named psyq/...) is listed as <name>_recomp with size 0: never recompiled,
called by name, implemented by the runtime.

    python ps1_syms.py --decomp <dw_decomp> --disc-dir <folder with the disc's files> --out <dir>
                       --file SLUS_010.32=config/us/main.yaml --file BTL_REL.BIN=config/us/btl.yaml ...

The first --file is the boot executable (PS-X EXE). The ROM layout (file order, 2048-byte
alignment) is what the runtime rebuilds from the player's disc in live mode.

A game the decomp names only partly: --scan finds the functions nobody named (ps1_scan.py),
and an overlay with no splat config is --file NAME@0xVRAM (the address the game loads it at):
all of it is scanned, its code ends where the scan does.
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
                ranges.append((start + seg_delta, end + seg_delta, name.startswith("psyq/")))
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


# BIOS functions by table (A0 / B0 / C0) and number: what PsyQ's call stubs reach
BIOS = {
    0xA0: {0x13: "setjmp", 0x14: "longjmp", 0x33: "malloc", 0x34: "free", 0x39: "InitHeap", 0x3F: "printf",
           0x44: "FlushCache", 0x49: "GPU_cw", 0x70: "_bu_init", 0x72: "CdRemove", 0x9F: "SetMem", 0xA1: "SystemError"},
    0xB0: {0x07: "DeliverEvent", 0x08: "OpenEvent", 0x09: "CloseEvent", 0x0A: "WaitEvent", 0x0B: "TestEvent",
           0x0C: "EnableEvent", 0x0D: "DisableEvent", 0x0E: "OpenTh", 0x0F: "CloseTh", 0x10: "ChangeTh",
           0x12: "InitPAD", 0x13: "StartPAD", 0x14: "StopPAD", 0x15: "PAD_init", 0x16: "PAD_dr",
           0x17: "ReturnFromException", 0x18: "ResetEntryInt", 0x19: "HookEntryInt", 0x20: "UnDeliverEvent",
           0x32: "open", 0x33: "lseek", 0x34: "read", 0x35: "write", 0x36: "close", 0x3F: "puts",
           0x42: "firstfile", 0x43: "nextfile", 0x44: "rename", 0x45: "erase", 0x4A: "InitCARD", 0x4B: "StartCARD",
           0x4C: "StopCARD", 0x4E: "_card_write", 0x4F: "_card_read", 0x50: "_new_card", 0x56: "GetC0Table",
           0x57: "GetB0Table", 0x5B: "ChangeClearPad"},
    0xC0: {0x0A: "ChangeClearRCnt"},
}


def bios_stub_name(data, delta, pc):
    """`addiu t2, zero, 0xA0|0xB0|0xC0; jr t2; addiu t1, zero, N`: the BIOS function's name."""
    off = pc - delta
    if off < 0 or off + 12 > len(data):
        return None
    a, b, c = struct.unpack_from("<3I", data, off)
    if (a >> 16) != 0x240A or b != 0x01400008 or (c >> 16) != 0x2409:
        return None
    return BIOS.get(a & 0xFFFF, {}).get(c & 0xFFFF)


def section_name_of(disc_name):
    """A C name for a file's section: SLUS_010.32 -> slus_010, SYSTEM/X00.BIN|... -> x00."""
    base = os.path.splitext(os.path.basename(disc_name.split("|")[0]))[0].lower()
    return re.sub(r"[^a-z0-9_]", "_", base)


def load_file(spec, decomp, disc_dir):
    """A --file: (disc name, data, code ranges, file offset -> vram delta, symbols)."""
    if "=" in spec:
        disc_name, yaml_path = spec.split("=", 1)
        with open(os.path.join(decomp, yaml_path), encoding="utf-8") as f:
            config = yaml.safe_load(f)
        ranges, delta = code_ranges(config)
        if delta is None:
            raise SystemExit(f"{yaml_path}: no segment with a vram")
        syms = read_symbols(config_symbol_paths(config, decomp))
    elif "@" in spec:
        disc_name, vram = spec.split("@", 1)
        delta = int(vram, 0)
        ranges, syms = None, []
    else:
        raise SystemExit(f"--file {spec}: DISCFILE=yaml or DISCFILE@0xVRAM")
    # PRIMARY|ALIAS|...: the same file at several places on the disc (the primary is read; a
    # read of any of them loads the section)
    with open(os.path.join(disc_dir, disc_name.split("|")[0]), "rb") as f:
        data = f.read()
    data += b"\0" * ((-len(data)) % 4)
    if ranges is None:
        ranges = [(delta, delta + len(data), False)]  # all of it: scanned
    return disc_name, data, merge(ranges), delta, syms


def runs_into(data, delta, start, end):
    """Whether the code of the function at `start` runs on past `end` (the next symbol): its last
    instructions before `end` (padding aside) are not a return or a jump away (jr, j, b) and its
    delay slot."""
    def word(pc):
        return struct.unpack_from("<I", data, pc - delta)[0]

    def leaves(w):
        return (w & 0xFC1FFFFF) == 0x00000008 or w >> 26 == 2 or w >> 16 == 0x1000

    pc = end - 4
    while pc > start and word(pc) == 0:
        pc -= 4
    return pc > start and not leaves(word(pc)) and not leaves(word(pc - 4))


def carve_entry(boot):
    """The boot executable's entry point (PS-X EXE pc0) is recompiled, also where a config counts
    it as library code (PsyQ's start code sits among the libraries): its function leaves the
    PsyQ range it is in, as far as its code goes."""
    import ps1_scan
    name, data, ranges, delta, syms = boot
    pc0 = struct.unpack_from("<I", data, 0x10)[0]
    for k, (lo, hi, is_psyq) in enumerate(ranges):
        if lo <= pc0 < hi and is_psyq:
            res = ps1_scan.Code(data, delta, lo, hi).walk(pc0, set())
            if res is None:
                raise SystemExit(f"{name}: the entry point {pc0:08X} isn't code")
            end = min(max(res[0]) + 4, hi)
            ranges[k:k + 1] = [r for r in ((lo, pc0, True), (pc0, end, False), (end, hi, True)) if r[1] > r[0]]
            print(f"{name}: entry point {pc0:08X}-{end:08X} recompiled (it was in PsyQ code)")
            return


def scan(loaded, specs):
    """ps1_scan.py over every file's game code: the functions nobody named (rounds, as one file's
    code reveals calls into another's). A call from one file into its own address span is its
    own (overlays share addresses); one outside it is into the files loaded there."""
    import ps1_scan
    found = [dict() for _ in loaded]
    calls = [set() for _ in loaded]  # jal targets of each file's code
    spans = [(delta, delta + len(data)) for _, data, _, delta, _ in loaded]
    # words of the other files: their data may point at this one's code (callback tables)
    pointers = set()
    for _, data, _, _, _ in loaded:
        pointers.update(w for w in struct.unpack(f"<{len(data) // 4}I", data) if 0x80000000 <= w < 0x80200000 and not w & 3)
    others = [pointers] * len(loaded)
    for _ in range(4):
        before = [len(c) for c in calls]
        for i, (name, data, ranges, delta, syms) in enumerate(loaded):
            named = {a for _, a in syms if not a & 3}
            into = set(calls[i])
            for j, c in enumerate(calls):
                if j != i:
                    into |= {t for t in c if not spans[j][0] <= t < spans[j][1]}
            for k, (lo, hi, is_psyq) in enumerate(ranges):
                # (library code too: what the symbols don't name in it is recompiled)
                own = {t for t in calls[i] if lo <= t < hi}
                # calls from other files: only where a function can start (overlays share addresses)
                other = {t for t in into - calls[i] if lo <= t < hi and ps1_scan.plausible_start(data, delta, lo, hi, t)}
                # a code range from a config starts a function (a scanned overlay starts with a table)
                first = set() if "=" not in specs[i] else {lo}
                seeds = {a for a in named if lo <= a < hi} | own | first
                starts, code_end, outside, ends = ps1_scan.discover(data, delta, lo, hi, seeds, others[i], other,
                                                                    {a for a in named if lo <= a < hi})
                found[i][k] = (starts, code_end, ends)
                calls[i] |= outside
        if [len(c) for c in calls] == before:
            break
    return found, set().union(*calls)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--decomp", required=True)
    ap.add_argument("--disc-dir", required=True, help="folder with the disc's files (extract_disc.py output)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--file", action="append", required=True,
                    help="DISCFILE=splat yaml (relative to --decomp), or DISCFILE@0xVRAM (no config: scanned)")
    ap.add_argument("--scan", action="store_true", help="find the functions the symbols don't name")
    ap.add_argument("--rename", action="append", default=[],
                    help="OLD=NEW: a function the decomp's symbols name wrongly (a PsyQ function is the "
                         "runtime's by its name)")
    ap.add_argument("--name", action="append", default=[],
                    help="0xADDR=NAME: a library function the decomp's symbols don't name (the runtime has "
                         "it by NAME)")
    args = ap.parse_args()
    renames = dict(r.split("=", 1) for r in args.rename)
    extra_names = [(n, int(a, 0)) for a, n in (s.split("=", 1) for s in args.name)]

    os.makedirs(args.out, exist_ok=True)
    rom = bytearray()
    sections = []
    psyq = set()
    files = []
    used_names = set()
    data_syms = {}
    loaded = [load_file(spec, args.decomp, args.disc_dir) for spec in args.file]
    loaded = [(n, d, r, dl, [(renames.get(s, s), a) for s, a in syms] +
               [(s, a) for s, a in extra_names if any(lo <= a < hi for lo, hi, _ in r)])
              for n, d, r, dl, syms in loaded]
    carve_entry(loaded[0])
    if any("@" in spec and "=" not in spec for spec in args.file) and not args.scan:
        raise SystemExit("a --file NAME@0xVRAM is scanned: add --scan")
    scanned, scan_targets = scan(loaded, args.file) if args.scan else ([{} for _ in loaded], set())
    # calls from game code into PsyQ code at addresses the symbols don't name
    all_targets = set().union(*(jal_targets(data, delta, ranges) for _, data, ranges, delta, _ in loaded)) \
        if not args.scan else scan_targets
    all_ranges = [r for _, _, ranges, _, _ in loaded for r in ranges]

    for file_index, (disc_name, data, ranges, delta, syms) in enumerate(loaded):
        # an overlay scanned whole: its code ends where the scan does (data follows it)
        for k, (lo, hi, is_psyq) in enumerate(list(ranges)):
            if k in scanned[file_index] and "@" in args.file[file_index] and "=" not in args.file[file_index]:
                ranges[k] = (lo, max(lo, scanned[file_index][k][1]), is_psyq)
        rom_base = len(rom)
        files.append(disc_name)
        # big-endian words (see above)
        rom += b"".join(struct.pack(">I", w) for w in struct.unpack(f"<{len(data) // 4}I", data))
        rom += b"\0" * ((-len(rom)) % ALIGN)

        vram_start = delta  # file offset 0
        vram_end = delta + len(data)
        funcs = {}
        data_after = {}  # a function's code ends early: data (or other code) follows it
        for name, addr in syms:
            if not any(lo <= addr < hi for lo, hi, _ in all_ranges):
                data_syms.setdefault(name, addr)
            if addr & 3:
                continue
            for lo, hi, is_psyq in ranges:
                if lo <= addr < hi:
                    funcs.setdefault(addr, (name, is_psyq, hi))
                    break
        # calls into library code the symbols don't name: a BIOS call stub is named after its
        # BIOS function (the runtime has those); any other is recompiled, with the unnamed
        # library code it calls in turn (the runtime can only stand in for what it can name)
        todo = sorted(all_targets)
        while todo:
            target = todo.pop()
            for lo, hi, is_psyq in ranges:
                if not (is_psyq and lo <= target < hi) or target in funcs:
                    continue
                bios = bios_stub_name(data, delta, target)
                if bios is not None:
                    funcs[target] = (bios if not any(n == bios for n, _, _ in funcs.values()) else f"psyq_{target:08X}", True, hi)
                    break
                import ps1_scan
                res = ps1_scan.Code(data, delta, lo, hi).walk(target, set(funcs))
                if res is None:
                    funcs[target] = (f"psyq_{target:08X}", True, hi)
                    break
                funcs[target] = (f"func_{target:08X}", False, hi)
                data_after[target] = max(res[0]) + 4
                todo.extend(c for c in res[1] if lo <= c < hi and c not in funcs)
                break
        # the functions the scan found (named after their address); in library code, those the
        # symbols don't name are recompiled (the runtime stands in for named ones only)
        for k, (starts, _, ends) in scanned[file_index].items():
            data_after.update(ends)
            lo, hi, is_psyq = ranges[k]
            for a in starts:
                if lo <= a < hi and a not in funcs:
                    bios = bios_stub_name(data, delta, a) if is_psyq else None
                    if bios is not None and not any(n == bios for n, _, _ in funcs.values()):
                        funcs[a] = (bios, True, hi)
                    else:
                        funcs[a] = (f"func_{a:08X}", False, hi)
        # every code range starts a function (a symbol-less start is named after its address),
        # unless it is a scanned overlay's (its code starts after a table)
        for k, (lo, hi, is_psyq) in enumerate(ranges):
            if lo not in funcs and not (k in scanned[file_index] and "=" not in args.file[file_index]):
                funcs[lo] = (f"func_{lo:08X}", is_psyq, hi)
        addrs = sorted(funcs)
        entries = []
        for i, addr in enumerate(addrs):
            name, is_psyq, range_end = funcs[addr]
            end = min(addrs[i + 1] if i + 1 < len(addrs) else range_end, range_end, data_after.get(addr, range_end))
            # a label the symbols put inside a function (its code runs on into it): the function goes
            # on to its own end (the label stays a function too, for calls to it)
            if not is_psyq and i + 1 < len(addrs) and end == addrs[i + 1] and not funcs[end][1] and end < range_end and \
                    runs_into(data, delta, addr, end):
                import ps1_scan
                lo = max(r_lo for r_lo, r_hi, _ in ranges if r_lo <= addr < r_hi)
                res = ps1_scan._code(data, delta, lo, range_end).walk(addr, set())
                if res is not None and max(res[0]) + 4 > end:
                    end = min(max(res[0]) + 4, range_end)
            if is_psyq:
                psyq.add(name)
                entries.append((f"{name}_recomp", addr, 0))
                continue
            # overlays reuse names (and addresses): each C function needs its own
            if name in RESERVED:
                name = f"{name}_game"
            unique = name if name not in used_names else f"{name}_{section_name_of(disc_name)}"
            while unique in used_names:
                unique += "_"
            used_names.add(unique)
            entries.append((unique, addr, end - addr))
        section_name = section_name_of(disc_name)
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
