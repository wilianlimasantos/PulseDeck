//! Command session: a worker thread owns the link and serialises every command.
//!
//! Guarantees (protocol `framing.flow_control`):
//! * strictly one command in flight — callers queue jobs, only the worker writes;
//! * a command ends at its terminal line, an `ERR` line, or its timeout;
//! * lines starting with `@` are never part of a reply: they are routed to the event callback;
//! * any other line received while nothing is pending is discarded.

use crate::link::Link;
use crate::protocol::Protocol;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

#[derive(Debug, Clone, PartialEq)]
pub enum CmdError {
    Timeout {
        cmd: String,
    },
    /// The pedal answered `ERR <CMD> <message>`.
    Device {
        cmd: String,
        message: String,
    },
    Io(String),
    Disconnected,
    LineTooLong {
        len: usize,
        max: usize,
    },
    BadLine,
}

impl std::fmt::Display for CmdError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            CmdError::Timeout { cmd } => write!(f, "{cmd}: timeout waiting for the pedal"),
            CmdError::Device { cmd, message } => write!(f, "{cmd}: {message}"),
            CmdError::Io(e) => write!(f, "serial error: {e}"),
            CmdError::Disconnected => write!(f, "pedal disconnected"),
            CmdError::LineTooLong { len, max } => {
                write!(f, "command too long ({len} > {max} chars)")
            }
            CmdError::BadLine => write!(f, "invalid command line"),
        }
    }
}
impl std::error::Error for CmdError {}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct Reply {
    /// All non-async lines received for the command, terminal line last.
    pub lines: Vec<String>,
}

impl Reply {
    pub fn terminal(&self) -> &str {
        self.lines.last().map(|s| s.as_str()).unwrap_or("")
    }
}

#[derive(Debug, Clone)]
pub enum SessionEvent {
    /// An unsolicited line starting with `@`.
    Async(String),
    Disconnected(String),
}

pub type EventSink = Arc<dyn Fn(SessionEvent) + Send + Sync>;

/// Does `line` terminate the reply of `command_line`?
pub fn is_terminal(command_line: &str, line: &str) -> bool {
    let mut parts = command_line.split_whitespace();
    let name = parts.next().unwrap_or("");
    let has_args = parts.next().is_some();
    let starts = |p: &str| line.starts_with(p);
    match name {
        "FXT" => starts("FXT end") || starts("FXT fin"),
        "FXS" => starts("FXS peak") || starts("FXS pico"),
        "PLIST" => starts("PLIST end") || starts("PLIST fin"),
        "CLIST" => starts("CLIST end"),
        "ILIST" => starts("ILIST end"),
        "SNAPNAME" if !has_args => starts("SNAPNAME end"),
        "FXMIX" | "FSA" | "FSLED" if !has_args => {
            let end = format!("{name} end");
            starts(&end)
        }
        "FXA" => starts("FX cpu"),
        "PING" => starts("PONG"),
        "LDEND" => starts("LOAD"),
        "IREND" => starts("IR "),
        _ => {
            let first = line.split_whitespace().next().unwrap_or("");
            if first == name {
                return true;
            }
            // chunk acks: "OK LDB <offset> <len>", "OK LDBEGIN <n>"
            let ok = format!("OK {name} ");
            line.starts_with(&ok)
        }
    }
}

type Done = Sender<Result<Reply, CmdError>>;

enum Job {
    Cmd {
        line: String,
        timeout: Duration,
        done: Done,
    },
    Exclusive(Box<dyn FnOnce(&mut Exec) + Send>),
    Shutdown,
}

/// Executor handed to exclusive transactions (uploads): same single-flight rules, but no other
/// job can interleave until the closure returns.
pub struct Exec<'a> {
    link: &'a mut dyn Link,
    protocol: &'a Protocol,
    max_line: usize,
    sink: &'a EventSink,
    failed: &'a mut Option<String>,
}

impl<'a> Exec<'a> {
    pub fn max_line(&self) -> usize {
        self.max_line
    }

    pub fn protocol(&self) -> &Protocol {
        self.protocol
    }

    /// Send with the protocol's timeout for that command.
    pub fn send(&mut self, line: &str) -> Result<Reply, CmdError> {
        let name = line.split_whitespace().next().unwrap_or("");
        let t = self.protocol.timeout_for(name);
        self.send_timeout(line, t)
    }

    pub fn send_timeout(&mut self, line: &str, timeout: Duration) -> Result<Reply, CmdError> {
        if self.failed.is_some() {
            return Err(CmdError::Disconnected);
        }
        let line = line.trim_end_matches(['\r', '\n']);
        if line.is_empty() || line.contains('\n') || line.contains('\r') || !line.is_ascii() {
            return Err(CmdError::BadLine);
        }
        if line.len() > self.max_line {
            return Err(CmdError::LineTooLong {
                len: line.len(),
                max: self.max_line,
            });
        }
        let name = line.split_whitespace().next().unwrap_or("").to_string();

        // Anything already waiting is stale: route '@' lines, drop the rest.
        if let Err(e) = self.drain_idle() {
            return Err(self.fail(e));
        }
        if let Err(e) = self.link.write_line(line) {
            return Err(self.fail(e.to_string()));
        }
        let deadline = Instant::now() + timeout;
        let mut lines = Vec::new();
        loop {
            let now = Instant::now();
            if now >= deadline {
                return Err(CmdError::Timeout { cmd: name });
            }
            let got = match self.link.read_line(deadline - now) {
                Ok(g) => g,
                Err(e) => return Err(self.fail(e.to_string())),
            };
            let Some(l) = got else { continue };
            if l.trim().is_empty() {
                continue;
            }
            if l.starts_with('@') {
                (self.sink)(SessionEvent::Async(l));
                continue;
            }
            if let Some(rest) = l.strip_prefix("ERR") {
                let rest = rest.trim();
                let mut it = rest.splitn(2, ' ');
                let who = it.next().unwrap_or("");
                // unsolicited calibration errors are not replies
                if who == "CALIB" && name != "CALIB" {
                    continue;
                }
                let message = it.next().unwrap_or("").to_string();
                return Err(CmdError::Device {
                    cmd: if who.is_empty() {
                        name
                    } else {
                        who.to_string()
                    },
                    message,
                });
            }
            let done = is_terminal(line, &l);
            lines.push(l);
            if done {
                return Ok(Reply { lines });
            }
        }
    }

    /// Like `send`, then keeps collecting lines until the pedal is quiet for `settle`.
    /// For read-back commands whose multi-line reply has no documented sentinel (GEQ, FSPRESS...).
    pub fn send_settle(&mut self, line: &str, settle: Duration) -> Result<Reply, CmdError> {
        let mut reply = self.send(line)?;
        loop {
            match self.link.read_line(settle) {
                Ok(Some(l)) if l.starts_with('@') => (self.sink)(SessionEvent::Async(l)),
                Ok(Some(l)) if l.trim().is_empty() => {}
                Ok(Some(l)) => reply.lines.push(l),
                Ok(None) => return Ok(reply),
                Err(e) => return Err(self.fail(e.to_string())),
            }
        }
    }

    fn drain_idle(&mut self) -> Result<(), String> {
        loop {
            match self.link.read_line(Duration::from_millis(0)) {
                Ok(Some(l)) => {
                    if l.starts_with('@') {
                        (self.sink)(SessionEvent::Async(l));
                    }
                }
                Ok(None) => return Ok(()),
                Err(e) => return Err(e.to_string()),
            }
        }
    }

    fn fail(&mut self, why: String) -> CmdError {
        *self.failed = Some(why.clone());
        CmdError::Io(why)
    }
}

/// Anything that can run protocol commands one at a time: an exclusive transaction (`Exec`) or,
/// in tests / simple callers, a whole `Session`. Upload and backup logic is written against this.
pub trait Wire {
    fn send(&mut self, line: &str) -> Result<Reply, CmdError>;
    fn send_settle(&mut self, line: &str, settle: Duration) -> Result<Reply, CmdError>;
    fn max_line(&self) -> usize;
    fn protocol(&self) -> &Protocol;
}

impl Wire for Exec<'_> {
    fn send(&mut self, line: &str) -> Result<Reply, CmdError> {
        Exec::send(self, line)
    }
    fn send_settle(&mut self, line: &str, settle: Duration) -> Result<Reply, CmdError> {
        Exec::send_settle(self, line, settle)
    }
    fn max_line(&self) -> usize {
        self.max_line
    }
    fn protocol(&self) -> &Protocol {
        self.protocol
    }
}

/// `Wire` over a plain session: each command is queued individually (other jobs may interleave).
pub struct SessionWire {
    pub session: Arc<Session>,
    pub protocol: Arc<Protocol>,
}

impl Wire for SessionWire {
    fn send(&mut self, line: &str) -> Result<Reply, CmdError> {
        self.session.send(line, None)
    }
    fn send_settle(&mut self, line: &str, settle: Duration) -> Result<Reply, CmdError> {
        let l = line.to_string();
        self.session
            .exclusive(move |ex| ex.send_settle(&l, settle))?
    }
    fn max_line(&self) -> usize {
        self.session.max_line()
    }
    fn protocol(&self) -> &Protocol {
        &self.protocol
    }
}

pub struct Session {
    tx: Mutex<Sender<Job>>,
    connected: Arc<AtomicBool>,
    max_line: Arc<AtomicUsize>,
    join: Mutex<Option<JoinHandle<()>>>,
}

impl Session {
    pub fn start(link: Box<dyn Link>, protocol: Arc<Protocol>, sink: EventSink) -> Session {
        let (tx, rx) = mpsc::channel::<Job>();
        let connected = Arc::new(AtomicBool::new(true));
        let max_line = Arc::new(AtomicUsize::new(protocol.max_line_chars(None)));
        let c2 = connected.clone();
        let m2 = max_line.clone();
        let join = thread::Builder::new()
            .name("pulsedeck-serial".into())
            .spawn(move || worker(link, protocol, sink, rx, c2, m2))
            .expect("spawn serial worker");
        Session {
            tx: Mutex::new(tx),
            connected,
            max_line,
            join: Mutex::new(Some(join)),
        }
    }

    pub fn is_connected(&self) -> bool {
        self.connected.load(Ordering::SeqCst)
    }

    /// Firmware-dependent line limit, learned from the PONG line.
    pub fn set_max_line(&self, n: usize) {
        self.max_line.store(n, Ordering::SeqCst);
    }

    pub fn max_line(&self) -> usize {
        self.max_line.load(Ordering::SeqCst)
    }

    fn submit(&self, job: Job) -> Result<(), CmdError> {
        self.tx
            .lock()
            .unwrap()
            .send(job)
            .map_err(|_| CmdError::Disconnected)
    }

    /// Queue a command and block until its reply, error or timeout.
    pub fn send(&self, line: &str, timeout: Option<Duration>) -> Result<Reply, CmdError> {
        let (done, rx) = mpsc::channel();
        let t = timeout.unwrap_or(Duration::ZERO);
        self.submit(Job::Cmd {
            line: line.to_string(),
            timeout: t,
            done,
        })?;
        rx.recv().map_err(|_| CmdError::Disconnected)?
    }

    /// Run a multi-command transaction without interleaving (file uploads).
    pub fn exclusive<R: Send + 'static>(
        &self,
        f: impl FnOnce(&mut Exec) -> R + Send + 'static,
    ) -> Result<R, CmdError> {
        let (rtx, rrx) = mpsc::channel();
        self.submit(Job::Exclusive(Box::new(move |ex| {
            let _ = rtx.send(f(ex));
        })))?;
        rrx.recv().map_err(|_| CmdError::Disconnected)
    }

    pub fn close(&self) {
        let _ = self.submit(Job::Shutdown);
        if let Some(j) = self.join.lock().unwrap().take() {
            let _ = j.join();
        }
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        let _ = self.tx.lock().map(|t| t.send(Job::Shutdown));
    }
}

fn worker(
    mut link: Box<dyn Link>,
    protocol: Arc<Protocol>,
    sink: EventSink,
    rx: Receiver<Job>,
    connected: Arc<AtomicBool>,
    max_line: Arc<AtomicUsize>,
) {
    let mut failed: Option<String> = None;
    loop {
        match rx.recv_timeout(Duration::from_millis(10)) {
            Ok(Job::Shutdown) => break,
            Ok(job) => {
                let mut ex = Exec {
                    link: link.as_mut(),
                    protocol: &protocol,
                    max_line: max_line.load(Ordering::SeqCst),
                    sink: &sink,
                    failed: &mut failed,
                };
                match job {
                    Job::Cmd {
                        line,
                        timeout,
                        done,
                    } => {
                        let r = if timeout.is_zero() {
                            ex.send(&line)
                        } else {
                            ex.send_timeout(&line, timeout)
                        };
                        let _ = done.send(r);
                    }
                    Job::Exclusive(f) => f(&mut ex),
                    Job::Shutdown => unreachable!(),
                }
            }
            Err(RecvTimeoutError::Timeout) => {
                // idle: keep consuming pushes (@L / @K) so the buffer never grows stale
                let mut ex = Exec {
                    link: link.as_mut(),
                    protocol: &protocol,
                    max_line: 0,
                    sink: &sink,
                    failed: &mut failed,
                };
                if let Err(e) = ex.drain_idle() {
                    failed = Some(e);
                }
            }
            Err(RecvTimeoutError::Disconnected) => break,
        }
        if failed.is_some() {
            break;
        }
    }
    connected.store(false, Ordering::SeqCst);
    // fail whatever is still queued
    while let Ok(job) = rx.try_recv() {
        if let Job::Cmd { done, .. } = job {
            let _ = done.send(Err(CmdError::Disconnected));
        }
    }
    if let Some(why) = failed {
        sink(SessionEvent::Disconnected(why));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn terminals() {
        assert!(is_terminal("FXT", "FXT end"));
        assert!(is_terminal("FXT", "FXT fin"));
        assert!(!is_terminal("FXT", "FXT 1 gate cost=4"));
        assert!(is_terminal("FXS", "FXS peak=1 ticks skipped=0"));
        assert!(is_terminal("FXS", "FXS pico=1"));
        assert!(!is_terminal("FXS", "FXS amp=1"));
        assert!(is_terminal(
            "FXA P 99 delay",
            "FX cpu=1/2 ticks (50%) mem_fx=0"
        ));
        assert!(!is_terminal("FXA P 99 delay", "FXA id=3 delay"));
        assert!(is_terminal("FXP 3 0 10", "FXP ok 10"));
        assert!(is_terminal("LDB 0 aabb", "OK LDB 0 2"));
        assert!(is_terminal("LDEND deadbeef", "LOAD rc=0 name='x'"));
        assert!(is_terminal(
            "IREND deadbeef",
            "IR ok 10 samples gain 0 mdB fs 48"
        ));
        assert!(is_terminal("SNAP 1", "SNAP ok 1 missing=0 ms=3"));
        assert!(!is_terminal("SNAP 1", "SNAPS 1111 current=1"));
        assert!(is_terminal("SNAPNAME", "SNAPNAME end"));
        assert!(is_terminal(
            "SNAPNAME 1 Lead",
            "SNAPNAME ok 1 'Lead' preset=0"
        ));
        assert!(is_terminal("PLIST", "PLIST end current=3"));
    }
}
