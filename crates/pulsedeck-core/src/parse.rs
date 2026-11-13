//! Parsers for the pedal's text replies.
//!
//! Every parser is tolerant: unknown trailing `key=value` fields are ignored, since the protocol
//! was derived by static reading and firmware revisions add fields.

use crate::protocol::{EffectType, ParamDef};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;

/// Split `key=value` tokens of a line into a map (tokens without `=` are skipped).
pub fn kv_map(line: &str) -> HashMap<&str, &str> {
    line.split_whitespace()
        .filter_map(|t| t.split_once('='))
        .collect()
}

/// Parse "a/b" into (a, b).
pub fn ratio(s: &str) -> Option<(i64, i64)> {
    let (a, b) = s.split_once('/')?;
    Some((a.trim().parse().ok()?, b.trim().parse().ok()?))
}

/// Extract the text between the first and the last single quote.
pub fn quoted(line: &str) -> Option<&str> {
    let a = line.find('\'')?;
    let b = line.rfind('\'')?;
    (b > a).then(|| &line[a + 1..b])
}

// ------------------------------------------------------------------------------------- PONG

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct Pong {
    pub name: String,
    pub version: String,
    /// `M5.26` -> 526
    pub firmware_number: Option<u32>,
    pub flags: Vec<String>,
    pub safe_mode: bool,
    pub build_date: String,
    pub raw: String,
}

impl Pong {
    pub fn has_flag(&self, f: &str) -> bool {
        self.flags.iter().any(|x| x == f)
    }
    /// "Tank-B" (bass) build when flagged `tankb`, else "Tank-G".
    pub fn model(&self) -> &'static str {
        if self.has_flag("tankb") {
            "Tank-B"
        } else {
            "Tank-G"
        }
    }
}

const MONTHS: [&str; 12] = [
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
];

pub fn parse_pong(line: &str) -> Option<Pong> {
    let line = line.trim();
    if !line.starts_with("PONG") {
        return None;
    }
    let safe_mode = line.ends_with(" safe");
    let tokens: Vec<&str> = line.split_whitespace().collect();
    // group 1 = everything between PONG and "<Mon> <dd> <yyyy>"
    let month_idx = tokens.iter().position(|t| MONTHS.contains(t));
    let end = month_idx.unwrap_or(tokens.len());
    let group = &tokens[1..end];
    let build_date = month_idx
        .map(|i| tokens[i..tokens.len().min(i + 3)].join(" "))
        .unwrap_or_default();
    let name = group.first().copied().unwrap_or("").to_string();
    let version = group.get(1).copied().unwrap_or("").to_string();
    let firmware_number = version
        .strip_prefix('M')
        .and_then(|v| v.split_once('.'))
        .and_then(|(a, b)| {
            let b: String = b.chars().take_while(|c| c.is_ascii_digit()).collect();
            Some(a.parse::<u32>().ok()? * 100 + b.parse::<u32>().ok()?)
        });
    let flags = group.iter().skip(2).map(|s| s.to_string()).collect();
    Some(Pong {
        name,
        version,
        firmware_number,
        flags,
        safe_mode,
        build_date,
        raw: line.into(),
    })
}

// --------------------------------------------------------------------------------------- FXT

/// Parse a full FXT reply (type lines, param lines, `os`/`pp` lines, `FXT end`).
pub fn parse_fxt(lines: &[String]) -> Vec<EffectType> {
    let mut out: Vec<EffectType> = Vec::new();
    for l in lines {
        let rest = match l.strip_prefix("FXT ") {
            Some(r) => r.trim(),
            None => continue,
        };
        if rest == "end" || rest == "fin" || rest.starts_with("os ") || rest.starts_with("pp ") {
            continue;
        }
        let mut it = rest.split_whitespace();
        let (Some(first), Some(second)) = (it.next(), it.next()) else {
            continue;
        };
        let Ok(num) = first.parse::<u32>() else {
            continue;
        };
        let kv = kv_map(rest);
        if kv.contains_key("cost") {
            out.push(EffectType {
                id: num,
                name: second.to_string(),
                category: kv.get("cat").map(|s| s.to_string()).unwrap_or_default(),
                cost: kv.get("cost").and_then(|s| s.parse().ok()),
                parameters: Vec::new(),
            });
        } else if let Some(cur) = out.last_mut() {
            // "<param_index> <param_name> <min>..<max> (<default>)"
            let range = it.next().unwrap_or("");
            let def = it
                .next()
                .unwrap_or("")
                .trim_matches(|c| c == '(' || c == ')');
            let Some(pos) = range.get(1..).and_then(|r| r.find("..")).map(|p| p + 1) else {
                continue;
            };
            let (min, max) = (&range[..pos], &range[pos + 2..]);
            if let (Ok(min), Ok(max)) = (min.parse::<i32>(), max.parse::<i32>()) {
                cur.parameters.push(ParamDef {
                    index: num as u8,
                    name: second.to_string(),
                    min,
                    max,
                    default: def.parse().unwrap_or(min),
                });
            }
        }
    }
    out
}

// --------------------------------------------------------------------------------------- FXS

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Block {
    pub id: u32,
    /// "P" or "Q"
    pub bank: String,
    pub position: u32,
    pub type_name: String,
    pub on: bool,
    pub params: Vec<i32>,
    pub ticks: u32,
    pub lane: u8,
    pub group: Option<u32>,
    /// IR length info (`irlen=samples/kHz`), only for ir blocks.
    pub irlen: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct Cpu {
    pub used: i64,
    pub budget: i64,
}

impl Cpu {
    pub fn percent(&self) -> f64 {
        if self.budget > 0 {
            self.used as f64 * 100.0 / self.budget as f64
        } else {
            0.0
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct Chain {
    pub blocks: Vec<Block>,
    pub amp_present: bool,
    pub amp_on: bool,
    pub cpu: Cpu,
}

pub fn parse_block_line(line: &str) -> Option<Block> {
    // FX <P|Q><position> id=<n> <type_name> on=<0|1> p=<v0,v1,..> ticks=<n> [l=..] [g=..] ...
    let rest = line.strip_prefix("FX ")?;
    let mut tok = rest.split_whitespace();
    let pos_tok = tok.next()?;
    let bank = &pos_tok[..1];
    if bank != "P" && bank != "Q" {
        return None;
    }
    let position: u32 = pos_tok[1..].parse().ok()?;
    let kv = kv_map(rest);
    let id: u32 = kv.get("id")?.parse().ok()?;
    // type name: the token right after `id=`
    let type_name = rest
        .split_whitespace()
        .skip_while(|t| !t.starts_with("id="))
        .nth(1)?
        .to_string();
    let params = kv
        .get("p")
        .map(|p| {
            p.split(',')
                .filter(|s| !s.is_empty())
                .filter_map(|s| s.parse().ok())
                .collect()
        })
        .unwrap_or_default();
    Some(Block {
        id,
        bank: bank.into(),
        position,
        type_name,
        on: kv.get("on").map(|v| *v != "0").unwrap_or(true),
        params,
        ticks: kv.get("ticks").and_then(|v| v.parse().ok()).unwrap_or(0),
        lane: kv.get("l").and_then(|v| v.parse().ok()).unwrap_or(0),
        group: kv.get("g").and_then(|v| v.parse().ok()),
        irlen: kv.get("irlen").map(|s| s.to_string()),
    })
}

pub fn parse_fxs(lines: &[String]) -> Chain {
    let mut chain = Chain::default();
    for l in lines {
        if let Some(b) = parse_block_line(l) {
            chain.blocks.push(b);
        } else if let Some(rest) = l.strip_prefix("FXS ") {
            let kv = kv_map(rest);
            if let Some(a) = kv.get("amp") {
                chain.amp_present = *a != "0";
                chain.amp_on = chain.amp_present && kv.get("on").map(|v| *v != "0").unwrap_or(true);
            }
            if let Some((u, b)) = kv.get("cpu").and_then(|v| ratio(v)) {
                chain.cpu = Cpu { used: u, budget: b };
            }
        }
    }
    chain
        .blocks
        .sort_by(|a, b| (a.bank.as_str(), a.position).cmp(&(b.bank.as_str(), b.position)));
    chain
}

// ------------------------------------------------------------------------------------- PLIST

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct PresetInfo {
    pub slot: u8,
    pub block_count: u32,
    pub name: String,
    pub color: Option<String>,
    pub scene_mask: Option<u32>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct PresetList {
    pub presets: Vec<PresetInfo>,
    pub current: Option<u8>,
}

pub fn parse_plist(lines: &[String]) -> PresetList {
    let mut out = PresetList::default();
    for l in lines {
        if let Some(rest) = l.strip_prefix("PRE ") {
            let mut it = rest.split_whitespace();
            let (Some(slot), Some(count)) = (it.next(), it.next()) else {
                continue;
            };
            let (Ok(slot), Ok(block_count)) = (slot.parse::<u8>(), count.parse::<u32>()) else {
                continue;
            };
            let name = quoted(rest).unwrap_or("").to_string();
            let tail = rest.rfind('\'').map(|i| &rest[i + 1..]).unwrap_or("");
            let kv = kv_map(tail);
            out.presets.push(PresetInfo {
                slot,
                block_count,
                name,
                color: kv.get("col").map(|s| s.to_string()),
                scene_mask: kv.get("s").and_then(|s| s.parse().ok()),
            });
        } else if l.starts_with("PLIST end") || l.starts_with("PLIST fin") {
            out.current = kv_map(l)
                .get("current")
                .and_then(|v| v.parse::<i32>().ok())
                .and_then(|v| u8::try_from(v).ok());
        }
    }
    out
}

// -------------------------------------------------------------------------------------- SNAPS

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct Scenes {
    pub stored: [bool; 4],
    pub current: Option<u8>,
}

pub fn parse_snaps(line: &str) -> Option<Scenes> {
    let rest = line.strip_prefix("SNAPS ")?;
    let bits = rest.split_whitespace().next()?;
    let mut stored = [false; 4];
    for (i, c) in bits.chars().take(4).enumerate() {
        stored[i] = c == '1';
    }
    let current = kv_map(rest)
        .get("current")
        .and_then(|v| v.parse::<i32>().ok())
        .and_then(|v| u8::try_from(v).ok());
    Some(Scenes { stored, current })
}

// ------------------------------------------------------------------------- CLIST / ILIST

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct LibEntry {
    pub slot: u8,
    pub bytes: u32,
    pub name: String,
    /// `trim=<dB>` of a capture slot
    #[serde(default)]
    pub trim_db: Option<f64>,
    /// `in=<dBu>` input level stored with a capture slot
    #[serde(default)]
    pub input_dbu: Option<f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct Library {
    pub entries: Vec<LibEntry>,
    pub current: Option<i32>,
    pub slots: Option<u8>,
}

/// `item_prefix` is "CAP" (CLIST) or "IRS" (ILIST); `end_prefix` is "CLIST end" / "ILIST end".
pub fn parse_library(lines: &[String], item_prefix: &str, end_prefix: &str) -> Library {
    let mut lib = Library::default();
    for l in lines {
        if let Some(rest) = l
            .strip_prefix(item_prefix)
            .and_then(|r| r.strip_prefix(' '))
        {
            let mut it = rest.split_whitespace();
            let (Some(slot), Some(bytes)) = (it.next(), it.next()) else {
                continue;
            };
            if let (Ok(slot), Ok(bytes)) = (slot.parse(), bytes.parse()) {
                let tail = rest.rfind('\'').map(|i| &rest[i + 1..]).unwrap_or("");
                let kv = kv_map(tail);
                lib.entries.push(LibEntry {
                    slot,
                    bytes,
                    name: quoted(rest).unwrap_or("").into(),
                    trim_db: kv.get("trim").and_then(|v| v.parse().ok()),
                    input_dbu: kv.get("in").and_then(|v| v.parse().ok()),
                });
            }
        } else if l.starts_with(end_prefix) {
            let kv = kv_map(l);
            lib.current = kv.get("current").and_then(|v| v.parse().ok());
            lib.slots = kv.get("slots").and_then(|v| v.parse().ok());
        }
    }
    lib
}

// ----------------------------------------------------------------------------------- LVL / @L

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct Level {
    pub in_db10: Option<i32>,
    pub out_db10: Option<i32>,
    pub cpu: Cpu,
    pub chain_generation: Option<i64>,
    pub preset: Option<i32>,
    pub scene: Option<i32>,
    pub amp_on: Option<bool>,
}

pub fn parse_lvl(line: &str) -> Option<Level> {
    let rest = line.strip_prefix("LVL ")?;
    let kv = kv_map(rest);
    let cpu = kv
        .get("fx")
        .and_then(|v| ratio(v))
        .map(|(u, b)| Cpu { used: u, budget: b })
        .unwrap_or_default();
    Some(Level {
        in_db10: kv.get("in").and_then(|v| v.parse().ok()),
        out_db10: kv.get("out").and_then(|v| v.parse().ok()),
        cpu,
        chain_generation: kv.get("c").and_then(|v| v.parse().ok()),
        preset: kv.get("p").and_then(|v| v.parse().ok()),
        scene: kv.get("s").and_then(|v| v.parse().ok()),
        amp_on: kv.get("a").map(|v| *v != "0"),
    })
}

/// Asynchronous `@L <in_dB10> <out_dB10> <cpu_used>/<cpu_budget> <engine_used>/<limit> <lr> <gen> [<capture_in>]`
pub fn parse_at_level(line: &str) -> Option<Level> {
    let rest = line.strip_prefix("@L ")?;
    let t: Vec<&str> = rest.split_whitespace().collect();
    if t.len() < 3 {
        return None;
    }
    let (u, b) = ratio(t[2])?;
    Some(Level {
        in_db10: t[0].parse().ok(),
        out_db10: t[1].parse().ok(),
        cpu: Cpu { used: u, budget: b },
        chain_generation: t.get(5).and_then(|v| v.parse().ok()),
        ..Default::default()
    })
}

/// Result of `LDEND`: `LOAD rc=<code> name='<n>' layers=<n> channels=<n> audio=<mode> engine=<A2L|AMx> ms=<n>`
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct LoadResult {
    pub rc: i32,
    pub audio: Option<i32>,
    pub engine: Option<String>,
}

pub fn parse_load(line: &str) -> Option<LoadResult> {
    let rest = line.strip_prefix("LOAD ")?;
    let kv = kv_map(rest);
    Some(LoadResult {
        rc: kv.get("rc")?.parse().ok()?,
        audio: kv.get("audio").and_then(|v| v.parse().ok()),
        engine: kv.get("engine").map(|s| s.to_string()),
    })
}

/// `AMPEQ volume=<n> bass=<n> mid=<n> treble=<n> presence=<n> drive=<n>`
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct AmpEq {
    pub volume: i32,
    pub bass: i32,
    pub mid: i32,
    pub treble: i32,
    pub presence: i32,
    pub drive: i32,
}

pub fn parse_ampeq(line: &str) -> Option<AmpEq> {
    let rest = line.strip_prefix("AMPEQ ")?;
    let kv = kv_map(rest);
    let g = |k: &str| kv.get(k).and_then(|v| v.parse().ok());
    Some(AmpEq {
        volume: g("volume")?,
        bass: g("bass")?,
        mid: g("mid")?,
        treble: g("treble")?,
        presence: g("presence")?,
        drive: g("drive").unwrap_or(0),
    })
}

/// `FXA id=<new_instance_id> <type_name>`
pub fn parse_fxa_id(lines: &[String]) -> Option<u32> {
    lines.iter().find_map(|l| {
        l.strip_prefix("FXA id=")?
            .split_whitespace()
            .next()?
            .parse()
            .ok()
    })
}

/// State of the pedal's tuner mode (`TUNER` reply).
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct TunerState {
    pub on: bool,
    pub mute: bool,
    /// A4 reference in Hz (integer on the wire)
    pub ref_hz: u32,
}

/// `TUNER <on> mute=<m> ref=<hz>`
pub fn parse_tuner(line: &str) -> Option<TunerState> {
    let rest = line.strip_prefix("TUNER ")?;
    let on = rest.split_whitespace().next()?;
    let kv = kv_map(rest);
    Some(TunerState {
        on: match on {
            "0" => false,
            "1" => true,
            _ => return None,
        },
        mute: kv.get("mute")? == &"1",
        ref_hz: kv.get("ref")?.parse().ok()?,
    })
}

/// One `TUNE` poll reply.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum TuneReading {
    /// the pedal left tuner mode on its own (footswitch)
    Off,
    /// tuner on, but no pitch detected (silence / noise)
    NoSignal { level_db: Option<f64> },
    Note {
        /// "A", "A#", ... (no flats)
        name: String,
        octave: i32,
        hz: f64,
        /// distance from the note in cents, positive = sharp
        cents: f64,
        level_db: Option<f64>,
        /// increments on every new note onset
        onset: Option<u32>,
    },
}

/// `TUNE off` | `TUNE <note><oct> hz=<f> cents=<+-n.n> [level=<dB x10>] [ons=<n>]` | `TUNE ...`
/// without a note. Levels at or below -140 dB mean silence.
pub fn parse_tune(line: &str) -> Option<TuneReading> {
    let rest = line.strip_prefix("TUNE ")?.trim();
    if rest == "off" || rest.starts_with("off ") || rest.starts_with("apagado") {
        return Some(TuneReading::Off);
    }
    let kv = kv_map(rest);
    let level_db = kv
        .get("level")
        .or_else(|| kv.get("nivel"))
        .and_then(|v| v.parse::<f64>().ok())
        .map(|v| v / 10.0);
    let first = rest.split_whitespace().next().unwrap_or("");
    let mut chars = first.chars();
    let letter = chars.next().filter(|c| ('A'..='G').contains(c));
    let tail: String = chars.collect();
    let (sharp, oct) = match tail.strip_prefix('#') {
        Some(o) => (true, o),
        None => (false, tail.as_str()),
    };
    let note = letter.and_then(|l| {
        let octave: i32 = oct.parse().ok()?;
        let hz: f64 = kv.get("hz")?.parse().ok()?;
        let cents: f64 = kv.get("cents")?.parse().ok()?;
        Some(TuneReading::Note {
            name: format!("{l}{}", if sharp { "#" } else { "" }),
            octave,
            hz,
            cents,
            level_db: level_db.filter(|l| *l > -140.0),
            onset: kv.get("ons").and_then(|v| v.parse().ok()),
        })
    });
    Some(note.unwrap_or(TuneReading::NoSignal {
        level_db: level_db.filter(|l| *l > -140.0),
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(s: &[&str]) -> Vec<String> {
        s.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn pong_parses_model_and_version() {
        let p =
            parse_pong("PONG tankg_fw M5.26 rel amx ble tankb v1 Nov 12 2026 10:11:12").unwrap();
        assert_eq!(p.name, "tankg_fw");
        assert_eq!(p.version, "M5.26");
        assert_eq!(p.firmware_number, Some(526));
        assert_eq!(p.model(), "Tank-B");
        assert!(p.has_flag("ble") && p.has_flag("amx"));
        assert_eq!(p.build_date, "Nov 12 2026");
        assert!(!p.safe_mode);
        assert!(
            parse_pong("PONG tankg_fw M4.10 Jan  3 2025 x safe")
                .unwrap()
                .safe_mode
        );
        assert!(parse_pong("hello").is_none());
    }

    #[test]
    fn fxt_builds_catalog() {
        let fx = parse_fxt(&v(&[
            "FXT 1 gate cost=40 cat=dynamics",
            "FXT 0 umbral_db -90..0 (-70)",
            "FXT 1 release_ms 5..1000 (120)",
            "FXT 3 overdrive cost=90 cat=drive mem=0",
            "FXT 0 drive_db 0..40 (18)",
            "FXT os tiers=0,1,3",
            "FXT end",
        ]));
        assert_eq!(fx.len(), 2);
        assert_eq!(fx[0].name, "gate");
        assert_eq!(fx[0].parameters.len(), 2);
        assert_eq!(fx[0].parameters[0].min, -90);
        assert_eq!(fx[0].parameters[0].default, -70);
        assert_eq!(fx[1].category, "drive");
    }

    #[test]
    fn fxt_negative_ranges() {
        let fx = parse_fxt(&v(&[
            "FXT 2 boost cost=1",
            "FXT 0 ganancia_db -12..24 (6)",
            "FXT 1 x -5..-1 (-3)",
        ]));
        assert_eq!(fx[0].parameters[1].min, -5);
        assert_eq!(fx[0].parameters[1].max, -1);
    }

    #[test]
    fn fxs_chain_and_cpu() {
        let c = parse_fxs(&v(&[
            "FX P0 id=3 boost on=1 p=6 ticks=12",
            "FX Q0 id=9 ir on=0 p=100,0,0,21 ticks=80 irlen=2048/48",
            "FX P1 id=4 overdrive on=1 p=18,4000,-8,1,0 ticks=40 l=1 g=0",
            "FXS amp=1 on=1",
            "FXS cpu=550/1000 ticks (55%) mem_fx=1234",
            "FXS peak=600 ticks skipped=0",
        ]));
        assert_eq!(c.blocks.len(), 3);
        assert_eq!(c.blocks[0].id, 3);
        assert_eq!(c.blocks[1].position, 1);
        assert_eq!(c.blocks[1].lane, 1);
        let ir = c.blocks.iter().find(|b| b.type_name == "ir").unwrap();
        assert!(!ir.on);
        assert_eq!(ir.params, vec![100, 0, 0, 21]);
        assert!(c.amp_present && c.amp_on);
        assert_eq!(
            c.cpu,
            Cpu {
                used: 550,
                budget: 1000
            }
        );
        assert!((c.cpu.percent() - 55.0).abs() < 1e-9);
    }

    #[test]
    fn plist_names_with_spaces() {
        let l = parse_plist(&v(&[
            "PRE 0 4 0 0 'Clean Fender Twin' t=- s=15 col=ff8800 amp=1",
            "PRE 5 2 0 0 'Lead' ",
            "PLIST end current=5",
        ]));
        assert_eq!(l.presets.len(), 2);
        assert_eq!(l.presets[0].name, "Clean Fender Twin");
        assert_eq!(l.presets[0].color.as_deref(), Some("ff8800"));
        assert_eq!(l.current, Some(5));
        let none = parse_plist(&v(&["PLIST end current=-1"]));
        assert_eq!(none.current, None);
    }

    #[test]
    fn snaps_lvl_and_async() {
        let s = parse_snaps("SNAPS 1101 current=2").unwrap();
        assert_eq!(s.stored, [true, true, false, true]);
        assert_eq!(s.current, Some(2));
        let l = parse_lvl("LVL in=-120 out=-80 ci=0 co=0 fx=420/1000 mem=1 sk=0 m=2 g=0/0 a=1 e=1/2 b=1200 s=1 c=77 p=3").unwrap();
        assert_eq!(
            l.cpu,
            Cpu {
                used: 420,
                budget: 1000
            }
        );
        assert_eq!(l.chain_generation, Some(77));
        assert_eq!(l.preset, Some(3));
        assert_eq!(l.scene, Some(1));
        let a = parse_at_level("@L -120 -80 900/1000 1/2 0 12").unwrap();
        assert_eq!(a.cpu.used, 900);
        assert_eq!(a.chain_generation, Some(12));
    }

    #[test]
    fn libraries() {
        let c = parse_library(
            &v(&[
                "CAP 0 1800 'Plexi' trim=0.0 in=12",
                "CAP 3 1900 'Dumble'",
                "CLIST end current=3 slots=15",
            ]),
            "CAP",
            "CLIST end",
        );
        assert_eq!(c.entries.len(), 2);
        assert_eq!(c.entries[1].slot, 3);
        assert_eq!(c.current, Some(3));
        assert_eq!(c.entries[0].trim_db, Some(0.0));
        assert_eq!(c.entries[0].input_dbu, Some(12.0));
        assert_eq!(c.entries[1].trim_db, None);
        assert_eq!(c.slots, Some(15));
        let i = parse_library(
            &v(&["IRS 1 4000 '4x12' fs=48", "ILIST end current=-1 slots=8"]),
            "IRS",
            "ILIST end",
        );
        assert_eq!(i.entries[0].name, "4x12");
        assert_eq!(i.slots, Some(8));
    }

    #[test]
    fn load_and_misc() {
        let r = parse_load("LOAD rc=0 name='Plexi' layers=10 channels=3 audio=2 engine=A2L ms=412")
            .unwrap();
        assert_eq!(
            r,
            LoadResult {
                rc: 0,
                audio: Some(2),
                engine: Some("A2L".into())
            }
        );
        assert_eq!(parse_load("LOAD rc=-4 name='x'").unwrap().rc, -4);
        let e = parse_ampeq("AMPEQ volume=0 bass=10 mid=-5 treble=20 presence=0 drive=3").unwrap();
        assert_eq!((e.bass, e.mid, e.drive), (10, -5, 3));
        assert_eq!(
            parse_fxa_id(&v(&["FXA id=14 delay", "FX cpu=1/2 ticks (50%) mem_fx=0"])),
            Some(14)
        );
    }

    #[test]
    fn tuner_state_and_readings() {
        let t = parse_tuner("TUNER 1 mute=1 ref=440").unwrap();
        assert_eq!(
            t,
            TunerState {
                on: true,
                mute: true,
                ref_hz: 440
            }
        );
        assert!(!parse_tuner("TUNER 0 mute=0 ref=432").unwrap().on);
        assert!(parse_tuner("TUNER 2 mute=0 ref=440").is_none());
        assert!(parse_tuner("TUNE E2 hz=82.4 cents=+0.0").is_none());

        assert_eq!(parse_tune("TUNE off"), Some(TuneReading::Off));
        match parse_tune("TUNE A#3 hz=233.10 cents=-4.5 level=-215 ons=7").unwrap() {
            TuneReading::Note {
                name,
                octave,
                hz,
                cents,
                level_db,
                onset,
            } => {
                assert_eq!((name.as_str(), octave, onset), ("A#", 3, Some(7)));
                assert!((hz - 233.10).abs() < 1e-9 && (cents + 4.5).abs() < 1e-9);
                assert_eq!(level_db, Some(-21.5));
            }
            other => panic!("{other:?}"),
        }
        match parse_tune("TUNE E2 hz=82.41 cents=+12.0").unwrap() {
            TuneReading::Note {
                cents, level_db, ..
            } => {
                assert_eq!((cents, level_db), (12.0, None));
            }
            other => panic!("{other:?}"),
        }
        // silence: no note; level at the floor is not reported
        assert_eq!(
            parse_tune("TUNE -- level=-1400"),
            Some(TuneReading::NoSignal { level_db: None })
        );
        assert_eq!(
            parse_tune("TUNE level=-300"),
            Some(TuneReading::NoSignal {
                level_db: Some(-30.0)
            })
        );
        assert!(parse_tune("TUNER 1 mute=0 ref=440").is_none());
    }
}
