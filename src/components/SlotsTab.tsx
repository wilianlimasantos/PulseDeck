import { useRigStore } from "../rigStore";
import { useStore } from "../store";
import SlotsPanel from "./SlotsPanel";
import { useT } from "../i18n";

/** SLOTS tab: the 15 capture and 8 IR slots of the pedal, managed with one click each. */
export default function SlotsTab() {
  const t = useT();
  const usage = useRigStore((s) => s.usage);
  const { loadUsage, setMemoryOpen } = useRigStore.getState();
  const busy = useStore((s) => s.busy);
  const connected = useStore((s) => s.snap?.status.state === "connected");
  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="flex items-center gap-1.5 p-3">
        <button
          disabled={busy || !connected}
          onClick={() => void loadUsage()}
          title={t("slots.usage_hint")}
          className="flex-1 rounded-md border border-ink-500 px-2 py-1.5 text-[11px] hover:border-pulse hover:text-pulse disabled:opacity-40"
        >
          {usage ? t("slots.reanalyze") : t("slots.scan")}
        </button>
        <button onClick={() => setMemoryOpen(true)} title={t("slots.fullscreen")} aria-label={t("slots.fullscreen")} className="rounded-md border border-ink-500 px-2 py-1.5 text-[11px] hover:border-pulse hover:text-pulse">⤢</button>
      </div>
      <div className="min-h-0 flex-1 space-y-5 overflow-y-auto px-2 pb-3">
        <SlotsPanel compact />
      </div>
    </div>
  );
}
