//! The converter must reproduce, byte for byte, blobs produced by the reference implementation
//! (fixtures were generated once with the original browser converter on synthetic A2-Lite models).
use pulsedeck_core::a2l::{convert_str, validate_blob, ConvertOptions};

fn check(name: &str) {
    let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/fixtures/");
    let nam = std::fs::read_to_string(format!("{dir}{name}.nam")).unwrap();
    let want = std::fs::read(format!("{dir}{name}.ref.a2l")).unwrap();
    let got = convert_str(&nam, &ConvertOptions::default()).unwrap().blob;
    validate_blob(&got).unwrap();
    assert_eq!(got.len(), want.len());
    assert!(got == want, "{name}: blob differs from reference");
}

#[test]
fn matches_reference_model_a() {
    check("a");
}

#[test]
fn matches_reference_model_b() {
    check("b");
}
