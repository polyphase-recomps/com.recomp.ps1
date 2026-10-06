# Recomp mode: a PS1 game recompiled from the disc

The decomp build (Runtime/README.md) needs a game whose decompilation is finished. **Recomp
mode** runs one whose decomp is not: N64Recomp (com.recomp.ps1's copy, with a PS1 mode)
recompiles the game's own MIPS code from the player's disc into C, or into machine code when the
game boots. The PS1 SDK it calls (PsyQ: libgpu, libgs, libgte, libcd, libsnd, libpress, libcard,
libapi ...) is the runtime's own replacement, the same code decomp builds use.

Windows x64 only (editor and PC builds). Consoles keep the decomp builds.

| Build mode | What the addon gets | The player needs |
|---|---|---|
| Decomp | the decomp translated by wasm2c (`Source/Guest/<name>`) | the disc (extracted into the package) |
| Recomp | the game recompiled from the developer's disc, as C, in a library | the disc |
| Recomp (live) | the runtime and N64Recomp's LiveRecomp, **no game code** | the disc: the game is recompiled from it when it boots (about 0.2 s) |

Recomp (live) is the one for releases: a build holds no code from the game at all.

## How it works

```
disc ─► ps1_rom.py ─► rom.bin (exe + overlay files, words big-endian) ─► N64Recomp (PS1 mode) ─► C
                                                    or, at boot: ps1_live.cpp ─► LiveRecomp ─► code
game code ──calls by name──► wrappers (o32 → wasm2c exports) ──► PsyQ replacement (ps1hle, wasm2c)
PsyQ replacement ──function pointers (callbacks)──► ps1w_guest_callback ──► recompiled code
```

- **Symbols** (`Recomp/syms.toml`, from `Runtime/tools/recomp/ps1_syms.py`): one section per disc
  file (the boot executable, each overlay), with its functions. PsyQ functions are size-0
  `<name>_recomp` symbols: never recompiled, called by name.
- **The libraries** (`Runtime/port/guest`) are compiled to WebAssembly and translated by wasm2c,
  like a decomp game without the game (`ps1hle`). They are linked at 0x80000000 over a
  placeholder of the game's data symbols (`data_symbols.txt`), so they use PsyQ's globals at
  the game's own addresses. Their linear memory is the guest memory the recompiled code uses
  (`Source/Wasm/ps1w.h`), so a PS1 address means the same thing on both sides.
- **Overlays**: a section becomes live when the game reads its file's sectors to its load
  address (the disc read hook). Calls by address go to the live section's function there.
- **The runtime** (`Runtime/recomp/`): boot from SYSTEM.CNF and the PS-X EXE header, code lookup,
  callbacks into the game, the GTE (through the libraries' `port_gte_*`), cop0, setjmp/longjmp,
  a small libc on guest memory, and a crash report naming the guest return address.
- The game is a `Ps1wModule` (`ps1w_module_<name>_recomp`), so every host of a wasm2c guest
  runs it unchanged: Ps1Player in the editor and in packaged games, and the standalone program
  (`<name>_recomp.exe`: window, `--headless`, `--dump`, `--wav`, `--script` as the decomp
  build's).

## A game package's Recomp/ folder

```
Packages/com.recomp.<game>/Recomp/
  game.json         "title", "name" (a C identifier: the library and module name), "config",
                    "rom": {"file": the boot executable, "sha1": its SHA-1, "disc": the usual image name}
  recomp.<r>.toml   N64Recomp config: [input] arch = "ps1", entrypoint, symbols_file_path,
                    rom_file_path, output_func_path (build_recomp.ps1 sets the last two)
  syms.toml         the sections and functions (ps1_syms.py)
  psyq.txt          the PsyQ functions the game has (ps1_syms.py)
  data_symbols.txt  the game's data symbols (ps1_syms.py)
  CMakeLists.txt    ps1_recomp_game(NAME TITLE PACKAGE GENERATED SYMS SECTION_FILES INCLUDES DISC)
```

`Runtime/cmake/Ps1Recomp.cmake` documents `ps1_recomp_game`. `INCLUDES` are the PsyQ headers
the libraries compile against (struct layouts the game shares). They are not part of this
package: a decomp checkout has them (for Digimon World, dw_decomp's
`external/psyq_headers/mw_lib41/include`). `com.recomp.digimonworld/Recomp` is the example.

### Making the symbols

From a splat-based decomp (its yamls and symbol files) and the files on the disc:

```bash
python Runtime/tools/recomp/ps1_syms.py --decomp <decomp> --disc-dir <extracted disc> --out <dir> --file SLUS_010.32=config/us/main.yaml --file BTL_REL.BIN=config/us/btl.yaml
```

The first `--file` is the boot executable; one more per overlay file. Functions are the symbols
in each file's code (splat `c` / `asm` subsegments); PsyQ library segments become runtime calls.
Names that clash with C (`main`, `memcpy` ...) get `_game`. Copy `syms.toml`, `psyq.txt` and
`data_symbols.txt` into `Recomp/`. They hold symbols only, no game code.

## Building

In the editor: **Packaging > Target Options > PS1 Recomp > Build mode**, then **Pre Process
Rom** (Tools > Recomp > <game>) or packaging. Both run `Runtime/tools/recomp/build_recomp.ps1`:

```bash
powershell -File Packages/com.recomp.ps1/Runtime/tools/recomp/build_recomp.ps1 -Package Packages/com.recomp.digimonworld -Disc "<your .bin or .cue>"
```

```bash
powershell -File Packages/com.recomp.ps1/Runtime/tools/recomp/build_recomp.ps1 -Package Packages/com.recomp.digimonworld -Live
```

1. `ps1_rom.py`: the recompiler's input from the disc, checking the boot executable's SHA-1
   (another region or revision would be recompiled wrongly).
2. N64Recomp (built once, `Runtime/build/n64recomp`), then the game's library
   (`<game>/Native/build/recomp-<Config>`).
3. Published: `Lib/Windows/<name>_recomp.lib`, and `Source/Guest/<name>_recomp/` - one file
   that links it into the addon and registers the game. The game's Decomp build
   (`Source/Guest/<name>`) is removed: one module per game package. Build mode Decomp makes it
   again and removes the Recomp one.
4. The disc is extracted to the package's `Assets/Disc` when it isn't yet (Ps1Player plays it).

`-Live` builds no C and needs no disc. It copies `game.json` and `syms.toml` to the package's
`Assets/Recomp/Live`, which ships with the project's builds; Ps1Player points the game at it.
`-DebugCrt` builds for an editor built in Debug (the editor passes it).

Then restart the editor (the addon is relinked as it loads).

**Limits:** one recompiled game per editor project (the libraries' symbols are the same for every
game; other PS1 games in the project stay in Decomp mode). Mods compiled into a decomp build
(a game package's `Native/game/*.c`, `patches/`) don't exist in Recomp mode; disc file mods
(`Assets/Disc`), mod settings and the launcher do.

## Launcher

Every PS1 game in the addon is a com.recomp.mod.base launcher (`Source/Ps1Launcher.h`): a
`RecompLauncher` scene, `@launcher:*` buttons and `Recomp.SetRomLocation` / `StartGame` in Lua
work for it. The player's disc is checked by what its SYSTEM.CNF boots and, for Recomp (live)
builds, that executable's SHA-1. It is remembered in `Saves/<package>.disc.txt`, and Ps1Player
plays it.

## Testing

The standalone program runs the same scripts as the decomp build's (Runtime/README.md, Headless
testing):

```bash
<game>/Native/build/recomp-RelWithDebInfo/dw_recomp.exe --disc "<image or Assets/Disc/disc.idx>" --headless --frames 20000 --dump out --every 1000 --script "<script>"
```

For a live build, `PS1_RECOMP_DIR=<folder with game.json and syms.toml>` gives the data. Digimon
World (2026-10-06): 4204 functions; Recomp, Recomp (live) and the extracted-disc run all give
frames identical to each other over 20000 frames of title, new game, memory card save, name
entry, the intro movie and the field. Against the decomp port they match from the field on (the
port patches its own movie player in).
