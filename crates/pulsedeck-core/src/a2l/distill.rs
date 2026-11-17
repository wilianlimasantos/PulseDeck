//! Model compaction ("distillation") for pedals whose capture player is smaller than a stock NAM A2 Lite network.
//!
//! A stock A2 Lite capture (3 channels, ~23 layers with kernels of 6..15 taps) needs ~1700 multiply-accumulates per sample. The open Tank-G
//! firmware reports its limit with `CAPS` (a2l_macs). When a capture is heavier, a SMALLER network of the same family (3 channels, kernel 3,
//! dilations 1..512) is trained here to imitate the original ("teacher") on synthetic guitar-like signals, and that student is what gets
//! quantised to A2L1 and uploaded. This is ordinary knowledge distillation by gradient descent (Adam, mean-squared error), written from
//! scratch for this network shape; nothing is taken from another converter.
//!
//! It is an approximation: the result is reported with its error-to-signal ratio (ESR) against the teacher so the caller can show it.
use super::{calibration_signals, receptive_field, Engine, Layer, Rng, CH};

/// Student shapes, lightest first: (dilations, kernel, head kernel). MACs = layers * (9k + 12) + 3 * head_k.
const SHAPES: &[(&[usize], usize, usize)] = &[
    (&[1, 4, 16], 3, 4),                                                              // 129 MACs (the open Tank-G firmware as measured on 2026-11-15)
    (&[1, 2, 4, 8, 16, 32], 3, 8),                                                    // 258 MACs (pedals that measured a low limit)
    (&[1, 2, 4, 8, 16, 32, 64, 128], 3, 8),                                           // 336 MACs
    (&[1, 2, 4, 8, 16, 32, 64, 128, 256, 512], 3, 16),                               // 438 MACs, 1532 B
    (&[1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1, 2, 4, 8, 16, 32], 3, 16),           // 672 MACs, 2360 B
];
pub(super) fn macs_of(n_layers: usize, k: usize, head_k: usize) -> usize { n_layers * (CH * CH * k + CH + CH * CH) + CH * head_k }
pub(super) fn engine_macs(e: &Engine) -> usize { e.layers.iter().map(|l| l.conv_w.len() + l.mix_w.len() + l.l1_w.len()).sum::<usize>() + e.head_w.len() }

/// The student shape for a budget: the heaviest shape that fits (best imitation the pedal can still run in real time); with
/// `prefer_quality` false and more than one fitting, the 438-MAC default if it fits, else the heaviest. `None` if nothing fits.
pub(super) fn pick_shape(max_macs: usize, max_bytes: usize, prefer_quality: bool) -> Option<(&'static [usize], usize, usize)> {
    let fits: Vec<_> = SHAPES.iter().filter(|(d, k, hk)| macs_of(d.len(), *k, *hk) <= max_macs && super::blob_size(CH, *hk, &vec![*k; d.len()]) <= max_bytes).collect();
    if prefer_quality { return fits.last().map(|s| **s); }
    fits.iter().rev().find(|(d, k, hk)| macs_of(d.len(), *k, *hk) <= 438).or(fits.last()).map(|s| **s)
}

// ---------------------------------------------------------------------------------------- float32 network (forward + backward)
struct Net {
    dil: Vec<usize>, k: usize, hk: usize, slope: f32,
    /// flat parameters: rech[C] | per layer: conv[C*C*K] ((co*C+ci)*K+k), conv_b[C], mix[C], l1[C*C], l1_b[C] | head[C*hk] (c*hk+k), head_b
    p: Vec<f32>,
}
impl Net {
    fn layer_size(&self) -> usize { CH * CH * self.k + CH + CH + CH * CH + CH }
    fn off_layer(&self, i: usize) -> usize { CH + i * self.layer_size() }
    fn off_head(&self) -> usize { CH + self.dil.len() * self.layer_size() }
    fn to_engine(&self, out_scale: f64) -> Engine {
        let p = &self.p; let f = |a: usize, n: usize| p[a..a + n].iter().map(|v| f64::from(*v)).collect::<Vec<f64>>();
        let mut layers = Vec::new();
        for (i, d) in self.dil.iter().enumerate() {
            let mut o = self.off_layer(i);
            let conv_w = f(o, CH * CH * self.k); o += CH * CH * self.k;
            let conv_b = f(o, CH); o += CH; let mix_w = f(o, CH); o += CH; let l1_w = f(o, CH * CH); o += CH * CH; let l1_b = f(o, CH);
            layers.push(Layer { k: self.k, dil: *d, slope: f64::from(self.slope), conv_w, conv_b, mix_w, l1_w, l1_b });
        }
        let oh = self.off_head();
        Engine { rech: f(0, CH), layers, head_w: f(oh, CH * self.hk), head_b: f64::from(p[oh + CH * self.hk]), head_k: self.hk, head_scale: out_scale }
    }
}
/// Any `Engine` (teacher or student) over a signal that starts from silence. f32, array form.
pub(super) fn forward_engine(e: &Engine, x: &[f32]) -> Vec<f32> {
    let n = x.len();
    let mut x3: Vec<Vec<f32>> = (0..CH).map(|c| x.iter().map(|v| e.rech[c] as f32 * v).collect()).collect();
    let mut head = vec![vec![0f32; n]; CH];
    for ly in &e.layers {
        let (k, d, slope) = (ly.k, ly.dil, ly.slope as f32);
        let mut a = vec![vec![0f32; n]; CH];
        for co in 0..CH {
            let z = &mut a[co];
            z.iter_mut().zip(x.iter()).for_each(|(z, xv)| *z = ly.conv_b[co] as f32 + ly.mix_w[co] as f32 * xv);
            for ci in 0..CH { for kk in 0..k {
                let w = ly.conv_w[(co * CH + ci) * k + kk] as f32; let off = (k - 1 - kk) * d; if off >= n { continue; }
                let src = &x3[ci][..n - off];
                z[off..].iter_mut().zip(src.iter()).for_each(|(z, s)| *z += w * s);
            } }
            z.iter_mut().for_each(|v| if *v < 0.0 { *v *= slope });
        }
        for co in 0..CH {
            let (w0, w1, w2, b) = (ly.l1_w[co * CH] as f32, ly.l1_w[co * CH + 1] as f32, ly.l1_w[co * CH + 2] as f32, ly.l1_b[co] as f32);
            for t in 0..n { x3[co][t] += w0 * a[0][t] + w1 * a[1][t] + w2 * a[2][t] + b; }
        }
        for c in 0..CH { head[c].iter_mut().zip(a[c].iter()).for_each(|(h, v)| *h += v); }
    }
    let hk = e.head_k; let mut y = vec![e.head_b as f32; n];
    for c in 0..CH { for kk in 0..hk {
        let w = e.head_w[c * hk + kk] as f32; let off = hk - 1 - kk; if off >= n { continue; }
        y[off..].iter_mut().zip(head[c][..n - off].iter()).for_each(|(y, h)| *y += w * h);
    } }
    let hs = e.head_scale as f32; y.iter_mut().for_each(|v| *v *= hs);
    y
}
/// Loss (sum of squared error over `t >= warm`) and its gradient for one segment. Returns (sum sq err, gradient).
fn grad_segment(net: &Net, x: &[f32], tgt: &[f32], warm: usize) -> (f64, Vec<f32>) {
    let n = x.len(); let (k, hk, slope) = (net.k, net.hk, net.slope); let p = &net.p; let nl = net.dil.len();
    // ---- forward, keeping every layer's input and pre-activation sign
    let mut xin: Vec<Vec<Vec<f32>>> = Vec::with_capacity(nl);   // [layer][c][t] : input of the layer
    let mut act: Vec<Vec<Vec<f32>>> = Vec::with_capacity(nl);   // [layer][c][t] : activation (sign tells the slope)
    let mut x3: Vec<Vec<f32>> = (0..CH).map(|c| x.iter().map(|v| p[c] * v).collect()).collect();
    let mut head = vec![vec![0f32; n]; CH];
    for (i, &d) in net.dil.iter().enumerate() {
        let o = net.off_layer(i); let (cw, cb, mw, lw, lb) = (o, o + CH * CH * k, o + CH * CH * k + CH, o + CH * CH * k + 2 * CH, o + CH * CH * k + 2 * CH + CH * CH);
        let mut a = vec![vec![0f32; n]; CH];
        for co in 0..CH {
            let z = &mut a[co];
            z.iter_mut().zip(x.iter()).for_each(|(z, xv)| *z = p[cb + co] + p[mw + co] * xv);
            for ci in 0..CH { for kk in 0..k {
                let w = p[cw + (co * CH + ci) * k + kk]; let off = (k - 1 - kk) * d; if off >= n { continue; }
                z[off..].iter_mut().zip(x3[ci][..n - off].iter()).for_each(|(z, s)| *z += w * s);
            } }
            z.iter_mut().for_each(|v| if *v < 0.0 { *v *= slope });
        }
        let mut nx = x3.clone();
        for co in 0..CH {
            let (w0, w1, w2, b) = (p[lw + co * CH], p[lw + co * CH + 1], p[lw + co * CH + 2], p[lb + co]);
            for t in 0..n { nx[co][t] += w0 * a[0][t] + w1 * a[1][t] + w2 * a[2][t] + b; }
        }
        for c in 0..CH { head[c].iter_mut().zip(a[c].iter()).for_each(|(h, v)| *h += v); }
        xin.push(std::mem::replace(&mut x3, nx)); act.push(a);
    }
    let oh = net.off_head(); let mut y = vec![p[oh + CH * hk]; n];
    for c in 0..CH { for kk in 0..hk {
        let w = p[oh + c * hk + kk]; let off = hk - 1 - kk; if off >= n { continue; }
        y[off..].iter_mut().zip(head[c][..n - off].iter()).for_each(|(y, h)| *y += w * h);
    } }
    // ---- backward
    let mut g = vec![0f32; p.len()]; let mut sse = 0f64;
    let mut gy = vec![0f32; n];
    for t in warm..n { let e = y[t] - tgt[t]; sse += f64::from(e) * f64::from(e); gy[t] = 2.0 * e; }
    g[oh + CH * hk] = gy.iter().sum();
    let mut ghead = vec![vec![0f32; n]; CH];
    for c in 0..CH { for kk in 0..hk {
        let off = hk - 1 - kk; if off >= n { continue; }
        g[oh + c * hk + kk] = gy[off..].iter().zip(head[c][..n - off].iter()).map(|(a, b)| a * b).sum();
        let w = p[oh + c * hk + kk];
        ghead[c][..n - off].iter_mut().zip(gy[off..].iter()).for_each(|(gh, gyv)| *gh += w * gyv);
    } }
    let mut gx3 = vec![vec![0f32; n]; CH];   // gradient w.r.t. the output of the current layer (nothing reads the last layer's residual output)
    for i in (0..nl).rev() {
        let d = net.dil[i]; let o = net.off_layer(i); let (cw, cb, mw, lw, lb) = (o, o + CH * CH * k, o + CH * CH * k + CH, o + CH * CH * k + 2 * CH, o + CH * CH * k + 2 * CH + CH * CH);
        let a = &act[i]; let xi = &xin[i];
        // through the 1x1: x3n = x3 + l1 * a + b
        let mut gz = vec![vec![0f32; n]; CH];
        for ci in 0..CH {
            let (w0, w1, w2) = (p[lw + ci], p[lw + CH + ci], p[lw + 2 * CH + ci]);
            for t in 0..n { let ga = ghead[ci][t] + w0 * gx3[0][t] + w1 * gx3[1][t] + w2 * gx3[2][t]; gz[ci][t] = if a[ci][t] < 0.0 { ga * slope } else { ga }; }
        }
        for co in 0..CH {
            g[lb + co] = gx3[co].iter().sum();
            for ci in 0..CH { g[lw + co * CH + ci] = gx3[co].iter().zip(a[ci].iter()).map(|(u, v)| u * v).sum(); }
            g[cb + co] = gz[co].iter().sum();
            g[mw + co] = gz[co].iter().zip(x.iter()).map(|(u, v)| u * v).sum();
        }
        // through the dilated convolution (gx3 already holds the residual path)
        for co in 0..CH { for ci in 0..CH { for kk in 0..k {
            let off = (k - 1 - kk) * d; if off >= n { continue; }
            let idx = cw + (co * CH + ci) * k + kk; let w = p[idx];
            g[idx] = gz[co][off..].iter().zip(xi[ci][..n - off].iter()).map(|(u, v)| u * v).sum();
            gx3[ci][..n - off].iter_mut().zip(gz[co][off..].iter()).for_each(|(gx, gzv)| *gx += w * gzv);
        } } }
    }
    for c in 0..CH { g[c] = gx3[c].iter().zip(x.iter()).map(|(u, v)| u * v).sum(); }
    (sse, g)
}

pub(super) struct Distilled { pub engine: Engine, pub esr: f64, pub steps: usize }

/// Training signals: the calibration set (sweep, noise, hot chords) plus the same chords and noise at lower playing levels.
fn training_signals() -> Vec<Vec<f32>> {
    let base = calibration_signals(48_000.0); let mut out: Vec<Vec<f32>> = Vec::new();
    for (i, s) in base.iter().enumerate() {
        out.push(s.iter().map(|v| *v as f32).collect());
        if i >= 1 { for lvl in [0.35f32, 0.12, 0.04] { out.push(s.iter().map(|v| *v as f32 * lvl).collect()); } }
    }
    out
}
fn esr_of(e: &Engine, sig: &[Vec<f32>], tgt: &[Vec<f32>]) -> f64 {
    let (mut se, mut ss) = (0f64, 0f64);
    for (x, t) in sig.iter().zip(tgt.iter()) { let y = forward_engine(e, x); for (a, b) in y.iter().zip(t.iter()) { se += f64::from(a - b).powi(2); ss += f64::from(*b).powi(2); } }
    se / ss.max(1e-30)
}

/// Trains a student of the given shape to reproduce `teacher`. `steps` Adam steps of `batch` random segments; `progress(step, steps, esr)`.
pub(super) fn distill(teacher: &Engine, dil: &[usize], k: usize, hk: usize, steps: usize, progress: &dyn Fn(usize, usize, f64)) -> Distilled {
    let sig = training_signals();
    let tgt_raw: Vec<Vec<f32>> = std::thread::scope(|sc| { let hs: Vec<_> = sig.iter().map(|x| sc.spawn(move || forward_engine(teacher, x))).collect(); hs.into_iter().map(|h| h.join().unwrap()).collect() });
    // train on targets normalised to unit RMS; the scale goes back into head_scale at the end
    let (mut ss, mut cnt) = (0f64, 0usize); for t in &tgt_raw { for v in t { ss += f64::from(*v).powi(2); } cnt += t.len(); }
    let scale = (ss / cnt.max(1) as f64).sqrt().max(1e-9);
    let tgt: Vec<Vec<f32>> = tgt_raw.iter().map(|t| t.iter().map(|v| (f64::from(*v) / scale) as f32).collect()).collect();
    let slope = teacher.layers.first().map(|l| l.slope as f32).unwrap_or(0.01);
    let mut net = Net { dil: dil.to_vec(), k, hk, slope, p: Vec::new() };
    let np = net.off_head() + CH * hk + 1; let mut rng = Rng(0x5EED_1234);
    net.p = vec![0f32; np];
    for c in 0..CH { net.p[c] = (rng.g() * 0.5) as f32 + if c == 0 { 1.0 } else { 0.0 }; }
    for i in 0..dil.len() {
        let o = net.off_layer(i); let nc = CH * CH * k; let sd = (1.0 / (CH * k) as f64).sqrt();
        for j in 0..nc { net.p[o + j] = (rng.g() * sd) as f32; }
        for j in 0..CH { net.p[o + nc + CH + j] = (rng.g() * 0.3) as f32; }                    // mixer
        for j in 0..CH * CH { net.p[o + nc + 2 * CH + j] = (rng.g() * 0.25) as f32; }           // 1x1
    }
    let oh = net.off_head(); for j in 0..CH * hk { net.p[oh + j] = (rng.g() * 0.3 / ((CH * hk) as f64).sqrt()) as f32; }
    let rf = { let e = net.to_engine(1.0); receptive_field(&e) };
    let seg = 4096usize; let warm = rf.min(3000); let batch = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4).clamp(2, 16);
    let (mut m1, mut m2) = (vec![0f32; np], vec![0f32; np]); let (b1, b2, eps) = (0.9f32, 0.999f32, 1e-8f32);
    let mut best = (f64::INFINITY, net.p.clone()); let mut run = 0f64;
    for step in 0..steps {
        let lr = 6e-3f32 * (0.02f32).powf(step as f32 / steps.max(1) as f32);   // 6e-3 -> 1.2e-4
        let picks: Vec<(usize, usize)> = (0..batch).map(|_| { let s = (rng.u() * sig.len() as f64) as usize % sig.len(); let max0 = sig[s].len().saturating_sub(seg + warm); ((s), (rng.u() * (max0 + 1) as f64) as usize) }).collect();
        let netr = &net; let (sigr, tgtr) = (&sig, &tgt);
        let parts: Vec<(f64, Vec<f32>)> = std::thread::scope(|sc| {
            let hs: Vec<_> = picks.iter().map(|&(s, a)| sc.spawn(move || { let b = (a + seg + warm).min(sigr[s].len()); let w = if a == 0 { 0 } else { warm.min(b - a) }; grad_segment(netr, &sigr[s][a..b], &tgtr[s][a..b], w) })).collect();
            hs.into_iter().map(|h| h.join().unwrap()).collect()
        });
        let mut g = vec![0f32; np]; let mut sse = 0f64;
        for (e, gp) in &parts { sse += e; g.iter_mut().zip(gp.iter()).for_each(|(a, b)| *a += b); }
        let norm = 1.0 / (batch * seg) as f32; g.iter_mut().for_each(|v| *v *= norm);
        let gn = g.iter().map(|v| v * v).sum::<f32>().sqrt(); if gn > 5.0 { let s = 5.0 / gn; g.iter_mut().for_each(|v| *v *= s); }   // clip
        let t = (step + 1) as i32; let (c1, c2) = (1.0 - b1.powi(t), 1.0 - b2.powi(t));
        for j in 0..np { m1[j] = b1 * m1[j] + (1.0 - b1) * g[j]; m2[j] = b2 * m2[j] + (1.0 - b2) * g[j] * g[j]; net.p[j] -= lr * (m1[j] / c1) / ((m2[j] / c2).sqrt() + eps); }
        let cur = sse / (batch * seg) as f64;   // targets have unit RMS: this is roughly the ESR of the batch
        run = if step == 0 { cur } else { 0.98 * run + 0.02 * cur };
        if !net.p.iter().all(|v| v.is_finite()) { net.p = best.1.clone(); m1.iter_mut().for_each(|v| *v = 0.0); m2.iter_mut().for_each(|v| *v = 0.0); continue; }
        if step > steps / 10 && step % 50 == 0 && run < best.0 { best = (run, net.p.clone()); }
        if step % 25 == 0 { progress(step, steps, run); }
    }
    // keep whichever is better on the whole set: the last weights or the best running snapshot
    let last = net.to_engine(scale); let e_last = esr_of(&last, &sig, &tgt_raw);
    net.p = best.1; let snap = net.to_engine(scale); let e_snap = if best.0.is_finite() { esr_of(&snap, &sig, &tgt_raw) } else { f64::INFINITY };
    progress(steps, steps, e_last.min(e_snap));
    if e_snap < e_last { Distilled { engine: snap, esr: e_snap, steps } } else { Distilled { engine: last, esr: e_last, steps } }
}

#[cfg(test)]
mod tests {
    use super::*;
    /// The analytic gradient must match finite differences (this is what makes the training trustworthy). With slope 1 the network is smooth
    /// and the match is tight; with a LeakyReLU a finite step can cross a kink, so that case gets a looser bound.
    #[test]
    fn gradient_matches_finite_differences() {
        for (slope, tol) in [(1.0f32, 0.01f64), (0.3, 0.10)] {
            let mut rng = Rng(7); let dil = [1usize, 2, 5]; let (k, hk) = (3usize, 4usize);
            let mut net = Net { dil: dil.to_vec(), k, hk, slope, p: Vec::new() };
            let np = net.off_head() + CH * hk + 1; net.p = (0..np).map(|_| (rng.g() * 0.4) as f32).collect();
            let x: Vec<f32> = (0..160).map(|_| (rng.g() * 0.5) as f32).collect(); let t: Vec<f32> = (0..160).map(|_| (rng.g() * 0.5) as f32).collect();
            let (_, g) = grad_segment(&net, &x, &t, 20);
            let loss = |n: &Net| -> f64 { let mut e = n.to_engine(1.0); e.layers.iter_mut().for_each(|l| l.slope = f64::from(slope)); let y = forward_engine(&e, &x); (20..160).map(|i| f64::from(y[i] - t[i]).powi(2)).sum() };
            let mut worst = 0f64;
            for j in 0..np {
                let h = 1e-3f32; let keep = net.p[j]; net.p[j] = keep + h; let lp = loss(&net); net.p[j] = keep - h; let lm = loss(&net); net.p[j] = keep;
                let fd = (lp - lm) / (2.0 * f64::from(h)); let an = f64::from(g[j]);
                worst = worst.max((fd - an).abs() / (fd.abs() + an.abs() + 1e-2));
            }
            assert!(worst < tol, "gradient check failed at slope {slope}: worst relative error {worst}");
        }
    }
    /// A small teacher is learnt almost exactly by a student of the same shape, and the result goes through the normal A2L1 pipeline.
    #[test]
    fn distils_a_small_teacher() {
        let text = super::super::testutil::synthetic_nam(&[1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1, 2, 4, 8, 16, 32], 6, 16, 3, serde_json::json!({"name": "Big"}));
        let plain = super::super::convert_str(&text, &super::super::ConvertOptions::default()).unwrap();
        assert!(!plain.stats.compacted && plain.stats.macs == 16 * 66 + 48);
        let opts = super::super::ConvertOptions { budget: Some(super::super::Budget { max_macs: 700, max_bytes: 4096 }), distill_steps: Some(400), ..Default::default() };
        let out = super::super::convert_str(&text, &opts).unwrap();
        assert!(out.stats.compacted && out.stats.source_macs == 16 * 66 + 48 && out.stats.macs == 438 && out.stats.layers == 10 && out.blob.len() == 1532);
        let esr = out.stats.esr.unwrap(); assert!(esr < 0.5, "the student must at least clearly follow the teacher after a short training (ESR {esr})");
        super::super::validate_blob(&out.blob).unwrap();
        // a capture that already fits is left alone
        let small = super::super::testutil::synthetic_nam(&[1, 2, 4, 8], 3, 8, 1, serde_json::json!({}));
        let o2 = super::super::convert_str(&small, &opts).unwrap(); assert!(!o2.stats.compacted && o2.stats.esr.is_none());
        // nothing fits a tiny budget
        let tiny = super::super::ConvertOptions { budget: Some(super::super::Budget { max_macs: 100, max_bytes: 4096 }), ..Default::default() };
        assert!(super::super::convert_str(&text, &tiny).is_err());
    }
}
