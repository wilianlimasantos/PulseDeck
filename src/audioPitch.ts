import { freqToNote, median, yin, type NoteReading } from "./pitch";
import { t } from "./i18n";

export interface AudioInput {
  deviceId: string;
  label: string;
}

/** Human-readable reason for a getUserMedia failure. */
export function describeAudioError(e: unknown): string {
  const name = e instanceof DOMException ? e.name : "";
  if (name === "NotAllowedError" || name === "SecurityError") return t("mic.denied");
  if (name === "NotFoundError" || name === "OverconstrainedError") return t("mic.notfound");
  if (name === "NotReadableError") return t("mic.busy");
  return e instanceof Error ? e.message : String(e);
}

export const audioSupported = (): boolean =>
  typeof navigator !== "undefined" && !!navigator.mediaDevices?.getUserMedia;

/** Audio inputs. Labels are empty until the user has granted access once. */
export async function listInputs(): Promise<AudioInput[]> {
  if (!navigator.mediaDevices?.enumerateDevices) return [];
  const all = await navigator.mediaDevices.enumerateDevices();
  return all
    .filter((d) => d.kind === "audioinput" && d.deviceId !== "")
    .map((d, i) => ({ deviceId: d.deviceId, label: d.label || `Entrada ${i + 1}` }));
}

export interface DetectorOptions {
  deviceId: string | null;
  /** current A4, read on every analysis so changing it applies immediately */
  getA4: () => number;
  onReading: (r: NoteReading | null, level: number) => void;
}

/** Below this RMS (about -46 dBFS) the input is treated as silence. */
const GATE_RMS = 0.005;
const FFT = 4096;
const INTERVAL_MS = 33;
/** keep showing the last note this long after the signal drops */
const HOLD_MS = 300;

/**
 * Live pitch detection on an audio input: getUserMedia -> AnalyserNode -> YIN, a median over the
 * last readings (steady needle) and a short hold. Processing off (echo cancellation, noise
 * suppression, AGC) so the string is analysed as played.
 */
export class AudioPitchDetector {
  private ctx: AudioContext | null = null;
  private stream: MediaStream | null = null;
  private timer: ReturnType<typeof setInterval> | null = null;
  private recent: number[] = [];
  private lastGood = 0;
  private last: NoteReading | null = null;

  constructor(private readonly opts: DetectorOptions) {}

  async start(): Promise<void> {
    if (!audioSupported()) throw new Error(t("mic.unsupported"));
    const audio: MediaTrackConstraints = {
      echoCancellation: false,
      noiseSuppression: false,
      autoGainControl: false,
      ...(this.opts.deviceId ? { deviceId: { exact: this.opts.deviceId } } : {}),
    };
    this.stream = await navigator.mediaDevices.getUserMedia({ audio });
    this.ctx = new AudioContext({ latencyHint: "interactive" });
    const src = this.ctx.createMediaStreamSource(this.stream);
    const an = this.ctx.createAnalyser();
    an.fftSize = FFT;
    an.smoothingTimeConstant = 0;
    src.connect(an); // not connected to the destination: nothing is played back
    const buf = new Float32Array(FFT);
    this.timer = setInterval(() => {
      an.getFloatTimeDomainData(buf);
      this.analyse(buf, this.ctx?.sampleRate ?? 48000);
    }, INTERVAL_MS);
  }

  private analyse(buf: Float32Array, sr: number) {
    let sum = 0;
    for (let i = 0; i < buf.length; i++) sum += buf[i] * buf[i];
    const rms = Math.sqrt(sum / buf.length);
    const level = rms > 0 ? 20 * Math.log10(rms) : -140;
    const now = performance.now();
    const p = rms >= GATE_RMS ? yin(buf, sr) : null;
    if (p && p.probability > 0.85) {
      this.recent.push(p.freq);
      if (this.recent.length > 5) this.recent.shift();
      // a jump of more than a semitone is a new note: do not average it with the old one
      const m = median(this.recent);
      if (Math.abs(1200 * Math.log2(p.freq / m)) > 100) this.recent = [p.freq];
      const f = median(this.recent);
      this.last = freqToNote(f, this.opts.getA4());
      this.lastGood = now;
      this.opts.onReading(this.last, level);
    } else if (this.last && now - this.lastGood < HOLD_MS) {
      this.opts.onReading(freqToNote(this.last.hz, this.opts.getA4()), level);
    } else {
      this.recent = [];
      this.last = null;
      this.opts.onReading(null, level);
    }
  }

  stop() {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
    this.stream?.getTracks().forEach((t) => t.stop());
    this.stream = null;
    void this.ctx?.close().catch(() => undefined);
    this.ctx = null;
    this.recent = [];
    this.last = null;
  }
}
