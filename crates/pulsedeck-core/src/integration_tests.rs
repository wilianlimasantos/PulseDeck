use crate::manager::{ConnectionStatus, Manager, ManagerEvents};
use crate::parse::{parse_fxs, parse_fxt, Pong};
use crate::protocol::Protocol;
use crate::session::{CmdError, EventSink, Session, SessionEvent};
use crate::sim::{SimConnector, SimLink, SimState};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

type Rig = (Arc<Session>, Arc<Mutex<SimState>>, Arc<Mutex<Vec<String>>>);

fn session_with_events() -> Rig {
    let p = Arc::new(Protocol::embedded());
    let st = Arc::new(Mutex::new(SimState::new(&p)));
    let asyncs = Arc::new(Mutex::new(Vec::new()));
    let a2 = asyncs.clone();
    let sink: EventSink = Arc::new(move |e| {
        if let SessionEvent::Async(l) = e {
            a2.lock().unwrap().push(l);
        }
    });
    let s = Arc::new(Session::start(Box::new(SimLink::new(st.clone())), p, sink));
    s.set_max_line(190);
    (s, st, asyncs)
}

#[test]
fn concurrent_callers_never_interleave_replies() {
    let (s, st, _) = session_with_events();
    // add a block we can poke
    let id = {
        let r = s.send("FXA P 99 boost", None).unwrap();
        crate::parse::parse_fxa_id(&r.lines).unwrap()
    };
    let mut handles = vec![];
    for t in 0..8 {
        let s = s.clone();
        handles.push(std::thread::spawn(move || {
            for i in 0..60 {
                let v = -10 + ((t * 7 + i) % 30);
                let r = s.send(&format!("FXP {id} 0 {v}"), None).unwrap();
                assert_eq!(r.terminal(), format!("FXP ok {v}"), "thread {t} iter {i}");
                // interleave a multi-line command
                if i % 20 == 0 {
                    let fxs = s.send("FXS", None).unwrap();
                    assert!(fxs.terminal().starts_with("FXS peak"));
                    assert!(parse_fxs(&fxs.lines).blocks.iter().any(|b| b.id == id));
                }
            }
        }));
    }
    for h in handles {
        h.join().unwrap();
    }
    assert_eq!(st.lock().unwrap().overlap_violations, 0);
}

#[test]
fn async_lines_are_routed_not_part_of_replies() {
    let (s, st, asyncs) = session_with_events();
    st.lock().unwrap().inject_async_mid_reply = true;
    let r = s.send("FXS", None).unwrap();
    assert!(r.lines.iter().all(|l| !l.starts_with('@')));
    assert!(r.terminal().starts_with("FXS peak"));
    assert!(asyncs.lock().unwrap().iter().any(|l| l.starts_with("@L ")));
}

#[test]
fn errors_timeouts_and_limits() {
    let (s, _st, _) = session_with_events();
    match s.send("FXP 999 0 1", None) {
        Err(CmdError::Device { cmd, message }) => {
            assert_eq!(cmd, "FXP");
            assert!(message.contains("bad argument"));
        }
        other => panic!("{other:?}"),
    }
    // a command the sim never answers with its own prefix -> timeout, and the queue keeps working
    let t0 = Instant::now();
    let r = s.send("BOGUS", Some(Duration::from_millis(50)));
    assert!(matches!(r, Err(CmdError::Device { .. })), "{r:?}"); // sim answers ERR for unknown
    assert!(t0.elapsed() < Duration::from_millis(500));
    let long = format!("FXA P 99 {}", "x".repeat(300));
    assert!(matches!(
        s.send(&long, None),
        Err(CmdError::LineTooLong { .. })
    ));
    s.set_max_line(31);
    assert!(matches!(
        s.send(&format!("PSAVE 0 {}", "n".repeat(30)), None),
        Err(CmdError::LineTooLong { .. })
    ));
    assert!(matches!(s.send("PING\nPING", None), Err(CmdError::BadLine)));
    assert!(s.send("PING", None).is_ok());
}

#[test]
fn catalog_roundtrip_through_fxt() {
    let (s, _st, _) = session_with_events();
    let r = s.send("FXT", None).unwrap();
    let cat = parse_fxt(&r.lines);
    let reference = Protocol::embedded().reference_effects();
    assert_eq!(cat.len(), reference.len());
    let ir = cat.iter().find(|e| e.name == "ir").unwrap();
    assert_eq!(ir.parameters.len(), 4);
    assert_eq!(ir.parameters[3].max, 85);
}

struct Rec {
    statuses: Mutex<Vec<ConnectionStatus>>,
    connects: AtomicUsize,
    disconnects: AtomicUsize,
}

impl ManagerEvents for Rec {
    fn status(&self, s: &ConnectionStatus) {
        self.statuses.lock().unwrap().push(s.clone());
    }
    fn connected(&self, _s: Arc<Session>, _p: &Pong) {
        self.connects.fetch_add(1, Ordering::SeqCst);
    }
    fn disconnected(&self) {
        self.disconnects.fetch_add(1, Ordering::SeqCst);
    }
    fn async_line(&self, _l: &str) {}
}

fn wait_for(what: &str, f: impl Fn() -> bool) {
    let t0 = Instant::now();
    while t0.elapsed() < Duration::from_secs(5) {
        if f() {
            return;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    panic!("timed out waiting for {what}");
}

#[test]
fn manager_connects_validates_pong_and_reconnects_after_hotplug() {
    let proto = Arc::new(Protocol::embedded());
    let conn = Arc::new(SimConnector::new(&proto));
    let rec = Arc::new(Rec {
        statuses: Mutex::new(vec![]),
        connects: AtomicUsize::new(0),
        disconnects: AtomicUsize::new(0),
    });
    let mgr = Manager::start_with_poll(proto, conn.clone(), rec.clone(), Duration::from_millis(30));

    wait_for("first connect", || rec.connects.load(Ordering::SeqCst) == 1);
    match rec.statuses.lock().unwrap().last().unwrap() {
        ConnectionStatus::Connected {
            model,
            version,
            firmware_number,
            ..
        } => {
            assert_eq!(model, "Tank-G");
            assert_eq!(version, "M5.26");
            assert_eq!(*firmware_number, Some(526));
        }
        s => panic!("{s:?}"),
    }
    let session = mgr.session().unwrap();
    assert_eq!(session.max_line(), 190); // learned from the PONG firmware number
    assert!(session.send("PING", None).is_ok());

    conn.unplug();
    wait_for("disconnect", || rec.disconnects.load(Ordering::SeqCst) == 1);
    assert!(mgr.session().is_none());
    assert!(matches!(
        rec.statuses.lock().unwrap().last().unwrap(),
        ConnectionStatus::Searching
    ));

    conn.plug();
    wait_for("reconnect", || rec.connects.load(Ordering::SeqCst) == 2);
    assert!(mgr.session().unwrap().send("PING", None).is_ok());
    mgr.stop();
}

#[test]
fn manager_reports_non_responding_port() {
    // A link that never answers PING.
    struct Mute;
    impl crate::link::Link for Mute {
        fn write_line(&mut self, _l: &str) -> std::io::Result<()> {
            Ok(())
        }
        fn read_line(&mut self, t: Duration) -> std::io::Result<Option<String>> {
            std::thread::sleep(t.min(Duration::from_millis(5)));
            Ok(None)
        }
    }
    struct MuteConn;
    impl crate::link::Connector for MuteConn {
        fn candidates(&self) -> Vec<crate::link::PortCandidate> {
            vec![crate::link::PortCandidate {
                name: "X".into(),
                vid: 0x3654,
                pid: 1,
                product: None,
                serial_number: None,
            }]
        }
        fn open(
            &self,
            _: &crate::link::PortCandidate,
        ) -> std::io::Result<Box<dyn crate::link::Link>> {
            Ok(Box::new(Mute))
        }
    }
    // shrink timings so the test is quick: custom protocol with 1 attempt of 700ms is the default; fine
    let proto = Arc::new(Protocol::embedded());
    let rec = Arc::new(Rec {
        statuses: Mutex::new(vec![]),
        connects: AtomicUsize::new(0),
        disconnects: AtomicUsize::new(0),
    });
    let mgr = Manager::start_with_poll(
        proto,
        Arc::new(MuteConn),
        rec.clone(),
        Duration::from_millis(30),
    );
    // 5 attempts x 700 ms
    let t0 = Instant::now();
    while t0.elapsed() < Duration::from_secs(6) {
        if rec
            .statuses
            .lock()
            .unwrap()
            .iter()
            .any(|s| matches!(s, ConnectionStatus::NotResponding { .. }))
        {
            break;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    assert!(rec
        .statuses
        .lock()
        .unwrap()
        .iter()
        .any(|s| matches!(s, ConnectionStatus::NotResponding { .. })));
    assert_eq!(rec.connects.load(Ordering::SeqCst), 0);
    mgr.stop();
}

#[test]
fn tuner_mode_and_polling() {
    use crate::parse::{parse_tune, parse_tuner, TuneReading};
    let (s, st, _) = session_with_events();
    // off: the poll says so
    let r = s.send("TUNE", None).unwrap();
    assert_eq!(parse_tune(r.terminal()), Some(TuneReading::Off));
    // on, muted, A = 432
    let r = s.send("TUNER 1 1 432", None).unwrap();
    let t = parse_tuner(r.terminal()).unwrap();
    assert!(t.on && t.mute && t.ref_hz == 432);
    assert!(st.lock().unwrap().tuner_mute);
    // TUNE is not confused with the TUNER reply and follows the reference
    let r = s.send("TUNE", None).unwrap();
    match parse_tune(r.terminal()).unwrap() {
        TuneReading::Note { hz, cents, .. } => {
            assert!(hz > 70.0 && cents.abs() <= 50.0, "{hz} {cents}");
        }
        other => panic!("{other:?}"),
    }
    // bad reference is rejected by the pedal, state unchanged
    assert!(s.send("TUNER 1 1 9999", None).is_err());
    assert_eq!(st.lock().unwrap().tuner_ref, 432);
    // leaving tuner mode
    s.send("TUNER 0", None).unwrap();
    assert!(!st.lock().unwrap().tuner_on);
    assert_eq!(st.lock().unwrap().overlap_violations, 0);
    // the poll uses its own short timeout from protocol.json
    assert_eq!(
        Protocol::embedded().timeout_for("TUNE"),
        std::time::Duration::from_millis(600)
    );
}
