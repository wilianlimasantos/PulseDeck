//! Access to the wire-protocol definition (`protocol.json`).
//!
//! The JSON file is embedded at compile time and is the single source of truth for transport
//! settings (VID, baud), framing limits, per-command timeouts and the reference effect catalog.
//! Nothing in here hardcodes effect ids: the catalog comes from the JSON and is then superseded
//! at runtime by the pedal's own `FXT` reply.

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::time::Duration;

const PROTOCOL_JSON: &str = include_str!("../../../protocol.json");

/// Fallback when the JSON prose cannot be parsed (framing.flow_control says "default 1500 ms").
const FALLBACK_TIMEOUT_MS: u64 = 1500;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ParamDef {
    pub index: u8,
    pub name: String,
    pub min: i32,
    pub max: i32,
    pub default: i32,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct EffectType {
    pub id: u32,
    pub name: String,
    #[serde(default)]
    pub category: String,
    #[serde(default)]
    pub cost: Option<u32>,
    #[serde(default)]
    pub parameters: Vec<ParamDef>,
}

/// Settings of the Bluetooth LE transport (see `Protocol::ble_config`). UUIDs are lower case.
#[derive(Debug, Clone, PartialEq)]
pub struct BleConfig {
    pub service: String,
    pub rx: String,
    pub tx: String,
    pub pair: String,
    pub extra_services: Vec<String>,
    pub name_filters: Vec<String>,
    pub scan: Duration,
    pub connect_timeout: Duration,
    pub pair_timeout: Duration,
    pub chunk_default: usize,
    pub chunk_max: usize,
    pub chunk_delay: Duration,
}

#[derive(Debug, Clone)]
pub struct Protocol {
    raw: Value,
    pub usb_vendor_id: u16,
    pub baud_rate: u32,
    pub default_timeout: Duration,
    /// (first firmware number where the limit applies, max line chars) sorted ascending.
    line_limits: Vec<(u32, usize)>,
    pub absolute_max_line: usize,
    pub capture_slots: u8,
    pub ir_slots: u8,
    pub pong_timeout: Duration,
    pub ping_attempts: u32,
}

fn parse_hex_u16(s: &str) -> Option<u16> {
    u16::from_str_radix(s.trim().trim_start_matches("0x"), 16).ok()
}

/// "firmware_ge_M4.83" -> 483
fn firmware_key_number(key: &str) -> Option<u32> {
    let rest = key.rsplit('M').next()?;
    let (maj, min) = rest.split_once('.')?;
    Some(maj.parse::<u32>().ok()? * 100 + min.parse::<u32>().ok()?)
}

fn find_key<'a>(v: &'a Value, key: &str) -> Option<&'a Value> {
    match v {
        Value::Object(m) => {
            if let Some(x) = m.get(key) {
                return Some(x);
            }
            m.values().find_map(|c| find_key(c, key))
        }
        Value::Array(a) => a.iter().find_map(|c| find_key(c, key)),
        _ => None,
    }
}

impl Protocol {
    pub fn embedded() -> Self {
        Self::from_json(PROTOCOL_JSON).expect("embedded protocol.json must be valid")
    }

    pub fn from_json(text: &str) -> Result<Self, String> {
        let raw: Value = serde_json::from_str(text).map_err(|e| e.to_string())?;
        let transport = &raw["transport"];
        let usb_vendor_id = transport["usb_vendor_id"]
            .as_str()
            .and_then(parse_hex_u16)
            .ok_or("transport.usb_vendor_id missing")?;
        let baud_rate = transport["serial_settings"]["baud_rate"]
            .as_u64()
            .ok_or("transport.serial_settings.baud_rate missing")? as u32;

        let framing = &raw["framing"];
        let mut line_limits: Vec<(u32, usize)> = Vec::new();
        let mut absolute_max_line = 199usize;
        if let Some(obj) = framing["max_command_line_chars_excluding_newline"].as_object() {
            for (k, v) in obj {
                let Some(n) = v.as_u64() else { continue };
                if k.starts_with("firmware_ge_") {
                    if let Some(fw) = firmware_key_number(k) {
                        line_limits.push((fw, n as usize));
                    }
                } else if k.starts_with("firmware_lt_") {
                    line_limits.push((0, n as usize));
                } else if k == "absolute_firmware_limit" {
                    absolute_max_line = n as usize;
                }
            }
        }
        line_limits.sort();
        if line_limits.is_empty() {
            return Err("framing line limits missing".into());
        }

        // "... (default 1500 ms; slow commands ...)"
        let default_timeout_ms = framing["flow_control"]
            .as_str()
            .and_then(|s| s.split("default ").nth(1))
            .and_then(|s| s.split(|c: char| !c.is_ascii_digit()).next())
            .and_then(|s| s.parse::<u64>().ok())
            .unwrap_or(FALLBACK_TIMEOUT_MS);

        let ping = &raw["commands"]["handshake_and_state_read"]["PING"];
        let pong_timeout = Duration::from_millis(ping["timeout_ms"].as_u64().unwrap_or(700));
        // transport.detection[] mentions "up to 5 times"
        let ping_attempts = raw["transport"]["detection"]
            .as_array()
            .and_then(|a| {
                a.iter().filter_map(|s| s.as_str()).find_map(|s| {
                    let rest = s.split("up to ").nth(1)?;
                    rest.split_whitespace().next()?.parse::<u32>().ok()
                })
            })
            .unwrap_or(5);

        Ok(Self {
            usb_vendor_id,
            baud_rate,
            default_timeout: Duration::from_millis(default_timeout_ms),
            line_limits,
            absolute_max_line,
            capture_slots: raw["conventions"]["capture_slots"].as_u64().unwrap_or(15) as u8,
            ir_slots: raw["conventions"]["ir_slots"].as_u64().unwrap_or(8) as u8,
            pong_timeout,
            ping_attempts,
            raw,
        })
    }

    pub fn raw(&self) -> &Value {
        &self.raw
    }

    /// Maximum command line length (without newline) for a firmware number (e.g. 526).
    pub fn max_line_chars(&self, firmware_number: Option<u32>) -> usize {
        let fw = firmware_number.unwrap_or(0);
        let mut limit = self.line_limits[0].1;
        for (from, n) in &self.line_limits {
            if fw >= *from {
                limit = *n;
            }
        }
        limit.min(self.absolute_max_line)
    }

    /// Timeout for a command, taken from the `timeout_ms` of its definition in `commands.*`
    /// (or `file_transfer.*.steps[]`) when present, else the framing default.
    pub fn timeout_for(&self, command: &str) -> Duration {
        let ms = self.command_timeout_ms(command);
        ms.map(Duration::from_millis)
            .unwrap_or(self.default_timeout)
    }

    fn command_timeout_ms(&self, command: &str) -> Option<u64> {
        if let Some(def) = find_key(&self.raw["commands"], command) {
            if let Some(ms) = def["timeout_ms"].as_u64() {
                return Some(ms);
            }
            // a few commands list timeouts per sub-case; use the longest
            if let Some(o) = def["timeout_ms"].as_object() {
                return o.values().filter_map(|v| v.as_u64()).max();
            }
        }
        // file-transfer steps: {"cmd": "LDEND <crc32_hex8>", "timeout_ms": ...}
        let steps = self.raw["file_transfer"].as_object()?;
        for sect in steps.values() {
            if let Some(arr) = sect["steps"].as_array() {
                for st in arr {
                    let first = st["cmd"].as_str().and_then(|c| c.split_whitespace().next());
                    if first == Some(command) {
                        return match &st["timeout_ms"] {
                            Value::Number(n) => n.as_u64(),
                            Value::Object(o) => o.values().filter_map(|v| v.as_u64()).max(),
                            _ => None,
                        };
                    }
                }
            }
        }
        // audio_mode.timeouts_ms.{OFF,A2L}
        if command == "AUDIO" {
            return self.raw["file_transfer"]["audio_mode"]["timeouts_ms"]
                .as_object()
                .and_then(|o| o.values().filter_map(|v| v.as_u64()).max());
        }
        None
    }

    /// Reference effect catalog shipped in protocol.json (superseded by the pedal's FXT).
    pub fn reference_effects(&self) -> Vec<EffectType> {
        self.raw["effects"]["table"]
            .as_array()
            .map(|a| {
                a.iter()
                    .filter_map(|e| serde_json::from_value::<EffectType>(e.clone()).ok())
                    .collect()
            })
            .unwrap_or_default()
    }

    pub fn chunk_size(&self, prefix: &str, total_len: usize, max_line: usize) -> usize {
        // floor((min(max_line,199) - len(PREFIX) - 2 - len(str(total_length))) / 2)
        let m = max_line.min(self.absolute_max_line);
        let overhead = prefix.len() + 2 + total_len.to_string().len();
        (m.saturating_sub(overhead)) / 2
    }

    /// Names of the global-setting commands persisted by `GSAVE` (GEQ, HPF, FSA, LEDCFG, ...),
    /// read from `commands.global_settings.settings_commands_saved_by_GSAVE`.
    pub fn global_setting_commands(&self) -> Vec<String> {
        self.raw["commands"]["global_settings"]["settings_commands_saved_by_GSAVE"]
            .as_object()
            .map(|o| o.keys().cloned().collect())
            .unwrap_or_default()
    }

    /// The effect that selects a stored IR (the one with an `ir_slot` parameter): its name and the
    /// index of that parameter. Stored as `slot + 1` (0 = shared preset IR).
    /// BLE transport settings: the Nordic UART service of `transport.alternative_transport`
    /// plus the host's own choices (`host_settings`).
    pub fn ble_config(&self) -> BleConfig {
        let alt = &self.raw["transport"]["alternative_transport"];
        let hs = &alt["host_settings"];
        let text = |v: &Value, d: &str| v.as_str().unwrap_or(d).to_ascii_lowercase();
        let list = |v: &Value| -> Vec<String> {
            v.as_array()
                .map(|a| {
                    a.iter()
                        .filter_map(|x| x.as_str().map(String::from))
                        .collect()
                })
                .unwrap_or_default()
        };
        let num = |v: &Value, d: u64| v.as_u64().unwrap_or(d);
        let mut name_filters = list(&hs["name_filters"]);
        if let Some(p) = alt["device_name_prefix"].as_str() {
            if !name_filters.iter().any(|n| n.eq_ignore_ascii_case(p)) {
                name_filters.push(p.into());
            }
        }
        BleConfig {
            service: text(&alt["service_uuid"], "6e400001-b5a3-f393-e0a9-e50e24dcca9e"),
            rx: text(
                &alt["rx_characteristic_host_writes"],
                "6e400002-b5a3-f393-e0a9-e50e24dcca9e",
            ),
            tx: text(
                &alt["tx_characteristic_pedal_notifies"],
                "6e400003-b5a3-f393-e0a9-e50e24dcca9e",
            ),
            pair: text(
                &alt["pairing_characteristic"],
                "6e400004-b5a3-f393-e0a9-e50e24dcca9e",
            ),
            extra_services: list(&hs["extra_service_uuids"])
                .into_iter()
                .map(|s| s.to_ascii_lowercase())
                .collect(),
            name_filters,
            scan: Duration::from_millis(num(&hs["scan_ms"], 5000)),
            connect_timeout: Duration::from_millis(num(&hs["connect_timeout_ms"], 15000)),
            pair_timeout: Duration::from_millis(num(&hs["pair_read_timeout_ms"], 30000)),
            chunk_default: num(&hs["chunk_default_bytes"], 20).max(1) as usize,
            chunk_max: num(&hs["chunk_max_bytes"], 244).max(20) as usize,
            chunk_delay: Duration::from_millis(num(&hs["chunk_delay_ms"], 12)),
        }
    }

    /// `commands.global_settings.host_ui`: ranges and footswitch quick setups for the settings modal.
    pub fn globals_ui(&self) -> Value {
        self.raw["commands"]["global_settings"]["host_ui"].clone()
    }

    pub fn ir_slot_param(&self) -> Option<(String, usize)> {
        self.reference_effects().into_iter().find_map(|e| {
            let p = e.parameters.iter().find(|p| p.name == "ir_slot")?;
            Some((e.name.clone(), usize::from(p.index)))
        })
    }

    pub fn ir_sample_limits(&self) -> (usize, usize) {
        // "N = 4076 for IR slots 0..6 ..., 2028 for slot 7" — documented in prose; the numbers are
        // read from there so a protocol revision flows through.
        let host = &self.raw["file_transfer"]["impulse_response_wav"]["host_preparation"];
        let text = host
            .as_array()
            .and_then(|a| a.first())
            .and_then(|s| s.as_str())
            .unwrap_or("");
        let nums: Vec<usize> = text
            .split(|c: char| !c.is_ascii_digit())
            .filter(|s| s.len() >= 3)
            .filter_map(|s| s.parse().ok())
            .collect();
        match nums.as_slice() {
            [a, b, ..] => (*a, *b),
            _ => (4076, 2028),
        }
    }
}

impl Default for Protocol {
    fn default() -> Self {
        Self::embedded()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_transport_from_json() {
        let p = Protocol::embedded();
        assert_eq!(p.usb_vendor_id, 0x3654);
        assert_eq!(p.baud_rate, 115200);
        assert_eq!(p.default_timeout, Duration::from_millis(1500));
        assert_eq!(p.ping_attempts, 5);
        assert_eq!(p.pong_timeout, Duration::from_millis(700));
    }

    #[test]
    fn line_limits_depend_on_firmware() {
        let p = Protocol::embedded();
        assert_eq!(p.max_line_chars(Some(526)), 190);
        assert_eq!(p.max_line_chars(Some(483)), 190);
        assert_eq!(p.max_line_chars(Some(482)), 31);
        assert_eq!(p.max_line_chars(None), 31);
    }

    #[test]
    fn timeouts_come_from_commands() {
        let p = Protocol::embedded();
        assert_eq!(p.timeout_for("FXT"), Duration::from_millis(3000));
        assert_eq!(p.timeout_for("FXS"), Duration::from_millis(2500));
        assert_eq!(p.timeout_for("FXA"), Duration::from_millis(4000));
        assert_eq!(p.timeout_for("PLOAD"), Duration::from_millis(8000));
        assert_eq!(p.timeout_for("FXP"), Duration::from_millis(1500));
        assert_eq!(p.timeout_for("LDEND"), Duration::from_millis(60000));
        assert_eq!(p.timeout_for("IREND"), Duration::from_millis(5000));
        assert_eq!(p.timeout_for("SOMETHING"), Duration::from_millis(1500));
    }

    #[test]
    fn chunk_size_matches_formula() {
        let p = Protocol::embedded();
        // (190 - 3 - 2 - 4) / 2 = 90
        assert_eq!(p.chunk_size("LDB", 1234, 190), 90);
    }

    #[test]
    fn global_commands_come_from_json() {
        let g = Protocol::embedded().global_setting_commands();
        for c in ["GEQ", "FSA", "LEDCFG", "HPF"] {
            assert!(g.iter().any(|x| x == c), "{c} in {g:?}");
        }
    }

    #[test]
    fn catalog_has_ir_block() {
        let p = Protocol::embedded();
        let fx = p.reference_effects();
        assert!(fx.len() >= 30);
        assert!(fx.iter().any(|e| e.name == "ir" && e.parameters.len() == 4));
        assert_eq!(p.ir_sample_limits(), (4076, 2028));
    }
}
