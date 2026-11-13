//! `cargo run -p pulsedeck-core --example convert -- in.nam out.a2l` — offline converter.
use pulsedeck_core::a2l::{convert_file, ConvertOptions};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: convert <in.nam> <out.a2l>");
        std::process::exit(2);
    }
    match convert_file(std::path::Path::new(&args[1]), &ConvertOptions::default()) {
        Ok((out, info)) => {
            std::fs::write(&args[2], &out.blob).expect("write");
            println!(
                "{} bytes, {:?}, {:?}",
                out.blob.len(),
                out.stats,
                info.class
            );
        }
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1);
        }
    }
}
