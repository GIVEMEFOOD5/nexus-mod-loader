// Saved manager data: settings, profiles, approved permissions and test results.
// Stored as JSON in the user's config folder (e.g. ~/.config/nexus-mod-manager/data.json).

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Serialize, Deserialize, Clone, Default, Debug)]
#[serde(default)]
pub struct Settings {
    /// The nexus-mod-loader checkout (the patched Endless Sky source).
    pub game_dir: String,
    /// The game executable. Empty means "find it in game_dir/build".
    pub game_exe: String,
    /// Where the game looks for code mods.
    pub mods_dir: String,
    /// Command used for "Open in editor". Empty means the system default.
    pub editor: String,
}

#[derive(Serialize, Deserialize, Clone, Default, Debug)]
#[serde(default)]
pub struct TestRecord {
    /// SHA-256 of the .wasm file that was tested.
    pub hash: String,
    pub ok: bool,
    pub summary: String,
    pub average_ms: f64,
    pub peak_ms: f64,
    pub output: String,
    /// Seconds since the Unix epoch.
    pub when: u64,
}

#[derive(Serialize, Deserialize, Clone, Default, Debug)]
#[serde(default)]
pub struct Store {
    pub settings: Settings,
    /// Profile name -> enabled mod folders, in load order.
    pub profiles: BTreeMap<String, Vec<String>>,
    /// Mod folder -> permissions the player has approved.
    pub approved: BTreeMap<String, Vec<String>>,
    /// Mod folder -> last test run.
    pub tests: BTreeMap<String, TestRecord>,
}

pub fn data_dir() -> PathBuf {
    dirs::config_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join("nexus-mod-manager")
}

pub fn backups_dir() -> PathBuf {
    data_dir().join("backups")
}

fn data_file() -> PathBuf {
    data_dir().join("data.json")
}

impl Store {
    pub fn load() -> Store {
        let mut store: Store = fs::read_to_string(data_file())
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_default();
        store.fill_defaults();
        store
    }

    pub fn save(&self) -> Result<(), String> {
        fs::create_dir_all(data_dir()).map_err(|e| e.to_string())?;
        let text = serde_json::to_string_pretty(self).map_err(|e| e.to_string())?;
        fs::write(data_file(), text).map_err(|e| format!("Could not save settings: {e}"))
    }

    fn fill_defaults(&mut self) {
        if self.settings.mods_dir.is_empty() {
            if let Some(data) = dirs::data_dir() {
                self.settings.mods_dir = data.join("endless-sky").join("mods").to_string_lossy().into();
            }
        }
        if self.settings.game_dir.is_empty() {
            if let Some(home) = dirs::home_dir() {
                for guess in ["Documents/GitHub/nexus-mod-loader", "GitHub/nexus-mod-loader", "nexus-mod-loader"] {
                    let path = home.join(guess);
                    if path.join("source").join("mod").is_dir() {
                        self.settings.game_dir = path.to_string_lossy().into();
                        break;
                    }
                }
            }
        }
    }
}

/// The game executable: the explicit setting, or the first build output found.
pub fn game_exe(settings: &Settings) -> Option<PathBuf> {
    if !settings.game_exe.is_empty() {
        let path = PathBuf::from(&settings.game_exe);
        return path.is_file().then_some(path);
    }
    if settings.game_dir.is_empty() {
        return None;
    }
    let name = if cfg!(windows) { "endless-sky.exe" } else { "endless-sky" };
    let root = Path::new(&settings.game_dir).join("build");
    [
        root.join(name),
        root.join("Release").join(name),
        root.join("linux").join("Release").join(name),
        root.join("linux-release").join(name),
        root.join("macos").join("Release").join(name),
        root.join("mingw").join("Release").join(name),
        root.join("clang-cl").join("Release").join(name),
    ]
    .into_iter()
    .find(|p| p.is_file())
}

pub fn sdk_dir(settings: &Settings) -> PathBuf {
    Path::new(&settings.game_dir).join("mods-sdk").join("c")
}

pub fn modcheck_exe(settings: &Settings) -> PathBuf {
    let name = if cfg!(windows) { "modcheck.exe" } else { "modcheck" };
    Path::new(&settings.game_dir).join("build-modcheck").join(name)
}
