"""The recompiler's input from the player's disc (com.recomp.ps1 recomp mode, Recomp build).

N64Recomp (PS1 mode) recompiles a "ROM": the disc files the game's syms.toml names ([ps1]
files: the boot executable and its overlays), one after the other at 2048-byte boundaries,
every 32-bit word stored big-endian (N64Recomp byte-swaps instructions as it reads them).
The live recompiler makes the same thing at boot (runtime/recomp/ps1_live.cpp).

    python ps1_rom.py --syms <syms.toml> --disc <image.bin|.cue|.iso, or an extract_disc.py folder>
                      --out <dir> [--sha1 <the boot executable's>]

writes <out>/rom.bin and <out>/section_files.c (the disc file of each section, for the
runtime). With --sha1 the boot executable is checked first: the symbols describe one
executable, and another region or revision would be recompiled wrongly.
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import extract_disc  # noqa: E402

ALIGN = 2048


def section_files(syms_path):
    """[ps1] files of a syms.toml (written by ps1_syms.py: one line, quoted names)."""
    in_ps1 = False
    with open(syms_path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("["):
                in_ps1 = line == "[ps1]"
            elif in_ps1 and line.startswith("files"):
                return [p.strip().strip('"') for p in line.split("=", 1)[1].strip().strip("[]").split(",") if p.strip()]
    sys.exit(f"{syms_path}: no [ps1] files")


def disc_files(disc):
    """name -> bytes reader for a disc image or an extracted folder."""
    if os.path.isdir(disc):
        def read(name):
            path = os.path.join(disc, *name.split("/"))
            if not os.path.isfile(path):
                sys.exit(f"{name} is not in {disc}")
            with open(path, "rb") as f:
                return f.read()
        return read
    img = extract_disc.Image(disc)
    pvd = img.data(16)
    if pvd[1:6] != b"CD001":
        sys.exit(f"{disc} is not a PS1 disc (no ISO 9660 volume)")
    root_lba, root_size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
    entries = []
    extract_disc.walk(img, root_lba, root_size, "", entries, set())
    table = {name.upper(): (lba, size) for name, lba, size in entries}

    def read(name):
        hit = table.get(name.upper())
        if hit is None:
            sys.exit(f"{name} is not on {disc}")
        lba, size = hit
        data = b"".join(img.data(lba + i) for i in range((size + 2047) // 2048))
        return data[:size]
    return read


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--syms", required=True)
    ap.add_argument("--disc", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sha1", default="")
    args = ap.parse_args()

    files = section_files(args.syms)
    read = disc_files(args.disc)
    rom = bytearray()
    for i, name in enumerate(files):
        data = read(name)
        if i == 0 and args.sha1:
            sha1 = hashlib.sha1(data).hexdigest()
            if sha1 != args.sha1.lower():
                sys.exit(f"{name} on this disc (sha1 {sha1}) is not the one the game's symbols describe ({args.sha1})")
        data += b"\0" * ((-len(data)) % 4)
        rom += b"".join(struct.pack(">I", w) for w in struct.unpack(f"<{len(data) // 4}I", data))
        rom += b"\0" * ((-len(rom)) % ALIGN)
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "rom.bin")
    if not os.path.exists(path) or open(path, "rb").read() != rom:
        with open(path, "wb") as f:
            f.write(rom)
    text = ("/* Made by ps1_rom.py: the disc file of each recompiled section, in section order. */\n"
            "static const char *const sFiles[] = {" + ", ".join(f'"{n}"' for n in files) + "};\n"
            f"const char *ps1r_section_file(unsigned index) {{ return index < {len(files)} ? sFiles[index] : \"\"; }}\n"
            f"unsigned ps1r_section_file_count(void) {{ return {len(files)}; }}\n")
    path = os.path.join(args.out, "section_files.c")
    if not os.path.exists(path) or open(path, encoding="utf-8").read() != text:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    print(f"ps1_rom: {len(files)} files, rom.bin {len(rom)} bytes -> {args.out}")


if __name__ == "__main__":
    main()
