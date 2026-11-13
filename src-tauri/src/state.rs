//! Shared application state: connection lifecycle, state mirror of the pedal and the sync logic.

use pulsedeck_core::ble::{self, BleConnector, SwitchConnector, TransportMode};
use pulsedeck_core::link::{Connector, SerialConnector};
use pulsedeck_core::manager::{ConnectionStatus, Manager, ManagerEvents};
use pulsedeck_core::parse::{
    parse_ampeq, parse_fxs, parse_fxt, parse_library, parse_lvl, parse_plist, parse_snaps, AmpEq,
    Chain, Level, Library, Pong, PresetList, Scenes,
};
use pulsedeck_core::protocol::{EffectType, Protocol};
use pulsedeck_core::session::{CmdError, Session};
use pulsedeck_core::sim::SimConnector;
use serde::Serialize;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tauri::{AppHandle, Emitter};

/// Edits not yet written to the pedal's flash.
#[derive(Debug, Clone, Copy, Serialize, Default, PartialEq)]
pub struct Unsaved {
    /// chain structure / capture changed: needs `PSAVE`
    pub preset: bool,
    /// knob, on/off, amp EQ changed: needs `SNAPSAVE`
    pub scene: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct Snapshot {
    pub unsaved: Unsaved,
    pub status: ConnectionStatus,
    pub effects: Vec<EffectType>,
    pub chain: Chain,
    pub presets: PresetList,
    pub scenes: Scenes,
    pub ampeq: AmpEq,
    pub level: Level,
    pub captures: Library,
    pub irs: Library,
    pub model: Option<String>,
    pub demo: bool,
}

impl Snapshot {
    fn new(effects: Vec<EffectType>, demo: bool) -> Self {
        Snapshot {
            unsaved: Unsaved::default(),
            status: ConnectionStatus::Searching,
            effects,
            chain: Chain::default(),
            presets: PresetList::default(),
            scenes: Scenes::default(),
            ampeq: AmpEq::default(),
            level: Level::default(),
            captures: Library::default(),
            irs: Library::default(),
            model: None,
            demo,
        }
    }
}

pub struct Core {
    pub app: AppHandle,
    pub protocol: Arc<Protocol>,
    pub snapshot: Mutex<Snapshot>,
    pub manager: Mutex<Option<Manager>>,
    pub switch: Arc<SwitchConnector>,
    /// global settings changed live and not yet saved with GSAVE
    pub globals_dirty: AtomicBool,
    dirty: AtomicBool,
}

pub type Shared = Arc<Core>;

#[derive(Debug, Clone, Serialize)]
pub struct BleSelected {
    pub id: String,
    pub name: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct TransportState {
    pub mode: TransportMode,
    pub selected: Option<BleSelected>,
    pub connected_ble: bool,
    /// why the last Bluetooth attempt / link failed
    pub error: Option<String>,
    /// non-fatal remark (e.g. pairing not completed: read-only)
    pub note: Option<String>,
    pub chunk_bytes: usize,
}

struct Bridge(Shared);

impl Core {
    pub fn start(app: AppHandle) -> Shared {
        let protocol = Arc::new(Protocol::embedded());
        let demo = std::env::var("PULSEDECK_SIM")
            .map(|v| v != "0" && !v.is_empty())
            .unwrap_or(false);
        let usb: Arc<dyn Connector> = if demo {
            Arc::new(SimConnector::new(&protocol))
        } else {
            Arc::new(SerialConnector::new((*protocol).clone()))
        };
        let switch = Arc::new(SwitchConnector::new(
            usb,
            Arc::new(BleConnector::new(protocol.ble_config())),
        ));
        let core = Arc::new(Core {
            app,
            switch: switch.clone(),
            globals_dirty: AtomicBool::new(false),
            snapshot: Mutex::new(Snapshot::new(protocol.reference_effects(), demo)),
            protocol: protocol.clone(),
            manager: Mutex::new(None),
            dirty: AtomicBool::new(false),
        });
        let mgr = Manager::start(protocol, switch, Arc::new(Bridge(core.clone())));
        *core.manager.lock().unwrap() = Some(mgr);
        core
    }

    pub fn session(&self) -> Result<Arc<Session>, String> {
        self.manager
            .lock()
            .unwrap()
            .as_ref()
            .and_then(|m| m.session())
            .ok_or_else(|| "Pedaleira desconectada".to_string())
    }

    /// Transport mode + Bluetooth selection, for the connection popover.
    pub fn transport_state(&self) -> TransportState {
        let ble = self.switch.ble();
        let connected = matches!(
            self.snapshot.lock().unwrap().status,
            ConnectionStatus::Connected { .. }
        );
        let ble_live = self.switch.mode() == TransportMode::Ble && connected;
        TransportState {
            mode: self.switch.mode(),
            selected: ble.selected().map(|(id, name)| BleSelected { id, name }),
            connected_ble: ble_live,
            error: ble.last_error(),
            note: ble.pair_note(),
            chunk_bytes: ble.chunk_bytes(),
        }
    }

    pub fn emit_transport(&self) {
        let _ = self.app.emit("pedal:transport", self.transport_state());
    }

    /// Big transfers (captures, IRs, backups) are impractically slow and fragile over Bluetooth.
    pub fn require_fast_link(&self, what: &str) -> Result<(), String> {
        if self.switch.mode() == TransportMode::Ble {
            return Err(format!(
                "{what} move vários KB e, por Bluetooth, cada linha é fatiada em pacotes de ~{} bytes: \
                 fica muito lento e uma queda no meio pode deixar o slot incompleto. \
                 Conecte a pedaleira pelo cabo USB para essa operação.",
                self.switch.ble().chunk_bytes()
            ));
        }
        Ok(())
    }

    pub fn snapshot(&self) -> Snapshot {
        self.snapshot.lock().unwrap().clone()
    }

    /// Record an edit that is live on the pedal but not in flash.
    pub fn mark_unsaved(&self, preset: bool, scene: bool) {
        let mut s = self.snapshot.lock().unwrap();
        let before = s.unsaved;
        s.unsaved.preset |= preset;
        s.unsaved.scene |= scene;
        let changed = s.unsaved != before;
        drop(s);
        if changed {
            self.emit_snapshot();
        }
    }

    pub fn clear_unsaved(&self, preset: bool, scene: bool) {
        let mut s = self.snapshot.lock().unwrap();
        if preset {
            s.unsaved.preset = false;
        }
        if scene {
            s.unsaved.scene = false;
        }
    }

    pub fn emit_snapshot(&self) {
        let _ = self.app.emit("pedal:snapshot", self.snapshot());
    }

    /// Send a command and map errors to a user-facing string.
    pub fn cmd(&self, line: &str) -> Result<pulsedeck_core::session::Reply, String> {
        self.session()?.send(line, None).map_err(|e| e.to_string())
    }

    /// FXS (+ derived values). Cheap enough to run after every structural edit.
    pub fn sync_chain(&self, session: &Session) -> Result<(), CmdError> {
        let r = session.send("FXS", None)?;
        let chain = parse_fxs(&r.lines);
        let mut s = self.snapshot.lock().unwrap();
        if chain.cpu.budget > 0 {
            s.level.cpu = chain.cpu.clone();
        }
        s.chain = chain;
        Ok(())
    }

    fn sync_presets(&self, session: &Session) {
        if let Ok(r) = session.send("PLIST", None) {
            self.snapshot.lock().unwrap().presets = parse_plist(&r.lines);
        }
        if let Ok(r) = session.send("SNAPS", None) {
            if let Some(sc) = parse_snaps(r.terminal()) {
                self.snapshot.lock().unwrap().scenes = sc;
            }
        }
    }

    pub fn sync_amp(&self, session: &Session) {
        if let Ok(r) = session.send("AMPEQ", None) {
            if let Some(e) = parse_ampeq(r.terminal()) {
                self.snapshot.lock().unwrap().ampeq = e;
            }
        }
        if let Ok(r) = session.send("MODEL", None) {
            let name = pulsedeck_core::parse::quoted(r.terminal()).map(|s| s.to_string());
            self.snapshot.lock().unwrap().model = name;
        }
    }

    pub fn sync_libraries(&self, session: &Session) {
        if let Ok(r) = session.send("CLIST", None) {
            self.snapshot.lock().unwrap().captures = parse_library(&r.lines, "CAP", "CLIST end");
        }
        if let Ok(r) = session.send("ILIST", None) {
            self.snapshot.lock().unwrap().irs = parse_library(&r.lines, "IRS", "ILIST end");
        }
    }

    /// Everything that depends on the current preset / scene.
    pub fn sync_state(&self, session: &Session) {
        let _ = self.sync_chain(session);
        self.sync_presets(session);
        self.sync_amp(session);
        self.emit_snapshot();
    }

    pub fn sync_all(&self, session: &Session) {
        if let Ok(r) = session.send("FXT", None) {
            let fx = parse_fxt(&r.lines);
            if !fx.is_empty() {
                self.snapshot.lock().unwrap().effects = fx;
            }
        }
        self.sync_libraries(session);
        self.sync_state(session);
    }

    fn poll_loop(self: Arc<Self>, session: Arc<Session>) {
        let mut last_gen: Option<i64> = None;
        let mut last_ps: (Option<i32>, Option<i32>) = (None, None);
        while session.is_connected() {
            std::thread::sleep(Duration::from_millis(250));
            let Ok(r) = session.send("LVL", None) else {
                continue;
            };
            let Some(lvl) = parse_lvl(r.terminal()) else {
                continue;
            };
            let changed = (last_gen.is_some() && lvl.chain_generation != last_gen)
                || (last_ps.0.is_some() && (lvl.preset, lvl.scene) != last_ps);
            // preset / scene switched (footswitch or UI): the loaded state replaces unsaved edits
            if last_ps.0.is_some() && lvl.preset != last_ps.0 {
                self.clear_unsaved(true, true);
            } else if last_ps.1.is_some() && lvl.scene != last_ps.1 {
                self.clear_unsaved(false, true);
            }
            last_gen = lvl.chain_generation;
            last_ps = (lvl.preset, lvl.scene);
            {
                let mut s = self.snapshot.lock().unwrap();
                s.level = Level {
                    cpu: lvl.cpu.clone(),
                    ..lvl.clone()
                };
            }
            let _ = self.app.emit("pedal:meter", &lvl);
            if changed || self.dirty.swap(false, Ordering::SeqCst) {
                self.sync_state(&session);
            }
        }
    }
}

impl ManagerEvents for Bridge {
    fn status(&self, status: &ConnectionStatus) {
        self.0.snapshot.lock().unwrap().status = status.clone();
        let _ = self.0.app.emit("pedal:status", status);
        self.0.emit_transport();
    }

    fn connected(&self, session: Arc<Session>, _pong: &Pong) {
        let core = self.0.clone();
        std::thread::spawn(move || {
            if core.switch.mode() == TransportMode::Ble {
                // learn the negotiated MTU so writes use MTU-3 bytes instead of 20
                if let Ok(r) = session.send("BLE", None) {
                    let info = r.lines.iter().find_map(|l| ble::parse_ble_info(l));
                    core.switch.ble().set_mtu(info.and_then(|i| i.mtu));
                }
                core.emit_transport();
            }
            core.sync_all(&session);
            core.poll_loop(session);
        });
    }

    fn disconnected(&self) {
        self.0.globals_dirty.store(false, Ordering::SeqCst);
        {
            let mut s = self.0.snapshot.lock().unwrap();
            s.chain = Chain::default();
            s.level = Level::default();
            s.model = None;
        }
        self.0.emit_snapshot();
        self.0.emit_transport();
    }

    fn async_line(&self, line: &str) {
        // @K: knob / footswitch changed on the pedal itself -> re-read the chain on the next tick
        if line.starts_with("@K") {
            self.0.dirty.store(true, Ordering::SeqCst);
        }
    }
}
