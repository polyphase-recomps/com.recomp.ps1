#!/usr/bin/env python3
"""Compiler driver for the wasm guest objects: C -> LLVM IR -> (rewritten) -> object.

Between the two steps every global variable gets the alignment the PS1 compiler
(MIPS GCC) gives it, because decomp code depends on the original data layout:
arrays defined one after another are indexed past their end into the next one
(the "matching" source keeps the original definition order). MIPS GCC aligns
arrays, structs and unions to at least a word (DATA_ALIGNMENT / CONSTANT_ALIGNMENT
in gcc/config/mips/mips.h) and other data to its natural alignment; clang's wasm
target instead aligns arrays of 16 bytes or more to 16.

Objects of overlay sources (src/<overlay>/*.c) also get their writable globals into
the data segment ovNN (1-based position in --overlays), which wasm-ld brackets with
__start_ovNN/__stop_ovNN for the overlay data reset (port/guest/overlays.c).

Usage: wasm_cc.py --overlays=btl,std,... <clang> <compile args...> -c <src> -o <obj>
       (-MD -MF <dep> pass through to the first step)
"""
import os
import re
import subprocess
import sys

# natural alignment of the scalar types in the wasm32 data layout
SCALAR_ALIGN = {"i1": 1, "i8": 1, "i16": 2, "i32": 4, "i64": 8, "i128": 16, "ptr": 4,
                "half": 2, "float": 4, "double": 8, "fp128": 16}


class TypeParser:
    def __init__(self, named):
        self.named = named  # "%struct.X" -> type text
        self.cache = {}

    def parse(self, s, i):
        """Parses the type at s[i:]; returns (natural alignment, aggregate?, end)."""
        while s[i] == " ":
            i += 1
        if s.startswith("<{", i):  # packed struct
            _, _, end = self.parse_fields(s, i + 2, "}>")
            return 1, True, end
        if s[i] == "{":
            align, _, end = self.parse_fields(s, i + 1, "}")
            return align, True, end
        if s[i] == "[":
            m = re.match(r"\[(\d+) x ", s[i:])
            align, _, end = self.parse(s, i + m.end())
            assert s[end] == "]", s[i:end + 10]
            return align, True, end + 1
        if s[i] == "<":
            m = re.match(r"<(\d+) x ", s[i:])
            align, _, end = self.parse(s, i + m.end())
            return 16, True, end + 1
        if s[i] == "%":
            m = re.match(r'%("[^"]+"|[\w.$-]+)', s[i:])
            name = m.group(0)
            if name not in self.cache:
                text = self.named[name]
                if text == "opaque":
                    self.cache[name] = (1, True)
                else:
                    a, agg, _ = self.parse(text, 0)
                    self.cache[name] = (a, agg)
            a, agg = self.cache[name]
            return a, agg, i + m.end()
        m = re.match(r"[\w]+", s[i:])
        return SCALAR_ALIGN.get(m.group(0), 4), False, i + m.end()

    def parse_fields(self, s, i, close):
        align = 1
        while True:
            while s[i] == " ":
                i += 1
            if s.startswith(close, i):
                return align, True, i + len(close)
            a, _, i = self.parse(s, i)
            align = max(align, a)
            while s[i] == " ":
                i += 1
            if s[i] == ",":
                i += 1


GLOBAL_RE = re.compile(r'^(@[^ ]+) = ((?:[a-z_]+(?:\([^)]*\))? )*)(global|constant) ')


def rewrite_ir(text, section):
    named = {}
    for m in re.finditer(r'^(%[^ ]+) = type (.*)$', text, re.M):
        named[m.group(1)] = m.group(2)
    types = TypeParser(named)
    out = []
    for line in text.split("\n"):
        m = GLOBAL_RE.match(line)
        if m and not re.search(r"\bexternal\b", m.group(2)) and ", align " in line:
            natural, aggregate, _ = types.parse(line, m.end())
            mips = max(natural, 4) if aggregate else natural
            cur = int(re.search(r", align (\d+)", line).group(1))
            # 16 on a type with smaller natural alignment is clang's large-array rule
            new = mips if (cur == 16 and natural < 16) else max(cur, mips)
            line = re.sub(r", align \d+", ", align %d" % new, line, count=1)
            if section and m.group(3) == "global" and ", section " not in line:
                line = line.replace(", align ", ', section "%s", align ' % section, 1)
        out.append(line)
    return "\n".join(out)


def overlay_index(src, overlays):
    m = re.search(r"/src/([a-z0-9]+)/[^/]+\.c$", src.replace("\\", "/"))
    if m and m.group(1) in overlays:
        return overlays.index(m.group(1)) + 1
    return 0


def main():
    args = sys.argv[1:]
    overlays = []
    if args and args[0].startswith("--overlays="):
        overlays = [o for o in args[0].split("=", 1)[1].split(",") if o]
        args = args[1:]
    cc, args = args[0], args[1:]
    src = args[args.index("-c") + 1]
    obj = args[args.index("-o") + 1]
    index = overlay_index(src, overlays)
    ll = obj + ".ll"

    # 1. C -> IR, unoptimised (the rewrite happens before the optimiser runs)
    first = [a if a != obj else ll for a in args if a != "-c"] + ["-S", "-emit-llvm", "-Xclang", "-disable-llvm-passes"]
    r = subprocess.call([cc] + first)
    if r != 0:
        return r
    with open(ll, encoding="utf-8", errors="surrogateescape") as f:
        text = f.read()
    text = rewrite_ir(text, "ov%02d" % index if index else None)
    with open(ll, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as f:
        f.write(text)

    # 2. IR -> object with the target and optimisation options
    keep = []
    skip = False
    for a in args:
        if skip:
            skip = False
            keep.append(a)
            continue
        if a.startswith("--target=") or a.startswith("-O") or (a.startswith("-m") and a != "-mllvm"):
            keep.append(a)
        elif a == "-mllvm":
            keep.append(a)
            skip = True
    r = subprocess.call([cc] + keep + ["-Wno-everything", "-c", ll, "-o", obj])
    if r == 0:
        os.remove(ll)
    return r


if __name__ == "__main__":
    sys.exit(main())
