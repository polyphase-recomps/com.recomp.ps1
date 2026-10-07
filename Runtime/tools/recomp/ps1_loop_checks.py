"""Give recompiled code preemption points: a cheap check on every loop's back edge.

A PS1 game may spin in a loop until an interrupt handler (a vblank event) sets what it waits
for; recompiled code is never interrupted, so it would spin forever. This inserts
RECOMP_LOOP_CHECK(<loop address>) before every backward `goto L_xxxxxxxx` of N64Recomp's
output: a counter that, now and then, has the runtime deliver the interrupts that are due
(Runtime/recomp/include/recomp.h, recomp_ps1.c: ps1r_loop_preempt).

    python ps1_loop_checks.py <N64Recomp output folder>

Idempotent: files already processed are left alone.
"""
import glob
import os
import re
import sys

MARK = "/* ps1_loop_checks */\n"
FUNC_RE = re.compile(r"^RECOMP_FUNC ")
LABEL_RE = re.compile(r"^(L_[0-9A-F]{8}):")
GOTO_RE = re.compile(r"^(\s*)goto (L_([0-9A-F]{8}));")


def process(path):
    with open(path, encoding="utf-8") as f:
        lines = f.readlines()
    if lines and lines[0] == MARK:
        return 0
    out, seen, count = [MARK], set(), 0
    for line in lines:
        if FUNC_RE.match(line):
            seen = set()
        m = LABEL_RE.match(line)
        if m:
            seen.add(m.group(1))
        m = GOTO_RE.match(line)
        if m and m.group(2) in seen:
            out.append(f"{m.group(1)}RECOMP_LOOP_CHECK(0x{m.group(3)});\n")
            count += 1
        out.append(line)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(out)
    return count


def main():
    folder = sys.argv[1]
    total = sum(process(p) for p in sorted(glob.glob(os.path.join(folder, "funcs_*.c"))))
    print(f"ps1_loop_checks: {total} loop back edges")


main()
