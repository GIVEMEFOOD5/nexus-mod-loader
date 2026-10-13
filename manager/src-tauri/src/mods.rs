// Reads the mods folder the same way the game does, and works out what to warn about.

use crate::store::{Store, TestRecord};
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

/// The mod API version the game provides (see source/mod/ModHost.h).
pub const MOD_API_VERSION: i64 = 1;
/// Average frame time above which a mod counts as slow.
pub const SLOW_MS: f64 = 1.0;
pub const LOAD_ORDER_FILE: &str = "load order.txt";

#[derive(Serialize, Clone, Debug)]
pub struct Warning {
    /// ai, sandbox, permissions, unverified, compat, perf, untested, build
    pub kind: String,
    pub title: String,
    pub detail: String,
    /// True if the game will refuse to run the mod because of this.
    pub blocking: bool,
}

#[derive(Serialize, Clone, Debug, Default)]
pub struct AiDisclosure {
    pub tool: String,
    pub scope: String,
    pub human_reviewed: bool,
}

#[derive(Serialize, Clone, Debug, Default)]
pub struct Manifest {
    pub name: String,
    pub version: String,
    pub authors: Vec<String>,
    pub description: String,
    pub homepage: String,
    pub runtime: String,
    pub entry: String,
    pub api_version: i64,
    pub game_versions: Vec<String>,
    pub hooks: Vec<String>,
    pub permissions: Vec<String>,
    pub dependencies: Vec<String>,
    pub conflicts: Vec<String>,
    pub checksum: String,
    pub ai_generated: Option<AiDisclosure>,
}

#[derive(Serialize, Clone, Debug)]
pub struct ModInfo {
    pub folder: String,
    pub path: String,
    pub is_link: bool,
    pub manifest: Manifest,
    pub manifest_error: Option<String>,
    pub enabled: bool,
    pub wasm_exists: bool,
    pub can_build: bool,
    pub needs_build: bool,
    pub permissions_approved: bool,
    pub test: Option<TestRecord>,
    pub test_current: bool,
    pub has_backups: bool,
    pub warnings: Vec<Warning>,
}

#[derive(Serialize, Clone, Debug)]
pub struct Library {
    pub mods_dir: String,
    pub mods_dir_exists: bool,
    /// False when there is no load order.txt, so the game runs every mod alphabetically.
    pub has_load_order: bool,
    /// Every mod: enabled ones first, in load order, then the rest alphabetically.
    pub mods: Vec<ModInfo>,
}

fn strings(value: Option<&toml::Value>) -> Vec<String> {
    match value {
        Some(toml::Value::Array(items)) => items
            .iter()
            .filter_map(|v| match v {
                toml::Value::String(s) => Some(s.clone()),
                toml::Value::Integer(i) => Some(i.to_string()),
                _ => None,
            })
            .collect(),
        Some(toml::Value::String(s)) => vec![s.clone()],
        _ => vec![],
    }
}

fn text(value: Option<&toml::Value>) -> String {
    match value {
        Some(toml::Value::String(s)) => s.clone(),
        Some(other) => other.to_string(),
        None => String::new(),
    }
}

fn truthy(value: Option<&toml::Value>) -> bool {
    match value {
        Some(toml::Value::Boolean(b)) => *b,
        Some(toml::Value::String(s)) => matches!(s.to_lowercase().as_str(), "yes" | "true"),
        _ => false,
    }
}

/// Look a key up at the top level or in a [mod] section, accepting - or _ spellings.
fn lookup<'a>(table: &'a toml::Table, key: &str) -> Option<&'a toml::Value> {
    let dashed = key.replace('_', "-");
    let spaced = key.replace('_', " ");
    for k in [key, dashed.as_str(), spaced.as_str()] {
        if let Some(v) = table.get(k) {
            return Some(v);
        }
        if let Some(toml::Value::Table(section)) = table.get("mod") {
            if let Some(v) = section.get(k) {
                return Some(v);
            }
        }
    }
    None
}

pub fn read_manifest(folder: &Path) -> Result<Manifest, String> {
    let file = folder.join("mod.toml");
    let content = fs::read_to_string(&file).map_err(|e| format!("Can't read mod.toml: {e}"))?;
    let table: toml::Table = content
        .parse()
        .map_err(|e: toml::de::Error| format!("mod.toml isn't valid: {}", e.message()))?;

    let mut m = Manifest {
        name: text(lookup(&table, "name")),
        version: text(lookup(&table, "version")),
        authors: strings(lookup(&table, "authors")),
        description: text(lookup(&table, "description")),
        homepage: text(lookup(&table, "homepage")),
        runtime: text(lookup(&table, "runtime")),
        entry: text(lookup(&table, "entry")),
        api_version: lookup(&table, "api_version").and_then(|v| v.as_integer()).unwrap_or(0),
        game_versions: strings(lookup(&table, "game_versions")),
        hooks: strings(lookup(&table, "hooks")),
        permissions: strings(lookup(&table, "permissions")),
        dependencies: strings(lookup(&table, "dependencies")),
        conflicts: strings(lookup(&table, "conflicts")),
        checksum: text(lookup(&table, "checksum")),
        ai_generated: None,
    };
    if m.name.is_empty() {
        m.name = folder.file_name().map(|n| n.to_string_lossy().into()).unwrap_or_default();
    }
    if m.runtime.is_empty() {
        m.runtime = "wasm".into();
    }
    if m.entry.is_empty() {
        m.entry = "mod.wasm".into();
    }
    if let Some(toml::Value::Table(ai)) = lookup(&table, "ai_generated") {
        m.ai_generated = Some(AiDisclosure {
            tool: text(ai.get("tool")),
            scope: strings(ai.get("scope")).join(", "),
            human_reviewed: truthy(ai.get("human_reviewed").or(ai.get("human-reviewed")).or(ai.get("human reviewed"))),
        });
    }
    Ok(m)
}

pub fn file_hash(path: &Path) -> Option<String> {
    let bytes = fs::read(path).ok()?;
    let digest = Sha256::digest(&bytes);
    Some(digest.iter().map(|b| format!("{b:02x}")).collect())
}

fn modified(path: &Path) -> Option<SystemTime> {
    fs::metadata(path).and_then(|m| m.modified()).ok()
}

/// Newest modification time of the mod's source files (anything that isn't the build output).
fn newest_source(folder: &Path, entry: &str) -> Option<SystemTime> {
    let mut newest = None;
    let mut stack = vec![folder.to_path_buf()];
    while let Some(dir) = stack.pop() {
        for item in fs::read_dir(&dir).ok()?.flatten() {
            let path = item.path();
            let name = item.file_name().to_string_lossy().to_string();
            if name.starts_with('.') || name == "target" || name == "build" {
                continue;
            }
            if path.is_dir() {
                stack.push(path);
                continue;
            }
            let ext = path.extension().map(|e| e.to_string_lossy().to_lowercase()).unwrap_or_default();
            let is_source = matches!(ext.as_str(), "c" | "h" | "cpp" | "hpp" | "cc" | "rs" | "zig" | "ts" | "go")
                || name == "build.sh";
            if is_source && path != folder.join(entry) {
                let time = modified(&path);
                if time > newest {
                    newest = time;
                }
            }
        }
    }
    newest
}

/// The enabled folders in load order, or None if there is no load order file.
pub fn read_load_order(mods_dir: &Path) -> Option<Vec<String>> {
    let content = fs::read_to_string(mods_dir.join(LOAD_ORDER_FILE)).ok()?;
    Some(
        content
            .lines()
            .map(|l| l.trim())
            .filter(|l| !l.is_empty() && !l.starts_with('#'))
            .map(String::from)
            .collect(),
    )
}

pub fn write_load_order(mods_dir: &Path, enabled: &[String]) -> Result<(), String> {
    fs::create_dir_all(mods_dir).map_err(|e| e.to_string())?;
    let mut content = String::from(
        "# Code mods to run, top first. Written by Nexus Mod Manager.\n# Mods in the folder but not listed here are turned off.\n",
    );
    for folder in enabled {
        content.push_str(folder);
        content.push('\n');
    }
    fs::write(mods_dir.join(LOAD_ORDER_FILE), content).map_err(|e| format!("Could not save the load order: {e}"))
}

pub fn mod_folders(mods_dir: &Path) -> Vec<PathBuf> {
    let mut folders: Vec<PathBuf> = fs::read_dir(mods_dir)
        .map(|it| {
            it.flatten()
                .map(|e| e.path())
                .filter(|p| p.is_dir() && p.join("mod.toml").is_file())
                .collect()
        })
        .unwrap_or_default();
    folders.sort();
    folders
}

pub fn scan(store: &Store) -> Library {
    let mods_dir = PathBuf::from(&store.settings.mods_dir);
    let folders = mod_folders(&mods_dir);
    let order = read_load_order(&mods_dir);
    let has_load_order = order.is_some();
    let all_names: Vec<String> = folders
        .iter()
        .map(|p| p.file_name().unwrap().to_string_lossy().to_string())
        .collect();
    let enabled_order: Vec<String> = match &order {
        Some(list) => list.iter().filter(|f| all_names.contains(f)).cloned().collect(),
        None => all_names.clone(),
    };

    let mut mods: Vec<ModInfo> = folders
        .iter()
        .map(|path| describe(store, path, enabled_order.contains(&path.file_name().unwrap().to_string_lossy().to_string())))
        .collect();

    // Cross-mod checks: dependencies must run earlier, conflicts can't both run.
    let manifest_names: BTreeMap<String, String> = mods
        .iter()
        .map(|m| (m.folder.clone(), m.manifest.name.clone()))
        .collect();
    let position = |folder: &str| enabled_order.iter().position(|f| f == folder);
    let running_names: BTreeSet<String> = enabled_order
        .iter()
        .filter_map(|f| manifest_names.get(f).cloned())
        .collect();
    for m in mods.iter_mut().filter(|m| m.enabled) {
        let me = position(&m.folder).unwrap_or(usize::MAX);
        for dep in &m.manifest.dependencies {
            let dep_pos = enabled_order
                .iter()
                .position(|f| manifest_names.get(f).map(|n| n == dep).unwrap_or(false));
            match dep_pos {
                None => m.warnings.push(Warning {
                    kind: "compat".into(),
                    title: format!("Needs \"{dep}\""),
                    detail: format!("Install and turn on \"{dep}\" first. The game won't start this mod without it."),
                    blocking: true,
                }),
                Some(p) if p > me => m.warnings.push(Warning {
                    kind: "compat".into(),
                    title: format!("\"{dep}\" must load first"),
                    detail: format!("Drag \"{dep}\" above this mod on the route."),
                    blocking: true,
                }),
                _ => {}
            }
        }
        for other in &m.manifest.conflicts {
            if running_names.contains(other) {
                m.warnings.push(Warning {
                    kind: "compat".into(),
                    title: format!("Conflicts with \"{other}\""),
                    detail: "Only one of these can run. The game skips whichever comes later in the load order.".into(),
                    blocking: true,
                });
            }
        }
    }

    mods.sort_by_key(|m| (position(&m.folder).unwrap_or(usize::MAX), m.folder.clone()));
    Library {
        mods_dir: mods_dir.to_string_lossy().into(),
        mods_dir_exists: mods_dir.is_dir(),
        has_load_order,
        mods,
    }
}

fn describe(store: &Store, path: &Path, enabled: bool) -> ModInfo {
    let folder = path.file_name().unwrap().to_string_lossy().to_string();
    let is_link = fs::symlink_metadata(path).map(|m| m.file_type().is_symlink()).unwrap_or(false);
    let (manifest, manifest_error) = match read_manifest(path) {
        Ok(m) => (m, None),
        Err(e) => (Manifest { name: folder.clone(), runtime: "wasm".into(), entry: "mod.wasm".into(), ..Default::default() }, Some(e)),
    };
    let wasm = path.join(&manifest.entry);
    let wasm_exists = wasm.is_file();
    let can_build = path.join("build.sh").is_file();
    let needs_build = can_build && (!wasm_exists || newest_source(path, &manifest.entry) > modified(&wasm));
    let hash = if wasm_exists { file_hash(&wasm) } else { None };
    let approved: BTreeSet<&String> = store.approved.get(&folder).map(|v| v.iter().collect()).unwrap_or_default();
    let permissions_approved = manifest.permissions.iter().all(|p| approved.contains(p));
    let test = store.tests.get(&folder).cloned();
    let test_current = matches!((&test, &hash), (Some(t), Some(h)) if &t.hash == h);
    let has_backups = crate::install::backups(&folder).map(|b| !b.is_empty()).unwrap_or(false);

    let mut w = Vec::new();
    if let Some(error) = &manifest_error {
        w.push(Warning { kind: "compat".into(), title: "Broken mod.toml".into(), detail: error.clone(), blocking: true });
    }
    if let Some(ai) = &manifest.ai_generated {
        let scope = if ai.scope.is_empty() { "Unspecified parts".to_string() } else { capitalize(&ai.scope) };
        let tool = if ai.tool.is_empty() { "an unnamed tool".to_string() } else { ai.tool.clone() };
        w.push(Warning {
            kind: "ai".into(),
            title: if ai.human_reviewed { "AI-made, human reviewed".into() } else { "AI-made, not reviewed".into() },
            detail: format!("{scope} made with {tool}, according to the author."),
            blocking: false,
        });
    }
    match manifest.runtime.as_str() {
        "wasm" | "data" => {}
        "external" => w.push(Warning {
            kind: "sandbox".into(),
            title: "Not sandboxed".into(),
            detail: "This mod runs as a normal program with full access to your computer. This game build can't run it yet.".into(),
            blocking: true,
        }),
        other => w.push(Warning {
            kind: "compat".into(),
            title: format!("The \"{other}\" runtime isn't supported yet"),
            detail: "This game build only runs WebAssembly mods.".into(),
            blocking: true,
        }),
    }
    if manifest.runtime == "wasm" && manifest_error.is_none() && manifest.api_version != MOD_API_VERSION {
        w.push(Warning {
            kind: "compat".into(),
            title: "Wrong mod API version".into(),
            detail: format!("Made for API version {}, but this game provides version {MOD_API_VERSION}.", manifest.api_version),
            blocking: true,
        });
    }
    if manifest.runtime == "wasm" && !wasm_exists {
        w.push(Warning {
            kind: "build".into(),
            title: "Not built".into(),
            detail: if can_build { format!("{} is missing. Build the mod to create it.", manifest.entry) } else { format!("{} is missing.", manifest.entry) },
            blocking: true,
        });
    } else if needs_build {
        w.push(Warning {
            kind: "build".into(),
            title: "Source changed since the last build".into(),
            detail: "The game is still running the older build.".into(),
            blocking: false,
        });
    }
    if !manifest.permissions.is_empty() {
        let title = if permissions_approved {
            "Permissions approved"
        } else if enabled {
            "Running without your approval"
        } else {
            "Asks for permissions"
        };
        let detail = if !permissions_approved && enabled {
            format!("It was turned on outside this app and can use: {}. Review them, or turn it off.", manifest.permissions.join(", "))
        } else {
            manifest.permissions.join(", ")
        };
        w.push(Warning { kind: "permissions".into(), title: title.into(), detail, blocking: false });
    }
    if manifest.checksum.is_empty() {
        w.push(Warning { kind: "unverified".into(), title: "Unverified".into(), detail: "No checksum or signature, so there's no way to confirm who made this file.".into(), blocking: false });
    } else if let Some(h) = &hash {
        let expected = manifest.checksum.trim_start_matches("sha256:").to_lowercase();
        if &expected != h {
            w.push(Warning { kind: "unverified".into(), title: "Checksum doesn't match".into(), detail: "The .wasm file has changed since its checksum was published.".into(), blocking: false });
        }
    }
    if manifest.runtime == "wasm" && wasm_exists {
        match &test {
            Some(t) if test_current && !t.ok => w.push(Warning { kind: "compat".into(), title: "Failed its test".into(), detail: t.summary.clone(), blocking: false }),
            Some(t) if test_current && t.average_ms > SLOW_MS => w.push(Warning {
                kind: "perf".into(),
                title: "Slow".into(),
                detail: format!("{:.2} ms per frame on average in its last test.", t.average_ms),
                blocking: false,
            }),
            Some(_) if test_current => {}
            _ => w.push(Warning { kind: "untested".into(), title: "Untested".into(), detail: "This build hasn't passed a test run yet.".into(), blocking: false }),
        }
    }

    ModInfo {
        folder,
        path: path.to_string_lossy().into(),
        is_link,
        manifest,
        manifest_error,
        enabled,
        wasm_exists,
        can_build,
        needs_build,
        permissions_approved,
        test,
        test_current,
        has_backups,
        warnings: w,
    }
}

fn capitalize(s: &str) -> String {
    let mut chars = s.chars();
    match chars.next() {
        Some(first) => first.to_uppercase().collect::<String>() + chars.as_str(),
        None => String::new(),
    }
}
