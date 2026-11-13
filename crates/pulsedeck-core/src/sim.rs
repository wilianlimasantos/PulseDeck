//! In-process pedal simulator. Used by the tests and by the app's demo mode
//! (`PULSEDECK_SIM=1`) so the whole stack can be exercised without hardware.
//!
//! It speaks the same text protocol (as documented in `protocol.json`) but is not the firmware:
//! behaviours not described in the protocol are simplified.

use crate::link::{Connector, Link, PortCandidate};
use crate::protocol::{EffectType, Protocol};
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::io;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

#[derive(Debug, Clone)]
pub struct SimBlock {
    pub id: u32,
    pub bank: char,
    pub type_name: String,
    pub on: bool,
    pub params: Vec<i32>,
    pub lane: u8,
}

#[derive(Debug, Clone)]
pub struct SceneState {
    /// (on, params) aligned with `SimState::ordered_ids()` (P blocks then Q blocks).
    pub blocks: Vec<(bool, Vec<i32>)>,
    pub amp_on: bool,
    pub ampeq: [i32; 6],
    pub gain: (i32, i32),
}

#[derive(Debug, Clone)]
pub struct StoredPreset {
    pub name: String,
    pub color: Option<String>,
    pub blocks: Vec<SimBlock>,
    pub amp_present: bool,
    pub amp_on: bool,
    pub ampeq: [i32; 6],
    pub gain: (i32, i32),
    pub capture: i32,
    pub scenes: [Option<SceneState>; 4],
    pub scene_names: [String; 4],
}

pub struct SimState {
    /// Flash presets by slot.
    pub stored: BTreeMap<u8, StoredPreset>,
    pub gain: (i32, i32),
    pub capture_cur: i32,
    pub preset_color: Option<String>,
    /// Raw data of stored captures ('C') and IRs ('I').
    pub blobs: HashMap<(char, u8), Vec<u8>>,
    pub ir_gain: HashMap<u8, i32>,
    pub last_ir_gain: i32,
    pub hpf: i32,
    pub ledcfg: BTreeMap<String, String>,
    pub fsa: BTreeMap<String, String>,
    pub fsmode: String,
    /// per footswitch: (custom, on, off)
    pub fsled: Vec<(bool, String, String)>,
    pub geq: [i32; 6],
    /// tuner mode state (`TUNER`) and a poll counter that drives the simulated pitch
    pub tuner_on: bool,
    pub tuner_mute: bool,
    pub tuner_ref: u32,
    pub tune_tick: u32,
    pub gsaved: bool,
    pub effects: Vec<EffectType>,
    pub blocks: Vec<SimBlock>, // order in vec = chain order within bank
    pub amp_present: bool,
    pub amp_on: bool,
    pub ampeq: [i32; 6],
    pub next_id: u32,
    pub generation: u32,
    pub preset: i32,
    pub scene: i32,
    pub cpu_budget: i64,
    pub captures: Vec<Option<(String, usize)>>,
    pub irs: Vec<Option<(String, usize)>>,
    pub audio: i32,
    pub stage: Option<Stage>,
    /// Every command line received, in order.
    pub log: Vec<String>,
    /// Set when a command arrives while the previous reply was not fully consumed.
    pub overlap_violations: usize,
    pub inject_async_mid_reply: bool,
    pub max_line: usize,
    pub fail_next: Option<String>,
    /// Fail the next command whose line starts with this prefix: (prefix, message).
    pub fail_on: Option<(String, String)>,
    pub last_blob: Vec<u8>,
    pub last_ir: Vec<u8>,
    pub ir_mem_limit: usize,
}

pub struct Stage {
    pub kind: char, // 'C' capture, 'I' IR
    pub expected: usize,
    pub data: Vec<u8>,
}

impl SimState {
    pub fn new(protocol: &Protocol) -> Self {
        let mut s = SimState {
            effects: protocol.reference_effects(),
            blocks: vec![],
            amp_present: true,
            amp_on: true,
            ampeq: [0; 6],
            next_id: 1,
            generation: 1,
            preset: 0,
            scene: 0,
            stored: BTreeMap::new(),
            gain: (0, 0),
            capture_cur: 0,
            preset_color: None,
            blobs: HashMap::new(),
            ir_gain: HashMap::new(),
            last_ir_gain: 0,
            hpf: 80,
            ledcfg: BTreeMap::from([
                ("bright".into(), "100".into()),
                ("tuner".into(), "needle".into()),
                ("tap".into(), "1".into()),
                ("meter".into(), "edit".into()),
                ("hold".into(), "1".into()),
                ("anim".into(), "1".into()),
                ("idle".into(), "0".into()),
            ]),
            fsa: (0..4)
                .flat_map(|f| {
                    [
                        (format!("{f} 0"), format!("1 {f} 255")),
                        (format!("{f} 1"), format!("{} 0 255", [4, 9, 7, 3][f])),
                        (format!("{f} 2"), "0 0 255".to_string()),
                    ]
                })
                .collect(),
            fsmode: "factory".into(),
            fsled: ["28beff", "28ff5a", "ffaa1e", "e63cff"]
                .iter()
                .map(|c| (false, c.to_string(), "000000".to_string()))
                .collect(),
            geq: [0; 6],
            tuner_on: false,
            tuner_mute: false,
            tuner_ref: 440,
            tune_tick: 0,
            gsaved: false,
            cpu_budget: 1000,
            captures: vec![None; 15],
            irs: vec![None; 8],
            audio: 2,
            stage: None,
            log: vec![],
            overlap_violations: 0,
            inject_async_mid_reply: false,
            max_line: 190,
            fail_next: None,
            fail_on: None,
            last_blob: vec![],
            last_ir: vec![],
            ir_mem_limit: usize::MAX,
        };
        s.captures[0] = Some(("Demo Plexi".into(), 2200));
        let mut demo_blob = b"A2L1".to_vec();
        demo_blob.resize(2200, 7);
        s.blobs.insert(('C', 0), demo_blob);
        s.add_block('P', 99, "boost");
        s.add_block('P', 99, "overdrive");
        s.add_block('Q', 99, "ir");
        s.add_block('Q', 99, "delay");
        s.add_block('Q', 99, "reverb2");
        s.store_current(0, "Clean Demo");
        s.preset = 0;
        // second demo preset: same chain, hotter drive
        if let Some(b) = s.blocks.iter_mut().find(|b| b.type_name == "overdrive") {
            b.params[0] = 30;
        }
        s.store_current(1, "Crunch Demo");
        s.apply_stored(0);
        s
    }

    pub fn ordered_ids(&self) -> Vec<u32> {
        ['P', 'Q']
            .iter()
            .flat_map(|bk| {
                self.blocks
                    .iter()
                    .filter(move |b| b.bank == *bk)
                    .map(|b| b.id)
            })
            .collect()
    }

    pub fn scene_state(&self) -> SceneState {
        let ids = self.ordered_ids();
        SceneState {
            blocks: ids
                .iter()
                .map(|id| {
                    let b = self.blocks.iter().find(|b| b.id == *id).unwrap();
                    (b.on, b.params.clone())
                })
                .collect(),
            amp_on: self.amp_on,
            ampeq: self.ampeq,
            gain: self.gain,
        }
    }

    fn apply_scene(&mut self, sc: &SceneState) {
        for (id, (on, params)) in self.ordered_ids().into_iter().zip(sc.blocks.iter()) {
            if let Some(b) = self.blocks.iter_mut().find(|b| b.id == id) {
                b.on = *on;
                b.params = params.clone();
            }
        }
        self.amp_on = sc.amp_on;
        self.ampeq = sc.ampeq;
        self.gain = sc.gain;
        self.generation += 1;
    }

    /// Write the sounding chain to flash slot `slot` (keeps scenes already stored there).
    pub fn store_current(&mut self, slot: u8, name: &str) {
        let old = self.stored.get(&slot).cloned();
        let sp = StoredPreset {
            name: name.to_string(),
            color: self.preset_color.clone(),
            blocks: self.blocks.clone(),
            amp_present: self.amp_present,
            amp_on: self.amp_on,
            ampeq: self.ampeq,
            gain: self.gain,
            capture: self.capture_cur,
            scenes: if self.preset == slot as i32 {
                old.as_ref()
                    .map(|o| o.scenes.clone())
                    .unwrap_or([None, None, None, None])
            } else {
                [None, None, None, None]
            },
            scene_names: old
                .map(|o| o.scene_names)
                .unwrap_or_else(|| [1, 2, 3, 4].map(|i| format!("Scene {i}"))),
        };
        self.stored.insert(slot, sp);
        self.preset = slot as i32;
    }

    /// Load a flash preset into the sounding chain (blocks get fresh instance ids).
    pub fn apply_stored(&mut self, slot: u8) -> bool {
        let Some(sp) = self.stored.get(&slot).cloned() else {
            return false;
        };
        self.blocks = sp
            .blocks
            .into_iter()
            .map(|mut b| {
                b.id = self.next_id;
                self.next_id += 1;
                b
            })
            .collect();
        self.amp_present = sp.amp_present;
        self.amp_on = sp.amp_on;
        self.ampeq = sp.ampeq;
        self.gain = sp.gain;
        self.capture_cur = sp.capture;
        self.preset_color = sp.color;
        self.preset = slot as i32;
        self.scene = 0;
        self.generation += 1;
        true
    }

    fn blob_info(&self, kind: char, slot: usize) -> Option<(String, Vec<u8>)> {
        let data = self.blobs.get(&(kind, slot as u8))?.clone();
        let name = if kind == 'C' {
            self.captures.get(slot)?.as_ref()?.0.clone()
        } else {
            self.irs.get(slot)?.as_ref()?.0.clone()
        };
        Some((name, data))
    }

    fn effect(&self, name: &str) -> Option<&EffectType> {
        self.effects.iter().find(|e| e.name == name)
    }

    fn cost(&self) -> i64 {
        let fx: i64 = self
            .blocks
            .iter()
            .filter(|b| b.on)
            .map(|b| 40 + 5 * b.params.len() as i64)
            .sum();
        fx + if self.amp_on && self.amp_present {
            380
        } else {
            0
        }
    }

    fn add_block(&mut self, bank: char, pos: usize, name: &str) -> Option<u32> {
        let defaults: Vec<i32> = self
            .effect(name)?
            .parameters
            .iter()
            .map(|p| p.default)
            .collect();
        let id = self.next_id;
        self.next_id += 1;
        let blk = SimBlock {
            id,
            bank,
            type_name: name.into(),
            on: true,
            params: defaults,
            lane: 0,
        };
        let in_bank: Vec<usize> = self
            .blocks
            .iter()
            .enumerate()
            .filter(|(_, b)| b.bank == bank)
            .map(|(i, _)| i)
            .collect();
        let at = if pos >= in_bank.len() {
            // after the last block of this bank (P before Q ordering is kept by sorting at read time)
            in_bank.last().map(|i| i + 1).unwrap_or(self.blocks.len())
        } else {
            in_bank[pos]
        };
        self.blocks.insert(at, blk);
        self.generation += 1;
        Some(id)
    }

    fn bank_blocks(&self, bank: char) -> Vec<&SimBlock> {
        self.blocks.iter().filter(|b| b.bank == bank).collect()
    }

    fn pong(&self) -> String {
        "PONG tankg_fw M5.26 rel amx ble v1 Nov 12 2026 10:00:00".into()
    }

    /// Handle one command line, return the reply lines.
    pub fn handle(&mut self, line: &str) -> Vec<String> {
        self.log.push(line.to_string());
        let mut out: Vec<String> = Vec::new();
        let t: Vec<&str> = line.split_whitespace().collect();
        let cmd = t.first().copied().unwrap_or("");
        if let Some(msg) = self.fail_next.take() {
            return vec![format!("ERR {cmd} {msg}")];
        }
        if let Some((p, m)) = self.fail_on.clone() {
            if line.starts_with(&p) {
                self.fail_on = None;
                return vec![format!("ERR {cmd} {m}")];
            }
        }
        if line.len() > self.max_line {
            return vec![format!("ERR {cmd} line too long")];
        }
        let arg = |i: usize| t.get(i).copied().unwrap_or("");
        let argn = |i: usize| t.get(i).and_then(|s| s.parse::<i64>().ok());
        match cmd {
            "PING" => out.push(self.pong()),
            "FXT" => {
                for e in &self.effects {
                    out.push(format!("FXT {} {} cost=40 cat={}", e.id, e.name, e.category));
                    for p in &e.parameters {
                        out.push(format!("FXT {} {} {}..{} ({})", p.index, p.name, p.min, p.max, p.default));
                    }
                }
                out.push("FXT end".into());
            }
            "FXS" => {
                for bank in ['P', 'Q'] {
                    for (pos, b) in self.bank_blocks(bank).iter().enumerate() {
                        let p = b.params.iter().map(|v| v.to_string()).collect::<Vec<_>>().join(",");
                        out.push(format!(
                            "FX {}{} id={} {} on={} p={} ticks=45 l={}",
                            bank, pos, b.id, b.type_name, b.on as u8, p, b.lane
                        ));
                        if self.inject_async_mid_reply && pos == 0 {
                            out.push("@L -200 -180 1/1000 0/2 0 1".into());
                        }
                    }
                }
                out.push(format!("FXS amp={} on={}", self.amp_present as u8, self.amp_on as u8));
                let used = self.cost();
                out.push(format!("FXS cpu={}/{} ticks ({}%) mem_fx=1000", used, self.cpu_budget, used * 100 / self.cpu_budget));
                out.push("FXS peak=500 ticks skipped=0".into());
            }
            "FXA" => {
                let bank = arg(1).chars().next().unwrap_or('P');
                let pos: usize = arg(2).parse().unwrap_or(99);
                let name = arg(3);
                if self.effect(name).is_none() || (bank != 'P' && bank != 'Q') {
                    return vec!["ERR FXA bad argument".into()];
                }
                if self.bank_blocks(bank).len() >= 10 {
                    return vec!["ERR FXA bank full".into()];
                }
                if self.cost() + 45 > self.cpu_budget {
                    return vec!["ERR FXA not enough CPU".into()];
                }
                let id = self.add_block(bank, pos, name).unwrap();
                out.push(format!("FXA id={id} {name}"));
                out.push(format!("FX cpu={}/{} ticks ({}%) mem_fx=1000", self.cost(), self.cpu_budget, self.cost() * 100 / self.cpu_budget));
            }
            "FXR" => {
                let id = argn(1).unwrap_or(-1) as u32;
                let before = self.blocks.len();
                self.blocks.retain(|b| b.id != id);
                if self.blocks.len() == before {
                    return vec!["ERR FXR bad argument".into()];
                }
                self.generation += 1;
                out.push(format!("FXR ok {id}"));
            }
            "FXM" => {
                let id = argn(1).unwrap_or(-1) as u32;
                let bank = arg(2).chars().next().unwrap_or('P');
                let pos: usize = arg(3).parse().unwrap_or(0);
                let Some(i) = self.blocks.iter().position(|b| b.id == id) else {
                    return vec!["ERR FXM bad argument".into()];
                };
                let mut b = self.blocks.remove(i);
                b.bank = bank;
                let in_bank: Vec<usize> = self.blocks.iter().enumerate().filter(|(_, x)| x.bank == bank).map(|(i, _)| i).collect();
                let at = if pos >= in_bank.len() { in_bank.last().map(|i| i + 1).unwrap_or(self.blocks.len()) } else { in_bank[pos] };
                self.blocks.insert(at, b);
                self.generation += 1;
                out.push(format!("FXM ok {id}"));
            }
            "FXE" => {
                let id = argn(1).unwrap_or(-1) as u32;
                let on = argn(2).unwrap_or(1) != 0;
                let used = self.cost();
                let Some(b) = self.blocks.iter_mut().find(|b| b.id == id) else {
                    return vec!["ERR FXE bad argument".into()];
                };
                b.on = on;
                let _ = used;
                self.generation += 1;
                out.push(format!("FXE ok cpu={}/{} ticks ({}%)", self.cost(), self.cpu_budget, self.cost() * 100 / self.cpu_budget));
            }
            "FXP" => {
                let (id, idx, val) = (argn(1).unwrap_or(-1) as u32, argn(2).unwrap_or(-1), argn(3).unwrap_or(0) as i32);
                let ty = self.blocks.iter().find(|b| b.id == id).map(|b| b.type_name.clone());
                let Some(ty) = ty else { return vec!["ERR FXP bad argument".into()] };
                let Some(def) = self.effect(&ty).and_then(|e| e.parameters.get(idx.max(0) as usize)).cloned() else {
                    return vec!["ERR FXP bad argument".into()];
                };
                let v = val.clamp(def.min, def.max);
                if let Some(b) = self.blocks.iter_mut().find(|b| b.id == id) {
                    b.params[idx as usize] = v;
                }
                out.push(format!("FXP ok {v}"));
            }
            "FXL" => {
                let id = argn(1).unwrap_or(-1) as u32;
                let Some(b) = self.blocks.iter_mut().find(|b| b.id == id) else {
                    return vec!["ERR FXL bad argument".into()];
                };
                b.lane = argn(2).unwrap_or(0) as u8;
                self.generation += 1;
                out.push(format!("FXL ok {} {}", arg(1), arg(2)));
            }
            "AMP" => {
                self.amp_on = argn(1).unwrap_or(1) != 0;
                self.generation += 1;
                out.push(format!("AMP {}", self.amp_on as u8));
            }
            "AMPX" => {
                self.amp_present = argn(1).unwrap_or(1) != 0;
                out.push(format!("AMPX {} {}", self.amp_present as u8, self.bank_blocks('P').len()));
            }
            "AMPEQ" => {
                if t.len() >= 6 {
                    for (i, v) in self.ampeq.iter_mut().take(5).enumerate() {
                        *v = argn(i + 1).unwrap_or(0) as i32;
                    }
                    if t.len() >= 7 {
                        self.ampeq[5] = argn(6).unwrap_or(0) as i32;
                    }
                }
                let e = self.ampeq;
                out.push(format!("AMPEQ volume={} bass={} mid={} treble={} presence={} drive={}", e[0], e[1], e[2], e[3], e[4], e[5]));
            }
            "PLIST" => {
                for (slot, p) in &self.stored {
                    let col = p.color.as_ref().map(|c| format!(" col={c}")).unwrap_or_default();
                    out.push(format!("PRE {slot} {} 0 0 '{}'{col}", p.blocks.len(), p.name));
                }
                out.push(format!("PLIST end current={}", self.preset));
            }
            "PLOAD" => {
                let n = argn(1).unwrap_or(-1);
                if !(0..32).contains(&n) || !self.apply_stored(n as u8) {
                    return vec!["ERR PLOAD empty or invalid slot".into()];
                }
                out.push(format!("PLOAD ok slot={n} missing=0 ms=12"));
            }
            "PNEW" => {
                self.blocks.clear();
                self.amp_present = true;
                self.amp_on = true;
                self.ampeq = [0; 6];
                self.gain = (0, 0);
                self.preset = -1;
                self.scene = -1;
                self.preset_color = None;
                self.generation += 1;
                out.push("PNEW ok".into());
            }
            "PSAVE" => {
                let slot = argn(1).unwrap_or(-1);
                if !(0..32).contains(&slot) {
                    return vec!["ERR PSAVE bad slot".into()];
                }
                let name = t[2..].join(" ");
                self.store_current(slot as u8, &name);
                out.push(format!("PSAVE ok slot={slot} capture={}", self.capture_cur));
            }
            "PCOLOR" => {
                self.preset_color = Some(arg(1).to_string()).filter(|c| c != "none");
                out.push(format!("PCOLOR {} preset={}", arg(1), self.preset));
            }
            "SNAPS" => {
                let bits: String = (0..4)
                    .map(|i| {
                        let has = self.stored.get(&(self.preset.max(0) as u8)).map(|p| p.scenes[i].is_some()).unwrap_or(false);
                        if self.preset >= 0 && has { '1' } else { '0' }
                    })
                    .collect();
                out.push(format!("SNAPS {bits} current={}", self.scene));
            }
            "SNAP" => {
                let n = argn(1).unwrap_or(-1);
                let sc = (0..4).contains(&n).then(|| self.stored.get(&(self.preset.max(0) as u8)).and_then(|p| p.scenes[n as usize].clone())).flatten();
                let Some(sc) = sc.filter(|_| self.preset >= 0) else {
                    return vec!["ERR SNAP empty slot".into()];
                };
                self.apply_scene(&sc);
                self.scene = n as i32;
                out.push(format!("SNAP ok {n} missing=0 ms=3"));
            }
            "SNAPSAVE" => {
                let n = argn(1).unwrap_or(-1);
                if !(0..4).contains(&n) || self.preset < 0 {
                    return vec!["ERR SNAPSAVE bad argument".into()];
                }
                let st = self.scene_state();
                let p = self.preset as u8;
                if let Some(sp) = self.stored.get_mut(&p) {
                    sp.scenes[n as usize] = Some(st);
                }
                self.scene = n as i32;
                out.push(format!("SNAPSAVE ok {n} preset={}", self.preset));
            }
            "SNAPNAME" => {
                let p = self.preset.max(0) as u8;
                if t.len() >= 3 {
                    let n = argn(1).unwrap_or(-1);
                    if !(0..4).contains(&n) {
                        return vec!["ERR SNAPNAME bad argument".into()];
                    }
                    let name = t[2..].join(" ");
                    if let Some(sp) = self.stored.get_mut(&p) {
                        sp.scene_names[n as usize] = name.clone();
                    }
                    out.push(format!("SNAPNAME ok {n} '{name}' preset={}", self.preset));
                } else {
                    for i in 0..4 {
                        let n = self.stored.get(&p).map(|sp| sp.scene_names[i].clone()).unwrap_or_else(|| format!("Scene {}", i + 1));
                        out.push(format!("SNAPNAME {i} '{n}'"));
                    }
                    out.push("SNAPNAME end".into());
                }
            }
            "TUNER" => {
                if t.len() >= 2 {
                    let on = argn(1).unwrap_or(0);
                    if !(0..=1).contains(&on) {
                        return vec!["ERR TUNER bad argument".into()];
                    }
                    self.tuner_on = on == 1;
                    if t.len() >= 3 {
                        self.tuner_mute = argn(2).unwrap_or(0) == 1;
                    }
                    if t.len() >= 4 {
                        match argn(3) {
                            Some(r) if (400..=480).contains(&r) => self.tuner_ref = r as u32,
                            _ => return vec!["ERR TUNER bad reference".into()],
                        }
                    }
                }
                out.push(format!(
                    "TUNER {} mute={} ref={}",
                    self.tuner_on as u8, self.tuner_mute as u8, self.tuner_ref
                ));
            }
            "TUNE" => {
                if !self.tuner_on {
                    out.push("TUNE off".into());
                } else {
                    // a slow sweep over the open strings of a guitar, a few cents off
                    const STRINGS: [(&str, i32, f64); 6] = [
                        ("E", 2, 82.4069),
                        ("A", 2, 110.0),
                        ("D", 3, 146.8324),
                        ("G", 3, 195.9977),
                        ("B", 3, 246.9417),
                        ("E", 4, 329.6276),
                    ];
                    self.tune_tick += 1;
                    let k = self.tune_tick as usize;
                    if (k / 60) % 7 == 6 {
                        out.push("TUNE level=-1400".into());
                    } else {
                        let (n, o, f) = STRINGS[(k / 60) % 7 % 6];
                        let cents = ((k % 60) as f64 - 30.0) * 1.6;
                        let scale = f64::from(self.tuner_ref) / 440.0;
                        let hz = f * scale * (cents / 1200.0).exp2();
                        out.push(format!(
                            "TUNE {n}{o} hz={hz:.2} cents={cents:+.1} level=-210 ons={}",
                            k / 60
                        ));
                    }
                }
            }
            "GAIN" => {
                if t.len() >= 3 {
                    self.gain = (argn(1).unwrap_or(0) as i32, argn(2).unwrap_or(0) as i32);
                }
                out.push(format!("GAIN in={} out={} dB", self.gain.0, self.gain.1));
            }
            "LVL" => out.push(format!(
                "LVL in=-300 out=-250 ci=0 co=0 fx={}/{} mem=1 sk=0 m={} g=0/0 a={} e=1/2 s={} c={} p={}",
                self.cost(), self.cpu_budget, self.audio, self.amp_on as u8, self.scene, self.generation, self.preset
            )),
            "MODEL" => {
                let n = self.captures.get(self.capture_cur.max(0) as usize).and_then(|c| c.as_ref()).map(|c| c.0.clone()).unwrap_or_default();
                out.push(format!("MODEL '{n}' layers=10 engine=A2L"));
            }
            "CLIST" => {
                for (i, c) in self.captures.iter().enumerate() {
                    if let Some((n, b)) = c {
                        out.push(format!("CAP {i} {b} '{n}'"));
                    }
                }
                out.push(format!("CLIST end current={} slots=15", self.capture_cur));
            }
            "ILIST" => {
                for (i, c) in self.irs.iter().enumerate() {
                    if let Some((n, b)) = c {
                        out.push(format!("IRS {i} {b} '{n}' fs=48"));
                    }
                }
                out.push("ILIST end current=-1 slots=8".into());
            }
            "AUDIO" => {
                self.audio = if arg(1) == "OFF" { 0 } else { 2 };
                out.push(format!("AUDIO {}", self.audio));
            }
            "LDBEGIN" | "IRBEGIN" => {
                let n = argn(1).unwrap_or(0).max(0) as usize;
                if n == 0 {
                    self.stage = None;
                    out.push(format!("OK {cmd} 0"));
                } else if cmd == "IRBEGIN" && (n > 4096 || n * 2 > self.ir_mem_limit) {
                    return vec![if n > 4096 { "ERR IRBEGIN 1..4096".into() } else { "ERR IRBEGIN out of memory".into() }];
                } else {
                    let bytes = if cmd == "IRBEGIN" { n * 2 } else { n };
                    if cmd == "IRBEGIN" {
                        self.last_ir_gain = argn(2).unwrap_or(0) as i32;
                    }
                    self.stage = Some(Stage { kind: if cmd == "LDBEGIN" { 'C' } else { 'I' }, expected: bytes, data: vec![] });
                    out.push(format!("OK {cmd} {n}"));
                }
            }
            "LDB" | "IRB" => {
                let Some(st) = self.stage.as_mut() else { return vec![format!("ERR {cmd} no upload")] };
                let off = argn(1).unwrap_or(-1);
                if off != st.data.len() as i64 {
                    return vec![format!("ERR {cmd} offset")];
                }
                let hex = arg(2);
                let bytes: Option<Vec<u8>> = (0..hex.len() / 2).map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).ok()).collect();
                let Some(bytes) = bytes else { return vec![format!("ERR {cmd} bad hex")] };
                st.data.extend_from_slice(&bytes);
                out.push(format!("OK {cmd} {off} {}", bytes.len()));
            }
            "LDEND" | "IREND" => {
                let Some(st) = self.stage.take() else { return vec![format!("ERR {cmd} no upload")] };
                let crc = format!("{:08x}", crc32fast::hash(&st.data));
                if crc != arg(1) || st.data.len() != st.expected {
                    return vec![format!("ERR {cmd} crc mismatch")];
                }
                if cmd == "LDEND" {
                    let ok = st.data.len() > 44 && &st.data[..4] == b"A2L1";
                    self.last_blob = st.data.clone();
                    let rc = if ok { 0 } else { -4 };
                    out.push(format!("LOAD rc={rc} name='sim' layers=10 channels=3 audio={} engine=A2L ms=5", self.audio));
                    self.stage = None;
                } else {
                    self.last_ir = st.data.clone();
                    out.push(format!("IR ok {} samples gain 0 mdB fs 48", st.data.len() / 2));
                }
            }
            "CTRIM" => out.push(if arg(1) == "live" {
                "CTRIM ok slot=-1 trim=0.0 saved=0".to_string()
            } else {
                format!("CTRIM ok slot={} trim={} saved=1", arg(1), arg(2))
            }),
            "CDEL" | "IDEL" => {
                let slot = argn(1).unwrap_or(-1);
                let (list, kind): (&mut Vec<Option<(String, usize)>>, char) =
                    if cmd == "CDEL" { (&mut self.captures, 'C') } else { (&mut self.irs, 'I') };
                if slot < 0 || slot as usize >= list.len() || list[slot as usize].is_none() {
                    return vec![format!("ERR {cmd} empty or invalid slot")];
                }
                list[slot as usize] = None;
                self.blobs.remove(&(kind, slot as u8));
                out.push(format!("{cmd} ok {slot}"));
            }
            "CSAVE" => {
                let slot = argn(1).unwrap_or(-1);
                if !(0..15).contains(&slot) {
                    return vec!["ERR CSAVE bad slot".into()];
                }
                let name = t[2..].join(" ");
                self.captures[slot as usize] = Some((name.clone(), self.last_blob.len()));
                self.blobs.insert(('C', slot as u8), self.last_blob.clone());
                out.push(format!("CSAVE ok {slot} '{name}' {} B", self.last_blob.len()));
            }
            "ISAVE" => {
                let slot = argn(1).unwrap_or(-1);
                if !(0..8).contains(&slot) {
                    return vec!["ERR ISAVE bad slot".into()];
                }
                let name = t[2..].join(" ");
                self.irs[slot as usize] = Some((name.clone(), self.last_ir.len()));
                self.blobs.insert(('I', slot as u8), self.last_ir.clone());
                self.ir_gain.insert(slot as u8, self.last_ir_gain);
                out.push(format!("ISAVE ok {slot} '{name}' {} samples fs=48", self.last_ir.len() / 2));
            }
            "FXIR" => out.push(format!("FXIR ok {} {}", arg(1), arg(2))),
            "CLOAD" => {
                let slot = argn(1).unwrap_or(-1);
                if !(0..15).contains(&slot) || self.captures[slot as usize].is_none() {
                    return vec!["ERR CLOAD empty or invalid slot".into()];
                }
                self.capture_cur = slot as i32;
                out.push(format!("CLOAD ok {slot} audio={}", self.audio));
            }
            "CREAD" | "IREAD" => {
                let kind = if cmd == "CREAD" { 'C' } else { 'I' };
                let slot = argn(1).unwrap_or(-1);
                let Some((name, data)) = (slot >= 0).then(|| self.blob_info(kind, slot as usize)).flatten() else {
                    return vec![format!("ERR {cmd} empty or invalid slot")];
                };
                match argn(2) {
                    None => {
                        let aux = if kind == 'I' { self.ir_gain.get(&(slot as u8)).copied().unwrap_or(0) } else { 0 };
                        let fs = if kind == 'I' { 48 } else { 0 };
                        out.push(format!("{cmd} {slot} len={} crc={:08x} id=00000000 aux={aux} fs={fs} name='{name}'", data.len(), crc32fast::hash(&data)));
                    }
                    Some(off) => {
                        let off = off.max(0) as usize;
                        if off >= data.len() {
                            return vec![format!("ERR {cmd} offset")];
                        }
                        let end = (off + 96).min(data.len());
                        use base64::Engine;
                        out.push(format!(
                            "{cmd} {slot} {off} {} {:08x} {}",
                            end - off,
                            crc32fast::hash(&data[off..end]),
                            base64::engine::general_purpose::STANDARD.encode(&data[off..end])
                        ));
                    }
                }
            }
            "HPF" => {
                if let Some(v) = argn(1) {
                    self.hpf = v as i32;
                }
                out.push(format!("HPF hz={}", self.hpf));
            }
            "LEDCFG" => {
                for kv in t.iter().skip(1) {
                    if let Some((k, v)) = kv.split_once('=') {
                        self.ledcfg.insert(k.to_string(), v.to_string());
                    }
                }
                let body: Vec<String> = self.ledcfg.iter().map(|(k, v)| format!("{k}={v}")).collect();
                out.push(format!("LEDCFG {}", body.join(" ")));
            }
            "FSMODE" => {
                if let Some(v) = arg(1).strip_prefix("layout=") {
                    self.fsmode = v.to_string();
                }
                out.push(format!(
                    "FSMODE layout={} mode=preset bank=1 bypass=0",
                    self.fsmode
                ));
            }
            "FSA" => {
                if t.len() >= 4 {
                    self.fsa.insert(format!("{} {}", t[1], t[2]), t[3..].join(" "));
                    out.push(format!("FSA {} {} {}", t[1], t[2], t[3..].join(" ")));
                } else {
                    for (k, v) in &self.fsa {
                        out.push(format!("FSA {k} {v}"));
                    }
                    out.push("FSA end".into());
                }
            }
            "FSLED" => {
                if t.len() >= 3 {
                    let f = argn(1).unwrap_or(0).clamp(0, 3) as usize;
                    let cur = self.fsled[f].clone();
                    self.fsled[f] = if t[2] == "auto" {
                        (false, cur.1, cur.2)
                    } else {
                        (true, t[2].to_string(), t.get(3).unwrap_or(&"000000").to_string())
                    };
                    let e = &self.fsled[f];
                    out.push(format!(
                        "FSLED {f} {} on={} off={}",
                        if e.0 { "custom" } else { "auto" },
                        e.1,
                        e.2
                    ));
                } else {
                    for (f, e) in self.fsled.iter().enumerate() {
                        out.push(format!(
                            "FSLED {f} {} on={} off={}",
                            if e.0 { "custom" } else { "auto" },
                            e.1,
                            e.2
                        ));
                    }
                    out.push("FSLED end".into());
                }
            }
            "GEQ" => {
                if t.len() >= 3 {
                    let band = argn(1).unwrap_or(0).clamp(0, 5) as usize;
                    self.geq[band] = argn(2).unwrap_or(0) as i32;
                    out.push(format!("GEQ {band} {}", self.geq[band]));
                } else {
                    let v: Vec<String> = self.geq[..5].iter().map(|x| x.to_string()).collect();
                    out.push(format!("GEQ {}", v.join(" ")));
                }
            }
            "GSAVE" => {
                self.gsaved = true;
                out.push("GSAVE ok".into());
            }
            "ILOAD" => out.push(format!("ILOAD ok {}", arg(1))),
            _ => out.push(format!("ERR {cmd} unknown command")),
        }
        out
    }
}

pub struct SimLink {
    pub state: Arc<Mutex<SimState>>,
    queue: VecDeque<String>,
    alive: Arc<Mutex<bool>>,
}

impl SimLink {
    pub fn new(state: Arc<Mutex<SimState>>) -> Self {
        Self {
            state,
            queue: VecDeque::new(),
            alive: Arc::new(Mutex::new(true)),
        }
    }

    pub fn alive_flag(&self) -> Arc<Mutex<bool>> {
        self.alive.clone()
    }
}

impl Link for SimLink {
    fn write_line(&mut self, line: &str) -> io::Result<()> {
        if !*self.alive.lock().unwrap() {
            return Err(io::Error::new(io::ErrorKind::BrokenPipe, "unplugged"));
        }
        let mut st = self.state.lock().unwrap();
        if self.queue.iter().any(|l| !l.starts_with('@')) {
            st.overlap_violations += 1;
        }
        let replies = st.handle(line);
        self.queue.extend(replies);
        Ok(())
    }

    fn read_line(&mut self, timeout: Duration) -> io::Result<Option<String>> {
        if !*self.alive.lock().unwrap() {
            return Err(io::Error::new(io::ErrorKind::BrokenPipe, "unplugged"));
        }
        if let Some(l) = self.queue.pop_front() {
            return Ok(Some(l));
        }
        if !timeout.is_zero() {
            // emulate waiting without burning the CPU, but never longer than needed
            let start = Instant::now();
            while start.elapsed() < timeout.min(Duration::from_millis(5)) {
                std::thread::sleep(Duration::from_millis(1));
            }
        }
        Ok(None)
    }
}

/// Connector exposing one simulated "port" (VID matches the protocol).
pub struct SimConnector {
    pub state: Arc<Mutex<SimState>>,
    pub vid: u16,
    pub plugged: Arc<Mutex<bool>>,
    pub links: Mutex<Vec<Arc<Mutex<bool>>>>,
}

impl SimConnector {
    pub fn new(protocol: &Protocol) -> Self {
        Self {
            state: Arc::new(Mutex::new(SimState::new(protocol))),
            vid: protocol.usb_vendor_id,
            plugged: Arc::new(Mutex::new(true)),
            links: Mutex::new(vec![]),
        }
    }

    /// Simulate unplugging: the port disappears and open links start failing.
    pub fn unplug(&self) {
        *self.plugged.lock().unwrap() = false;
        for l in self.links.lock().unwrap().iter() {
            *l.lock().unwrap() = false;
        }
    }

    pub fn plug(&self) {
        *self.plugged.lock().unwrap() = true;
    }
}

impl Connector for SimConnector {
    fn candidates(&self) -> Vec<PortCandidate> {
        if *self.plugged.lock().unwrap() {
            vec![PortCandidate {
                name: "SIM0".into(),
                vid: self.vid,
                pid: 1,
                product: Some("UltraRig (simulated)".into()),
                serial_number: None,
            }]
        } else {
            vec![]
        }
    }

    fn open(&self, _port: &PortCandidate) -> io::Result<Box<dyn Link>> {
        let l = SimLink::new(self.state.clone());
        self.links.lock().unwrap().push(l.alive_flag());
        Ok(Box::new(l))
    }
}
