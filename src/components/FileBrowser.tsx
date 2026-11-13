import { useMemo, useState } from "react";
import { api } from "../api";
import { useStore } from "../store";
import type { FileEntry, GearClass } from "../types";
import { useT } from "../i18n";

type Filter = "all" | "FULL_RIG" | "AMP_ONLY" | "ir";

export function ClassTag({ cls, gearType }: { cls: GearClass; gearType: string | null }) {
  if (cls === "FULL_RIG") return <span className="rounded border border-rig/50 bg-rig/10 px-1.5 py-px text-[9px] font-bold tracking-wide text-rig">[FULL RIG]</span>;
  if (cls === "AMP_ONLY") return <span className="rounded border border-amber/50 bg-amber/10 px-1.5 py-px text-[9px] font-bold tracking-wide text-amber">[AMP ONLY]</span>;
  return <span className="rounded border border-ink-500 px-1.5 py-px text-[9px] font-bold tracking-wide text-ink-300">[{(gearType ?? "?").toUpperCase()}]</span>;
}

const kb = (n: number) => (n >= 1e6 ? `${(n / 1e6).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1e3))} KB`);

function Row({ f, canSend, onOpen }: { f: FileEntry; canSend: boolean; onOpen: () => void }) {
  const t = useT();
  const send = useStore((s) => s.sendFile);
  const busy = useStore((s) => s.busy);
  if (f.kind === "dir")
    return (
      <button onClick={onOpen} className="flex w-full items-center gap-2 rounded-lg px-2 py-1.5 text-left hover:bg-ink-700">
        <span className="text-ink-300">▸</span>
        <span className="truncate">{f.name}</span>
      </button>
    );
  const label = f.name.replace(/\.(nam|wav|a2l)$/i, "");
  const sub = f.info ? [f.info.gear_make, f.info.gear_model].filter(Boolean).join(" ") : f.error ? t("fb.invalid") : "";
  return (
    <div className="group flex items-center gap-2 rounded-lg px-2 py-1.5 hover:bg-ink-700" title={f.error ?? f.path}>
      <div className="min-w-0 flex-1">
        <div className="flex items-center gap-1.5">
          <span className={`truncate ${f.error ? "text-ink-300 line-through" : ""}`}>{label}</span>
        </div>
        <div className="mt-0.5 flex items-center gap-1.5">
          {f.kind === "nam" && f.info && <ClassTag cls={f.info.class} gearType={f.info.gear_type} />}
          {f.kind === "wav" && <span className="rounded border border-sky-400/40 bg-sky-400/10 px-1.5 py-px text-[9px] font-bold tracking-wide text-sky-300">[IR]</span>}
          {f.kind === "a2l" && <span className="rounded border border-ink-500 px-1.5 py-px text-[9px] font-bold tracking-wide text-ink-200">[A2L]</span>}
          {f.kind === "nam" && f.error && <span className="rounded border border-danger/40 px-1.5 py-px text-[9px] font-bold text-danger">{t("fb.error_tag")}</span>}
          <span className="truncate text-[10px] text-ink-300">{sub || kb(f.size)}</span>
        </div>
      </div>
      <button
        disabled={!canSend || busy || !!f.error}
        onClick={() => send(f)}
        className="shrink-0 rounded-md border border-ink-500 px-2 py-1 text-[11px] font-medium text-ink-100 opacity-0 transition hover:border-pulse hover:text-pulse focus:opacity-100 disabled:cursor-not-allowed disabled:opacity-30 group-hover:opacity-100 group-hover:disabled:opacity-30"
        aria-label={t("fb.send_aria", { name: label })}
      >
        {t("fb.send")}
      </button>
    </div>
  );
}

function SlotSelect({ label, count, value, onChange }: { label: string; count: number; value: number | null; onChange: (v: number | null) => void }) {
  const t = useT();
  return (
    <label className="flex min-w-0 flex-1 flex-col gap-0.5 text-[10px] uppercase tracking-wider text-ink-300">
      {label}
      <select
        value={value ?? ""}
        onChange={(e) => onChange(e.target.value === "" ? null : Number(e.target.value))}
        className="rounded-md border border-ink-600 bg-ink-900 px-1.5 py-1 text-[11px] normal-case tracking-normal text-ink-100 outline-none focus:border-pulse"
      >
        <option value="">{t("fb.auto_slot")}</option>
        {Array.from({ length: count }, (_, i) => <option key={i} value={i}>{t("fb.slot", { n: i + 1 })}</option>)}
      </select>
    </label>
  );
}

export default function FileBrowser() {
  const t = useT();
  const listing = useStore((s) => s.listing);
  const openFolder = useStore((s) => s.openFolder);
  const connected = useStore((s) => s.snap?.status.state === "connected");
  const info = useStore((s) => s.info);
  const { captureSlot, irSlot, setSlots } = useStore.getState();
  const cs = useStore((s) => s.captureSlot);
  const is = useStore((s) => s.irSlot);
  const toast = useStore((s) => s.toast);
  const [filter, setFilter] = useState<Filter>("all");
  const [q, setQ] = useState("");
  void captureSlot; void irSlot;

  const entries = useMemo(() => {
    const ql = q.trim().toLowerCase();
    return (listing?.entries ?? []).filter((f) => {
      if (f.kind === "dir") return filter === "all" && (!ql || f.name.toLowerCase().includes(ql));
      if (ql && !f.name.toLowerCase().includes(ql)) return false;
      if (filter === "all") return true;
      if (filter === "ir") return f.kind === "wav";
      return f.kind === "nam" && f.info?.class === filter;
    });
  }, [listing, filter, q]);

  const open = (p: string) => openFolder(p).catch((e) => toast("error", String(e)));
  const chips: Array<[Filter, string]> = [["all", t("fb.all")], ["FULL_RIG", "Full rig"], ["AMP_ONLY", "Amp only"], ["ir", "IR"]];

  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="space-y-2 p-3">
        <div className="flex items-center gap-1.5">
          <button
            onClick={async () => { const p = await api.pickFolder(); if (p) open(p); }}
            className="rounded-md border border-ink-500 px-2 py-1 text-[11px] hover:border-pulse hover:text-pulse"
          >
            {t("fb.open_folder")}
          </button>
        </div>
        <div className="flex items-center gap-1">
          <button disabled={!listing?.parent} onClick={() => listing?.parent && open(listing.parent)} aria-label={t("fb.parent")} className="h-6 w-6 shrink-0 rounded border border-ink-600 text-ink-200 hover:border-ink-300 disabled:opacity-30">↑</button>
          <div dir="rtl" className="min-w-0 flex-1 truncate rounded bg-ink-900 px-2 py-1 font-mono text-[10px] text-ink-200" title={listing?.path}><bdo dir="ltr">{listing?.path ?? "…"}</bdo></div>
        </div>
        <input
          value={q}
          onChange={(e) => setQ(e.target.value)}
          placeholder={t("fb.filter_ph")}
          className="w-full rounded-md border border-ink-600 bg-ink-900 px-2 py-1.5 text-[12px] outline-none focus:border-pulse"
        />
        <div className="flex gap-1">
          {chips.map(([k, l]) => (
            <button key={k} onClick={() => setFilter(k)} className={`flex-1 rounded-md px-1 py-1 text-[11px] ${filter === k ? "bg-ink-600 text-white" : "text-ink-300 hover:bg-ink-700"}`}>{l}</button>
          ))}
        </div>
        <div className="flex gap-2">
          <SlotSelect label={t("fb.capture_to")} count={info?.capture_slots ?? 15} value={cs} onChange={(v) => setSlots(v, is)} />
          <SlotSelect label={t("fb.ir_to")} count={info?.ir_slots ?? 8} value={is} onChange={(v) => setSlots(cs, v)} />
        </div>
      </div>
      <div className="min-h-0 flex-1 space-y-px overflow-y-auto px-2 pb-3">
        {entries.length === 0 && <div className="p-4 text-center text-ink-300">{t("fb.empty")}</div>}
        {entries.map((f) => <Row key={f.path} f={f} canSend={!!connected} onOpen={() => open(f.path)} />)}
      </div>
    </div>
  );
}
