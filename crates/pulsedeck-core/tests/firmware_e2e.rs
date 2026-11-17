//! End to end: PulseDeck's real session / upload / parse code against the REAL firmware logic of the open Tank-G firmware, running as a host
//! process (firmware-tankg/build/host_pedal: the whole tg_main loop with serial on stdin/stdout and the flash region in a file).
//! Build it first:   (cd firmware-tankg && tools/host_pedal.sh)      -- the test is skipped (passes with a note) when the binary is missing.
use pulsedeck_core::link::Link;
use pulsedeck_core::parse::{parse_fxa_id, parse_fxs, parse_library, parse_lvl, parse_plist};
use pulsedeck_core::protocol::Protocol;
use pulsedeck_core::session::{EventSink, Session};
use pulsedeck_core::upload::{upload_capture_wire, upload_ir_wire, CaptureJob, IrJob, ProgressFn};
use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{channel, Receiver, RecvTimeoutError};
use std::sync::Arc;
use std::time::Duration;

struct ProcLink { child: Child, stdin: ChildStdin, rx: Receiver<String> }
impl Link for ProcLink {
    fn write_line(&mut self, line: &str) -> std::io::Result<()> { writeln!(self.stdin, "{line}")?; self.stdin.flush() }
    fn read_line(&mut self, timeout: Duration) -> std::io::Result<Option<String>> {
        match self.rx.recv_timeout(timeout) {
            Ok(l) => Ok(Some(l)),
            Err(RecvTimeoutError::Timeout) => Ok(None),
            Err(RecvTimeoutError::Disconnected) => Err(std::io::Error::new(std::io::ErrorKind::BrokenPipe, "pedal process ended")),
        }
    }
}
impl Drop for ProcLink { fn drop(&mut self) { let _ = self.child.kill(); let _ = self.child.wait(); } }

fn pedal_bin() -> Option<PathBuf> {
    let p = std::env::var("TANKG_HOST_PEDAL").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../firmware-tankg/build/host_pedal"));
    p.exists().then_some(p)
}
fn power_on(bin: &PathBuf, flash: &PathBuf) -> Arc<Session> {
    let mut child = Command::new(bin).env("TG_FLASH_FILE", flash).stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().expect("start host pedal");
    let stdin = child.stdin.take().unwrap(); let out = child.stdout.take().unwrap();
    let (tx, rx) = channel();
    std::thread::spawn(move || { for l in BufReader::new(out).lines().map_while(Result::ok) { if tx.send(l.trim_end().to_string()).is_err() { break; } } });
    let sink: EventSink = Arc::new(|_| {});
    let s = Arc::new(Session::start(Box::new(ProcLink { child, stdin, rx }), Arc::new(Protocol::embedded()), sink));
    let pong = s.send("PING", Some(Duration::from_secs(3))).expect("PING");
    assert!(pong.terminal().starts_with("PONG tankg_fw"));
    s.set_max_line(190);
    s
}
fn one(s: &Session, c: &str) -> String { s.send(c, None).unwrap_or_else(|e| panic!("{c}: {e}")).terminal().to_string() }
fn sound(s: &Session) -> String { one(s, "XAUDIO") }

/// A NAM A2 "Lite" file with the shape of the real captures (3 channels, 23 layers, kernels 6 and 15, head 16), deterministic small weights.
fn stock_lite_nam() -> String {
    let ks: Vec<usize> = vec![6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6];
    let ds: Vec<usize> = vec![1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13, 1, 3, 7, 17, 41, 101, 239];
    let (c, hk) = (3usize, 16usize); let mut seed = 5u32;
    let mut rnd = move || { seed = seed.wrapping_mul(1664525).wrapping_add(1013904223); ((seed >> 8) as f64 / 16777216.0) * 2.0 - 1.0 };
    let mut w: Vec<f64> = (0..c).map(|_| rnd() * 0.8).collect();
    for k in &ks { w.extend((0..c * c * k).map(|_| rnd() * 0.25)); w.extend((0..c).map(|_| rnd() * 0.05)); w.extend((0..c).map(|_| rnd() * 0.3)); w.extend((0..c * c).map(|_| rnd() * 0.3)); w.extend((0..c).map(|_| rnd() * 0.05)); }
    w.extend((0..c * hk).map(|_| rnd() * 0.5)); w.push(rnd() * 0.02); w.push(1.0);
    serde_json::json!({"version": "0.7.0", "architecture": "WaveNet", "sample_rate": 48000, "metadata": {"name": "Stock Lite"},
        "config": {"head_scale": 0.05, "layers": [{"input_size": 1, "condition_size": 1, "channels": c, "bottleneck": c, "kernel_sizes": ks, "dilations": ds,
            "activation": {"type": "LeakyReLU", "negative_slope": 0.01}, "head": {"kernel_size": hk, "out_channels": 1, "bias": true},
            "layer1x1": {"active": true, "groups": 1}, "head1x1": {"active": false}, "gating_mode": "none"}]}, "weights": w}).to_string()
}

#[test]
fn editor_against_firmware_logic() {
    let Some(bin) = pedal_bin() else { eprintln!("SKIPPED: firmware-tankg/build/host_pedal not built (run firmware-tankg/tools/host_pedal.sh)"); return; };
    let flash = std::env::temp_dir().join(format!("tankg_flash_{}.bin", std::process::id()));
    let _ = std::fs::remove_file(&flash);
    let fixtures = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
    let progress: ProgressFn = Arc::new(|_| {});
    {
        let s = power_on(&bin, &flash);
        // ---- state read exactly as the editor does on connect
        let pl = parse_plist(&s.send("PLIST", None).unwrap().lines);
        assert_eq!(pl.presets.iter().map(|p| p.name.as_str()).collect::<Vec<_>>(), ["Clean", "Crunch", "Lead", "Ambient"]);
        let irs = parse_library(&s.send("ILIST", None).unwrap().lines, "IRS", "ILIST end");
        assert_eq!(irs.entries.len(), 8); assert!(irs.entries.iter().all(|e| e.bytes == 0), "built-in cabinets are listed with size 0 (= free for an upload)");
        assert!(parse_library(&s.send("CLIST", None).unwrap().lines, "CAP", "CLIST end").entries.is_empty());
        // ---- chain editing: add / parameter / on-off / move / remove, preset switch, save
        assert!(one(&s, "PLOAD 0").starts_with("PLOAD"));
        let before = parse_fxs(&s.send("FXS", None).unwrap().lines);
        let snd0 = sound(&s);
        let g0 = parse_lvl(&one(&s, "LVL")).unwrap().chain_generation;
        let id = parse_fxa_id(&s.send("FXA P 0 pitchpro", None).unwrap().lines).expect("octaver added");
        assert_eq!(one(&s, &format!("FXP {id} 0 -12")), "FXP ok -12"); assert_eq!(one(&s, &format!("FXP {id} 2 80")), "FXP ok 80");
        let snd1 = sound(&s); assert_ne!(snd0, snd1, "adding the octaver changes the sound");
        assert!(one(&s, &format!("FXE {id} 0")).starts_with("FXE")); assert_eq!(sound(&s), snd0, "switched off = previous sound");
        assert!(one(&s, &format!("FXE {id} 1")).starts_with("FXE"));
        assert!(one(&s, &format!("FXM {id} Q 0")).starts_with("FXM"), "move");
        let moved = parse_fxs(&s.send("FXS", None).unwrap().lines); assert!(moved.blocks.iter().any(|b| b.id == id && b.bank == "Q"));
        assert!(one(&s, &format!("FXR {id}")).starts_with("FXR"), "remove");
        let after = parse_fxs(&s.send("FXS", None).unwrap().lines); assert_eq!(after.blocks.len(), before.blocks.len()); assert_eq!(sound(&s), snd0);
        assert_ne!(parse_lvl(&one(&s, "LVL")).unwrap().chain_generation, g0, "edits bump the generation the editor polls");
        assert!(one(&s, "PLOAD 2").starts_with("PLOAD")); assert_eq!(parse_lvl(&one(&s, "LVL")).unwrap().preset, Some(2)); assert_ne!(sound(&s), snd0, "preset 2 sounds different");
        // ---- capture upload with the editor's own transaction (AUDIO OFF, LDBEGIN, LDB..., LDEND, AUDIO A2L, CSAVE)
        one(&s, "PLOAD 0");
        let blob = std::fs::read(fixtures.join("a.ref.a2l")).unwrap();
        let job = CaptureJob { blob: blob.clone(), name: "Fixture Amp".into(), slot: 1, input_level_dbu: Some(12.0) };
        let p2 = progress.clone();
        let rep = s.exclusive(move |ex| upload_capture_wire(ex, &job, &p2)).unwrap().expect("capture upload");
        assert!(rep.detail.starts_with("CSAVE ok 1 'Fixture Amp'"), "{}", rep.detail);
        let caps = parse_library(&s.send("CLIST", None).unwrap().lines, "CAP", "CLIST end");
        assert_eq!(caps.entries.len(), 1); assert_eq!((caps.entries[0].slot, caps.entries[0].bytes as usize, caps.current), (1, blob.len(), Some(1)));
        let snd_cap = sound(&s); assert_ne!(snd_cap, snd0, "the uploaded capture sounds immediately");
        assert_eq!(parse_lvl(&one(&s, "LVL")).unwrap().preset, Some(0));
        assert!(one(&s, "MODEL").contains("layers=10"));
        // ---- a stock-size A2 Lite capture (23 layers, kernels 6/15: ~1730 MACs, 4.9 kB) is too heavy for this firmware: the editor asks CAPS,
        // compacts it into a small network and uploads that. The uncompacted blob is refused by the pedal (rc=-100) instead of glitching.
        let budget = pulsedeck_core::a2l::Budget::from_caps(&one(&s, "CAPS")).expect("open firmware reports its capture limits");
        assert!(budget.max_macs >= 438 && budget.max_macs < 1728);
        let big = stock_lite_nam();
        let plain = pulsedeck_core::a2l::convert_str(&big, &Default::default()).unwrap();
        assert_eq!(plain.stats.macs, 1728);
        let job = CaptureJob { blob: plain.blob, name: "Too Big".into(), slot: 3, input_level_dbu: None };
        let p4 = progress.clone();
        let err = s.exclusive(move |ex| upload_capture_wire(ex, &job, &p4)).unwrap().expect_err("the heavy blob must be refused");
        assert!(err.to_string().contains("memória"), "{err}");
        assert_eq!(sound(&s), snd_cap, "a refused upload leaves the playing capture alone");
        let opts = pulsedeck_core::a2l::ConvertOptions { budget: Some(budget), distill_steps: Some(300), ..Default::default() };
        let small = pulsedeck_core::a2l::convert_str(&big, &opts).unwrap();
        assert!(small.stats.compacted && small.stats.macs <= budget.max_macs && small.blob.len() <= budget.max_bytes);
        let job = CaptureJob { blob: small.blob.clone(), name: "Compacted".into(), slot: 3, input_level_dbu: None };
        let p5 = progress.clone();
        let rep = s.exclusive(move |ex| upload_capture_wire(ex, &job, &p5)).unwrap().expect("compacted capture upload");
        assert!(rep.detail.starts_with("CSAVE ok 3 'Compacted'"), "{}", rep.detail);
        let caps = parse_library(&s.send("CLIST", None).unwrap().lines, "CAP", "CLIST end");
        assert!(caps.entries.iter().any(|e| e.slot == 3 && e.bytes as usize == small.blob.len()) && caps.current == Some(3));
        assert_ne!(sound(&s), snd_cap, "the compacted capture is the one sounding now");
        assert_eq!(one(&s, "CLOAD 1"), "CLOAD ok 1 audio=2"); assert_eq!(sound(&s), snd_cap);
        assert_eq!(one(&s, "CDEL 3"), "CDEL ok 3");
        // ---- IR upload with the editor's own code (resample/normalise/IRBEGIN/IRB/IREND/ISAVE/FXIR)
        let ir_block = parse_fxs(&s.send("FXS", None).unwrap().lines).blocks.iter().find(|b| b.type_name == "ir").map(|b| b.id).expect("preset 0 has an ir block");
        let mut seed = 99u32; let mut lp = 0.0f64;
        let mono: Vec<f64> = (0..3000).map(|i| { seed = seed.wrapping_mul(1664525).wrapping_add(1013904223); let w = ((seed >> 8) as f64 / 8388608.0 - 1.0) * (-(i as f64) / 150.0).exp(); lp += 0.25 * (w - lp); lp }).collect();
        let job = IrJob { mono48: mono, name: "Test Cab".into(), slot: Some(6), assign_block: Some(ir_block) };
        let p3 = progress.clone();
        let rep = s.exclusive(move |ex| upload_ir_wire(ex, &job, &p3)).unwrap().expect("IR upload");
        assert_eq!(rep.slot, 6);
        let irs = parse_library(&s.send("ILIST", None).unwrap().lines, "IRS", "ILIST end");
        let e6 = irs.entries.iter().find(|e| e.slot == 6).unwrap(); assert_eq!(e6.name, "Test Cab"); assert!(e6.bytes > 0);
        let chain = parse_fxs(&s.send("FXS", None).unwrap().lines); assert_eq!(chain.blocks.iter().find(|b| b.id == ir_block).unwrap().params[2], 7, "FXIR pointed the block at slot 6");
        let snd_ir = sound(&s); assert_ne!(snd_ir, snd_cap, "the uploaded IR sounds immediately");
        // ---- save to flash
        assert!(one(&s, "PSAVE 0 Mine").starts_with("PSAVE ok"));
        assert_eq!(one(&s, "SAVE"), "SAVE ok slot=0");
        s.close();
    }
    {
        // ---- power cycle: same flash file, new process
        let s = power_on(&bin, &flash);
        let pl = parse_plist(&s.send("PLIST", None).unwrap().lines); assert_eq!(pl.presets[0].name, "Mine");
        let caps = parse_library(&s.send("CLIST", None).unwrap().lines, "CAP", "CLIST end"); assert_eq!(caps.entries.len(), 1); assert_eq!(caps.entries[0].name, "Fixture Amp");
        let irs = parse_library(&s.send("ILIST", None).unwrap().lines, "IRS", "ILIST end"); assert!(irs.entries.iter().any(|e| e.slot == 6 && e.name == "Test Cab" && e.bytes > 0));
        one(&s, "PLOAD 0");
        let with = sound(&s);
        assert_eq!(one(&s, "CDEL 1"), "CDEL ok 1"); let no_cap = sound(&s); assert_ne!(with, no_cap, "the saved preset really used the stored capture");
        assert_eq!(one(&s, "IDEL 6"), "IDEL ok 6"); assert_ne!(sound(&s), no_cap, "and the stored IR");
        s.close();
    }
    let _ = std::fs::remove_file(&flash);
}
