import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { cpuPercent, CPU_ALERT_THRESHOLD } from "../alerts";
import { presetCode } from "../fx";
import { useRigStore } from "../rigStore";
import { useGlobals } from "../globalsStore";
import { useStore } from "../store";
import { useTuner } from "../tunerStore";
import ConnectionPill from "./ConnectionPill";
import { TuningForkIcon } from "./Tuner";
import { LANGS, useLang, useT } from "../i18n";

function Logo() {
  return (
    <div className="flex items-center gap-2">
      <svg width="26" height="26" viewBox="0 0 26 26" aria-hidden>
        <rect width="26" height="26" rx="7" fill="var(--color-ink-700)" />
        <path d="M3 14h5l3-8 4 14 3-6h5" fill="none" stroke="var(--color-pulse)" strokeWidth="2.2" strokeLinecap="round" strokeLinejoin="round" />
      </svg>
      <span className="hidden text-[15px] font-semibold tracking-tight min-[1280px]:inline">Pulse<span className="text-pulse">Deck</span></span>
    </div>
  );
}

function PresetPicker() {
  const t = useT();
  const snap = useStore((s) => s.snap)!;
  const count = useStore((s) => s.info?.preset_count ?? 32);
  const setPreset = useStore((s) => s.setPreset);
  const cur = snap.presets.current;
  const connected = snap.status.state === "connected";
  const names = new Map(snap.presets.presets.map((p) => [p.slot, p.name]));
  const go = (d: number) => setPreset((((cur ?? 0) + d) % count + count) % count);
  return (
    <div className="flex shrink-0 items-center gap-1">
      <button aria-label={t("preset.prev")} disabled={!connected} onClick={() => go(-1)} className="h-8 w-8 rounded-lg border border-ink-600 bg-ink-800 hover:border-ink-300 disabled:opacity-40">‹</button>
      <label className="relative">
        <span className="sr-only">{t("preset.label")}</span>
        <select
          disabled={!connected}
          value={cur ?? ""}
          onChange={(e) => setPreset(Number(e.target.value))}
          className="h-8 w-32 appearance-none min-[1200px]:w-44 rounded-lg border border-ink-600 bg-ink-800 pl-3 pr-7 text-[12px] font-medium outline-none hover:border-ink-300 focus:border-pulse disabled:opacity-40"
        >
          {cur === null && <option value="">{t("preset.none")}</option>}
          {Array.from({ length: count }, (_, slot) => (
            <option key={slot} value={slot}>{presetCode(slot)} · {names.get(slot) || t("preset.default_name", { n: slot + 1 })}</option>
          ))}
        </select>
        <span className="pointer-events-none absolute right-2.5 top-1.5 text-ink-300">▾</span>
      </label>
      <button aria-label={t("preset.next")} disabled={!connected} onClick={() => go(1)} className="h-8 w-8 rounded-lg border border-ink-600 bg-ink-800 hover:border-ink-300 disabled:opacity-40">›</button>
    </div>
  );
}

function ScenePicker() {
  const t = useT();
  const snap = useStore((s) => s.snap)!;
  const setScene = useStore((s) => s.setScene);
  const connected = snap.status.state === "connected";
  return (
    <div className="flex shrink-0 items-center gap-1 rounded-lg border border-ink-600 bg-ink-800 p-0.5" role="group" aria-label={t("scene.group")}>
      {[0, 1, 2, 3].map((i) => {
        const active = snap.scenes.current === i;
        return (
          <button
            key={i}
            disabled={!connected}
            onClick={() => setScene(i)}
            title={snap.scenes.stored[i] ? t("scene.title", { n: i + 1 }) : t("scene.title_empty", { n: i + 1 })}
            className={`relative h-7 w-9 rounded-md text-[12px] font-semibold transition disabled:opacity-40 ${active ? "bg-pulse text-ink-950" : "text-ink-200 hover:bg-ink-600"}`}
          >
            {i + 1}
            {snap.scenes.stored[i] && !active && <span className="absolute bottom-0.5 left-1/2 h-1 w-1 -translate-x-1/2 rounded-full bg-pulse-dim" />}
          </button>
        );
      })}
    </div>
  );
}

function CpuMeter() {
  const t = useT();
  const cpu = useStore((s) => s.snap!.level.cpu);
  const high = useStore((s) => s.cpuHigh);
  const pct = cpuPercent(cpu);
  const color = high ? "var(--color-danger)" : pct > 70 ? "var(--color-amber)" : "var(--color-pulse)";
  return (
    <div className="w-24 min-[1200px]:w-40" title={t("cpu.title")}>
      <div className="mb-1 flex items-baseline justify-between text-[10px] uppercase tracking-wider text-ink-300">
        <span>DSP</span>
        <span className={`font-mono text-[12px] normal-case ${high ? "blink text-danger" : "text-ink-100"}`}>
          {cpu.budget > 0 ? `${pct.toFixed(0)}%` : "—"}
          <span className="ml-1 hidden text-[10px] text-ink-300 min-[1300px]:inline">{cpu.budget > 0 ? `${cpu.used}/${cpu.budget}` : ""}</span>
        </span>
      </div>
      <div className="relative h-2 overflow-hidden rounded-full bg-ink-700" role="meter" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(pct)}>
        <div className="h-full rounded-full transition-[width] duration-200" style={{ width: `${Math.min(100, pct)}%`, background: color }} />
        <div className="absolute inset-y-0 w-px bg-ink-300/70" style={{ left: `${CPU_ALERT_THRESHOLD}%` }} />
      </div>
    </div>
  );
}

function TunerButton() {
  const t = useT();
  const open = useTuner((s) => s.open);
  const toggle = useTuner((s) => s.toggleTuner);
  return (
    <button
      onClick={toggle}
      aria-label={t("tuner.button_hint")}
      aria-pressed={open}
      title={t("tuner.button_hint")}
      className={`flex h-8 shrink-0 items-center gap-1.5 rounded-lg border px-2.5 text-[12px] font-medium transition ${
        open ? "border-tuned bg-tuned/10 text-tuned" : "border-ink-600 bg-ink-800 text-ink-100 hover:border-ink-300"
      }`}
    >
      <TuningForkIcon size={15} />
      <span className="hidden min-[1100px]:inline">{t("tuner.button")}</span>
    </button>
  );
}

function SaveButtons() {
  const t = useT();
  const unsaved = useStore((s) => s.snap!.unsaved);
  const connected = useStore((s) => s.snap!.status.state === "connected");
  const scene = useStore((s) => s.snap!.scenes.current);
  const busy = useStore((s) => s.busy);
  const { saveScene, savePreset } = useStore.getState();
  const any = unsaved.preset || unsaved.scene;
  const Btn = ({ label, aria, dirty, onClick, title }: { label: string; aria: string; dirty: boolean; onClick: () => void; title: string }) => (
    <button
      disabled={!connected || busy}
      onClick={onClick}
      title={title}
      aria-label={aria}
      className={`relative flex h-8 shrink-0 items-center gap-1.5 rounded-lg border px-2.5 text-[12px] font-medium transition disabled:opacity-40 ${
        dirty ? "border-amber/70 bg-amber/10 text-amber hover:bg-amber/20" : "border-ink-600 bg-ink-800 text-ink-100 hover:border-ink-300"
      }`}
    >
      <svg width="13" height="13" viewBox="0 0 16 16" aria-hidden fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinejoin="round">
        <path d="M2.5 2.5h9l2 2v9h-11z" /><path d="M5 2.5v3.5h5V2.5M5 13.5V9h6v4.5" />
      </svg>
      <span className="hidden min-[1100px]:inline">{label}</span>
      {dirty && <span className="absolute -right-1 -top-1 h-2.5 w-2.5 rounded-full bg-amber shadow-[0_0_8px_var(--color-amber)]" />}
    </button>
  );
  return (
    <div className="flex shrink-0 items-center gap-1.5" role="group" aria-label={t("save.group")}>
      <Btn label={t("save.scene")} aria={t("save.scene_aria")} dirty={unsaved.scene} onClick={() => void saveScene()} title={scene !== null && scene >= 0 ? t("save.scene_title", { n: scene + 1 }) : t("save.scene_pick")} />
      <Btn label={t("save.preset")} aria={t("save.preset_aria")} dirty={unsaved.preset} onClick={() => void savePreset()} title={t("save.preset_title")} />
      <span className={`hidden whitespace-nowrap text-[11px] min-[1400px]:inline ${any ? "text-amber" : "text-ink-500"}`} aria-live="polite">{any ? t("save.unsaved") : t("save.saved")}</span>
    </div>
  );
}

const I = {
  memory: "M3 4.5h10v7H3z M5.5 4.5v-2 M8 4.5v-2 M10.5 4.5v-2 M5.5 13.5v-2 M8 13.5v-2 M10.5 13.5v-2",
  export: "M8 10.5v-8 M4.5 6L8 2.5 11.5 6 M3 13.5h10",
  restore: "M8 2.5v8 M4.5 7L8 10.5 11.5 7 M3 13.5h10",
  sliders: "M3 4.5h6 M12 4.5h1 M3 8h1 M7 8h6 M3 11.5h7 M13 11.5h0 M10.5 3v3 M5.5 6.5v3 M11 10v3",
};

function MenuIcon({ d }: { d: string }) {
  return (
    <svg width="18" height="18" viewBox="0 0 16 16" aria-hidden fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" strokeLinejoin="round" className="shrink-0 text-ink-200">
      <path d={d} />
    </svg>
  );
}

/** The single top-right menu: opens a dark dropdown anchored to the button (portal, never clipped). */
function MainMenu() {
  const t = useT();
  const { lang, setLang } = useLang();
  const [open, setOpen] = useState(false);
  const [right, setRight] = useState({ top: 0, right: 0 });
  const btn = useRef<HTMLButtonElement>(null);
  const list = useRef<HTMLDivElement>(null);
  const busy = useStore((s) => s.busy);
  const connected = useStore((s) => s.snap!.status.state === "connected");
  const { exportBackup, restoreBackup } = useStore.getState();
  const setMemoryOpen = useRigStore((s) => s.setMemoryOpen);

  const toggle = () => {
    const r = btn.current?.getBoundingClientRect();
    if (r) setRight({ top: r.bottom + 8, right: Math.max(8, window.innerWidth - r.right) });
    setOpen((o) => !o);
  };

  useEffect(() => {
    if (!open) return;
    list.current?.querySelector<HTMLElement>("button:not(:disabled)")?.focus();
    const close = (e: MouseEvent) => {
      const t = e.target as Node;
      if (!list.current?.contains(t) && !btn.current?.contains(t)) setOpen(false);
    };
    const key = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        setOpen(false);
        btn.current?.focus();
      } else if (e.key === "ArrowDown" || e.key === "ArrowUp") {
        e.preventDefault();
        const items = [...(list.current?.querySelectorAll<HTMLElement>("button:not(:disabled)") ?? [])];
        const at = items.indexOf(document.activeElement as HTMLElement);
        items[(at + (e.key === "ArrowDown" ? 1 : -1) + items.length) % items.length]?.focus();
      }
    };
    const resize = () => setOpen(false);
    window.addEventListener("mousedown", close);
    window.addEventListener("keydown", key);
    window.addEventListener("resize", resize);
    return () => {
      window.removeEventListener("mousedown", close);
      window.removeEventListener("keydown", key);
      window.removeEventListener("resize", resize);
    };
  }, [open]);

  const item = (icon: string, label: string, hint: string, fn: () => void, needsPedal = true) => (
    <button
      role="menuitem"
      disabled={(needsPedal && !connected) || busy}
      onClick={() => { setOpen(false); fn(); }}
      className="flex w-full items-center gap-3 rounded-lg px-3 py-2.5 text-left outline-none hover:bg-ink-700 focus-visible:bg-ink-700 disabled:cursor-not-allowed disabled:opacity-40"
    >
      <MenuIcon d={icon} />
      <span className="min-w-0">
        <span className="block text-[13px] font-medium">{label}</span>
        <span className="block text-[11px] text-ink-300">{hint}</span>
      </span>
    </button>
  );

  return (
    <>
      <button
        ref={btn}
        aria-label={t("menu.manage")}
        aria-haspopup="menu"
        aria-expanded={open}
        onClick={toggle}
        className={`flex h-8 shrink-0 items-center gap-1.5 rounded-lg border px-2.5 text-[12px] font-medium transition ${open ? "border-pulse bg-ink-700" : "border-ink-600 bg-ink-800 hover:border-ink-300"}`}
      >
        <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round">
          <path d="M2.5 4.5h11M2.5 8h11M2.5 11.5h11" />
        </svg>
        <span className="hidden min-[1100px]:inline">{t("menu.manage")}</span>
      </button>
      {open &&
        createPortal(
          <div
            ref={list}
            role="menu"
            aria-label={t("menu.manage")}
            style={{ top: right.top, right: right.right }}
            className="fixed z-[120] w-[min(22rem,calc(100vw-16px))] rounded-xl border border-ink-600 bg-ink-800 p-1.5 shadow-2xl"
          >
            {item(I.sliders, t("menu.globals"), t("menu.globals_hint"), () => void useGlobals.getState().show())}
            {item(I.memory, t("menu.memory"), t("menu.memory_hint"), () => setMemoryOpen(true), false)}
            <div className="mx-2 my-1 h-px bg-ink-700" />
            {item(I.export, t("menu.export"), t("menu.export_hint"), () => void exportBackup())}
            {item(I.restore, t("menu.restore"), t("menu.restore_hint"), () => void restoreBackup())}
            <div className="mx-2 my-1 h-px bg-ink-700" />
            <div className="flex items-center justify-between gap-3 px-3 py-2" role="group" aria-label={t("menu.language")}>
              <span className="text-[12px] text-ink-200">{t("menu.language")}</span>
              <div className="inline-flex rounded-lg bg-ink-900 p-0.5">
                {LANGS.map((l) => (
                  <button
                    key={l.code}
                    aria-pressed={lang === l.code}
                    onClick={() => setLang(l.code)}
                    className={`rounded-md px-2.5 py-1 text-[11px] font-semibold outline-none focus-visible:ring-1 focus-visible:ring-pulse ${lang === l.code ? "bg-ink-600 text-white" : "text-ink-300 hover:text-white"}`}
                  >
                    {l.short}
                  </button>
                ))}
              </div>
            </div>
          </div>,
          document.body,
        )}
    </>
  );
}

export default function TopBar() {
  return (
    <header className="flex h-14 w-full min-w-0 shrink-0 items-center gap-2 border-b border-ink-700 bg-ink-850 px-3">
      <div className="flex min-w-0 shrink items-center gap-2">
        <Logo />
        <ConnectionPill />
      </div>
      <div className="flex min-w-0 flex-1 items-center justify-center gap-2">
        <PresetPicker />
        <ScenePicker />
        <TunerButton />
        <SaveButtons />
      </div>
      <div className="flex shrink-0 items-center gap-2">
        <CpuMeter />
        <MainMenu />
      </div>
    </header>
  );
}
