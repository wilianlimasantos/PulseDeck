import { useEffect, useMemo, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { useStore } from "../store";
import { categoryHue, categoryLabel, isAmpCategory } from "../fx";
import { useT } from "../i18n";

/** Effect picker: a centered modal (portal to <body>, position: fixed) so it is never clipped by the column. */
function Picker({ bank, onClose }: { bank: "P" | "Q"; onClose: () => void }) {
  const t = useT();
  const effects = useStore((s) => s.snap?.effects ?? []);
  const addBlock = useStore((s) => s.addBlock);
  const [q, setQ] = useState("");
  const search = useRef<HTMLInputElement>(null);

  useEffect(() => {
    search.current?.focus();
    const esc = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.stopPropagation();
        onClose();
      }
    };
    window.addEventListener("keydown", esc, true);
    return () => window.removeEventListener("keydown", esc, true);
  }, [onClose]);

  const groups = useMemo(() => {
    const m = new Map<string, typeof effects>();
    for (const e of effects) {
      if (isAmpCategory(e.category)) continue; // amps live only in the central AMP column
      if (q && !e.name.includes(q.trim().toLowerCase())) continue;
      m.set(e.category, [...(m.get(e.category) ?? []), e]);
    }
    return [...m.entries()];
  }, [effects, q]);

  return createPortal(
    <div
      className="fixed inset-0 z-[100] flex items-center justify-center bg-black/70 p-6 backdrop-blur-[2px]"
      onMouseDown={(e) => e.target === e.currentTarget && onClose()}
    >
      <div
        role="dialog"
        aria-modal="true"
        aria-label={t("add.aria", { bank })}
        className="flex max-h-full w-full max-w-md flex-col rounded-2xl border border-ink-600 bg-ink-800 p-4 shadow-2xl"
      >
        <div className="mb-3 flex items-center justify-between">
          <h2 className="text-[13px] font-semibold">
            {t("add.title")} <span className="text-ink-300">· {t("add.bank", { bank })}</span>
          </h2>
          <button aria-label={t("g.close")} onClick={onClose} className="px-1 text-ink-300 hover:text-white">✕</button>
        </div>
        <input
          ref={search}
          value={q}
          onChange={(e) => setQ(e.target.value)}
          placeholder={t("add.search")}
          className="mb-3 w-full rounded-md border border-ink-600 bg-ink-900 px-3 py-2 outline-none focus:border-pulse"
        />
        <div className="min-h-0 flex-1 overflow-y-auto pr-1">
          {groups.length === 0 && <div className="p-2 text-ink-300">{t("add.none")}</div>}
          {groups.map(([cat, list]) => (
            <div key={cat} className="mb-3">
              <div className="px-1 pb-1 text-[10px] uppercase tracking-wider text-ink-300">{categoryLabel(cat)}</div>
              <div className="flex flex-wrap gap-1.5">
                {list.map((e) => (
                  <button
                    key={e.id}
                    onClick={() => {
                      onClose();
                      void addBlock(bank, e.name);
                    }}
                    style={{ borderColor: `oklch(0.6 0.12 ${categoryHue(cat)} / .6)` }}
                    className="rounded-md border bg-ink-700 px-2.5 py-1.5 text-[12px] hover:bg-ink-600"
                  >
                    {e.name}
                  </button>
                ))}
              </div>
            </div>
          ))}
        </div>
      </div>
    </div>,
    document.body,
  );
}

export default function AddBlock({ bank }: { bank: "P" | "Q" }) {
  const t = useT();
  const [open, setOpen] = useState(false);
  return (
    <div className="min-h-[148px]">
      <button
        onClick={() => setOpen(true)}
        aria-haspopup="dialog"
        className="flex h-full min-h-[148px] w-full flex-col items-center justify-center gap-1 rounded-xl border border-dashed border-ink-500 text-ink-300 transition hover:border-pulse hover:text-pulse"
      >
        <span className="text-2xl leading-none">+</span>
        <span className="text-[11px]">{t("add.title")}</span>
      </button>
      {open && <Picker bank={bank} onClose={() => setOpen(false)} />}
    </div>
  );
}
