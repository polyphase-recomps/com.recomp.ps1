"""The game's data symbols for the recomp runtime's PsyQ libraries (com.recomp.ps1 recomp mode).

The libraries use some of PsyQ's own globals (libgs: GsDRAWENV, POSITION, CLIP2 ...), and
the recompiled game reads and writes them at their addresses in its executable. So, as in a
wasm game build (gen_symbols.py), the libraries' module is linked with PS1 RAM as a 2 MB
placeholder first and every data symbol of the game as a label at its address in it: the
libraries then use the game's copies.

    python make_hle_symbols.py <data_symbols.txt from ps1_syms.py> <out ps1_symbols.s> [--ram-size=N]
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gen_symbols  # noqa: E402


def main():
    src, out = sys.argv[1], sys.argv[2]
    for arg in sys.argv[3:]:
        if arg.startswith("--ram-size="):
            gen_symbols.RAM_SIZE = int(arg.split("=", 1)[1], 0)
    data = {}
    with open(src, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", line)
            if m:
                addr = int(m.group(2), 16)
                if gen_symbols.RAM_BASE <= addr < gen_symbols.RAM_BASE + gen_symbols.RAM_SIZE:
                    data.setdefault(m.group(1), addr)
    gen_symbols.write_wasm_symbols(data, out)
    print(f"make_hle_symbols: {len(data)} data symbols")


main()
