//! Commands for the pedal's capture / IR memory, the backup preview and the local preset library
//! (`.pulsedeck` packages stored under the app data directory, grouped in setlist folders).

use crate::state::Shared;
use pulsedeck_core::backup;
use pulsedeck_core::rig::{self, LoadReport, MemoryUsage, PkgManifest, SlotKind};
use pulsedeck_core::upload::{Progress, ProgressFn};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tauri::{AppHandle, Emitter, Manager, State};

const EXT: &str = "pulsedeck";
const DEFAULT_SETLIST: &str = "Geral";

async fn blocking<T: Send + 'static>(
    f: impl FnOnce() -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f)
        .await
        .map_err(|e| e.to_string())?
}

fn progress_fn(app: &AppHandle) -> ProgressFn {
    let app = app.clone();
    Arc::new(move |p: Progress| {
        let _ = app.emit("pedal:progress", p);
    })
}

fn firmware(core: &Shared) -> String {
    match core.snapshot().status {
        pulsedeck_core::manager::ConnectionStatus::Connected { pong, .. } => pong,
        _ => String::new(),
    }
}

// ------------------------------------------------------------------------------- memory

/// Which presets use each slot. Loads every preset in turn (and returns to the current one), so
/// unsaved edits on the pedal are discarded: the UI asks first.
#[tauri::command]
pub async fn memory_usage(core: State<'_, Shared>, app: AppHandle) -> Result<MemoryUsage, String> {
    let core = core.inner().clone();
    blocking(move || {
        let session = core.session()?;
        let progress = progress_fn(&app);
        let usage = session
            .exclusive(move |ex| rig::scan_usage(ex, &progress))
            .map_err(|e| e.to_string())?;
        core.clear_unsaved(true, true);
        core.sync_all(&session);
        usage.map_err(|e| e.to_string())
    })
    .await
}

fn kind_of(kind: &str) -> Result<SlotKind, String> {
    match kind {
        "capture" => Ok(SlotKind::Capture),
        "ir" => Ok(SlotKind::Ir),
        other => Err(format!("Tipo de slot desconhecido: {other}")),
    }
}

fn check_slot(core: &Shared, kind: SlotKind, slot: u8) -> Result<(), String> {
    let count = match kind {
        SlotKind::Capture => core.protocol.capture_slots,
        SlotKind::Ir => core.protocol.ir_slots,
    };
    if slot >= count {
        return Err(format!("Slot {} fora do intervalo 1..{count}", slot + 1));
    }
    Ok(())
}

/// Rename a stored capture / IR (re-written in place under the new name).
#[tauri::command]
pub async fn slot_rename(
    core: State<'_, Shared>,
    app: AppHandle,
    kind: String,
    slot: u8,
    name: String,
) -> Result<Vec<String>, String> {
    let core = core.inner().clone();
    blocking(move || {
        let kind = kind_of(&kind)?;
        core.require_fast_link("Renomear um slot (reenvia o arquivo)")?;
        check_slot(&core, kind, slot)?;
        let session = core.session()?;
        let progress = progress_fn(&app);
        let res = session
            .exclusive(move |ex| rig::rename_slot(ex, kind, slot, &name, &progress))
            .map_err(|e| e.to_string())?;
        core.sync_all(&session);
        res.map_err(|e| e.to_string())
    })
    .await
}

/// Clear a stored capture / IR slot on the pedal (`CDEL` / `IDEL`).
#[tauri::command]
pub async fn slot_delete(core: State<'_, Shared>, kind: String, slot: u8) -> Result<(), String> {
    let core = core.inner().clone();
    blocking(move || {
        let kind = kind_of(&kind)?;
        check_slot(&core, kind, slot)?;
        let session = core.session()?;
        let res = session
            .exclusive(move |ex| rig::delete_slot(ex, kind, slot))
            .map_err(|e| e.to_string())?;
        core.sync_all(&session);
        res.map_err(|e| e.to_string())
    })
    .await
}

// ------------------------------------------------------------------------------ backup

#[derive(Serialize)]
pub struct BackupPreview {
    pub firmware: String,
    pub created_unix: u64,
    pub presets: usize,
    pub captures: usize,
    pub irs: usize,
    pub globals: usize,
    pub preset_names: Vec<String>,
}

/// Validate a backup `.zip` and describe what restoring it would write.
#[tauri::command]
pub async fn inspect_backup(path: String) -> Result<BackupPreview, String> {
    blocking(move || {
        let b = backup::read_zip(Path::new(&path)).map_err(|e| e.to_string())?;
        Ok(BackupPreview {
            firmware: b.manifest.firmware,
            created_unix: b.manifest.created_unix,
            presets: b.presets.len(),
            captures: b.captures.len(),
            irs: b.irs.len(),
            globals: b.globals.settings.len(),
            preset_names: b
                .presets
                .iter()
                .map(|p| format!("{:02} · {}", p.slot + 1, p.name))
                .collect(),
        })
    })
    .await
}

// ----------------------------------------------------------------------------- library

#[derive(Serialize)]
pub struct LibItem {
    pub path: String,
    pub name: String,
    pub setlist: String,
    pub created_unix: u64,
    pub firmware: String,
    pub has_capture: bool,
    pub ir_count: usize,
    pub scenes: usize,
}

#[derive(Serialize)]
pub struct Setlist {
    pub name: String,
    pub items: Vec<LibItem>,
}

fn library_root(app: &AppHandle) -> Result<PathBuf, String> {
    let root = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("Pasta de dados indisponível: {e}"))?
        .join("library");
    std::fs::create_dir_all(root.join(DEFAULT_SETLIST))
        .map_err(|e| format!("Não foi possível criar a biblioteca: {e}"))?;
    Ok(root)
}

/// A plain folder / file name: no separators, no dots at the start, not empty.
fn safe_name(name: &str) -> Result<String, String> {
    let s: String = name
        .trim()
        .chars()
        .map(|c| {
            if c.is_control() || matches!(c, '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|') {
                '_'
            } else {
                c
            }
        })
        .collect();
    let s = s.trim().trim_start_matches('.').trim().to_string();
    let s: String = s.chars().take(60).collect();
    if s.is_empty() {
        Err("O nome não pode ficar vazio.".into())
    } else {
        Ok(s)
    }
}

fn setlist_dir(root: &Path, setlist: &str) -> Result<PathBuf, String> {
    Ok(root.join(safe_name(setlist)?))
}

/// `path` must be a `.pulsedeck` file inside the library root.
fn library_file(root: &Path, path: &str) -> Result<PathBuf, String> {
    let root = root.canonicalize().map_err(|e| e.to_string())?;
    let p = PathBuf::from(path)
        .canonicalize()
        .map_err(|e| format!("Arquivo não encontrado: {e}"))?;
    let ok = p.starts_with(&root) && p.extension().is_some_and(|e| e == EXT);
    if ok {
        Ok(p)
    } else {
        Err("Arquivo fora da biblioteca.".into())
    }
}

fn item(path: &Path, setlist: &str, m: PkgManifest) -> LibItem {
    LibItem {
        path: path.to_string_lossy().into(),
        name: m.name,
        setlist: setlist.into(),
        created_unix: m.created_unix,
        firmware: m.firmware,
        has_capture: m.has_capture,
        ir_count: m.ir_count,
        scenes: m.scenes,
    }
}

fn unique_path(dir: &Path, stem: &str) -> PathBuf {
    let first = dir.join(format!("{stem}.{EXT}"));
    if !first.exists() {
        return first;
    }
    (2..)
        .map(|n| dir.join(format!("{stem} ({n}).{EXT}")))
        .find(|p| !p.exists())
        .unwrap_or(first)
}

#[tauri::command]
pub async fn library_list(app: AppHandle) -> Result<Vec<Setlist>, String> {
    blocking(move || {
        let root = library_root(&app)?;
        let mut out = Vec::new();
        for d in std::fs::read_dir(&root)
            .map_err(|e| e.to_string())?
            .flatten()
        {
            if !d.path().is_dir() {
                continue;
            }
            let name = d.file_name().to_string_lossy().to_string();
            let mut items: Vec<LibItem> = std::fs::read_dir(d.path())
                .map_err(|e| e.to_string())?
                .flatten()
                .filter(|f| f.path().extension().is_some_and(|e| e == EXT))
                .filter_map(|f| {
                    let m = rig::read_package_manifest(&f.path()).ok()?;
                    Some(item(&f.path(), &name, m))
                })
                .collect();
            items.sort_by_key(|i| i.name.to_lowercase());
            out.push(Setlist { name, items });
        }
        out.sort_by_key(|s| (s.name != DEFAULT_SETLIST, s.name.to_lowercase()));
        Ok(out)
    })
    .await
}

#[tauri::command]
pub async fn library_create_setlist(app: AppHandle, name: String) -> Result<(), String> {
    blocking(move || {
        let dir = setlist_dir(&library_root(&app)?, &name)?;
        if dir.exists() {
            return Err("Já existe uma pasta com esse nome.".into());
        }
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())
    })
    .await
}

/// Only empty setlists can be removed (presets are deleted one by one, on purpose).
#[tauri::command]
pub async fn library_delete_setlist(app: AppHandle, name: String) -> Result<(), String> {
    blocking(move || {
        let dir = setlist_dir(&library_root(&app)?, &name)?;
        std::fs::remove_dir(&dir)
            .map_err(|_| "A pasta não está vazia: apague os presets antes.".to_string())
    })
    .await
}

/// Save the preset that is loaded on the pedal (chain, scenes, capture and IR data) as a
/// self-contained `.pulsedeck` file. Scenes are read by switching to them, so unsaved edits on
/// the pedal are replaced by the stored scenes (the UI asks first).
#[tauri::command]
pub async fn library_save(
    core: State<'_, Shared>,
    app: AppHandle,
    setlist: String,
    name: String,
) -> Result<LibItem, String> {
    let core = core.inner().clone();
    blocking(move || {
        let root = library_root(&app)?;
        let dir = setlist_dir(&root, &setlist)?;
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        core.require_fast_link("Salvar na biblioteca (lê capturas e IRs da pedaleira)")?;
        let label = name.trim().to_string();
        let file_stem = safe_name(&label)?;
        let session = core.session()?;
        let (fw, progress) = (firmware(&core), progress_fn(&app));
        let label2 = label.clone();
        let pkg = session
            .exclusive(move |ex| rig::package_from_pedal(ex, &label2, &fw, &progress))
            .map_err(|e| e.to_string())?;
        core.clear_unsaved(true, true);
        core.sync_all(&session);
        let pkg = pkg.map_err(|e| e.to_string())?;
        let path = unique_path(&dir, &file_stem);
        rig::write_package(&pkg, &path).map_err(|e| e.to_string())?;
        let setlist_name = dir
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_default();
        Ok(item(&path, &setlist_name, pkg.manifest))
    })
    .await
}

/// Send a library preset to the pedal: capture / IR go to a free slot unless an identical one is
/// already stored, then the preset is applied. With `save_slot` it is also written to that preset
/// slot (with its scenes); otherwise it stays live and unsaved.
#[tauri::command]
pub async fn library_load(
    core: State<'_, Shared>,
    app: AppHandle,
    path: String,
    save_slot: Option<u8>,
) -> Result<LoadReport, String> {
    let core = core.inner().clone();
    blocking(move || {
        core.require_fast_link("Carregar um pacote da biblioteca")?;
        let root = library_root(&app)?;
        let file = library_file(&root, &path)?;
        if save_slot.is_some_and(|s| u32::from(s) >= 32) {
            return Err("Preset fora do intervalo 1..32".into());
        }
        let pkg = rig::read_package(&file).map_err(|e| e.to_string())?;
        let session = core.session()?;
        let progress = progress_fn(&app);
        let res = session
            .exclusive(move |ex| rig::load_package(ex, &pkg, save_slot, &progress))
            .map_err(|e| e.to_string())?;
        core.clear_unsaved(true, true);
        core.sync_all(&session);
        if res.is_ok() && save_slot.is_none() {
            core.mark_unsaved(true, true);
        }
        res.map_err(|e| e.to_string())
    })
    .await
}

#[tauri::command]
pub async fn library_delete(app: AppHandle, path: String) -> Result<(), String> {
    blocking(move || {
        let file = library_file(&library_root(&app)?, &path)?;
        std::fs::remove_file(file).map_err(|e| e.to_string())
    })
    .await
}

/// Copy an external `.pulsedeck` file into a setlist (validated first).
#[tauri::command]
pub async fn library_import(
    app: AppHandle,
    path: String,
    setlist: String,
) -> Result<LibItem, String> {
    blocking(move || {
        let src = PathBuf::from(&path);
        let m = rig::read_package_manifest(&src).map_err(|e| e.to_string())?;
        let dir = setlist_dir(&library_root(&app)?, &setlist)?;
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let dest = unique_path(&dir, &safe_name(&m.name)?);
        std::fs::copy(&src, &dest).map_err(|e| e.to_string())?;
        let setlist_name = dir
            .file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_default();
        Ok(item(&dest, &setlist_name, m))
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_are_plain() {
        assert_eq!(safe_name("  Show / Ao vivo ").unwrap(), "Show _ Ao vivo");
        assert_eq!(safe_name("..\\..\\etc").unwrap(), "_.._etc");
        assert!(safe_name("   ").is_err());
        assert!(safe_name("...").is_err());
        assert_eq!(safe_name("a:b*c").unwrap(), "a_b_c");
    }

    #[test]
    fn library_files_must_live_in_the_library() {
        let base = std::env::temp_dir().join(format!("pd-lib-{}", std::process::id()));
        let root = base.join("library");
        std::fs::create_dir_all(root.join("Geral")).unwrap();
        let inside = root.join("Geral").join("a.pulsedeck");
        let outside = base.join("b.pulsedeck");
        let wrong_ext = root.join("Geral").join("c.txt");
        for f in [&inside, &outside, &wrong_ext] {
            std::fs::write(f, b"x").unwrap();
        }
        assert!(library_file(&root, inside.to_str().unwrap()).is_ok());
        assert!(library_file(&root, outside.to_str().unwrap()).is_err());
        assert!(library_file(&root, wrong_ext.to_str().unwrap()).is_err());
        assert!(library_file(
            &root,
            root.join("Geral/../../b.pulsedeck").to_str().unwrap()
        )
        .is_err());
        let u = unique_path(&root.join("Geral"), "a");
        assert_eq!(u.file_name().unwrap(), "a (2).pulsedeck");
        std::fs::remove_dir_all(base).unwrap();
    }
}
