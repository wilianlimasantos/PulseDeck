//! `.nam` (NAM A2 "Lite" WaveNet, 3 channels) -> native `A2L1` blob.
//!
//! Pipeline (formulas documented in `protocol.json` -> `file_transfer.a2l1_quantization`):
//! 1. parse the NAM JSON, pick the Lite sub-model of a SlimmableContainer, reject what the
//!    pedal engine cannot run;
//! 2. split the flat `weights` vector per module (rechannel, per-layer conv/mixer/1x1, head);
//! 3. run the network in f64 over a calibration signal and record max |x| of every stage;
//! 4. derive fractional bits per stage (activations: int32, ranges x2) and per tensor
//!    (weights: int16, largest F that does not overflow), quantise, fold `head_scale` into the head;
//! 5. pack little-endian + CRC32 and re-validate with the firmware's shift rules.

use crate::nam::NamInfo;
use serde::{Deserialize, Serialize};
use serde_json::Value;

const MAGIC: &[u8; 4] = b"A2L1";
const VERSION: u16 = 1;
/// Fractional bits of the stored LeakyReLU slope (`lmul = round(slope * 2^24)`).
const LSHIFT: u32 = 24;
const FS: u32 = 48_000;
/// Safety margin applied to the ranges measured with the built-in synthetic signal (+3.5 dB).
const EMBEDDED_GUARD: f64 = 1.5;
/// Largest |shift| the engine accepts (`A2L_MAX_SHIFT`).
const MAX_SHIFT: i32 = 62;
const CH: usize = 3;
const WBITS: u32 = 16;
const ABITS: u32 = 32;

#[derive(Debug, Clone, PartialEq, thiserror::Error)]
#[error("{0}")]
pub struct ConvertError(pub String);

fn bad<T>(msg: impl Into<String>) -> Result<T, ConvertError> {
    Err(ConvertError(msg.into()))
}

#[derive(Debug, Clone, Default)]
pub struct ConvertOptions {
    /// Name stored in the blob header (truncated to 16 bytes). Default: `metadata.name`.
    pub name: Option<String>,
    /// Calibration segments; default is the built-in synthetic signal.
    pub signals: Option<Vec<Vec<f64>>>,
    /// Extra margin multiplied onto measured ranges (default 1.5 for the built-in signal, 1 otherwise).
    pub guard: Option<f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ConvertStats {
    pub layers: usize,
    pub channels: usize,
    pub macs: usize,
    pub head_k: usize,
    pub receptive_field: usize,
    pub sample_rate: u32,
    pub bytes: usize,
    pub calib_samples: usize,
}

#[derive(Debug, Clone)]
pub struct ConvertOutput {
    pub blob: Vec<u8>,
    pub name: String,
    pub stats: ConvertStats,
}

// ------------------------------------------------------------------------------------ parsing

fn describe_unsupported(d: &Value) -> Option<String> {
    let arch = d["architecture"].as_str().unwrap_or("");
    match arch {
        "WaveNet" => {
            let n = d["config"]["layers"]
                .as_array()
                .map(|a| a.len())
                .unwrap_or(0);
            (n == 2).then(|| {
                "Esta captura é NAM A1: baixe-a novamente (agora existe em A2).".to_string()
            })
        }
        "LSTM" => Some("Esta captura é NAM LSTM: baixe a versão A2.".into()),
        "ConvNet" | "Linear" | "Sequential" => Some(format!(
            "Modelo NAM \"{arch}\": a pedaleira só executa capturas NAM A2 (WaveNet)."
        )),
        _ => None,
    }
}

fn pick_submodel(d: &Value) -> Result<&Value, ConvertError> {
    if d["architecture"].as_str() == Some("SlimmableContainer") {
        let subs = d["config"]["submodels"].as_array();
        let Some(subs) = subs.filter(|s| !s.is_empty()) else {
            return bad("Este .nam não tem sub-modelos (SlimmableContainer inválido).");
        };
        let best = subs
            .iter()
            .min_by(|a, b| {
                a["max_value"]
                    .as_f64()
                    .unwrap_or(f64::MAX)
                    .total_cmp(&b["max_value"].as_f64().unwrap_or(f64::MAX))
            })
            .unwrap();
        return Ok(&best["model"]);
    }
    Ok(d)
}

fn num(v: &Value, key: &str, default: f64) -> f64 {
    v.get(key).and_then(|x| x.as_f64()).unwrap_or(default)
}

fn check_supported(sub: &Value) -> Result<(), ConvertError> {
    let mut errs: Vec<String> = Vec::new();
    let arch = sub["architecture"].as_str().unwrap_or("?");
    if arch != "WaveNet" {
        errs.push(format!("arquitetura {arch} (só WaveNet A2)"));
    }
    let cfg = &sub["config"];
    let layers = cfg["layers"].as_array().cloned().unwrap_or_default();
    if layers.len() != 1 {
        errs.push(format!("{} arrays de camadas (só 1)", layers.len()));
    }
    if !cfg["head"].is_null() {
        errs.push("head global (só o head_rechannel do array)".into());
    }
    if cfg
        .get("condition_dsp")
        .map(|v| !v.is_null() && v != &Value::Bool(false))
        .unwrap_or(false)
    {
        errs.push("condition_dsp".into());
    }
    if !errs.is_empty() {
        return bad(format!("Modelo não suportado: {}.", errs.join("; ")));
    }
    let l = &layers[0];
    let ks = l["kernel_sizes"].as_array();
    let ds = l["dilations"].as_array();
    let (Some(ks), Some(ds)) = (ks, ds) else {
        return bad("Modelo não suportado: faltam campos (kernel_sizes, dilations, channels, activation ou weights).");
    };
    if ks.len() != ds.len()
        || ks.is_empty()
        || l["channels"].is_null()
        || l["activation"].is_null()
        || !sub["weights"].is_array()
    {
        return bad("Modelo não suportado: faltam campos (kernel_sizes, dilations, channels, activation ou weights).");
    }
    let n = ks.len();
    if num(l, "input_size", 1.0) != 1.0 || num(l, "condition_size", 1.0) != 1.0 {
        errs.push("input/condition size != 1".into());
    }
    if l["head1x1"]["active"].as_bool().unwrap_or(false) {
        errs.push("head1x1 ativo".into());
    }
    if !l["layer1x1"]["active"].as_bool().unwrap_or(false)
        || num(&l["layer1x1"], "groups", 1.0) != 1.0
    {
        errs.push("layer1x1 inativa ou agrupada".into());
    }
    let channels = num(l, "channels", 0.0);
    if num(l, "bottleneck", channels) != channels {
        errs.push("bottleneck != channels".into());
    }
    for k in ["groups_input", "groups_input_mixin"] {
        if num(l, k, 1.0) != 1.0 {
            errs.push(format!("{k} != 1"));
        }
    }
    if let Some(obj) = l.as_object() {
        for (k, v) in obj {
            if k.ends_with("_film") && v.get("active").and_then(|a| a.as_bool()).unwrap_or(false) {
                errs.push(format!("FiLM ativo: {k}"));
            }
        }
    }
    let as_list = |v: &Value| -> Vec<Value> {
        match v {
            Value::Array(a) => a.clone(),
            other => vec![other.clone()],
        }
    };
    let gating = l
        .get("gating_mode")
        .cloned()
        .unwrap_or(Value::String("none".into()));
    if as_list(&gating)
        .iter()
        .any(|g| !g.is_null() && g.as_str() != Some("none"))
    {
        errs.push("gating".into());
    }
    let sec = l
        .get("secondary_activation")
        .cloned()
        .unwrap_or(Value::Null);
    if as_list(&sec).iter().any(|s| !s.is_null()) {
        errs.push("secondary_activation".into());
    }
    let acts = activations(l, n);
    if acts.iter().any(|a| a["type"].as_str() != Some("LeakyReLU")) {
        errs.push("ativações que não são LeakyReLU".into());
    }
    if num(&l["head"], "out_channels", 1.0) != 1.0 {
        errs.push("head com mais de 1 saída".into());
    }
    if !errs.is_empty() {
        return bad(format!("Modelo não suportado: {}.", errs.join("; ")));
    }
    if channels != CH as f64 {
        return bad(format!(
            "Esta rede tem {channels} canais; o motor da pedaleira executa a rede A2-Lite de {CH} canais."
        ));
    }
    let hk = num(&l["head"], "kernel_size", 0.0);
    if !(1.0..=64.0).contains(&hk) {
        return bad(format!("Modelo não suportado: kernel do head = {hk}."));
    }
    for i in 0..n {
        let k = ks[i].as_f64().unwrap_or(0.0);
        let d = ds[i].as_f64().unwrap_or(0.0);
        if !(1.0..=15.0).contains(&k) || !(1.0..=65535.0).contains(&d) {
            return bad(format!(
                "Modelo não suportado: kernel/dilatação fora do intervalo na camada {}.",
                i + 1
            ));
        }
    }
    Ok(())
}

fn activations(l: &Value, n: usize) -> Vec<Value> {
    match &l["activation"] {
        Value::Array(a) => a.clone(),
        v => vec![v.clone(); n],
    }
}

// ------------------------------------------------------------------------------------ engine

struct Layer {
    k: usize,
    dil: usize,
    slope: f64,
    /// `[(co*C + ci)*K + k]`
    conv_w: Vec<f64>,
    conv_b: Vec<f64>,
    mix_w: Vec<f64>,
    l1_w: Vec<f64>,
    l1_b: Vec<f64>,
}

struct Engine {
    rech: Vec<f64>,
    layers: Vec<Layer>,
    /// `[ci*hk + k]`
    head_w: Vec<f64>,
    head_b: f64,
    head_k: usize,
    head_scale: f64,
}

fn build_engine(sub: &Value) -> Result<Engine, ConvertError> {
    let l = &sub["config"]["layers"][0];
    let ks = l["kernel_sizes"].as_array().unwrap();
    let ds = l["dilations"].as_array().unwrap();
    let n = ks.len();
    let hk = num(&l["head"], "kernel_size", 0.0) as usize;
    let head_bias = l["head"]["bias"].as_bool().unwrap_or(true);
    // torch reads the list as float32
    let w: Vec<f64> = sub["weights"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v.as_f64().unwrap_or(f64::NAN) as f32 as f64)
        .collect();
    if w.iter().any(|v| !v.is_finite()) {
        return bad("Os pesos contêm NaN ou infinito.");
    }
    let mut need = CH;
    for k in ks {
        let k = k.as_u64().unwrap_or(0) as usize;
        need += CH * CH * k + CH + CH + CH * CH + CH;
    }
    need += CH * hk + usize::from(head_bias) + 1; // head + bias + trailing head_scale
    if w.len() != need {
        return bad(format!(
            "Número de pesos inesperado: o arquivo tem {}, uma rede A2-Lite com esta configuração precisa de {need}.",
            w.len()
        ));
    }
    let mut o = 0usize;
    let mut take = |k: usize| {
        let s = w[o..o + k].to_vec();
        o += k;
        s
    };
    let rech = take(CH);
    let acts = activations(l, n);
    let mut layers = Vec::with_capacity(n);
    for i in 0..n {
        let k = ks[i].as_u64().unwrap() as usize;
        layers.push(Layer {
            k,
            dil: ds[i].as_u64().unwrap() as usize,
            slope: acts[i]
                .get("negative_slope")
                .and_then(|v| v.as_f64())
                .unwrap_or(0.01),
            conv_w: take(CH * CH * k),
            conv_b: take(CH),
            mix_w: take(CH),
            l1_w: take(CH * CH),
            l1_b: take(CH),
        });
    }
    let head_w = take(CH * hk);
    let head_b = if head_bias { take(1)[0] } else { 0.0 };
    let head_scale = sub["config"]
        .get("head_scale")
        .and_then(|v| v.as_f64())
        .unwrap_or(1.0);
    Ok(Engine {
        rech,
        layers,
        head_w,
        head_b,
        head_k: hk,
        head_scale,
    })
}

fn receptive_field(e: &Engine) -> usize {
    1 + e.layers.iter().map(|l| (l.k - 1) * l.dil).sum::<usize>() + e.head_k - 1
}

#[derive(Debug, Clone, Default)]
struct Ranges {
    rech: f64,
    z: Vec<f64>,
    x3: Vec<f64>,
    head: f64,
    out: f64,
}

#[inline]
fn amax(v: &[f64]) -> f64 {
    v.iter().fold(0.0, |m, x| m.max(x.abs()))
}

/// Float forward pass; returns max |.| of every stage for input `x1` (preceded by rf-1 zeros).
fn measure(e: &Engine, x1: &[f64]) -> Ranges {
    let rf = receptive_field(e);
    let pad = rf - 1;
    let n = pad + x1.len();
    let mut xin = vec![0.0; n];
    xin[pad..].copy_from_slice(x1);

    let mut r = Ranges::default();
    let mut x3: Vec<Vec<f64>> = (0..CH)
        .map(|c| xin.iter().map(|v| e.rech[c] * v).collect())
        .collect();
    r.rech = x3.iter().map(|a| amax(a)).fold(0.0, f64::max);
    let mut cur = n;
    let mut head: Option<Vec<Vec<f64>>> = None;

    for ly in &e.layers {
        let (k, d) = (ly.k, ly.dil);
        let lout = cur - d * (k - 1);
        let off = cur - lout;
        let mut a: Vec<Vec<f64>> = Vec::with_capacity(CH);
        let mut zmax = 0.0f64;
        for co in 0..CH {
            let mut z = vec![ly.conv_b[co]; lout];
            for kk in 0..k {
                let sh = kk * d;
                let w0 = ly.conv_w[(co * CH) * k + kk];
                let w1 = ly.conv_w[(co * CH + 1) * k + kk];
                let w2 = ly.conv_w[(co * CH + 2) * k + kk];
                let (s0, s1, s2) = (
                    &x3[0][sh..sh + lout],
                    &x3[1][sh..sh + lout],
                    &x3[2][sh..sh + lout],
                );
                for t in 0..lout {
                    z[t] += w0 * s0[t] + w1 * s1[t] + w2 * s2[t];
                }
            }
            let mw = ly.mix_w[co];
            let xo = n - lout;
            let mut act = vec![0.0; lout];
            for t in 0..lout {
                let v = z[t] + mw * xin[xo + t];
                zmax = zmax.max(v.abs());
                act[t] = if v >= 0.0 { v } else { ly.slope * v };
            }
            a.push(act);
        }
        r.z.push(zmax);

        let mut nx: Vec<Vec<f64>> = Vec::with_capacity(CH);
        let mut xmax = 0.0f64;
        for co in 0..CH {
            let (w0, w1, w2) = (ly.l1_w[co * CH], ly.l1_w[co * CH + 1], ly.l1_w[co * CH + 2]);
            let b = ly.l1_b[co];
            let src = &x3[co];
            let mut o = vec![0.0; lout];
            for t in 0..lout {
                let v = src[off + t] + (w0 * a[0][t] + w1 * a[1][t] + w2 * a[2][t] + b);
                xmax = xmax.max(v.abs());
                o[t] = v;
            }
            nx.push(o);
        }
        r.x3.push(xmax);
        x3 = nx;

        if let Some(h) = &head {
            for c in 0..CH {
                let ho = h[c].len() - lout;
                for t in 0..lout {
                    a[c][t] += h[c][ho + t];
                }
            }
        }
        head = Some(a);
        cur = lout;
    }

    let head = head.expect("at least one layer");
    r.head = head.iter().map(|h| amax(h)).fold(0.0, f64::max);
    let hk = e.head_k;
    let lo = cur - hk + 1;
    let mut om = 0.0f64;
    for t in 0..lo {
        let mut y = e.head_b;
        for k in 0..hk {
            y += e.head_w[k] * head[0][t + k]
                + e.head_w[hk + k] * head[1][t + k]
                + e.head_w[2 * hk + k] * head[2][t + k];
        }
        y *= e.head_scale;
        om = om.max(y.abs());
    }
    r.out = om;
    r
}

fn merge(rs: &[Ranges]) -> Ranges {
    let mx = |f: &dyn Fn(&Ranges) -> f64| rs.iter().map(f).fold(0.0, f64::max);
    Ranges {
        rech: mx(&|r| r.rech),
        head: mx(&|r| r.head),
        out: mx(&|r| r.out),
        z: (0..rs[0].z.len()).map(|i| mx(&|r| r.z[i])).collect(),
        x3: (0..rs[0].x3.len()).map(|i| mx(&|r| r.x3[i])).collect(),
    }
}

// ------------------------------------------------------------------------ calibration signal

/// mulberry32 + Box-Muller: deterministic and portable.
struct Rng(u32);

impl Rng {
    fn u(&mut self) -> f64 {
        self.0 = self.0.wrapping_add(0x6D2B_79F5);
        let mut t = self.0;
        t = (t ^ (t >> 15)).wrapping_mul(t | 1);
        t ^= t.wrapping_add((t ^ (t >> 7)).wrapping_mul(t | 61));
        f64::from(t ^ (t >> 14)) / 4_294_967_296.0
    }
    fn g(&mut self) -> f64 {
        let a = self.u();
        let b = self.u();
        (-2.0 * (1.0 - a).ln()).sqrt() * (2.0 * std::f64::consts::PI * b).cos()
    }
}

fn sweep(fs: f64, secs: f64) -> Vec<f64> {
    let n = (secs * fs).floor() as usize;
    let t_total = (n as f64 - 1.0) / fs;
    let r = fs / 2.0 / 20.0;
    let lr = r.ln();
    (0..n)
        .map(|i| {
            let t = i as f64 / fs;
            (2.0 * std::f64::consts::PI * 20.0 * (r.powf(t / t_total) - 1.0) * t_total / lr).sin()
        })
        .collect()
}

/// Karplus-Strong plucked string.
fn pluck(fs: f64, f: f64, n: usize, rng: &mut Rng, decay: f64) -> Vec<f64> {
    let len = ((fs / f).round() as usize).max(2);
    let mut line: Vec<f64> = (0..len).map(|_| rng.u() * 2.0 - 1.0).collect();
    let mut y = vec![0.0; n];
    let mut p = 0usize;
    for out in y.iter_mut() {
        let a = line[p];
        let b = line[(p + 1) % len];
        *out = a;
        line[p] = decay * 0.5 * (a + b);
        p = (p + 1) % len;
    }
    y
}

fn strum(fs: f64, freqs: &[f64], secs: f64, peak: f64, rng: &mut Rng, decay: f64) -> Vec<f64> {
    let n = (secs * fs).floor() as usize;
    let mut y = vec![0.0; n];
    for (i, f) in freqs.iter().enumerate() {
        let off = (i as f64 * 0.012 * fs).floor() as usize;
        let s = pluck(fs, *f, n - off, rng, decay);
        for (t, v) in s.iter().enumerate() {
            y[off + t] += v;
        }
    }
    let m = amax(&y);
    if m > 0.0 {
        y.iter_mut().for_each(|v| *v *= peak / m);
    }
    y
}

/// Short synthetic calibration set: sweep, noise, hot guitar chords.
pub fn calibration_signals(fs: f64) -> Vec<Vec<f64>> {
    let mut rng = Rng(12345);
    let mut out = vec![sweep(fs, 2.0)];
    let n = (2.0 * fs).floor() as usize;
    out.push((0..n).map(|_| (rng.g() * 0.5).clamp(-1.0, 1.0)).collect());
    let e = [82.41, 123.47, 164.81, 207.65, 246.94, 329.63];
    let a = [110.0, 164.81, 220.0, 277.18, 329.63, 440.0];
    let d = [73.42, 110.0, 146.83, 220.0, 293.66, 369.99];
    out.push(strum(fs, &e, 2.0, 0.99, &mut rng, 0.9985));
    out.push(strum(fs, &a, 1.5, 0.99, &mut rng, 0.9985));
    out.push(strum(fs, &d, 1.5, 0.99, &mut rng, 0.9985));
    out
}

// -------------------------------------------------------------------------- quantisation

/// Fractional bits so that `maxabs` fits in a signed `bits`-bit integer.
fn fbits(maxabs: f64, bits: u32) -> i32 {
    if maxabs <= 0.0 {
        return bits as i32 - 1;
    }
    let ib = maxabs.log2().floor() as i32 + 1;
    bits as i32 - 1 - ib
}

fn q_i32(v: f64, f: i32, bits: u32) -> i64 {
    let lim = 1i64 << (bits - 1);
    let q = (v * 2f64.powi(f)).round_ties_even();
    (q.clamp(-(lim as f64), (lim - 1) as f64)) as i64
}

fn q_i64(v: f64, f: i32) -> i64 {
    // f64 -> i64 `as` saturates, which is the clamp we want
    (v * 2f64.powi(f)).round_ties_even() as i64
}

/// int16 weights with per-tensor F (largest that does not overflow, optionally capped).
fn qweight(w: &[f64], fmax: i32) -> (i32, Vec<i16>) {
    let f = fbits(amax(w), WBITS).min(fmax);
    (f, w.iter().map(|v| q_i32(*v, f, WBITS) as i16).collect())
}

struct QLayer {
    k: usize,
    dil: usize,
    fz: i32,
    fx3: i32,
    lmul: i32,
    cf: i32,
    cw: Vec<i16>,
    cb: Vec<i64>,
    mf: i32,
    mw: Vec<i16>,
    lf: i32,
    lw: Vec<i16>,
    lb: Vec<i64>,
}

struct QModel {
    name: String,
    fs: u32,
    head_k: usize,
    fin: i32,
    fx3_0: i32,
    fh: i32,
    fout: i32,
    rech_f: i32,
    rech_w: Vec<i16>,
    layers: Vec<QLayer>,
    hf: i32,
    hw: Vec<i16>,
    hb: i64,
}

fn quantize(e: &Engine, r: &Ranges, fs: u32, name: &str) -> QModel {
    let fin = fbits(1.0, ABITS);
    let fx3_0 = fbits(r.rech * 2.0, ABITS);
    let mut fh = fbits(r.head * 2.0, ABITS);
    let fout = fbits(r.out * 2.0, ABITS);
    let (rech_f, rech_w) = qweight(&e.rech, fx3_0 - fin + 31);
    let mut layers = Vec::new();
    let mut fx3_prev = fx3_0;
    for (i, ly) in e.layers.iter().enumerate() {
        let fz = fbits(r.z[i] * 2.0, ABITS);
        let fx3 = fbits(r.x3[i] * 2.0, ABITS);
        let (cf, cw) = qweight(&ly.conv_w, fz - fx3_prev + 31);
        let (mf, mw) = qweight(&ly.mix_w, fz - fin + 31);
        let (lf, lw) = qweight(&ly.l1_w, fx3 - fz + 31);
        layers.push(QLayer {
            k: ly.k,
            dil: ly.dil,
            fz,
            fx3,
            lmul: (ly.slope * f64::from(1u32 << LSHIFT)).round() as i32,
            cf,
            cw,
            cb: ly.conv_b.iter().map(|v| q_i64(*v, cf + fx3_prev)).collect(),
            mf,
            mw,
            lf,
            lw,
            lb: ly.l1_b.iter().map(|v| q_i64(*v, lf + fz)).collect(),
        });
        fx3_prev = fx3;
    }
    // the head-sum shift (Fz - Fh) must stay >= 1 in every layer
    fh = fh.min(layers.iter().map(|l| l.fz).min().unwrap_or(0) - 1);
    let hw_scaled: Vec<f64> = e.head_w.iter().map(|v| v * e.head_scale).collect();
    let (hf, hw) = qweight(&hw_scaled, fout - fh + 31);
    let hb = q_i64(e.head_b * e.head_scale, hf + fh);
    QModel {
        name: name.to_string(),
        fs,
        head_k: e.head_k,
        fin,
        fx3_0,
        fh,
        fout,
        rech_f,
        rech_w,
        layers,
        hf,
        hw,
        hb,
    }
}

// ------------------------------------------------------------------------------- packing

fn i8_checked(v: i32, what: &str) -> Result<u8, ConvertError> {
    i8::try_from(v)
        .map(|x| x as u8)
        .map_err(|_| ConvertError(format!("Este modelo precisa de {v} bits fracionários em {what} (limite ±127): pesos/níveis extremos para a pedaleira.")))
}

pub fn blob_size(c: usize, head_k: usize, layer_ks: &[usize]) -> usize {
    let mut size = 36 + 1 + 2 * c + 4 + 1 + 2 * c * head_k + 8;
    for k in layer_ks {
        size += 9 + 1 + 2 * c * c * k + 8 * c + 1 + 2 * c + 1 + 2 * c * c + 8 * c;
    }
    size
}

fn pack(m: &QModel) -> Result<Vec<u8>, ConvertError> {
    let ks: Vec<usize> = m.layers.iter().map(|l| l.k).collect();
    let mut b: Vec<u8> = Vec::with_capacity(blob_size(CH, m.head_k, &ks));
    b.extend_from_slice(MAGIC);
    b.extend_from_slice(&VERSION.to_le_bytes());
    b.push(WBITS as u8);
    b.push(ABITS as u8);
    b.extend_from_slice(&m.fs.to_le_bytes());
    b.extend_from_slice(&(m.layers.len() as u16).to_le_bytes());
    b.push(CH as u8);
    b.push(m.head_k as u8);
    b.push(i8_checked(m.fin, "Fin")?);
    b.push(i8_checked(m.fx3_0, "Fx3_0")?);
    b.push(i8_checked(m.fh, "Fh")?);
    b.push(i8_checked(m.fout, "Fout")?);
    let mut name = [0u8; 16];
    let mut end = m.name.len().min(16);
    while !m.name.is_char_boundary(end) {
        end -= 1;
    }
    name[..end].copy_from_slice(&m.name.as_bytes()[..end]);
    b.extend_from_slice(&name);

    let h =
        |b: &mut Vec<u8>, a: &[i16]| a.iter().for_each(|v| b.extend_from_slice(&v.to_le_bytes()));
    let q =
        |b: &mut Vec<u8>, a: &[i64]| a.iter().for_each(|v| b.extend_from_slice(&v.to_le_bytes()));

    b.push(i8_checked(m.rech_f, "rechannel")?);
    h(&mut b, &m.rech_w);
    for (i, l) in m.layers.iter().enumerate() {
        let at = |s: &str| format!("{s} (camada {})", i + 1);
        b.push(l.k as u8);
        b.extend_from_slice(&(l.dil as u16).to_le_bytes());
        b.push(i8_checked(l.fz, &at("Fz"))?);
        b.push(i8_checked(l.fx3, &at("Fx3"))?);
        b.extend_from_slice(&l.lmul.to_le_bytes());
        b.push(i8_checked(l.cf, &at("cF"))?);
        h(&mut b, &l.cw);
        q(&mut b, &l.cb);
        b.push(i8_checked(l.mf, &at("mF"))?);
        h(&mut b, &l.mw);
        b.push(i8_checked(l.lf, &at("lF"))?);
        h(&mut b, &l.lw);
        q(&mut b, &l.lb);
    }
    b.push(i8_checked(m.hf, "head")?);
    h(&mut b, &m.hw);
    q(&mut b, &[m.hb]);
    let crc = crc32fast::hash(&b);
    b.extend_from_slice(&crc.to_le_bytes());
    debug_assert_eq!(b.len(), blob_size(CH, m.head_k, &ks));
    Ok(b)
}

/// Re-read the blob and apply the firmware's shift rules so a blob that would be rejected
/// (`LOAD rc=-4`) never leaves the host.
pub fn validate_blob(u: &[u8]) -> Result<(), ConvertError> {
    let n = u.len();
    if n < 44 {
        return bad("interno: blob curto demais.");
    }
    if &u[..4] != MAGIC {
        return bad("interno: magic inválido.");
    }
    let crc = u32::from_le_bytes(u[n - 4..].try_into().unwrap());
    if crc32fast::hash(&u[..n - 4]) != crc {
        return bad("interno: CRC não confere.");
    }
    let i8at = |o: usize| u[o] as i8 as i32;
    let wb = u[6];
    let ab = u[7];
    let nl = u16::from_le_bytes([u[12], u[13]]) as usize;
    let c = u[14] as usize;
    let hk = u[15] as usize;
    let (fin, fx30, fh, fout) = (i8at(16), i8at(17), i8at(18), i8at(19));
    if wb != 16 || (ab != 16 && ab != 32) || c == 0 || c > 16 || hk == 0 || hk > 64 || nl == 0 {
        return bad("interno: cabeçalho inválido.");
    }
    let sh = |s: i32, what: &str| -> Result<(), ConvertError> {
        if s.abs() > MAX_SHIFT {
            bad(format!("Este modelo precisa de um deslocamento de {s} bits em {what} (o motor aceita ±{MAX_SHIFT}): pesos/níveis extremos para a pedaleira."))
        } else {
            Ok(())
        }
    };
    let mut o = 36usize;
    let need = |o: usize, len: usize| -> Result<(), ConvertError> {
        if o + len > n {
            bad("interno: layout do blob não confere.")
        } else {
            Ok(())
        }
    };
    need(o, 1 + 2 * c)?;
    let rf = i8at(o);
    o += 1 + 2 * c;
    sh(rf + fin - fx30, "o rechannel de entrada")?;
    let mut fprev = fx30;
    for i in 0..nl {
        need(o, 9)?;
        let k = u[o] as usize;
        let fz = i8at(o + 3);
        let fx3 = i8at(o + 4);
        o += 9;
        need(o, 1)?;
        let cf = i8at(o);
        o += 1 + 2 * c * c * k + 8 * c;
        need(o, 1)?;
        let mf = i8at(o);
        o += 1 + 2 * c;
        need(o, 1)?;
        let lf = i8at(o);
        o += 1 + 2 * c * c + 8 * c;
        let at = format!(" (camada {})", i + 1);
        sh(cf + fprev - fz, &format!("conv{at}"))?;
        sh(mf + fin - fz, &format!("mixer{at}"))?;
        sh(lf + fz - fx3, &format!("1x1{at}"))?;
        sh(fprev - fx3, &format!("residual{at}"))?;
        sh(fz - fh, &format!("soma do head{at}"))?;
        let cs = cf + fprev - fz;
        if !(0..=31).contains(&cs) {
            return bad(format!("Este modelo precisa de um deslocamento de conv de {cs} bits{at} (o motor aceita 0..31)."));
        }
        fprev = fx3;
    }
    need(o, 1)?;
    let hf = i8at(o);
    o += 1 + 2 * c * hk + 8;
    let so = hf + fh - fout;
    if !(0..=31).contains(&so) {
        return bad(format!(
            "Este modelo precisa de um deslocamento de saída de {so} bits (o motor aceita 0..31)."
        ));
    }
    if o + 4 != n {
        return bad("interno: layout do blob não confere.");
    }
    Ok(())
}

// ------------------------------------------------------------------------------- public API

pub fn convert_str(text: &str, opts: &ConvertOptions) -> Result<ConvertOutput, ConvertError> {
    let d: Value = serde_json::from_str(text).map_err(|_| {
        ConvertError("Este arquivo não é um JSON válido, então não é uma captura .nam.".into())
    })?;
    if !d.is_object() || d["architecture"].is_null() || d["config"].is_null() {
        return bad("Isto não parece uma captura NAM (sem \"architecture\"/\"config\").");
    }
    if let Some(msg) = describe_unsupported(&d) {
        return bad(msg);
    }
    let sub = pick_submodel(&d)?;
    check_supported(sub)?;
    let sr = sub["sample_rate"]
        .as_f64()
        .or(d["sample_rate"].as_f64())
        .unwrap_or(f64::from(FS));
    if sr != f64::from(FS) {
        return bad(format!(
            "Esta captura é de {sr} Hz; a pedaleira roda a 48 kHz. Baixe a versão 48 kHz."
        ));
    }
    let e = build_engine(sub)?;
    let macs = CH
        + e.layers
            .iter()
            .map(|l| l.conv_w.len() + l.mix_w.len() + l.l1_w.len())
            .sum::<usize>()
        + e.head_w.len();

    let signals = opts
        .signals
        .clone()
        .unwrap_or_else(|| calibration_signals(f64::from(FS)));
    let calib_samples: usize = signals.iter().map(|s| s.len()).sum();
    let measured: Vec<Ranges> = signals.iter().map(|s| measure(&e, s)).collect();
    let mut ranges = merge(&measured);
    let guard = opts.guard.unwrap_or(if opts.signals.is_some() {
        1.0
    } else {
        EMBEDDED_GUARD
    });
    if guard != 1.0 {
        ranges.rech *= guard;
        ranges.head *= guard;
        ranges.out *= guard;
        ranges.z.iter_mut().for_each(|v| *v *= guard);
        ranges.x3.iter_mut().for_each(|v| *v *= guard);
    }
    let name = opts
        .name
        .clone()
        .or_else(|| {
            d["metadata"]["name"]
                .as_str()
                .filter(|s| !s.is_empty())
                .map(String::from)
        })
        .unwrap_or_else(|| "NAM".into());
    let model = quantize(&e, &ranges, FS, &name);
    let blob = pack(&model)?;
    validate_blob(&blob)?;
    Ok(ConvertOutput {
        stats: ConvertStats {
            layers: e.layers.len(),
            channels: CH,
            macs,
            head_k: e.head_k,
            receptive_field: receptive_field(&e),
            sample_rate: FS,
            bytes: blob.len(),
            calib_samples,
        },
        blob,
        name,
    })
}

/// Convenience used by the uploader: reads the file, converts, returns the NAM info too.
pub fn convert_file(
    path: &std::path::Path,
    opts: &ConvertOptions,
) -> Result<(ConvertOutput, NamInfo), ConvertError> {
    let text = std::fs::read_to_string(path)
        .map_err(|e| ConvertError(format!("Não foi possível ler o arquivo: {e}")))?;
    let info = crate::nam::info_from_str(&text).map_err(ConvertError)?;
    Ok((convert_str(&text, opts)?, info))
}

#[cfg(test)]
pub mod testutil {
    use serde_json::json;

    /// Deterministic A2-Lite (3 channels) NAM JSON with small random-ish weights.
    pub fn synthetic_nam(
        dilations: &[usize],
        kernel: usize,
        head_k: usize,
        seed: u32,
        extra_meta: serde_json::Value,
    ) -> String {
        let c = 3usize;
        let mut s = seed;
        let mut rnd = move || {
            s = s.wrapping_mul(1664525).wrapping_add(1013904223);
            ((s >> 8) as f64 / 16777216.0) * 2.0 - 1.0
        };
        let mut w: Vec<f64> = Vec::new();
        w.extend((0..c).map(|_| rnd() * 0.8));
        for _ in dilations {
            w.extend((0..c * c * kernel).map(|_| rnd() * 0.4));
            w.extend((0..c).map(|_| rnd() * 0.05));
            w.extend((0..c).map(|_| rnd() * 0.3));
            w.extend((0..c * c).map(|_| rnd() * 0.4));
            w.extend((0..c).map(|_| rnd() * 0.05));
        }
        w.extend((0..c * head_k).map(|_| rnd() * 0.5));
        w.push(rnd() * 0.02);
        w.push(1.0);
        let n = dilations.len();
        json!({
            "version": "0.6.0",
            "architecture": "WaveNet",
            "config": {
                "layers": [{
                    "input_size": 1, "condition_size": 1, "channels": c, "bottleneck": c,
                    "kernel_sizes": vec![kernel; n], "dilations": dilations,
                    "activation": {"type": "LeakyReLU", "negative_slope": 0.01},
                    "head": {"kernel_size": head_k, "out_channels": 1, "bias": true},
                    "layer1x1": {"active": true, "groups": 1}, "head1x1": {"active": false},
                    "gating_mode": "none"
                }],
                "head_scale": 0.9
            },
            "weights": w,
            "sample_rate": 48000,
            "metadata": extra_meta
        })
        .to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::testutil::*;
    use super::*;
    use serde_json::json;

    #[test]
    fn rng_is_deterministic_and_in_range() {
        let mut a = Rng(12345);
        let mut b = Rng(12345);
        for _ in 0..100 {
            let (x, y) = (a.u(), b.u());
            assert_eq!(x, y);
            assert!((0.0..1.0).contains(&x));
        }
    }

    #[test]
    fn fbits_matches_definition() {
        assert_eq!(fbits(1.0, 32), 30); // 1.0 needs 1 integer bit
        assert_eq!(fbits(0.4, 16), 16); // floor(log2 .4) = -2 -> ib = -1 -> 15 - (-1)
        assert_eq!(fbits(0.0, 16), 15);
        assert_eq!(fbits(3.9, 16), 13);
    }

    #[test]
    fn quant_rounds_half_even_and_clamps() {
        assert_eq!(q_i32(0.5, 0, 16), 0);
        assert_eq!(q_i32(1.5, 0, 16), 2);
        assert_eq!(q_i32(2.5, 0, 16), 2);
        assert_eq!(q_i32(1e9, 0, 16), 32767);
        assert_eq!(q_i32(-1e9, 0, 16), -32768);
        assert_eq!(q_i64(1e30, 0), i64::MAX);
    }

    #[test]
    fn converts_synthetic_capture() {
        let t = synthetic_nam(
            &[1, 2, 4, 8, 16, 32, 64, 128],
            3,
            4,
            7,
            json!({"name": "Test Amp", "gear_type": "amp"}),
        );
        let out = convert_str(&t, &ConvertOptions::default()).expect("convert");
        let b = &out.blob;
        assert_eq!(&b[..4], b"A2L1");
        assert_eq!(u16::from_le_bytes([b[4], b[5]]), 1);
        assert_eq!(b[6], 16);
        assert_eq!(b[7], 32);
        assert_eq!(u32::from_le_bytes(b[8..12].try_into().unwrap()), 48000);
        assert_eq!(u16::from_le_bytes([b[12], b[13]]), 8);
        assert_eq!(b[14], 3);
        assert_eq!(b[15], 4);
        assert_eq!(&b[20..28], b"Test Amp");
        assert_eq!(b.len(), blob_size(3, 4, &[3; 8]));
        let crc = u32::from_le_bytes(b[b.len() - 4..].try_into().unwrap());
        assert_eq!(crc, crc32fast::hash(&b[..b.len() - 4]));
        assert_eq!(out.stats.layers, 8);
        assert_eq!(
            out.stats.receptive_field,
            1 + 2 * (1 + 2 + 4 + 8 + 16 + 32 + 64 + 128) + 3
        );
        validate_blob(b).unwrap();
        // deterministic
        assert_eq!(
            convert_str(&t, &ConvertOptions::default()).unwrap().blob,
            out.blob
        );
    }

    #[test]
    fn quantised_weights_reproduce_float_weights() {
        let t = synthetic_nam(&[1, 2, 4], 3, 2, 3, json!({}));
        let out = convert_str(&t, &ConvertOptions::default()).unwrap();
        let b = &out.blob;
        // rechannel: F then 3 x i16
        let f = b[36] as i8 as i32;
        let d: Value = serde_json::from_str(&t).unwrap();
        for c in 0..3 {
            let q = i16::from_le_bytes([b[37 + 2 * c], b[38 + 2 * c]]) as f64;
            let w = d["weights"][c].as_f64().unwrap() as f32 as f64;
            assert!((q / 2f64.powi(f) - w).abs() < 2f64.powi(-f), "c={c}");
        }
    }

    #[test]
    fn slimmable_container_picks_lite() {
        let lite: Value =
            serde_json::from_str(&synthetic_nam(&[1, 2], 3, 2, 1, json!({}))).unwrap();
        let mut big = lite.clone();
        big["config"]["layers"][0]["channels"] = json!(8);
        let container = json!({
            "architecture": "SlimmableContainer",
            "config": {"submodels": [{"max_value": 1.0, "model": big}, {"max_value": 0.3, "model": lite}]},
            "metadata": {"name": "Slim"}
        });
        let out = convert_str(&container.to_string(), &ConvertOptions::default()).unwrap();
        assert_eq!(out.name, "Slim");
        assert_eq!(out.blob[14], 3);
    }

    #[test]
    fn rejects_unsupported_models() {
        let e = |s: &str| convert_str(s, &ConvertOptions::default()).unwrap_err().0;
        assert!(e("nope").contains("JSON"));
        assert!(e("{}").contains("NAM"));
        assert!(e(r#"{"architecture":"LSTM","config":{}}"#).contains("LSTM"));
        // wrong channel count
        let mut v: Value = serde_json::from_str(&synthetic_nam(&[1], 3, 2, 1, json!({}))).unwrap();
        v["config"]["layers"][0]["channels"] = json!(4);
        v["config"]["layers"][0]["bottleneck"] = json!(4);
        assert!(e(&v.to_string()).contains("canais"));
        // wrong weight count
        let mut v: Value = serde_json::from_str(&synthetic_nam(&[1], 3, 2, 1, json!({}))).unwrap();
        v["weights"].as_array_mut().unwrap().pop();
        assert!(e(&v.to_string()).contains("pesos"));
        // wrong sample rate
        let mut v: Value = serde_json::from_str(&synthetic_nam(&[1], 3, 2, 1, json!({}))).unwrap();
        v["sample_rate"] = json!(44100);
        assert!(e(&v.to_string()).contains("48 kHz"));
        // non-LeakyReLU
        let mut v: Value = serde_json::from_str(&synthetic_nam(&[1], 3, 2, 1, json!({}))).unwrap();
        v["config"]["layers"][0]["activation"] = json!({"type": "Tanh"});
        assert!(e(&v.to_string()).contains("LeakyReLU"));
    }

    #[test]
    fn validate_catches_corruption() {
        let t = synthetic_nam(&[1, 2], 3, 2, 9, json!({}));
        let mut blob = convert_str(&t, &ConvertOptions::default()).unwrap().blob;
        let n = blob.len();
        blob[n / 2] ^= 0xFF;
        assert!(validate_blob(&blob).unwrap_err().0.contains("CRC"));
    }

    #[test]
    fn calibration_signal_shape() {
        let s = calibration_signals(48000.0);
        assert_eq!(s.len(), 5);
        assert_eq!(s[0].len(), 96000);
        assert_eq!(s[2].len(), 96000);
        assert_eq!(s[3].len(), 72000);
        assert!((amax(&s[2]) - 0.99).abs() < 1e-9);
        assert!(amax(&s[0]) <= 1.0 + 1e-12);
    }
}
