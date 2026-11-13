//! Global settings commands (GEQ, HPF, footswitch layout / actions, LEDs). They go through the
//! normal session queue, so they work the same over USB and Bluetooth.

use crate::state::Shared;
use pulsedeck_core::globals::{self, FsLed, GlobalSettings};
use pulsedeck_core::session::SessionWire;
use serde::Serialize;
use std::sync::atomic::Ordering;
use tauri::State;

#[derive(Serialize)]
pub struct GlobalsReply {
    #[serde(flatten)]
    pub settings: GlobalSettings,
    /// live edits not yet written to flash (`GSAVE`)
    pub dirty: bool,
}

fn wire(core: &Shared) -> Result<SessionWire, String> {
    Ok(SessionWire {
        session: core.session()?,
        protocol: core.protocol.clone(),
    })
}

async fn run<T: Send + 'static>(
    core: &Shared,
    f: impl FnOnce(&mut SessionWire) -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    let core = core.clone();
    tauri::async_runtime::spawn_blocking(move || f(&mut wire(&core)?))
        .await
        .map_err(|e| e.to_string())?
}

fn reply(core: &Shared, settings: GlobalSettings) -> GlobalsReply {
    GlobalsReply {
        settings,
        dirty: core.globals_dirty.load(Ordering::SeqCst),
    }
}

#[tauri::command]
pub async fn get_global_settings(core: State<'_, Shared>) -> Result<GlobalsReply, String> {
    let core = core.inner().clone();
    let s = run(&core, |w| Ok(globals::read_all(w))).await?;
    Ok(reply(&core, s))
}

/// One GEQ band in dB; returns the value the pedal received (clamped to the documented range).
#[tauri::command]
pub async fn set_global_eq(core: State<'_, Shared>, band: u8, value: i32) -> Result<i32, String> {
    let core = core.inner().clone();
    let v = run(&core, move |w| {
        globals::set_geq(w, usize::from(band), value).map_err(|e| e.to_string())
    })
    .await?;
    core.globals_dirty.store(true, Ordering::SeqCst);
    Ok(v)
}

/// All bands at once (reset / bypass / restore).
#[tauri::command]
pub async fn set_global_eq_all(
    core: State<'_, Shared>,
    values: Vec<i32>,
) -> Result<Vec<i32>, String> {
    let core = core.inner().clone();
    let v = run(&core, move |w| {
        globals::set_geq_all(w, &values).map_err(|e| e.to_string())
    })
    .await?;
    core.globals_dirty.store(true, Ordering::SeqCst);
    Ok(v)
}

#[tauri::command]
pub async fn set_input_hpf(core: State<'_, Shared>, hz: u32) -> Result<(), String> {
    let core = core.inner().clone();
    run(&core, move |w| {
        globals::set_hpf(w, hz).map_err(|e| e.to_string())
    })
    .await?;
    core.globals_dirty.store(true, Ordering::SeqCst);
    Ok(())
}

/// Apply a footswitch quick setup (`factory`, `scenes`, `presets`, `stompbox`); returns the
/// settings read back from the pedal.
#[tauri::command]
pub async fn set_footswitch_mode(
    core: State<'_, Shared>,
    setup: String,
) -> Result<GlobalsReply, String> {
    let core = core.inner().clone();
    let s = run(&core, move |w| {
        globals::apply_quick_setup(w, &setup).map_err(|e| e.to_string())?;
        Ok(globals::read_all(w))
    })
    .await?;
    core.globals_dirty.store(true, Ordering::SeqCst);
    Ok(reply(&core, s))
}

/// `LEDCFG <key>=<value>` (bright, tuner, tap, meter, hold, anim, idle).
#[tauri::command]
pub async fn set_led_config(
    core: State<'_, Shared>,
    key: String,
    value: String,
) -> Result<(), String> {
    let core = core.inner().clone();
    run(&core, move |w| {
        globals::set_ledcfg(w, &key, &value).map_err(|e| e.to_string())
    })
    .await?;
    core.globals_dirty.store(true, Ordering::SeqCst);
    Ok(())
}

/// Ring colors of one footswitch (`custom = false` -> automatic).
#[tauri::command]
pub async fn set_footswitch_led(core: State<'_, Shared>, fs: u8, led: FsLed) -> Result<(), String> {
    let core = core.inner().clone();
    run(&core, move |w| {
        globals::set_fsled(w, usize::from(fs), &led).map_err(|e| e.to_string())
    })
    .await?;
    core.globals_dirty.store(true, Ordering::SeqCst);
    Ok(())
}

/// `GSAVE`: write the global settings to flash.
#[tauri::command]
pub async fn save_global_settings(core: State<'_, Shared>) -> Result<(), String> {
    let core = core.inner().clone();
    run(&core, |w| globals::save(w).map_err(|e| e.to_string())).await?;
    core.globals_dirty.store(false, Ordering::SeqCst);
    Ok(())
}
