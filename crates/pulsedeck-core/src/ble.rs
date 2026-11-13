//! Bluetooth LE transport (btleplug): the same text protocol as the USB serial line, carried over
//! a GATT UART (Nordic UART Service by default, see `transport.alternative_transport` in
//! `protocol.json`).
//!
//! The module is split in two halves:
//! * pure logic (device filtering, GATT endpoint selection, MTU chunking, line reassembly), unit
//!   tested without a radio;
//! * the btleplug glue (`scan`, [`BleLink`], [`BleConnector`]) that runs on a private tokio
//!   runtime and presents a blocking [`Link`] to the session, so the "one command in flight"
//!   machinery of [`crate::session`] is reused unchanged.

use crate::link::{Connector, Link, PortCandidate};
use crate::protocol::BleConfig;
use btleplug::api::{
    Central, CentralEvent, CharPropFlags, Characteristic, Manager as _, Peripheral as _,
    ScanFilter, WriteType,
};
use btleplug::platform::{Adapter, Manager, Peripheral};
use futures::StreamExt;
use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::io;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{self, RecvTimeoutError};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};
use uuid::Uuid;

/// Prefix of [`PortCandidate::name`] for Bluetooth devices (`ble:<peripheral id>`).
pub const PORT_PREFIX: &str = "ble:";

// ====================================================================== pure logic

/// Does the advertised name look like one of the pedals (case-insensitive substring)?
pub fn name_matches(name: &str, filters: &[String]) -> bool {
    let n = name.to_ascii_lowercase();
    filters
        .iter()
        .any(|f| !f.is_empty() && n.contains(&f.to_ascii_lowercase()))
}

fn parse_uuid(s: &str) -> Option<Uuid> {
    Uuid::parse_str(s).ok()
}

/// Does the device advertise the pedal's service or one of the generic UART bridge services?
pub fn services_match(advertised: &[Uuid], cfg: &BleConfig) -> bool {
    let wanted: Vec<Uuid> = std::iter::once(&cfg.service)
        .chain(cfg.extra_services.iter())
        .filter_map(|s| parse_uuid(s))
        .collect();
    advertised.iter().any(|u| wanted.contains(u))
}

/// Characteristic properties that matter for a serial-over-GATT link.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Props {
    pub read: bool,
    pub write: bool,
    pub write_no_rsp: bool,
    pub notify: bool,
    pub indicate: bool,
}

impl From<CharPropFlags> for Props {
    fn from(f: CharPropFlags) -> Self {
        Props {
            read: f.contains(CharPropFlags::READ),
            write: f.contains(CharPropFlags::WRITE),
            write_no_rsp: f.contains(CharPropFlags::WRITE_WITHOUT_RESPONSE),
            notify: f.contains(CharPropFlags::NOTIFY),
            indicate: f.contains(CharPropFlags::INDICATE),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CharInfo {
    pub service: Uuid,
    pub uuid: Uuid,
    pub props: Props,
}

impl CharInfo {
    fn writable(&self) -> bool {
        self.props.write || self.props.write_no_rsp
    }
    fn notifies(&self) -> bool {
        self.props.notify || self.props.indicate
    }
}

/// The characteristics used for the text lines.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Endpoints {
    pub service: Uuid,
    /// host -> pedal
    pub write: Uuid,
    /// the write characteristic only supports "write without response"
    pub write_no_rsp: bool,
    /// pedal -> host
    pub notify: Uuid,
    /// reading it asks the OS for an encrypted (paired) link
    pub pair: Option<Uuid>,
}

/// GATT services 0x1800..0x18ff (Generic Access, Device Information, ...): never the UART.
fn is_standard_service(u: Uuid) -> bool {
    let base = Uuid::parse_str("00000000-0000-1000-8000-00805f9b34fb")
        .map(|b| b.as_u128())
        .unwrap_or(0);
    let v = u.as_u128();
    v & !(0xffff_ffffu128 << 96) == base && (0x1800..=0x18ff).contains(&((v >> 96) as u32))
}

/// Find the write and notify characteristics. Services are tried in this order: the documented
/// Nordic UART service, the generic UART services from the settings, then any other
/// non-standard service that has both a writable and a notifying characteristic.
pub fn pick_endpoints(chars: &[CharInfo], cfg: &BleConfig) -> Option<Endpoints> {
    let nus = parse_uuid(&cfg.service);
    let (rx, tx, pair) = (
        parse_uuid(&cfg.rx),
        parse_uuid(&cfg.tx),
        parse_uuid(&cfg.pair),
    );
    let mut order: Vec<Uuid> = nus.into_iter().collect();
    order.extend(cfg.extra_services.iter().filter_map(|s| parse_uuid(s)));
    for c in chars {
        if !order.contains(&c.service) && !is_standard_service(c.service) {
            order.push(c.service);
        }
    }
    for svc in order {
        let mine: Vec<&CharInfo> = chars.iter().filter(|c| c.service == svc).collect();
        let is_nus = Some(svc) == nus;
        // notify: the documented TX characteristic, else the first that notifies
        let notify = mine
            .iter()
            .find(|c| is_nus && Some(c.uuid) == tx && c.notifies())
            .or_else(|| mine.iter().find(|c| c.notifies()));
        let Some(notify) = notify else { continue };
        // write: the documented RX characteristic, else one that is not the notify one, else the
        // notify characteristic itself when it is also writable (a single bidirectional char)
        let write = mine
            .iter()
            .find(|c| is_nus && Some(c.uuid) == rx && c.writable())
            .or_else(|| mine.iter().find(|c| c.writable() && c.uuid != notify.uuid))
            .or_else(|| mine.iter().find(|c| c.writable()));
        let Some(write) = write else { continue };
        return Some(Endpoints {
            service: svc,
            write: write.uuid,
            // with-response writes are flow controlled by the link layer: prefer them
            write_no_rsp: !write.props.write,
            notify: notify.uuid,
            pair: mine.iter().find(|c| Some(c.uuid) == pair).map(|c| c.uuid),
        });
    }
    None
}

/// Bytes per GATT write: `MTU - 3` when the pedal reported its MTU, else the 20-byte default.
pub fn chunk_size(mtu: Option<usize>, cfg: &BleConfig) -> usize {
    match mtu {
        Some(m) if m > 23 => (m - 3).min(cfg.chunk_max).max(cfg.chunk_default),
        _ => cfg.chunk_default,
    }
}

/// Send `data` in pieces of at most `size` bytes with `delay` between them (not after the last),
/// so the pedal's small BLE RX buffer is never overrun.
pub fn write_chunks(
    data: &[u8],
    size: usize,
    delay: Duration,
    mut write: impl FnMut(&[u8]) -> io::Result<()>,
    mut sleep: impl FnMut(Duration),
) -> io::Result<()> {
    for (i, piece) in data.chunks(size.max(1)).enumerate() {
        if i > 0 && !delay.is_zero() {
            sleep(delay);
        }
        write(piece)?;
    }
    Ok(())
}

/// Reassembles text lines from notification payloads, which split anywhere (a line may arrive in
/// several packets, and one packet may carry several lines).
#[derive(Debug, Default)]
pub struct LineAssembler {
    buf: Vec<u8>,
}

impl LineAssembler {
    pub fn push(&mut self, bytes: &[u8]) -> Vec<String> {
        self.buf.extend_from_slice(bytes);
        let mut out = Vec::new();
        while let Some(nl) = self.buf.iter().position(|b| *b == b'\n') {
            let mut line: Vec<u8> = self.buf.drain(..=nl).collect();
            line.pop();
            if line.last() == Some(&b'\r') {
                line.pop();
            }
            out.push(String::from_utf8_lossy(&line).into_owned());
        }
        if self.buf.len() > 64 * 1024 {
            self.buf.clear(); // garbage without newlines: keep memory bounded
        }
        out
    }
}

/// What the `BLE` command reports (only the parts the host uses).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct BleInfo {
    pub mtu: Option<usize>,
    /// the link is encrypted (paired)
    pub encrypted: Option<bool>,
}

/// `BLE ... mtu=<n> enc=<0|1> ...`
pub fn parse_ble_info(line: &str) -> Option<BleInfo> {
    let rest = line.strip_prefix("BLE")?;
    if !rest.is_empty() && !rest.starts_with(' ') {
        return None;
    }
    let kv = crate::parse::kv_map(rest);
    Some(BleInfo {
        mtu: kv.get("mtu").and_then(|v| v.parse().ok()),
        encrypted: kv.get("enc").map(|v| *v == "1"),
    })
}

/// `ERR <cmd> pair`: the pedal answers like this to a central that is not paired.
pub fn is_pair_error(line: &str) -> bool {
    let mut t = line.split_whitespace();
    t.next() == Some("ERR") && t.next().is_some() && t.next() == Some("pair") && t.next().is_none()
}

// ======================================================================= btleplug glue

/// A device found while scanning.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct BleDevice {
    /// opaque peripheral id (pass it back to connect)
    pub id: String,
    pub name: Option<String>,
    /// signal strength in dBm (closer to 0 = stronger)
    pub rssi: Option<i16>,
    pub services: Vec<String>,
    /// name or services look like the pedal
    pub matches: bool,
}

/// Private runtime for all btleplug futures. The session / command threads are plain threads, so
/// `block_on` from them is fine.
fn runtime() -> &'static tokio::runtime::Runtime {
    static RT: OnceLock<tokio::runtime::Runtime> = OnceLock::new();
    RT.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .thread_name("pulsedeck-ble")
            .enable_all()
            .build()
            .expect("start the Bluetooth runtime")
    })
}

fn bt_err(e: impl std::fmt::Display) -> String {
    let s = e.to_string();
    let l = s.to_ascii_lowercase();
    if l.contains("permission") || l.contains("not authorized") || l.contains("access denied") {
        format!("Sem permissão para usar o Bluetooth: {s}")
    } else if l.contains("powered")
        || l.contains("not ready")
        || l.contains("rfkill")
        || l.contains("blocked")
    {
        format!("O Bluetooth está desligado ou bloqueado: {s}")
    } else {
        s
    }
}

async fn first_adapter() -> Result<Adapter, String> {
    let manager = Manager::new().await.map_err(bt_err)?;
    manager
        .adapters()
        .await
        .map_err(bt_err)?
        .into_iter()
        .next()
        .ok_or_else(|| "Nenhum adaptador Bluetooth encontrado neste computador.".to_string())
}

/// Scan for `cfg.scan` and list the devices that look like the pedal (or all devices with
/// `include_all`), strongest signal first.
pub fn scan(cfg: &BleConfig, include_all: bool) -> Result<Vec<BleDevice>, String> {
    let cfg = cfg.clone();
    runtime().block_on(async move {
        let adapter = first_adapter().await?;
        adapter
            .start_scan(ScanFilter::default())
            .await
            .map_err(bt_err)?;
        tokio::time::sleep(cfg.scan).await;
        let _ = adapter.stop_scan().await;
        let mut found = Vec::new();
        for p in adapter.peripherals().await.map_err(bt_err)? {
            let Ok(Some(props)) = p.properties().await else {
                continue;
            };
            let name = props.local_name.filter(|n| !n.trim().is_empty());
            let matches = name
                .as_deref()
                .is_some_and(|n| name_matches(n, &cfg.name_filters))
                || services_match(&props.services, &cfg);
            if !matches && !include_all {
                continue;
            }
            found.push(BleDevice {
                id: p.id().to_string(),
                name,
                rssi: props.rssi,
                services: props.services.iter().map(|u| u.to_string()).collect(),
                matches,
            });
        }
        found.sort_by_key(|d| (!d.matches, std::cmp::Reverse(d.rssi.unwrap_or(i16::MIN))));
        Ok(found)
    })
}

/// Selection / error state shared between the UI commands, the connector and the live link.
#[derive(Debug, Default)]
struct Sel {
    device: Option<(String, Option<String>)>,
    last_error: Option<String>,
    /// the link works but the pedal did not accept pairing (read-only)
    pair_note: Option<String>,
}

type SharedSel = Arc<Mutex<Sel>>;

/// A connected GATT UART as a blocking line [`Link`].
pub struct BleLink {
    peripheral: Peripheral,
    write: Characteristic,
    write_type: WriteType,
    chunk: Arc<AtomicUsize>,
    delay: Duration,
    rx: mpsc::Receiver<Vec<u8>>,
    asm: LineAssembler,
    pending: VecDeque<String>,
    alive: Arc<AtomicBool>,
    sel: SharedSel,
    last_probe: Instant,
}

impl BleLink {
    fn mark_lost(&self, why: &str) {
        if self.alive.swap(false, Ordering::SeqCst) {
            let mut s = self.sel.lock().unwrap();
            s.last_error = Some(why.into());
            s.device = None; // stop the manager from reconnecting by itself
        }
    }

    fn lost_err(&self) -> io::Error {
        io::Error::new(io::ErrorKind::BrokenPipe, "conexão Bluetooth perdida")
    }

    async fn open(
        cfg: &BleConfig,
        id: &str,
        chunk: Arc<AtomicUsize>,
        sel: SharedSel,
    ) -> Result<BleLink, String> {
        let adapter = first_adapter().await?;
        // known devices first; otherwise a short scan to rediscover it
        let find = |adapter: Adapter, id: String| async move {
            let list = adapter.peripherals().await.map_err(bt_err)?;
            Ok::<_, String>(list.into_iter().find(|p| p.id().to_string() == id))
        };
        let mut peripheral = find(adapter.clone(), id.to_string()).await?;
        if peripheral.is_none() {
            adapter
                .start_scan(ScanFilter::default())
                .await
                .map_err(bt_err)?;
            tokio::time::sleep(Duration::from_millis(3000)).await;
            let _ = adapter.stop_scan().await;
            peripheral = find(adapter.clone(), id.to_string()).await?;
        }
        let peripheral = peripheral.ok_or_else(|| {
            "Dispositivo Bluetooth não encontrado: ligue a pedaleira e procure de novo.".to_string()
        })?;

        tokio::time::timeout(cfg.connect_timeout, peripheral.connect())
            .await
            .map_err(|_| "Tempo esgotado ao conectar por Bluetooth.".to_string())?
            .map_err(bt_err)?;
        peripheral.discover_services().await.map_err(bt_err)?;

        let infos: Vec<CharInfo> = peripheral
            .characteristics()
            .iter()
            .map(|c| CharInfo {
                service: c.service_uuid,
                uuid: c.uuid,
                props: c.properties.into(),
            })
            .collect();
        let Some(ep) = pick_endpoints(&infos, cfg) else {
            let _ = peripheral.disconnect().await;
            return Err("O dispositivo não expõe um serviço serial (UART) compatível.".into());
        };
        let find_char = |u: Uuid| {
            peripheral
                .characteristics()
                .into_iter()
                .find(|c| c.uuid == u && c.service_uuid == ep.service)
        };
        let write = find_char(ep.write).ok_or("característica de escrita sumiu")?;
        let notify = find_char(ep.notify).ok_or("característica de notificação sumiu")?;

        // pedal -> host: notifications into a channel the blocking reader waits on
        let (tx, rx) = mpsc::channel::<Vec<u8>>();
        let alive = Arc::new(AtomicBool::new(true));
        let mut stream = peripheral.notifications().await.map_err(bt_err)?;
        peripheral.subscribe(&notify).await.map_err(bt_err)?;
        {
            let (alive, notify_uuid) = (alive.clone(), notify.uuid);
            tokio::spawn(async move {
                while let Some(n) = stream.next().await {
                    if n.uuid == notify_uuid && tx.send(n.value).is_err() {
                        break;
                    }
                }
                alive.store(false, Ordering::SeqCst); // the stream ends when the device is gone
            });
        }
        // explicit disconnect events (the notification stream does not always end)
        if let Ok(mut events) = adapter.events().await {
            let (alive, pid) = (alive.clone(), peripheral.id());
            tokio::spawn(async move {
                while let Some(ev) = events.next().await {
                    if let CentralEvent::DeviceDisconnected(id) = ev {
                        if id == pid {
                            alive.store(false, Ordering::SeqCst);
                            break;
                        }
                    }
                }
            });
        }

        // reading the pairing characteristic makes the OS ask for an encrypted link; without
        // one the pedal is read-only ("ERR <cmd> pair"). Failing is not fatal.
        if let Some(pu) = ep.pair {
            if let Some(pc) = find_char(pu) {
                let note = match tokio::time::timeout(cfg.pair_timeout, peripheral.read(&pc)).await {
                    Ok(Ok(_)) => None,
                    Ok(Err(e)) => Some(format!("Emparelhamento não concluído ({}): a pedaleira fica em modo só-leitura.", bt_err(e))),
                    Err(_) => Some("Emparelhamento não concluído a tempo: a pedaleira fica em modo só-leitura.".into()),
                };
                sel.lock().unwrap().pair_note = note;
            }
        }

        Ok(BleLink {
            peripheral,
            write,
            write_type: if ep.write_no_rsp {
                WriteType::WithoutResponse
            } else {
                WriteType::WithResponse
            },
            chunk,
            delay: cfg.chunk_delay,
            rx,
            asm: LineAssembler::default(),
            pending: VecDeque::new(),
            alive,
            sel,
            last_probe: Instant::now(),
        })
    }
}

impl Link for BleLink {
    fn write_line(&mut self, line: &str) -> io::Result<()> {
        if !self.alive.load(Ordering::SeqCst) {
            return Err(self.lost_err());
        }
        let mut data = Vec::with_capacity(line.len() + 1);
        data.extend_from_slice(line.as_bytes());
        data.push(b'\n');
        let size = self.chunk.load(Ordering::SeqCst);
        let (p, c, wt) = (&self.peripheral, &self.write, self.write_type);
        let res = write_chunks(
            &data,
            size,
            self.delay,
            |piece| {
                runtime()
                    .block_on(p.write(c, piece, wt))
                    .map_err(|e| io::Error::new(io::ErrorKind::BrokenPipe, e.to_string()))
            },
            std::thread::sleep,
        );
        if res.is_err() {
            self.mark_lost("Conexão Bluetooth perdida durante o envio.");
        }
        res
    }

    fn read_line(&mut self, timeout: Duration) -> io::Result<Option<String>> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(l) = self.pending.pop_front() {
                return Ok(Some(l));
            }
            // drain what is already there before judging the link
            match self.rx.try_recv() {
                Ok(bytes) => {
                    self.pending.extend(self.asm.push(&bytes));
                    continue;
                }
                Err(mpsc::TryRecvError::Empty) => {}
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.mark_lost("Conexão Bluetooth perdida.");
                    return Err(self.lost_err());
                }
            }
            if !self.alive.load(Ordering::SeqCst) {
                self.mark_lost("Conexão Bluetooth perdida.");
                return Err(self.lost_err());
            }
            // some stacks never report a silent drop: ask now and then
            if self.last_probe.elapsed() > Duration::from_secs(2) {
                self.last_probe = Instant::now();
                if !runtime()
                    .block_on(self.peripheral.is_connected())
                    .unwrap_or(false)
                {
                    self.mark_lost("Conexão Bluetooth perdida.");
                    return Err(self.lost_err());
                }
            }
            let now = Instant::now();
            if now >= deadline {
                return Ok(None);
            }
            let slice = (deadline - now).min(Duration::from_millis(50));
            match self.rx.recv_timeout(slice) {
                Ok(bytes) => self.pending.extend(self.asm.push(&bytes)),
                Err(RecvTimeoutError::Timeout) => {}
                Err(RecvTimeoutError::Disconnected) => {
                    self.mark_lost("Conexão Bluetooth perdida.");
                    return Err(self.lost_err());
                }
            }
        }
    }
}

impl Drop for BleLink {
    fn drop(&mut self) {
        self.alive.store(false, Ordering::SeqCst);
        let p = self.peripheral.clone();
        runtime().spawn(async move {
            let _ = p.disconnect().await;
        });
    }
}

/// [`Connector`] for the one Bluetooth device the user picked: it lists it as the only candidate
/// and opens a [`BleLink`] to it. A failed open or a lost link clears the selection (so nothing
/// retries in a loop) and leaves the reason in [`BleConnector::last_error`].
pub struct BleConnector {
    cfg: BleConfig,
    sel: SharedSel,
    chunk: Arc<AtomicUsize>,
}

impl BleConnector {
    pub fn new(cfg: BleConfig) -> Self {
        let chunk = Arc::new(AtomicUsize::new(cfg.chunk_default));
        Self {
            cfg,
            sel: Arc::new(Mutex::new(Sel::default())),
            chunk,
        }
    }

    pub fn config(&self) -> &BleConfig {
        &self.cfg
    }

    /// Choose the device to connect to; the connection manager picks it up on its next poll.
    pub fn select(&self, id: &str, name: Option<&str>) {
        let mut s = self.sel.lock().unwrap();
        s.device = Some((id.into(), name.map(String::from)));
        s.last_error = None;
        s.pair_note = None;
        self.chunk.store(self.cfg.chunk_default, Ordering::SeqCst);
    }

    /// Forget the selection: the manager closes the link.
    pub fn clear(&self) {
        let mut s = self.sel.lock().unwrap();
        s.device = None;
        s.pair_note = None;
    }

    pub fn selected(&self) -> Option<(String, Option<String>)> {
        self.sel.lock().unwrap().device.clone()
    }

    pub fn last_error(&self) -> Option<String> {
        self.sel.lock().unwrap().last_error.clone()
    }

    pub fn pair_note(&self) -> Option<String> {
        self.sel.lock().unwrap().pair_note.clone()
    }

    /// Apply the MTU the pedal reported (`BLE` command): writes become `MTU - 3` bytes.
    pub fn set_mtu(&self, mtu: Option<usize>) {
        self.chunk
            .store(chunk_size(mtu, &self.cfg), Ordering::SeqCst);
    }

    pub fn chunk_bytes(&self) -> usize {
        self.chunk.load(Ordering::SeqCst)
    }
}

impl Connector for BleConnector {
    fn candidates(&self) -> Vec<PortCandidate> {
        self.sel
            .lock()
            .unwrap()
            .device
            .iter()
            .map(|(id, name)| PortCandidate {
                name: format!("{PORT_PREFIX}{id}"),
                vid: 0,
                pid: 0,
                product: name.clone(),
                serial_number: None,
            })
            .collect()
    }

    fn open(&self, port: &PortCandidate) -> io::Result<Box<dyn Link>> {
        let id = port
            .name
            .strip_prefix(PORT_PREFIX)
            .ok_or_else(|| io::Error::other("not a Bluetooth device"))?
            .to_string();
        let res = runtime().block_on(BleLink::open(
            &self.cfg,
            &id,
            self.chunk.clone(),
            self.sel.clone(),
        ));
        match res {
            Ok(link) => {
                self.sel.lock().unwrap().last_error = None;
                Ok(Box::new(link))
            }
            Err(e) => {
                let mut s = self.sel.lock().unwrap();
                s.last_error = Some(e.clone());
                s.device = None;
                Err(io::Error::other(e))
            }
        }
    }
}

/// Which transport the connection manager is using.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum TransportMode {
    Usb,
    Ble,
}

/// Routes the manager to the USB serial connector or the Bluetooth one. Switching the mode makes
/// the current port vanish from the candidates, so the manager closes the old session cleanly.
pub struct SwitchConnector {
    usb: Arc<dyn Connector>,
    ble: Arc<BleConnector>,
    mode: Mutex<TransportMode>,
}

impl SwitchConnector {
    pub fn new(usb: Arc<dyn Connector>, ble: Arc<BleConnector>) -> Self {
        Self {
            usb,
            ble,
            mode: Mutex::new(TransportMode::Usb),
        }
    }

    pub fn mode(&self) -> TransportMode {
        *self.mode.lock().unwrap()
    }

    pub fn set_mode(&self, mode: TransportMode) {
        *self.mode.lock().unwrap() = mode;
        if mode == TransportMode::Usb {
            self.ble.clear();
        }
    }

    pub fn ble(&self) -> &Arc<BleConnector> {
        &self.ble
    }
}

impl Connector for SwitchConnector {
    fn candidates(&self) -> Vec<PortCandidate> {
        match self.mode() {
            TransportMode::Usb => self.usb.candidates(),
            TransportMode::Ble => self.ble.candidates(),
        }
    }

    fn open(&self, port: &PortCandidate) -> io::Result<Box<dyn Link>> {
        if port.name.starts_with(PORT_PREFIX) {
            self.ble.open(port)
        } else {
            self.usb.open(port)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::Protocol;

    fn cfg() -> BleConfig {
        Protocol::embedded().ble_config()
    }

    fn u(s: &str) -> Uuid {
        Uuid::parse_str(s).unwrap()
    }

    fn ch(service: &str, uuid: &str, p: Props) -> CharInfo {
        CharInfo {
            service: u(service),
            uuid: u(uuid),
            props: p,
        }
    }

    const W: Props = Props {
        read: false,
        write: true,
        write_no_rsp: true,
        notify: false,
        indicate: false,
    };
    const N: Props = Props {
        read: false,
        write: false,
        write_no_rsp: false,
        notify: true,
        indicate: false,
    };
    const R: Props = Props {
        read: true,
        write: false,
        write_no_rsp: false,
        notify: false,
        indicate: false,
    };

    const NUS: &str = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
    const NUS_RX: &str = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
    const NUS_TX: &str = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
    const NUS_PAIR: &str = "6e400004-b5a3-f393-e0a9-e50e24dcca9e";

    #[test]
    fn config_comes_from_protocol_json() {
        let c = cfg();
        assert_eq!(c.service, NUS);
        assert_eq!((c.rx.as_str(), c.tx.as_str()), (NUS_RX, NUS_TX));
        assert_eq!(c.chunk_default, 20);
        for n in ["UltraRig", "Tank-G", "Tank-B", "M-VAVE"] {
            assert!(c.name_filters.iter().any(|x| x == n), "{n}");
        }
        assert!(c.extra_services.iter().any(|s| s.starts_with("0000ae00")));
        assert!(c.extra_services.iter().any(|s| s.starts_with("0000fff0")));
        assert!(c.scan >= Duration::from_secs(1));
    }

    #[test]
    fn names_and_services_are_filtered() {
        let c = cfg();
        for n in [
            "Tank-G",
            "tank-b v2",
            "UltraRig-1A2B",
            "M-VAVE Chocolate",
            "My UltraRig",
        ] {
            assert!(name_matches(n, &c.name_filters), "{n}");
        }
        for n in ["JBL Flip", "", "Galaxy Buds"] {
            assert!(!name_matches(n, &c.name_filters), "{n}");
        }
        assert!(services_match(&[u(NUS)], &c));
        assert!(services_match(
            &[u("0000ae00-0000-1000-8000-00805f9b34fb")],
            &c
        ));
        assert!(services_match(
            &[u("0000fff0-0000-1000-8000-00805f9b34fb")],
            &c
        ));
        assert!(!services_match(
            &[u("0000180f-0000-1000-8000-00805f9b34fb")],
            &c
        ));
    }

    #[test]
    fn picks_the_nordic_uart_endpoints() {
        let chars = vec![
            ch(
                "00001800-0000-1000-8000-00805f9b34fb",
                "00002a00-0000-1000-8000-00805f9b34fb",
                R,
            ),
            ch(NUS, NUS_TX, N),
            ch(NUS, NUS_RX, W),
            ch(NUS, NUS_PAIR, R),
        ];
        let ep = pick_endpoints(&chars, &cfg()).unwrap();
        assert_eq!(ep.service, u(NUS));
        assert_eq!((ep.write, ep.notify), (u(NUS_RX), u(NUS_TX)));
        assert_eq!(ep.pair, Some(u(NUS_PAIR)));
        assert!(
            !ep.write_no_rsp,
            "with-response writes are preferred when supported"
        );
    }

    #[test]
    fn falls_back_to_generic_uart_services() {
        // ae00: ae01 write, ae02 notify
        let ae = "0000ae00-0000-1000-8000-00805f9b34fb";
        let chars = vec![
            ch(ae, "0000ae01-0000-1000-8000-00805f9b34fb", W),
            ch(ae, "0000ae02-0000-1000-8000-00805f9b34fb", N),
        ];
        let ep = pick_endpoints(&chars, &cfg()).unwrap();
        assert_eq!(ep.write, u("0000ae01-0000-1000-8000-00805f9b34fb"));
        assert_eq!(ep.notify, u("0000ae02-0000-1000-8000-00805f9b34fb"));
        assert_eq!(ep.pair, None);

        // fff0 with one bidirectional characteristic
        let ff = "0000fff0-0000-1000-8000-00805f9b34fb";
        let both = Props {
            notify: true,
            write_no_rsp: true,
            ..Props::default()
        };
        let ep = pick_endpoints(
            &[ch(ff, "0000fff1-0000-1000-8000-00805f9b34fb", both)],
            &cfg(),
        )
        .unwrap();
        assert_eq!(ep.write, ep.notify);
        assert!(ep.write_no_rsp);

        // an unknown vendor service with both kinds of characteristic still works
        let vendor = "12345678-1234-5678-1234-56789abcdef0";
        let ep = pick_endpoints(
            &[
                ch(vendor, "12345678-1234-5678-1234-56789abcdef1", N),
                ch(vendor, "12345678-1234-5678-1234-56789abcdef2", W),
            ],
            &cfg(),
        )
        .unwrap();
        assert_eq!(ep.service, u(vendor));
    }

    #[test]
    fn rejects_devices_without_a_serial_service() {
        let battery = "0000180f-0000-1000-8000-00805f9b34fb";
        let chars = vec![
            ch(
                battery,
                "00002a19-0000-1000-8000-00805f9b34fb",
                Props {
                    read: true,
                    notify: true,
                    ..Props::default()
                },
            ),
            ch(
                "00001800-0000-1000-8000-00805f9b34fb",
                "00002a00-0000-1000-8000-00805f9b34fb",
                W,
            ),
        ];
        assert_eq!(pick_endpoints(&chars, &cfg()), None);
        // only a notify characteristic, nothing to write to
        assert_eq!(pick_endpoints(&[ch(NUS, NUS_TX, N)], &cfg()), None);
    }

    #[test]
    fn chunk_size_follows_the_mtu() {
        let c = cfg();
        assert_eq!(chunk_size(None, &c), 20);
        assert_eq!(chunk_size(Some(23), &c), 20);
        assert_eq!(chunk_size(Some(185), &c), 182);
        assert_eq!(chunk_size(Some(517), &c), c.chunk_max);
    }

    #[test]
    fn long_lines_are_split_with_delays_between_pieces_only() {
        let line: Vec<u8> = (0..191u32).map(|i| b'a' + (i % 26) as u8).collect(); // 190 chars + \n
        let mut pieces: Vec<Vec<u8>> = Vec::new();
        let mut sleeps = 0;
        write_chunks(
            &line,
            20,
            Duration::from_millis(12),
            |p| {
                pieces.push(p.to_vec());
                Ok(())
            },
            |d| {
                assert_eq!(d, Duration::from_millis(12));
                sleeps += 1;
            },
        )
        .unwrap();
        assert_eq!(pieces.len(), 10);
        assert!(pieces.iter().all(|p| p.len() <= 20));
        assert_eq!(pieces.concat(), line);
        assert_eq!(sleeps, 9);
        // a failing write stops the transfer
        let mut n = 0;
        let r = write_chunks(
            &line,
            20,
            Duration::ZERO,
            |_| {
                n += 1;
                if n == 3 {
                    Err(io::Error::other("gone"))
                } else {
                    Ok(())
                }
            },
            |_| {},
        );
        assert!(r.is_err() && n == 3);
    }

    #[test]
    fn notifications_are_reassembled_into_lines() {
        let mut a = LineAssembler::default();
        assert!(a.push(b"PONG tankg_fw M5.").is_empty());
        assert_eq!(a.push(b"48 ble\r\nFXE ok"), vec!["PONG tankg_fw M5.48 ble"]);
        assert_eq!(
            a.push(b" cpu=1/2\n@K 3 1\r\n\n"),
            vec!["FXE ok cpu=1/2", "@K 3 1", ""]
        );
        assert!(a.push(b"partial").is_empty());
    }

    #[test]
    fn ble_info_and_pair_errors() {
        let i = parse_ble_info("BLE on mtu=185 enc=1 bonds=2 pairwin=0 bt=1").unwrap();
        assert_eq!((i.mtu, i.encrypted), (Some(185), Some(true)));
        assert_eq!(parse_ble_info("BLE enc=0").unwrap().encrypted, Some(false));
        assert!(parse_ble_info("BLEX mtu=1").is_none());
        assert!(parse_ble_info("PONG").is_none());
        assert!(is_pair_error("ERR FXP pair"));
        assert!(!is_pair_error("ERR FXP bad argument"));
        assert!(!is_pair_error("FXP ok"));
    }

    #[test]
    fn selection_drives_the_candidates() {
        let c = BleConnector::new(cfg());
        assert!(c.candidates().is_empty());
        c.select("dev-1", Some("UltraRig-12"));
        let cands = c.candidates();
        assert_eq!(cands.len(), 1);
        assert_eq!(cands[0].name, "ble:dev-1");
        assert_eq!(cands[0].product.as_deref(), Some("UltraRig-12"));
        c.clear();
        assert!(c.candidates().is_empty());
        // mtu -> chunk size; a new selection starts from the safe default again
        c.set_mtu(Some(185));
        assert_eq!(c.chunk_bytes(), 182);
        c.select("dev-2", None);
        assert_eq!(c.chunk_bytes(), 20);
    }

    struct FakeUsb;
    impl Connector for FakeUsb {
        fn candidates(&self) -> Vec<PortCandidate> {
            vec![PortCandidate {
                name: "/dev/ttyACM0".into(),
                vid: 0x3654,
                pid: 1,
                product: None,
                serial_number: None,
            }]
        }
        fn open(&self, _: &PortCandidate) -> io::Result<Box<dyn Link>> {
            Err(io::Error::other("fake"))
        }
    }

    #[test]
    fn switching_transport_swaps_the_candidates() {
        let ble = Arc::new(BleConnector::new(cfg()));
        let sw = SwitchConnector::new(Arc::new(FakeUsb), ble.clone());
        assert_eq!(sw.candidates()[0].name, "/dev/ttyACM0");
        sw.set_mode(TransportMode::Ble);
        assert!(sw.candidates().is_empty(), "no device chosen yet");
        ble.select("dev-1", Some("Tank-G"));
        assert_eq!(sw.candidates()[0].name, "ble:dev-1");
        // back to USB forgets the Bluetooth selection
        sw.set_mode(TransportMode::Usb);
        assert!(ble.selected().is_none());
        assert_eq!(sw.candidates()[0].name, "/dev/ttyACM0");
    }
}
