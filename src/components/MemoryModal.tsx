import { useEffect, useRef } from "react";
import { createPortal } from "react-dom";
import { useRigStore } from "../rigStore";
import { useStore } from "../store";
import ProgressBar from "./ProgressBar";
import SlotsPanel from "./SlotsPanel";
import { useT } from "../i18n";

/** "Memória da Pedaleira": the 15 capture and 8 IR slots as stored on the Tank-G. */
export default function MemoryModal() {
  const t = useT();
  const open = useRigStore((s) => s.memoryOpen);
  const usage = useRigStore((s) => s.usage);
  const { setMemoryOpen, loadUsage } = useRigStore.getState();
  const snap = useStore((s) => s.snap);
  const busy = useStore((s) => s.busy);
  const close = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    if (!open) return;
    close.current?.focus();
    const esc = (e: KeyboardEvent) => {
      if (e.key === "Escape" && !(e.target as HTMLElement | null)?.closest("input")) {
        e.stopPropagation();
        setMemoryOpen(false);
      }
    };
    window.addEventListener("keydown", esc, true);
    return () => window.removeEventListener("keydown", esc, true);
  }, [open, setMemoryOpen]);

  if (!open || !snap) return null;
  return createPortal(
    <div
      className="fixed inset-0 z-[100] flex items-center justify-center bg-black/70 p-6 backdrop-blur-[2px]"
      onMouseDown={(e) => e.target === e.currentTarget && !busy && setMemoryOpen(false)}
    >
      <div role="dialog" aria-modal="true" aria-label={t("mem.aria")} className="flex max-h-full w-full max-w-5xl flex-col rounded-2xl border border-ink-600 bg-ink-900 shadow-2xl">
        <header className="flex items-center gap-3 border-b border-ink-700 px-5 py-3">
          <h2 className="text-[14px] font-semibold">{t("mem.title")}</h2>
          <span className="text-[11px] text-ink-300">{snap.status.state === "connected" ? t("mem.live") : t("mem.disconnected")}</span>
          <button
            disabled={busy || snap.status.state !== "connected"}
            onClick={() => void loadUsage()}
            title={t("slots.usage_hint")}
            className="ml-auto rounded-md border border-ink-500 px-2.5 py-1 text-[11px] hover:border-pulse hover:text-pulse disabled:opacity-40"
          >
            {usage ? t("slots.reanalyze") : t("slots.scan")}
          </button>
          <button ref={close} aria-label={t("g.close")} onClick={() => setMemoryOpen(false)} disabled={busy} className="px-1 text-ink-300 hover:text-white disabled:opacity-40">✕</button>
        </header>
        <ProgressBar className="border-b border-ink-700 px-5 py-2" />
        <div className="grid min-h-0 flex-1 gap-6 overflow-y-auto p-5 lg:grid-cols-[3fr_2fr]">
          <SlotsPanel />
        </div>
      </div>
    </div>,
    document.body,
  );
}
