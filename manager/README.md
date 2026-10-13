# Nexus Mod Manager

A desktop app for choosing, testing and running Endless Sky code mods. It works with
the mod loader in this repository (`source/mod/`) and reads the same mods folder and
`load order.txt` the game uses, so anything you do here is exactly what the game runs.

## What it does

- **Mods**: every mod in the mods folder. Mods that run sit on the route, in load
  order: drag to reorder, or select one and press Alt+Up / Alt+Down. Turned-off mods
  sit below. Warnings are always shown: AI-made, not sandboxed, permissions,
  unverified, compatibility, slow, untested and needs a rebuild.
- **Add mod**: from a folder, a zip, or a GitHub repository, or link a folder you're
  developing so edits are picked up straight away. New mods arrive turned off.
  Replacing a mod keeps the old version, so **Roll back** can restore it.
- **Permissions**: a mod that asks for permissions can't be turned on until you
  approve them, and you're asked again if a new version wants more.
- **New mod**: creates a mod from a C template, builds it and turns it on.
- **Build** and **Test**: build with the mod's `build.sh`, and test it for a minute of
  simulated flight outside the game. The test catches crashes, runaway code and slow
  mods, and the result is kept until the mod is rebuilt.
- **Launch game**: starts the game, optionally with live reload, and shows each mod's
  output in the **Console**, filterable by mod and by level.
- **Profiles**: save which mods run and in what order, and switch between sets.

## Running it on Fedora

You need Rust 1.90 or newer, Node.js, and the WebKitGTK libraries:

    sudo dnf install rust cargo nodejs nodejs-npm webkit2gtk4.1-devel openssl-devel libappindicator-gtk3-devel librsvg2-devel
    sudo dnf group install c-development

Then, from this folder:

    npm install
    npm run dev        # run it while you work on it
    npm run build      # make an installable .rpm, .deb and AppImage

The installers end up in `src-tauri/target/release/bundle/`.

To run the automated tests: `cd src-tauri && cargo test` (add `-- --ignored` to also
test downloading from GitHub, which needs internet access).

## First run

Open **Settings** and check three things:

1. **Game source folder** is your `nexus-mod-loader` checkout, and it says the game was found.
2. **Mods folder** is `~/.local/share/endless-sky/mods` (the default on Linux).
3. Under **Tools**, press **Build test tool** once. It needs CMake and a C++ compiler,
   like the game itself.

Building and creating mods also needs `clang` and `lld` (`sudo dnf install clang lld`).

Settings, profiles, approved permissions and test results are stored in
`~/.config/nexus-mod-manager/data.json`. Backups of replaced or removed mods are kept
in `~/.config/nexus-mod-manager/backups/`.
