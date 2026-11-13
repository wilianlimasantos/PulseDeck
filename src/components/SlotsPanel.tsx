import { useEffect, useRef, useState } from "react";
import { presetCode } from "../fx";
import { useRigStore } from "../rigStore";
import { useStore } from "../store";
import type { LibEntry, Library, SlotKind } from "../types";
import { useT } from "../i18n";

function Icon({ d }: { d: string }) {
  return (
    <svg width="14" height="14" viewBox="0 0 16 16" aria-hidden fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <path d={d} />
    </svg>
  );
}

const kb = (n: number) => `${Math.max(1, Math.round(n / 1024))} KB`;
const pad = (n: number) => String(n + 1).padStart(2, "0");

interface RowProps {
  /** narrow layout for the sidebar: icon buttons instead of text buttons */
  compact?: boolean;
  kind: SlotKind;
  slot: number;
  entry: LibEntry | undefined;
  loaded: boolean;
  /** preset slots using this slot, or null when usage has not been scanned yet */
  usedBy: number[] | null;
}

function SlotRow({ kind, slot, entry, loaded, usedBy, compact = false }: RowProps) {
  const t = useT();
  const { renameSlot, deleteSlot, sendToSlot } = useRigStore.getState();
  const busy = useStore((s) => s.busy);
  const connected = useStore((s) => s.snap?.status.state === "connected");
  const presets = useStore((s) => s.snap?.presets.presets ?? []);
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState("");
  const disabled = busy || !connected;
  const input = useRef<HTMLInputElement>(null);

  useEffect(() => { if (editing) input.current?.select(); }, [editing]);

  const startEdit = () => { setDraft(entry?.name ?? ""); setEditing(true); };
  const commit = async () => {
    const n = draft.trim();
    setEditing(false);
    if (n && entry && n !== entry.name) await renameSlot(kind, slot, n);
  };
  const preset = (p: number) => presets.find((x) => x.slot === p)?.name ?? "";

  const ico = "grid h-7 w-7 place-items-center rounded-md border border-ink-600 text-ink-200 transition hover:border-pulse hover:text-pulse disabled:cursor-not-allowed disabled:opacity-30";
  const btn = "rounded-md border border-ink-500 px-2 py-1 text-[11px] text-ink-100 transition hover:border-pulse hover:text-pulse disabled:cursor-not-allowed disabled:opacity-30";
  return (
    <li className={`flex items-start ${compact ? "gap-2 px-2 py-1.5" : "gap-3 px-3 py-2"} rounded-lg border ${entry ? "border-ink-600 bg-ink-800" : "border-dashed border-ink-700 bg-ink-850"}`}>
      <span className="mt-0.5 w-6 shrink-0 font-mono text-[12px] text-ink-300">{pad(slot)}</span>
      <div className="min-w-0 flex-1">
        {editing ? (
          <input
            ref={input}
            value={draft}
            maxLength={23}
            aria-label={t("sl.new_name", { n: slot + 1 })}
            onChange={(e) => setDraft(e.target.value)}
            onBlur={() => void commit()}
            onKeyDown={(e) => {
              if (e.key === "Enter") void commit();
              if (e.key === "Escape") { e.stopPropagation(); setEditing(false); }
            }}
            className="w-full rounded-md border border-pulse bg-ink-900 px-2 py-1 text-[12px] outline-none"
          />
        ) : (
          <div className="flex items-center gap-1.5">
            <span className={`truncate text-[13px] ${entry ? "font-medium" : "text-ink-500"}`} title={entry?.name}>{entry?.name ?? t("sl.free")}</span>
            {loaded && <span className="rounded border border-pulse/50 bg-pulse/10 px-1.5 py-px text-[9px] font-bold tracking-wide text-pulse">{t("sl.loaded")}</span>}
          </div>
        )}
        {entry && (
          <div className="mt-0.5 flex flex-wrap items-center gap-x-2 gap-y-1 text-[10px] text-ink-300">
            <span>{kb(entry.bytes)}</span>
            {entry.trim_db !== null && <span>trim {entry.trim_db} dB</span>}
            {entry.input_dbu !== null && <span>in {entry.input_dbu} dBu</span>}
            {usedBy !== null && (usedBy.length === 0
              ? <span className="text-ink-500">{t("sl.unused")}</span>
              : usedBy.map((p) => (
                  <span key={p} title={preset(p)} className="rounded border border-ink-500 px-1.5 py-px font-mono text-[9px] text-ink-100">{presetCode(p)}</span>
                )))}
          </div>
        )}
      </div>
      {compact ? (
        <div className="flex shrink-0 gap-1">
          {entry && <button disabled={disabled} onClick={startEdit} title={t("sl.rename")} aria-label={t("sl.rename_aria", { n: slot + 1 })} className={ico}><Icon d="M3 13l1-3.5L10.5 3l2.5 2.5L6.5 12z M9 4.5l2.5 2.5" /></button>}
          <button disabled={disabled} onClick={() => void sendToSlot(kind, slot, entry?.name ?? null)} title={entry ? t("sl.replace_title") : t("sl.send_title")} aria-label={entry ? t("sl.replace_aria", { n: slot + 1 }) : t("sl.send_aria", { n: slot + 1 })} className={ico}><Icon d="M8 11V3 M4.5 6.5L8 3l3.5 3.5 M3 13h10" /></button>
          {entry && <button disabled={disabled} onClick={() => void deleteSlot(kind, slot, entry.name)} title={t("sl.delete")} aria-label={t("sl.delete_aria", { n: slot + 1 })} className={`${ico} hover:!border-danger hover:!text-danger`}><Icon d="M3.5 4.5h9 M6.5 4.5V3h3v1.5 M4.5 4.5l.6 8h5.8l.6-8" /></button>}
        </div>
      ) : (
        <div className="flex shrink-0 gap-1.5">
          {entry && <button disabled={disabled} onClick={startEdit} className={btn}>{t("sl.rename")}</button>}
          <button disabled={disabled} onClick={() => void sendToSlot(kind, slot, entry?.name ?? null)} className={btn}>{entry ? t("sl.replace") : t("sl.send_title")}</button>
          {entry && <button disabled={disabled} onClick={() => void deleteSlot(kind, slot, entry.name)} aria-label={t("sl.delete_aria", { n: slot + 1 })} className={`${btn} hover:!border-danger hover:!text-danger`}>{t("sl.delete")}</button>}
        </div>
      )}
    </li>
  );
}

export function SlotsSection({ title, kind, count, lib, usage, compact = false }: { title: string; kind: SlotKind; count: number; lib: Library; usage: Record<string, number[]> | null; compact?: boolean }) {
  const t = useT();
  const used = lib.entries.length;
  return (
    <section aria-label={title} className="min-w-0">
      <h3 className="mb-2 flex items-baseline gap-2 text-[11px] font-semibold uppercase tracking-[0.18em] text-ink-200">
        {title} <span className="font-normal normal-case tracking-normal text-ink-300">{t("sl.occupied", { used, count })}</span>
      </h3>
      <ul className="space-y-1.5">
        {Array.from({ length: count }, (_, slot) => (
          <SlotRow
            compact={compact}
            key={slot}
            kind={kind}
            slot={slot}
            entry={lib.entries.find((e) => e.slot === slot)}
            loaded={lib.current === slot}
            usedBy={usage ? (usage[String(slot)] ?? []) : null}
          />
        ))}
      </ul>
    </section>
  );
}


/** Capture + IR slot lists with the usage scan, shared by the SLOTS sidebar tab and the modal. */
export default function SlotsPanel({ compact = false }: { compact?: boolean }) {
  const t = useT();
  const snap = useStore((s) => s.snap);
  const info = useStore((s) => s.info);
  const usage = useRigStore((s) => s.usage);
  if (!snap) return null;
  return (
    <>
      <SlotsSection compact={compact} title={t("sl.captures")} kind="capture" count={info?.capture_slots ?? 15} lib={snap.captures} usage={usage?.captures ?? null} />
      <SlotsSection compact={compact} title={t("sl.irs")} kind="ir" count={info?.ir_slots ?? 8} lib={snap.irs} usage={usage?.irs ?? null} />
    </>
  );
}
