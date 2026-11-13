//! Pedal memory management (capture / IR slots) and self-contained `.pulsedeck` preset packages.
//!
//! * [`scan_usage`] finds which presets use each capture / IR slot (it loads every preset in turn
//!   and puts the pedal back where it was).
//! * [`rename_slot`] has no dedicated protocol command: the stored item is read back and written
//!   again under the new name (same slot, same trim / gain).
//! * [`Package`] bundles one preset (with its scenes) together with the capture and IR data it
//!   references, so loading it never depends on what happens to be stored on a given pedal.

use crate::backup::{
    read_blob, read_chunk, read_current_preset, rebuild_preset, step, BackupError, LibBk, PresetBk,
};
use crate::ir::IrPayload;
use crate::parse::{kv_map, parse_fxs, parse_library, parse_lvl, parse_plist, LibEntry};
use crate::session::{CmdError, Wire};
use crate::upload::{
    sanitize_name, upload_capture_wire, upload_ir_payload_wire, CaptureJob, ProgressFn,
};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};
use std::io::{Read, Seek, Write};
use std::path::Path;

/// Which kind of library slot an operation targets.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum SlotKind {
    Capture,
    Ir,
}

impl SlotKind {
    fn list_cmd(self) -> &'static str {
        match self {
            SlotKind::Capture => "CLIST",
            SlotKind::Ir => "ILIST",
        }
    }
    fn parse(self, lines: &[String]) -> crate::parse::Library {
        match self {
            SlotKind::Capture => parse_library(lines, "CAP", "CLIST end"),
            SlotKind::Ir => parse_library(lines, "IRS", "ILIST end"),
        }
    }
    fn read_cmd(self) -> &'static str {
        match self {
            SlotKind::Capture => "CREAD",
            SlotKind::Ir => "IREAD",
        }
    }
    fn delete_cmd(self) -> &'static str {
        match self {
            SlotKind::Capture => "CDEL",
            SlotKind::Ir => "IDEL",
        }
    }
}

/// Read-back piece size of `CREAD`/`IREAD` (protocol: up to 96 bytes).
const READ_PIECE: usize = 96;

fn list(w: &mut dyn Wire, kind: SlotKind) -> Result<Vec<LibEntry>, BackupError> {
    Ok(kind.parse(&w.send(kind.list_cmd())?.lines).entries)
}

// ------------------------------------------------------------------------------- usage

/// Presets (by slot) that use each capture slot / IR slot.
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct MemoryUsage {
    pub captures: BTreeMap<u8, Vec<u8>>,
    pub irs: BTreeMap<u8, Vec<u8>>,
}

/// IR slots referenced by the `ir_slot` parameter of the IR blocks of a chain.
fn ir_slots_in(blocks: &[(bool, Vec<i32>)], types: &[String], ir: &(String, usize)) -> Vec<u8> {
    blocks
        .iter()
        .zip(types)
        .filter(|(_, t)| **t == ir.0)
        .filter_map(|((_, p), _)| p.get(ir.1).copied())
        .filter(|v| *v > 0)
        .filter_map(|v| u8::try_from(v - 1).ok())
        .collect()
}

/// Load every stored preset and record its capture / IR slots. Restores the previously active
/// preset and scene afterwards (unsaved edits on the pedal are lost: callers should warn).
pub fn scan_usage(w: &mut dyn Wire, progress: &ProgressFn) -> Result<MemoryUsage, BackupError> {
    let ir = w.protocol().ir_slot_param();
    let lvl = w.send("LVL").ok().and_then(|r| parse_lvl(r.terminal()));
    let plist = parse_plist(&w.send("PLIST")?.lines);
    let total = plist.presets.len();
    let mut usage = MemoryUsage::default();
    for (i, p) in plist.presets.iter().enumerate() {
        step(
            progress,
            &format!("Analisando preset {}", p.slot + 1),
            i,
            total,
        );
        match w.send(&format!("PLOAD {}", p.slot)) {
            Ok(_) => {}
            Err(CmdError::Device { .. }) => continue,
            Err(e) => return Err(e.into()),
        }
        let cap = w
            .send("CLIST")
            .ok()
            .map(|r| parse_library(&r.lines, "CAP", "CLIST end"))
            .and_then(|l| {
                let cur = u8::try_from(l.current?).ok()?;
                l.entries.iter().any(|e| e.slot == cur).then_some(cur)
            });
        if let Some(c) = cap {
            usage.captures.entry(c).or_default().push(p.slot);
        }
        if let Some(ir) = &ir {
            let chain = parse_fxs(&w.send("FXS")?.lines);
            let types: Vec<String> = chain.blocks.iter().map(|b| b.type_name.clone()).collect();
            let st: Vec<(bool, Vec<i32>)> = chain
                .blocks
                .iter()
                .map(|b| (b.on, b.params.clone()))
                .collect();
            let slots: BTreeSet<u8> = ir_slots_in(&st, &types, ir).into_iter().collect();
            for s in slots {
                usage.irs.entry(s).or_default().push(p.slot);
            }
        }
    }
    if let Some(p) = lvl.as_ref().and_then(|l| l.preset).filter(|p| *p >= 0) {
        let _ = w.send(&format!("PLOAD {p}"));
        if let Some(s) = lvl.as_ref().and_then(|l| l.scene).filter(|s| *s >= 0) {
            let _ = w.send(&format!("SNAP {s}"));
        }
    }
    step(progress, "Uso dos slots lido", total, total);
    Ok(usage)
}

// ------------------------------------------------------------------- rename / delete

pub fn delete_slot(w: &mut dyn Wire, kind: SlotKind, slot: u8) -> Result<(), BackupError> {
    w.send(&format!("{} {slot}", kind.delete_cmd()))?;
    Ok(())
}

/// Rename by reading the slot back and writing it again under the new name. Returns warnings
/// (e.g. trim could not be re-applied). The slot content is unchanged otherwise.
pub fn rename_slot(
    w: &mut dyn Wire,
    kind: SlotKind,
    slot: u8,
    new_name: &str,
    progress: &ProgressFn,
) -> Result<Vec<String>, BackupError> {
    let name = sanitize_name(new_name);
    if new_name.trim().is_empty() {
        return Err(BackupError::Invalid("o nome não pode ficar vazio".into()));
    }
    let entry = list(w, kind)?
        .into_iter()
        .find(|e| e.slot == slot)
        .ok_or_else(|| BackupError::Invalid(format!("slot {} está vazio", slot + 1)))?;
    let (info, data) = read_blob(w, kind.read_cmd(), slot)?;
    let mut warnings = Vec::new();
    match kind {
        SlotKind::Capture => {
            let job = CaptureJob {
                blob: data,
                name,
                slot,
                input_level_dbu: entry.input_dbu,
            };
            upload_capture_wire(w, &job, progress)?;
            if let Some(trim) = entry.trim_db.filter(|t| *t != 0.0) {
                if let Err(e) = w.send(&format!("CTRIM {slot} {trim} -")) {
                    warnings.push(format!("trim de {trim} dB não reaplicado: {e}"));
                }
            }
        }
        SlotKind::Ir => {
            let kv = kv_map(&info);
            let gain = kv.get("aux").and_then(|v| v.parse().ok()).unwrap_or(0);
            let samples = le_i16(&data);
            upload_ir_payload_wire(
                w,
                &IrPayload {
                    samples,
                    gain_mdb: gain,
                },
                &name,
                slot,
                progress,
            )?;
        }
    }
    Ok(warnings)
}

fn le_i16(data: &[u8]) -> Vec<i16> {
    data.as_chunks::<2>()
        .0
        .iter()
        .map(|c| i16::from_le_bytes(*c))
        .collect()
}

// ------------------------------------------------------------------------ .pulsedeck

pub const PKG_FORMAT: &str = "pulsedeck-preset";
pub const PKG_VERSION: u32 = 1;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct PkgManifest {
    pub format: String,
    pub version: u32,
    pub name: String,
    pub created_unix: u64,
    pub firmware: String,
    pub has_capture: bool,
    pub ir_count: usize,
    pub scenes: usize,
}

/// One preset (chain + scenes) plus the capture / IR blobs it needs.
#[derive(Debug, Clone, PartialEq)]
pub struct Package {
    pub manifest: PkgManifest,
    pub preset: PresetBk,
    pub capture: Option<LibBk>,
    pub irs: Vec<LibBk>,
}

fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// Snapshot the preset that is currently loaded on the pedal, with its capture and IR data.
pub fn package_from_pedal(
    w: &mut dyn Wire,
    name: &str,
    firmware: &str,
    progress: &ProgressFn,
) -> Result<Package, BackupError> {
    step(progress, "Lendo o preset", 0, 3);
    let slot = w
        .send("LVL")
        .ok()
        .and_then(|r| parse_lvl(r.terminal()))
        .and_then(|l| l.preset)
        .and_then(|p| u8::try_from(p).ok())
        .unwrap_or(0);
    let preset = read_current_preset(w, slot, name, None)?;
    let ir = w.protocol().ir_slot_param();

    step(progress, "Lendo a captura", 1, 3);
    let capture = match preset.capture_slot {
        Some(s) => {
            let e = list(w, SlotKind::Capture)?
                .into_iter()
                .find(|e| e.slot == s);
            match e {
                Some(e) => {
                    let (info, data) = read_blob(w, "CREAD", s)?;
                    Some(LibBk {
                        slot: s,
                        name: e.name,
                        info,
                        trim_db: e.trim_db,
                        input_dbu: e.input_dbu,
                        data,
                    })
                }
                None => None,
            }
        }
        None => None,
    };

    step(progress, "Lendo os IRs", 2, 3);
    let mut irs = Vec::new();
    if let Some(ir) = &ir {
        let wanted = ir_slots_of(&preset, ir);
        let stored = list(w, SlotKind::Ir)?;
        for s in wanted {
            if let Some(e) = stored.iter().find(|e| e.slot == s) {
                let (info, data) = read_blob(w, "IREAD", s)?;
                irs.push(LibBk {
                    slot: s,
                    name: e.name.clone(),
                    info,
                    trim_db: None,
                    input_dbu: None,
                    data,
                });
            }
        }
    }
    step(progress, "Preset lido", 3, 3);
    Ok(Package {
        manifest: PkgManifest {
            format: PKG_FORMAT.into(),
            version: PKG_VERSION,
            name: name.into(),
            created_unix: now(),
            firmware: firmware.into(),
            has_capture: capture.is_some(),
            ir_count: irs.len(),
            scenes: preset.scenes.len(),
        },
        preset,
        capture,
        irs,
    })
}

fn states(p: &PresetBk) -> impl Iterator<Item = &crate::backup::StateBk> {
    std::iter::once(&p.base).chain(p.scenes.iter().map(|s| &s.state))
}

fn ir_slots_of(p: &PresetBk, ir: &(String, usize)) -> BTreeSet<u8> {
    let types: Vec<String> = p.blocks.iter().map(|b| b.type_name.clone()).collect();
    states(p)
        .flat_map(|s| ir_slots_in(&s.blocks, &types, ir))
        .collect()
}

/// Rewrite the IR slot references of a preset (old slot -> new slot).
fn remap_ir(p: &mut PresetBk, ir: &(String, usize), map: &BTreeMap<u8, u8>) {
    let fix = |params: &mut Vec<i32>| {
        if let Some(v) = params.get_mut(ir.1) {
            if *v > 0 {
                if let Some(n) = u8::try_from(*v - 1).ok().and_then(|o| map.get(&o)) {
                    *v = i32::from(*n) + 1;
                }
            }
        }
    };
    let types: Vec<String> = p.blocks.iter().map(|b| b.type_name.clone()).collect();
    for (b, t) in p.blocks.iter_mut().zip(&types) {
        if *t == ir.0 {
            fix(&mut b.params);
        }
    }
    let fix_state = |st: &mut crate::backup::StateBk| {
        for ((_, params), t) in st.blocks.iter_mut().zip(&types) {
            if *t == ir.0 {
                fix(params);
            }
        }
    };
    fix_state(&mut p.base);
    for s in &mut p.scenes {
        fix_state(&mut s.state);
    }
}

/// Slot on the pedal that already holds exactly this data, if any.
///
/// A whole-stream CRC cannot tell captures apart (the blob ends with its own CRC32, so the CRC of
/// the stream is a constant), so candidates of equal length are compared by content: first the
/// last read-back piece (it contains the blob's own CRC), then the full data.
fn find_same(
    w: &mut dyn Wire,
    kind: SlotKind,
    entries: &[LibEntry],
    data: &[u8],
) -> Result<Option<u8>, BackupError> {
    let crc = format!("{:08x}", crc32fast::hash(data));
    let tail_off = (data.len().saturating_sub(1) / READ_PIECE) * READ_PIECE;
    for e in entries.iter().filter(|e| e.bytes as usize == data.len()) {
        let info = w
            .send(&format!("{} {}", kind.read_cmd(), e.slot))?
            .terminal()
            .to_string();
        if kv_map(&info).get("crc").copied() != Some(crc.as_str()) {
            continue;
        }
        if read_chunk(w, kind.read_cmd(), e.slot, tail_off)? != data[tail_off..] {
            continue;
        }
        let (_, stored) = read_blob(w, kind.read_cmd(), e.slot)?;
        if stored == data {
            return Ok(Some(e.slot));
        }
    }
    Ok(None)
}

fn first_free(entries: &[LibEntry], count: u8) -> Option<u8> {
    (0..count).find(|s| !entries.iter().any(|e| e.slot == *s))
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct LoadReport {
    pub capture_slot: Option<u8>,
    pub capture_uploaded: bool,
    pub ir_slots: Vec<u8>,
    pub irs_uploaded: usize,
    pub saved_slot: Option<u8>,
    pub warnings: Vec<String>,
}

/// Put a package on the pedal: reuse identical capture / IR slots or upload to free ones, then
/// rebuild the preset live. With `save_slot` the preset (and its scenes) is also written to flash.
pub fn load_package(
    w: &mut dyn Wire,
    pkg: &Package,
    save_slot: Option<u8>,
    progress: &ProgressFn,
) -> Result<LoadReport, BackupError> {
    let mut rep = LoadReport::default();
    let mut preset = pkg.preset.clone();
    let quiet: ProgressFn = std::sync::Arc::new(|_| {});
    let total = 2 + pkg.irs.len();
    let mut done = 0;

    if let Some(c) = &pkg.capture {
        step(progress, "Capturas", done, total);
        let entries = list(w, SlotKind::Capture)?;
        let slot = match find_same(w, SlotKind::Capture, &entries, &c.data)? {
            Some(s) => s,
            None => {
                let count = w.protocol().capture_slots;
                let s = first_free(&entries, count).ok_or_else(|| {
                    BackupError::Invalid(
                        "Todos os slots de captura estão ocupados: libere um na Memória da Pedaleira.".into(),
                    )
                })?;
                upload_capture_wire(
                    w,
                    &CaptureJob {
                        blob: c.data.clone(),
                        name: c.name.clone(),
                        slot: s,
                        input_level_dbu: c.input_dbu,
                    },
                    &quiet,
                )?;
                if let Some(t) = c.trim_db.filter(|t| *t != 0.0) {
                    if let Err(e) = w.send(&format!("CTRIM {s} {t} -")) {
                        rep.warnings.push(format!("trim não reaplicado: {e}"));
                    }
                }
                rep.capture_uploaded = true;
                s
            }
        };
        preset.capture_slot = Some(slot);
        rep.capture_slot = Some(slot);
    }
    done += 1;

    let mut map = BTreeMap::new();
    for e in &pkg.irs {
        step(progress, &format!("IR {}", e.name), done, total);
        done += 1;
        let entries = list(w, SlotKind::Ir)?;
        let slot = match find_same(w, SlotKind::Ir, &entries, &e.data)? {
            Some(s) => s,
            None => {
                let count = w.protocol().ir_slots;
                let s = first_free(&entries, count).ok_or_else(|| {
                    BackupError::Invalid(
                        "Todos os slots de IR estão ocupados: libere um na Memória da Pedaleira."
                            .into(),
                    )
                })?;
                let gain = kv_map(&e.info)
                    .get("aux")
                    .and_then(|v| v.parse().ok())
                    .unwrap_or(0);
                upload_ir_payload_wire(
                    w,
                    &IrPayload {
                        samples: le_i16(&e.data),
                        gain_mdb: gain,
                    },
                    &e.name,
                    s,
                    &quiet,
                )?;
                rep.irs_uploaded += 1;
                s
            }
        };
        map.insert(e.slot, slot);
        rep.ir_slots.push(slot);
    }
    if let Some(ir) = w.protocol().ir_slot_param() {
        remap_ir(&mut preset, &ir, &map);
    }

    step(progress, "Aplicando o preset", done, total);
    if !rebuild_preset(w, &preset, save_slot, &mut rep.warnings) {
        return Err(BackupError::Invalid(
            "a pedaleira recusou criar o preset".into(),
        ));
    }
    rep.saved_slot = save_slot;
    step(progress, "Preset carregado", total, total);
    Ok(rep)
}

// ------------------------------------------------------------------------- zip format

fn zerr(e: impl std::fmt::Display) -> BackupError {
    BackupError::Zip(e.to_string())
}

fn json<T: Serialize>(v: &T) -> Result<Vec<u8>, BackupError> {
    serde_json::to_vec_pretty(v).map_err(zerr)
}

pub fn write_package_to<W: Write + Seek>(pkg: &Package, out: W) -> Result<(), BackupError> {
    let mut z = zip::ZipWriter::new(out);
    let opts = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    let mut put = |name: String, data: &[u8]| -> Result<(), BackupError> {
        z.start_file(name, opts).map_err(zerr)?;
        z.write_all(data).map_err(zerr)
    };
    put("manifest.json".into(), &json(&pkg.manifest)?)?;
    put("preset.json".into(), &json(&pkg.preset)?)?;
    if let Some(c) = &pkg.capture {
        put("capture.json".into(), &json(c)?)?;
        put("capture.bin".into(), &c.data)?;
    }
    for e in &pkg.irs {
        put(format!("irs/{:02}.json", e.slot), &json(e)?)?;
        put(format!("irs/{:02}.bin", e.slot), &e.data)?;
    }
    z.finish().map_err(zerr)?;
    Ok(())
}

pub fn write_package(pkg: &Package, path: &Path) -> Result<(), BackupError> {
    let f = std::fs::File::create(path).map_err(zerr)?;
    write_package_to(pkg, std::io::BufWriter::new(f))
}

pub fn read_package_from<R: Read + Seek>(r: R) -> Result<Package, BackupError> {
    let mut z = zip::ZipArchive::new(r).map_err(zerr)?;
    let mut read = |name: &str| -> Result<Vec<u8>, BackupError> {
        let mut f = z
            .by_name(name)
            .map_err(|_| BackupError::Zip(format!("falta {name} no pacote")))?;
        let mut v = Vec::new();
        f.read_to_end(&mut v).map_err(zerr)?;
        Ok(v)
    };
    let manifest: PkgManifest = serde_json::from_slice(&read("manifest.json")?).map_err(zerr)?;
    if manifest.format != PKG_FORMAT || manifest.version != PKG_VERSION {
        return Err(BackupError::Invalid(format!(
            "não é um preset PulseDeck v{PKG_VERSION} (formato '{}' v{})",
            manifest.format, manifest.version
        )));
    }
    let preset: PresetBk = serde_json::from_slice(&read("preset.json")?).map_err(zerr)?;
    let capture = if manifest.has_capture {
        let mut c: LibBk = serde_json::from_slice(&read("capture.json")?).map_err(zerr)?;
        c.data = read("capture.bin")?;
        Some(c)
    } else {
        None
    };
    let mut irs = Vec::new();
    let names: Vec<String> = z
        .file_names()
        .filter_map(|n| n.ok().map(|c| c.into_owned()))
        .filter(|n| n.starts_with("irs/") && n.ends_with(".json"))
        .collect();
    let mut read = |name: &str| -> Result<Vec<u8>, BackupError> {
        let mut f = z
            .by_name(name)
            .map_err(|_| BackupError::Zip(format!("falta {name} no pacote")))?;
        let mut v = Vec::new();
        f.read_to_end(&mut v).map_err(zerr)?;
        Ok(v)
    };
    for n in names {
        let mut e: LibBk = serde_json::from_slice(&read(&n)?).map_err(zerr)?;
        e.data = read(&format!("irs/{:02}.bin", e.slot))?;
        irs.push(e);
    }
    irs.sort_by_key(|e| e.slot);
    Ok(Package {
        manifest,
        preset,
        capture,
        irs,
    })
}

pub fn read_package(path: &Path) -> Result<Package, BackupError> {
    read_package_from(std::io::BufReader::new(
        std::fs::File::open(path).map_err(zerr)?,
    ))
}

/// Only the manifest, for listings (reads one small entry, not the blobs).
pub fn read_package_manifest(path: &Path) -> Result<PkgManifest, BackupError> {
    let f = std::fs::File::open(path).map_err(zerr)?;
    let mut z = zip::ZipArchive::new(std::io::BufReader::new(f)).map_err(zerr)?;
    let mut v = Vec::new();
    z.by_name("manifest.json")
        .map_err(|_| BackupError::Zip("falta manifest.json".into()))?
        .read_to_end(&mut v)
        .map_err(zerr)?;
    let m: PkgManifest = serde_json::from_slice(&v).map_err(zerr)?;
    if m.format != PKG_FORMAT {
        return Err(BackupError::Invalid("não é um preset PulseDeck".into()));
    }
    Ok(m)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::Protocol;
    use crate::session::{EventSink, Session, SessionWire};
    use crate::sim::{SimLink, SimState};
    use crate::upload::{upload_ir_wire, IrJob};
    use std::io::Cursor;
    use std::sync::{Arc, Mutex};

    fn pedal() -> (SessionWire, Arc<Mutex<SimState>>) {
        let p = Arc::new(Protocol::embedded());
        let st = Arc::new(Mutex::new(SimState::new(&p)));
        let sink: EventSink = Arc::new(|_| {});
        let s = Arc::new(Session::start(
            Box::new(SimLink::new(st.clone())),
            p.clone(),
            sink,
        ));
        s.set_max_line(190);
        (
            SessionWire {
                session: s,
                protocol: p,
            },
            st,
        )
    }

    fn quiet() -> ProgressFn {
        Arc::new(|_| {})
    }

    fn put_capture(w: &mut SessionWire, slot: u8, name: &str, seed: u32) {
        let nam = crate::a2l::testutil::synthetic_nam(
            &[1, 2, 4],
            3,
            2,
            seed,
            serde_json::json!({ "name": name }),
        );
        let blob = crate::a2l::convert_str(&nam, &Default::default())
            .unwrap()
            .blob;
        upload_capture_wire(
            w,
            &CaptureJob {
                blob,
                name: name.into(),
                slot,
                input_level_dbu: None,
            },
            &quiet(),
        )
        .unwrap();
    }

    fn put_ir(w: &mut SessionWire, slot: u8, name: &str, decay: f64) {
        let x: Vec<f64> = (0..3000)
            .map(|i| (i as f64 * 0.1).sin() * (-(i as f64) / decay).exp())
            .collect();
        upload_ir_wire(
            w,
            &IrJob {
                mono48: x,
                name: name.into(),
                slot: Some(slot),
                assign_block: None,
            },
            &quiet(),
        )
        .unwrap();
    }

    /// Source pedal: capture in slot 4 and IR in slot 3, both used by preset 0.
    fn source() -> (SessionWire, Arc<Mutex<SimState>>) {
        let (mut w, st) = pedal();
        put_capture(&mut w, 4, "Src Amp", 11);
        put_ir(&mut w, 3, "Src IR", 500.0);
        w.send("PLOAD 0").unwrap();
        w.send("CLOAD 4").unwrap();
        let (ir_name, ir_idx) = w.protocol().ir_slot_param().unwrap();
        let ir = parse_fxs(&w.send("FXS").unwrap().lines)
            .blocks
            .into_iter()
            .find(|b| b.type_name == ir_name)
            .unwrap();
        w.send(&format!("FXP {} {ir_idx} 4", ir.id)).unwrap(); // slot 3 + 1
        w.send("PSAVE 0 Lead Tone").unwrap();
        w.send("SNAPSAVE 0").unwrap();
        w.send("SNAPSAVE 2").unwrap();
        w.send("PLOAD 0").unwrap();
        (w, st)
    }

    #[test]
    fn usage_lists_presets_per_slot_and_restores_position() {
        let (mut w, st) = source();
        w.send("PLOAD 1").unwrap();
        let u = scan_usage(&mut w, &quiet()).unwrap();
        assert_eq!(u.captures.get(&4), Some(&vec![0]));
        assert_eq!(u.irs.get(&3), Some(&vec![0]));
        assert_eq!(st.lock().unwrap().preset, 1);
    }

    #[test]
    fn rename_keeps_data_and_slot() {
        let (mut w, st) = source();
        let before_c = st.lock().unwrap().blobs[&('C', 4)].clone();
        let before_i = st.lock().unwrap().blobs[&('I', 3)].clone();
        rename_slot(&mut w, SlotKind::Capture, 4, "Novo Amp", &quiet()).unwrap();
        rename_slot(&mut w, SlotKind::Ir, 3, "Novo IR", &quiet()).unwrap();
        let s = st.lock().unwrap();
        assert_eq!(s.captures[4].as_ref().unwrap().0, "Novo Amp");
        assert_eq!(s.irs[3].as_ref().unwrap().0, "Novo IR");
        assert_eq!(s.blobs[&('C', 4)], before_c);
        assert_eq!(s.blobs[&('I', 3)], before_i);
        drop(s);
        assert!(rename_slot(&mut w, SlotKind::Capture, 9, "x", &quiet()).is_err());
        assert!(rename_slot(&mut w, SlotKind::Capture, 4, "  ", &quiet()).is_err());
    }

    #[test]
    fn delete_frees_the_slot() {
        let (mut w, st) = source();
        delete_slot(&mut w, SlotKind::Capture, 4).unwrap();
        delete_slot(&mut w, SlotKind::Ir, 3).unwrap();
        let s = st.lock().unwrap();
        assert!(s.captures[4].is_none() && s.irs[3].is_none());
        drop(s);
        assert!(delete_slot(&mut w, SlotKind::Ir, 3).is_err());
    }

    #[test]
    fn package_roundtrip_remaps_slots_and_reuses_identical_data() {
        let (mut src, _) = source();
        let pkg = package_from_pedal(&mut src, "Lead Tone", "tankg M5.26", &quiet()).unwrap();
        assert_eq!(pkg.capture.as_ref().unwrap().name, "Src Amp");
        assert_eq!(pkg.irs.len(), 1);
        assert_eq!(pkg.manifest.scenes, 2);

        let mut buf = Cursor::new(Vec::new());
        write_package_to(&pkg, &mut buf).unwrap();
        let back = read_package_from(Cursor::new(buf.into_inner())).unwrap();
        assert_eq!(back, pkg);

        // destination: capture slot 0 and IR slot 0 taken by something else
        let (mut dst, dst_state) = pedal();
        put_capture(&mut dst, 0, "Other", 99);
        put_ir(&mut dst, 0, "Other IR", 200.0);
        let rep = load_package(&mut dst, &back, Some(5), &quiet()).unwrap();
        assert!(rep.warnings.is_empty(), "{:?}", rep.warnings);
        assert!(rep.capture_uploaded && rep.irs_uploaded == 1);
        assert_eq!(rep.capture_slot, Some(1));
        assert_eq!(rep.ir_slots, vec![1]);
        {
            let d = dst_state.lock().unwrap();
            assert_eq!(d.captures[1].as_ref().unwrap().0, "Src Amp");
            assert_eq!(d.irs[1].as_ref().unwrap().0, "Src IR");
            let p = &d.stored[&5];
            assert_eq!(p.capture, 1);
            assert!(p.scenes[0].is_some() && p.scenes[2].is_some() && p.scenes[1].is_none());
            let (_, idx) = Protocol::embedded().ir_slot_param().unwrap();
            let ir_ty = Protocol::embedded().ir_slot_param().unwrap().0;
            let b = p.blocks.iter().find(|b| b.type_name == ir_ty).unwrap();
            assert_eq!(b.params[idx], 2, "IR reference remapped to slot 1 (+1)");
        }

        // loading again reuses the identical slots instead of uploading twice
        let rep2 = load_package(&mut dst, &back, None, &quiet()).unwrap();
        assert!(!rep2.capture_uploaded && rep2.irs_uploaded == 0);
        assert_eq!(rep2.capture_slot, Some(1));
        assert_eq!(rep2.saved_slot, None);
    }

    #[test]
    fn load_fails_clearly_when_no_capture_slot_is_free() {
        let (mut src, _) = source();
        let pkg = package_from_pedal(&mut src, "x", "", &quiet()).unwrap();
        let (mut dst, _) = pedal();
        for s in 0..15 {
            put_capture(&mut dst, s, &format!("C{s}"), 100 + u32::from(s));
        }
        let err = load_package(&mut dst, &pkg, None, &quiet()).unwrap_err();
        assert!(err.to_string().contains("slots de captura"), "{err}");
    }

    #[test]
    fn rejects_foreign_package() {
        assert!(matches!(
            read_package_from(Cursor::new(b"nope".to_vec())),
            Err(BackupError::Zip(_))
        ));
    }
}
