//! Full backup / restore of the pedal: presets (with scenes), capture and IR slots, and global
//! settings, stored in a single `.zip`.
//!
//! Presets are read through the documented commands only (`PLOAD`, `FXS`, `SNAP`, `AMPEQ`, `GAIN`,
//! ...) and rebuilt the same way on restore (`PNEW`, `FXA`, `FXP`, `FXE`, `PSAVE`, `SNAPSAVE`).
//! Capture / IR slots use the `CREAD` / `IREAD` read-back and the normal upload sequences.
//! Parts of the protocol are marked "unverified": restore reports per-step warnings instead of
//! failing, so a partially compatible firmware still gets everything that can be restored.

use crate::ir::IrPayload;
use crate::parse::{
    kv_map, parse_ampeq, parse_fxa_id, parse_fxs, parse_library, parse_lvl, parse_plist,
    parse_snaps, quoted, AmpEq,
};
use crate::session::{CmdError, Wire};
use crate::upload::{
    upload_capture_wire, upload_ir_payload_wire, CaptureJob, Progress, ProgressFn, UploadError,
};
use base64::Engine;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::io::{Read, Seek, Write};
use std::path::Path;
use std::time::Duration;

pub const FORMAT: &str = "pulsedeck-backup";
pub const VERSION: u32 = 1;
const SETTLE: Duration = Duration::from_millis(120);

#[derive(Debug, Clone, PartialEq, thiserror::Error)]
pub enum BackupError {
    #[error("{0}")]
    Cmd(CmdError),
    #[error("{0}")]
    Upload(UploadError),
    #[error("arquivo de backup: {0}")]
    Zip(String),
    #[error("{0}")]
    Invalid(String),
}

impl From<CmdError> for BackupError {
    fn from(e: CmdError) -> Self {
        BackupError::Cmd(e)
    }
}
impl From<UploadError> for BackupError {
    fn from(e: UploadError) -> Self {
        BackupError::Upload(e)
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Manifest {
    pub format: String,
    pub version: u32,
    pub created_unix: u64,
    pub firmware: String,
    pub presets: usize,
    pub captures: usize,
    pub irs: usize,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct BlockBk {
    pub bank: String,
    pub type_name: String,
    pub on: bool,
    pub params: Vec<i32>,
    pub lane: u8,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct StateBk {
    /// (on, params) per block, same order as `PresetBk::blocks`.
    pub blocks: Vec<(bool, Vec<i32>)>,
    pub amp_on: bool,
    pub ampeq: AmpEq,
    pub gain: Option<(i32, i32)>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct SceneBk {
    pub index: u8,
    pub name: Option<String>,
    pub state: StateBk,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct PresetBk {
    pub slot: u8,
    pub name: String,
    pub color: Option<String>,
    pub capture_slot: Option<u8>,
    pub amp_present: bool,
    pub blocks: Vec<BlockBk>,
    pub base: StateBk,
    pub scenes: Vec<SceneBk>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct LibBk {
    pub slot: u8,
    pub name: String,
    /// Raw `CREAD`/`IREAD` info line (len, crc, aux = IR gain, fs, ...).
    pub info: String,
    /// `trim=<dB>` of a capture slot (from `CLIST`)
    #[serde(default)]
    pub trim_db: Option<f64>,
    /// `in=<dBu>` input level of a capture slot (from `CLIST`)
    #[serde(default)]
    pub input_dbu: Option<f64>,
    #[serde(skip)]
    pub data: Vec<u8>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct GlobalsBk {
    /// Read-back reply lines per command (GEQ, HPF, FSA, LEDCFG, ...).
    pub settings: BTreeMap<String, Vec<String>>,
    /// Commands the firmware did not answer.
    pub unsupported: Vec<String>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Backup {
    pub manifest: Manifest,
    pub presets: Vec<PresetBk>,
    pub captures: Vec<LibBk>,
    pub irs: Vec<LibBk>,
    pub globals: GlobalsBk,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct RestoreReport {
    pub presets: usize,
    pub captures: usize,
    pub irs: usize,
    pub globals: usize,
    pub warnings: Vec<String>,
}

pub fn step(progress: &ProgressFn, stage: &str, done: usize, total: usize) {
    progress(Progress {
        stage: stage.into(),
        done,
        total,
    });
}

// ------------------------------------------------------------------------------- export

pub fn read_state(w: &mut dyn Wire) -> Result<(Vec<BlockBk>, bool, StateBk), BackupError> {
    let chain = parse_fxs(&w.send("FXS")?.lines);
    let ampeq = w
        .send("AMPEQ")
        .ok()
        .and_then(|r| parse_ampeq(r.terminal()))
        .unwrap_or_default();
    let gain = w.send("GAIN").ok().and_then(|r| {
        let kv = kv_map(r.terminal());
        Some((kv.get("in")?.parse().ok()?, kv.get("out")?.parse().ok()?))
    });
    let blocks: Vec<BlockBk> = chain
        .blocks
        .iter()
        .map(|b| BlockBk {
            bank: b.bank.clone(),
            type_name: b.type_name.clone(),
            on: b.on,
            params: b.params.clone(),
            lane: b.lane,
        })
        .collect();
    let state = StateBk {
        blocks: chain
            .blocks
            .iter()
            .map(|b| (b.on, b.params.clone()))
            .collect(),
        amp_on: chain.amp_on,
        ampeq,
        gain,
    };
    Ok((blocks, chain.amp_present, state))
}

/// Read a stored capture / IR through `CREAD` / `IREAD`, verifying every chunk CRC.
/// One `CREAD`/`IREAD <slot> <offset>` piece (up to 96 bytes), checked against its own CRC32.
pub fn read_chunk(
    w: &mut dyn Wire,
    cmd: &str,
    slot: u8,
    off: usize,
) -> Result<Vec<u8>, BackupError> {
    let r = w.send(&format!("{cmd} {slot} {off}"))?;
    let t: Vec<&str> = r.terminal().split_whitespace().collect();
    // CREAD <slot> <offset> <len> <crc32_hex8> <base64>
    let ok = t.len() >= 6 && t[2].parse::<usize>().ok() == Some(off);
    if !ok {
        return Err(BackupError::Invalid(format!(
            "{cmd} {slot}: bloco inesperado: {}",
            r.terminal()
        )));
    }
    let chunk = base64::engine::general_purpose::STANDARD
        .decode(t[5])
        .map_err(|e| BackupError::Invalid(format!("{cmd} {slot}: base64 inválido: {e}")))?;
    if chunk.is_empty() || t[3].parse::<usize>().ok() != Some(chunk.len()) {
        return Err(BackupError::Invalid(format!(
            "{cmd} {slot}: tamanho do bloco inconsistente em {off}"
        )));
    }
    if format!("{:08x}", crc32fast::hash(&chunk)) != t[4] {
        return Err(BackupError::Invalid(format!(
            "{cmd} {slot}: CRC32 do bloco em {off} não confere"
        )));
    }
    Ok(chunk)
}

pub fn read_blob(w: &mut dyn Wire, cmd: &str, slot: u8) -> Result<(String, Vec<u8>), BackupError> {
    let info = w.send(&format!("{cmd} {slot}"))?.terminal().to_string();
    let kv = kv_map(&info);
    let len: usize = kv
        .get("len")
        .and_then(|v| v.parse().ok())
        .ok_or_else(|| BackupError::Invalid(format!("{cmd} {slot}: resposta sem len: {info}")))?;
    let mut data = Vec::with_capacity(len);
    while data.len() < len {
        let chunk = read_chunk(w, cmd, slot, data.len())?;
        data.extend_from_slice(&chunk);
    }
    data.truncate(len);
    if let Some(crc) = kv.get("crc") {
        if format!("{:08x}", crc32fast::hash(&data)) != *crc {
            return Err(BackupError::Invalid(format!(
                "{cmd} {slot}: CRC32 total não confere"
            )));
        }
    }
    Ok((info, data))
}

/// Read the sounding preset (chain, amp, gain, capture reference) plus every stored scene.
/// Scenes are read by switching to them; the scene that was active is selected again at the end
/// (so unsaved edits on the pedal are replaced by the stored scenes).
pub fn read_current_preset(
    w: &mut dyn Wire,
    slot: u8,
    name: &str,
    color: Option<String>,
) -> Result<PresetBk, BackupError> {
    let (blocks, amp_present, base) = read_state(w)?;
    let capture_slot = w
        .send("CLIST")
        .ok()
        .map(|r| parse_library(&r.lines, "CAP", "CLIST end"))
        .and_then(|l| {
            let cur = u8::try_from(l.current?).ok()?;
            l.entries.iter().any(|e| e.slot == cur).then_some(cur)
        });
    let snaps = w
        .send("SNAPS")
        .ok()
        .and_then(|r| parse_snaps(r.terminal()))
        .unwrap_or_default();
    let names: BTreeMap<u8, String> = w
        .send("SNAPNAME")
        .map(|r| {
            r.lines
                .iter()
                .filter_map(|l| {
                    let rest = l.strip_prefix("SNAPNAME ")?;
                    let idx: u8 = rest.split_whitespace().next()?.parse().ok()?;
                    Some((idx, quoted(rest)?.to_string()))
                })
                .collect()
        })
        .unwrap_or_default();
    let mut scenes = Vec::new();
    for (i, has) in snaps.stored.iter().enumerate() {
        if !*has || w.send(&format!("SNAP {i}")).is_err() {
            continue;
        }
        let (_, _, state) = read_state(w)?;
        scenes.push(SceneBk {
            index: i as u8,
            name: names.get(&(i as u8)).cloned(),
            state,
        });
    }
    if let Some(cur) = snaps
        .current
        .filter(|c| snaps.stored[usize::from(*c).min(3)])
    {
        let _ = w.send(&format!("SNAP {cur}"));
    }
    Ok(PresetBk {
        slot,
        name: name.to_string(),
        color,
        capture_slot,
        amp_present,
        blocks,
        base,
        scenes,
    })
}

pub fn export(
    w: &mut dyn Wire,
    firmware: &str,
    progress: &ProgressFn,
) -> Result<Backup, BackupError> {
    let lvl = w.send("LVL").ok().and_then(|r| parse_lvl(r.terminal()));
    let plist = parse_plist(&w.send("PLIST")?.lines);
    let clist = parse_library(&w.send("CLIST")?.lines, "CAP", "CLIST end");
    let ilist = parse_library(&w.send("ILIST")?.lines, "IRS", "ILIST end");
    let globals_cmds = w.protocol().global_setting_commands();
    let total = plist.presets.len() + clist.entries.len() + ilist.entries.len() + 1;
    let mut done = 0usize;

    let mut captures = Vec::new();
    for e in &clist.entries {
        step(
            progress,
            &format!("Lendo captura {}", e.slot + 1),
            done,
            total,
        );
        let (info, data) = read_blob(w, "CREAD", e.slot)?;
        captures.push(LibBk {
            slot: e.slot,
            name: e.name.clone(),
            info,
            trim_db: e.trim_db,
            input_dbu: e.input_dbu,
            data,
        });
        done += 1;
    }
    let mut irs = Vec::new();
    for e in &ilist.entries {
        step(progress, &format!("Lendo IR {}", e.slot + 1), done, total);
        let (info, data) = read_blob(w, "IREAD", e.slot)?;
        irs.push(LibBk {
            slot: e.slot,
            name: e.name.clone(),
            info,
            trim_db: None,
            input_dbu: None,
            data,
        });
        done += 1;
    }

    let mut presets = Vec::new();
    for p in &plist.presets {
        step(
            progress,
            &format!("Lendo preset {} — {}", p.slot + 1, p.name),
            done,
            total,
        );
        done += 1;
        match w.send(&format!("PLOAD {}", p.slot)) {
            Ok(_) => {}
            Err(CmdError::Device { .. }) => continue, // empty slot listed by mistake
            Err(e) => return Err(e.into()),
        }
        presets.push(read_current_preset(w, p.slot, &p.name, p.color.clone())?);
    }

    step(progress, "Lendo configurações globais", done, total);
    let mut globals = GlobalsBk::default();
    for cmd in &globals_cmds {
        match w.send_settle(cmd, SETTLE) {
            Ok(r) => {
                let lines: Vec<String> = r
                    .lines
                    .into_iter()
                    .filter(|l| {
                        l.split_whitespace().next() == Some(cmd.as_str()) && !l.ends_with(" end")
                    })
                    .collect();
                globals.settings.insert(cmd.clone(), lines);
            }
            Err(CmdError::Device { .. }) | Err(CmdError::Timeout { .. }) => {
                globals.unsupported.push(cmd.clone())
            }
            Err(e) => return Err(e.into()),
        }
    }

    // put the pedal back where the user left it
    if let Some(l) = lvl {
        if let Some(p) = l.preset.filter(|p| *p >= 0) {
            let _ = w.send(&format!("PLOAD {p}"));
            if let Some(s) = l.scene.filter(|s| *s >= 0) {
                let _ = w.send(&format!("SNAP {s}"));
            }
        }
    }
    step(progress, "Backup lido", total, total);
    Ok(Backup {
        manifest: Manifest {
            format: FORMAT.into(),
            version: VERSION,
            created_unix: std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs())
                .unwrap_or(0),
            firmware: firmware.into(),
            presets: presets.len(),
            captures: captures.len(),
            irs: irs.len(),
        },
        presets,
        captures,
        irs,
        globals,
    })
}

// ---------------------------------------------------------------------------------- zip

fn zerr(e: impl std::fmt::Display) -> BackupError {
    BackupError::Zip(e.to_string())
}

pub fn write_zip_to<W: Write + Seek>(b: &Backup, out: W) -> Result<(), BackupError> {
    let mut z = zip::ZipWriter::new(out);
    let opts = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    let mut put = |name: String, data: &[u8]| -> Result<(), BackupError> {
        z.start_file(name, opts).map_err(zerr)?;
        z.write_all(data).map_err(zerr)
    };
    let json = |v: &dyn erased::Ser| v.to_vec();
    put("manifest.json".into(), &json(&b.manifest)?)?;
    for p in &b.presets {
        put(format!("presets/{:02}.json", p.slot), &json(p)?)?;
    }
    for (dir, list) in [("captures", &b.captures), ("irs", &b.irs)] {
        for e in list {
            put(format!("{dir}/{:02}.json", e.slot), &json(e)?)?;
            put(format!("{dir}/{:02}.bin", e.slot), &e.data)?;
        }
    }
    put("globals.json".into(), &json(&b.globals)?)?;
    z.finish().map_err(zerr)?;
    Ok(())
}

mod erased {
    use super::{zerr, BackupError};
    pub trait Ser {
        fn to_vec(&self) -> Result<Vec<u8>, BackupError>;
    }
    impl<T: serde::Serialize> Ser for T {
        fn to_vec(&self) -> Result<Vec<u8>, BackupError> {
            serde_json::to_vec_pretty(self).map_err(zerr)
        }
    }
}

pub fn write_zip(b: &Backup, path: &Path) -> Result<(), BackupError> {
    let f = std::fs::File::create(path).map_err(zerr)?;
    write_zip_to(b, std::io::BufWriter::new(f))
}

pub fn read_zip_from<R: Read + Seek>(r: R) -> Result<Backup, BackupError> {
    let mut z = zip::ZipArchive::new(r).map_err(zerr)?;
    let mut read = |name: &str| -> Result<Vec<u8>, BackupError> {
        let mut f = z
            .by_name(name)
            .map_err(|_| BackupError::Zip(format!("falta {name} no backup")))?;
        let mut v = Vec::new();
        f.read_to_end(&mut v).map_err(zerr)?;
        Ok(v)
    };
    let manifest: Manifest = serde_json::from_slice(&read("manifest.json")?).map_err(zerr)?;
    if manifest.format != FORMAT || manifest.version != VERSION {
        return Err(BackupError::Invalid(format!(
            "não é um backup PulseDeck v{VERSION} (formato '{}' v{})",
            manifest.format, manifest.version
        )));
    }
    let names: Vec<String> = z
        .file_names()
        .filter_map(|n| n.ok().map(|c| c.into_owned()))
        .collect();
    let mut presets = Vec::new();
    for n in names
        .iter()
        .filter(|n| n.starts_with("presets/") && n.ends_with(".json"))
    {
        presets.push(
            serde_json::from_slice::<PresetBk>(&{
                let mut f = z.by_name(n).map_err(zerr)?;
                let mut v = Vec::new();
                f.read_to_end(&mut v).map_err(zerr)?;
                v
            })
            .map_err(zerr)?,
        );
    }
    presets.sort_by_key(|p| p.slot);
    let mut load_lib = |dir: &str| -> Result<Vec<LibBk>, BackupError> {
        let mut out = Vec::new();
        for n in names
            .iter()
            .filter(|n| n.starts_with(&format!("{dir}/")) && n.ends_with(".json"))
        {
            let mut v = Vec::new();
            z.by_name(n)
                .map_err(zerr)?
                .read_to_end(&mut v)
                .map_err(zerr)?;
            let mut e: LibBk = serde_json::from_slice(&v).map_err(zerr)?;
            let mut d = Vec::new();
            z.by_name(&format!("{dir}/{:02}.bin", e.slot))
                .map_err(|_| BackupError::Zip(format!("falta {dir}/{:02}.bin", e.slot)))?
                .read_to_end(&mut d)
                .map_err(zerr)?;
            e.data = d;
            out.push(e);
        }
        out.sort_by_key(|e| e.slot);
        Ok(out)
    };
    let captures = load_lib("captures")?;
    let irs = load_lib("irs")?;
    let mut gv = Vec::new();
    z.by_name("globals.json")
        .map_err(zerr)?
        .read_to_end(&mut gv)
        .map_err(zerr)?;
    let globals: GlobalsBk = serde_json::from_slice(&gv).map_err(zerr)?;
    Ok(Backup {
        manifest,
        presets,
        captures,
        irs,
        globals,
    })
}

pub fn read_zip(path: &Path) -> Result<Backup, BackupError> {
    read_zip_from(std::io::BufReader::new(
        std::fs::File::open(path).map_err(zerr)?,
    ))
}

// ------------------------------------------------------------------------------ restore

/// Rebuild one preset on the pedal from its description. With `save_slot` the chain is written
/// to flash (`PSAVE`) and the stored scenes recreated; without it the chain is only applied live
/// (scenes need a preset slot to live in, so they are skipped).
pub fn rebuild_preset(
    w: &mut dyn Wire,
    p: &PresetBk,
    save_slot: Option<u8>,
    warnings: &mut Vec<String>,
) -> bool {
    let what = format!("preset {}", p.slot + 1);
    if warn_on_err(w.send("PNEW"), &format!("{what} PNEW"), warnings).is_none() {
        return false;
    }
    warn_on_err(
        w.send(&format!("AMPX {}", p.amp_present as u8)),
        &format!("{what} AMPX"),
        warnings,
    );
    let mut ids: Vec<Option<u32>> = Vec::new();
    for blk in &p.blocks {
        let id = match w.send(&format!("FXA {} 99 {}", blk.bank, blk.type_name)) {
            Ok(r) => parse_fxa_id(&r.lines),
            Err(e) => {
                warnings.push(format!(
                    "{what}: bloco {} não restaurado ({e})",
                    blk.type_name
                ));
                None
            }
        };
        if let (Some(id), true) = (id, blk.lane > 0) {
            warn_on_err(
                w.send(&format!("FXL {id} {}", blk.lane)),
                &format!("{what} FXL"),
                warnings,
            );
        }
        ids.push(id);
    }
    apply_state(w, &ids, &p.base, warnings, &what);
    if let Some(c) = p.capture_slot {
        warn_on_err(
            w.send(&format!("CLOAD {c}")),
            &format!("{what} CLOAD"),
            warnings,
        );
    }
    if let Some(col) = &p.color {
        warn_on_err(
            w.send(&format!("PCOLOR {col}")),
            &format!("{what} PCOLOR"),
            warnings,
        );
    }
    let Some(save_slot) = save_slot else {
        return true;
    };
    let name = crate::upload::sanitize_name(&p.name);
    if warn_on_err(
        w.send(&format!("PSAVE {save_slot} {name}")),
        &format!("{what} PSAVE"),
        warnings,
    )
    .is_none()
    {
        return false;
    }
    for sc in &p.scenes {
        apply_state(
            w,
            &ids,
            &sc.state,
            warnings,
            &format!("{what} cena {}", sc.index + 1),
        );
        warn_on_err(
            w.send(&format!("SNAPSAVE {}", sc.index)),
            &format!("{what} SNAPSAVE"),
            warnings,
        );
        if let Some(n) = &sc.name {
            let n = crate::upload::sanitize_name(n);
            warn_on_err(
                w.send(&format!("SNAPNAME {} {n}", sc.index)),
                &format!("{what} SNAPNAME"),
                warnings,
            );
        }
    }
    true
}

/// Turn a read-back line into the command that sets it (best effort; see module docs).
fn restore_commands(cmd: &str, lines: &[String]) -> Vec<String> {
    let mut out = Vec::new();
    for l in lines {
        let rest = l.strip_prefix(cmd).unwrap_or("").trim();
        match cmd {
            "HPF" => {
                if let Some(hz) = kv_map(l).get("hz") {
                    out.push(format!("HPF {hz}"));
                }
            }
            // the query returns one line with all bands; the pedal is set band by band
            "GEQ" => {
                let nums: Vec<&str> = rest.split_whitespace().collect();
                if nums.len() >= 5 {
                    out.extend(
                        nums.iter()
                            .take(5)
                            .enumerate()
                            .map(|(i, v)| format!("GEQ {i} {v}")),
                    );
                } else {
                    out.push(l.clone());
                }
            }
            // keep only what `FSA <fs> <gesture> <type> <a> <b>` takes (drops `id=..`)
            "FSA" => out.push(
                l.split_whitespace()
                    .take_while(|t| !t.contains('='))
                    .collect::<Vec<_>>()
                    .join(" "),
            ),
            // the layout comes from `layout=`; mode / bank / bypass are live state
            "FSMODE" => {
                if let Some(v) = kv_map(l).get("layout") {
                    out.push(format!("FSMODE layout={v}"));
                }
            }
            "FSLED" => {
                let kv = kv_map(l);
                let mut tok = l.split_whitespace().skip(1);
                if let (Some(f), Some(mode), Some(on), Some(off)) =
                    (tok.next(), tok.next(), kv.get("on"), kv.get("off"))
                {
                    out.push(if mode == "custom" {
                        format!("FSLED {f} {on} {off}")
                    } else {
                        format!("FSLED {f} auto")
                    });
                }
            }
            "FSPRESS" => {
                for (i, v) in rest.split_whitespace().enumerate() {
                    out.push(format!("FSPRESS {i} {v}"));
                }
            }
            "LEDCFG" => out.extend(
                rest.split_whitespace()
                    .filter(|t| t.contains('='))
                    .map(|kv| format!("LEDCFG {kv}")),
            ),
            _ => out.push(l.clone()),
        }
    }
    out
}

pub fn warn_on_err<T>(r: Result<T, CmdError>, what: &str, warnings: &mut Vec<String>) -> Option<T> {
    match r {
        Ok(v) => Some(v),
        Err(e) => {
            warnings.push(format!("{what}: {e}"));
            None
        }
    }
}

pub fn apply_state(
    w: &mut dyn Wire,
    ids: &[Option<u32>],
    st: &StateBk,
    warnings: &mut Vec<String>,
    what: &str,
) {
    for (id, (on, params)) in ids.iter().zip(st.blocks.iter()) {
        let Some(id) = id else { continue };
        for (i, v) in params.iter().enumerate() {
            warn_on_err(
                w.send(&format!("FXP {id} {i} {v}")),
                &format!("{what} FXP"),
                warnings,
            );
        }
        warn_on_err(
            w.send(&format!("FXE {id} {}", *on as u8)),
            &format!("{what} FXE"),
            warnings,
        );
    }
    warn_on_err(
        w.send(&format!("AMP {}", st.amp_on as u8)),
        &format!("{what} AMP"),
        warnings,
    );
    let e = &st.ampeq;
    warn_on_err(
        w.send(&format!(
            "AMPEQ {} {} {} {} {} {}",
            e.volume, e.bass, e.mid, e.treble, e.presence, e.drive
        )),
        &format!("{what} AMPEQ"),
        warnings,
    );
    if let Some((i, o)) = st.gain {
        warn_on_err(
            w.send(&format!("GAIN {i} {o}")),
            &format!("{what} GAIN"),
            warnings,
        );
    }
}

pub fn restore(
    w: &mut dyn Wire,
    b: &Backup,
    progress: &ProgressFn,
) -> Result<RestoreReport, BackupError> {
    let mut rep = RestoreReport::default();
    let total = b.captures.len() + b.irs.len() + b.presets.len() + 1;
    let mut done = 0usize;
    let quiet: ProgressFn = std::sync::Arc::new(|_| {});

    // captures first: presets reference them by slot
    for c in &b.captures {
        step(
            progress,
            &format!("Restaurando captura {}", c.slot + 1),
            done,
            total,
        );
        done += 1;
        let job = CaptureJob {
            blob: c.data.clone(),
            name: c.name.clone(),
            slot: c.slot,
            input_level_dbu: c.input_dbu,
        };
        match upload_capture_wire(w, &job, &quiet) {
            Ok(_) => rep.captures += 1,
            Err(e) => rep.warnings.push(format!("captura {}: {e}", c.slot + 1)),
        }
    }
    for e in &b.irs {
        step(
            progress,
            &format!("Restaurando IR {}", e.slot + 1),
            done,
            total,
        );
        done += 1;
        let kv = kv_map(&e.info);
        let fs: u32 = kv.get("fs").and_then(|v| v.parse().ok()).unwrap_or(48);
        if fs != 48 {
            rep.warnings.push(format!(
                "IR {}: taxa {fs} kHz não suportada pelo envio (só 48 kHz)",
                e.slot + 1
            ));
            continue;
        }
        let gain: i32 = kv.get("aux").and_then(|v| v.parse().ok()).unwrap_or(0);
        let samples: Vec<i16> = e
            .data
            .as_chunks::<2>()
            .0
            .iter()
            .map(|c| i16::from_le_bytes(*c))
            .collect();
        match upload_ir_payload_wire(
            w,
            &IrPayload {
                samples,
                gain_mdb: gain,
            },
            &e.name,
            e.slot,
            &quiet,
        ) {
            Ok(_) => rep.irs += 1,
            Err(err) => rep.warnings.push(format!("IR {}: {err}", e.slot + 1)),
        }
    }

    // presets
    for p in &b.presets {
        step(
            progress,
            &format!("Restaurando preset {} — {}", p.slot + 1, p.name),
            done,
            total,
        );
        done += 1;
        if rebuild_preset(w, p, Some(p.slot), &mut rep.warnings) {
            rep.presets += 1;
        }
    }

    // global settings
    step(progress, "Restaurando configurações globais", done, total);
    for (cmd, lines) in &b.globals.settings {
        for line in restore_commands(cmd, lines) {
            if warn_on_err(w.send(&line), &format!("global {cmd}"), &mut rep.warnings).is_some() {
                rep.globals += 1;
            }
        }
    }
    if !b.globals.settings.is_empty() {
        warn_on_err(w.send("GSAVE"), "GSAVE", &mut rep.warnings);
    }
    step(progress, "Backup restaurado", total, total);
    Ok(rep)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::Protocol;
    use crate::session::{EventSink, Session, SessionWire};
    use crate::sim::{SimLink, SimState};
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

    /// Give the source pedal a distinctive state: scenes, a capture, an IR, globals.
    fn prepare_source(st: &Arc<Mutex<SimState>>, w: &mut SessionWire) {
        // capture + IR through the real upload paths
        let nam = crate::a2l::testutil::synthetic_nam(
            &[1, 2, 4],
            3,
            2,
            11,
            serde_json::json!({"name": "Src Amp"}),
        );
        let blob = crate::a2l::convert_str(&nam, &Default::default())
            .unwrap()
            .blob;
        upload_capture_wire(
            w,
            &CaptureJob {
                blob,
                name: "Src Amp".into(),
                slot: 4,
                input_level_dbu: None,
            },
            &quiet(),
        )
        .unwrap();
        let x: Vec<f64> = (0..3000)
            .map(|i| (i as f64 * 0.1).sin() * (-(i as f64) / 500.0).exp())
            .collect();
        crate::upload::upload_ir_wire(
            w,
            &crate::upload::IrJob {
                mono48: x,
                name: "Src IR".into(),
                slot: Some(3),
                assign_block: None,
            },
            &quiet(),
        )
        .unwrap();
        // preset 0: tweak, save, scenes with different drive
        w.send("PLOAD 0").unwrap();
        w.send("CLOAD 4").unwrap();
        let od = parse_fxs(&w.send("FXS").unwrap().lines)
            .blocks
            .into_iter()
            .find(|b| b.type_name == "overdrive")
            .unwrap();
        w.send(&format!("FXP {} 0 22", od.id)).unwrap();
        w.send("AMPEQ 5 10 -20 30 40 50").unwrap();
        w.send("GAIN 3 -2").unwrap();
        w.send("PCOLOR ff8800").unwrap();
        w.send("PSAVE 0 Lead Tone").unwrap();
        w.send("SNAPSAVE 0").unwrap();
        w.send(&format!("FXP {} 0 35", od.id)).unwrap();
        w.send(&format!("FXE {} 0", od.id)).unwrap();
        w.send("SNAPSAVE 2").unwrap();
        w.send("SNAPNAME 2 Solo").unwrap();
        // globals
        w.send("HPF 120").unwrap();
        w.send("LEDCFG bright=9").unwrap();
        w.send("FSA 2 1 tuner").unwrap();
        w.send("GEQ 3 -4").unwrap();
        w.send("PLOAD 0").unwrap();
        let _ = st;
    }

    #[test]
    fn export_zip_restore_roundtrip() {
        let (mut src_w, src) = pedal();
        prepare_source(&src, &mut src_w);

        let bk = export(&mut src_w, "tankg_fw M5.26", &quiet()).unwrap();
        assert_eq!(bk.presets.len(), 2);
        assert_eq!(bk.captures.len(), 2); // demo capture + uploaded
        assert_eq!(bk.irs.len(), 1);
        let p0 = &bk.presets[0];
        assert_eq!((p0.slot, p0.name.as_str()), (0, "Lead Tone"));
        assert_eq!(p0.scenes.len(), 2);
        assert_eq!(p0.scenes[1].name.as_deref(), Some("Solo"));
        assert_eq!(p0.capture_slot, Some(4));
        assert!(bk.globals.settings["HPF"][0].contains("120"));
        assert!(bk.globals.settings["FSA"]
            .iter()
            .any(|l| l.contains("tuner")));
        assert_eq!(
            restore_commands("GEQ", &bk.globals.settings["GEQ"])[3],
            "GEQ 3 -4"
        );
        // export leaves the pedal on the preset it was on
        assert_eq!(src.lock().unwrap().preset, 0);

        // zip roundtrip
        let mut buf = Cursor::new(Vec::new());
        write_zip_to(&bk, &mut buf).unwrap();
        let bytes = buf.into_inner();
        let names: Vec<String> = zip::ZipArchive::new(Cursor::new(&bytes))
            .unwrap()
            .file_names()
            .filter_map(|n| n.ok().map(|c| c.into_owned()))
            .collect();
        for n in [
            "manifest.json",
            "globals.json",
            "presets/00.json",
            "presets/01.json",
            "captures/04.bin",
            "irs/03.bin",
        ] {
            assert!(names.iter().any(|x| x == n), "{n} in {names:?}");
        }
        let back = read_zip_from(Cursor::new(bytes)).unwrap();
        assert_eq!(back, bk);

        // restore into a factory-fresh pedal
        let (mut dst_w, dst) = pedal();
        {
            let mut d = dst.lock().unwrap();
            d.stored.clear();
            d.captures = vec![None; 15];
            d.blobs.clear();
        }
        let rep = restore(&mut dst_w, &back, &quiet()).unwrap();
        assert!(rep.warnings.is_empty(), "{:?}", rep.warnings);
        assert_eq!((rep.presets, rep.captures, rep.irs), (2, 2, 1));

        let (s, d) = (src.lock().unwrap(), dst.lock().unwrap());
        // captures / IRs byte-identical
        for k in [('C', 4u8), ('C', 0), ('I', 3)] {
            assert_eq!(s.blobs[&k], d.blobs[&k], "{k:?}");
        }
        assert_eq!(d.captures[4].as_ref().unwrap().0, "Src Amp");
        assert_eq!(s.ir_gain[&3], d.ir_gain[&3]);
        // presets: same chain, scenes, names, colour, capture
        for slot in [0u8, 1] {
            let (a, b) = (&s.stored[&slot], &d.stored[&slot]);
            assert_eq!(a.name, b.name);
            assert_eq!(a.color, b.color);
            assert_eq!(a.capture, b.capture);
            assert_eq!(a.amp_on, b.amp_on);
            assert_eq!(a.ampeq, b.ampeq);
            assert_eq!(a.gain, b.gain);
            let sig = |p: &crate::sim::StoredPreset| {
                p.blocks
                    .iter()
                    .map(|x| (x.bank, x.type_name.clone(), x.on, x.params.clone(), x.lane))
                    .collect::<Vec<_>>()
            };
            assert_eq!(sig(a), sig(b), "slot {slot} chain");
            for i in 0..4 {
                assert_eq!(
                    a.scenes[i].is_some(),
                    b.scenes[i].is_some(),
                    "slot {slot} scene {i}"
                );
                if let (Some(x), Some(y)) = (&a.scenes[i], &b.scenes[i]) {
                    assert_eq!(x.blocks, y.blocks);
                    assert_eq!((x.amp_on, x.ampeq, x.gain), (y.amp_on, y.ampeq, y.gain));
                }
            }
            assert_eq!(a.scene_names, b.scene_names);
        }
        // globals
        assert_eq!(
            (s.hpf, s.fsmode.clone(), s.geq),
            (d.hpf, d.fsmode.clone(), d.geq)
        );
        assert_eq!(s.ledcfg, d.ledcfg);
        assert_eq!(s.fsa, d.fsa);
        assert!(d.gsaved);
    }

    #[test]
    fn rejects_foreign_zip() {
        let mut buf = Cursor::new(Vec::new());
        {
            let mut z = zip::ZipWriter::new(&mut buf);
            z.start_file("manifest.json", zip::write::SimpleFileOptions::default())
                .unwrap();
            z.write_all(br#"{"format":"other","version":1,"created_unix":0,"firmware":"","presets":0,"captures":0,"irs":0}"#).unwrap();
            z.finish().unwrap();
        }
        assert!(matches!(
            read_zip_from(Cursor::new(buf.into_inner())),
            Err(BackupError::Invalid(_))
        ));
        assert!(matches!(
            read_zip_from(Cursor::new(b"not a zip".to_vec())),
            Err(BackupError::Zip(_))
        ));
    }

    #[test]
    fn restore_reports_warnings_instead_of_failing() {
        let (mut w, st) = pedal();
        let mut bk = export(&mut w, "x", &quiet()).unwrap();
        bk.presets[0].blocks.push(BlockBk {
            bank: "P".into(),
            type_name: "nonexistent_fx".into(),
            on: true,
            params: vec![],
            lane: 0,
        });
        bk.presets[0].base.blocks.push((true, vec![]));
        st.lock().unwrap().stored.clear();
        let rep = restore(&mut w, &bk, &quiet()).unwrap();
        assert!(
            rep.warnings.iter().any(|m| m.contains("nonexistent_fx")),
            "{:?}",
            rep.warnings
        );
        assert_eq!(rep.presets, 2);
    }
}
