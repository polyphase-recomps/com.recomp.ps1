# Modding PS1 recomp games

This guide covers every game built on `com.recomp.ps1` (Digimon World is the reference
game). Game-specific hook points and recipes live in each game package, e.g.
[`com.recomp.digimonworld/Docs/Modding.md`](../../com.recomp.digimonworld/Docs/Modding.md).

A recompiled game is the game's own C code (from its decompilation) running on this
runtime. That makes a mod ordinary C: you change what the game does at the place it does
it, and it runs at full speed on every target (Windows, Wii, GameCube, ...).

## What kind of mod is it?

| You want to... | Use | Status |
|---|---|---|
| Change a rule of the game (damage, hunger, prices, AI, ...) | **Patch** + **mod code** | Works today |
| Add a feature that runs every frame (cheats, auto-skip, trainers) | **Patch** (one hook) + **mod code** | Works today |
| Let the player switch it on/off or tune it | **Option** in `game.json` | Works today |
| Replace a texture, model, sound or text file | **Asset** in `Assets/Disc` | Works today, with a size rule |
| Read game state or trigger actions from Polyphase (Lua, UI, buttons) | **Script bridge** | Works today (in-engine builds) |

A typical mod is all three of the first: a small **patch** that calls into your
**mod code**, which reads an **option** to decide what to do. The Digimon World
"hold A to skip" mod is a complete example:
`Native/patches/0006-hold-to-skip.patch`, `Native/game/holdskip.c` and
`"options": "holdskip=2"` in `Assets/game.json`.

## Where things live

```
Packages/com.recomp.<game>/
  Assets/game.json            title, disc, saves and "options" (mod settings)
  Assets/Disc/                the game's extracted disc: replace files here (asset mods)
  Native/CMakeLists.txt       build: lists the mod sources (EXTRA_SOURCES)
  Native/patches/NNNN-*.patch changes to the decomp's code, applied in name order
  Native/game/*.c             mod code, compiled into the game
```

The decomp checkout itself (`dw_decomp`) is **never edited**. Patches are applied to
copies under `Native/build/<config>/gen/patched/` every time the build configures, so the
decomp can be updated (`git pull`) and the patches re-apply on top.

## 1. Patches: changing the game's code

A patch is a unified diff against the decomp, for example:

```diff
diff --git a/src/main/main.c b/src/main/main.c
--- a/src/main/main.c
+++ b/src/main/main.c
@@ -6546,6 +6546,13 @@
 	}
 #endif
 	pollInputGame();
+#ifdef PORT
+	{
+		/* mod "holdskip": hold Cross to skip through scenes (game/holdskip.c) */
+		extern void dw_holdskip_tick(void);
+		dw_holdskip_tick();
+	}
+#endif
 	ACTIVE_FRAMEBUFFER = GsGetActiveBuff();
```

### Rules of thumb

- **Keep patches small: hook, don't rewrite.** Add one call or one condition and put the
  logic in a mod source file. Small hunks survive decomp updates; big rewrites don't.
- **Wrap your lines in `#ifdef PORT`.** Patched copies are only compiled for the port, so
  it is optional, but it marks port-only code clearly (all Digimon World patches do it).
- **One mod, one patch file.** Name it `NNNN-what-it-does.patch`. Patches are applied
  one after another in file name order, each on top of the previous ones, so a later
  patch's context may include lines an earlier patch added (0006 above sits right below
  0004's hook, and 0007 adds a line inside 0004's block).
- **Use the game's names.** Decomp function and variable names (`tickScript`,
  `POLLED_INPUT`, ...) are stable; addresses and `MAIN_func_800FFDF4`-style placeholder
  names can be renamed by the decomp project later. When that happens the patch stops
  applying and the build tells you which hunk (see below), so you only fix the context.

### Making a patch

1. Get the files as the build sees them (the decomp plus every existing patch):

   ```powershell
   cd Packages\com.recomp.<game>\Native
   python ..\..\com.recomp.ps1\Runtime\tools\apply_patches.py `
       ..\..\..\..\dw_decomp  $env:TEMP\mod\a  (Get-ChildItem patches\*.patch)  `
       ..\..\..\..\dw_decomp\src\main\main.c
   ```

   This writes patched copies under `$env:TEMP\mod\a\src\...`. A file no existing patch
   touches is not copied; take it straight from the decomp.
2. Copy `a` to `b` and edit the files in `b`.
3. Write the diff with paths relative to the decomp root and save it in `patches/`:

   ```bash
   cd "$TEMP/mod"
   f=src/main/main.c
   { echo "diff --git a/$f b/$f"; diff -u --label a/$f --label b/$f a/$f b/$f; } \
       | tr -d '\r' > path/to/Native/patches/0007-my-mod.patch
   ```

   (Repeat the `{ ... }` line with `>>` for more files.) `git diff --no-index` output
   works too.
4. Build. The patch is applied on the next configure.

The applier matches each hunk by its context lines (removed and unchanged lines), looking
up to 200 lines around the line number in the `@@` header, so line numbers may drift.
If the context isn't found the build stops with
`apply_patches: hunk at line N does not apply to src/...`. Line endings don't matter.

## 2. Mod code: `Native/game/*.c`

Mod sources are compiled into the game like its own code (same compiler, same headers,
`PORT` defined). Add each one to `EXTRA_SOURCES` in `Native/CMakeLists.txt`:

```cmake
    EXTRA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/game/debug.c"
                  "${CMAKE_CURRENT_SOURCE_DIR}/game/holdskip.c"
                  "${CMAKE_CURRENT_SOURCE_DIR}/game/my_mod.c"
```

A mod file usually has one function that a patch calls, plus whatever state it keeps:

```c
#include <port_host.h>

extern unsigned int POLLED_INPUT;  /* the game's globals: declare what you use */

void mymod_tick(void)              /* called once per frame by a patch */
{
    static int enabled = -1;
    int v[1];

    if (enabled < 0)               /* read the option once */
        enabled = port_debug_values("mymod", v, 1) == 1 && v[0];
    if (!enabled) return;
    /* ... */
}
```

You can include the decomp's headers (`#include "dw/script.h"` and so on) instead of
declaring globals yourself; they are on the include path. Declaring just what you need
keeps the mod independent of header changes in the decomp.

### Where to hook

| Hook | Runs | Use for |
|---|---|---|
| The game's main/field loop (e.g. Digimon World `gameLoop`) | Once per game frame, after input is read | Cheats that hold values ("always full"), trainers, input features |
| The function that computes a value | Every time it's computed | Multipliers, "never miss", "one hit KO" |
| A condition inside a function | When the game decides something | Turning off a penalty, forcing a branch |

Per-frame hooks are the easiest and safest: overwrite the value after the game has
updated it. Prefer them for "infinite / always / never" cheats. Use a hook inside the
calculation when the value must change *before* the game acts on it (damage, hit chance).

**Don't block, wait or loop** in a hook: the game thread also drives the display and the
sound. Do small work and return.

### The runtime API (`port_host.h`)

| Call | What it does |
|---|---|
| `port_debug_values(name, out, max)` | Integer values of option `name` (see Options). Returns how many. |
| `port_log(fmt, ...)` | Writes a line to the log (the editor's log, `game.log`, the console log). |
| `port_vblank_count()` | Vblanks (1/60 s) since the game started. Use it for timers. |
| `port_pad_state()` | Pad bits held right now (PsyQ `PadRead` layout, pad 1 in the low 16 bits). Usually you read the game's own input variable instead. |
| `port_set_speed(n)` | Fast-forward up to `n`× (1 = normal). Must be called every frame while wanted; the host falls back to 1× after ~30 vblanks without a call. |
| `port_file_read/write/size/delete/list` | Files in the game's save folder (for mod data that should persist). |

Pad bits (PsyQ layout, pressed = 1): L2 `0x1`, R2 `0x2`, L1 `0x4`, R1 `0x8`,
Triangle `0x10`, Circle `0x20`, Cross `0x40`, Square `0x80`, Select `0x100`,
Start `0x800`, Up `0x1000`, Right `0x2000`, Down `0x4000`, Left `0x8000`.

## 3. Options: `game.json`

```json
{
    "title": "Digimon World",
    "saves": "Saves/DigimonWorld",
    "options": "holdskip=2 attackmul=3 alwaysfull=1"
}
```

`"options"` is a list of `name=value` or `name=v1,v2,...` entries (integers), separated
by spaces or `;`. The game reads them with `port_debug_values`:

```c
int v[2];
int n = port_debug_values("attackmul", v, 2);   /* n = 1, v[0] = 3 */
```

A missing option returns 0, so make "absent" mean "mod off". The same options reach the
game everywhere it runs:

| Where | How |
|---|---|
| Editor / packaged builds (`Ps1Player`) | from `Packages/<game>/Assets/game.json` |
| Standalone Windows exe, headless tests | `--debug-NAME values`, e.g. `dw.exe --debug-holdskip 2` |
| Standalone Wii / GameCube `.dol` | `/ps1/<title>.args` on the SD card, same `--debug-` syntax |

Options are read when the game starts; change `game.json` and restart the game (stop and
play again, or Reset Game in the HOME menu) to apply.

## 4. Assets: textures, models, sound, text

The build extracts the disc into `Assets/Disc/` with the original folder layout plus
`disc.idx` (which sector range each file came from). The game reads the disc through
that index, so **a file you replace in `Assets/Disc` is what the game loads**. These
files ship with packaged builds, and on consoles they're read from the SD card.

- **Same size or smaller: works.** The file is read in place of the original.
- **Bigger: usually not without a code patch.** Most PS1 games read files by sector
  number and size from tables in their executable (Digimon World does, see its guide),
  so they still read the original size, and a read past it runs into the next file on
  the disc. The runtime can't fix that on its own; growing a file means patching the
  game's file table and making sure the game's load buffer fits it. Keep replaced files
  at or below the original size.
- Re-extracting never overwrites your files. To restore the originals:
  `python Runtime/tools/extract_disc.py <disc.bin> <Assets/Disc> --force`.
- Formats are the game's own: PS1 games use TIM textures and TMD (or custom) models,
  often packed into archives. You need a tool that edits that format; the decomp's
  loaders (named in each game's guide) show the layout.

Texture/model **upgrades** beyond the PS1 format (higher resolution textures, new
models drawn by Polyphase) need runtime support that doesn't exist yet: the PS1 GPU
renders at the original resolution from 1 MB of VRAM. Format-compatible replacements
work today.

## 5. Building and testing

- In the editor: packaging runs **Setup Dependencies** first (Packaging window →
  Target Options → PS1 Recomp), which rebuilds the game with your patches and mod code.
  Run it from there while iterating, then **Reload Native Addons**.
- From a terminal: `Native\build.ps1` (Windows exe) or
  `Native\build.ps1 -Guest wasm -Target ps1_addon` (what the editor uses).
- Headless tests make mods easy to check without playing:

  ```
  dw.exe --headless --frames 9600 --dump out --every 300 ^
         --script "100:800,300:40,4560:40:4800" --debug-holdskip 2 --debug-nomovie 1
  ```

  `--script` presses pad bits at given vblanks (`frame:HEX[:duration]`), `--dump` writes
  frames as images, and your `port_log` lines go to stdout. Run once with the mod and
  once without and compare the frames or the log.

## 6. Script bridge: driving the game from Polyphase

The bridge lets Lua scripts and Polyphase UI read the game's state and ask the game to
do things: a stats panel, an evolution tracker, a map with fast-travel buttons, debug
tools. It works wherever the game runs inside the engine (editor play, packaged
Windows / Wii / GameCube builds). The standalone test programs ignore it.

```
 Polyphase (main thread)                      Game (its own thread)
 ───────────────────────                      ─────────────────────
 Ps1.Get("money") ───── reads game memory ──► (the last frame's values)
 Ps1.Request("warp", 3, 0) ─► queue ─► port_bridge_pump() in the game loop runs it
 Ps1.Result(id)  ◄──────────── result ◄──── handler's return value
```

Reads are immediate and safe at any time: values are those of the game's last frame.
Writes and requests are **queued** and carried out by the game itself, once per frame,
at a point where calling its own functions is safe. They complete one or two frames
later, so poll `Ps1.Result` from a Tick.

### Game side: publishing variables and requests

A game package declares what it offers in its mod code with `port_bridge.h`:

```c
#include <port_bridge.h>

extern int32_t MONEY;

static int add_money(const int *args, int nargs)
{
    if (nargs < 1) return PB_RESULT_BAD_ARGS;
    MONEY += args[0];
    return MONEY;                      /* what Ps1.Result(id) gets */
}

static const PortBridgeVar kVars[] = {
    /* name     address  type    count stride help */
    { "money",  &MONEY,  PB_S32, 1,    0,     "bits" },
};
static const PortBridgeRequest kRequests[] = {
    { "add_money", add_money, "n: add bits; returns the new amount" },
};

void mygame_bridge_tick(void)          /* called once per frame by a patch */
{
    static int published;
    if (!published)
    {
        published = 1;
        port_bridge_init(kVars, 1, kRequests, 1);
    }
    port_bridge_pump();                /* runs the queued requests */
}
```

- **Types:** `PB_U8`, `PB_S8`, `PB_U16`, `PB_S16`, `PB_U32`, `PB_S32`, `PB_STR`.
- **Arrays:** `count` elements, `stride` bytes apart (0 = packed). A table of structs is an
  array with `stride = sizeof(struct)` pointing at one field of element 0.
- **Text:** `PB_STR` with `count` strings of `stride` bytes each. With `stride` 0 it is
  one string of `count` bytes.
- **Derived values:** publish a static variable your tick keeps up to date (for example
  a "how close to evolving" score). Reads see whatever it held at the last frame.
- **Writing:** every variable can be written with `Ps1.Set`, which runs as the built-in
  request `set <name>` in the game's pump.
- **Results:** return `>= 0` for success and negative values your help text documents.
  The bridge itself returns `PB_RESULT_UNKNOWN` (-1000) for unknown names and
  `PB_RESULT_BAD_ARGS` (-1001) for bad arguments.
- **Safety:** a handler runs on the game thread between frames, but it is up to you to
  check that the game is in a state where the action makes sense (not in a cutscene,
  menu or battle), and return a "busy" code otherwise.

Call the pump from a loop that runs while the game should accept requests (usually the
field/overworld loop). While another loop runs (a battle, a menu with its own loop),
requests wait in the queue; reads keep working.

### Engine side: Lua

```lua
Ps1.IsRunning()                 -- true while a game runs in-process
Ps1.Get(name [, index])         -- number, or string for text; nil if unavailable
Ps1.Set(name, value [, index])  -- queues a write; returns a request id (or nil)
Ps1.Request(name, ...)          -- queues a request with integer arguments; id or nil
Ps1.Result(id)                  -- result once the game ran it, else nil
Ps1.Variables()                 -- { {name=, type=, count=, help=}, ... }
Ps1.Requests()                  -- { {name=, help=}, ... }
```

A button that warps and reports back:

```lua
function FastTravel:OnClick()
    self.pending = Ps1.Request("warp", self.map, self.exit)
end

function FastTravel:Tick(dt)
    if self.pending then
        local r = Ps1.Result(self.pending)
        if r ~= nil then
            self.pending = nil
            if r < 0 then Log.Debug("can't travel right now (" .. r .. ")") end
        end
    end
end
```

`Ps1.Variables()` and `Ps1.Requests()` list what the running game published, with help
text, so a debug panel can be generic.

### Engine side: UI without code

`com.recomp.ps1` adds widget types bound to the bridge: `Ps1Text` (format with
`{variable}` tokens), `Ps1Toggle`, `Ps1Button` (request or step a variable) and
`Ps1Bar`. Their bindings are inspector properties, so a UI made of them needs no
script. See `Source/Ps1Widgets.h`; the Digimon World guide has the token syntax and the
ready-made UIs (Tools > Recomp > Digimon), which are built from them by
`Source/Ps1GameUIs.cpp`. A new game adds its own UI tools there.

### Engine side: C++

Native code (other addons, nodes) uses the same functions directly, from
`Packages/com.recomp.ps1/Source/Ps1GuestHost.h`:
`BridgeVariables()`, `BridgeRequests()`, `BridgeGet(name, index, value)`,
`BridgeGetString(name, index, text)`, `BridgeRequest(name, args)` and
`BridgeResult(id, result)`. Call them from the main thread.
