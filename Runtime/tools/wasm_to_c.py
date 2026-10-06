#!/usr/bin/env python3
"""Turn the linked guest module (.wasm) into C for the host compilers.

1. Trims the zero runs at the ends of every active data segment (the PS1 RAM
   placeholder alone is 2 MB of zeros; linear memory starts zeroed anyway).
2. Runs wasm2c, split into several files so they compile in parallel.
3. Makes the generated helpers follow the runtime's memory model by including
   Source/Wasm/ps1w_mem_ops.h and ps1w_ops.h at the right places of the
   generated -impl.h (masked addresses, little-endian layout on any host, R3000
   division, no call_indirect type check).
4. Replaces the traps wasm-ld puts in place of direct calls whose signature does
   not match the callee (K&R declarations in decomps) with calls that pass the
   arguments the callee takes, as a MIPS call would.
5. Writes the module descriptor (Source/Wasm/ps1w_module.h) and, with --register, the
   C++ file that registers it with Ps1Player when the addon loads.

Usage: wasm_to_c.py <wasm2c.exe> <in.wasm> <out_dir> <name> <num_outputs>
                    [--package=<id>] [--title=<text>] [--disc=<file name>]
                    [--runtime-include=<path prefix of Source/Wasm>] [--register]
                    [--define=<macro>] (defined at the top of the -impl.h: PS1W_RECOMP)
Writes <out_dir>/<name>_guest.h, _guest-impl.h, _guest_0.c ..., _guest_module.c,
[_guest_register.cpp] and <name>_stubs.txt; the wasm2c module is named <name>.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time


# ---- wasm binary: trim data segments ----------------------------------------------
def read_uleb(b, p):
    result = shift = 0
    while True:
        byte = b[p]
        p += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, p


def read_sleb(b, p):
    result = shift = 0
    while True:
        byte = b[p]
        p += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            if byte & 0x40:
                result -= 1 << shift
            return result, p


def uleb(v):
    out = bytearray()
    while True:
        byte = v & 0x7F
        v >>= 7
        if v:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def sleb(v):
    out = bytearray()
    while True:
        byte = v & 0x7F
        v >>= 7
        if (v == 0 and not byte & 0x40) or (v == -1 and byte & 0x40):
            out.append(byte)
            return bytes(out)
        out.append(byte | 0x80)


def to_i32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def trim_data(wasm):
    assert wasm[:8] == b"\0asm\1\0\0\0", "not a wasm module"
    out = bytearray(wasm[:8])
    p = 8
    saved = 0
    while p < len(wasm):
        sec_id = wasm[p]
        size, body = read_uleb(wasm, p + 1)
        end = body + size
        if sec_id != 11:
            out += wasm[p:end]
            p = end
            continue
        count, q = read_uleb(wasm, body)
        new = bytearray(uleb(count))
        for _ in range(count):
            flags, q = read_uleb(wasm, q)
            if flags not in (0, 2):
                # passive segment: copy as is
                n, r = read_uleb(wasm, q)
                new += uleb(flags) + wasm[q:r + n]
                q = r + n
                continue
            memidx = b""
            if flags == 2:
                mi, r = read_uleb(wasm, q)
                memidx = uleb(mi)
                q = r
            assert wasm[q] == 0x41, "data offset must be i32.const"
            offset, q = read_sleb(wasm, q + 1)
            assert wasm[q] == 0x0B
            q += 1
            n, q = read_uleb(wasm, q)
            data = wasm[q:q + n]
            q += n
            lead = len(data) - len(data.lstrip(b"\0"))
            data = data[lead:].rstrip(b"\0")
            saved += n - len(data)
            new += uleb(flags) + memidx + b"\x41" + sleb(to_i32(offset + lead)) + b"\x0B" + uleb(len(data)) + data
        out += bytes([11]) + uleb(len(new)) + new
        p = end
    return bytes(out), saved


# ---- generated C: overrides and signature-mismatch stubs -----------------------------
DECL_RE = re.compile(r"^(?:static )?(void|u32|u64|f32|f64) (w2c_\w+)\((w2c_\w+\*[^)]*)\);", re.M)
STUB_RE = re.compile(
    r"^(static )?(void|u32|u64|f32|f64) (w2c_\w+?)_signature_mismatch0x3A(\w+)\((w2c_\w+\* instance[^)]*)\) \{\n.*?^\}\n",
    re.M | re.S)


def param_types(params):
    # "w2c_ps1* instance, u32 var_p0" or "w2c_ps1*, u32" -> ["u32", ...] (instance dropped)
    parts = [x.strip() for x in params.split(",")][1:]
    return [x.split()[0] for x in parts if x]


def patch_impl(path, prefix):
    text = open(path, encoding="utf-8").read()
    anchor = "DEFINE_LOAD(i32_load,"
    assert anchor in text, "wasm2c output changed: no " + anchor
    text = text.replace(anchor, '#include "%sps1w_mem_ops.h"\n' % prefix + anchor, 1)
    m = re.search(r"^#define REM_U\(x, y\).*$", text, re.M)
    assert m, "wasm2c output changed: no REM_U"
    text = text[:m.end()] + '\n#include "%sps1w_ops.h"' % prefix + text[m.end():]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return text


def patch_header(path, prefix):
    text = open(path, encoding="utf-8").read()
    for h in ("wasm-rt.h", "wasm-rt-exceptions.h"):
        text = text.replace('#include "%s"' % h, '#include "%s%s"' % (prefix, h))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def c_string(text):
    return json.dumps(text)


def write_module(out_dir, name, stem, prefix, package, title, disc, register):
    module = f"""/* Generated by wasm_to_c.py - do not edit. */
#include "{stem}.h"
#include "{prefix}ps1w_module.h"

static w2c_{name} sInstance;

static void instantiate(struct w2c_env *env) {{ wasm2c_{name}_instantiate(&sInstance, env); }}
static void release(void) {{ wasm2c_{name}_free(&sInstance); }}
static void run(void) {{ w2c_{name}_port_game_entry(&sInstance); }}
static wasm_rt_memory_t *memory(void) {{ return &sInstance.w2c_memory; }}

const Ps1wModule ps1w_module_{name} = {{
    {c_string(name)}, {c_string(package)}, {c_string(title)}, {c_string(disc)},
    instantiate, release, run, memory,
}};
"""
    with open(os.path.join(out_dir, stem + "_module.c"), "w", newline="\n") as f:
        f.write(module)
    reg = os.path.join(out_dir, stem + "_register.cpp")
    if register:
        with open(reg, "w", newline="\n") as f:
            f.write(f"""// Generated by wasm_to_c.py - do not edit. Registers {title} with Ps1Player.
#include "{prefix}ps1w_module.h"

extern "C" const Ps1wModule ps1w_module_{name};

namespace
{{
struct Register
{{
    Register() {{ ps1w_register_module(&ps1w_module_{name}); }}
}} sRegister;
}}
""")
    elif os.path.exists(reg):
        os.remove(reg)


FUNCREF_RE = re.compile(r"([A-Za-z_][\w>\-.]*_table)\.data\[(var_\w+)\]\.module_instance")


def fix_stubs(c_files, impl_text):
    decls = {name: (ret, param_types(params)) for ret, name, params in DECL_RE.findall(impl_text)}
    report = []
    for path in c_files:
        text = open(path, encoding="utf-8").read()

        def repl(m):
            static, ret, prefix, target, params = m.groups()
            names = [p.split()[1] for p in params.split(",")[1:] if p.strip()]
            types = param_types(params)
            # a second stub for the same callee is named "<callee>.1" (0x2E = '.')
            callee = prefix + "_" + re.sub(r"0x2E\d+$", "", target)
            # wasm_link.py exports every callee; wasm2c then names the export wrapper (which
            # runs the module constructors) <callee> and the function itself <callee>_0
            if callee + "_0" in decls:
                callee += "_0"
            if callee not in decls:
                report.append(f"{target}: callee not found, left as a trap")
                return m.group(0)
            cret, ctypes = decls[callee]
            args = ["instance"] + [f"({t}){names[i]}" if i < len(names) else f"({t})0" for i, t in enumerate(ctypes)]
            call = f"{callee}({', '.join(args)})"
            if ret == "void":
                body = f"  {call};\n"
            elif cret == "void":
                body = f"  {call};\n  return 0;\n"
            else:
                body = f"  return ({ret}){call};\n"
            report.append(f"{target}: caller ({', '.join(types) or 'void'}) -> {ret}, callee ({', '.join(ctypes) or 'void'}) -> {cret}")
            return f"{static or ''}{ret} {prefix}_signature_mismatch0x3A{target}({params}) {{\n{body}}}\n"

        new = STUB_RE.sub(repl, text)
        # call_indirect passes the slot's module instance as an argument, read before the slot
        # is checked: with a PS1 code address for a slot (recomp mode callbacks, Source/Wasm/
        # ps1w_ops.h) that read is out of bounds, so it goes through a checked accessor
        new = FUNCREF_RE.sub(r"PS1W_FUNCREF_INSTANCE(\1, \2)", new)
        if new != text:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(new)
    return report


def replace_with_retry(src, dst, attempts=40, delay=0.25):
    # Windows: a generated file another process still has open (the editor's addon build,
    # an IDE indexer, antivirus scanning the new file) can't be replaced for a moment.
    for i in range(attempts):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if i == attempts - 1:
                raise
            time.sleep(delay)


# WABT's Windows release builds of wasm2c link OpenSSL (libcrypto-3-x64.dll) but don't ship
# it. Shells often find it on PATH (Git for Windows has one), the editor and other
# launchers often don't: wasm2c then dies with STATUS_DLL_NOT_FOUND.
WASM2C_DLLS = ["libcrypto-3-x64.dll"]
STATUS_DLL_NOT_FOUND = 0xC0000135


def dll_dirs_for_windows():
    dirs = [d for d in os.environ.get("PATH", "").split(os.pathsep) if d]
    git = shutil.which("git")
    if git:
        root = os.path.dirname(os.path.dirname(os.path.realpath(git)))  # <Git>\cmd or \bin -> <Git>
        if os.path.basename(root).lower() == "mingw64":
            root = os.path.dirname(root)
        dirs += [os.path.join(root, "mingw64", "bin"), os.path.join(root, "usr", "bin")]
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramW6432"), os.environ.get("ProgramFiles(x86)"),
                 os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs")):
        if base:
            dirs += [os.path.join(base, "Git", "mingw64", "bin"), os.path.join(base, "OpenSSL-Win64", "bin")]
    dirs += [r"C:\msys64\mingw64\bin", r"C:\msys64\ucrt64\bin"]
    return dirs


def wasm2c_env(wasm2c):
    """The environment to run wasm2c in: PATH extended by the folders holding the DLLs it
    needs and that aren't next to it."""
    env = dict(os.environ)
    if os.name != "nt":
        return env
    here = os.path.dirname(os.path.abspath(wasm2c))
    extra = []
    for dll in WASM2C_DLLS:
        if os.path.isfile(os.path.join(here, dll)):
            continue
        found = next((d for d in dll_dirs_for_windows() if os.path.isfile(os.path.join(d, dll))), None)
        if found is None:
            sys.exit(f"wasm2c needs {dll} (OpenSSL 3), which WABT's release doesn't include: install Git for "
                     f"Windows (it has one), or copy {dll} next to {wasm2c}")
        if found not in extra:
            extra.append(found)
    if extra:
        env["PATH"] = os.pathsep.join(extra + [env.get("PATH", "")])
    return env


def run_wasm2c(wasm2c, trimmed, name, outputs, out_dir, stem):
    # wasm2c writes into a scratch folder next to out_dir; the old files are replaced only
    # once it succeeded, so a failure (or a locked file) never leaves out_dir half empty.
    env = wasm2c_env(wasm2c)
    tmp = tempfile.mkdtemp(prefix=".wasm2c-", dir=out_dir)
    try:
        cmd = [wasm2c, trimmed, "-n", name, "--num-outputs=" + outputs, "--disable-tail-call",
               "-o", os.path.join(tmp, stem + ".c")]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout or "")
            if (proc.returncode & 0xFFFFFFFF) == STATUS_DLL_NOT_FOUND:
                sys.exit(f"wasm2c could not start: a DLL it needs is missing (STATUS_DLL_NOT_FOUND). Put the "
                         f"DLLs wasm2c.exe imports next to it ({wasm2c}).")
            sys.exit(f"wasm2c failed (exit {proc.returncode}): {' '.join(cmd)}")
        made = set(os.listdir(tmp))
        for f in sorted(made):
            dst = os.path.join(out_dir, f)
            try:
                replace_with_retry(os.path.join(tmp, f), dst)
            except PermissionError as e:
                sys.exit(f"wasm2c: cannot replace {dst}: {e} (is it open in another program?)")
        for old in os.listdir(out_dir):
            if old.startswith(stem) and (old.endswith(".c") or old.endswith(".h")) and old not in made:
                os.remove(os.path.join(out_dir, old))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) if "=" in a else (a[2:], "1") for a in sys.argv[1:] if a.startswith("--"))
    wasm2c, src, out_dir, name, outputs = args[:5]
    prefix = opts.get("runtime-include", "")
    stem = name + "_guest"
    os.makedirs(out_dir, exist_ok=True)
    wasm, saved = trim_data(open(src, "rb").read())
    trimmed = os.path.splitext(src)[0] + ".trimmed.wasm"
    with open(trimmed, "wb") as f:
        f.write(wasm)
    run_wasm2c(wasm2c, trimmed, name, outputs, out_dir, stem)
    patch_header(os.path.join(out_dir, stem + ".h"), prefix)
    impl_text = patch_impl(os.path.join(out_dir, stem + "-impl.h"), prefix)
    if "define" in opts:
        impl_path = os.path.join(out_dir, stem + "-impl.h")
        impl_text = f"#define {opts['define']} 1\n" + impl_text
        with open(impl_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(impl_text)
    c_files = [os.path.join(out_dir, f"{stem}_{i}.c") for i in range(int(outputs))]
    report = fix_stubs(c_files, impl_text)
    with open(os.path.join(out_dir, name + "_stubs.txt"), "w", newline="\n") as f:
        f.write("\n".join(report) + "\n")
    write_module(out_dir, name, stem, prefix, opts.get("package", ""), opts.get("title", name),
                 opts.get("disc", ""), "register" in opts)
    print(f"wasm_to_c: {saved // 1024} KB of zero data trimmed, {len(report)} signature-mismatch calls adapted")


if __name__ == "__main__":
    main()
