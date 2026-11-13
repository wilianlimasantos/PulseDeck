import { useState } from "react";
import { useStore } from "../store";
import type { Block } from "../types";
import AddBlock from "./AddBlock";
import AmpCard from "./AmpCard";
import BlockCard from "./BlockCard";
import { useT } from "../i18n";

type Bank = "P" | "Q";
interface Hint { bank: Bank; index: number; side: "before" | "after" }

function Bank({ bank, title, subtitle, blocks, drag, setDrag }: {
  bank: Bank; title: string; subtitle: string; blocks: Block[];
  drag: number | null; setDrag: (id: number | null) => void;
}) {
  const effects = useStore((s) => s.snap?.effects ?? []);
  const all = useStore((s) => s.snap?.chain.blocks ?? []);
  const moveBlock = useStore((s) => s.moveBlock);
  const [hint, setHint] = useState<Hint | null>(null);

  /** insertion index in this bank (before removing the dragged block) -> final index */
  const drop = (insertAt: number) => {
    const src = all.find((b) => b.id === drag);
    setHint(null);
    setDrag(null);
    if (!src) return;
    let target = insertAt;
    if (src.bank === bank && src.position < insertAt) target -= 1;
    if (src.bank === bank && target === src.position) return;
    void moveBlock(src.id, bank, target);
  };

  return (
    <section className="flex min-h-0 min-w-0 flex-col">
      <header className="mb-2 flex items-baseline gap-2 px-1">
        <h2 className="text-[11px] font-semibold uppercase tracking-[0.18em] text-ink-100">{title}</h2>
        <span className="text-[11px] text-ink-300">{subtitle}</span>
        <span className="ml-auto font-mono text-[10px] text-ink-300">{blocks.length}/10</span>
      </header>
      <div
        className="grid min-h-0 flex-1 auto-rows-min content-start gap-x-4 gap-y-3 overflow-y-auto px-2 pb-6 pt-1"
        style={{ gridTemplateColumns: "repeat(auto-fill, minmax(176px, 1fr))" }}
        onDragOver={(e) => drag !== null && e.preventDefault()}
        onDrop={(e) => { e.preventDefault(); drop(blocks.length); }}
      >
        {blocks.map((b, i) => (
          <BlockCard
            key={b.id}
            block={b}
            effect={effects.find((e) => e.name === b.type_name)}
            hint={hint && hint.bank === bank && hint.index === i ? hint.side : null}
            canLeft={i > 0}
            canRight={i < blocks.length - 1}
            onMove={(dir) => void moveBlock(b.id, bank, i + dir)}
            onDragStart={() => setDrag(b.id)}
            onDragEnd={() => { setDrag(null); setHint(null); }}
            onDragOver={(e) => {
              if (drag === null || drag === b.id) return;
              e.preventDefault();
              e.stopPropagation();
              const r = e.currentTarget.getBoundingClientRect();
              setHint({ bank, index: i, side: e.clientX < r.left + r.width / 2 ? "before" : "after" });
            }}
            onDrop={(e) => {
              e.preventDefault();
              e.stopPropagation();
              if (drag === b.id) return drop(i);
              drop(hint && hint.index === i && hint.side === "after" ? i + 1 : i);
            }}
          />
        ))}
        <AddBlock bank={bank} />
      </div>
    </section>
  );
}

/** The amp / capture block was removed from the preset: AMPX 1 puts it back at the P/Q boundary. */
function AddAmp() {
  const t = useT();
  const setAmpPresent = useStore((s) => s.setAmpPresent);
  return (
    <button
      onClick={() => void setAmpPresent(true)}
      className="flex min-h-[220px] w-full flex-col items-center justify-center gap-2 rounded-2xl border border-dashed border-amber/50 bg-amber/5 px-4 text-center text-amber transition hover:border-amber hover:bg-amber/10"
    >
      <span className="text-3xl leading-none">+</span>
      <span className="text-[13px] font-semibold">{t("chain.add_amp")}</span>
      <span className="text-[11px] text-ink-300">{t("chain.add_amp_hint")}</span>
    </button>
  );
}

export default function SignalChain() {
  const t = useT();
  const snap = useStore((s) => s.snap)!;
  const [drag, setDrag] = useState<number | null>(null);
  const P = snap.chain.blocks.filter((b) => b.bank === "P").sort((a, b) => a.position - b.position);
  const Q = snap.chain.blocks.filter((b) => b.bank === "Q").sort((a, b) => a.position - b.position);

  return (
    <div className="flex h-full min-h-0 flex-col px-4 pb-0 pt-3">
      <div className="mb-3 flex items-center gap-2 text-[10px] font-semibold uppercase tracking-[0.2em] text-ink-300">
        <span className="rounded border border-ink-600 px-1.5 py-0.5">{t("chain.in")}</span>
        <div className="signal-line h-[2px] flex-1" />
        <span>{t("chain.pre")}</span>
        <div className="signal-line h-[2px] w-10" />
        <span className="text-amber">{t("chain.amp")}</span>
        <div className="signal-line h-[2px] w-10" />
        <span>{t("chain.post")}</span>
        <div className="signal-line h-[2px] flex-1" />
        <span className="rounded border border-ink-600 px-1.5 py-0.5">{t("chain.out")}</span>
      </div>
      <div className="grid min-h-0 flex-1 gap-4" style={{ gridTemplateColumns: "minmax(0,1fr) 296px minmax(0,1fr)" }}>
        <Bank bank="P" title={t("chain.bank_p")} subtitle={t("chain.before_amp")} blocks={P} drag={drag} setDrag={setDrag} />
        <section className="flex min-h-0 flex-col">
          <header className="mb-2 px-1 text-[11px] font-semibold uppercase tracking-[0.18em] text-amber">{t("chain.amp")}</header>
          <div className="min-h-0 flex-1 overflow-y-auto px-1 pb-6 pt-1">
            {snap.chain.amp_present ? <AmpCard /> : <AddAmp />}
          </div>
        </section>
        <Bank bank="Q" title={t("chain.bank_q")} subtitle={t("chain.after_amp")} blocks={Q} drag={drag} setDrag={setDrag} />
      </div>
    </div>
  );
}
