//! `.wav` -> impulse-response payload (mono, 48 kHz, int16) following
//! `file_transfer.impulse_response_wav.host_preparation`.

use std::path::Path;

pub const TARGET_RATE: u32 = 48_000;

#[derive(Debug, Clone, PartialEq, thiserror::Error)]
#[error("{0}")]
pub struct IrError(pub String);

#[derive(Debug, Clone)]
pub struct IrPayload {
    pub samples: Vec<i16>,
    /// Integer milli-dB the pedal re-applies.
    pub gain_mdb: i32,
}

impl IrPayload {
    /// int16 little-endian byte stream (what is hex-encoded on the wire).
    pub fn to_bytes(&self) -> Vec<u8> {
        self.samples.iter().flat_map(|s| s.to_le_bytes()).collect()
    }
}

/// Decode the first channel of a WAV file as f64 in [-1, 1]. Returns (samples, sample_rate).
pub fn read_wav_first_channel(path: &Path) -> Result<(Vec<f64>, u32), IrError> {
    let mut r = hound::WavReader::open(path).map_err(|e| IrError(format!("WAV inválido: {e}")))?;
    let spec = r.spec();
    let ch = spec.channels.max(1) as usize;
    let samples: Vec<f64> = match spec.sample_format {
        hound::SampleFormat::Float => r
            .samples::<f32>()
            .step_by(ch)
            .map(|s| s.map(f64::from))
            .collect::<Result<_, _>>()
            .map_err(|e| IrError(e.to_string()))?,
        hound::SampleFormat::Int => {
            let scale = 2f64.powi(i32::from(spec.bits_per_sample) - 1);
            r.samples::<i32>()
                .step_by(ch)
                .map(|s| s.map(|v| f64::from(v) / scale))
                .collect::<Result<_, _>>()
                .map_err(|e| IrError(e.to_string()))?
        }
    };
    if samples.is_empty() {
        return Err(IrError("O WAV está vazio.".into()));
    }
    Ok((samples, spec.sample_rate))
}

/// Windowed-sinc resampler producing at most `max_out` output samples (low-pass when downsampling).
pub fn resample(x: &[f64], from: u32, to: u32, max_out: usize) -> Vec<f64> {
    if from == to {
        return x.iter().copied().take(max_out).collect();
    }
    let ratio = f64::from(from) / f64::from(to); // source samples per output sample
    let total_out = ((x.len() as f64) / ratio).floor() as usize;
    let n_out = total_out.min(max_out);
    let fc = (1.0 / ratio).min(1.0); // cutoff relative to source Nyquist
    let hw = 16.0 / fc; // half width in source samples
    let pi = std::f64::consts::PI;
    (0..n_out)
        .map(|i| {
            let c = i as f64 * ratio;
            let lo = (c - hw).ceil().max(0.0) as usize;
            let hi = ((c + hw).floor() as usize).min(x.len() - 1);
            let mut acc = 0.0;
            for (j, xj) in x.iter().enumerate().take(hi + 1).skip(lo) {
                let d = j as f64 - c;
                let a = pi * fc * d;
                let sinc = if a.abs() < 1e-12 { 1.0 } else { a.sin() / a };
                let win = 0.5 * (1.0 + (pi * d / hw).cos()); // Hann
                acc += xj * sinc * fc * win;
            }
            acc
        })
        .collect()
}

/// Steps 1..6 of the host preparation for `n` samples.
pub fn prepare_ir(mono48: &[f64], n: usize) -> Result<IrPayload, IrError> {
    let mut x: Vec<f64> = mono48.iter().copied().take(n).collect();
    if x.is_empty() {
        return Err(IrError("IR sem amostras.".into()));
    }
    // raised-cosine fade-out over the last min(N/8, 256) samples
    let len = x.len();
    let fade = (len / 8).min(256);
    for k in 0..fade {
        let g = 0.5 * (1.0 + (std::f64::consts::PI * (k + 1) as f64 / fade as f64).cos());
        x[len - fade + k] *= g;
    }
    // unit energy
    let energy: f64 = x.iter().map(|v| v * v).sum::<f64>().sqrt();
    if energy <= 0.0 || !energy.is_finite() {
        return Err(IrError(
            "O IR é silencioso (todas as amostras são zero).".into(),
        ));
    }
    x.iter_mut().for_each(|v| *v /= energy);
    let peak = x.iter().fold(0.0f64, |m, v| m.max(v.abs()));
    let mut q: Vec<i16> = x
        .iter()
        .map(|v| (v / peak * 32767.0).round() as i16)
        .collect();
    while q.last() == Some(&0) {
        q.pop();
    }
    if q.is_empty() {
        return Err(IrError("O IR é silencioso.".into()));
    }
    let gain_mdb = (20000.0 * (peak * 32768.0 / 32767.0).log10()).round() as i32;
    Ok(IrPayload {
        samples: q,
        gain_mdb,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn prepares_unit_peak_and_gain() {
        let mut x = vec![0.0; 1000];
        x[0] = 0.5;
        x[10] = -0.25;
        let p = prepare_ir(&x, 4076).unwrap();
        assert_eq!(p.samples.len(), 11); // trailing zeros stripped
        assert_eq!(p.samples[0], 32767);
        // energy normalised: peak = 0.5/sqrt(.3125)
        let peak = 0.5 / (0.25f64 + 0.0625).sqrt();
        let expected = (20000.0 * (peak * 32768.0 / 32767.0).log10()).round() as i32;
        assert_eq!(p.gain_mdb, expected);
        assert_eq!(p.to_bytes().len(), 22);
    }

    #[test]
    fn rejects_silence_and_truncates() {
        assert!(prepare_ir(&[0.0; 100], 4076).is_err());
        let x: Vec<f64> = (0..10_000).map(|i| ((i as f64) * 0.3).sin()).collect();
        let p = prepare_ir(&x, 2028).unwrap();
        assert!(p.samples.len() <= 2028);
        // fade makes the tail small
        let last = p.samples[p.samples.len() - 1].abs();
        assert!(last < 2000, "tail {last}");
    }

    #[test]
    fn resampler_preserves_a_sine() {
        let fs_in = 44_100u32;
        let f = 1000.0;
        let x: Vec<f64> = (0..8000)
            .map(|i| (2.0 * std::f64::consts::PI * f * i as f64 / f64::from(fs_in)).sin())
            .collect();
        let y = resample(&x, fs_in, 48_000, 4000);
        assert_eq!(y.len(), 4000);
        // compare away from the edges
        for (i, got) in y.iter().enumerate().take(3500).skip(500) {
            let want = (2.0 * std::f64::consts::PI * f * i as f64 / 48_000.0).sin();
            assert!((got - want).abs() < 5e-3, "i={i} {got} vs {want}");
        }
        let z = resample(&x, 96_000, 48_000, 1000);
        assert_eq!(z.len(), 1000);
        assert_eq!(resample(&x, 48_000, 48_000, 10).len(), 10);
    }

    #[test]
    fn reads_wav_first_channel_and_resamples() {
        let dir = std::env::temp_dir().join(format!("pd-ir-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("t.wav");
        let spec = hound::WavSpec {
            channels: 2,
            sample_rate: 44100,
            bits_per_sample: 16,
            sample_format: hound::SampleFormat::Int,
        };
        let mut w = hound::WavWriter::create(&p, spec).unwrap();
        for i in 0..2000 {
            w.write_sample(if i == 0 { 16384i16 } else { 0 }).unwrap(); // L
            w.write_sample(1000i16).unwrap(); // R, ignored
        }
        w.finalize().unwrap();
        let (x, sr) = read_wav_first_channel(&p).unwrap();
        assert_eq!(sr, 44100);
        assert_eq!(x.len(), 2000);
        assert!((x[0] - 0.5).abs() < 1e-9);
        assert_eq!(x[1], 0.0);
        std::fs::remove_dir_all(&dir).ok();
    }
}
