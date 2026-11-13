//! Bluetooth LE commands: transport mode, scan, connect, disconnect.

use crate::state::{Shared, TransportState};
use pulsedeck_core::ble::{self, BleDevice, TransportMode};
use tauri::State;

#[tauri::command]
pub fn transport_get(core: State<'_, Shared>) -> TransportState {
    core.transport_state()
}

/// Switch between USB (auto-connect) and Bluetooth (explicit connect). The current link is
/// closed by the connection manager on its next poll.
#[tauri::command]
pub fn transport_set(core: State<'_, Shared>, mode: TransportMode) -> TransportState {
    core.switch.set_mode(mode);
    core.emit_transport();
    core.transport_state()
}

/// Scan for pedals. `all = true` also lists unrelated devices (for pedals with other names).
#[tauri::command]
pub async fn ble_scan(core: State<'_, Shared>, all: bool) -> Result<Vec<BleDevice>, String> {
    let cfg = core.switch.ble().config().clone();
    tauri::async_runtime::spawn_blocking(move || ble::scan(&cfg, all))
        .await
        .map_err(|e| e.to_string())?
}

/// Pick the device; the manager opens it and runs the PING handshake. Progress and failures
/// arrive through `pedal:status` / `pedal:transport`.
#[tauri::command]
pub fn ble_connect(core: State<'_, Shared>, id: String, name: Option<String>) -> TransportState {
    core.switch.set_mode(TransportMode::Ble);
    core.switch.ble().select(&id, name.as_deref());
    core.emit_transport();
    core.transport_state()
}

#[tauri::command]
pub fn ble_disconnect(core: State<'_, Shared>) -> TransportState {
    core.switch.ble().clear();
    core.emit_transport();
    core.transport_state()
}
