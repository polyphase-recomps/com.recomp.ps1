#!/usr/bin/env python3
"""Builds a disc image a GameCube build reads its game files from, when there is no SD
card (port/host/ogc/host_ogc.c mounts the drive's ISO9660 file system as dvd:/).

    make_gc_iso.py <out.iso> <folder> [--id GPSX01] [--name "PS1 game files"]

The folder's contents become the disc's root, laid out as on the SD card: e.g.
<folder>/ps1/<game title>/disc.idx + files (tools/extract_disc.py). The image has a
GameCube disc header (so Dolphin and loaders take it as a GameCube disc), a file table
the GameCube host reads (port/host/ogc/host_ogc.c, device gcdisc:) and, for PC tools,
the same files as ISO9660. Layout:

  sector 0      GameCube disc header (game id, magic 0xC2339F3D, name)
  sector 8      "PS1FILES", table sector, table bytes, file count (big-endian u32s)
  16..          ISO9660 descriptors, path tables, directories
  ..            the files, each from a sector boundary
  last          the table: per file u32 sector, u32 size, u16 name length, the path
                relative to the folder ('/' separated)

It boots nothing: start the game's .dol (Dolphin: Config > Paths > Default ISO = this
image, then open the .dol; on a console, a loader that leaves the disc in the drive).
"""
import os
import struct
import sys
import time

SECTOR = 2048


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def dir_date(t):
    lt = time.gmtime(t)
    return bytes([lt.tm_year - 1900, lt.tm_mon, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, 0])


def vol_date(t):
    return time.strftime("%Y%m%d%H%M%S", time.gmtime(t)).encode() + b"00" + b"\0"


class Node:
    def __init__(self, name, path, is_dir, parent=None):
        self.name, self.path, self.is_dir, self.parent = name, path, is_dir, parent
        self.children = []
        self.lba = 0
        self.size = 0
        self.number = 0  # path table number (directories)


def scan(path, name, parent):
    node = Node(name, path, True, parent)
    for entry in sorted(os.listdir(path), key=lambda n: n.upper()):
        full = os.path.join(path, entry)
        if os.path.isdir(full):
            node.children.append(scan(full, entry, node))
        else:
            child = Node(entry, full, False, node)
            child.size = os.path.getsize(full)
            node.children.append(child)
    return node


def record(node_lba, size, is_dir, name_bytes, mtime):
    rec_len = 33 + len(name_bytes)
    rec_len += rec_len & 1
    rec = bytes([rec_len, 0]) + both32(node_lba) + both32(size) + dir_date(mtime)
    rec += bytes([2 if is_dir else 0, 0, 0]) + both16(1) + bytes([len(name_bytes)]) + name_bytes
    return rec + b"\0" * (rec_len - len(rec))


def dir_records(d, mtime):
    recs = [record(d.lba, d.size, True, b"\0", mtime), record((d.parent or d).lba, (d.parent or d).size, True, b"\1", mtime)]
    for c in d.children:
        recs.append(record(c.lba, c.size, c.is_dir, c.name.encode("latin-1"), mtime))
    return recs


def dir_extent_size(recs):
    used, sectors = 0, 1
    for r in recs:
        if used + len(r) > SECTOR:
            sectors += 1
            used = 0
        used += len(r)
    return sectors * SECTOR


def pack_dir(recs, size):
    out, cur = b"", b""
    for r in recs:
        if len(cur) + len(r) > SECTOR:
            out += cur + b"\0" * (SECTOR - len(cur))
            cur = b""
        cur += r
    out += cur + b"\0" * (SECTOR - len(cur))
    return out + b"\0" * (size - len(out))


def main():
    args = sys.argv[1:]
    game_id, name = "GPSX01", "PS1 game files"
    if "--id" in args:
        i = args.index("--id")
        game_id = args[i + 1]
        del args[i:i + 2]
    if "--name" in args:
        i = args.index("--name")
        name = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    out_path, folder = args
    mtime = time.time()

    root = scan(folder, "", None)
    dirs = []
    queue = [root]
    while queue:  # breadth first: the path table's order
        d = queue.pop(0)
        dirs.append(d)
        queue.extend(c for c in d.children if c.is_dir)
    for i, d in enumerate(dirs):
        d.number = i + 1

    # sectors: 0-15 system area (GameCube header), 16 PVD, 17 terminator, path tables, dirs, files
    def path_table(big):
        fmt = ">" if big else "<"
        out = b""
        for d in dirs:
            ident = d.name.encode("latin-1") if d.parent else b"\0"
            out += bytes([len(ident), 0]) + struct.pack(fmt + "I", d.lba) + struct.pack(fmt + "H", (d.parent or d).number)
            out += ident + (b"\0" if len(ident) & 1 else b"")
        return out

    pt_size = len(path_table(False))  # lbas don't change the size
    pt_sectors = (pt_size + SECTOR - 1) // SECTOR
    lba = 18 + 2 * pt_sectors
    for d in dirs:  # directory sizes depend only on names
        d.size = dir_extent_size(dir_records(d, mtime))
        d.lba = lba
        lba += d.size // SECTOR
    files = []
    stack = [root]
    while stack:
        d = stack.pop()
        for c in d.children:
            if c.is_dir:
                stack.append(c)
            else:
                c.lba = lba
                lba += max(1, (c.size + SECTOR - 1) // SECTOR)
                files.append(c)
    # the GameCube host's file table, after the files
    table = b""
    for c in files:
        rel = os.path.relpath(c.path, folder).replace(os.sep, "/").encode("latin-1")
        table += struct.pack(">IIH", c.lba, c.size, len(rel)) + rel
    table_lba = lba
    lba += (len(table) + SECTOR - 1) // SECTOR
    total = lba

    with open(out_path, "wb") as f:
        hdr = bytearray(SECTOR * 16)
        hdr[0:6] = game_id.encode()[:6].ljust(6, b"0")
        struct.pack_into(">I", hdr, 0x1C, 0xC2339F3D)
        hdr[0x20:0x20 + 64] = name.encode()[:63].ljust(64, b"\0")
        hdr[8 * SECTOR:8 * SECTOR + 20] = b"PS1FILES" + struct.pack(">III", table_lba, len(table), len(files))
        f.write(hdr)

        pvd = bytearray(SECTOR)
        pvd[0:7] = b"\x01CD001\x01"
        pvd[8:40] = b"GAMECUBE".ljust(32)
        pvd[40:72] = b"PS1".ljust(32)
        pvd[80:88] = both32(total)
        pvd[120:124] = both16(1)
        pvd[124:128] = both16(1)
        pvd[128:132] = both16(SECTOR)
        pvd[132:140] = both32(pt_size)
        struct.pack_into("<I", pvd, 140, 18)
        struct.pack_into(">I", pvd, 148, 18 + pt_sectors)
        pvd[156:190] = record(root.lba, root.size, True, b"\0", mtime)
        pvd[190:318] = b" " * 128
        pvd[318:446] = b" " * 128
        pvd[446:574] = b" " * 128
        pvd[574:702] = b"make_gc_iso.py".ljust(128)
        pvd[702:813] = b" " * 111
        for off in (813, 830):
            pvd[off:off + 17] = vol_date(mtime)
        for off in (847, 864):
            pvd[off:off + 17] = b"0" * 16 + b"\0"
        pvd[881] = 1
        f.write(pvd)
        f.write(b"\xffCD001\x01" + b"\0" * (SECTOR - 7))
        for big in (False, True):
            t = path_table(big)
            f.write(t + b"\0" * (pt_sectors * SECTOR - len(t)))
        for d in dirs:
            f.write(pack_dir(dir_records(d, mtime), d.size))
        for c in files:
            assert f.tell() == c.lba * SECTOR
            with open(c.path, "rb") as src:
                while True:
                    block = src.read(1 << 20)
                    if not block:
                        break
                    f.write(block)
            pad = (-c.size) % SECTOR or (SECTOR if c.size == 0 else 0)
            f.write(b"\0" * pad)
        assert f.tell() == table_lba * SECTOR
        f.write(table + b"\0" * ((-len(table)) % SECTOR))
    print(f"{out_path}: {len(files)} files in {len(dirs)} folders, {total * SECTOR // (1 << 20)} MB")


if __name__ == "__main__":
    main()
