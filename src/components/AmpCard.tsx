import { useRigStore } from "../rigStore";
import { useStore } from "../store";
import type { AmpEq } from "../types";
import CustomSelect from "./CustomSelect";
import Knob from "./Knob";
import { useT, type Key } from "../i18n";

const EQ: Array<{ key: keyof AmpEq; label: Key }> = [
  { key: "volume", label: "amp.volume" },
  { key: "bass", label: "amp.bass" },
  { key: "mid", label: "amp.mid" },
  { key: "treble", label: "amp.treble" },
  { key: "presence", label: "amp.presence" },
  { key: "drive", label: "amp.drive" },
];

const kb = (n: number) => `${Math.max(1, Math.round(n / 1024))} KB`;

const dB10 = (v: number) => `${(v / 10).toFixed(1)} dB`;

export default function AmpCard() {
  const t = useT();
  const snap = useStore((s) => s.snap)!;
  const ranges = useStore((s) => s.info?.amp_eq_ranges ?? {});
  const { setAmpOn, setAmpEq, loadCapture, setAmpPresent } = useStore.getState();
  const setSidebarTab = useRigStore((s) => s.setSidebarTab);
  const on = snap.chain.amp_on;
  const caps = snap.captures.entries;
  const cur = caps.find((c) => c.name === snap.model)?.slot ?? "";

  return (
    <div
      className={`relative rounded-2xl border bg-gradient-to-b from-ink-700 to-ink-800 p-3 ${
        on ? "border-amber/60 shadow-[0_0_40px_-12px_var(--color-amber)]" : "border-ink-600 opacity-80"
      }`}
    >
      <div className="flex items-center gap-2">
        <button
          aria-label={on ? t("amp.bypass_aria") : t("amp.enable_aria")}
          aria-pressed={on}
          onClick={() => setAmpOn(!on)}
          className={`h-5 w-5 rounded-full border transition ${on ? "border-amber bg-amber shadow-[0_0_10px_var(--color-amber)]" : "border-ink-500 bg-ink-700 hover:border-ink-300"}`}
        />
        <div className="flex-1">
          <div className="text-[10px] uppercase tracking-[0.2em] text-ink-300">{t("amp.title")}</div>
          <div className="truncate text-[15px] font-semibold" title={snap.model ?? ""}>{snap.model ?? t("amp.no_capture")}</div>
        </div>
        <button
          onClick={() => setAmpOn(!on)}
          className={`rounded-md border px-2 py-1 text-[11px] ${on ? "border-ink-500 text-ink-200 hover:border-amber hover:text-amber" : "border-amber text-amber"}`}
        >
          {on ? t("amp.bypass") : t("amp.enable")}
        </button>
      </div>

      <div className="mt-3">
        <div className="flex items-center justify-between text-[10px] uppercase tracking-wider text-ink-300">
          <span id="cap-label">{t("amp.capture_on_pedal")}</span>
          <button onClick={() => setSidebarTab("slots")} className="normal-case tracking-normal text-ink-300 underline-offset-2 hover:text-amber hover:underline">{t("amp.manage_slots")}</button>
        </div>
        <div className="mt-1">
          <CustomSelect
            accent="amber"
            ariaLabel={t("amp.capture_on_pedal")}
            value={cur === "" ? null : cur}
            placeholder={snap.model ?? "—"}
            options={caps.map((c) => ({ value: c.slot, label: `${String(c.slot + 1).padStart(2, "0")} · ${c.name}`, hint: kb(c.bytes) }))}
            onChange={(slot) => void loadCapture(slot)}
          />
        </div>
      </div>

      <div className="mt-3 flex items-center justify-between text-[10px] uppercase tracking-wider text-ink-300">
        <span>{t("amp.eq")}</span>
        <button onClick={() => setAmpPresent(false)} className="normal-case tracking-normal text-ink-300 underline-offset-2 hover:text-danger hover:underline">{t("amp.remove")}</button>
      </div>
      <div className="mt-1.5 grid grid-cols-3 gap-y-3">
        {EQ.map(({ key, label }) => {
          const [min, max] = ranges[key] ?? [-120, 120];
          return (
            <div key={key} className="flex justify-center">
              <Knob
                label={t(label)}
                value={snap.ampeq[key]}
                min={min}
                max={max}
                def={0}
                display={dB10(snap.ampeq[key])}
                hue={75}
                size={48}
                disabled={!on}
                onChange={(v) => setAmpEq({ ...useStore.getState().snap!.ampeq, [key]: v })}
              />
            </div>
          );
        })}
      </div>
    </div>
  );
}
