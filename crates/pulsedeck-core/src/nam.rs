//! Lightweight `.nam` inspection: classify captures (FULL_RIG vs AMP_ONLY) and read the input
//! level without loading the (potentially large) weight arrays into memory.

use serde::{Deserialize, Serialize};
use std::fs::File;
use std::io::BufReader;
use std::path::Path;

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
pub enum GearClass {
    /// Capture includes the cabinet / whole signal chain: no IR needed afterwards.
    FullRig,
    /// Amp (or preamp) head only: expects an IR / cab simulation after it.
    AmpOnly,
    /// Pedal, unknown or missing `gear_type`.
    Other,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct NamInfo {
    pub name: Option<String>,
    pub gear_type: Option<String>,
    pub class: GearClass,
    /// `metadata.input_level_dbu` (or `input_level`) when it is a finite number.
    pub input_level_dbu: Option<f64>,
    pub architecture: Option<String>,
    pub sample_rate: Option<f64>,
    pub gear_make: Option<String>,
    pub gear_model: Option<String>,
    pub tone_type: Option<String>,
    pub modeled_by: Option<String>,
}

/// Classify `metadata.gear_type`.
///
/// NAM's own vocabulary is `amp`, `pedal`, `pedal_amp`, `amp_cab`, `amp_pedal_cab`, `preamp`,
/// `studio`. A capture that already contains the cabinet (`*_cab`, `studio`, `full_rig`) is a
/// FULL_RIG; amp/preamp/pedal+amp without a cab is AMP_ONLY.
pub fn classify_gear_type(gear_type: Option<&str>) -> GearClass {
    let Some(g) = gear_type else {
        return GearClass::Other;
    };
    let norm = g.trim().to_ascii_lowercase().replace(['-', ' '], "_");
    let tokens: Vec<&str> = norm.split('_').collect();
    let has = |t: &str| tokens.contains(&t);
    if has("cab") || has("studio") || has("rig") || has("full") || has("cabinet") {
        GearClass::FullRig
    } else if has("amp") || has("preamp") || has("head") {
        GearClass::AmpOnly
    } else {
        GearClass::Other
    }
}

#[derive(Deserialize, Default)]
struct Meta {
    name: Option<String>,
    gear_type: Option<String>,
    gear_make: Option<String>,
    gear_model: Option<String>,
    tone_type: Option<String>,
    modeled_by: Option<String>,
    input_level_dbu: Option<f64>,
    input_level: Option<f64>,
}

#[derive(Deserialize, Default)]
struct Head {
    architecture: Option<String>,
    sample_rate: Option<f64>,
    metadata: Option<Meta>,
}

pub fn info_from_str(text: &str) -> Result<NamInfo, String> {
    let head: Head =
        serde_json::from_str(text).map_err(|e| format!("not a valid .nam (JSON): {e}"))?;
    Ok(build_info(head))
}

pub fn read_nam_info(path: &Path) -> Result<NamInfo, String> {
    let f = File::open(path).map_err(|e| e.to_string())?;
    let head: Head = serde_json::from_reader(BufReader::new(f))
        .map_err(|e| format!("not a valid .nam (JSON): {e}"))?;
    Ok(build_info(head))
}

fn build_info(head: Head) -> NamInfo {
    let m = head.metadata.unwrap_or_default();
    let level = m
        .input_level_dbu
        .or(m.input_level)
        .filter(|v| v.is_finite());
    NamInfo {
        class: classify_gear_type(m.gear_type.as_deref()),
        name: m.name,
        gear_type: m.gear_type,
        input_level_dbu: level,
        architecture: head.architecture,
        sample_rate: head.sample_rate,
        gear_make: m.gear_make,
        gear_model: m.gear_model,
        tone_type: m.tone_type,
        modeled_by: m.modeled_by,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn classification() {
        use GearClass::*;
        assert_eq!(classify_gear_type(Some("amp")), AmpOnly);
        assert_eq!(classify_gear_type(Some("preamp")), AmpOnly);
        assert_eq!(classify_gear_type(Some("pedal_amp")), AmpOnly);
        assert_eq!(classify_gear_type(Some("amp_cab")), FullRig);
        assert_eq!(classify_gear_type(Some("amp_pedal_cab")), FullRig);
        assert_eq!(classify_gear_type(Some("studio")), FullRig);
        assert_eq!(classify_gear_type(Some("FULL_RIG")), FullRig);
        assert_eq!(classify_gear_type(Some("AMP_ONLY")), AmpOnly);
        assert_eq!(classify_gear_type(Some("pedal")), Other);
        assert_eq!(classify_gear_type(None), Other);
    }

    #[test]
    fn reads_metadata_only() {
        let t = r#"{"version":"0.5.4","architecture":"WaveNet","config":{"layers":[]},"weights":[0.1,0.2],
            "sample_rate":48000,"metadata":{"name":"Plexi","gear_type":"amp_cab","input_level_dbu":12.5,"gear_make":"Marshall"}}"#;
        let i = info_from_str(t).unwrap();
        assert_eq!(i.class, GearClass::FullRig);
        assert_eq!(i.input_level_dbu, Some(12.5));
        assert_eq!(i.name.as_deref(), Some("Plexi"));
        assert_eq!(i.sample_rate, Some(48000.0));
        assert!(info_from_str("nope").is_err());
        assert_eq!(info_from_str("{}").unwrap().class, GearClass::Other);
    }
}
