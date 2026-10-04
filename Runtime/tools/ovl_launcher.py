"""Compiler launcher for the game objects of the native build (wasm: wasm_cc.py).

Overlay objects (src/<overlay>/*.c) are compiled through assembly so that all their
writable data lands in the overlay's section group (.ovNN$d), between the .ovNN$a and
.ovNN$z markers. clang's '#pragma clang section' does not name sections on COFF and
llvm-objcopy cannot rename COFF sections, hence the detour.
Usage: ovl_launcher.py --overlays=btl,std,... <compiler> <args...>
(the overlay list order gives the section index, 1-based, matching ovl_markers.c)
"""
import os
import re
import subprocess
import sys

OVERLAYS = []


def overlay_of(src):
    m = re.search(r"/src/([a-z0-9]+)/[^/]+\.c$", src.replace(chr(92), "/"))
    if m and m.group(1) in OVERLAYS:
        return OVERLAYS.index(m.group(1)) + 1
    return 0


def main():
    cmd = sys.argv[1:]
    if cmd and cmd[0].startswith("--overlays="):
        OVERLAYS.extend(o for o in cmd[0].split("=", 1)[1].split(",") if o)
        cmd = cmd[1:]
    src = obj = None
    for i, arg in enumerate(cmd):
        if arg == "-c" and i + 1 < len(cmd):
            src = cmd[i + 1]
        elif arg == "-o" and i + 1 < len(cmd):
            obj = cmd[i + 1]
    index = overlay_of(src) if src and obj else 0
    if not index:
        return subprocess.call(cmd)

    # Overlay object: compile to assembly with all zero-initialised data in .data, move
    # .data into the overlay's group, assemble.
    asm = obj + ".s"
    compile_cmd = [asm if a == obj else a for a in cmd] + ["-S", "-fno-zero-initialized-in-bss"]
    result = subprocess.call(compile_cmd)
    if result != 0:
        return result
    section = "\t.section\t.ov%02d$d,\"dw\"\n" % index
    with open(asm, "r", encoding="utf-8", errors="surrogateescape") as f:
        lines = [section if line.strip() == ".data" else line for line in f]
    with open(asm, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        f.writelines(lines)
    result = subprocess.call([cmd[0], "--target=i686-pc-windows-gnu", "-c", asm, "-o", obj])
    if result == 0:
        os.remove(asm)
    return result


if __name__ == "__main__":
    sys.exit(main())
