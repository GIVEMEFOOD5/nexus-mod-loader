# Endless Sky code mods (mod API version 1)

This fork can run extra code alongside the game. Code mods are WebAssembly (`.wasm`)
modules, so they can be written in any language that compiles to WebAssembly, and
the same file works on Windows, macOS and Linux. The game itself is unchanged: with
no mods installed, it plays exactly like upstream Endless Sky.

Data-only plugins (ships, outfits, missions in the usual text format) keep working
as normal and don't need anything here.

## Installing a mod

Put each mod in its own folder inside the `mods` folder of your Endless Sky config
directory (the same place as `saves/` and `plugins/`):

```
<config>/mods/
    hello-c/
        mod.toml
        mod.wasm
    load order.txt      (optional)
```

`load order.txt` lists the mod folders to run, one per line, top first. Lines
starting with `#` are comments. Mods that are present but not listed are disabled.
Without this file, every mod runs, in alphabetical order.

Useful command-line flags:

| Flag | What it does |
|------|--------------|
| `--mods` | List installed mods, their status and warnings, then exit. |
| `--no-mods` | Start the game without running any code mods. |
| `--mod-dev` | Reload a mod whenever its `.wasm` file changes, without restarting. |

Mod output goes to the game's log (`errors.txt` in the config folder) and, for
messages, to the in-game message log.

## Safety

Each mod runs in its own sandbox:

- No file, network, clock or system access. The only functions a mod can call are
  the `es` functions listed below. A module that imports anything else (for example
  WASI) is refused at load time.
- Each call has an instruction budget (about 2 million per frame by default; a mod
  can ask for up to 20 million with `fuel_per_frame`). A mod that runs past it is
  stopped.
- Memory is capped at 64 MB per mod.
- If a mod crashes or runs out of budget, only that mod is stopped, for the rest of
  the session, and the player is told why.
- Changing game state needs a permission declared in `mod.toml`.

## Writing a mod

### mod.toml

```toml
name = "hello-c"
version = "1.0.0"
authors = ["Your Name"]
description = "What it does."
runtime = "wasm"            # "data" mods are loaded as plugins instead
entry = "mod.wasm"
api_version = 1
game_versions = ["0.11"]
hooks = ["init", "data_loaded", "frame", "event"]
permissions = []            # e.g. ["conditions-write"]
dependencies = []           # mod names that must run earlier in the load order
conflicts = []              # mod names that can't run alongside this one
# fuel_per_frame = 5000000
# ai_generated = { tool = "...", scope = ["code"], human_reviewed = true }
```

If any part of the mod was made with AI, fill in `ai_generated`. It is shown to
players as a warning, so be accurate.

### Hooks (exports from your module)

All are optional except `es_mod_api_version`, and `es_scratch` if you use events.
The C SDK's `ES_MOD_SETUP();` defines both for you.

| Export | Signature | When |
|--------|-----------|------|
| `es_mod_api_version` | `() -> i32` | Load time. Must return 1. |
| `es_scratch` | `(size: i32) -> i32` | Returns a buffer the game copies event text into. |
| `es_mod_init` | `() -> i32` | After loading. Return 0, or the mod is stopped. |
| `es_on_data_loaded` | `()` | Game data and the most recent pilot have loaded. |
| `es_on_frame` | `(frame: i64)` | Every game step in flight (60 per second). |
| `es_on_event` | `(kind: i32, ptr: i32, len: i32)` | Something happened (table below). |
| `es_mod_shutdown` | `()` | The game is closing, or the mod is being reloaded. |

Event kinds: 1 enter system (system name), 2 land (planet name; also sent when a saved
game is loaded on a planet), 3 take off (planet
name), 4 day passed (the new date). Ignore kinds you don't recognise; new ones may
be added within API version 1.

### Game functions (imports from module `es`)

| Import | Signature | Notes |
|--------|-----------|-------|
| `log` | `(level, ptr, len)` | 0 info, 1 warning, 2 error. Up to 32 per call. |
| `message` | `(ptr, len)` | In-game message. Up to 4 per call. |
| `has_permission` | `(ptr, len) -> i32` | 1 if the manifest grants it. |
| `condition_get` | `(ptr, len) -> i64` | Any condition, e.g. `credits`, `net worth`. |
| `condition_set` | `(ptr, len, value: i64) -> i32` | Needs `conditions-write`. 1 ok, 0 no pilot, -1 no permission. |
| `query` | `(kptr, klen, out, cap) -> i32` | Returns the length, or -1 for an unknown key. |

Query keys: `game.date`, `game.version`, `api.version`, `player.name`,
`player.system`, `player.planet`, `player.credits`, `flagship.name`, `flagship.model`.

Strings are UTF-8, passed as pointer and length into the module's exported `memory`.

### C and C++

```sh
clang --target=wasm32 -O2 -nostdlib -fno-builtin -Wl,--no-entry -I mods-sdk/c -o mod.wasm mod.c
```

See `examples/hello-c` for a complete mod and build script. C++ works the same way
with `clang++ ... -fno-builtin -fno-exceptions -fno-rtti`. There is no C library in the sandbox;
`es_mod.h` includes a small string builder instead of `printf`.

### Other languages

Any language that produces a plain WebAssembly module (no WASI) can be used, as long
as it exports the functions above and imports from module `es`. Templates for Rust,
Zig and AssemblyScript are planned.

## Testing a mod without the game

`tools/modcheck` loads a mod exactly as the game does, runs its hooks against a fake
game state and reports timings and any faults:

```sh
cmake -S mods-sdk/tools -B build-modcheck && cmake --build build-modcheck
build-modcheck/modcheck mods-sdk/examples/hello-c 3600
```

`tests/run_tests.sh <modcheck>` builds a set of deliberately broken mods and checks
that the sandbox stops each one.
