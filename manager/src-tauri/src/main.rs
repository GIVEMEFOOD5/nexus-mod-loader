// Nexus Mod Manager: choose, test and run code mods for Endless Sky.
// Copyright (c) 2026 by the Nexus Mod Platform contributors. GPL v3 or later.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod install;
mod mods;
mod store;

use serde::Serialize;
use std::collections::BTreeMap;
use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use store::{Settings, Store, TestRecord};
use tauri::{AppHandle, Emitter, State};
use tauri_plugin_dialog::DialogExt;

struct AppState {
    store: Mutex<Store>,
    game: Arc<Mutex<Option<Child>>>,
}

#[derive(Serialize)]
struct Overview {
    settings: Settings,
    library: mods::Library,
    profiles: BTreeMap<String, Vec<String>>,
    game_exe: Option<String>,
    sdk_ready: bool,
    modcheck_ready: bool,
    game_running: bool,
    api_version: i64,
}

#[derive(Serialize)]
struct RunResult {
    ok: bool,
    output: String,
}

#[derive(Serialize, Clone)]
struct LogLine {
    line: String,
    stream: String,
}

fn mods_dir(state: &State<AppState>) -> PathBuf {
    PathBuf::from(&state.store.lock().unwrap().settings.mods_dir)
}

/// Run a command to completion, with a time limit, collecting stdout and stderr together.
fn run(mut command: Command, timeout: Duration) -> RunResult {
    command.stdout(Stdio::piped()).stderr(Stdio::piped()).stdin(Stdio::null());
    let mut child = match command.spawn() {
        Ok(c) => c,
        Err(e) => return RunResult { ok: false, output: format!("Could not start {:?}: {e}", command.get_program()) },
    };
    let mut out = child.stdout.take().unwrap();
    let mut err = child.stderr.take().unwrap();
    let out_thread = thread::spawn(move || {
        let mut s = String::new();
        let _ = out.read_to_string(&mut s);
        s
    });
    let err_thread = thread::spawn(move || {
        let mut s = String::new();
        let _ = err.read_to_string(&mut s);
        s
    });
    let start = Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break Some(status),
            Ok(None) if start.elapsed() > timeout => {
                let _ = child.kill();
                let _ = child.wait();
                break None;
            }
            Ok(None) => thread::sleep(Duration::from_millis(50)),
            Err(_) => break None,
        }
    };
    let mut output = out_thread.join().unwrap_or_default();
    let errors = err_thread.join().unwrap_or_default();
    if !errors.trim().is_empty() {
        if !output.is_empty() && !output.ends_with('\n') {
            output.push('\n');
        }
        output.push_str(&errors);
    }
    match status {
        Some(s) => RunResult { ok: s.success(), output },
        None => RunResult { ok: false, output: format!("{output}\nStopped after {} seconds.", timeout.as_secs()) },
    }
}

#[tauri::command]
fn overview(state: State<AppState>) -> Overview {
    let store = state.store.lock().unwrap();
    let settings = store.settings.clone();
    let game_running = state.game.lock().unwrap().is_some();
    Overview {
        library: mods::scan(&store),
        profiles: store.profiles.clone(),
        game_exe: store::game_exe(&settings).map(|p| p.to_string_lossy().into()),
        sdk_ready: store::sdk_dir(&settings).join("es_mod.h").is_file(),
        modcheck_ready: store::modcheck_exe(&settings).is_file(),
        game_running,
        api_version: mods::MOD_API_VERSION,
        settings,
    }
}

#[tauri::command]
fn save_settings(state: State<AppState>, settings: Settings) -> Result<(), String> {
    let mut store = state.store.lock().unwrap();
    store.settings = settings;
    store.save()
}

/// Make sure a load order file exists, so newly added mods start switched off
/// instead of the game running everything in the folder.
fn ensure_load_order(dir: &Path, before: &[String]) -> Result<(), String> {
    if mods::read_load_order(dir).is_none() {
        mods::write_load_order(dir, before)?;
    }
    Ok(())
}

fn current_folders(dir: &Path) -> Vec<String> {
    mods::mod_folders(dir).iter().map(|p| p.file_name().unwrap().to_string_lossy().to_string()).collect()
}

#[tauri::command]
fn set_load_order(state: State<AppState>, enabled: Vec<String>) -> Result<(), String> {
    let store = state.store.lock().unwrap();
    let dir = PathBuf::from(&store.settings.mods_dir);
    // Only mods being turned on now need approval. A mod already running (for example,
    // turned on by editing load order.txt) is flagged in the list but doesn't block reordering.
    let running = mods::read_load_order(&dir).unwrap_or_else(|| current_folders(&dir));
    for folder in enabled.iter().filter(|f| !running.contains(f)) {
        if let Ok(m) = mods::read_manifest(&dir.join(folder)) {
            let approved = store.approved.get(folder).cloned().unwrap_or_default();
            if let Some(p) = m.permissions.iter().find(|p| !approved.contains(p)) {
                return Err(format!("Approve the \"{p}\" permission for {} before turning it on.", m.name));
            }
        }
    }
    mods::write_load_order(&dir, &enabled)
}

#[tauri::command]
fn approve_permissions(state: State<AppState>, folder: String) -> Result<(), String> {
    let mut store = state.store.lock().unwrap();
    let dir = PathBuf::from(&store.settings.mods_dir);
    let m = mods::read_manifest(&dir.join(&folder))?;
    store.approved.insert(folder, m.permissions);
    store.save()
}

#[tauri::command(async)]
fn pick_folder(app: AppHandle) -> Option<String> {
    app.dialog().file().blocking_pick_folder().and_then(|p| p.into_path().ok()).map(|p| p.to_string_lossy().into())
}

#[tauri::command(async)]
fn pick_zip(app: AppHandle) -> Option<String> {
    app.dialog()
        .file()
        .add_filter("Zip archives", &["zip"])
        .blocking_pick_file()
        .and_then(|p| p.into_path().ok())
        .map(|p| p.to_string_lossy().into())
}

#[tauri::command(async)]
fn pick_file(app: AppHandle) -> Option<String> {
    app.dialog().file().blocking_pick_file().and_then(|p| p.into_path().ok()).map(|p| p.to_string_lossy().into())
}

/// kind: "folder", "link", "zip" or "git".
#[tauri::command(async)]
fn add_mod(state: State<AppState>, kind: String, source: String, replace: bool) -> Result<Vec<String>, String> {
    let dir = mods_dir(&state);
    let before = mods::read_load_order(&dir).unwrap_or_else(|| current_folders(&dir));
    let added = match kind.as_str() {
        "folder" => install::add_folder(Path::new(&source), &dir, false, replace),
        "link" => install::add_folder(Path::new(&source), &dir, true, replace),
        "zip" => install::add_zip(Path::new(&source), &dir, replace),
        "git" => install::add_git(&source, &dir, replace),
        _ => Err("Unknown source.".into()),
    }?;
    ensure_load_order(&dir, &before.into_iter().filter(|f| !added.contains(f)).collect::<Vec<_>>())?;
    // A replaced mod keeps its place; its permissions must be approved again if they changed.
    Ok(added)
}

#[tauri::command(async)]
fn remove_mod(state: State<AppState>, folder: String) -> Result<(), String> {
    let dir = mods_dir(&state);
    let order = mods::read_load_order(&dir).unwrap_or_else(|| current_folders(&dir));
    install::remove(&dir, &folder)?;
    mods::write_load_order(&dir, &order.into_iter().filter(|f| f != &folder).collect::<Vec<_>>())
}

#[tauri::command(async)]
fn rollback_mod(state: State<AppState>, folder: String) -> Result<String, String> {
    install::rollback(&mods_dir(&state), &folder)
}

#[tauri::command(async)]
fn new_mod(state: State<AppState>, name: String) -> Result<String, String> {
    let dir = mods_dir(&state);
    let mut order = mods::read_load_order(&dir).unwrap_or_else(|| current_folders(&dir));
    let folder = install::create(&dir, &name)?;
    order.push(folder.clone());
    mods::write_load_order(&dir, &order)?;
    Ok(folder)
}

#[tauri::command(async)]
fn build_mod(state: State<AppState>, folder: String) -> RunResult {
    let (dir, sdk) = {
        let store = state.store.lock().unwrap();
        (PathBuf::from(&store.settings.mods_dir), store::sdk_dir(&store.settings))
    };
    let path = dir.join(&folder);
    if !path.join("build.sh").is_file() {
        return RunResult { ok: false, output: "This mod has no build.sh, so it can't be built here.".into() };
    }
    let mut command = Command::new("sh");
    command.arg("build.sh").current_dir(&path).env("ES_MOD_SDK", &sdk);
    run(command, Duration::from_secs(120))
}

/// Run the mod outside the game with modcheck and remember the result.
#[tauri::command(async)]
fn test_mod(state: State<AppState>, folder: String) -> Result<TestRecord, String> {
    let (dir, modcheck) = {
        let store = state.store.lock().unwrap();
        (PathBuf::from(&store.settings.mods_dir), store::modcheck_exe(&store.settings))
    };
    if !modcheck.is_file() {
        return Err("The test tool isn't built yet. Build it from Settings.".into());
    }
    let path = dir.join(&folder);
    let entry = mods::read_manifest(&path)?.entry;
    let hash = mods::file_hash(&path.join(&entry)).ok_or("Build the mod before testing it.")?;
    let mut command = Command::new(&modcheck);
    command.arg(&path).arg("3600");
    let result = run(command, Duration::from_secs(60));

    let mut record = TestRecord { hash, ok: false, output: result.output.clone(), when: install::now(), ..Default::default() };
    for line in result.output.lines() {
        if let Some(rest) = line.strip_prefix("Calls: ") {
            // "Calls: 3607, average 0.0001 ms, peak 0.02 ms"
            let numbers: Vec<f64> = rest
                .split(|c: char| !(c.is_ascii_digit() || c == '.' || c == 'e' || c == '-'))
                .filter_map(|s| s.parse().ok())
                .collect();
            if numbers.len() >= 3 {
                record.average_ms = numbers[1];
                record.peak_ms = numbers[2];
            }
        } else if line == "OK" {
            record.ok = true;
            record.summary = "Passed: ran 3,600 frames (one minute of flight) without problems.".into();
        } else if let Some(reason) = line.strip_prefix("FAULTED: ") {
            record.summary = format!("Stopped: {reason}");
        } else if let Some(reason) = line.strip_prefix("FAILED to load: ") {
            record.summary = format!("Didn't load: {reason}");
        }
    }
    if record.summary.is_empty() {
        record.summary = "The test didn't finish. See the output for details.".into();
    }
    let mut store = state.store.lock().unwrap();
    store.tests.insert(folder, record.clone());
    store.save()?;
    Ok(record)
}

/// Build the modcheck test tool from the game checkout.
#[tauri::command(async)]
fn build_test_tool(state: State<AppState>) -> RunResult {
    let game_dir = state.store.lock().unwrap().settings.game_dir.clone();
    if !Path::new(&game_dir).join("mods-sdk").join("tools").is_dir() {
        return RunResult { ok: false, output: "Set the game folder in Settings first.".into() };
    }
    let mut configure = Command::new("cmake");
    configure.args(["-S", "mods-sdk/tools", "-B", "build-modcheck", "-DCMAKE_BUILD_TYPE=Release"]).current_dir(&game_dir);
    let first = run(configure, Duration::from_secs(600));
    if !first.ok {
        return first;
    }
    let mut build = Command::new("cmake");
    build.args(["--build", "build-modcheck"]).current_dir(&game_dir);
    let second = run(build, Duration::from_secs(900));
    RunResult { ok: second.ok, output: format!("{}\n{}", first.output, second.output) }
}

/// Ask the real game which mods it would run, and why not.
#[tauri::command(async)]
fn check_with_game(state: State<AppState>) -> RunResult {
    let (exe, game_dir) = {
        let store = state.store.lock().unwrap();
        (store::game_exe(&store.settings), store.settings.game_dir.clone())
    };
    let Some(exe) = exe else {
        return RunResult { ok: false, output: "Couldn't find the game. Build it, or set its location in Settings.".into() };
    };
    let mut command = Command::new(exe);
    command.arg("--mods");
    if !game_dir.is_empty() {
        command.current_dir(game_dir);
    }
    run(command, Duration::from_secs(60))
}

#[tauri::command]
fn launch_game(app: AppHandle, state: State<AppState>, dev: bool) -> Result<(), String> {
    let mut slot = state.game.lock().unwrap();
    if slot.is_some() {
        return Err("The game is already running.".into());
    }
    let (exe, game_dir) = {
        let store = state.store.lock().unwrap();
        (store::game_exe(&store.settings), store.settings.game_dir.clone())
    };
    let exe = exe.ok_or("Couldn't find the game. Build it, or set its location in Settings.")?;
    let mut command = Command::new(&exe);
    if dev {
        command.arg("--mod-dev");
    }
    if !game_dir.is_empty() {
        command.current_dir(&game_dir);
    }
    let mut child = command
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .stdin(Stdio::null())
        .spawn()
        .map_err(|e| format!("Could not start the game: {e}"))?;

    for (stream, name) in [
        (child.stdout.take().map(|s| Box::new(s) as Box<dyn Read + Send>), "out"),
        (child.stderr.take().map(|s| Box::new(s) as Box<dyn Read + Send>), "err"),
    ] {
        if let Some(stream) = stream {
            let app = app.clone();
            thread::spawn(move || {
                for line in BufReader::new(stream).lines().map_while(Result::ok) {
                    let _ = app.emit("game-log", LogLine { line, stream: name.into() });
                }
            });
        }
    }
    *slot = Some(child);
    drop(slot);

    let _ = app.emit("game-start", exe.to_string_lossy().to_string());
    let game = state.game.clone();
    let waiter = app.clone();
    thread::spawn(move || loop {
        thread::sleep(Duration::from_millis(250));
        let mut slot = game.lock().unwrap();
        let finished = match slot.as_mut() {
            Some(child) => match child.try_wait() {
                Ok(Some(status)) => Some(status.code()),
                Ok(None) => None,
                Err(_) => Some(None),
            },
            None => Some(None),
        };
        if let Some(code) = finished {
            *slot = None;
            let _ = waiter.emit("game-exit", code);
            break;
        }
    });
    Ok(())
}

#[tauri::command]
fn stop_game(state: State<AppState>) -> Result<(), String> {
    match state.game.lock().unwrap().as_mut() {
        Some(child) => child.kill().map_err(|e| e.to_string()),
        None => Ok(()),
    }
}

fn open_with(program: &str, target: &str) -> Result<(), String> {
    let mut parts = program.split_whitespace();
    let exe = parts.next().ok_or("No program set.")?;
    Command::new(exe)
        .args(parts)
        .arg(target)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("Could not run \"{exe}\": {e}"))
}

fn system_opener() -> &'static str {
    if cfg!(target_os = "macos") {
        "open"
    } else if cfg!(windows) {
        "explorer"
    } else {
        "xdg-open"
    }
}

#[tauri::command]
fn open_path(path: String) -> Result<(), String> {
    open_with(system_opener(), &path)
}

#[tauri::command]
fn open_in_editor(state: State<AppState>, folder: String) -> Result<(), String> {
    let (dir, editor) = {
        let store = state.store.lock().unwrap();
        (PathBuf::from(&store.settings.mods_dir), store.settings.editor.clone())
    };
    let path = dir.join(&folder);
    let path = path.to_string_lossy();
    if editor.trim().is_empty() {
        open_with(system_opener(), &path)
    } else {
        open_with(&editor, &path)
    }
}

#[tauri::command]
fn save_profile(state: State<AppState>, name: String) -> Result<(), String> {
    let mut store = state.store.lock().unwrap();
    let name = name.trim().to_string();
    if name.is_empty() {
        return Err("Give the profile a name.".into());
    }
    let dir = PathBuf::from(&store.settings.mods_dir);
    let enabled = mods::read_load_order(&dir).unwrap_or_else(|| current_folders(&dir));
    store.profiles.insert(name, enabled);
    store.save()
}

#[tauri::command]
fn apply_profile(state: State<AppState>, name: String) -> Result<Vec<String>, String> {
    let store = state.store.lock().unwrap();
    let list = store.profiles.get(&name).ok_or("That profile doesn't exist.")?.clone();
    let dir = PathBuf::from(&store.settings.mods_dir);
    let installed = current_folders(&dir);
    let missing: Vec<String> = list.iter().filter(|f| !installed.contains(f)).cloned().collect();
    let present: Vec<String> = list.into_iter().filter(|f| installed.contains(f)).collect();
    drop(store);
    set_load_order(state, present)?;
    Ok(missing)
}

#[tauri::command]
fn delete_profile(state: State<AppState>, name: String) -> Result<(), String> {
    let mut store = state.store.lock().unwrap();
    store.profiles.remove(&name);
    store.save()
}

fn main() {
    let state = AppState { store: Mutex::new(Store::load()), game: Arc::new(Mutex::new(None)) };
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(state)
        .invoke_handler(tauri::generate_handler![
            overview,
            save_settings,
            set_load_order,
            approve_permissions,
            pick_folder,
            pick_zip,
            pick_file,
            add_mod,
            remove_mod,
            rollback_mod,
            new_mod,
            build_mod,
            test_mod,
            build_test_tool,
            check_with_game,
            launch_game,
            stop_game,
            open_path,
            open_in_editor,
            save_profile,
            apply_profile,
            delete_profile,
        ])
        .run(tauri::generate_context!())
        .expect("error while running Nexus Mod Manager");
}
