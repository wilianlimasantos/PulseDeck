//! Pedal tuner: `TUNER <on> <mute> <ref>` switches tuner mode (and silences the output), `TUNE`
//! is polled by the UI for the note / cents reading while it is on.

use crate::state::Shared;
use pulsedeck_core::parse::{parse_tune, parse_tuner, TuneReading, TunerState};
use tauri::State;

/// A4 reference range offered by the UI (integer Hz on the wire).
const REF_RANGE: std::ops::RangeInclusive<u32> = 432..=445;

async fn blocking<T: Send + 'static>(
    f: impl FnOnce() -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f)
        .await
        .map_err(|e| e.to_string())?
}

/// Enter / leave the pedal's tuner mode. `mute` silences the output while tuning.
#[tauri::command]
pub async fn tuner_set(
    core: State<'_, Shared>,
    on: bool,
    mute: bool,
    ref_hz: u32,
) -> Result<TunerState, String> {
    let core = core.inner().clone();
    blocking(move || {
        if !REF_RANGE.contains(&ref_hz) {
            return Err(format!(
                "Referência fora de {}–{} Hz",
                REF_RANGE.start(),
                REF_RANGE.end()
            ));
        }
        let line = if on {
            format!("TUNER 1 {} {ref_hz}", mute as u8)
        } else {
            "TUNER 0".to_string()
        };
        let r = core.cmd(&line)?;
        parse_tuner(r.terminal())
            .ok_or_else(|| format!("Resposta TUNER inesperada: {}", r.terminal()))
    })
    .await
}

/// One `TUNE` poll: the detected note and cents, no signal, or `off` when the pedal left tuner
/// mode by itself (footswitch).
#[tauri::command]
pub async fn tuner_read(core: State<'_, Shared>) -> Result<TuneReading, String> {
    let core = core.inner().clone();
    blocking(move || {
        let r = core.cmd("TUNE")?;
        Ok(parse_tune(r.terminal()).unwrap_or(TuneReading::NoSignal { level_db: None }))
    })
    .await
}
