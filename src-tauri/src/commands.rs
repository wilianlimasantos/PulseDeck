//! Tauri commands exposed to the React frontend. Every command that touches the serial line runs
//! on a blocking thread so the UI never freezes; the session itself serialises the commands.

use crate::files::{self, Listing};
use crate::state::{Shared, Snapshot};
use pulsedeck_core::a2l::{convert_file, ConvertOptions};
use pulsedeck_core::backup::{self, RestoreReport};
use pulsedeck_core::ir::{read_wav_first_channel, resample, TARGET_RATE};
use pulsedeck_core::nam::GearClass;
use pulsedeck_core::parse::{parse_fxa_id, AmpEq};
use pulsedeck_core::upload::{
    sanitize_name, upload_capture, upload_ir, CaptureJob, IrJob, Progress, ProgressFn,
};
use serde::Serialize;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tauri::{AppHandle, Emitter, Manager, State};

/// Effect category of the amp models (`amp_clean`, ...): kept out of the P/Q banks.
const AMP_CATEGORY: &str = "amp";

async fn blocking<T: Send + 'static>(
    f: impl FnOnce() -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f)
        .await
        .map_err(|e| e.to_string())?
}

#[tauri::command]
pub fn get_snapshot(core: State<'_, Shared>) -> Snapshot {
    core.snapshot()
}

#[derive(Serialize)]
pub struct ProtocolInfo {
    pub amp_eq_ranges: serde_json::Value,
    pub preset_count: u32,
    pub scene_count: u32,
    pub capture_slots: u8,
    pub ir_slots: u8,
    pub vendor_id: u16,
}

#[tauri::command]
pub fn protocol_info(core: State<'_, Shared>) -> ProtocolInfo {
    let raw = core.protocol.raw();
    let first_int = |s: &str| {
        s.split(|c: char| !c.is_ascii_digit())
            .find_map(|t| t.parse::<u32>().ok())
    };
    ProtocolInfo {
        amp_eq_ranges: raw["commands"]["block_editing"]["amp_eq"]["ranges"].clone(),
        preset_count: raw["conventions"]["preset_slots"]
            .as_str()
            .and_then(first_int)
            .unwrap_or(32),
        scene_count: 4,
        capture_slots: core.protocol.capture_slots,
        ir_slots: core.protocol.ir_slots,
        vendor_id: core.protocol.usb_vendor_id,
    }
}

#[tauri::command]
pub async fn set_preset(core: State<'_, Shared>, slot: u8) -> Result<(), String> {
    let core = core.inner().clone();
    blocking(move || {
        if slot >= 32 {
            return Err("Preset fora do intervalo 0..31".into());
        }
        let s = core.session()?;
        s.send(&format!("PLOAD {slot}"), None)
            .map_err(|e| e.to_string())?;
        core.sync_state(&s);
        Ok(())
    })
    .await
}

#[tauri::command]
pub async fn set_scene(core: State<'_, Shared>, scene: u8) -> Result<(), String> {
    let core = core.inner().clone();
    blocking(move || {
        if scene > 3 {
            return Err("Cena fora do intervalo 0..3".into());
        }
        let s = core.session()?;
        s.send(&format!("SNAP {scene}"), None)
            .map_err(|e| e.to_string())?;
        core.sync_state(&s);
        Ok(())
    })
    .await
}

/// Live knob change (`FXP`). Returns the value the pedal actually applied.
#[tauri::command]
pub async fn block_set_param(
    core: State<'_, Shared>,
    id: u32,
    index: u8,
    value: i32,
) -> Result<i32, String> {
    let core = core.inner().clone();
    blocking(move || {
        let r = core.cmd(&format!("FXP {id} {index} {value}"))?;
        core.mark_unsaved(false, true);
        let applied = r
            .terminal()
            .rsplit(' ')
            .next()
            .and_then(|v| v.parse::<i32>().ok())
            .unwrap_or(value);
        let mut s = core.snapshot.lock().unwrap();
        if let Some(b) = s.chain.blocks.iter_mut().find(|b| b.id == id) {
            if let Some(p) = b.params.get_mut(index as usize) {
                *p = applied;
            }
        }
        Ok(applied)
    })
    .await
}

#[tauri::command]
pub async fn block_set_on(core: State<'_, Shared>, id: u32, on: bool) -> Result<(), String> {
    let core = core.inner().clone();
    blocking(move || {
        let s = core.session()?;
        let res = s
            .send(&format!("FXE {id} {}", on as u8), None)
            .map_err(|e| e.to_string());
        core.mark_unsaved(false, true);
        let _ = core.sync_chain(&s);
        core.emit_snapshot();
        res.map(|_| ())
    })
    .await
}

#[tauri::command]
pub async fn amp_set_on(core: State<'_, Shared>, on: bool) -> Result<(), String> {
    let core = core.inner().clone();
    blocking(move || {
        let s = core.session()?;
        let res = s
            .send(&format!("AMP {}", on as u8), None)
            .map_err(|e| e.to_string());
        core.mark_unsaved(false, true);
        let _ = core.sync_chain(&s);
        core.emit_snapshot();
        res.map(|_| ())
    })
    .await
}

#[tauri::command]
pub async fn amp_set_eq(core: State<'_, Shared>, eq: AmpEq) -> Result<AmpEq, String> {
    let core = core.inner().clone();
    blocking(move || {
        let r = core.cmd(&format!(
            "AMPEQ {} {} {} {} {} {}",
            eq.volume, eq.bass, eq.mid, eq.treble, eq.presence, eq.drive
        ))?;
        core.mark_unsaved(false, true);
        let applied = pulsedeck_core::parse::parse_ampeq(r.terminal()).unwrap_or(eq);
        core.snapshot.lock().unwrap().ampeq = applied.clone();
        Ok(applied)
    })
    .await
}

fn valid_bank(b: &str) -> Result<&str, String> {
    if b == "P" || b == "Q" {
        Ok(b)
    } else {
        Err("Banco inválido (use P ou Q)".into())
    }
}

#[tauri::command]
pub async fn block_add(
    core: State<'_, Shared>,
    bank: String,
    position: u32,
    type_name: String,
) -> Result<u32, String> {
    let core = core.inner().clone();
    blocking(move || {
        let bank = valid_bank(&bank)?.to_string();
        match core.snapshot().effects.iter().find(|e| e.name == type_name) {
            None => return Err(format!("Efeito desconhecido: {type_name}")),
            // the amp lives in the central AMP column: two amps by accident is never wanted
            Some(e) if e.category == AMP_CATEGORY => {
                return Err("Amplificadores ficam só na coluna central AMP.".into())
            }
            Some(_) => {}
        }
        let s = core.session()?;
        let r = s
            .send(&format!("FXA {bank} {position} {type_name}"), None)
            .map_err(|e| e.to_string())?;
        let id = parse_fxa_id(&r.lines).ok_or("Resposta FXA inesperada")?;
        core.mark_unsaved(true, true);
        let _ = core.sync_chain(&s);
        core.emit_snapshot();
        Ok(id)
    })
    .await
}

#[tauri::command]
pub async fn block_remove(core: State<'_, Shared>, id: u32) -> Result<(), String> {
    let core = core.inner().clone();
    blocking(move || {
        let s = core.session()?;
        let res = s
            .send(&format!("FXR {id}"), None)
            .map_err(|e| e.to_string());
        core.mark_unsaved(true, true);
        let _ = core.sync_chain(&s);
        core.emit_snapshot();
        res.map(|_| ())
    })
    .await
}

#[tauri::command]
pub async fn block_move(
    core: State<'_, Shared>,
    id: u32,
    bank: String,
    position: u32,
) -> Result<(), String> {
    let core = core.inner().clone();
    blocking(move || {
        let bank = valid_bank(&bank)?.to_string();
        let s = core.session()?;
        let res = s
            .send(&format!("FXM {id} {bank} {position}"), None)
            .map_err(|e| e.to_string());
        core.mark_unsaved(true, true);
        let _ = core.sync_chain(&s);
        core.emit_snapshot();
        res.map(|_| ())
    })
    .await
}

#[tauri::command]
pub async fn capture_load(core: State<'_, Shared>, slot: u8) -> Result<(), String> {
    let core = core.inner().clone();
    blocking(move || {
        let s = core.session()?;
        s.send(&format!("CLOAD {slot}"), None)
            .map_err(|e| e.to_string())?;
        core.mark_unsaved(true, true);
        core.sync_amp(&s);
        let _ = core.sync_chain(&s);
        core.emit_snapshot();
        Ok(())
    })
    .await
}

#[tauri::command]
pub fn default_folder(app: AppHandle) -> String {
    app.path()
        .document_dir()
        .or_else(|_| app.path().home_dir())
        .unwrap_or_else(|_| PathBuf::from("/"))
        .to_string_lossy()
        .into()
}

#[tauri::command]
pub async fn list_folder(path: String) -> Result<Listing, String> {
    blocking(move || files::list_folder(Path::new(&path))).await
}

#[derive(Serialize)]
pub struct UploadOutcome {
    pub kind: String,
    pub slot: u8,
    pub name: String,
    pub bytes: usize,
    pub detail: String,
    pub class: Option<GearClass>,
    pub input_level_dbu: Option<f64>,
}

fn first_free(used: &[u8], total: u8) -> Option<u8> {
    (0..total).find(|s| !used.contains(s))
}

/// One-click send: `.nam` -> convert + capture upload, `.a2l` -> capture upload as is,
/// `.wav` -> IR upload. `slot = None` picks the first free slot.
#[tauri::command]
pub async fn upload_file(
    core: State<'_, Shared>,
    app: AppHandle,
    path: String,
    slot: Option<u8>,
) -> Result<UploadOutcome, String> {
    let core = core.inner().clone();
    blocking(move || {
        core.require_fast_link("Capturas (.nam) e IRs (.wav)")?;
        let session = core.session()?;
        let p = PathBuf::from(&path);
        let ext = p
            .extension()
            .map(|e| e.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        let stem = p
            .file_stem()
            .map(|s| s.to_string_lossy().to_string())
            .unwrap_or_else(|| "NAM".into());
        let app2 = app.clone();
        let progress: ProgressFn = Arc::new(move |pr: Progress| {
            let _ = app2.emit("pedal:progress", pr);
        });
        let _ = app.emit(
            "pedal:progress",
            Progress {
                stage: "Preparando".into(),
                done: 0,
                total: 1,
            },
        );

        core.sync_libraries(&session);
        let snap = core.snapshot();
        let res = match ext.as_str() {
            "nam" | "a2l" => {
                let used: Vec<u8> = snap.captures.entries.iter().map(|e| e.slot).collect();
                let slot = match slot {
                    Some(s) => s,
                    None => first_free(&used, core.protocol.capture_slots).ok_or(
                        "Todos os slots de captura estão ocupados: escolha um para sobrescrever.",
                    )?,
                };
                if slot >= core.protocol.capture_slots {
                    return Err(format!("Slot de captura inválido: {slot}"));
                }
                let (blob, name, class, level) = if ext == "nam" {
                    let _ = app.emit(
                        "pedal:progress",
                        Progress {
                            stage: "Convertendo .nam → A2L1".into(),
                            done: 0,
                            total: 1,
                        },
                    );
                    let (out, info) =
                        convert_file(&p, &ConvertOptions::default()).map_err(|e| e.to_string())?;
                    let name = info.name.clone().filter(|n| !n.is_empty()).unwrap_or(stem);
                    (out.blob, name, Some(info.class), info.input_level_dbu)
                } else {
                    (
                        std::fs::read(&p).map_err(|e| e.to_string())?,
                        stem,
                        None,
                        None,
                    )
                };
                let rep = upload_capture(
                    &session,
                    CaptureJob {
                        blob,
                        name: sanitize_name(&name),
                        slot,
                        input_level_dbu: level,
                    },
                    progress,
                )
                .map_err(|e| e.to_string())?;
                UploadOutcome {
                    kind: "capture".into(),
                    slot,
                    name: rep.name,
                    bytes: rep.bytes,
                    detail: rep.detail,
                    class,
                    input_level_dbu: level,
                }
            }
            "wav" => {
                let used: Vec<u8> = snap.irs.entries.iter().map(|e| e.slot).collect();
                let slot = match slot {
                    Some(s) => s,
                    None => first_free(&used, core.protocol.ir_slots).ok_or(
                        "Todos os slots de IR estão ocupados: escolha um para sobrescrever.",
                    )?,
                };
                if slot >= core.protocol.ir_slots {
                    return Err(format!("Slot de IR inválido: {slot}"));
                }
                let _ = app.emit(
                    "pedal:progress",
                    Progress {
                        stage: "Reamostrando WAV → 48 kHz".into(),
                        done: 0,
                        total: 1,
                    },
                );
                let (x, rate) = read_wav_first_channel(&p).map_err(|e| e.to_string())?;
                let mono = resample(&x, rate, TARGET_RATE, 8192);
                let rep = upload_ir(
                    &session,
                    IrJob {
                        mono48: mono,
                        name: sanitize_name(&stem),
                        slot: Some(slot),
                        assign_block: None,
                    },
                    progress,
                )
                .map_err(|e| e.to_string())?;
                UploadOutcome {
                    kind: "ir".into(),
                    slot,
                    name: rep.name,
                    bytes: rep.bytes,
                    detail: rep.detail,
                    class: None,
                    input_level_dbu: None,
                }
            }
            other => return Err(format!("Tipo de arquivo não suportado: .{other}")),
        };
        core.sync_libraries(&session);
        core.sync_amp(&session);
        let _ = core.sync_chain(&session);
        core.emit_snapshot();
        let _ = app.emit(
            "pedal:progress",
            Progress {
                stage: "Concluído".into(),
                done: 1,
                total: 1,
            },
        );
        Ok(res)
    })
    .await
}

/// Restore the neural amp / capture block (`AMPX 1`) or remove it (`AMPX 0`).
/// (The protocol re-adds it at the P/Q boundary with AMPX; FXA only creates effect blocks.)
#[tauri::command]
pub async fn amp_set_present(core: State<'_, Shared>, present: bool) -> Result<(), String> {
    let core = core.inner().clone();
    blocking(move || {
        let s = core.session()?;
        let res = s
            .send(&format!("AMPX {}", present as u8), None)
            .map_err(|e| e.to_string());
        core.mark_unsaved(true, true);
        core.sync_amp(&s);
        let _ = core.sync_chain(&s);
        core.emit_snapshot();
        res.map(|_| ())
    })
    .await
}

/// `PSAVE <slot> <name>`: write the sounding chain to flash.
#[tauri::command]
pub async fn save_preset(core: State<'_, Shared>, slot: u8, name: String) -> Result<(), String> {
    let core = core.inner().clone();
    blocking(move || {
        if slot >= 32 {
            return Err("Preset fora do intervalo 0..31".into());
        }
        let s = core.session()?;
        s.send(&format!("PSAVE {slot} {}", sanitize_name(&name)), None)
            .map_err(|e| e.to_string())?;
        core.clear_unsaved(true, false);
        core.sync_state(&s);
        Ok(())
    })
    .await
}

/// `SNAPSAVE <scene>`: store the current state as that scene of the current preset.
#[tauri::command]
pub async fn save_scene(core: State<'_, Shared>, scene: u8) -> Result<(), String> {
    let core = core.inner().clone();
    blocking(move || {
        if scene > 3 {
            return Err("Cena fora do intervalo 0..3".into());
        }
        let s = core.session()?;
        s.send(&format!("SNAPSAVE {scene}"), None)
            .map_err(|e| e.to_string())?;
        core.clear_unsaved(false, true);
        core.sync_state(&s);
        Ok(())
    })
    .await
}

#[derive(Serialize)]
pub struct BackupSummary {
    pub presets: usize,
    pub captures: usize,
    pub irs: usize,
    pub globals: usize,
    pub warnings: Vec<String>,
}

fn progress_fn(app: &AppHandle) -> ProgressFn {
    let app = app.clone();
    Arc::new(move |p: Progress| {
        let _ = app.emit("pedal:progress", p);
    })
}

/// Full backup to a `.zip`: 32 presets (with scenes), capture / IR slots, global settings.
#[tauri::command]
pub async fn export_backup(
    core: State<'_, Shared>,
    app: AppHandle,
    path: String,
) -> Result<BackupSummary, String> {
    let core = core.inner().clone();
    blocking(move || {
        core.require_fast_link("O backup completo")?;
        let session = core.session()?;
        let firmware = match core.snapshot().status {
            pulsedeck_core::manager::ConnectionStatus::Connected { pong, .. } => pong,
            _ => String::new(),
        };
        let progress = progress_fn(&app);
        let p2 = progress.clone();
        let result = session
            .exclusive(move |ex| backup::export(ex, &firmware, &p2))
            .map_err(|e| e.to_string())?;
        // the export loads each preset in turn: re-read what the pedal is showing now
        core.clear_unsaved(true, true);
        core.sync_all(&session);
        let bk = result.map_err(|e| e.to_string())?;
        backup::write_zip(&bk, Path::new(&path)).map_err(|e| e.to_string())?;
        progress(Progress {
            stage: "Backup salvo".into(),
            done: 1,
            total: 1,
        });
        Ok(BackupSummary {
            presets: bk.presets.len(),
            captures: bk.captures.len(),
            irs: bk.irs.len(),
            globals: bk.globals.settings.len(),
            warnings: bk
                .globals
                .unsupported
                .iter()
                .map(|c| format!("{c}: sem resposta da pedaleira (não incluído)"))
                .collect(),
        })
    })
    .await
}

/// Restore a `.zip` made by `export_backup` (overwrites the slots it contains).
#[tauri::command]
pub async fn restore_backup(
    core: State<'_, Shared>,
    app: AppHandle,
    path: String,
) -> Result<BackupSummary, String> {
    let core = core.inner().clone();
    blocking(move || {
        core.require_fast_link("A restauração de backup")?;
        let bk = backup::read_zip(Path::new(&path)).map_err(|e| e.to_string())?;
        let session = core.session()?;
        let progress = progress_fn(&app);
        let result = session
            .exclusive(move |ex| backup::restore(ex, &bk, &progress))
            .map_err(|e| e.to_string())?;
        core.clear_unsaved(true, true);
        core.globals_dirty
            .store(false, std::sync::atomic::Ordering::SeqCst); // restore ends with GSAVE
        core.sync_all(&session);
        let rep: RestoreReport = result.map_err(|e| e.to_string())?;
        Ok(BackupSummary {
            presets: rep.presets,
            captures: rep.captures,
            irs: rep.irs,
            globals: rep.globals,
            warnings: rep.warnings,
        })
    })
    .await
}
