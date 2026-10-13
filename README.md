# Nexus Mod Loader for Endless Sky

Run your own code inside [Endless Sky](https://github.com/endless-sky/endless-sky), safely.

This is a fork of Endless Sky v0.11.2 that adds **code mods**: small programs, compiled
to WebAssembly, that the game loads alongside its normal data. Mods can react to what
happens in the game, read information about your pilot and ship, show messages, and
(with permission) change game conditions. Every mod runs in a sandbox with limits on
time and memory, so a broken or hostile mod can't freeze the game or touch your files.

The project has three parts:

| Part | Folder | What it is |
|------|--------|------------|
| **Mod loader** | `source/mod/` | The changes to the game that find, sandbox and run mods. |
| **Mod SDK** | `mods-sdk/` | A C/C++ header, an example mod, a test tool and test mods for writing your own. |
| **Nexus Mod Manager** | `manager/` | A desktop app for choosing, ordering, testing and launching mods. |

With no mods installed, the game plays exactly like Endless Sky v0.11.2. Existing saves,
plugins and data mods keep working unchanged.

> **Status: early and experimental.** It works and is tested, but only on Linux so far,
> and the mod API is small. Read [Limitations](#limitations) before relying on it.

---

## Contents

- [Quick start (Fedora)](#quick-start-fedora)
- [How it works](#how-it-works)
- [Installing and managing mods](#installing-and-managing-mods)
- [The Nexus Mod Manager](#the-nexus-mod-manager)
- [Writing a mod](#writing-a-mod)
- [Mod API reference](#mod-api-reference)
- [mod.toml reference](#modtoml-reference)
- [Safety model](#safety-model)
- [Limitations](#limitations)
- [Building on other systems](#building-on-other-systems)
- [Troubleshooting](#troubleshooting)
- [Repository layout](#repository-layout)
- [Roadmap](#roadmap)
- [Credits and licence](#credits-and-licence)

---

## Quick start (Fedora)

### 1. Install what's needed

```sh
sudo dnf install git gcc-c++ cmake ninja-build SDL2-devel libpng-devel libjpeg-turbo-devel \
    libavif-devel mesa-libGL-devel glew-devel minizip-devel openal-soft-devel libmad-devel \
    flac-devel libuuid-devel clang lld
```

For the manager app, you also need:

```sh
sudo dnf install rust cargo nodejs nodejs-npm webkit2gtk4.1-devel openssl-devel \
    libappindicator-gtk3-devel librsvg2-devel
```

### 2. Build the game

```sh
git clone https://github.com/<your-username>/nexus-mod-loader.git
cd nexus-mod-loader
git checkout nexus-mod-loader
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build
```

The first build downloads the WebAssembly runtime (Wasmtime) automatically, so it needs
an internet connection. Check it worked:

```sh
./build/endless-sky --version     # Endless Sky ver. 0.11.2
./build/endless-sky --mods        # lists installed code mods
```

### 3. Start the manager

```sh
cd manager
npm install
npm run dev
```

The first start compiles for a few minutes. Then:

1. Open **Settings**. Check that the game and the Mod SDK are found, and press **Build test tool**.
2. On **Mods**, press **New mod** and give it a name. It's created, built and turned on.
3. Press **Test**. It should pass.
4. Tick **Live reload** and press **Launch game**. Load a pilot and jump to another
   system: your mod says "Arrived in …" in the game and in the manager's **Console**.

---

## How it works

### The big picture

```
 mods folder                        Endless Sky
 ─────────────                      ──────────────────────────────────────────
 load order.txt  ──┐                ModManager (source/mod/ModManager.cpp)
 my-mod/           │   at startup     • reads load order.txt and each mod.toml
   mod.toml      ──┼──────────────▶   • checks API version, dependencies, conflicts
   mod.wasm      ──┘                  • loads each .wasm into its own sandbox
                                      │
                                      │  game hooks: data loaded, every frame,
                                      │  enter system, land, take off, day passed
                                      ▼
                                    WasmMod (source/mod/WasmMod.cpp)
                                      • one Wasmtime store per mod
                                      • instruction budget per call ("fuel")
                                      • 64 MB memory cap
                                      • only the "es" functions are importable
                                      │
                                      ▼
                                    GameHost: log, message, conditions, queries
```

### Startup

When the game starts, `ModManager::Init` runs right after the game sets up its folders,
before any game data loads. It:

1. Looks in the `mods` folder of your Endless Sky config folder for subfolders that contain a `mod.toml`.
2. Reads `load order.txt` to decide which mods run and in what order. If there's no such
   file, every mod found runs, in alphabetical order.
3. For each enabled mod, in order:
   - parses `mod.toml` and skips the mod if it's invalid;
   - skips mods whose runtime isn't `wasm` (`data` mods are loaded as normal plugins
     instead; `lua` and `external` aren't supported yet);
   - skips the mod if a dependency isn't already running earlier in the order, or a
     conflicting mod is already running;
   - compiles the `.wasm` file and instantiates it in a fresh sandbox;
   - checks that the module's `es_mod_api_version()` and the manifest's `api_version`
     both match the game's mod API version (currently **1**);
   - calls `es_mod_init()`.

A mod that fails any step is skipped, with the reason written to the game's log
(`errors.txt`) and shown by `--mods`. The rest of the mods and the game carry on.

### While the game runs

The game calls into mods at these points:

| Hook | When | Where in the engine |
|------|------|---------------------|
| `es_on_data_loaded` | Game data and the most recent pilot have finished loading. | `GameLoadingPanel.cpp` |
| `es_on_frame` | Every game step while flying and not paused (60 per second). | `Engine::Step` |
| `es_on_event` (enter system) | You arrive in a system. | `Engine::EnterSystem` |
| `es_on_event` (land) | You land on a planet, or load a save that starts on one. | `PlayerInfo::Land` |
| `es_on_event` (take off) | You take off. | `PlayerInfo::TakeOff` |
| `es_on_event` (day passed) | The in-game date advances. | `PlayerInfo::AdvanceDate` |
| `es_mod_shutdown` | The game is closing, or the mod is being reloaded. | `main.cpp` |

All calls into mods happen on the game's main thread, while the engine's calculation
thread is paused. Some events (entering a system, days passing) happen on the
calculation thread; those are queued and delivered at the start of the next frame.

### When a mod misbehaves

If a mod crashes (a WebAssembly trap), runs past its instruction budget, or passes a bad
pointer to a game function, that mod is **stopped for the rest of the session**. The
game shows a message in the in-game log saying which mod stopped and why, and writes it
to `errors.txt`. Other mods and the game itself keep running.

If a mod averages more than 1 ms per frame over its first 600 frames, a "slow mod"
warning is logged.

### Live reload

Started with `--mod-dev`, the game checks about once a second whether any running mod's
`.wasm` file has changed. If it has, that mod is shut down, reloaded from disk, and
re-initialised, and the in-game log says "Reloaded mod …". The mod's in-memory state is
lost on reload.

### What changed in the game's own code

Apart from the new `source/mod/` folder, the fork changes six existing files, adding
about 45 lines. No gameplay code is altered:

| File | Change |
|------|--------|
| `CMakeLists.txt` | Includes `utils/wasmtime.cmake` and links Wasmtime. |
| `source/CMakeLists.txt` | Adds the `source/mod/` files. |
| `source/main.cpp` | Starts and stops mods; adds `--mods`, `--no-mods` and `--mod-dev`. |
| `source/GameLoadingPanel.cpp` | Calls the "data loaded" hook. |
| `source/Engine.cpp` | Calls the frame hook and the "enter system" event. |
| `source/PlayerInfo.cpp` | Sends the land, take-off and day-passed events. |

---

## Installing and managing mods

### Where mods go

Each mod is a folder inside the `mods` folder of your Endless Sky config folder:

| System | Mods folder |
|--------|-------------|
| Linux | `~/.local/share/endless-sky/mods/` |
| Windows | `%APPDATA%\endless-sky\mods\` |
| macOS | `~/Library/Application Support/endless-sky/mods/` |

`./build/endless-sky --mods` prints the exact path on your computer. The game creates the
folder if it's missing.

```
mods/
├── load order.txt        optional: which mods run, top first
├── hello-c/
│   ├── mod.toml          required
│   └── mod.wasm          required (or whatever "entry" names)
└── another-mod/
    ├── mod.toml
    └── mod.wasm
```

The mod folder must sit **directly** inside `mods/`. Unzipping often creates an extra
level (`mods/hello-c/hello-c/mod.toml`), which the game won't find.

### load order.txt

```
# Lines starting with # are comments.
hello-c
another-mod
```

- Mods listed run, in this order (top first).
- Mods in the folder but not listed are turned off.
- With no `load order.txt`, every mod in the folder runs, alphabetically.
- Names are **folder names**, not the `name` in `mod.toml`.

The manager writes this file for you.

### Command-line options

| Option | Effect |
|--------|--------|
| `--mods` | List installed mods with their status, reasons and warnings, then exit. |
| `--no-mods` | Start the game without running any code mods. |
| `--mod-dev` | Reload a mod whenever its `.wasm` file changes. |

The `--mods` listing shows each mod as `running`, `disabled`, `not loaded` (with the
reason), `faulted`, or `data only`.

---

## The Nexus Mod Manager

A desktop app (Tauri: Rust plus a web interface) in `manager/`. It reads and writes the
same mods folder and `load order.txt` the game uses, so what you see is what the game runs.

### Mods screen

- **The route.** Mods that run are drawn as stops on a route, like the galaxy map, in
  load order. Drag to reorder, or select a mod and press Alt+Up / Alt+Down. Turned-off
  mods are listed underneath.
- **Warnings, always shown.** Each mod lists everything worth knowing:

| Warning | Meaning |
|---------|---------|
| AI-made | The author says parts were made with AI: which parts, which tool, and whether a person reviewed it. |
| Not sandboxed | An "external" mod that would run as a normal program. The game can't run these yet. |
| Asks for permissions / Not approved | The mod wants extra abilities. You approve them before turning it on. |
| Unverified | No checksum or signature, or the file no longer matches its checksum. |
| Won't run | Broken `mod.toml`, wrong API version, missing dependency, conflict or missing `.wasm`. |
| Not built / Rebuild | No `.wasm` yet, or the source changed since the last build. |
| Untested / Failed test / Slow | Results of the manager's test for the current build. |

- **Details panel.** Select a mod to see its description, authors, warnings and last
  test result, and to use **Turn on/off**, **Build**, **Test**, **Open in editor**,
  **Show folder**, **Roll back** and **Remove**.

### Adding mods

**Add mod** offers four sources:

- **From a folder:** copies the mod in.
- **Link a folder you're working on:** creates a link, so your edits are picked up
  directly. Removing a linked mod only removes the link, never your files.
- **From a zip file:** finds every mod inside the zip (up to three folders deep).
- **From GitHub:** downloads the repository with `git` and adds every mod in it.

New mods arrive **turned off**. If a mod is already installed, you're asked before it's
replaced, and the old version is kept so **Roll back** can restore it. Removed mods are
kept in the backups too.

### Permissions

A mod that asks for permissions can't be turned on in the manager until you approve
them, and you're asked again if a new version asks for more. If a mod with unapproved
permissions was turned on some other way (by editing `load order.txt`), the manager flags
it as "Running without your approval". See [Safety model](#safety-model) for what the
game itself enforces.

### New mod, Build and Test

- **New mod** creates a folder with `mod.toml`, a C source file using the SDK, and a
  `build.sh`, then builds it and turns it on.
- **Build** runs the mod's `build.sh`, pointing it at the SDK in your game folder.
- **Test** runs the mod in the SDK's `modcheck` tool for 3,600 frames (one minute of
  flight) against a pretend game, then records whether it passed, plus its average and
  peak time per frame. The result is kept until the mod is rebuilt.

### Console, Profiles and Settings

- **Launch game** starts the game (optionally with **Live reload**) and streams its
  output to the **Console**, where you can filter by mod, by warnings and errors, or by
  text.
- **Profiles** save which mods run and in what order, so you can switch sets in one click.
- **Settings** set the game source folder, the game program, the mods folder and your
  editor command. They also build the test tool and run **Check with the game**
  (`--mods`).

The manager keeps its own data in `~/.config/nexus-mod-manager/`: `data.json` holds
settings, profiles, approvals and test results, and `backups/` holds replaced and
removed mods.

### Building an installable app

```sh
cd manager
npm run build
```

Installers (`.rpm`, `.deb`, AppImage on Linux) appear in `src-tauri/target/release/bundle/`.
The manager's own tests run with `cd manager/src-tauri && cargo test` (add
`-- --ignored` to include the GitHub download test).

---

## Writing a mod

### Your first mod

The easiest start is **New mod** in the manager. By hand:

```sh
cp -r mods-sdk/examples/hello-c ~/.local/share/endless-sky/mods/my-mod
cd ~/.local/share/endless-sky/mods/my-mod
# edit mod.toml: change name = "hello-c" to name = "my-mod"
ES_MOD_SDK=~/Documents/GitHub/nexus-mod-loader/mods-sdk/c ./build.sh
```

### A minimal mod in C

```c
#include "es_mod.h"

ES_MOD_SETUP();   // required, once, at file scope

ES_EXPORT(es_on_event) void es_on_event(int kind, const char *text, int len)
{
	if(kind != ES_EVENT_ENTER_SYSTEM)
		return;
	char buffer[256];
	es_str s = es_str_init(buffer, sizeof(buffer));
	es_str_add(&s, "Arrived in ");
	es_str_add_n(&s, text, len);
	es_message(buffer);
}
```

### Building

```sh
clang --target=wasm32 -O2 -nostdlib -fno-builtin -Wl,--no-entry -I path/to/mods-sdk/c -o mod.wasm mod.c
```

- `--target=wasm32 -nostdlib`: plain WebAssembly with no C library. The sandbox
  doesn't provide one.
- `-fno-builtin`: stops clang turning loops into C library calls. clang 21, Fedora's
  version, does this.
- `-Wl,--no-entry`: a mod has no `main`.

C++ works the same way with `clang++` plus `-fno-exceptions -fno-rtti`.

### Rules of the sandbox

- **No C library.** No `printf`, `malloc`, files or time. `es_mod.h` provides a small
  string builder (`es_str`) and basic `strlen`, `memcpy`, `memset`, `memmove` and
  `memcmp`. Use fixed-size buffers or static arrays.
- **Stay within budget.** `es_on_frame` runs 60 times a second with about 2 million
  instructions per call by default. Do expensive work occasionally (e.g. every 60th frame).
- **State lives in memory.** Global variables persist while the game runs, but reset
  when the game restarts or the mod reloads. To keep something in a save, store it in a
  game condition (needs the `conditions-write` permission).

### Testing outside the game

`modcheck` runs a mod exactly as the game does (same loader, sandbox and limits) against
a pretend game state, and reports what it did:

```sh
cmake -S mods-sdk/tools -B build-modcheck && cmake --build build-modcheck
./build-modcheck/modcheck path/to/my-mod 3600
```

`mods-sdk/tests/run_tests.sh ./build-modcheck/modcheck` builds nine deliberately broken
or unusual mods and checks the sandbox handles each correctly: infinite loop, crash,
memory hog, bad pointer, file-system access attempt, wrong API version, permission
denied, permission granted, and event delivery.

---

## Mod API reference

Mod API **version 1**. Mods export functions the game calls, and import functions from
the WebAssembly module `es`. Strings are UTF-8, passed as a pointer and length into the
mod's exported `memory`.

### Exports: functions your mod provides

| Export | Signature | Required | Notes |
|--------|-----------|----------|-------|
| `es_mod_api_version` | `() -> i32` | Yes | Must return 1. `ES_MOD_SETUP()` defines it. |
| `es_scratch` | `(size: i32) -> i32` | For events | Returns a buffer for event text. `ES_MOD_SETUP()` defines it. |
| `es_mod_init` | `() -> i32` | No | Return 0 for success, anything else stops the mod. |
| `es_on_data_loaded` | `()` | No | Game data and pilot loaded. |
| `es_on_frame` | `(frame: i64)` | No | Each step in flight. `frame` counts from 1 per session. |
| `es_on_event` | `(kind: i32, ptr: i32, len: i32)` | No | See event kinds below. |
| `es_mod_shutdown` | `()` | No | Game closing or mod reloading. |

### Event kinds

| Kind | C constant | Text passed |
|------|-----------|-------------|
| 1 | `ES_EVENT_ENTER_SYSTEM` | System name |
| 2 | `ES_EVENT_LAND` | Planet name (also sent when a save is loaded on a planet) |
| 3 | `ES_EVENT_TAKE_OFF` | Planet name |
| 4 | `ES_EVENT_DAY_PASSED` | The new date, e.g. `Wed, 17 Nov 3013` |

New kinds may be added within API version 1, so ignore kinds you don't recognise.

### Imports: functions the game provides (module `es`)

| Import | Signature | What it does |
|--------|-----------|--------------|
| `log` | `(level, ptr, len)` | Writes to the game log. Level 0 info, 1 warning, 2 error. Up to 32 per call. |
| `message` | `(ptr, len)` | Shows a line in the in-game message log. Up to 4 per call. |
| `has_permission` | `(ptr, len) -> i32` | 1 if the mod's manifest grants that permission. |
| `condition_get` | `(ptr, len) -> i64` | Reads a condition (e.g. `credits`, `net worth`, mission flags). Unknown names read as 0. |
| `condition_set` | `(ptr, len, value: i64) -> i32` | Writes a condition. Needs `conditions-write`. Returns 1 if set, 0 if no pilot is loaded, -1 without permission. |
| `query` | `(key_ptr, key_len, out_ptr, out_cap) -> i32` | Reads a text value. Returns its length (copying it if it fits), or -1 for an unknown key. |

The C header wraps these as `es_log`, `es_info`, `es_message`, `es_has_permission`,
`es_condition_get`, `es_condition_set` and `es_query` for ordinary C strings.

### Query keys

| Key | Value |
|-----|-------|
| `api.version` | `1` |
| `game.version` | The running game version |
| `game.date` | The in-game date |
| `player.name` | Pilot's first and last name |
| `player.system` | Current system |
| `player.planet` | Current planet, or empty in space |
| `player.credits` | Credits, as text |
| `flagship.name` | Flagship's name |
| `flagship.model` | Flagship's ship model |

Before a pilot is loaded, `player.*`, `flagship.*` and `game.date` return empty text, and
`condition_get` reads the game's global conditions.

---

## mod.toml reference

```toml
name = "trade-helper"            # shown to players; defaults to the folder name
version = "1.0.0"
authors = ["Your Name"]
description = "What the mod does, in a sentence."
homepage = "https://github.com/you/trade-helper"

runtime = "wasm"                 # wasm (default) | data | lua | external
entry = "mod.wasm"               # the module file, relative to this folder
api_version = 1                  # required for wasm mods
game_versions = ["0.11"]         # informational for now
hooks = ["init", "data_loaded", "frame", "event"]   # informational for now

permissions = ["conditions-write"]
dependencies = ["other-mod"]     # must be running, earlier in the load order
conflicts = ["rival-mod"]        # this mod is skipped if one of these is running

fuel_per_frame = 5000000         # optional: up to 20,000,000; default 2,000,000
checksum = "sha256:…"            # optional: SHA-256 of the .wasm, checked by the manager

ai_generated = { tool = "…", scope = ["code"], human_reviewed = true }
```

- Keys may also sit under a `[mod]` section, and `api-version` style spellings work too.
- `dependencies` and `conflicts` use the other mod's **`name`**, not its folder.
- The game itself only understands a simple subset of TOML: strings, integers, true/false,
  one-line or multi-line arrays, `[sections]` and one-level inline tables.

### Permissions

| Permission | Enforced by the game? | Meaning |
|------------|----------------------|---------|
| `conditions-write` | **Yes** | Lets `condition_set` change conditions, which can affect missions, events and your save. |
| `network`, `file-read`, `file-write`, `mods-data` | Not applicable | Reserved names. Mods can't do any of these in this version, with or without the permission. |

---

## Safety model

**What a mod can't do**, however it's written:

- Read or write files, open network connections, start programs or read the clock.
  WebAssembly mods have no access to the operating system: the game gives them only the
  six `es` functions, and a module asking for anything else (including WASI) fails to load.
- Run forever. Every call has an instruction budget ("fuel"); running out stops the mod.
- Use unlimited memory. Each mod is capped at 64 MB.
- Read or corrupt the game's memory or other mods' memory. Each mod has its own isolated
  store, and every pointer it passes is bounds-checked.
- Spam the player. Messages and log lines per call are capped.

**What a mod can do:** read the information in the [API reference](#mod-api-reference),
show messages, and change game conditions if its manifest asks for `conditions-write`.
Changing conditions can affect missions and is saved with your pilot, so only allow it
for mods you trust, and keep backups of saves you care about.

**Trust is based on the mod's own word.** The AI-made disclosure, the author and the
permission list all come from `mod.toml`, written by the mod's author. See the
limitations below.

---

## Limitations

This is an early version. Known limitations, grouped by area:

### What mods can do

- **No ship, AI or combat control yet.** Mods can't steer ships, choose targets, fire
  weapons, or react to combat (hits, damage, ships destroyed or boarded). These hooks
  from the project plan aren't implemented.
- **A small window into the game.** Mods can read only the query keys listed above plus
  conditions. They can't list ships, outfits, cargo, missions, systems or planets, and
  can't change anything except conditions.
- **No data-stage hooks.** Mods can't create or patch game data (ships, outfits,
  missions) in code. Use normal data plugins for that.
- **No drawing or UI.** Mods can't draw on screen or add panels; they can only add
  lines to the message log.
- **No input.** Mods can't read keys or the mouse.
- **No persistent storage except conditions.** A mod's memory resets every time the
  game starts or the mod reloads.
- **No clock or randomness from the game.** Use the frame counter or in-game date.

### Events and timing

- **Mods start once, when the game starts.** Loading a different pilot from the menu
  doesn't re-run `es_on_data_loaded`; the land event (sent on load) is the only signal.
- **"Land" is also sent when a save is loaded** on a planet, so a mod can't tell a real
  landing from loading a game.
- **Some events arrive one frame late** (entering a system, days passing), because they
  happen on the engine's calculation thread.
- **`es_on_frame` only runs while flying and not paused.** It doesn't run while landed,
  in menus or paused.
- **The frame counter is per session**, not game time, and isn't saved.
- **Fuel counts instructions, not time.** The budget catches runaway loops, but it's not
  a precise time limit. Time spent inside game functions isn't counted.
- **Runs aren't guaranteed to be identical.** Mods are deterministic, but the game
  around them isn't necessarily, so the same actions may not produce the same mod output.

### Runtimes and languages

- **Only WebAssembly mods run.** The `lua` and `external` runtimes from the plan aren't
  implemented; such mods are listed but skipped.
- **Only C and C++ are tested.** Other languages that compile to plain WebAssembly
  (Rust, Zig, AssemblyScript…) should work if they export the right functions without
  WASI, but there are no templates or examples yet.
- **Mods are built with clang.** There's no build support for other toolchains in the
  manager, which runs each mod's `build.sh`.
- **One mod API version.** Mods for a future API version won't load on this build, and
  the API may change before it's stable.

### Safety and trust

- **Permission approval lives in the manager, not the game.** The game grants a mod
  whatever permissions its own `mod.toml` lists. Approval is checked only when you turn a
  mod on in the manager, so a mod turned on by editing `load order.txt` gets its declared
  permissions without approval. The manager flags this as "Running without your approval".
- **No signatures.** There's no way yet to prove who made a mod. The `checksum` field
  is only compared by the manager, and almost every mod shows as **Unverified**.
- **AI disclosure is self-reported.** The "AI-made" warning appears only if the author
  fills in `ai_generated`; the software can't detect AI-made code.
- **The sandbox is only as strong as Wasmtime and the loader.** Wasmtime is a mature,
  widely used runtime, but the loader around it is new and hasn't had a security review.
- **Mods can't hurt your computer, but they can affect your save** through conditions
  if allowed. Back up saves you care about.

### Saves

- **Saves don't record which mods were used.** Loading a save made with different mods
  gives no warning.
- **Removing a mod leaves its conditions** in your save.

### The manager

- **Tests use a pretend game.** **Test** runs a mod in `modcheck` against fixed sample
  data (a pilot in the Rutilicus system, for example), not in the real game. It catches
  crashes, runaway loops and slow code, but not gameplay mistakes.
- **No full headless test runner yet.** Battle, mission and data-load scenarios in the
  real game aren't available from the manager.
- **Building and Test need the game source folder,** for the SDK and the test tool.
  The manager doesn't yet work with an installed game alone.
- **No Nexus catalogue or website.** Browsing and sharing mods online isn't built.
- **Linked mods on Windows** need Windows Developer Mode (for folder links), and
  `build.sh` needs a Unix-style shell such as Git Bash.
- **The mod list refreshes every five seconds** while the window has focus, plus when
  you return to it, so changes made outside the manager can take a moment to show.
- **Approvals and test results are keyed by folder name.** Renaming a mod's folder
  makes it look new to the manager.

### Platforms and building

- **Only tested on Linux (x86-64)**, on Ubuntu 24.04 and Fedora. Windows and macOS build
  settings are written but untested; Windows is the likeliest to hit problems.
- **Based on Endless Sky v0.11.2.** Newer upstream releases need merging by hand, and
  the patch may need small adjustments.
- **The first build needs internet access** to download Wasmtime 25.0.0. Offline, set
  `ES_WASMTIME_DIR` to an extracted Wasmtime C API folder.
- **No online or multiplayer features.** Endless Sky is single-player, and none of the
  planned online stages exist yet.

---

## Building on other systems

The patch doesn't change Endless Sky's normal build process; see
[`docs/readme-developer.md`](docs/readme-developer.md) for each platform's dependencies.

Notes for this fork:

- Add `-DBUILD_TESTING=OFF` unless you've installed Catch2 for the unit tests.
- In v0.11.2, the `linux-release` CMake preset is reserved for the official release
  servers. Use the plain commands from the quick start, or `cmake --preset linux` with
  `cmake --build --preset linux-release`.
- Wasmtime is downloaded for your platform: `x86_64` or `aarch64`, Linux, macOS, MinGW
  or MSVC. The version is set by `ES_WASMTIME_VERSION` in `utils/wasmtime.cmake`.
- The manager needs Rust 1.90 or newer, Node.js, and your system's web view libraries
  (WebKitGTK on Linux; built in on Windows and macOS).

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `Could not find a package configuration file provided by "Catch2"` | Add `-DBUILD_TESTING=OFF` to the first `cmake` command, after deleting `build/`. |
| `Could not use disabled preset "linux-release"` | Use the plain `cmake -S . -B build …` commands from the quick start. |
| `undefined symbol: strlen` (or `memcpy`, `memset`) when building a mod | Update `mods-sdk/c/es_mod.h` to the current version and add `-fno-builtin` to the build command. |
| `--mods` says "No mods found." | Check each mod folder sits directly inside `mods/`, with `mod.toml` at its top level. |
| A mod is `not loaded: … unknown import: wasi_snapshot_preview1…` | It was built for WASI. Build with `--target=wasm32 -nostdlib` instead of a WASI target. |
| A mod is `not loaded: built for mod API version …` | It was made for a different version of this mod API. |
| A mod shows `disabled: not in load order.txt` | Turn it on in the manager, or add its folder name to `load order.txt`. |
| `faulted: … ran past its time budget` | The mod did too much in one call. Spread the work over several frames, or raise `fuel_per_frame`. |
| The manager's **Test** says the test tool isn't built | **Settings → Build test tool**. Needs CMake and a C++ compiler. |
| The manager can't find the game | **Settings → Game source folder**: set it to your checkout, which must contain `build/endless-sky`. |
| Mod messages don't appear in game | Messages show while flying. Check the manager's Console or `errors.txt` for `[mod …]` lines. |

---

## Repository layout

```
nexus-mod-loader/
├── source/mod/                 the mod loader
│   ├── ModManager.cpp/.h       finds, orders and runs mods; the game's side of the API
│   ├── WasmMod.cpp/.h          one sandboxed mod on Wasmtime
│   ├── ModManifest.cpp/.h      reads mod.toml
│   └── ModHost.h               API version, event kinds, host interface
├── utils/wasmtime.cmake        downloads and links Wasmtime
├── mods-sdk/
│   ├── README.md               modding guide
│   ├── c/es_mod.h              C/C++ SDK header
│   ├── examples/hello-c/       example mod
│   ├── tools/modcheck.cpp      runs a mod outside the game
│   └── tests/                  sandbox test mods and run_tests.sh
├── manager/                    Nexus Mod Manager (Tauri)
│   ├── ui/                     interface (HTML, CSS, JavaScript)
│   └── src-tauri/src/          backend (Rust)
└── …                           Endless Sky v0.11.2
```

---

## Roadmap

Roughly in order:

1. **Gameplay hooks:** ship AI decisions (targets, weapons, movement, retreat), combat
   events, and spawn and mission triggers.
2. **More game data for mods:** ships, outfits, cargo, missions and the map.
3. **Saves record their mods,** with a warning when loading a save made with different mods.
4. **Permission approval enforced by the game,** plus signed mods.
5. **Headless test runner:** battle, mission and data-load scenarios in the real game,
   from the manager.
6. **Templates for Rust, Zig and AssemblyScript.**
7. **In-game Mods screen** for players who don't use the manager.
8. **Nexus catalogue and website** for sharing mods.
9. **Lua and external mods.**
10. **Online features,** starting with non-real-time ones.

---

## Credits and licence

- **Endless Sky** by Michael Zahniser and contributors,
  <https://github.com/endless-sky/endless-sky>. Licensed under the GNU GPL v3 or later.
- **Wasmtime** by the Bytecode Alliance, <https://github.com/bytecodealliance/wasmtime>.
  Apache License 2.0 with LLVM exception; downloaded at build time, not stored in this repository.
- **Nexus Mod Loader, SDK and Manager** by the Nexus Mod Platform contributors.

This fork, like Endless Sky, is free software under the
[GNU General Public License v3 or later](license.txt). You may share and change it, but
versions you distribute must stay under the same licence with their source available.

### AI-made disclosure

Following this project's own rule that AI-made work is disclosed: most of the mod
loader, SDK, manager and this documentation were written with Claude, an AI assistant
made by Anthropic. The code was built and tested, including in the real game, as
described above. It has not had an independent code or security review.
