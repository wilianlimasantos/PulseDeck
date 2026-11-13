import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { audioSupported } from "../audioPitch";
import { clampRef, formatCents, GUITAR_STRINGS, IN_TUNE_CENTS, matchingString, REF_MAX, REF_MIN } from "../pitch";
import { useStore } from "../store";
import { useTuner } from "../tunerStore";
import CustomSelect from "./CustomSelect";
import { useT } from "../i18n";

/** One LED per 2 cents from -50 to +50. */
const LEDS = 51;
const ledCents = (i: number) => -50 + 2 * i;

const zoneColor = (c: number) =>
  Math.abs(c) <= IN_TUNE_CENTS ? "var(--color-tuned)" : Math.abs(c) <= 15 ? "var(--color-amber)" : "var(--color-danger)";

/** Eases the displayed value toward the target (steady needle); snaps on large jumps (new note). */
function useEased(target: number | null): number | null {
  const [shown, setShown] = useState<number | null>(target);
  const goal = useRef<number | null>(target);
  const cur = useRef<number | null>(target);
  goal.current = target;
  useEffect(() => {
    let raf = 0;
    const tick = () => {
      const g = goal.current;
      if (g === null) {
        if (cur.current !== null) {
          cur.current = null;
          setShown(null);
        }
      } else {
        const c = cur.current;
        const next = c === null || Math.abs(g - c) > 30 ? g : c + (g - c) * 0.35;
        if (c === null || Math.abs(next - c) > 0.02) {
          cur.current = next;
          setShown(next);
        }
      }
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, []);
  return shown;
}

function Fork({ size = 18 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 16 16" aria-hidden fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <path d="M5.5 1.5v5a2.5 2.5 0 0 0 5 0v-5M8 9v5.5" />
    </svg>
  );
}

export { Fork as TuningForkIcon };

function LedBar({ cents, inTune }: { cents: number | null; inTune: boolean }) {
  const t = useT();
  const clamped = cents === null ? null : Math.max(-50, Math.min(50, cents));
  return (
    <div className="w-full max-w-4xl">
      <div
        role="meter"
        aria-label={t("tn.cents_meter")}
        aria-valuemin={-50}
        aria-valuemax={50}
        aria-valuenow={clamped === null ? undefined : Math.round(clamped)}
        aria-valuetext={cents === null ? t("tn.no_signal") : `${formatCents(cents)} cents`}
        className="flex h-24 items-center gap-[3px]"
      >
        {Array.from({ length: LEDS }, (_, i) => {
          const lc = ledCents(i);
          const center = Math.abs(lc) <= IN_TUNE_CENTS;
          const major = lc % 10 === 0;
          const active = clamped !== null && Math.abs(lc - clamped) <= 1;
          const filled = clamped !== null && (clamped >= 0 ? lc >= 0 && lc <= clamped + 1 : lc <= 0 && lc >= clamped - 1);
          // in tune: the whole centre zone lights up emerald
          const lit = inTune ? center : filled;
          const color = zoneColor(lc);
          return (
            <span
              key={lc}
              className="flex-1 rounded-[3px] transition-[opacity,box-shadow] duration-100"
              style={{
                height: lc === 0 ? "100%" : major ? "72%" : "50%",
                background: color,
                opacity: lit ? (active || inTune ? 1 : 0.55) : center ? 0.28 : 0.12,
                boxShadow: lit && (active || inTune) ? `0 0 14px 1px ${color}` : undefined,
              }}
            />
          );
        })}
      </div>
      <div className="mt-1 flex justify-between font-mono text-[12px] text-ink-300" aria-hidden>
        {["-50", "-25", "0", "+25", "+50"].map((t) => <span key={t}>{t}</span>)}
      </div>
    </div>
  );
}

function RefControl() {
  const t = useT();
  const refHz = useTuner((s) => s.refHz);
  const setRef = useTuner((s) => s.setRef);
  const [draft, setDraft] = useState(String(refHz));
  useEffect(() => setDraft(String(refHz)), [refHz]);
  const step = "grid h-8 w-8 place-items-center rounded-md border border-ink-600 bg-ink-800 text-[15px] hover:border-ink-300 disabled:opacity-30";
  return (
    <div className="flex items-center gap-2" role="group" aria-label={t("tn.ref_group")}>
      <span className="text-[11px] uppercase tracking-wider text-ink-300">A4 =</span>
      <button className={step} aria-label={t("tn.dec")} disabled={refHz <= REF_MIN} onClick={() => setRef(refHz - 1)}>−</button>
      <input
        type="number"
        inputMode="numeric"
        min={REF_MIN}
        max={REF_MAX}
        step={1}
        value={draft}
        aria-label={t("tn.ref_hz")}
        onChange={(e) => setDraft(e.target.value)}
        onBlur={() => { const v = clampRef(Number(draft)); setDraft(String(v)); setRef(v); }}
        onKeyDown={(e) => e.key === "Enter" && (e.target as HTMLInputElement).blur()}
        className="h-8 w-16 rounded-md border border-ink-600 bg-ink-900 text-center font-mono text-[14px] outline-none focus:border-pulse"
      />
      <button className={step} aria-label={t("tn.inc")} disabled={refHz >= REF_MAX} onClick={() => setRef(refHz + 1)}>+</button>
      <span className="text-[11px] text-ink-300">Hz</span>
      <button onClick={() => setRef(440)} disabled={refHz === 440} className="rounded-md px-1.5 text-[11px] text-ink-300 underline-offset-2 hover:text-pulse hover:underline disabled:opacity-0">440</button>
    </div>
  );
}

/** Full-screen tuner: giant note, LED bar from -50 to +50 cents, frequency, mute and source options. */
export default function Tuner() {
  const t = useT();
  const open = useTuner((s) => s.open);
  const source = useTuner((s) => s.source);
  const mute = useTuner((s) => s.mute);
  const reading = useTuner((s) => s.reading);
  const level = useTuner((s) => s.level);
  const inTune = useTuner((s) => s.inTune);
  const error = useTuner((s) => s.error);
  const refHz = useTuner((s) => s.refHz);
  const devices = useTuner((s) => s.devices);
  const deviceId = useTuner((s) => s.deviceId);
  const { closeTuner, setSource, setMute, setDevice, refreshDevices } = useTuner.getState();
  const connected = useStore((s) => s.snap?.status.state === "connected");
  const closeBtn = useRef<HTMLButtonElement>(null);
  const eased = useEased(reading ? reading.cents : null);

  useEffect(() => {
    if (!open) return;
    closeBtn.current?.focus();
    void refreshDevices();
    const esc = (e: KeyboardEvent) => {
      if (e.key !== "Escape" || document.querySelector('[role="listbox"]')) return;
      e.stopPropagation();
      useTuner.getState().closeTuner();
    };
    window.addEventListener("keydown", esc, true);
    return () => window.removeEventListener("keydown", esc, true);
  }, [open, refreshDevices]);

  if (!open) return null;

  const cents = reading ? eased ?? reading.cents : null;
  const sharp = reading?.name.endsWith("#");
  const letter = reading ? reading.name[0] : "–";
  const string = reading ? matchingString(reading) : -1;
  const flat = cents !== null && !inTune && cents < -IN_TUNE_CENTS;
  const high = cents !== null && !inTune && cents > IN_TUNE_CENTS;
  const seg = (on: boolean) =>
    `rounded-md px-3 py-1.5 text-[12px] font-medium transition disabled:cursor-not-allowed disabled:opacity-40 ${on ? "bg-ink-600 text-white" : "text-ink-300 hover:bg-ink-700"}`;

  return createPortal(
    <div data-tuner="" role="dialog" aria-modal="true" aria-label={t("tn.title")} className="fixed inset-0 z-[130] flex flex-col overflow-y-auto bg-ink-950/[0.97] backdrop-blur-sm">
      <header className="flex flex-wrap items-center gap-x-5 gap-y-3 border-b border-ink-700 px-5 py-3">
        <h2 className="flex items-center gap-2 text-[14px] font-semibold"><Fork size={20} /> {t("tn.title")}</h2>

        <div role="group" aria-label={t("tn.source")} className="flex gap-1 rounded-lg border border-ink-600 bg-ink-850 p-0.5">
          <button aria-pressed={source === "pedal"} disabled={!connected} onClick={() => setSource("pedal")} title={connected ? t("tn.src_pedal_title") : t("tn.connect_pedal")} className={seg(source === "pedal")}>{t("tn.src_pedal")}</button>
          <button aria-pressed={source === "audio"} disabled={!audioSupported()} onClick={() => setSource("audio")} title={t("tn.src_audio_title")} className={seg(source === "audio")}>{t("tn.src_audio")}</button>
        </div>

        {source === "audio" && (
          <div className="w-56">
            <CustomSelect
              ariaLabel={t("tn.input")}
              value={deviceId ?? ""}
              options={[{ value: "", label: t("tn.default_input") }, ...devices.map((d) => ({ value: d.deviceId, label: d.label }))]}
              onChange={(v) => setDevice(v === "" ? null : v)}
            />
          </div>
        )}

        <RefControl />

        <button
          role="switch"
          aria-checked={mute}
          disabled={!connected}
          onClick={() => setMute(!mute)}
          title={connected ? t("tn.mute_title") : t("tn.no_pedal")}
          className="flex items-center gap-2 rounded-lg border border-ink-600 bg-ink-800 px-3 py-1.5 text-[12px] disabled:opacity-40"
        >
          <span className={`relative h-4 w-7 rounded-full transition ${mute && connected ? "bg-pulse" : "bg-ink-500"}`}>
            <span className={`absolute top-0.5 h-3 w-3 rounded-full bg-ink-950 transition-all ${mute ? "left-3.5" : "left-0.5"}`} />
          </span>
          {t("tn.mute")}
        </button>

        <button ref={closeBtn} aria-label={t("tn.close_aria")} title={t("tn.close_title")} onClick={() => closeTuner()} className="ml-auto rounded-md border border-ink-600 px-3 py-1.5 text-[12px] hover:border-ink-300">{t("tn.close")}</button>
      </header>

      {error && <div role="alert" className="border-b border-danger/40 bg-danger/10 px-5 py-2 text-[12px] text-danger">{error}</div>}

      <main className="flex min-h-0 flex-1 flex-col items-center justify-center gap-5 px-5 py-6">
        <div className="flex items-center gap-8" aria-live="polite">
          <span className={`text-[min(9vw,10vh)] leading-none transition-colors ${flat ? "text-amber" : "text-ink-500"}`} aria-hidden>♭</span>
          <div
            className={`flex items-start font-black leading-[0.85] tabular-nums transition-colors duration-150 text-[min(26vw,36vh)] ${
              !reading ? "text-ink-500" : inTune ? "text-tuned [text-shadow:0_0_48px_var(--color-tuned)]" : "text-ink-100"
            }`}
            aria-label={reading ? (inTune ? t("tn.in_tune_note", { note: `${reading.name}${reading.octave}` }) : `${reading.name}${reading.octave}`) : t("tn.no_note")}
          >
            <span>{letter}</span>
            {sharp && <span className="mt-[0.08em] text-[0.4em] leading-none">♯</span>}
            {reading && <span className="mt-auto self-end text-[0.26em] font-bold leading-none text-ink-300">{reading.octave}</span>}
          </div>
          <span className={`text-[min(9vw,10vh)] leading-none transition-colors ${high ? "text-amber" : "text-ink-500"}`} aria-hidden>♯</span>
        </div>

        <LedBar cents={cents} inTune={inTune} />

        <div className="flex items-baseline gap-10 font-mono tabular-nums">
          <div className="text-center">
            <div className={`text-[28px] font-semibold ${inTune ? "text-tuned" : ""}`}>{reading ? `${formatCents(reading.cents)} ¢` : "— ¢"}</div>
            <div className="text-[11px] uppercase tracking-wider text-ink-300">{t("tn.cents")}</div>
          </div>
          <div className="text-center">
            <div className="text-[28px] font-semibold">{reading ? reading.hz.toFixed(2) : "—"}<span className="ml-1 text-[14px] text-ink-300">Hz</span></div>
            <div className="text-[11px] uppercase tracking-wider text-ink-300">{t("tn.frequency")}</div>
          </div>
        </div>

        <div className="h-5 text-[13px]" aria-live="polite">
          {inTune ? <span className="font-semibold text-tuned">{t("tn.in_tune")}</span> : !reading ? <span className="text-ink-300">{t("tn.pluck")}</span> : null}
        </div>

        <div className="flex gap-2" aria-label={t("tn.strings")}>
          {GUITAR_STRINGS.map((s, i) => (
            <span
              key={i}
              className={`grid h-10 w-14 place-items-center rounded-lg border font-mono text-[14px] transition ${
                i === string ? (inTune ? "border-tuned bg-tuned/15 text-tuned" : "border-pulse bg-pulse/10 text-pulse") : "border-ink-600 text-ink-300"
              }`}
            >
              {s.name}{s.octave}
            </span>
          ))}
        </div>
      </main>

      <footer className="flex flex-wrap justify-between gap-2 border-t border-ink-700 px-5 py-2 text-[11px] text-ink-300">
        <span>{source === "pedal" ? t("tn.foot_pedal") : t("tn.foot_audio")} · A4 = {refHz} Hz</span>
        <span>{level !== null ? t("tn.level", { db: level.toFixed(0) }) : ""}</span>
        <span>{t("tn.closes")}</span>
      </footer>
    </div>,
    document.body,
  );
}
