//! Global pedal settings: output EQ (GEQ), input high-pass (HPF), footswitch actions / layout
//! (FSA, FSMODE) and LED settings (LEDCFG, FSLED). Everything is applied live; `GSAVE` writes it to
//! flash. The reply shapes and the quick setups come from `protocol.json`
//! (`commands.global_settings`).

use crate::parse::kv_map;
use crate::protocol::Protocol;
use crate::session::{CmdError, Wire};
use serde::{Deserialize, Serialize};
use serde_json::Value;

pub const GEQ_BANDS: usize = 5;

/// One footswitch gesture: `[type, a, b]` as in `FSA <fs> <gesture> <type> <a> <b>`.
pub type FsAction = [u32; 3];

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct LedCfg {
    pub bright: u32,
    pub tuner: String,
    pub tap: u32,
    pub meter: String,
    pub hold: u32,
    pub anim: u32,
    pub idle: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct FsLed {
    pub custom: bool,
    pub on: String,
    pub off: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct GlobalSettings {
    /// `None`: the firmware did not answer the query
    pub geq: Option<Vec<i32>>,
    pub hpf: Option<u32>,
    pub layout: Option<String>,
    /// 4 footswitches x 3 gestures
    pub fsa: Option<Vec<Vec<FsAction>>>,
    /// key of the quick setup that matches the current layout / actions
    pub quick_active: Option<String>,
    pub led: Option<LedCfg>,
    pub fsled: Option<Vec<FsLed>>,
    /// ranges and quick setups (`host_ui` of protocol.json)
    pub schema: Value,
}

// ------------------------------------------------------------------------------ parsing

/// `GEQ a b c d e` (one line) or one `GEQ <band> <dB>` line per band.
pub fn parse_geq(lines: &[String]) -> Option<Vec<i32>> {
    let mut vals = vec![0i32; GEQ_BANDS];
    let mut seen = 0;
    for l in lines {
        let nums: Vec<i32> = l
            .strip_prefix("GEQ")?
            .split_whitespace()
            .filter_map(|t| t.parse().ok())
            .collect();
        if nums.len() >= GEQ_BANDS {
            return Some(nums[..GEQ_BANDS].to_vec());
        }
        if nums.len() == 2 && (0..GEQ_BANDS as i32).contains(&nums[0]) {
            vals[nums[0] as usize] = nums[1];
            seen += 1;
        }
    }
    (seen == GEQ_BANDS).then_some(vals)
}

pub fn parse_hpf(lines: &[String]) -> Option<u32> {
    lines
        .iter()
        .find_map(|l| kv_map(l).get("hz").and_then(|v| v.parse().ok()))
}

pub fn parse_layout(lines: &[String]) -> Option<String> {
    lines.iter().find_map(|l| {
        l.starts_with("FSMODE")
            .then(|| kv_map(l).get("layout").map(|v| v.to_string()))
            .flatten()
    })
}

pub fn parse_fsa(lines: &[String]) -> Option<Vec<Vec<FsAction>>> {
    let mut t = vec![vec![[0u32; 3]; 3]; 4];
    let mut seen = 0;
    for l in lines {
        let mut tok = l.split_whitespace();
        if tok.next() != Some("FSA") {
            continue;
        }
        let nums: Vec<u32> = tok.map_while(|x| x.parse().ok()).collect();
        if nums.len() >= 3 && nums[0] < 4 && nums[1] < 3 {
            t[nums[0] as usize][nums[1] as usize] = [
                nums[2],
                nums.get(3).copied().unwrap_or(0),
                nums.get(4).copied().unwrap_or(255),
            ];
            seen += 1;
        }
    }
    (seen >= 12).then_some(t)
}

pub fn parse_ledcfg(lines: &[String]) -> Option<LedCfg> {
    let l = lines.iter().find(|l| l.starts_with("LEDCFG"))?;
    let kv = kv_map(l);
    let n = |k: &str| kv.get(k).and_then(|v| v.parse::<u32>().ok());
    let s = |k: &str| kv.get(k).map(|v| v.to_string());
    Some(LedCfg {
        bright: n("bright")?,
        tuner: s("tuner")?,
        tap: n("tap")?,
        meter: s("meter")?,
        hold: n("hold")?,
        anim: n("anim")?,
        idle: n("idle")?,
    })
}

fn is_hex6(s: &str) -> bool {
    s.len() == 6 && s.bytes().all(|b| b.is_ascii_hexdigit())
}

pub fn parse_fsled(lines: &[String]) -> Option<Vec<FsLed>> {
    let mut out: Vec<Option<FsLed>> = vec![None; 4];
    for l in lines {
        let mut tok = l.split_whitespace();
        if tok.next() != Some("FSLED") {
            continue;
        }
        let (Some(Ok(f)), Some(mode)) = (tok.next().map(str::parse::<usize>), tok.next()) else {
            continue;
        };
        let kv = kv_map(l);
        let (Some(on), Some(off)) = (kv.get("on"), kv.get("off")) else {
            continue;
        };
        if f < 4 && is_hex6(on) && is_hex6(off) {
            out[f] = Some(FsLed {
                custom: mode == "custom",
                on: on.to_ascii_lowercase(),
                off: off.to_ascii_lowercase(),
            });
        }
    }
    out.into_iter().collect()
}

// ----------------------------------------------------------------------------- quick setups

#[derive(Debug, Clone, PartialEq)]
pub struct QuickSetup {
    pub key: String,
    pub layout: String,
    /// `None` for the factory layout (no FSA map)
    pub actions: Option<Vec<Vec<FsAction>>>,
}

pub fn quick_setups(protocol: &Protocol) -> Vec<QuickSetup> {
    let ui = protocol.globals_ui();
    ui["footswitch_quick_setups"]
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|q| {
                    let actions = if q["actions"].is_null() {
                        None
                    } else {
                        let rows = q["actions"].as_array()?;
                        Some(
                            rows.iter()
                                .map(|r| {
                                    r.as_array()
                                        .map(|g| {
                                            g.iter()
                                                .map(|x| {
                                                    let n = |i: usize| {
                                                        x[i].as_u64().unwrap_or(0) as u32
                                                    };
                                                    [n(0), n(1), n(2)]
                                                })
                                                .collect()
                                        })
                                        .unwrap_or_default()
                                })
                                .collect(),
                        )
                    };
                    Some(QuickSetup {
                        key: q["key"].as_str()?.into(),
                        layout: q["layout"].as_str()?.into(),
                        actions,
                    })
                })
                .collect()
        })
        .unwrap_or_default()
}

/// Same action? "Toggle block" (type 8) points to a block by identity, so its arguments are
/// ignored; other actions compare type and first argument (second only for presets, type 2).
fn same_action(a: &FsAction, b: &FsAction) -> bool {
    a[0] == b[0] && (a[0] == 0 || a[0] == 8 || (a[1] == b[1] && (a[0] != 2 || a[2] == b[2])))
}

fn same_table(a: &[Vec<FsAction>], b: &[Vec<FsAction>]) -> bool {
    a.len() == b.len()
        && a.iter()
            .zip(b)
            .all(|(x, y)| x.len() == y.len() && x.iter().zip(y).all(|(p, q)| same_action(p, q)))
}

/// Which quick setup is on the pedal now (`factory` wins when the layout says so).
pub fn active_quick(
    setups: &[QuickSetup],
    layout: Option<&str>,
    fsa: Option<&Vec<Vec<FsAction>>>,
) -> Option<String> {
    if layout == Some("factory") {
        return setups
            .iter()
            .find(|q| q.actions.is_none())
            .map(|q| q.key.clone());
    }
    let fsa = fsa?;
    setups
        .iter()
        .find(|q| q.actions.as_ref().is_some_and(|a| same_table(a, fsa)))
        .map(|q| q.key.clone())
}

// ----------------------------------------------------------------------------------- I/O

fn query(w: &mut dyn Wire, cmd: &str) -> Option<Vec<String>> {
    w.send(cmd).ok().map(|r| r.lines)
}

pub fn read_all(w: &mut dyn Wire) -> GlobalSettings {
    let protocol = w.protocol().clone();
    let geq = query(w, "GEQ").and_then(|l| parse_geq(&l));
    let hpf = query(w, "HPF").and_then(|l| parse_hpf(&l));
    let layout = query(w, "FSMODE").and_then(|l| parse_layout(&l));
    let fsa = query(w, "FSA").and_then(|l| parse_fsa(&l));
    let led = query(w, "LEDCFG").and_then(|l| parse_ledcfg(&l));
    let fsled = query(w, "FSLED").and_then(|l| parse_fsled(&l));
    let quick_active = active_quick(&quick_setups(&protocol), layout.as_deref(), fsa.as_ref());
    GlobalSettings {
        geq,
        hpf,
        layout,
        fsa,
        quick_active,
        led,
        fsled,
        schema: protocol.globals_ui(),
    }
}

fn clamp_range(protocol: &Protocol, lo: &str, hi: &str, d: (i64, i64)) -> (i64, i64) {
    let g = &protocol.globals_ui()["geq"];
    (g[lo].as_i64().unwrap_or(d.0), g[hi].as_i64().unwrap_or(d.1))
}

/// Set one GEQ band (clamped to the documented range). Returns the value the host sent.
pub fn set_geq(w: &mut dyn Wire, band: usize, db: i32) -> Result<i32, CmdError> {
    let (lo, hi) = clamp_range(w.protocol(), "min_db", "max_db", (-12, 12));
    let v = i64::from(db).clamp(lo, hi) as i32;
    if band >= GEQ_BANDS {
        return Err(CmdError::Device {
            cmd: "GEQ".into(),
            message: format!("banda {band} fora de 0..{}", GEQ_BANDS - 1),
        });
    }
    w.send(&format!("GEQ {band} {v}"))?;
    Ok(v)
}

pub fn set_geq_all(w: &mut dyn Wire, values: &[i32]) -> Result<Vec<i32>, CmdError> {
    values
        .iter()
        .take(GEQ_BANDS)
        .enumerate()
        .map(|(b, v)| set_geq(w, b, *v))
        .collect()
}

pub fn set_hpf(w: &mut dyn Wire, hz: u32) -> Result<(), CmdError> {
    let h = &w.protocol().globals_ui()["hpf"];
    let (min, max, step) = (
        h["min_hz"].as_u64().unwrap_or(20) as u32,
        h["max_hz"].as_u64().unwrap_or(120) as u32,
        h["step_hz"].as_u64().unwrap_or(5).max(1) as u32,
    );
    let v = if hz == 0 {
        0
    } else {
        (hz.clamp(min, max) - min) / step * step + min
    };
    w.send(&format!("HPF {v}")).map(|_| ())
}

/// Apply a footswitch quick setup: the 12 FSA lines first, then the layout (the pedal wants the
/// actions before `FSMODE layout=`). Factory only switches the layout and keeps the action map.
pub fn apply_quick_setup(w: &mut dyn Wire, key: &str) -> Result<(), CmdError> {
    let setups = quick_setups(w.protocol());
    let q = setups
        .iter()
        .find(|q| q.key == key)
        .ok_or_else(|| CmdError::Device {
            cmd: "FSMODE".into(),
            message: format!("configuração desconhecida: {key}"),
        })?;
    if let Some(table) = &q.actions {
        for (f, row) in table.iter().enumerate() {
            for (g, a) in row.iter().enumerate() {
                w.send(&format!("FSA {f} {g} {} {} {}", a[0], a[1], a[2]))?;
            }
        }
    }
    w.send(&format!("FSMODE layout={}", q.layout))?;
    Ok(())
}

const LED_KEYS: [&str; 7] = ["bright", "tuner", "tap", "meter", "hold", "anim", "idle"];

/// `LEDCFG <key>=<value>`; the value must be one of the documented ones.
pub fn set_ledcfg(w: &mut dyn Wire, key: &str, value: &str) -> Result<(), CmdError> {
    let bad = |m: String| CmdError::Device {
        cmd: "LEDCFG".into(),
        message: m,
    };
    if !LED_KEYS.contains(&key) {
        return Err(bad(format!("opção desconhecida: {key}")));
    }
    let spec = w.protocol().globals_ui()["led"][key].clone();
    let ok = if key == "bright" {
        let n: i64 = value.parse().map_err(|_| bad("brilho inválido".into()))?;
        (spec["min"].as_i64().unwrap_or(0)..=spec["max"].as_i64().unwrap_or(100)).contains(&n)
    } else {
        spec.as_array().is_some_and(|a| {
            a.iter().any(|x| {
                x.as_str()
                    .map(String::from)
                    .or_else(|| x.as_u64().map(|n| n.to_string()))
                    .as_deref()
                    == Some(value)
            })
        })
    };
    if !ok {
        return Err(bad(format!("valor inválido para {key}: {value}")));
    }
    w.send(&format!("LEDCFG {key}={value}")).map(|_| ())
}

/// `FSLED <fs> auto` or `FSLED <fs> <on> <off>`.
pub fn set_fsled(w: &mut dyn Wire, fs: usize, led: &FsLed) -> Result<(), CmdError> {
    let bad = |m: &str| CmdError::Device {
        cmd: "FSLED".into(),
        message: m.into(),
    };
    if fs >= 4 {
        return Err(bad("footswitch fora de 0..3"));
    }
    if !led.custom {
        return w.send(&format!("FSLED {fs} auto")).map(|_| ());
    }
    let (on, off) = (
        led.on.trim_start_matches('#'),
        led.off.trim_start_matches('#'),
    );
    if !is_hex6(on) || !is_hex6(off) {
        return Err(bad("cor inválida (use rrggbb)"));
    }
    w.send(&format!(
        "FSLED {fs} {} {}",
        on.to_ascii_lowercase(),
        off.to_ascii_lowercase()
    ))
    .map(|_| ())
}

pub fn save(w: &mut dyn Wire) -> Result<(), CmdError> {
    w.send("GSAVE").map(|_| ())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn l(s: &[&str]) -> Vec<String> {
        s.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn geq_both_shapes() {
        assert_eq!(
            parse_geq(&l(&["GEQ 0 -2 3 4 12"])),
            Some(vec![0, -2, 3, 4, 12])
        );
        assert_eq!(
            parse_geq(&l(&[
                "GEQ 0 1", "GEQ 1 2", "GEQ 2 3", "GEQ 3 -4", "GEQ 4 5"
            ])),
            Some(vec![1, 2, 3, -4, 5])
        );
        assert_eq!(parse_geq(&l(&["GEQ 1 2"])), None);
        assert_eq!(parse_geq(&l(&["ERR GEQ x"])), None);
    }

    #[test]
    fn fsa_table_and_extra_tokens() {
        let mut lines = Vec::new();
        for f in 0..4 {
            for g in 0..3 {
                lines.push(format!("FSA {f} {g} 1 {f} 255 id=3,0"));
            }
        }
        lines.push("FSA end ok".into());
        let t = parse_fsa(&lines).unwrap();
        assert_eq!(t[2][1], [1, 2, 255]);
        assert!(parse_fsa(&lines[..5]).is_none());
    }

    #[test]
    fn led_lines() {
        let c = parse_ledcfg(&l(&[
            "LEDCFG bright=80 tuner=needle tap=1 meter=edit hold=1 anim=0 idle=30",
        ]))
        .unwrap();
        assert_eq!(
            (c.bright, c.tuner.as_str(), c.idle, c.anim),
            (80, "needle", 30, 0)
        );
        assert!(parse_ledcfg(&l(&["LEDCFG bright=1"])).is_none());
        let f = parse_fsled(&l(&[
            "FSLED 0 auto on=28BEFF off=010203",
            "FSLED 1 custom on=28ff5a off=000000",
            "FSLED 2 auto on=ffaa1e off=000000",
            "FSLED 3 auto on=e63cff off=000000",
            "FSLED end",
        ]))
        .unwrap();
        assert!(!f[0].custom && f[1].custom);
        assert_eq!(f[0].on, "28beff");
        assert!(parse_fsled(&l(&["FSLED 0 auto on=zzzzzz off=000000"])).is_none());
    }

    #[test]
    fn quick_setups_are_recognised() {
        let p = Protocol::embedded();
        let setups = quick_setups(&p);
        assert_eq!(
            setups.iter().map(|q| q.key.as_str()).collect::<Vec<_>>(),
            ["factory", "scenes", "presets", "stompbox"]
        );
        for q in setups.iter().filter(|q| q.actions.is_some()) {
            let t = q.actions.clone().unwrap();
            assert_eq!(t.len(), 4);
            assert!(t.iter().all(|r| r.len() == 3));
            assert_eq!(
                active_quick(&setups, Some(&q.layout), Some(&t)).as_deref(),
                Some(q.key.as_str())
            );
        }
        // stompbox ignores which block each switch points at
        let mut st = setups[3].actions.clone().unwrap();
        st[0][0] = [8, 5, 1];
        assert_eq!(
            active_quick(&setups, Some("custom"), Some(&st)).as_deref(),
            Some("stompbox")
        );
        assert_eq!(
            active_quick(&setups, Some("factory"), None).as_deref(),
            Some("factory")
        );
        st[1][1] = [4, 0, 255];
        assert_eq!(active_quick(&setups, Some("custom"), Some(&st)), None);
    }

    mod on_sim {
        use super::super::*;
        use crate::session::{EventSink, Session, SessionWire};
        use crate::sim::{SimLink, SimState};
        use std::sync::{Arc, Mutex};

        fn wire() -> SessionWire {
            let p = Arc::new(Protocol::embedded());
            let st = Arc::new(Mutex::new(SimState::new(&p)));
            let sink: EventSink = Arc::new(|_| {});
            let s = Arc::new(Session::start(Box::new(SimLink::new(st)), p.clone(), sink));
            s.set_max_line(190);
            SessionWire {
                session: s,
                protocol: p,
            }
        }

        #[test]
        fn read_edit_and_read_back() {
            let mut w = wire();
            let g = read_all(&mut w);
            assert_eq!(g.geq, Some(vec![0; 5]));
            assert_eq!(g.layout.as_deref(), Some("factory"));
            assert_eq!(g.quick_active.as_deref(), Some("factory"));
            assert_eq!(g.led.as_ref().unwrap().bright, 100);
            assert_eq!(g.fsled.as_ref().unwrap().len(), 4);

            assert_eq!(set_geq(&mut w, 2, 99).unwrap(), 12, "clamped to +12 dB");
            set_geq_all(&mut w, &[1, -2, 3, -4, 5]).unwrap();
            assert!(set_geq(&mut w, 5, 0).is_err());
            set_hpf(&mut w, 57).unwrap();
            set_ledcfg(&mut w, "bright", "55").unwrap();
            set_ledcfg(&mut w, "tuner", "strobe").unwrap();
            assert!(set_ledcfg(&mut w, "tuner", "disco").is_err());
            assert!(set_ledcfg(&mut w, "bright", "101").is_err());
            assert!(set_ledcfg(&mut w, "nope", "1").is_err());
            let red = FsLed {
                custom: true,
                on: "#FF0000".into(),
                off: "110000".into(),
            };
            set_fsled(&mut w, 1, &red).unwrap();
            assert!(set_fsled(&mut w, 4, &red).is_err());
            apply_quick_setup(&mut w, "stompbox").unwrap();

            let g = read_all(&mut w);
            assert_eq!(g.geq, Some(vec![1, -2, 3, -4, 5]));
            assert_eq!(g.hpf, Some(55), "snapped to the 5 Hz grid");
            let led = g.led.unwrap();
            assert_eq!((led.bright, led.tuner.as_str()), (55, "strobe"));
            let f = g.fsled.unwrap();
            assert!(f[1].custom && f[1].on == "ff0000" && f[1].off == "110000");
            assert_eq!(g.layout.as_deref(), Some("custom"));
            assert_eq!(g.quick_active.as_deref(), Some("stompbox"));

            apply_quick_setup(&mut w, "factory").unwrap();
            assert_eq!(read_all(&mut w).quick_active.as_deref(), Some("factory"));
            assert!(apply_quick_setup(&mut w, "nope").is_err());
            save(&mut w).unwrap();
        }
    }
}
