//! Local file browser: lists folders, `.nam`, `.wav` and `.a2l` files and classifies `.nam` captures.

use pulsedeck_core::nam::{read_nam_info, NamInfo};
use serde::Serialize;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::SystemTime;

#[derive(Debug, Clone, Serialize)]
pub struct FileEntry {
    pub path: String,
    pub name: String,
    /// "dir" | "nam" | "wav" | "a2l"
    pub kind: String,
    pub size: u64,
    pub info: Option<NamInfo>,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct Listing {
    pub path: String,
    pub parent: Option<String>,
    pub entries: Vec<FileEntry>,
}

type CacheKey = (PathBuf, SystemTime, u64);

static CACHE: Mutex<Option<HashMap<CacheKey, Result<NamInfo, String>>>> = Mutex::new(None);

fn cached_info(path: &Path, mtime: SystemTime, size: u64) -> Result<NamInfo, String> {
    let key = (path.to_path_buf(), mtime, size);
    if let Some(hit) = CACHE.lock().unwrap().as_ref().and_then(|m| m.get(&key)) {
        return hit.clone();
    }
    let res = read_nam_info(path);
    CACHE
        .lock()
        .unwrap()
        .get_or_insert_with(HashMap::new)
        .insert(key, res.clone());
    res
}

pub fn list_folder(path: &Path) -> Result<Listing, String> {
    let rd = std::fs::read_dir(path).map_err(|e| format!("Não foi possível abrir a pasta: {e}"))?;
    let mut entries = Vec::new();
    for ent in rd.flatten() {
        let name = ent.file_name().to_string_lossy().to_string();
        if name.starts_with('.') {
            continue;
        }
        let p = ent.path();
        let Ok(meta) = ent.metadata() else { continue };
        let ext = p
            .extension()
            .map(|e| e.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        let kind = if meta.is_dir() {
            "dir"
        } else {
            match ext.as_str() {
                "nam" => "nam",
                "wav" => "wav",
                "a2l" => "a2l",
                _ => continue,
            }
        };
        let (mut info, mut error) = (None, None);
        if kind == "nam" {
            match cached_info(
                &p,
                meta.modified().unwrap_or(SystemTime::UNIX_EPOCH),
                meta.len(),
            ) {
                Ok(i) => info = Some(i),
                Err(e) => error = Some(e),
            }
        }
        entries.push(FileEntry {
            path: p.to_string_lossy().into(),
            name,
            kind: kind.into(),
            size: meta.len(),
            info,
            error,
        });
    }
    entries.sort_by(|a, b| {
        (a.kind != "dir", a.name.to_lowercase()).cmp(&(b.kind != "dir", b.name.to_lowercase()))
    });
    Ok(Listing {
        path: path.to_string_lossy().into(),
        parent: path.parent().map(|p| p.to_string_lossy().into()),
        entries,
    })
}
