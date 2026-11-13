import { useStore } from "../store";
import { categoryHue, formatParam, paramLabel } from "../fx";
import type { Block, EffectType } from "../types";
import Knob from "./Knob";
import { useT } from "../i18n";

interface Props {
  block: Block;
  effect: EffectType | undefined;
  hint: "before" | "after" | null;
  onDragStart: () => void;
  onDragEnd: () => void;
  onDragOver: (e: React.DragEvent) => void;
  onDrop: (e: React.DragEvent) => void;
  onMove: (dir: -1 | 1) => void;
  canLeft: boolean;
  canRight: boolean;
}

export default function BlockCard({ block, effect, hint, onDragStart, onDragEnd, onDragOver, onDrop, onMove, canLeft, canRight }: Props) {
  const t = useT();
  const { setParam, setBlockOn, removeBlock } = useStore.getState();
  const irs = useStore((s) => s.snap?.irs.entries ?? []);
  const hue = categoryHue(effect?.category ?? "");
  const params = effect?.parameters ?? block.params.map((_, i) => ({ index: i, name: `p${i}`, min: 0, max: 100, default: 0 }));
  const irSlotIdx = effect?.name === "ir" ? effect.parameters.findIndex((p) => p.name === "ir_slot") : -1;
  const irName =
    irSlotIdx >= 0 ? (block.params[irSlotIdx] > 0 ? irs.find((e) => e.slot === block.params[irSlotIdx] - 1)?.name ?? t("block.slot", { n: block.params[irSlotIdx] }) : t("block.preset_ir")) : null;

  return (
    <div
      draggable
      onDragStart={(e) => {
        e.dataTransfer.effectAllowed = "move";
        e.dataTransfer.setData("text/plain", String(block.id));
        onDragStart();
      }}
      onDragEnd={onDragEnd}
      onDragOver={onDragOver}
      onDrop={onDrop}
      style={{ ["--hue" as string]: `oklch(0.75 0.14 ${hue})` }}
      className={`group relative flex min-h-[148px] flex-col rounded-xl border bg-ink-800 transition-opacity ${
        block.on ? "card-glow-on border-transparent" : "border-ink-600 opacity-70"
      }`}
    >
      {hint && (
        <div className={`absolute inset-y-1 w-1 rounded bg-pulse ${hint === "before" ? "-left-2" : "-right-2"}`} />
      )}
      <div className="flex items-center gap-1.5 px-2 pt-2">
        <button
          aria-label={block.on ? t("block.off") : t("block.on")}
          aria-pressed={block.on}
          onClick={() => setBlockOn(block.id, !block.on)}
          className={`h-4 w-4 shrink-0 rounded-full border transition ${
            block.on ? "border-pulse bg-pulse shadow-[0_0_8px_var(--color-pulse)]" : "border-ink-500 bg-ink-700 hover:border-ink-300"
          }`}
        />
        <div className="min-w-0 flex-1 cursor-grab truncate text-[11px] font-semibold uppercase tracking-wide" title={block.type_name}>
          {block.type_name}
        </div>
        {block.lane > 0 && (
          <span className="rounded bg-ink-600 px-1 text-[9px] text-ink-200" title={t("block.lane")}>
            {block.lane === 1 ? "A" : "B"}
          </span>
        )}
        <div className="flex items-center opacity-0 transition group-hover:opacity-100 focus-within:opacity-100">
          <button aria-label={t("block.left")} disabled={!canLeft} onClick={() => onMove(-1)} className="px-0.5 text-ink-300 hover:text-white disabled:opacity-25">‹</button>
          <button aria-label={t("block.right")} disabled={!canRight} onClick={() => onMove(1)} className="px-0.5 text-ink-300 hover:text-white disabled:opacity-25">›</button>
          <button aria-label={t("block.remove")} onClick={() => removeBlock(block.id)} className="px-1 text-ink-300 hover:text-danger">✕</button>
        </div>
      </div>
      {irName && <div className="truncate px-2 pt-0.5 text-[10px] text-ink-300">{irName}</div>}
      <div className="flex flex-wrap justify-center gap-x-0.5 gap-y-2 px-1 pb-2.5 pt-2">
        {params.map((p) => (
          <Knob
            key={p.index}
            label={paramLabel(p.name)}
            value={block.params[p.index] ?? p.default}
            min={p.min}
            max={p.max}
            def={p.default}
            display={formatParam(p, block.params[p.index] ?? p.default)}
            hue={hue}
            size={42}
            disabled={!block.on}
            onChange={(v) => setParam(block, p.index, v)}
          />
        ))}
      </div>
    </div>
  );
}
