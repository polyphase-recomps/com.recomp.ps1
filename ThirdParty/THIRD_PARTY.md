# Third-party code in com.recomp.ps1

## N64Recomp (`ThirdParty/N64Recomp`)

A trimmed snapshot of [N64Recomp](https://github.com/N64Recomp/N64Recomp) and the libraries it
builds with, used by the Recomp build modes (README, "Recomp mode"). It is the same snapshot as
com.recomp.n64's, kept as this package's own copy. The exact commits are in
`N64Recomp/VERSION.txt`, and `update_n64recomp.ps1` refreshes it. Only the sources its CMake uses
are kept (no tests, docs, bindings or examples).

| Component | Path | License | Notice file |
|---|---|---|---|
| N64Recomp | `N64Recomp/` | MIT | `N64Recomp/LICENSE` |
| rabbitizer (MIPS / R3000GTE disassembler) | `N64Recomp/lib/rabbitizer` | MIT | `lib/rabbitizer/LICENSE` |
| fmt | `N64Recomp/lib/fmt` | MIT, with an optional exception for binaries | `lib/fmt/LICENSE` |
| toml++ | `N64Recomp/lib/tomlplusplus` | MIT | `lib/tomlplusplus/LICENSE` |
| ELFIO | `N64Recomp/lib/ELFIO` | MIT | `lib/ELFIO/LICENSE.txt` |
| sljit | `N64Recomp/lib/sljit` | BSD-2-Clause | `lib/sljit/LICENSE` |

**What uses what:**
- **Editor / developer tooling:** the N64Recomp CLI, to recompile a game from the developer's own
  disc (`Runtime/tools/recomp/build_recomp.ps1`).
- **PC runtime (Recomp (live)):** LiveRecomp, N64Recomp's library, toml++, fmt, rabbitizer and
  sljit, linked into the game (`Runtime/recomp/ps1_live.cpp`).
- **Decomp builds and console builds:** link none of it.

**Binary distributions** that include any of these (the editor addon in a Recomp mode, PC games in
Recomp (live)) must reproduce the notices above; sljit's BSD-2 license requires it. Ship this file
plus the listed license files as the game's third-party notices.

### Local changes to N64Recomp

`n64recomp-local.patch` (re-applied by `update_n64recomp.ps1`; regenerate it from a pristine copy
of the snapshot with `git diff --no-index`, paths rewritten to `ThirdParty/N64Recomp/...`):

- **PS1 mode** (`Context::ps1`, syms/config `[input] arch = "ps1"`): the input is PS1 code (R3000,
  little-endian, the GTE as cop2).
  - Decoding: GTE commands, `mfc2/cfc2/mtc2/ctc2`, `lwc2/swc2`, `mfc0/mtc0` and `rfe` go to new
    `Generator` hooks (`emit_ps1_*`). The C generator calls the runtime's `ps1_gte_*`,
    `ps1_cop0_*` and `ps1_rfe`.
  - `div`/`divu` give the R3000's results for a zero divisor (`PS1_DIV`).
  - `syscall` doesn't return from the function.
  - Size-0 symbols named `<name>_recomp` are runtime functions (the PsyQ libraries), and a `jal`
    to one is a call by name.
  - A jump table with no entries in the image (filled at run time) becomes an indirect call.
  - setjmp is inlined as a host `setjmp` at the call site.
  - LiveRecomp: the same hooks through `LiveGeneratorInputs`; loads and stores go through the
    runtime's address folding (`ps1_fold`) with no byte swizzle; `lwl/lwr/swl/swr` are
    little-endian; `rdram_offset` is per generator.
- **com.recomp.n64's LiveRecomp hooks** (`external_functions`, `loop_budget` / `loop_preempt`),
  which the snapshot was copied with.

All of it is additive: with `ps1` unset and the hooks unused, N64Recomp behaves as upstream. The
patch is MIT like the code it changes.

## Not used

PCSX / psxrecomp / RecompOne and other PS1 recompilers or emulators are not part of this package,
nor is any of their code. The recomp runtime (`Runtime/recomp`) is our own and implements
N64Recomp's generated-code contract for PS1 independently.
