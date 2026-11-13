import { useStore } from "../store";

/** Live progress of the running pedal operation (upload, backup, restore, library...). */
export default function ProgressBar({ className = "" }: { className?: string }) {
  const progress = useStore((s) => s.progress);
  if (!progress) return null;
  const known = progress.total > 1;
  const pct = Math.round((progress.done / Math.max(1, progress.total)) * 100);
  return (
    <div className={className} role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={known ? pct : undefined} aria-label={progress.stage}>
      <div className="mb-1 flex justify-between gap-2 text-[11px] text-ink-200">
        <span className="truncate">{progress.stage}</span>
        <span className="font-mono text-ink-300">{known ? `${pct}%` : ""}</span>
      </div>
      <div className="h-1.5 overflow-hidden rounded-full bg-ink-700">
        <div
          className={`h-full rounded-full bg-pulse transition-all ${!known && progress.done === 0 ? "blink w-1/3" : ""}`}
          style={known || progress.done > 0 ? { width: `${pct}%` } : undefined}
        />
      </div>
    </div>
  );
}
