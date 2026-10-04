# com.recomp.ps1 runtime

Builds a 100%-decompiled PS1 game as a native 32-bit Windows program, and plays it in
Polyphase through the **Ps1Player** node (`../Source`). Game packages such as
`com.recomp.digimonworld` only carry a build config, patches against their decomp and a
`game.json`.

```
Runtime/
  cmake/Ps1Game.cmake    ps1_add_game(): the whole build, parameterised per game
  port/include           PsyQ-compatible headers the game compiles against, port_shm.h
  port/guest             PsyQ replacement (libgpu/libgte/libgs/libetc/libcd/libapi/libsnd),
                         boot, overlays, movies (STR/MDEC + XA), memory card, fault handler
  port/host              Windows host: window (software GPU, GDI blit), audio, pads,
                         shared-memory mode for Ps1Player, headless test options
  tools/                 build_game.ps1, buildlog.ps1, check_addon.ps1, apply_patches.py,
                         sjis_sources.py, gen_symbols.py, ovl_launcher.py, extract_disc.py
```

Modding (patches, mod code, options, assets, the Lua script bridge): see
[`../Docs/Modding.md`](../Docs/Modding.md).

## Adding a game

1. New package `Packages/com.recomp.<game>` with:
   - `package.json` with `"dependencies": {"com.recomp.ps1": "^1.0.0"}` and no `native` block.
   - `game.json`: `exe` and `disc` relative to the package, `saves` relative to the project:
     ```json
     { "title": "My Game", "exe": "Bin/mygame.exe", "disc": "../../../my_decomp/disc.bin", "saves": "Saves/MyGame" }
     ```
   - `Native/CMakeLists.txt` that includes `../../com.recomp.ps1/Runtime/cmake/Ps1Game.cmake`
     and calls `ps1_add_game(...)` (parameters are documented at the top of that file; see
     `com.recomp.digimonworld/Native/CMakeLists.txt` for a full example).
   - `Native/build.ps1`: copy the Digimon World one (it forwards to `tools/build_game.ps1`).
   - `Native/tools/list_sources.py`: prints the game's C sources (usually read from the
     decomp's makefiles / splat config).
2. Fixes to the game code go in `Native/patches/*.patch` (unified diffs against the decomp,
   e.g. `git diff` output). They are applied to copies under `build/gen/patched` at
   configure time; the decomp checkout stays clean. Guard nothing: the patched copies are
   only ever compiled for the port.
3. `Native/build.ps1` → `Bin/<name>.exe`; in the editor add a Ps1Player node with
   *Game* = the package id.

## How it works

- Game code is compiled with clang for `i686-pc-windows-gnu` with `-mno-ms-bitfields`
  (PS1/GCC struct layout), `-fno-delete-null-pointer-checks` and `-mno-stack-arg-probe`.
- The exe is linked at 0x80800000 (fixed base, large-address-aware); PS1 main RAM is
  mapped at 0x80000000 and the scratchpad at 0x1F800000, and the boot executable from the
  disc is loaded to its original address, so data the decomp leaves to the linker has its
  real contents and PS1 pointers stay valid as they are.
- PS1 behaviour Windows does not have is emulated by a fault handler: accesses through
  NULL (the PS1 maps RAM at address 0) and integer division by zero.
- Each overlay's globals (`.ovNN$d` sections, `OVERLAYS` order) are reset when the game
  loads that overlay, as on the console.
- Japanese text in the decomp is re-encoded to CP932 at configure time.

## Running

Standalone: `Bin\<name>.exe [--disc path.bin]` (the disc from `ps1_add_game(DISC ...)` is
baked in as the default, then `<exe dir>\<disc file name>`).

| PS1 pad | Keyboard | XInput |
|---|---|---|
| D-pad | arrow keys | d-pad |
| Cross | Z | A |
| Circle | X | B |
| Square | A | X |
| Triangle | S | Y |
| L1 / R1 | Q / W | LB / RB |
| L2 / R2 | 1 / 2 | triggers |
| Start | Enter | Start |
| Select | Backspace / Right Shift | Back |

Alt+Enter or F11 toggles fullscreen. Memory card 1 is a folder: `saves\card0\` next to the
exe, or `--saves DIR`.

Inside Polyphase: **Tools → Addons → Reload Native Addons**, add a **Ps1Player** node
(it creates a full-screen Quad child) and press Play. *Game* picks the package; *Game
Executable*, *Disc Image* and *Save Folder* override its `game.json` when set. The game
runs as a child process (it needs the fixed 32-bit addresses), frames and pad bits go
through shared memory (`port/include/port_shm.h`), the child plays its own audio and
writes its log to `<Save Folder>\game.log`. It is killed with the editor. Other platforms
get a stub that logs a warning.

## Consoles and packaged games (in-process guest)

`Nativeuild.ps1 -Guest wasm` compiles the game to WebAssembly (wasi-sdk), translates it
to C (wasm2c, WABT) and writes that C to `com.recomp.ps1/Source/Guest/<name>/`. The addon
then contains the game: the editor compiles it into the Windows addon and into every
console build, and Ps1Player runs it in-process on a thread of its own
(`Source/Ps1GuestHost.cpp`; frames to its Quad, audio through engine audio streams, pad
from engine input). Without a translated guest, Ps1Player falls back to running the game
package's `Assets/Bin/<name>.exe` (Windows only).

- Memory: one 16 MB buffer per running game (`Source/Wasm/ps1w.h`); the guest is
  little-endian on every host (big-endian hosts byte-swap on load/store).
- Disc image: the *Disc Image* property, else a file with the name the game was built
  for in the project folder, its `Assets/`, the game package's `Assets/`, or on Wii/GC
  `sd:/ps1/`, `usb:/ps1/`.
- Saves: `Saves/<title>/` under the project (game.json `saves` overrides).
- Tools: wasi-sdk and WABT are found in a `Tools` folder next to a parent of the game
  package (here `P:\Projects\Recomp\Tools`), or set `PS1_WASI_SDK` / `PS1_WABT`.
- Standalone builds of the same guest: `-Guest wasm` also gives `build\wasm-<Config>\<name>.exe`,
  and `-Platform wii|gamecube` a libogc `.dol` (`Runtime/port/host/ogc`; disc and saves in
  `/ps1/` on the SD card, test options in `/ps1/<title>.args`). For Dolphin without SD
  folder sync, `tools\make_sd_image.py` builds an `sd.raw`.
- Decomp code that relies on MIPS behaviour the wasm build does not reproduce shows up as
  differences from the native build (compare frame dumps of both): e.g. `void` functions
  whose callers use the value left in `v0` need a patch (Digimon World: patch 0005).

## Headless testing

```
<name>.exe --headless --frames N --dump DIR --every N [--dump-from F] [--dump-vram]
           [--script "F:HEX[:DUR],..."]   # pad bits (PadRead layout) held from vblank F
           [--wav out.wav] [--trace-gpu F] [--debug-nomovie 1]
```

Games can add their own test hooks through `EXTRA_SOURCES` (Digimon World: `--debug-warp`,
`--debug-battle`).

## Checks

- `tools\buildlog.ps1 -GameDir <pkg>\Native`: build with keep-going, write
  `build\last_build.log` and print an error summary.
- `tools\check_addon.ps1`: compiles the com.recomp.ps1 addon with cl outside the editor.
