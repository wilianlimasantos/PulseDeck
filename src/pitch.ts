/** Pitch detection (YIN) and note maths for the system-audio tuner. Pure functions, no DOM. */

export interface Pitch {
  /** fundamental in Hz */
  freq: number;
  /** 0..1, higher = cleaner periodicity */
  probability: number;
}

export interface NoteReading {
  /** "A", "A#", ... (sharps only) */
  name: string;
  octave: number;
  hz: number;
  /** distance from the nearest note in cents, positive = sharp */
  cents: number;
}

export interface YinOptions {
  /** absolute threshold on the normalised difference (default 0.15) */
  threshold?: number;
  /** lowest / highest fundamental searched (defaults cover a 5-string bass B0 to a high guitar E6) */
  minFreq?: number;
  maxFreq?: number;
}

/**
 * YIN (de Cheveigné & Kawahara, 2002): difference function, cumulative mean normalisation,
 * absolute threshold, parabolic interpolation. `buf` needs at least `window + sampleRate/minFreq`
 * samples (4096 at 48 kHz is enough for 50 Hz). Returns null when nothing periodic is found.
 */
export function yin(buf: Float32Array, sampleRate: number, opts: YinOptions = {}): Pitch | null {
  const threshold = opts.threshold ?? 0.15;
  const minFreq = opts.minFreq ?? 30;
  const maxFreq = opts.maxFreq ?? 1400;
  const tauMin = Math.max(2, Math.floor(sampleRate / maxFreq));
  const tauMax = Math.min(Math.ceil(sampleRate / minFreq), buf.length - 2);
  const w = Math.min(2048, buf.length - tauMax);
  if (w < 64 || tauMax <= tauMin + 2) return null;

  // difference function
  const d = new Float32Array(tauMax + 1);
  for (let tau = 1; tau <= tauMax; tau++) {
    let sum = 0;
    for (let j = 0; j < w; j++) {
      const diff = buf[j] - buf[j + tau];
      sum += diff * diff;
    }
    d[tau] = sum;
  }
  // cumulative mean normalised difference
  const cm = new Float32Array(tauMax + 1);
  cm[0] = 1;
  let running = 0;
  for (let tau = 1; tau <= tauMax; tau++) {
    running += d[tau];
    cm[tau] = running === 0 ? 1 : (d[tau] * tau) / running;
  }
  // first dip below the threshold, followed down to its local minimum
  let tau = -1;
  for (let t = tauMin; t < tauMax; t++) {
    if (cm[t] < threshold) {
      while (t + 1 < tauMax && cm[t + 1] < cm[t]) t++;
      tau = t;
      break;
    }
  }
  if (tau < 0) return null;

  // parabolic interpolation around the minimum
  let better = tau;
  if (tau > 1 && tau < tauMax) {
    const a = cm[tau - 1], b = cm[tau], c = cm[tau + 1];
    const denom = 2 * (2 * b - c - a);
    if (denom !== 0) better = tau + (c - a) / denom;
  }
  return { freq: sampleRate / better, probability: 1 - cm[tau] };
}

const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

/** Nearest note (equal temperament) for `freq`, with `a4` as the reference for A4. */
export function freqToNote(freq: number, a4 = 440): NoteReading | null {
  if (!(freq > 0) || !(a4 > 0)) return null;
  const midi = 69 + 12 * Math.log2(freq / a4);
  const nearest = Math.round(midi);
  return {
    name: NAMES[((nearest % 12) + 12) % 12],
    octave: Math.floor(nearest / 12) - 1,
    hz: freq,
    cents: (midi - nearest) * 100,
  };
}

/** Frequency of a note name + octave for the reference `a4`. */
export function noteToFreq(name: string, octave: number, a4 = 440): number {
  const i = NAMES.indexOf(name);
  return a4 * Math.pow(2, (i - 9 + 12 * (octave - 4)) / 12);
}

export const median = (xs: number[]): number => {
  const s = [...xs].sort((a, b) => a - b);
  const m = s.length >> 1;
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
};

export const IN_TUNE_CENTS = 3;
/** Once in tune, stay "in tune" until the error exceeds this (no flicker at the edge). */
const OUT_OF_TUNE_CENTS = 4.5;

export function inTuneWithHysteresis(was: boolean, cents: number): boolean {
  const a = Math.abs(cents);
  return was ? a <= OUT_OF_TUNE_CENTS : a <= IN_TUNE_CENTS;
}

/** A4 reference range offered by the UI (the pedal takes an integer). */
export const REF_MIN = 432;
export const REF_MAX = 445;
export const REF_DEFAULT = 440;
export const clampRef = (hz: number): number =>
  Math.min(REF_MAX, Math.max(REF_MIN, Math.round(Number.isFinite(hz) ? hz : REF_DEFAULT)));

/** Standard guitar tuning, low to high. */
export const GUITAR_STRINGS: ReadonlyArray<{ name: string; octave: number }> = [
  { name: "E", octave: 2 }, { name: "A", octave: 2 }, { name: "D", octave: 3 },
  { name: "G", octave: 3 }, { name: "B", octave: 3 }, { name: "E", octave: 4 },
];

/** Index of the string whose note is the reading, or -1 when it is not one of the open strings. */
export function matchingString(r: Pick<NoteReading, "name" | "octave">): number {
  return GUITAR_STRINGS.findIndex((s) => s.name === r.name && s.octave === r.octave);
}

export function formatCents(c: number): string {
  const v = Math.round(c * 10) / 10;
  return `${v > 0 ? "+" : ""}${v.toFixed(1)}`;
}
