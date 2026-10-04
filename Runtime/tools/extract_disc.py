#!/usr/bin/env python3
"""Extract a PS1 disc image into a folder the runtime reads instead of the image.

    extract_disc.py [--force] <image.bin|.cue|.iso> <out_dir>

The folder holds the disc's files under their own paths, so they can be modded, and
disc.idx, which maps the original sector ranges to them: games mostly address the disc
by sector number (their own file tables) rather than by name.

    # PS1 disc index: <first sector> <sector count> <d|r> <path>[@<sector offset>]
    24 1 d SYSTEM.CNF
    ...

  d  plain data file (2048-byte sectors; reading past its end gives zeros)
  r  raw 2352-byte sectors: files with Mode 2 Form 2 sectors (STR movies, XA audio,
     whose audio exists only in that form), and _unfiled.raw, the sectors outside any
     file that are not empty (system area, directories, padding with data)

A modded file is read in place of the original as far as the original sector range
goes (the game's own tables still say where and how long it is). Existing files are
never overwritten, so re-running the tool keeps mods; --force restores the originals.
"""
import os
import struct
import sys

RAW = 2352
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


class Image:
    def __init__(self, path):
        if path.lower().endswith(".cue"):
            path = self.bin_from_cue(path)
        self.f = open(path, "rb")
        size = os.path.getsize(path)
        self.f.seek(0)
        self.raw = size % RAW == 0 and self.f.read(12) == SYNC
        self.sectors = size // (RAW if self.raw else 2048)

    @staticmethod
    def bin_from_cue(cue):
        for line in open(cue, encoding="latin1"):
            line = line.strip()
            if line.upper().startswith("FILE"):
                name = line[line.index('"') + 1:line.rindex('"')] if '"' in line else line.split()[1]
                return os.path.join(os.path.dirname(cue), name)
        sys.exit("no FILE line in " + cue)

    def raw_sector(self, lba):
        """2352 bytes (a 2048-byte image gets a Mode 2 Form 1 header made up for it)."""
        if self.raw:
            self.f.seek(lba * RAW)
            return self.f.read(RAW)
        self.f.seek(lba * 2048)
        data = self.f.read(2048)
        return SYNC + bytes(4) + b"\x00\x00\x08\x00" * 2 + data + bytes(280)

    def data(self, lba):
        return self.raw_sector(lba)[24:24 + 2048]


def walk(img, lba, size, prefix, out, dirs):
    count = (size + 2047) // 2048
    dirs.update(range(lba, lba + count))
    data = b"".join(img.data(lba + i) for i in range(count))
    pos = 0
    while pos < len(data):
        length = data[pos]
        if length == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        rec = data[pos:pos + length]
        ext_lba, ext_size = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
        name = rec[33:33 + rec[32]].decode("latin1")
        pos += length
        if name in ("\x00", "\x01"):
            continue
        if rec[25] & 2:
            walk(img, ext_lba, ext_size, prefix + name + "/", out, dirs)
        else:
            out.append((prefix + name.split(";")[0], ext_lba, ext_size))


FORCE = False


def write_if_changed(path, data, keep_existing=True):
    """Writes the file unless it is there already (a mod, or a previous extraction)."""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    if os.path.exists(path):
        if keep_existing and not FORCE:
            return 0
        if os.path.getsize(path) == len(data):
            with open(path, "rb") as f:
                if f.read() == data:
                    return 0
    with open(path, "wb") as f:
        f.write(data)
    return 1


def main():
    global FORCE
    args = [a for a in sys.argv[1:] if a != "--force"]
    FORCE = len(args) != len(sys.argv) - 1
    src, out_dir = args[0], args[1]
    img = Image(src)
    pvd = img.data(16)
    if pvd[1:6] != b"CD001":
        sys.exit("not an ISO9660 disc image: " + src)
    root = pvd[156:190]
    files, dirs = [], set()
    walk(img, struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "", files, dirs)

    index, covered, written = [], set(), 0
    for name, lba, size in sorted(files, key=lambda x: x[1]):
        count = max(1, (size + 2047) // 2048)
        covered.update(range(lba, lba + count))
        sectors = [img.raw_sector(lba + i) for i in range(count)]
        raw = img.raw and any(s[18] & 0x20 for s in sectors)  # subheader submode: form 2
        if raw:
            data = b"".join(sectors)
        else:
            data = b"".join(s[24:24 + 2048] for s in sectors)[:size]
        written += write_if_changed(os.path.join(out_dir, name), data)
        index.append((lba, count, "r" if raw else "d", name))

    # sectors outside every file that hold something (system area, directories, ...)
    unfiled, ranges, start = [], [], None
    for lba in range(img.sectors):
        keep = False
        if lba not in covered:
            sector = img.raw_sector(lba)
            keep = lba < 16 or lba in dirs or any(sector[24:24 + 2048])
        if keep:
            if start is None:
                start = lba
            unfiled.append(sector)
        elif start is not None:
            ranges.append((start, lba - start))
            start = None
    if start is not None:
        ranges.append((start, img.sectors - start))
    offset = 0
    for first, count in ranges:
        # all in one raw file: each range continues where the previous one ended
        index.append((first, count, "r", "_unfiled.raw" + ("@%d" % offset if offset else "")))
        offset += count
    written += write_if_changed(os.path.join(out_dir, "_unfiled.raw"), b"".join(unfiled), False)

    lines = ["# PS1 disc index: <first sector> <sector count> <d|r> <path>[@<sector offset in file>]",
             "# extracted from %s (%d sectors) by com.recomp.ps1 extract_disc.py" % (os.path.basename(src), img.sectors)]
    lines += ["%d %d %s %s" % e for e in sorted(index)]
    written += write_if_changed(os.path.join(out_dir, "disc.idx"), ("\n".join(lines) + "\n").encode(), False)
    print("extract_disc: %d files, %d unfiled ranges, %d sectors -> %s (%d written)"
          % (len(files), len(ranges), img.sectors, out_dir, written))


if __name__ == "__main__":
    main()
