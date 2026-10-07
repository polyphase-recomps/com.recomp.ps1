"""Function starts in PS1 code nobody has named (com.recomp.ps1 recomp mode, ps1_syms.py --scan).

A partly decompiled game names only some of its functions, and an overlay no decomp has split
names none. N64Recomp needs every function's start (it splits off the static functions it
reaches by jal itself). This finds them by walking the code from what is known:

  - seeds: named functions, jal targets from any file, an overlay's entry table, words in the
    file that point at code;
  - each function is walked along its branches until its returns ("jr ra"; "jr" of another
    register is a jump table: the walk stops there and the cases stay in the function);
  - code nobody reached: a stack frame set up ("addiu sp, sp, -N") after the end of a walked
    function, or a pointer to it in data, starts another function - if walking it gives
    valid instructions up to a return.

Sizes are then "up to the next start", as ps1_syms.py does for named functions; the code
ends where the last walk ends (an overlay's data follows its code).
"""
import struct

JR_RA = 0x03E00008
NOP = 0

# R3000 encodings (cop0/cop2 as the PS1 has them); anything else is data
VALID_OPS = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 32, 33, 34, 35, 36, 37, 38,
             40, 41, 42, 43, 46, 50, 58}
VALID_FUNCT = {0, 2, 3, 4, 6, 7, 8, 9, 12, 13, 16, 17, 18, 19, 24, 25, 26, 27, 32, 33, 34, 35, 36, 37,
               38, 39, 42, 43}
VALID_REGIMM = {0, 1, 16, 17}


def valid(w):
    op = w >> 26
    if op not in VALID_OPS:
        return False
    if w == 0:
        return True  # nop
    # (code never writes $zero but as a nop)
    if op in (8, 9, 10, 11, 12, 13, 14, 15, 32, 33, 34, 35, 36, 37, 38) and (w >> 16) & 31 == 0:
        return False
    # lb from $zero + offset: a pointer into RAM (0x80000000-0x801FFFFF), not code (which reads a
    # byte at an absolute low address no other way)
    if op == 32 and (w >> 21) & 31 == 0:
        return False
    if op == 0:
        f = w & 63
        if f not in VALID_FUNCT:
            return False
        rs, rt, rd, sa = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        if rd == 0 and f not in (8, 12, 13, 17, 19, 24, 25, 26, 27):  # (writes $zero)
            return False
        # the fields each one leaves zero (data seldom does)
        if f in (0, 2, 3):              # sll srl sra
            return rs == 0
        if f in (4, 6, 7):              # sllv srlv srav
            return sa == 0
        if f == 8:                      # jr
            return rt == 0 and rd == 0 and sa == 0 and rs != 0
        if f == 9:                      # jalr
            return rt == 0 and sa == 0 and rs != 0
        if f in (12, 13):               # syscall break
            return True
        if f in (16, 18):               # mfhi mflo
            return rs == 0 and rt == 0 and sa == 0
        if f in (17, 19):               # mthi mtlo
            return rt == 0 and rd == 0 and sa == 0
        if f in (24, 25, 26, 27):       # mult multu div divu
            return rd == 0 and sa == 0
        return sa == 0                  # add addu sub subu and or xor nor slt sltu
    if op == 1:
        return ((w >> 16) & 31) in VALID_REGIMM
    if op == 16:
        rs = (w >> 21) & 31
        return rs in (0, 4) or w == 0x42000010  # mfc0 / mtc0 / rfe
    return True


def is_prologue(w):
    return (w >> 16) == 0x27BD and (w & 0x8000) != 0


def branch_target(w, pc):
    op = w >> 26
    if op in (4, 5, 6, 7) or op == 1 or (op == 18 and (w >> 21) & 31 == 8) or (op == 16 and (w >> 21) & 31 == 8):
        imm = w & 0xFFFF
        if imm & 0x8000:
            imm -= 0x10000
        return pc + 4 + imm * 4
    return None


class Code:
    def __init__(self, data, base, lo, hi):
        self.base, self.lo, self.hi = base, lo, hi
        n = len(data) // 4
        self.words = struct.unpack(f"<{n}I", data[:n * 4])

    def word(self, pc):
        return self.words[(pc - self.base) // 4]

    def inside(self, pc):
        return self.lo <= pc < self.hi and pc & 3 == 0

    def table_entries(self, table):
        """The cases of a jump table at `table` (in the file's data): code addresses in a row."""
        out = []
        off = table - self.base
        while 0 <= off < len(self.words) * 4 and len(out) < 1024:
            t = self.words[off // 4]
            if not self.inside(t) or not valid(self.word(t)):
                break
            out.append(t)
            off += 4
        return out

    def walk(self, entry, starts):
        """(addresses of the function's instructions, jal targets, whether it has a jump table it
        couldn't read, addresses the code builds into registers) or None if it runs into data."""
        seen, calls, refs, tails = set(), set(), set(), set()
        table = False
        todo = [(entry, {}, {})]
        while todo:
            pc, hi16, loaded = todo.pop()
            hi16, loaded = dict(hi16), dict(loaded)
            while True:
                if pc in seen:
                    break
                if not self.inside(pc):
                    return None
                w = self.word(pc)
                if not valid(w):
                    return None
                seen.add(pc)
                op = w >> 26
                rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
                imm = w & 0xFFFF
                simm = imm - 0x10000 if imm & 0x8000 else imm
                # addresses built in registers: lui + addiu/ori (a function pointer, a jump
                # table's base) and what lw then loads from a table
                if op == 15:  # lui
                    hi16[rt] = imm << 16
                    loaded.pop(rt, None)
                elif op in (9, 13) and rs in hi16:  # addiu / ori
                    value = (hi16[rs] + simm) & 0xFFFFFFFF if op == 9 else hi16[rs] | imm
                    hi16[rt] = value
                    loaded.pop(rt, None)
                    if self.inside(value):
                        refs.add(value)
                elif op == 0 and (w & 63) == 33 and rs in hi16:  # addu: base + index
                    hi16[rd] = hi16[rs]
                    loaded.pop(rd, None)
                elif op == 35 and rs in hi16:  # lw from a table
                    loaded[rt] = (hi16[rs] + simm) & 0xFFFFFFFF
                    hi16.pop(rt, None)
                elif op in (8, 9, 10, 11, 12, 13, 14, 32, 33, 34, 35, 36, 37, 38):
                    hi16.pop(rt, None)
                    loaded.pop(rt, None)
                elif op == 0:
                    hi16.pop(rd, None)
                    loaded.pop(rd, None)
                if op == 0 and (w & 63) == 13:  # break: the end (PsyQ's start code ends so)
                    break
                if op == 0 and (w & 63) == 8:  # jr: a return, or a jump table
                    if self.inside(pc + 4) and valid(self.word(pc + 4)):
                        seen.add(pc + 4)
                    if rs != 31:
                        cases = self.table_entries(loaded[rs]) if rs in loaded else []
                        if cases:
                            todo.extend((c, {}, {}) for c in cases)
                        else:
                            table = True
                    break
                if op in (2, 3):
                    t = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                    if not (0x80010000 <= t < 0x80200000):  # (game code is above the kernel: this is data)
                        return None
                if op == 3:  # jal
                    calls.add((pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
                elif op == 2:  # j: a tail call to a known function, else a jump in this one
                    t = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                    if self.inside(pc + 4):
                        seen.add(pc + 4)
                    if self.inside(t) and t != entry and (t in starts or is_prologue(self.word(t))):
                        tails.add(t)  # (a function of its own: it sets up a frame)
                    elif self.inside(t):
                        todo.append((t, hi16, loaded))
                    break
                t = branch_target(w, pc)
                if t is not None:
                    if self.inside(t):
                        todo.append((t, hi16, loaded))
                    else:  # a branch out of the code: this is data
                        return None
                pc += 4
        # (a tail call's target is a function too, but a jump is no proof of one: refs)
        return seen, calls, table, refs | tails


_codes = {}


def _code(data, base, lo, hi):
    key = (id(data), base, lo, hi)
    code = _codes.get(key)
    if code is None or code.data is not data:
        code = Code(data, base, lo, hi)
        code.data = data
        _codes[key] = code
    return code


def escapes(code, entry, end, known):
    """Branch / jump targets of the function at entry, as N64Recomp sees it (up to `end`), that
    leave it without being a known function's start (tail calls are fine)."""
    out, seen, todo = set(), set(), [entry]
    while todo:
        pc = todo.pop()
        while pc not in seen:
            if not entry <= pc < end:
                out.add(pc)  # (it runs on into the next one)
                break
            seen.add(pc)
            w = code.word(pc)
            op = w >> 26
            if op == 0 and (w & 63) in (8, 13):  # jr / break
                break
            if op == 2:
                t = (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if entry <= t < end:
                    todo.append(t)
                elif t not in known:
                    out.add(t)
                break
            t = branch_target(w, pc)
            if t is not None:
                if entry <= t < end:
                    todo.append(t)
                else:
                    out.add(t)
            pc += 4
    return out


def plausible_start(data, base, lo, hi, pc, code=None):
    """Whether a call from elsewhere (an address several overlays share), or a pointer, can start
    a function here: right after another one's end, or after data (a table, then its function)."""
    code = code or _code(data, base, lo, hi)
    if not code.inside(pc) or not valid(code.word(pc)):
        return False
    # (a stack frame set up is not enough: some functions do one thing before it)
    return after_end(code, pc) or after_data(code, pc)


def after_data(code, pc):
    """Whether what comes before pc is data, not code that could run into it: a word that isn't an
    instruction, or a pointer into RAM (as an instruction, a load from $zero + offset that game
    code never does) - a table, its function right after it."""
    if pc - 4 < code.lo:
        return False
    w = code.word(pc - 4)
    return not valid(w) or 0x80000000 <= w < 0x80800000


def after_end(code, pc):
    """Whether pc is the first instruction after a function's end (a return or a jump away,
    its delay slot, padding)."""
    lo = code.lo
    prev = pc - 4
    while prev >= lo and code.word(prev) == NOP:
        prev -= 4
    if prev < lo:
        return False

    def ends(w):  # a return, or a jump away (a tail call); not a jump table's jr (its cases follow)
        return w == JR_RA or w >> 26 == 2

    if ends(code.word(prev)):
        return pc - prev >= 8  # its delay slot (a nop) lies between: pc is not that slot
    return prev - 4 >= lo and ends(code.word(prev - 4))  # prev is its delay slot


def discover(data, base, lo, hi, seeds, extra_pointer_words=(), weak_seeds=(), named=()):
    """Function starts in [lo, hi) of a file loaded at `base`, the end of its code, and the jal
    targets its code has outside it. Weak seeds (calls from other files, which may be into
    another overlay at the same address) are only taken after the file's own walks, where
    those didn't reach."""
    import bisect
    code = _code(data, base, lo, hi)
    starts, covered, outside = set(), set(), set()
    pending = sorted(s for s in seeds if code.inside(s))
    rejected = set()
    tables = set()  # functions that end in a jump table (their cases are pointed to from data)
    built = set()   # code addresses the code builds in registers (callbacks)
    walked = {}     # function -> the instructions its walks reached
    weakly = set()  # starts only data, built addresses or other files' calls suggested
    strong = set(s for s in seeds if code.inside(s))  # named, or called from this file's code
    named_set = set(named)

    def take(entry, part_of=None):
        res = code.walk(entry, starts)
        if res is None:
            rejected.add(entry)
            return False
        seen, calls, table, refs = res
        built.update(refs)
        owner = part_of if part_of is not None else entry
        walked.setdefault(owner, set()).update(seen)
        # calls from code that surely is code (named, called, a frame set up) start functions;
        # calls from what only data or a gap suggested may be data walked as code
        credible = owner not in weakly and (owner in strong or is_prologue(code.word(owner)))
        for c in calls:
            if code.inside(c) and credible:
                strong.add(c)
        if part_of is None:
            starts.add(entry)
            if table:
                tables.add(entry)
        elif table:
            tables.add(part_of)
        covered.update(seen)
        for c in calls:
            if code.inside(c):
                if c not in starts and c not in rejected:
                    pending.append(c)
            else:
                outside.add(c)
        return True

    # words (of this file and the others) that could point at this code
    pointers = sorted({w for w in code.words if code.inside(w)} | {w for w in extra_pointer_words if code.inside(w)})
    prologues = [pc for pc in range(lo, hi, 4) if is_prologue(code.word(pc))]
    weak = sorted(set(s for s in weak_seeds if code.inside(s)))
    while True:
        while pending:
            e = pending.pop()
            if e not in starts and e not in rejected:
                take(e)
        found = []
        # a stack frame set up where no walk went, right after one that ended
        for pc in prologues:
            if pc in covered or pc in starts or pc in rejected:
                continue
            prev = pc - 4
            while prev >= lo and code.word(prev) == NOP and prev not in covered:
                prev -= 4
            if prev < lo or prev in covered or after_end(code, pc):
                found.append(pc)
        # code that data points to: a jump table's case in the function before it (walked as part
        # of it), else a function (function tables)
        ordered = sorted(starts)
        cases = []
        for w in pointers:
            if w in covered or w in starts or w in rejected:
                continue
            i = bisect.bisect_right(ordered, w) - 1
            owner = ordered[i] if i >= 0 else None
            if owner is not None and owner in tables and not is_prologue(code.word(w)):
                cases.append((w, owner))
                continue
            if plausible_start(data, base, lo, hi, w, code):
                found.append(w)
                if not is_prologue(code.word(w)):
                    weakly.add(w)
        # code nobody refers to, right after a function its walk fully covered (no jump table
        # it couldn't read: then that code could be its cases): another function
        ordered = sorted(starts)
        for idx, st in enumerate(ordered):
            if st in tables or st not in walked:
                continue
            nxt = ordered[idx + 1] if idx + 1 < len(ordered) else hi
            pc = max(walked[st]) + 4
            while pc < nxt and code.word(pc) == NOP:
                pc += 4
            if pc < nxt and pc not in covered and pc not in rejected and valid(code.word(pc)) and after_end(code, pc):
                found.append(pc)
                if not is_prologue(code.word(pc)):
                    weakly.add(pc)
        # code addresses the code builds (a callback passed on): where a function can start
        for w in built:
            if w not in covered and w not in starts and w not in rejected and plausible_start(data, base, lo, hi, w, code):
                found.append(w)
                if not is_prologue(code.word(w)):
                    weakly.add(w)
        progressed = False
        for w, owner in cases:
            if w not in covered and w not in rejected:
                progressed = take(w, part_of=owner) or progressed
        found = [f for f in found if f not in starts and f not in rejected]
        if not found:
            if progressed:
                continue
            # everything this file shows itself is found: now the calls from elsewhere, where
            # none of that reached
            if weak:
                late = [w for w in weak if w not in covered and w not in starts]
                weakly.update(w for w in late if not is_prologue(code.word(w)))
                pending.extend(late)
                weak = []
                continue
            break
        pending.extend(sorted(set(found)))
    # N64Recomp's rule: a function's branches stay inside it (up to the next start). A start a
    # function's code runs past (with every start a function, so a jump to one is a tail call)
    # is that function's code - a switch case, a label - unless it is named or called.
    keep = sorted(starts)
    code_end_ = max(covered) + 4 if covered else lo
    while True:
        known = set(keep)
        drop = set()
        for idx, st in enumerate(keep):
            nxt = keep[idx + 1] if idx + 1 < len(keep) else code_end_
            past = [a for a in escapes(code, st, nxt, known) if nxt <= a < hi]
            if past:
                top = max(past)
                # (a stack frame set up never sits inside a function; calls go to a function's
                # start, so a "call" into one's middle came from data walked as code)
                # (a call into one's middle - overlays sharing addresses call each other's
                # functions - is looked up when it runs: N64Recomp, PS1 mode)
                drop.update(n for n in keep[idx + 1:] if n <= top and n not in named_set
                            and not is_prologue(code.word(n)))
        if not drop:
            break
        keep = [st for st in keep if st not in drop]
    code_end = max(covered) + 4 if covered else lo
    # where each function's walked code ends: data may follow it before the next start
    ends = {}
    for idx, st in enumerate(keep):
        res = code.walk(st, set(keep))
        if res is None:
            continue
        last = max(res[0]) + 4
        nxt = keep[idx + 1] if idx + 1 < len(keep) else hi
        if any(not valid(code.word(a)) for a in range(last, min(nxt, hi), 4)):
            ends[st] = last
    return keep, code_end, outside, ends
