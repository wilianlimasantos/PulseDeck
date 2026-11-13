//! File transfer to the pedal: captures (`LDBEGIN`/`LDB`/`LDEND`/`CSAVE`) and impulse
//! responses (`IRBEGIN`/`IRB`/`IREND`/`ISAVE`). Stop-and-wait, one chunk per command line,
//! CRC32 over the whole stream. Runs as an exclusive session transaction.

use crate::ir::{prepare_ir, IrPayload};
use crate::parse::{kv_map, parse_load};
use crate::session::{CmdError, Session, Wire};
use serde::{Deserialize, Serialize};
use std::sync::Arc;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Progress {
    pub stage: String,
    pub done: usize,
    pub total: usize,
}

pub type ProgressFn = Arc<dyn Fn(Progress) + Send + Sync>;

#[derive(Debug, Clone, PartialEq, thiserror::Error)]
pub enum UploadError {
    #[error("{0}")]
    Cmd(CmdError),
    #[error("transferência falhou no byte {offset}: {reason}")]
    Transfer { offset: usize, reason: String },
    #[error("a pedaleira rejeitou a captura: {0}")]
    Rejected(String),
    #[error("{0}")]
    Invalid(String),
}

impl From<CmdError> for UploadError {
    fn from(e: CmdError) -> Self {
        UploadError::Cmd(e)
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UploadReport {
    pub slot: u8,
    pub name: String,
    pub bytes: usize,
    pub detail: String,
}

/// ASCII-only, no quotes/control chars, at most 23 chars (`CSAVE`/`ISAVE` name field).
pub fn sanitize_name(name: &str) -> String {
    let s: String = name
        .chars()
        .map(|c| if !c.is_ascii() { '_' } else { c })
        .filter(|c| !c.is_ascii_control() && *c != '\'' && *c != '"')
        .collect();
    let s = s.trim();
    let s: String = s.chars().take(23).collect();
    let s = s.trim().to_string();
    if s.is_empty() {
        "NAM".into()
    } else {
        s
    }
}

pub fn hex(bytes: &[u8]) -> String {
    const H: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push(H[(b >> 4) as usize] as char);
        s.push(H[(b & 15) as usize] as char);
    }
    s
}

/// Send `data` as `<prefix> <offset> <hex>` lines, verifying every ack.
fn send_chunks(
    ex: &mut dyn Wire,
    prefix: &str,
    data: &[u8],
    stage: &str,
    progress: &ProgressFn,
) -> Result<(), UploadError> {
    let total = data.len();
    let chunk = ex.protocol().chunk_size(prefix, total, ex.max_line());
    if chunk == 0 {
        return Err(UploadError::Invalid(
            "limite de linha do firmware pequeno demais para transferir".into(),
        ));
    }
    let mut off = 0usize;
    while off < total {
        let end = (off + chunk).min(total);
        let line = format!("{prefix} {off} {}", hex(&data[off..end]));
        let reply = ex.send(&line).map_err(|e| match e {
            CmdError::Device { message, .. } => UploadError::Transfer {
                offset: off,
                reason: message,
            },
            other => UploadError::Cmd(other),
        })?;
        let t = reply.terminal();
        let want = format!("OK {prefix} {off} ");
        let written: Option<usize> = t.strip_prefix(&want).and_then(|r| r.trim().parse().ok());
        if written != Some(end - off) {
            return Err(UploadError::Transfer {
                offset: off,
                reason: format!("ack inesperado: {t}"),
            });
        }
        off = end;
        progress(Progress {
            stage: stage.into(),
            done: off,
            total,
        });
    }
    Ok(())
}

fn current_audio_mode(ex: &mut dyn Wire) -> Option<i32> {
    let r = ex.send("LVL").ok()?;
    kv_map(r.terminal()).get("m").and_then(|v| v.parse().ok())
}

pub struct CaptureJob {
    pub blob: Vec<u8>,
    pub name: String,
    pub slot: u8,
    /// `.nam` metadata input level; stored through `CTRIM live` when within -20..40 dBu.
    pub input_level_dbu: Option<f64>,
}

fn capture_transaction(
    ex: &mut dyn Wire,
    job: &CaptureJob,
    progress: &ProgressFn,
    was_on: &mut bool,
) -> Result<UploadReport, UploadError> {
    let total = job.blob.len();
    progress(Progress {
        stage: "Preparando".into(),
        done: 0,
        total,
    });
    *was_on = current_audio_mode(ex).map(|m| m == 2).unwrap_or(true);
    ex.send("AUDIO OFF")?;
    ex.send(&format!("LDBEGIN {total}"))?;
    send_chunks(ex, "LDB", &job.blob, "Enviando captura", progress)?;
    let crc = crc32fast::hash(&job.blob);
    progress(Progress {
        stage: "Carregando no motor".into(),
        done: total,
        total,
    });
    let r = ex.send(&format!("LDEND {crc:08x}"))?;
    let load = parse_load(r.terminal())
        .ok_or_else(|| UploadError::Invalid(format!("resposta inesperada: {}", r.terminal())))?;
    if load.rc != 0 {
        let why = match load.rc {
            -4 => "blob rejeitado pelo validador".to_string(),
            -2 | -3 | -21 => "o firmware não tem o motor para este formato".to_string(),
            -100 => "memória insuficiente".to_string(),
            n => format!("erro rc={n}"),
        };
        return Err(UploadError::Rejected(why));
    }
    if *was_on && load.audio != Some(2) {
        ex.send("AUDIO A2L")?;
    }
    *was_on = false; // engine is running again; nothing to clean up
    if let Some(dbu) = job.input_level_dbu.filter(|v| (-20.0..=40.0).contains(v)) {
        ex.send(&format!("CTRIM live 0 {}", (dbu * 10.0).round() as i32))?;
    }
    let name = sanitize_name(&job.name);
    let r = ex.send(&format!("CSAVE {} {}", job.slot, name))?;
    Ok(UploadReport {
        slot: job.slot,
        name,
        bytes: total,
        detail: r.terminal().to_string(),
    })
}

/// Capture upload over any `Wire` (used directly by the backup restore).
pub fn upload_capture_wire(
    ex: &mut dyn Wire,
    job: &CaptureJob,
    progress: &ProgressFn,
) -> Result<UploadReport, UploadError> {
    if job.blob.is_empty() {
        return Err(UploadError::Invalid("blob vazio".into()));
    }
    let mut was_on = false;
    let res = capture_transaction(ex, job, progress, &mut was_on);
    if res.is_err() {
        // release the staging buffer and re-enable audio
        let _ = ex.send("LDBEGIN 0");
        if was_on {
            let _ = ex.send("AUDIO A2L");
        }
    }
    res
}

/// Upload a converted capture and save it to `slot` (0..14).
pub fn upload_capture(
    session: &Session,
    job: CaptureJob,
    progress: ProgressFn,
) -> Result<UploadReport, UploadError> {
    session
        .exclusive(move |ex| upload_capture_wire(ex, &job, &progress))
        .map_err(UploadError::Cmd)?
}

pub struct IrJob {
    /// Mono 48 kHz samples (already resampled), not yet truncated/normalised.
    pub mono48: Vec<f64>,
    pub name: String,
    /// `None` = use as the sounding shared IR only (no ISAVE).
    pub slot: Option<u8>,
    /// Assign the stored IR to this `ir` block afterwards (`FXIR`).
    pub assign_block: Option<u32>,
}

fn is_oom(e: &UploadError) -> bool {
    let s = e.to_string().to_ascii_lowercase();
    s.contains("memory") || s.contains("memória")
}

fn ir_attempt(
    ex: &mut dyn Wire,
    payload: &IrPayload,
    name: &str,
    slot_to_save: Option<u8>,
    assign_block: Option<u32>,
    progress: &ProgressFn,
) -> Result<UploadReport, UploadError> {
    let bytes = payload.to_bytes();
    ex.send(&format!(
        "IRBEGIN {} {} 48",
        payload.samples.len(),
        payload.gain_mdb
    ))?;
    send_chunks(ex, "IRB", &bytes, "Enviando IR", progress)?;
    let crc = crc32fast::hash(&bytes);
    let r = ex.send(&format!("IREND {crc:08x}"))?;
    let mut detail = r.terminal().to_string();
    let name = sanitize_name(name);
    let mut slot = 0u8;
    if let Some(s) = slot_to_save {
        slot = s;
        let r = ex.send(&format!("ISAVE {s} {name}"))?;
        detail = r.terminal().to_string();
        if let Some(b) = assign_block {
            ex.send(&format!("FXIR {b} {s}"))?;
        }
    }
    Ok(UploadReport {
        slot,
        name,
        bytes: bytes.len(),
        detail,
    })
}

/// Re-upload an already prepared payload (backup restore): single attempt, no resampling.
pub fn upload_ir_payload_wire(
    ex: &mut dyn Wire,
    payload: &IrPayload,
    name: &str,
    slot: u8,
    progress: &ProgressFn,
) -> Result<UploadReport, UploadError> {
    let r = ir_attempt(ex, payload, name, Some(slot), None, progress);
    if r.is_err() {
        let _ = ex.send("IRBEGIN 0 0 48");
    }
    r
}

/// IR upload over any `Wire`; N = 4076 samples (2028 for slot 7), retrying with 2048 then 1024
/// on out-of-memory.
pub fn upload_ir_wire(
    ex: &mut dyn Wire,
    job: &IrJob,
    progress: &ProgressFn,
) -> Result<UploadReport, UploadError> {
    let (n_full, n_slot7) = ex.protocol().ir_sample_limits();
    let first = if job.slot == Some(7) { n_slot7 } else { n_full };
    let mut sizes = vec![first];
    for fallback in [2048usize, 1024] {
        if fallback < first {
            sizes.push(fallback);
        }
    }
    let mut last: Option<UploadError> = None;
    for n in sizes {
        let payload = match prepare_ir(&job.mono48, n) {
            Ok(p) => p,
            Err(e) => return Err(UploadError::Invalid(e.0)),
        };
        match ir_attempt(
            ex,
            &payload,
            &job.name,
            job.slot,
            job.assign_block,
            progress,
        ) {
            Ok(r) => return Ok(r),
            Err(e) => {
                let _ = ex.send("IRBEGIN 0 0 48"); // release the staging buffer
                if is_oom(&e) {
                    last = Some(e);
                    continue;
                }
                return Err(e);
            }
        }
    }
    Err(last.unwrap_or(UploadError::Invalid("IR não pôde ser enviado".into())))
}

pub fn upload_ir(
    session: &Session,
    job: IrJob,
    progress: ProgressFn,
) -> Result<UploadReport, UploadError> {
    session
        .exclusive(move |ex| upload_ir_wire(ex, &job, &progress))
        .map_err(UploadError::Cmd)?
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::a2l::{convert_str, testutil::synthetic_nam, ConvertOptions};
    use crate::protocol::Protocol;
    use crate::session::EventSink;
    use crate::sim::{SimLink, SimState};
    use serde_json::json;
    use std::sync::Mutex;

    fn setup() -> (Session, Arc<Mutex<SimState>>) {
        let p = Arc::new(Protocol::embedded());
        let st = Arc::new(Mutex::new(SimState::new(&p)));
        let sink: EventSink = Arc::new(|_| {});
        let s = Session::start(Box::new(SimLink::new(st.clone())), p, sink);
        s.set_max_line(190);
        (s, st)
    }

    fn quiet() -> ProgressFn {
        Arc::new(|_| {})
    }

    #[test]
    fn sanitizes_names() {
        assert_eq!(
            sanitize_name("Marshall 'Plexi' ñandú"),
            "Marshall Plexi _and_"
        );
        assert_eq!(sanitize_name("  "), "NAM");
        assert_eq!(sanitize_name("0123456789012345678901234567890").len(), 23);
    }

    #[test]
    fn uploads_capture_end_to_end() {
        let (s, st) = setup();
        let nam = synthetic_nam(&[1, 2, 4, 8], 3, 3, 5, json!({"name": "Test"}));
        let blob = convert_str(&nam, &ConvertOptions::default()).unwrap().blob;
        let rep = upload_capture(
            &s,
            CaptureJob {
                blob: blob.clone(),
                name: "My Amp".into(),
                slot: 3,
                input_level_dbu: Some(12.0),
            },
            quiet(),
        )
        .unwrap();
        assert_eq!(rep.slot, 3);
        let st = st.lock().unwrap();
        assert_eq!(st.last_blob, blob);
        assert_eq!(st.captures[3].as_ref().unwrap().0, "My Amp");
        let log = &st.log;
        // sequence from file_transfer.capture_nam
        let pos = |p: &str| {
            log.iter()
                .position(|l| l.starts_with(p))
                .unwrap_or_else(|| panic!("missing {p}: {log:?}"))
        };
        assert!(pos("AUDIO OFF") < pos("LDBEGIN"));
        assert!(pos("LDBEGIN") < pos("LDB 0 "));
        assert!(pos("LDB 0 ") < pos("LDEND"));
        assert!(pos("LDEND") < pos("CTRIM live 0 120"));
        assert!(pos("CTRIM") < pos("CSAVE 3 My Amp"));
        assert!(log
            .iter()
            .any(|l| l == &format!("LDEND {:08x}", crc32fast::hash(&blob))));
        // every line fits the firmware limit and chunks are 90 bytes (190 - 3 - 2 - len)
        assert!(log.iter().all(|l| l.len() <= 190));
        // audio restored after upload (sim reports audio=0 after OFF so the host must re-enable)
        assert_eq!(st.audio, 2);
    }

    #[test]
    fn capture_failure_releases_buffer_and_audio() {
        let (s, st) = setup();
        let r = upload_capture(
            &s,
            CaptureJob {
                blob: vec![0u8; 100],
                name: "bad".into(),
                slot: 0,
                input_level_dbu: None,
            },
            quiet(),
        );
        assert!(matches!(r, Err(UploadError::Rejected(_))), "{r:?}");
        let st = st.lock().unwrap();
        assert!(st.log.iter().any(|l| l == "LDBEGIN 0"));
        assert_eq!(st.audio, 2);
        assert!(st.stage.is_none());
    }

    #[test]
    fn chunk_error_aborts() {
        let (s, st) = setup();
        let nam = synthetic_nam(&[1, 2], 3, 2, 1, json!({}));
        let blob = convert_str(&nam, &ConvertOptions::default()).unwrap().blob;
        st.lock().unwrap().fail_on = Some(("LDB 0 ".into(), "offset".into()));
        let r = upload_capture(
            &s,
            CaptureJob {
                blob,
                name: "x".into(),
                slot: 0,
                input_level_dbu: None,
            },
            quiet(),
        );
        assert!(
            matches!(r, Err(UploadError::Transfer { offset: 0, .. })),
            "{r:?}"
        );
        let st = st.lock().unwrap();
        assert!(st.captures[0].as_ref().map(|c| c.0 != "x").unwrap_or(true));
        assert!(st.log.iter().any(|l| l == "LDBEGIN 0"));
        assert_eq!(st.audio, 2);
    }

    #[test]
    fn uploads_ir_with_fallback_on_oom() {
        let (s, st) = setup();
        let x: Vec<f64> = (0..6000)
            .map(|i| (i as f64 * 0.05).sin() * (-(i as f64) / 800.0).exp())
            .collect();
        st.lock().unwrap().ir_mem_limit = 2048 * 2; // 4076 samples don't fit, 2048 do
        let rep = upload_ir(
            &s,
            IrJob {
                mono48: x,
                name: "Cab 4x12".into(),
                slot: Some(2),
                assign_block: Some(9),
            },
            quiet(),
        )
        .unwrap();
        assert_eq!(rep.slot, 2);
        let st = st.lock().unwrap();
        let counts: Vec<usize> = st
            .log
            .iter()
            .filter(|l| l.starts_with("IRBEGIN ") && !l.starts_with("IRBEGIN 0 "))
            .map(|l| l.split_whitespace().nth(1).unwrap().parse().unwrap())
            .collect();
        // first attempt up to 4076 samples (trailing zeros stripped), then the 2048 fallback
        assert_eq!(counts.len(), 2, "{counts:?}");
        assert!(counts[0] > 2048 && counts[0] <= 4076, "{counts:?}");
        assert!(counts[1] <= 2048 && counts[1] > 1024, "{counts:?}");
        assert!(st.log.iter().any(|l| l == "IRBEGIN 0 0 48"));
        assert!(st.log.iter().any(|l| l == "ISAVE 2 Cab 4x12"));
        assert!(st.log.iter().any(|l| l == "FXIR 9 2"));
        assert_eq!(st.irs[2].as_ref().unwrap().0, "Cab 4x12");
    }

    #[test]
    fn slot7_uses_short_ir() {
        let (s, st) = setup();
        let x: Vec<f64> = (0..6000)
            .map(|i| (i as f64 * 0.05).sin() * (-(i as f64) / 3000.0).exp())
            .collect();
        upload_ir(
            &s,
            IrJob {
                mono48: x,
                name: "s7".into(),
                slot: Some(7),
                assign_block: None,
            },
            quiet(),
        )
        .unwrap();
        let st = st.lock().unwrap();
        let n: usize = st
            .log
            .iter()
            .find(|l| l.starts_with("IRBEGIN "))
            .unwrap()
            .split_whitespace()
            .nth(1)
            .unwrap()
            .parse()
            .unwrap();
        assert!(n <= 2028 && n > 1800, "{n}");
    }
}
