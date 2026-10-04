#!/usr/bin/env python3
"""Builds a FAT32 SD card image from a folder, for Dolphin's emulated SD card
(Config > Wii > SD Card Path, file sd.raw) when testing Wii/GameCube builds.

Usage: make_sd_image.py <out.raw> <size_mb> <folder>
       (the folder's contents become the card's root: e.g. <folder>/ps1/<disc>.bin)
Files are stored contiguously; long names get VFAT entries.
"""
import os
import struct
import sys
import time

SECTOR = 512
SPC = 8                 # sectors per cluster (4 KB)
CLUSTER = SECTOR * SPC
RESERVED = 32
EOC = 0x0FFFFFFF


def fat_date_time(t):
    lt = time.localtime(t)
    date = ((lt.tm_year - 1980) << 9) | (lt.tm_mon << 5) | lt.tm_mday
    tm = (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec // 2)
    return date, tm


class Image:
    def __init__(self, path, size_mb):
        self.total = size_mb * 1024 * 1024 // SECTOR
        fatsz = 1
        while True:
            clusters = (self.total - RESERVED - 2 * fatsz) // SPC
            need = (clusters + 2) * 4 // SECTOR + 1
            if need <= fatsz:
                break
            fatsz = need
        if clusters < 65525:
            sys.exit("image too small for FAT32 (use at least 260 MB)")
        self.fatsz, self.clusters = fatsz, clusters
        self.data_start = RESERVED + 2 * fatsz
        self.fat = [0] * (clusters + 2)
        self.fat[0], self.fat[1] = 0x0FFFFFF8, EOC
        self.next = 2
        self.f = open(path, "wb+")
        self.f.truncate(self.total * SECTOR)

    def alloc(self, nbytes):
        n = max(1, (nbytes + CLUSTER - 1) // CLUSTER)
        first = self.next
        if first + n > self.clusters + 2:
            sys.exit("image too small")
        for c in range(first, first + n - 1):
            self.fat[c] = c + 1
        self.fat[first + n - 1] = EOC
        self.next += n
        return first

    def write_cluster_data(self, cluster, data):
        self.f.seek((self.data_start + (cluster - 2) * SPC) * SECTOR)
        self.f.write(data)

    def finish(self):
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"MSWIN4.1"
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, SPC, RESERVED, 2, 0, 0, 0xF8, 0, 63, 255, 0, self.total)
        struct.pack_into("<IHHIHH", bs, 36, self.fatsz, 0, 0, 2, 1, 6)
        bs[64], bs[66] = 0x80, 0x29
        struct.pack_into("<I", bs, 67, 0x50533101)
        bs[71:82] = b"PS1RECOMP  "
        bs[82:90] = b"FAT32   "
        bs[510:512] = b"\x55\xAA"
        info = bytearray(SECTOR)
        struct.pack_into("<I", info, 0, 0x41615252)
        struct.pack_into("<III", info, 484, 0x61417272, self.clusters + 2 - self.next, self.next)
        struct.pack_into("<I", info, 508, 0xAA550000)
        for base in (0, 6):
            self.f.seek(base * SECTOR)
            self.f.write(bs)
            self.f.write(info)
        fat = struct.pack("<%dI" % len(self.fat), *self.fat)
        for i in range(2):
            self.f.seek((RESERVED + i * self.fatsz) * SECTOR)
            self.f.write(fat)
        self.f.close()


def short_name(name, used):
    base, ext = os.path.splitext(name.upper())
    ext = ext[1:]
    clean = lambda s: "".join(c for c in s if c.isalnum() or c in "_-~!#$%&'()@^{}")
    b, e = clean(base), clean(ext)[:3]
    if b == base and e == ext and 0 < len(b) <= 8 and (b + "." + e if e else b) == name and name.upper() == name:
        sn = b.ljust(8) + e.ljust(3)
        if sn not in used:
            used.add(sn)
            return sn, False
    for i in range(1, 1000):
        tail = "~%d" % i
        sn = (b[:8 - len(tail)] + tail).ljust(8) + e.ljust(3)
        if sn not in used:
            used.add(sn)
            return sn, True
    sys.exit("too many similar names")


def lfn_entries(name, sn):
    chk = 0
    for c in sn.encode("ascii"):
        chk = (((chk & 1) << 7) + (chk >> 1) + c) & 0xFF
    units = list(name.encode("utf-16-le"))
    chars = [units[i] | (units[i + 1] << 8) for i in range(0, len(units), 2)] + [0]
    while len(chars) % 13:
        chars.append(0xFFFF)
    parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    out = []
    for n, part in reversed(list(enumerate(parts, 1))):
        e = bytearray(32)
        e[0] = n | (0x40 if n == len(parts) else 0)
        struct.pack_into("<5H", e, 1, *part[0:5])
        e[11], e[13] = 0x0F, chk
        struct.pack_into("<6H", e, 14, *part[5:11])
        struct.pack_into("<2H", e, 28, *part[11:13])
        out.append(bytes(e))
    return out


def dir_entry(sn, attr, cluster, size, mtime):
    date, tm = fat_date_time(mtime)
    e = bytearray(32)
    e[0:11] = sn.encode("ascii")
    e[11] = attr
    # create time/date, access date, cluster high, write time/date, cluster low, size
    struct.pack_into("<HHHHHHHI", e, 14, tm, date, date, cluster >> 16, tm, date, cluster & 0xFFFF, size)
    return bytes(e)


def add_dir(img, path, cluster, parent):
    entries = []
    if parent is not None:
        entries.append(dir_entry(".          ", 0x10, cluster, 0, time.time()))
        entries.append(dir_entry("..         ", 0x10, parent if parent != 2 else 0, 0, time.time()))
    used = set()
    children = []
    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        sn, need_lfn = short_name(name, used)
        if need_lfn:
            entries += lfn_entries(name, sn)
        if os.path.isdir(full):
            # directory size is known only after its children: reserve 1 cluster per 128 entries
            count = 2 + sum(1 + (len(n) + 12) // 13 for n in os.listdir(full))
            c = img.alloc(count * 32)
            entries.append(dir_entry(sn, 0x10, c, 0, os.path.getmtime(full)))
            children.append((full, c))
        else:
            size = os.path.getsize(full)
            c = img.alloc(size) if size else 0
            entries.append(dir_entry(sn, 0x20, c, size, os.path.getmtime(full)))
            if size:
                with open(full, "rb") as src:
                    img.f.seek((img.data_start + (c - 2) * SPC) * SECTOR)
                    while True:
                        chunk = src.read(8 << 20)
                        if not chunk:
                            break
                        img.f.write(chunk)
    img.write_cluster_data(cluster, b"".join(entries))
    for full, c in children:
        add_dir(img, full, c, cluster)


def main():
    out, size_mb, folder = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    img = Image(out, size_mb)
    count = 2 + sum(1 + (len(n) + 12) // 13 for n in os.listdir(folder))
    root = img.alloc(count * 32)
    assert root == 2
    add_dir(img, folder, root, None)
    img.finish()
    print("%s: %d MB FAT32, %d clusters used" % (out, size_mb, img.next - 2))


if __name__ == "__main__":
    main()
