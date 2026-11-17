//! Convert a .nam capture to the pedal's A2L1 blob from the command line, with the same code the editor uses.
//!   cargo run -p pulsedeck-core --example nam2a2l -- <in.nam> <out.a2l> [--macs 700] [--bytes 4096] [--steps 3000] [--quality]
//! Without --macs the Lite network of the file is quantised as it is (stock firmware). With --macs a capture that is heavier is compacted.
use pulsedeck_core::a2l::{convert_file, Budget, ConvertOptions, DistillProgress};
use std::sync::Arc;
fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() < 2 { eprintln!("usage: nam2a2l <in.nam> <out.a2l> [--macs N] [--bytes N] [--steps N] [--quality]"); std::process::exit(2); }
    let val = |k: &str| a.iter().position(|x| x == k).and_then(|i| a.get(i + 1)).and_then(|v| v.parse::<usize>().ok());
    let mut o = ConvertOptions::default();
    if let Some(m) = val("--macs") { o.budget = Some(Budget { max_macs: m, max_bytes: val("--bytes").unwrap_or(4096) }); }
    o.distill_steps = val("--steps"); o.prefer_quality = a.iter().any(|x| x == "--quality");
    o.progress = Some(DistillProgress(Arc::new(|s, n, e| if s % 250 == 0 || s == n { eprintln!("  compacting: step {s}/{n}, error {e:.4}"); })));
    let t0 = std::time::Instant::now();
    match convert_file(std::path::Path::new(&a[0]), &o) {
        Ok((out, info)) => {
            std::fs::write(&a[1], &out.blob).expect("write output");
            println!("name='{}' class={:?} layers={} macs={} (file: {}) bytes={} compacted={} esr={:?} time={:.1}s", out.name, info.class, out.stats.layers, out.stats.macs, out.stats.source_macs, out.stats.bytes, out.stats.compacted, out.stats.esr, t0.elapsed().as_secs_f64());
        }
        Err(e) => { eprintln!("error: {e}"); std::process::exit(1); }
    }
}
