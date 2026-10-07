"""Mods for a recompiled PS1 game (com.recomp.ps1 Recomp mode; Docs/Recomp.md, Mods).

A game package's Recomp/mods.toml lists mod code (C, compiled into the runtime's library
module "ps1hle" with the game's headers: it sees the game's globals at their addresses) and
where the game calls it (hooks on the recompiled code):

    sources = ["../Native/game/bridge.c", "mods.c"]   # relative to the toml

    [[hook]]
    func = "gameLoop"          # the game function (a name in syms.toml)
    at = 0x800EFB70            # before this instruction (not a delay slot)
    call = "dw_bridge_tick"    # a mod function
    args = ["a0", "v0"]        # registers passed to it (as 32-bit integers), in order
    result = "v0"              # register set to what it returns

    [[hook]]
    func = "damageTick"
    entry = true               # at the start of the function, before anything of it runs
    call = "dwr_damage_tick"
    args = ["a0", "a1"]
    return_if = "nonzero"      # entry only: the game function returns right away when the
                               # call gives nonzero ("nonnegative": >= 0), with `result` set

    [[hook]]
    func = "BTL_calculateDamage"
    after = true               # when the function has returned: args name its registers at
    call = "dw_cheat_damage"   # entry (a0-a3), "v0" / "v1" what it returned
    args = ["a0", "a1", "v0"]
    result = "v0"

Hooks at one place run in the order listed. Mod code calls game functions by name (the
symbols): those calls become imports of the module that run the recompiled function.

    ps1_mods.py sources <mods.toml>                       the mod sources (absolute), one per line
    ps1_mods.py imports <syms.toml> <imports.txt> <out.txt>  the runtime's imports (imports.txt)
                                                          and the game functions mod code may
                                                          call (wasm-ld --allow-undefined-file)
    ps1_mods.py hooks <mods.toml> <syms.toml> <out.toml>  [[patches.hook]] for N64Recomp
    ps1_mods.py glue <mods.toml> <syms.toml> <ps1hle_guest.h> <out dir>
        ps1_mod_hooks.c (the hook functions, their table), ps1_mod_hooks.h (declarations the
        generated code is compiled with), ps1_game_calls.c (mod code -> game functions)
A game without mods.toml gets the same files, empty.
"""
import os
import re
import sys

try:
    import tomllib
except ImportError:  # Python < 3.11
    tomllib = None

REGS = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
EXPORT_RE = re.compile(r"^(u32|u64|void|s32|s64|f32|f64)\s+w2c_ps1hle_(\w+)\(w2c_ps1hle\*(?:\s*\w*)?(?:,\s*([^)]*))?\);")
IMPORT_RE = re.compile(r"^(u32|u64|void|s32|s64|f32|f64)\s+w2c_env_(\w+)\(struct w2c_env\*(?:\s*\w*)?(?:,\s*([^)]*))?\);")


def fail(msg):
    sys.exit(f"ps1_mods: {msg}")


def load_toml(path):
    if tomllib is None:
        fail("Python 3.11 or newer is needed (tomllib)")
    with open(path, "rb") as f:
        return tomllib.load(f)


def reg(name, where):
    name = name.lower()
    if name == "s8":
        name = "fp"
    if name not in REGS:
        fail(f"{where}: unknown register '{name}'")
    return REGS.index(name)


def load_functions(syms_path):
    """name -> (vram, size) of every game function (PsyQ ones excluded)."""
    funcs = {}
    for line in open(syms_path, encoding="utf-8"):
        m = re.search(r'\{ name = "([^"]+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+)', line)
        if m and int(m.group(3), 16) > 0:
            funcs.setdefault(m.group(1), (int(m.group(2), 16), int(m.group(3), 16)))
    return funcs


def load_mods(path):
    if not path or not os.path.exists(path):
        return {"sources": [], "hook": []}, ""
    data = load_toml(path)
    data.setdefault("sources", [])
    data.setdefault("hook", [])
    return data, os.path.dirname(os.path.abspath(path))


def sites(mods, funcs):
    """[(func, at or None, [hooks])] in file order; at None = the function's entry."""
    order, by_key = [], {}
    for i, h in enumerate(mods["hook"]):
        where = f"hook {i + 1} ({h.get('call', '?')})"
        for key in ("func", "call"):
            if key not in h:
                fail(f"{where}: no '{key}'")
        if h["func"] not in funcs:
            fail(f"{where}: no game function '{h['func']}' in the symbols")
        vram, size = funcs[h["func"]]
        kinds = [k for k in ("at", "entry", "after") if k in h and h[k] is not False]
        if len(kinds) != 1:
            fail(f"{where}: needs one of at = <address>, entry = true, after = true")
        if "at" in h:
            at = h["at"]
            if at & 3 or not (vram <= at < vram + size):
                fail(f"{where}: 0x{at:08X} is not an instruction of {h['func']} ({vram:08X}+{size:X})")
            if at == vram:
                at = None
        else:
            at = None
        if "return_if" in h and not h.get("entry"):
            fail(f"{where}: return_if is for entry hooks")
        if h.get("return_if", "nonzero") not in ("nonzero", "nonnegative"):
            fail(f"{where}: return_if is 'nonzero' or 'nonnegative'")
        for r in h.get("args", []):
            reg(r, where)
        if "result" in h:
            reg(h["result"], where)
        key = (h["func"], at)
        if key not in by_key:
            by_key[key] = []
            order.append(key)
        by_key[key].append(h)
    return [(f, at, by_key[(f, at)]) for f, at in order]


def can_return(hooks):
    return any(h.get("entry") and "return_if" in h or h.get("after") for h in hooks)


def hook_text(name, hooks):
    return f"if ({name}(rdram, ctx)) return;" if can_return(hooks) else f"{name}(rdram, ctx);"


def cmd_sources(args):
    mods, base = load_mods(args[0])
    for s in mods["sources"]:
        print(os.path.normpath(os.path.join(base, s)).replace("\\", "/"))


def cmd_imports(args):
    funcs = load_functions(args[0])
    base = [line.strip() for line in open(args[1], encoding="utf-8") if line.strip()]
    with open(args[2], "w", encoding="utf-8", newline="\n") as f:
        for name in base + sorted(set(funcs) - set(base)):
            f.write(name + "\n")


def cmd_hooks(args):
    mods, _ = load_mods(args[0])
    funcs = load_functions(args[1])
    lines = ["", "# Generated by ps1_mods.py from mods.toml - the mods' hooks"]
    for i, (func, at, hooks) in enumerate(sites(mods, funcs)):
        text = hook_text(f"ps1_mod_hook_{i}", hooks)
        lines += ["[[patches.hook]]", f'func = "{func}"']
        if at is not None:
            lines.append(f"before_vram = 0x{at:08X}")
        lines.append(f'text = "{text}"')
    with open(args[2], "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def read_header(path):
    exports, imports = {}, {}
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        m = EXPORT_RE.match(line)
        if m:
            exports[m.group(2)] = (m.group(1), [p.strip() for p in (m.group(3) or "").split(",") if p.strip()])
            continue
        m = IMPORT_RE.match(line)
        if m:
            imports[m.group(2)] = (m.group(1), [p.strip() for p in (m.group(3) or "").split(",") if p.strip()])
    return exports, imports


def cmd_glue(args):
    mods, _ = load_mods(args[0])
    funcs = load_functions(args[1])
    exports, imports = read_header(args[2])
    out = args[3]
    os.makedirs(out, exist_ok=True)
    all_sites = sites(mods, funcs)

    c = ["/* Generated by ps1_mods.py - do not edit. The mods' hooks on the recompiled game. */",
         '#include "recomp.h"',
         '#include "ps1hle_guest.h"',
         "",
         "extern w2c_ps1hle ps1r_hle;",
         "void ps1r_enter(recomp_context *ctx);",
         "void ps1r_leave(void);",
         "recomp_func_t *get_function(int32_t vram);",
         "",
         "/* for the live recompiler (ps1_live.cpp): where each hook goes, its text as N64Recomp's */",
         "typedef struct { const char *func; uint32_t vram; const char *text; int (*fn)(uint8_t *, recomp_context *); } Ps1rModHook;",
         ""]
    h = ["/* Generated by ps1_mods.py - do not edit. */",
         '#include "recomp.h"']
    table, resets = [], []
    for i, (func, at, hooks) in enumerate(all_sites):
        name = f"ps1_mod_hook_{i}"
        h.append(f"int {name}(uint8_t *rdram, recomp_context *ctx);")
        body = []
        for k, hk in enumerate(hooks):
            call = hk["call"]
            exp = exports.get(call)
            if exp is None:
                fail(f"hook {call}: the mod code has no function '{call}' (is its file in sources?)")
            ret, params = exp
            regs = [reg(r, call) for r in hk.get("args", [])]
            if len(regs) != len(params):
                fail(f"hook {call}: args names {len(regs)} registers, the function takes {len(params)}")
            if ret not in ("void", "u32", "s32"):
                fail(f"hook {call}: returns {ret}; hooks return void or a 32-bit integer")
            if ("result" in hk or "return_if" in hk) and ret == "void":
                fail(f"hook {call}: result / return_if need a function that returns a value")
            if hk.get("after"):
                vram = funcs[func][0]
                flag = f"s{name}_{k}_inside"
                c.append(f"static int {flag};")
                resets.append(f"    {flag} = 0;")
                saved = ", ".join(f"r{r} = (uint32_t)ctx->r{r}" for r in range(4, 8))
                callargs = ", ".join(["&ps1r_hle"] + [(f"r{r}" if 4 <= r < 8 else f"(uint32_t)ctx->r{r}") for r in regs])
                body += [f"    /* {call}: after {func} returns */",
                         f"    if (!{flag})",
                         "    {",
                         f"        const uint32_t {saved};",
                         f"        {flag} = 1;",
                         f"        get_function((int32_t)0x{vram:08X}u)(rdram, ctx); /* {func} itself */",
                         f"        {flag} = 0;",
                         "        ps1r_enter(ctx);",
                         ("        " + (f"ctx->r{reg(hk['result'], call)} = (gpr)(int32_t)" if "result" in hk else "")
                          + f"w2c_ps1hle_{call}({callargs});"),
                         "        ps1r_leave();",
                         "        return 1;",
                         "    }"]
            else:
                callargs = ", ".join(["&ps1r_hle"] + [f"(uint32_t)ctx->r{r}" for r in regs])
                body.append("    ps1r_enter(ctx);")
                if ret == "void":
                    body.append(f"    w2c_ps1hle_{call}({callargs});")
                    body.append("    ps1r_leave();")
                else:
                    body.append(f"    {{ const int32_t r = (int32_t)w2c_ps1hle_{call}({callargs});")
                    body.append("    ps1r_leave();")
                    cond = None
                    if hk.get("entry") and "return_if" in hk:
                        cond = "r != 0" if hk["return_if"] == "nonzero" else "r >= 0"
                    if "result" in hk and cond:
                        body.append(f"    if ({cond}) {{ ctx->r{reg(hk['result'], call)} = (gpr)r; return 1; }}")
                    elif cond:
                        body.append(f"    if ({cond}) return 1;")
                    elif "result" in hk:
                        body.append(f"    ctx->r{reg(hk['result'], call)} = (gpr)r;")
                    body.append("    }")
        where = f"0x{at:08X}" if at is not None else "entry"
        c += [f"/* {func} {where}: {', '.join(x['call'] for x in hooks)} */",
              f"int {name}(uint8_t *rdram, recomp_context *ctx)",
              "{",
              "    (void)rdram;"] + body + ["    return 0;", "}", ""]
        table.append(f'    {{"{func}", 0x{at if at is not None else 0:08X}u, "{hook_text(name, hooks)}", {name}}},')
    c += ["const Ps1rModHook ps1r_mod_hooks[] = {"] + table + ['    {0, 0, 0, 0},', "};",
          f"const unsigned ps1r_mod_hook_count = {len(all_sites)};", "",
          "/* a new run of the game: no hook is half way */",
          "void ps1r_mods_reset(void)", "{"] + resets + ["}", ""]

    # mod code -> game functions: the module's imports named after one
    g = ["/* Generated by ps1_mods.py - do not edit. Mod code calling the recompiled game. */",
         '#include "recomp.h"',
         '#include "ps1hle_guest.h"',
         "",
         "uint32_t ps1r_call_game(uint32_t vram, int nargs, const uint32_t *args);",
         ""]
    calls = 0
    for imp, (ret, params) in sorted(imports.items()):
        if imp not in funcs:
            continue
        if ret not in ("void", "u32", "s32") or any(p not in ("u32", "s32") for p in params):
            fail(f"mod code calls {imp} with arguments or a result that aren't 32-bit integers")
        decl = ", ".join(["struct w2c_env *env"] + [f"u32 a{i}" for i in range(len(params))])
        arr = ", ".join(f"a{i}" for i in range(len(params))) or "0"
        call = f"ps1r_call_game(0x{funcs[imp][0]:08X}u, {len(params)}, (const uint32_t[]){{{arr}}})"
        g += [f"{ret} w2c_env_{imp}({decl})",
              "{",
              "    (void)env;",
              f"    {'return ' if ret != 'void' else ''}{call};",
              "}", ""]
        calls += 1

    for name, lines in (("ps1_mod_hooks.c", c), ("ps1_mod_hooks.h", h), ("ps1_game_calls.c", g)):
        text = "\n".join(lines) + "\n"
        path = os.path.join(out, name)
        if not os.path.exists(path) or open(path, encoding="utf-8").read() != text:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
    print(f"ps1_mods: {len(mods['sources'])} mod sources, {sum(len(s[2]) for s in all_sites)} hooks at "
          f"{len(all_sites)} places, {calls} game functions the mods call")


def main():
    cmds = {"sources": (cmd_sources, 1), "imports": (cmd_imports, 3), "hooks": (cmd_hooks, 3), "glue": (cmd_glue, 4)}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds or len(sys.argv) - 2 != cmds[sys.argv[1]][1]:
        sys.exit(__doc__)
    cmds[sys.argv[1]][0](sys.argv[2:])


if __name__ == "__main__":
    main()
