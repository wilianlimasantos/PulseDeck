//! Read/write checks against a REAL Tank-G running the open firmware (firmware-tankg). Not run by default:
//!   PULSEDECK_HW=1 cargo test -p pulsedeck-core --test hardware -- --ignored --nocapture
//! Everything is done in the pedal's RAM (PLOAD / FXA / FXP / FXE) and undone with PLOAD; nothing is saved to flash (no PSAVE).
use pulsedeck_core::link::{Connector, SerialConnector};
use pulsedeck_core::manager::handshake;
use pulsedeck_core::parse::{parse_fxs, parse_plist, parse_lvl};
use pulsedeck_core::protocol::Protocol;
use pulsedeck_core::session::EventSink;
use std::sync::Arc;

#[test]
#[ignore]
fn open_tank_g_firmware_read_write() {
    if std::env::var("PULSEDECK_HW").is_err() { eprintln!("set PULSEDECK_HW=1"); return; }
    let proto = Arc::new(Protocol::embedded());
    let conn = SerialConnector::new((*proto).clone());
    let cands = conn.candidates();
    assert!(!cands.is_empty(), "no serial port found: the open firmware shows up as 1209:0001 'Felucca'");
    let sink: EventSink = Arc::new(|_| {});
    let (s, pong) = cands.iter().find_map(|c| handshake(&proto, &conn, c, sink.clone()).ok().map(|r| r)).expect("no PONG from any candidate");
    println!("connected: {:?}", pong);
    // --- read: preset list + current chain
    let pl = parse_plist(&s.send("PLIST", None).unwrap().lines);
    assert!(pl.presets.len() >= 4, "presets: {:?}", pl.presets);
    s.send("PLOAD 1", None).unwrap();
    let before = parse_fxs(&s.send("FXS", None).unwrap().lines);
    assert!(!before.blocks.is_empty());
    println!("preset 1 blocks: {:?}", before.blocks.iter().map(|b| format!("{}{}:{}={}", b.bank, b.position, b.type_name, b.on)).collect::<Vec<_>>());
    let gen0 = parse_lvl(s.send("LVL", None).unwrap().terminal()).unwrap().chain_generation;
    // --- write: add a block that is not there (Delay) and a cabinet, change a parameter, switch on/off
    let had_delay = before.blocks.iter().any(|b| b.type_name == "delay");
    let (delay_id, added) = if let Some(b) = before.blocks.iter().find(|b| b.type_name == "delay") { (b.id, false) } else {
        let r = s.send("FXA Q 0 delay", None).unwrap();
        (pulsedeck_core::parse::parse_fxa_id(&r.lines).expect("FXA id"), true)
    };
    s.send(&format!("FXE {delay_id} 1"), None).unwrap();
    s.send(&format!("FXP {delay_id} 0 480"), None).unwrap();   // tiempo_ms
    s.send(&format!("FXP {delay_id} 2 41"), None).unwrap();    // mezcla_pct
    let ir_id = if let Some(b) = before.blocks.iter().find(|b| b.type_name == "ir") { b.id } else {
        pulsedeck_core::parse::parse_fxa_id(&s.send("FXA Q 0 ir", None).unwrap().lines).expect("FXA ir id")
    };
    s.send(&format!("FXE {ir_id} 1"), None).unwrap();
    s.send(&format!("FXP {ir_id} 2 3"), None).unwrap();        // ir_slot 3 = 4x12 Modern
    let after = parse_fxs(&s.send("FXS", None).unwrap().lines);
    let d = after.blocks.iter().find(|b| b.id == delay_id).expect("delay in chain");
    assert!(d.on && d.params[0] == 480 && d.params[2] == 41, "delay: {:?}", d);
    let ir = after.blocks.iter().find(|b| b.id == ir_id).expect("ir in chain");
    assert!(ir.on && ir.params[2] == 3, "ir: {:?}", ir);
    let gen1 = parse_lvl(s.send("LVL", None).unwrap().terminal()).unwrap().chain_generation;
    assert!(gen1 != gen0, "LVL c= (chain generation) must change after edits: {:?} -> {:?}", gen0, gen1);
    println!("added delay: {added} (had {had_delay}); chain now has {} blocks", after.blocks.len());
    // --- slot change from the editor, then back
    s.send("PLOAD 2", None).unwrap();
    let p2 = parse_lvl(s.send("LVL", None).unwrap().terminal()).unwrap().preset;
    assert_eq!(p2, Some(2));
    // --- undo: reloading the preset from flash discards the RAM edits (nothing was saved)
    s.send("PLOAD 1", None).unwrap();
    let back = parse_fxs(&s.send("FXS", None).unwrap().lines);
    assert_eq!(back.blocks.len(), before.blocks.len(), "RAM edits must be gone after PLOAD");
    for (a, b) in back.blocks.iter().zip(before.blocks.iter()) { assert_eq!((a.type_name.as_str(), a.on, &a.params), (b.type_name.as_str(), b.on, &b.params)); }
    // --- adding a block that is really absent: preset 0 (Clean) has no delay
    s.send("PLOAD 0", None).unwrap();
    let c0 = parse_fxs(&s.send("FXS", None).unwrap().lines);
    if !c0.blocks.iter().any(|b| b.type_name == "delay") {
        let id = pulsedeck_core::parse::parse_fxa_id(&s.send("FXA Q 0 delay", None).unwrap().lines).expect("FXA id");
        s.send(&format!("FXP {id} 0 300"), None).unwrap();
        let c1 = parse_fxs(&s.send("FXS", None).unwrap().lines);
        assert!(c1.blocks.iter().any(|b| b.id == id && b.type_name == "delay" && b.params[0] == 300), "delay added to Clean");
        assert_eq!(c1.blocks.len(), c0.blocks.len() + 1);
        s.send("PLOAD 0", None).unwrap();   // discard
        assert_eq!(parse_fxs(&s.send("FXS", None).unwrap().lines).blocks.len(), c0.blocks.len());
        println!("added a delay to preset 0 and discarded it");
    }
    s.close();
}
