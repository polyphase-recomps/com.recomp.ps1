# com.recomp.ps1: PS1 recompilation runtime for Polyphase

A Polyphase native addon that runs PlayStation 1 games rebuilt from their
decompilations. The game's own C code is compiled (through WebAssembly and wasm2c) into
this addon. The PS1 hardware and SDK it needs are replaced by this runtime: GPU, GTE,
sound, CD, movies, memory card. The result runs in the editor and in packaged builds
for Windows, Wii and GameCube.

This package is the shared runtime. Each game is a separate package that carries its
build setup, patches and mods, for example
[`com.recomp.digimonworld`](../com.recomp.digimonworld). Start with that game's README;
it lists what to install and how to set it up.

> **No game is included.** Game packages build from the user's own decomp checkout and
> disc image. The code they generate here (`Source/Guest/`) is never committed; see
> `.gitignore`.

## What's in it

| Path | |
|---|---|
| `Source/` | The addon: `Ps1Player` node, the in-engine game host, the script bridge (Lua `Ps1.*`), bridge-bound UI widgets, editor tools (Tools > Recomp > <game> > Pre Process Rom, the UI builders) |
| `Source/Wasm/` | wasm2c runtime and the host side of the GPU, disc and imports |
| `Source/Guest/<game>/` | **Generated** by each game's Pre Process Rom (not in git) |
| `Runtime/` | The PS1 replacement (`port/`), the game build (`cmake/Ps1Game.cmake`) and the tools |
| `Docs/Modding.md` | Writing mods, patches, options, asset replacements and script-bridge APIs |

## Docs

- [Runtime/README.md](Runtime/README.md): adding a game, how the runtime works,
  running, consoles, headless testing.
- [Docs/Modding.md](Docs/Modding.md): modding guide.

## Requirements

The same as for building a game: Windows, the Polyphase editor, Visual Studio 2022
(C++ and Clang tools), Python 3, wasi-sdk and WABT, plus devkitPro for Wii and
GameCube. The game package's README has the details and versions.
