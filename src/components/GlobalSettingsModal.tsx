import { useEffect, useRef } from "react";
import { createPortal } from "react-dom";
import { dimColor, useGlobals } from "../globalsStore";
import { useStore } from "../store";
import CustomSelect from "./CustomSelect";
import { useT, type Key } from "../i18n";

/** Vertical dB slider (pointer + keyboard); double click resets to 0. */
function VSlider({ value, min, max, disabled, label, onChange }: { value: number; min: number; max: number; disabled?: boolean; label: string; onChange: (v: number) => void }) {
  const track = useRef<HTMLDivElement>(null);
  const H = 170;
  const frac = (value - min) / (max - min);
  const fromY = (clientY: number) => {
    const r = track.current!.getBoundingClientRect();
    const f = 1 - Math.min(1, Math.max(0, (clientY - r.top) / r.height));
    return Math.round(min + f * (max - min));
  };
  return (
    <div
      ref={track}
      role="slider"
      tabIndex={disabled ? -1 : 0}
      aria-label={label}
      aria-orientation="vertical"
      aria-valuemin={min}
      aria-valuemax={max}
      aria-valuenow={value}
      aria-valuetext={`${value > 0 ? "+" : ""}${value} dB`}
      aria-disabled={disabled}
      style={{ height: H }}
      className={`relative w-9 touch-none select-none rounded-full outline-none focus-visible:ring-1 focus-visible:ring-pulse ${disabled ? "opacity-40" : "cursor-pointer"}`}
      onPointerDown={(e) => {
        if (disabled) return;
        e.currentTarget.setPointerCapture(e.pointerId);
        onChange(fromY(e.clientY));
      }}
      onPointerMove={(e) => {
        if (!disabled && e.currentTarget.hasPointerCapture(e.pointerId)) onChange(fromY(e.clientY));
      }}
      onDoubleClick={() => !disabled && onChange(0)}
      onKeyDown={(e) => {
        if (disabled) return;
        const d = e.key === "ArrowUp" || e.key === "ArrowRight" ? 1 : e.key === "ArrowDown" || e.key === "ArrowLeft" ? -1 : e.key === "PageUp" ? 3 : e.key === "PageDown" ? -3 : 0;
        if (d) {
          e.preventDefault();
          onChange(Math.min(max, Math.max(min, value + d)));
        } else if (e.key === "Home") onChange(max);
        else if (e.key === "End") onChange(min);
        else if (e.key === "0") onChange(0);
      }}
    >
      <div className="absolute inset-y-0 left-1/2 w-1.5 -translate-x-1/2 rounded-full bg-ink-700" />
      <div className="absolute left-1/2 top-1/2 h-px w-5 -translate-x-1/2 bg-ink-500" />
      <div
        className="absolute left-1/2 w-1.5 -translate-x-1/2 rounded-full bg-pulse/70"
        style={value >= 0 ? { bottom: "50%", height: `${(frac - 0.5) * 100}%` } : { top: "50%", height: `${(0.5 - frac) * 100}%` }}
      />
      <div
        className="absolute left-1/2 h-4 w-7 -translate-x-1/2 translate-y-1/2 rounded-md border border-ink-300 bg-ink-600 shadow"
        style={{ bottom: `${frac * 100}%` }}
      />
    </div>
  );
}

function Section({ title, hint, children, right }: { title: string; hint?: string; children: React.ReactNode; right?: React.ReactNode }) {
  return (
    <section className="rounded-xl border border-ink-700 bg-ink-850 p-4">
      <div className="mb-3 flex items-start justify-between gap-3">
        <div>
          <h3 className="text-[13px] font-semibold">{title}</h3>
          {hint && <p className="mt-0.5 text-[11px] leading-snug text-ink-300">{hint}</p>}
        </div>
        {right}
      </div>
      {children}
    </section>
  );
}

function Seg<T extends string | number>({ value, options, onChange, label }: { value: T; options: [T, string][]; onChange: (v: T) => void; label: string }) {
  return (
    <div role="group" aria-label={label} className="inline-flex rounded-lg bg-ink-900 p-0.5">
      {options.map(([v, text]) => (
        <button
          key={String(v)}
          aria-pressed={value === v}
          onClick={() => onChange(v)}
          className={`rounded-md px-2.5 py-1 text-[11px] font-medium ${value === v ? "bg-ink-600 text-white" : "text-ink-300 hover:text-white"}`}
        >
          {text}
        </button>
      ))}
    </div>
  );
}

const FS_NAMES = ["A", "B", "C", "D"];
type Tr = (key: Key, params?: Record<string, string | number>) => string;
const idleText = (t: Tr, s: number) => (s === 0 ? t("g.off") : s < 60 ? t("g.idle_s", { n: s }) : t("g.idle_min", { n: s / 60 }));
const onOff = (t: Tr): [number, string][] => [[0, t("g.off")], [1, t("g.on")]];

function Ring({ color, on, bright, label }: { color: string; on: boolean; bright: number; label: string }) {
  const c = `#${color}`;
  return (
    <div
      aria-label={label}
      className="grid h-12 w-12 place-items-center rounded-full border-[5px] bg-ink-900"
      style={{ borderColor: c, opacity: 0.25 + 0.75 * (bright / 100), boxShadow: on ? `0 0 14px ${c}` : "none" }}
    />
  );
}

export default function GlobalSettingsModal() {
  const t = useT();
  const g = useGlobals();
  const connected = useStore((s) => s.snap?.status.state === "connected");
  const close = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    if (!g.open) return;
    close.current?.focus();
    const esc = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.stopPropagation();
        useGlobals.getState().close();
      }
    };
    window.addEventListener("keydown", esc, true);
    return () => window.removeEventListener("keydown", esc, true);
  }, [g.open]);

  // the pedal went away while the modal was open
  useEffect(() => {
    if (g.open && !connected) useGlobals.getState().close();
  }, [g.open, connected]);

  if (!g.open) return null;
  const d = g.data;
  const sc = d?.schema;
  const bypassed = g.stash !== null;

  return createPortal(
    <div
      className="fixed inset-0 z-[100] flex items-center justify-center bg-black/70 p-6 backdrop-blur-[2px]"
      onMouseDown={(e) => e.target === e.currentTarget && g.close()}
    >
      <div role="dialog" aria-modal="true" aria-label={t("g.title")} className="flex max-h-full w-full max-w-3xl flex-col rounded-2xl border border-ink-600 bg-ink-900 shadow-2xl">
        <header className="flex items-center gap-3 border-b border-ink-700 px-5 py-3">
          <h2 className="text-[14px] font-semibold">{t("g.title")}</h2>
          <span className="text-[11px] text-ink-300">{t("g.subtitle")}</span>
          {d?.dirty && <span className="rounded bg-amber/20 px-1.5 py-0.5 text-[10px] font-medium uppercase text-amber">{t("g.unsaved_badge")}</span>}
          <button ref={close} aria-label={t("g.close")} onClick={g.close} className="ml-auto px-1 text-ink-300 hover:text-white">✕</button>
        </header>

        <div className="min-h-0 flex-1 space-y-4 overflow-y-auto p-5">
          {g.loading && !d && <p className="text-ink-300">{t("g.loading")}</p>}
          {!g.loading && !d && (
            <p className="text-ink-300">
              {t("g.read_fail")}<button className="underline" onClick={() => void g.reload()}>{t("g.retry")}</button>
            </p>
          )}

          {d && sc && (
            <>
              <Section
                title={t("g.eq_title")}
                hint={d.geq ? t("g.eq_hint") : t("g.eq_unsupported")}
                right={
                  <div className="flex shrink-0 items-center gap-2">
                    <button
                      role="switch"
                      aria-checked={!bypassed}
                      disabled={!d.geq}
                      onClick={() => void g.toggleEq()}
                      className={`rounded-md border px-2.5 py-1 text-[11px] font-medium disabled:opacity-40 ${bypassed ? "border-ink-500 text-ink-300" : "border-pulse/60 text-pulse"}`}
                    >
                      {bypassed ? t("g.eq_bypassed") : t("g.eq_active")}
                    </button>
                    <button disabled={!d.geq} onClick={() => void g.resetEq()} className="rounded-md border border-ink-500 px-2.5 py-1 text-[11px] hover:border-ink-300 disabled:opacity-40">
                      {t("g.eq_reset")}
                    </button>
                  </div>
                }
              >
                {d.geq && (
                  <div className="flex flex-wrap items-end justify-center gap-6 sm:gap-10">
                    {sc.geq.bands_hz.map((hz, b) => (
                      <div key={hz} className="flex flex-col items-center gap-1.5">
                        <span className="font-mono text-[11px] tabular-nums text-ink-100">{g.geq[b] > 0 ? "+" : ""}{g.geq[b]} dB</span>
                        <VSlider
                          value={g.geq[b]}
                          min={sc.geq.min_db}
                          max={sc.geq.max_db}
                          disabled={bypassed}
                          label={t("g.band", { f: hz >= 1000 ? `${hz / 1000} kHz` : `${hz} Hz` })}
                          onChange={(v) => g.setBand(b, v)}
                        />
                        <span className="text-[11px] text-ink-300">{hz >= 1000 ? `${hz / 1000}k` : hz} Hz</span>
                      </div>
                    ))}
                  </div>
                )}
                {d.hpf !== null && (
                  <div className="mt-4 flex items-center justify-between gap-3 border-t border-ink-700 pt-3">
                    <div>
                      <div className="text-[12px] font-medium">{t("g.hpf_title")}</div>
                      <div className="text-[11px] text-ink-300">{t("g.hpf_desc")}</div>
                    </div>
                    <CustomSelect
                      ariaLabel={t("g.hpf_aria")}
                      value={d.hpf}
                      onChange={(v) => void g.setHpf(v)}
                      className="w-32"
                      options={[
                        { value: sc.hpf.off, label: t("g.off") },
                        ...Array.from({ length: (sc.hpf.max_hz - sc.hpf.min_hz) / sc.hpf.step_hz + 1 }, (_, i) => {
                          const hz = sc.hpf.min_hz + i * sc.hpf.step_hz;
                          return { value: hz, label: `${hz} Hz` };
                        }),
                      ]}
                    />
                  </div>
                )}
              </Section>

              <Section title={t("g.fs_title")} hint={t("g.fs_hint")}>
                <div className="grid gap-2 sm:grid-cols-2">
                  {sc.footswitch_quick_setups.map((q) => {
                    const on = d.quick_active === q.key;
                    return (
                      <button
                        key={q.key}
                        aria-pressed={on}
                        onClick={() => void g.applyQuick(q.key)}
                        className={`rounded-lg border p-3 text-left transition ${on ? "border-pulse bg-pulse/10" : "border-ink-600 bg-ink-800 hover:border-ink-300"}`}
                      >
                        <div className="flex items-center justify-between text-[12px] font-semibold">
                          {t(`fsq.${q.key}.label` as Key)}
                          {on && <span className="text-[10px] font-medium uppercase text-pulse">{t("g.fs_active")}</span>}
                        </div>
                        <div className="mt-1 text-[11px] leading-snug text-ink-300">{t(`fsq.${q.key}.desc` as Key)}</div>
                      </button>
                    );
                  })}
                </div>
                {d.quick_active === null && d.layout !== null && (
                  <p className="mt-2 text-[11px] text-ink-300">{t("g.fs_custom", { layout: d.layout })}</p>
                )}
                {d.layout === null && <p className="mt-2 text-[11px] text-ink-300">{t("g.fs_unknown")}</p>}
              </Section>

              <Section title={t("g.led_title")} hint={t("g.led_hint")}>
                {d.fsled ? (
                  <div className="space-y-3">
                    <div className="grid gap-3 sm:grid-cols-2">
                      {d.fsled.map((l, i) => {
                        const bright = d.led?.bright ?? 100;
                        return (
                          <div key={i} className="rounded-lg border border-ink-700 bg-ink-800 p-3">
                            <div className="mb-2 flex items-center gap-3">
                              <Ring color={l.on} on bright={bright} label={t("g.led_active", { fs: FS_NAMES[i] })} />
                              <Ring color={l.off} on={false} bright={bright} label={t("g.led_inactive", { fs: FS_NAMES[i] })} />
                              <div className="min-w-0 flex-1 leading-tight">
                                <div className="text-[12px] font-semibold">{t("g.led_scene", { fs: FS_NAMES[i], n: i + 1 })}</div>
                                <div className="text-[10px] text-ink-300">{t("g.led_legend")}</div>
                              </div>
                              <Seg
                                label={t("g.led_mode", { fs: FS_NAMES[i] })}
                                value={l.custom ? 1 : 0}
                                options={[[0, t("g.led_auto")], [1, t("g.led_colors")]]}
                                onChange={(v) => void g.setFsLed(i, { custom: v === 1 })}
                              />
                            </div>
                            {l.custom && (
                              <div className="flex flex-wrap items-center gap-1.5">
                                {sc.led.palette.map((c) => (
                                  <button
                                    key={c}
                                    aria-label={t("g.led_color", { c })}
                                    aria-pressed={l.on === c}
                                    onClick={() => void g.setFsLed(i, { custom: true, on: c, off: dimColor(c, sc.led.dim_divisor) })}
                                    style={{ background: `#${c}` }}
                                    className={`h-5 w-5 rounded-full border ${l.on === c ? "border-white ring-1 ring-white" : "border-ink-500"}`}
                                  />
                                ))}
                                <input
                                  type="color"
                                  aria-label={t("g.led_custom_color", { fs: FS_NAMES[i] })}
                                  value={`#${l.on}`}
                                  onChange={(e) => {
                                    const c = e.target.value.slice(1);
                                    void g.setFsLed(i, { custom: true, on: c, off: dimColor(c, sc.led.dim_divisor) });
                                  }}
                                  className="h-6 w-8 cursor-pointer rounded border border-ink-500 bg-transparent"
                                />
                                <button
                                  onClick={() => void g.setFsLed(i, { custom: true, off: "000000" })}
                                  className={`ml-1 rounded border px-1.5 py-0.5 text-[10px] ${l.off === "000000" ? "border-pulse text-pulse" : "border-ink-500 text-ink-300"}`}
                                >
                                  {t("g.led_dark")}
                                </button>
                              </div>
                            )}
                          </div>
                        );
                      })}
                    </div>
                  </div>
                ) : (
                  <p className="text-[11px] text-ink-300">{t("g.led_unsupported")}</p>
                )}

                {d.led && (
                  <div className="mt-4 space-y-2.5 border-t border-ink-700 pt-3">
                    <Row label={t("g.bright", { v: d.led.bright })}>
                      <input
                        type="range"
                        aria-label={t("g.bright_aria")}
                        min={sc.led.bright.min}
                        max={sc.led.bright.max}
                        step={sc.led.bright.step}
                        value={d.led.bright}
                        onChange={(e) => void g.setLed("bright", Number(e.target.value))}
                        className="w-48 accent-[var(--color-pulse)]"
                      />
                    </Row>
                    <Row label={t("g.tuner_leds")}>
                      <Seg label={t("g.tuner_leds")} value={d.led.tuner} options={sc.led.tuner.map((v) => [v, t(`g.tuner.${v}` as Key)])} onChange={(v) => void g.setLed("tuner", v)} />
                    </Row>
                    <Row label={t("g.tap")}>
                      <Seg label={t("g.tap")} value={d.led.tap} options={sc.led.tap.map((v) => [v, t(`g.tap.${v}` as Key)])} onChange={(v) => void g.setLed("tap", v)} />
                    </Row>
                    <Row label={t("g.meter")}>
                      <Seg label={t("g.meter")} value={d.led.meter} options={sc.led.meter.map((v) => [v, t(`g.meter.${v}` as Key)])} onChange={(v) => void g.setLed("meter", v)} />
                    </Row>
                    <Row label={t("g.hold")}>
                      <Seg label={t("g.hold")} value={d.led.hold} options={onOff(t)} onChange={(v) => void g.setLed("hold", v)} />
                    </Row>
                    <Row label={t("g.anim")}>
                      <Seg label={t("g.anim")} value={d.led.anim} options={onOff(t)} onChange={(v) => void g.setLed("anim", v)} />
                    </Row>
                    <Row label={t("g.idle")}>
                      <Seg label={t("g.idle")} value={d.led.idle} options={sc.led.idle.map((v) => [v, idleText(t, v)])} onChange={(v) => void g.setLed("idle", v)} />
                    </Row>
                  </div>
                )}
              </Section>
            </>
          )}
        </div>

        <footer className="flex items-center gap-3 border-t border-ink-700 px-5 py-3">
          <p className="mr-auto text-[11px] leading-snug text-ink-300">
            {d?.dirty ? t("g.dirty_note") : t("g.clean_note")}
          </p>
          <button onClick={g.close} className="rounded-lg border border-ink-500 px-3 py-1.5 text-[12px] hover:border-ink-300">{t("g.close")}</button>
          <button
            disabled={!d?.dirty}
            onClick={() => void g.save()}
            className="rounded-lg bg-pulse px-3 py-1.5 text-[12px] font-semibold text-ink-950 disabled:cursor-not-allowed disabled:opacity-40"
          >
            {t("g.save")}
          </button>
        </footer>
      </div>
    </div>,
    document.body,
  );
}

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-3">
      <span className="text-[12px]">{label}</span>
      {children}
    </div>
  );
}
