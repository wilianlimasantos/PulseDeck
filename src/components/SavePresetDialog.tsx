import { useEffect, useState } from "react";
import { presetCode } from "../fx";
import { useStore } from "../store";
import { t, useT } from "../i18n";

/** Shown when the pedal has no current preset (after PNEW): pick the flash slot and a name. */
export default function SavePresetDialog() {
  const tt = useT();
  const open = useStore((s) => s.saveDialog);
  const presets = useStore((s) => s.snap?.presets.presets ?? []);
  const count = useStore((s) => s.info?.preset_count ?? 32);
  const { setSaveDialog, savePresetAs } = useStore.getState();
  const used = new Map(presets.map((p) => [p.slot, p.name]));
  const firstFree = Array.from({ length: count }, (_, i) => i).find((i) => !used.has(i)) ?? 0;
  const [slot, setSlot] = useState(firstFree);
  const [name, setName] = useState(() => t("sp.new_preset"));

  useEffect(() => {
    if (!open) return;
    setSlot(firstFree);
    const esc = (e: KeyboardEvent) => e.key === "Escape" && setSaveDialog(false);
    window.addEventListener("keydown", esc);
    return () => window.removeEventListener("keydown", esc);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open]);

  if (!open) return null;
  const overwrite = used.get(slot);
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60" onMouseDown={(e) => e.target === e.currentTarget && setSaveDialog(false)}>
      <form
        role="dialog"
        aria-label={tt("sp.aria")}
        className="w-96 space-y-3 rounded-2xl border border-ink-600 bg-ink-800 p-5 shadow-2xl"
        onSubmit={(e) => { e.preventDefault(); void savePresetAs(slot, name.trim() || t("preset.default_name", { n: slot + 1 })); }}
      >
        <h2 className="text-[14px] font-semibold">{tt("sp.title")}</h2>
        <label className="block text-[11px] uppercase tracking-wider text-ink-300">
          {tt("sp.slot")}
          <select value={slot} onChange={(e) => setSlot(Number(e.target.value))} className="mt-1 w-full rounded-md border border-ink-600 bg-ink-900 px-2 py-1.5 text-[12px] normal-case tracking-normal text-ink-100 outline-none focus:border-pulse">
            {Array.from({ length: count }, (_, i) => (
              <option key={i} value={i}>{presetCode(i)} · {used.get(i) ?? tt("lib.free")}</option>
            ))}
          </select>
        </label>
        <label className="block text-[11px] uppercase tracking-wider text-ink-300">
          {tt("sp.name")}
          <input autoFocus maxLength={23} value={name} onChange={(e) => setName(e.target.value)} className="mt-1 w-full rounded-md border border-ink-600 bg-ink-900 px-2 py-1.5 text-[12px] normal-case tracking-normal text-ink-100 outline-none focus:border-pulse" />
        </label>
        {overwrite !== undefined && <p className="text-[12px] text-amber">{tt("sp.overwrite", { name: overwrite })}</p>}
        <div className="flex justify-end gap-2 pt-1">
          <button type="button" onClick={() => setSaveDialog(false)} className="rounded-md border border-ink-500 px-3 py-1.5 text-[12px] hover:border-ink-300">{tt("sp.cancel")}</button>
          <button type="submit" className="rounded-md bg-pulse px-3 py-1.5 text-[12px] font-semibold text-ink-950 hover:brightness-110">{tt("sp.save")}</button>
        </div>
      </form>
    </div>
  );
}
