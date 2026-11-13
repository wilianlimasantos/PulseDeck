//! Connection manager: finds the pedal (USB VID from `protocol.json`), validates it with
//! `PING` -> `PONG`, and keeps reconnecting after unplug / reboot (hot-plug).

use crate::link::{Connector, PortCandidate};
use crate::parse::{parse_pong, Pong};
use crate::protocol::Protocol;
use crate::session::{EventSink, Session, SessionEvent};
use serde::{Deserialize, Serialize};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum ConnectionStatus {
    /// No candidate port with the pedal's VID.
    Searching,
    /// A port exists but did not answer PONG (other firmware / busy).
    NotResponding { port: String, reason: String },
    Connected {
        port: String,
        model: String,
        version: String,
        firmware_number: Option<u32>,
        flags: Vec<String>,
        pong: String,
    },
}

pub trait ManagerEvents: Send + Sync {
    fn status(&self, status: &ConnectionStatus);
    /// Called on the manager thread right after the handshake; use it for the initial state sync.
    fn connected(&self, session: Arc<Session>, pong: &Pong);
    fn disconnected(&self);
    fn async_line(&self, line: &str);
}

struct Current {
    session: Arc<Session>,
    port: PortCandidate,
}

pub struct Manager {
    inner: Arc<Inner>,
    join: Mutex<Option<JoinHandle<()>>>,
}

struct Inner {
    protocol: Arc<Protocol>,
    connector: Arc<dyn Connector>,
    events: Arc<dyn ManagerEvents>,
    current: Mutex<Option<Current>>,
    stop: AtomicBool,
    last_port: Mutex<Option<String>>,
    poll: Duration,
}

/// Open `port`, send PING up to N times (700 ms each), return the session on the first PONG.
pub fn handshake(
    protocol: &Arc<Protocol>,
    connector: &dyn Connector,
    port: &PortCandidate,
    sink: EventSink,
) -> Result<(Arc<Session>, Pong), String> {
    let link = connector
        .open(port)
        .map_err(|e| format!("cannot open {}: {e}", port.name))?;
    let session = Arc::new(Session::start(link, protocol.clone(), sink));
    for _ in 0..protocol.ping_attempts {
        match session.send("PING", Some(protocol.pong_timeout)) {
            Ok(reply) => {
                if let Some(p) = parse_pong(reply.terminal()) {
                    session.set_max_line(protocol.max_line_chars(p.firmware_number));
                    return Ok((session, p));
                }
            }
            Err(crate::session::CmdError::Timeout { .. }) => {}
            Err(e) => {
                session.close();
                return Err(e.to_string());
            }
        }
    }
    session.close();
    Err("no PONG: not an UltraRig firmware or wrong port".into())
}

impl Manager {
    pub fn start(
        protocol: Arc<Protocol>,
        connector: Arc<dyn Connector>,
        events: Arc<dyn ManagerEvents>,
    ) -> Manager {
        Self::start_with_poll(protocol, connector, events, Duration::from_millis(1000))
    }

    pub fn start_with_poll(
        protocol: Arc<Protocol>,
        connector: Arc<dyn Connector>,
        events: Arc<dyn ManagerEvents>,
        poll: Duration,
    ) -> Manager {
        let inner = Arc::new(Inner {
            protocol,
            connector,
            events,
            current: Mutex::new(None),
            stop: AtomicBool::new(false),
            last_port: Mutex::new(None),
            poll,
        });
        let i2 = inner.clone();
        let join = thread::Builder::new()
            .name("pulsedeck-connect".into())
            .spawn(move || i2.run())
            .expect("spawn connection manager");
        Manager {
            inner,
            join: Mutex::new(Some(join)),
        }
    }

    pub fn session(&self) -> Option<Arc<Session>> {
        self.inner
            .current
            .lock()
            .unwrap()
            .as_ref()
            .map(|c| c.session.clone())
    }

    pub fn stop(&self) {
        self.inner.stop.store(true, Ordering::SeqCst);
        if let Some(j) = self.join.lock().unwrap().take() {
            let _ = j.join();
        }
        if let Some(c) = self.inner.current.lock().unwrap().take() {
            c.session.close();
        }
    }
}

impl Drop for Manager {
    fn drop(&mut self) {
        self.inner.stop.store(true, Ordering::SeqCst);
    }
}

impl Inner {
    fn run(self: Arc<Self>) {
        let mut last_status: Option<ConnectionStatus> = None;
        let emit = |s: ConnectionStatus,
                    last: &mut Option<ConnectionStatus>,
                    ev: &Arc<dyn ManagerEvents>| {
            if last.as_ref() != Some(&s) {
                ev.status(&s);
                *last = Some(s);
            }
        };
        while !self.stop.load(Ordering::SeqCst) {
            let present = self.connector.candidates();
            // drop a dead / vanished session
            let lost = {
                let cur = self.current.lock().unwrap();
                cur.as_ref().map(|c| {
                    !c.session.is_connected() || !present.iter().any(|p| p.name == c.port.name)
                })
            };
            if lost == Some(true) {
                if let Some(c) = self.current.lock().unwrap().take() {
                    c.session.close();
                }
                self.events.disconnected();
                emit(ConnectionStatus::Searching, &mut last_status, &self.events);
            }
            let have = self.current.lock().unwrap().is_some();
            if !have {
                if present.is_empty() {
                    emit(ConnectionStatus::Searching, &mut last_status, &self.events);
                } else {
                    // prefer the port used last time, then the rest
                    let pref = self.last_port.lock().unwrap().clone();
                    let mut ordered = present.clone();
                    ordered.sort_by_key(|p| Some(&p.name) != pref.as_ref());
                    let mut last_err = String::new();
                    let mut last_name = String::new();
                    for port in ordered {
                        let events = self.events.clone();
                        let sink: EventSink = Arc::new(move |e| {
                            if let SessionEvent::Async(l) = e {
                                events.async_line(&l)
                            }
                        });
                        match handshake(&self.protocol, self.connector.as_ref(), &port, sink) {
                            Ok((session, pong)) => {
                                *self.last_port.lock().unwrap() = Some(port.name.clone());
                                *self.current.lock().unwrap() = Some(Current {
                                    session: session.clone(),
                                    port: port.clone(),
                                });
                                emit(
                                    ConnectionStatus::Connected {
                                        port: port.name.clone(),
                                        model: pong.model().into(),
                                        version: pong.version.clone(),
                                        firmware_number: pong.firmware_number,
                                        flags: pong.flags.clone(),
                                        pong: pong.raw.clone(),
                                    },
                                    &mut last_status,
                                    &self.events,
                                );
                                self.events.connected(session, &pong);
                                last_err.clear();
                                break;
                            }
                            Err(e) => {
                                last_err = e;
                                last_name = port.name.clone();
                            }
                        }
                    }
                    if !last_err.is_empty() {
                        emit(
                            ConnectionStatus::NotResponding {
                                port: last_name,
                                reason: last_err,
                            },
                            &mut last_status,
                            &self.events,
                        );
                    }
                }
            }
            // sleep in small steps so stop() is responsive
            let mut waited = Duration::ZERO;
            while waited < self.poll && !self.stop.load(Ordering::SeqCst) {
                thread::sleep(Duration::from_millis(20));
                waited += Duration::from_millis(20);
            }
        }
    }
}
