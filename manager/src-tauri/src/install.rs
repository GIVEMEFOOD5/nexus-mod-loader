// Adding mods (folder, zip, GitHub), replacing them with a backup, rolling back,
// removing, and creating new mods from a template.

use crate::store::backups_dir;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

pub fn now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

pub fn copy_dir(from: &Path, to: &Path) -> io::Result<()> {
    fs::create_dir_all(to)?;
    for entry in fs::read_dir(from)? {
        let entry = entry?;
        let name = entry.file_name();
        if name == ".git" {
            continue;
        }
        let target = to.join(&name);
        if entry.file_type()?.is_dir() {
            copy_dir(&entry.path(), &target)?;
        } else {
            fs::copy(entry.path(), target)?;
        }
    }
    Ok(())
}

fn remove_path(path: &Path) -> io::Result<()> {
    let meta = fs::symlink_metadata(path)?;
    if meta.file_type().is_symlink() || meta.is_file() {
        fs::remove_file(path)
    } else {
        fs::remove_dir_all(path)
    }
}

/// Folders under `root` (up to three levels down) that contain a mod.toml.
fn find_mods(root: &Path, depth: u32, found: &mut Vec<PathBuf>) {
    if root.join("mod.toml").is_file() {
        found.push(root.to_path_buf());
        return;
    }
    if depth == 0 {
        return;
    }
    if let Ok(entries) = fs::read_dir(root) {
        let mut dirs: Vec<PathBuf> = entries.flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect();
        dirs.sort();
        for dir in dirs {
            if dir.file_name().map(|n| n.to_string_lossy().starts_with('.')).unwrap_or(true) {
                continue;
            }
            find_mods(&dir, depth - 1, found);
        }
    }
}

/// Move an installed mod into the backup area, so it can be rolled back to.
fn back_up(mods_dir: &Path, folder: &str) -> Result<(), String> {
    let current = mods_dir.join(folder);
    if fs::symlink_metadata(&current).is_err() {
        return Ok(());
    }
    let is_link = fs::symlink_metadata(&current).map(|m| m.file_type().is_symlink()).unwrap_or(false);
    let version = crate::mods::read_manifest(&current).map(|m| m.version).unwrap_or_default();
    let label = if version.is_empty() { "unknown".to_string() } else { version.replace(['/', '\\', ' '], "_") };
    let target = backups_dir().join(folder).join(format!("{}-{}", now(), label));
    fs::create_dir_all(target.parent().unwrap()).map_err(|e| e.to_string())?;
    if is_link {
        // A linked mod is the author's own working copy: back up a copy and just remove the link.
        copy_dir(&current, &target).map_err(|e| format!("Could not back up \"{folder}\": {e}"))?;
        fs::remove_file(&current).map_err(|e| e.to_string())
    } else if fs::rename(&current, &target).is_err() {
        copy_dir(&current, &target).map_err(|e| format!("Could not back up \"{folder}\": {e}"))?;
        fs::remove_dir_all(&current).map_err(|e| e.to_string())
    } else {
        Ok(())
    }
}

/// Install every mod found under `source`. Returns the folder names installed.
/// If a mod is already installed and `replace` is false, nothing is changed and
/// the error starts with "exists:".
fn install_from(source: &Path, mods_dir: &Path, replace: bool, link: bool) -> Result<Vec<String>, String> {
    let mut found = Vec::new();
    find_mods(source, 3, &mut found);
    if found.is_empty() {
        return Err("No mod.toml found there, so this doesn't look like a code mod.".into());
    }
    let names: Vec<String> = found.iter().map(|p| p.file_name().unwrap().to_string_lossy().to_string()).collect();
    let existing: Vec<&String> = names.iter().filter(|n| fs::symlink_metadata(mods_dir.join(n)).is_ok()).collect();
    if !existing.is_empty() && !replace {
        return Err(format!("exists:{}", existing.iter().map(|s| s.as_str()).collect::<Vec<_>>().join(", ")));
    }
    fs::create_dir_all(mods_dir).map_err(|e| format!("Could not create the mods folder: {e}"))?;
    for (path, name) in found.iter().zip(&names) {
        back_up(mods_dir, name)?;
        let target = mods_dir.join(name);
        if link {
            make_link(path, &target)?;
        } else {
            copy_dir(path, &target).map_err(|e| format!("Could not copy \"{name}\": {e}"))?;
        }
    }
    Ok(names)
}

#[cfg(unix)]
fn make_link(from: &Path, to: &Path) -> Result<(), String> {
    let from = fs::canonicalize(from).map_err(|e| e.to_string())?;
    std::os::unix::fs::symlink(from, to).map_err(|e| format!("Could not link the folder: {e}"))
}

#[cfg(windows)]
fn make_link(from: &Path, to: &Path) -> Result<(), String> {
    let from = fs::canonicalize(from).map_err(|e| e.to_string())?;
    std::os::windows::fs::symlink_dir(from, to)
        .map_err(|e| format!("Could not link the folder (Windows needs Developer Mode for this): {e}"))
}

pub fn add_folder(path: &Path, mods_dir: &Path, link: bool, replace: bool) -> Result<Vec<String>, String> {
    install_from(path, mods_dir, replace, link)
}

fn temp_dir(prefix: &str) -> Result<PathBuf, String> {
    let dir = std::env::temp_dir().join(format!("nexus-{prefix}-{}-{}", std::process::id(), now()));
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Ok(dir)
}

pub fn add_zip(path: &Path, mods_dir: &Path, replace: bool) -> Result<Vec<String>, String> {
    let file = fs::File::open(path).map_err(|e| format!("Could not open the zip: {e}"))?;
    let mut archive = zip::ZipArchive::new(file).map_err(|e| format!("That isn't a readable zip file: {e}"))?;
    let temp = temp_dir("zip")?;
    // extract() refuses entries that would escape the target folder.
    let result = archive
        .extract(&temp)
        .map_err(|e| format!("Could not unpack the zip: {e}"))
        .and_then(|_| install_from(&temp, mods_dir, replace, false));
    let _ = fs::remove_dir_all(&temp);
    result
}

pub fn add_git(url: &str, mods_dir: &Path, replace: bool) -> Result<Vec<String>, String> {
    let url = url.trim();
    if !(url.starts_with("https://") || url.starts_with("git@")) {
        return Err("Use a GitHub address starting with https://".into());
    }
    let temp = temp_dir("git")?;
    let output = Command::new("git")
        .args(["clone", "--depth", "1", url])
        .arg(temp.join("repo"))
        .output()
        .map_err(|e| format!("Could not run git. Is it installed? ({e})"))?;
    let result = if output.status.success() {
        install_from(&temp.join("repo"), mods_dir, replace, false)
    } else {
        Err(format!("git couldn't download it:\n{}", String::from_utf8_lossy(&output.stderr).trim()))
    };
    let _ = fs::remove_dir_all(&temp);
    result
}

pub fn remove(mods_dir: &Path, folder: &str) -> Result<(), String> {
    let path = mods_dir.join(folder);
    let is_link = fs::symlink_metadata(&path).map(|m| m.file_type().is_symlink()).unwrap_or(false);
    if is_link {
        // Only unlink: never delete the author's working copy.
        return remove_path(&path).map_err(|e| e.to_string());
    }
    back_up(mods_dir, folder)
}

/// Backups for a mod, newest first: (folder name, version label, timestamp).
pub fn backups(folder: &str) -> Result<Vec<(String, String, u64)>, String> {
    let dir = backups_dir().join(folder);
    let mut list: Vec<(String, String, u64)> = match fs::read_dir(&dir) {
        Ok(entries) => entries
            .flatten()
            .filter_map(|e| {
                let name = e.file_name().to_string_lossy().to_string();
                let (stamp, version) = name.split_once('-')?;
                Some((name.clone(), version.to_string(), stamp.parse().ok()?))
            })
            .collect(),
        Err(_) => return Ok(vec![]),
    };
    list.sort_by(|a, b| b.2.cmp(&a.2));
    Ok(list)
}

/// Put the newest backup back, keeping the current version as a backup in turn.
pub fn rollback(mods_dir: &Path, folder: &str) -> Result<String, String> {
    let list = backups(folder)?;
    let (name, version, _) = list.first().ok_or("There's no earlier version to go back to.")?.clone();
    let source = backups_dir().join(folder).join(&name);
    let staging = temp_dir("rollback")?;
    copy_dir(&source, &staging.join(folder)).map_err(|e| e.to_string())?;
    fs::remove_dir_all(&source).map_err(|e| e.to_string())?;
    back_up(mods_dir, folder)?;
    copy_dir(&staging.join(folder), &mods_dir.join(folder)).map_err(|e| e.to_string())?;
    let _ = fs::remove_dir_all(&staging);
    Ok(version)
}

pub fn slug(name: &str) -> String {
    let mut s: String = name
        .trim()
        .to_lowercase()
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
        .collect();
    while s.contains("--") {
        s = s.replace("--", "-");
    }
    s.trim_matches('-').to_string()
}

const TEMPLATE_C: &str = r#"/* {NAME}: an Endless Sky code mod. */
#include "es_mod.h"

ES_MOD_SETUP();

ES_EXPORT(es_mod_init) int es_mod_init(void)
{
	es_info("{NAME} started");
	return 0;
}

/* Called once the game data and the pilot have loaded. */
ES_EXPORT(es_on_data_loaded) void es_on_data_loaded(void)
{
	char name[128];
	char buffer[256];
	es_str s = es_str_init(buffer, sizeof(buffer));
	es_query("player.name", name, sizeof(name));
	es_str_add(&s, "{NAME} is running. Hello, ");
	es_str_add(&s, name);
	es_message(buffer);
}

/* Called when you enter a system, land, take off, or a day passes. */
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

/* Called 60 times a second while flying. Keep it quick. */
ES_EXPORT(es_on_frame) void es_on_frame(long long frame)
{
	(void)frame;
}
"#;

const TEMPLATE_TOML: &str = r#"name = "{NAME}"
version = "0.1.0"
authors = ["{AUTHOR}"]
description = "Describe what your mod does."
homepage = ""

runtime = "wasm"
entry = "mod.wasm"
api_version = 1
game_versions = ["0.11"]
hooks = ["init", "data_loaded", "event", "frame"]

# Ask only for what you need, e.g. ["conditions-write"] to change game conditions.
permissions = []
dependencies = []
conflicts = []

# If any part was made with AI, say so. Players see this as a warning.
# ai_generated = { tool = "...", scope = ["code"], human_reviewed = true }
"#;

const TEMPLATE_BUILD: &str = r#"#!/bin/sh
# Builds mod.wasm with clang and lld. ES_MOD_SDK points at the folder with es_mod.h;
# Nexus Mod Manager sets it for you.
set -e
cd "$(dirname "$0")"
if [ -z "$ES_MOD_SDK" ]; then
	echo "Set ES_MOD_SDK to the mods-sdk/c folder of your Endless Sky checkout." >&2
	exit 1
fi
clang --target=wasm32 -O2 -nostdlib -fno-builtin -Wl,--no-entry -Wl,--strip-all -I "$ES_MOD_SDK" -o mod.wasm *.c
echo "Built mod.wasm"
"#;

pub fn create(mods_dir: &Path, name: &str) -> Result<String, String> {
    let folder = slug(name);
    if folder.is_empty() {
        return Err("Give the mod a name with at least one letter or number.".into());
    }
    let path = mods_dir.join(&folder);
    if fs::symlink_metadata(&path).is_ok() {
        return Err(format!("A mod called \"{folder}\" is already installed."));
    }
    let author = std::env::var("USER").or_else(|_| std::env::var("USERNAME")).unwrap_or_else(|_| "Your Name".into());
    fs::create_dir_all(&path).map_err(|e| e.to_string())?;
    let fill = |t: &str| t.replace("{NAME}", &folder).replace("{AUTHOR}", &author);
    fs::write(path.join("mod.toml"), fill(TEMPLATE_TOML)).map_err(|e| e.to_string())?;
    fs::write(path.join("mod.c"), fill(TEMPLATE_C)).map_err(|e| e.to_string())?;
    fs::write(path.join("build.sh"), TEMPLATE_BUILD).map_err(|e| e.to_string())?;
    fs::write(path.join(".gitignore"), "*.wasm\n").map_err(|e| e.to_string())?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = fs::set_permissions(path.join("build.sh"), fs::Permissions::from_mode(0o755));
    }
    Ok(folder)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn write_mod(dir: &Path, name: &str, version: &str) {
        fs::create_dir_all(dir).unwrap();
        fs::write(dir.join("mod.toml"), format!("name = \"{name}\"\nversion = \"{version}\"\napi_version = 1\n")).unwrap();
        fs::write(dir.join("mod.wasm"), version.as_bytes()).unwrap();
    }

    fn make_zip(path: &Path, folder: &str, version: &str) {
        let file = fs::File::create(path).unwrap();
        let mut zip = zip::ZipWriter::new(file);
        let options = zip::write::SimpleFileOptions::default();
        zip.start_file(format!("release/{folder}/mod.toml"), options).unwrap();
        write!(zip, "name = \"{folder}\"\nversion = \"{version}\"\napi_version = 1\n").unwrap();
        zip.start_file(format!("release/{folder}/mod.wasm"), options).unwrap();
        zip.write_all(version.as_bytes()).unwrap();
        zip.finish().unwrap();
    }

    fn version_of(mods: &Path, folder: &str) -> String {
        crate::mods::read_manifest(&mods.join(folder)).unwrap().version
    }

    // One test, run in order, because backups live in a shared config folder.
    #[test]
    fn install_replace_rollback_remove() {
        let root = std::env::temp_dir().join(format!("nexus-test-{}", now()));
        std::env::set_var("XDG_CONFIG_HOME", root.join("config"));
        fs::create_dir_all(&root).unwrap();
        let mods = root.join("mods");

        // Zip install finds mods nested inside the archive.
        make_zip(&root.join("v1.zip"), "radar", "1.0");
        assert_eq!(add_zip(&root.join("v1.zip"), &mods, false).unwrap(), vec!["radar"]);
        assert_eq!(version_of(&mods, "radar"), "1.0");

        // Installing again without replace is refused and changes nothing.
        make_zip(&root.join("v2.zip"), "radar", "2.0");
        assert!(add_zip(&root.join("v2.zip"), &mods, false).unwrap_err().starts_with("exists:radar"));
        assert_eq!(version_of(&mods, "radar"), "1.0");

        // Replace keeps a backup, and rollback swaps back (keeping 2.0 as a backup in turn).
        add_zip(&root.join("v2.zip"), &mods, true).unwrap();
        assert_eq!(version_of(&mods, "radar"), "2.0");
        assert_eq!(backups("radar").unwrap().len(), 1);
        assert_eq!(rollback(&mods, "radar").unwrap(), "1.0");
        assert_eq!(version_of(&mods, "radar"), "1.0");
        assert_eq!(backups("radar").unwrap()[0].1, "2.0");

        // Folder copy, and a folder with no mod.toml is rejected.
        write_mod(&root.join("src/shields"), "shields", "0.5");
        assert_eq!(add_folder(&root.join("src"), &mods, false, false).unwrap(), vec!["shields"]);
        assert!(add_folder(&root.join("config"), &mods, false, false).is_err());

        // A linked mod is only unlinked on removal; the working copy survives.
        write_mod(&root.join("work/dev-mod"), "dev-mod", "0.1");
        add_folder(&root.join("work/dev-mod"), &mods, true, false).unwrap();
        assert!(fs::symlink_metadata(mods.join("dev-mod")).unwrap().file_type().is_symlink());
        remove(&mods, "dev-mod").unwrap();
        assert!(fs::symlink_metadata(mods.join("dev-mod")).is_err());
        assert!(root.join("work/dev-mod/mod.toml").is_file());

        // Removing an installed mod moves it to the backups.
        remove(&mods, "shields").unwrap();
        assert!(!mods.join("shields").exists());
        assert_eq!(backups("shields").unwrap().len(), 1);

        // New mods get a safe folder name and the template files.
        assert_eq!(create(&mods, "My Cool Mod!").unwrap(), "my-cool-mod");
        assert!(mods.join("my-cool-mod/build.sh").is_file());
        assert!(create(&mods, "my cool mod").is_err());
        assert!(create(&mods, "!!!").is_err());

        // A zip trying to write outside its folder is not extracted outside.
        let evil = root.join("evil.zip");
        {
            let mut zip = zip::ZipWriter::new(fs::File::create(&evil).unwrap());
            zip.start_file("../../escaped.txt", zip::write::SimpleFileOptions::default()).unwrap();
            zip.write_all(b"x").unwrap();
            zip.finish().unwrap();
        }
        assert!(add_zip(&evil, &mods, false).is_err());
        assert!(!root.join("escaped.txt").exists() && !std::env::temp_dir().join("escaped.txt").exists());

        fs::remove_dir_all(&root).unwrap();
    }
}

#[cfg(test)]
mod network_tests {
    // Needs internet access: cargo test -- --ignored
    #[test]
    #[ignore]
    fn github_repo_without_a_mod_is_explained() {
        let mods = std::env::temp_dir().join(format!("nexus-git-{}", super::now()));
        let error = super::add_git("https://github.com/octocat/Hello-World", &mods, false).unwrap_err();
        assert!(error.contains("No mod.toml"), "{error}");
        assert!(super::add_git("not a url", &mods, false).unwrap_err().contains("https://"));
    }
}
