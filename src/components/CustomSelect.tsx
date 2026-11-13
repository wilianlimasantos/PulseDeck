import { useCallback, useEffect, useId, useLayoutEffect, useMemo, useRef, useState } from "react";
import { createPortal } from "react-dom";

export interface SelectOption<V extends string | number> {
  value: V;
  label: string;
  /** secondary text on the right (size, usage...) */
  hint?: string;
  disabled?: boolean;
}

interface Props<V extends string | number> {
  value: V | null;
  options: SelectOption<V>[];
  onChange: (v: V) => void;
  /** shown on the button when `value` matches no option */
  placeholder?: string;
  ariaLabel: string;
  disabled?: boolean;
  /** accent colour class for the focus ring / selected row, e.g. "amber" or "pulse" */
  accent?: "amber" | "pulse";
  className?: string;
}

const ACCENT = {
  amber: { ring: "focus-visible:border-amber", open: "border-amber", sel: "text-amber" },
  pulse: { ring: "focus-visible:border-pulse", open: "border-pulse", sel: "text-pulse" },
} as const;

interface Pos { left: number; width: number; top?: number; bottom?: number; maxHeight: number }

/**
 * Dark-themed dropdown (the native <select> popup is drawn by the OS and ignores the palette).
 * The list is a fixed-position portal, so it is never clipped by a column and flips upward when
 * there is no room below. Keyboard: Up/Down/Home/End move, Enter/Space choose, Esc closes.
 */
export default function CustomSelect<V extends string | number>({
  value, options, onChange, placeholder = "—", ariaLabel, disabled, accent = "pulse", className = "",
}: Props<V>) {
  const [open, setOpen] = useState(false);
  const [active, setActive] = useState(0);
  const [pos, setPos] = useState<Pos | null>(null);
  const btn = useRef<HTMLButtonElement>(null);
  const list = useRef<HTMLUListElement>(null);
  const id = useId();
  const a = ACCENT[accent];

  const selected = useMemo(() => options.findIndex((o) => o.value === value), [options, value]);
  const enabled = useMemo(() => options.map((o, i) => (o.disabled ? -1 : i)).filter((i) => i >= 0), [options]);

  const place = useCallback(() => {
    const r = btn.current?.getBoundingClientRect();
    if (!r) return;
    const gap = 4, margin = 8, want = 280;
    const below = window.innerHeight - r.bottom - margin;
    const above = r.top - margin;
    const down = below >= Math.min(want, 160) || below >= above;
    const maxHeight = Math.max(120, Math.min(want, (down ? below : above) - gap));
    setPos({
      left: Math.max(margin, Math.min(r.left, window.innerWidth - r.width - margin)),
      width: r.width,
      maxHeight,
      ...(down ? { top: r.bottom + gap } : { bottom: window.innerHeight - r.top + gap }),
    });
  }, []);

  const close = useCallback((refocus = true) => {
    setOpen(false);
    if (refocus) btn.current?.focus();
  }, []);

  const openList = () => {
    if (disabled) return;
    setActive(selected >= 0 && !options[selected].disabled ? selected : (enabled[0] ?? 0));
    place();
    setOpen(true);
  };

  const choose = (i: number) => {
    const o = options[i];
    if (!o || o.disabled) return;
    onChange(o.value);
    close();
  };

  useLayoutEffect(() => {
    if (!open) return;
    list.current?.querySelector<HTMLElement>(`[data-i="${active}"]`)?.scrollIntoView({ block: "nearest" });
  }, [open, active]);

  useEffect(() => {
    if (!open) return;
    const outside = (e: MouseEvent) => {
      const t = e.target as Node;
      if (!btn.current?.contains(t) && !list.current?.contains(t)) close(false);
    };
    const reflow = () => place();
    window.addEventListener("mousedown", outside);
    window.addEventListener("resize", reflow);
    window.addEventListener("scroll", reflow, true);
    return () => {
      window.removeEventListener("mousedown", outside);
      window.removeEventListener("resize", reflow);
      window.removeEventListener("scroll", reflow, true);
    };
  }, [open, close, place]);

  const move = (delta: number) => {
    if (enabled.length === 0) return;
    const at = Math.max(0, enabled.indexOf(active));
    setActive(enabled[(at + delta + enabled.length) % enabled.length]);
  };

  const onKeyDown = (e: React.KeyboardEvent) => {
    if (!open) {
      if (e.key === "ArrowDown" || e.key === "ArrowUp" || e.key === "Enter" || e.key === " ") {
        e.preventDefault();
        openList();
      }
      return;
    }
    switch (e.key) {
      case "Escape": e.preventDefault(); e.stopPropagation(); close(); break;
      case "ArrowDown": e.preventDefault(); move(1); break;
      case "ArrowUp": e.preventDefault(); move(-1); break;
      case "Home": e.preventDefault(); setActive(enabled[0] ?? 0); break;
      case "End": e.preventDefault(); setActive(enabled[enabled.length - 1] ?? 0); break;
      case "Enter": case " ": e.preventDefault(); choose(active); break;
      case "Tab": close(false); break;
    }
  };

  const current = selected >= 0 ? options[selected] : null;

  return (
    <>
      <button
        ref={btn}
        type="button"
        role="combobox"
        aria-label={ariaLabel}
        aria-haspopup="listbox"
        aria-expanded={open}
        aria-controls={open ? id : undefined}
        disabled={disabled}
        onClick={() => (open ? close(false) : openList())}
        onKeyDown={onKeyDown}
        className={`flex w-full items-center gap-2 rounded-md border bg-ink-900 px-2 py-1.5 text-left text-[12px] normal-case tracking-normal text-ink-100 outline-none transition disabled:cursor-not-allowed disabled:opacity-40 ${
          open ? a.open : `border-ink-600 hover:border-ink-400 ${a.ring}`
        } ${className}`}
      >
        <span className={`min-w-0 flex-1 truncate ${current ? "" : "text-ink-300"}`}>{current?.label ?? placeholder}</span>
        <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden className={`shrink-0 text-ink-300 transition ${open ? "rotate-180" : ""}`}>
          <path d="M1.5 3.5 5 7l3.5-3.5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
        </svg>
      </button>
      {open && pos &&
        createPortal(
          <ul
            ref={list}
            id={id}
            role="listbox"
            aria-label={ariaLabel}
            style={{ left: pos.left, width: pos.width, top: pos.top, bottom: pos.bottom, maxHeight: pos.maxHeight }}
            className="fixed z-[110] overflow-y-auto rounded-lg border border-ink-600 bg-ink-800 p-1 shadow-2xl"
          >
            {options.length === 0 && <li className="px-2 py-1.5 text-[12px] text-ink-300">Nada para escolher.</li>}
            {options.map((o, i) => (
              <li
                key={String(o.value)}
                data-i={i}
                role="option"
                aria-selected={i === selected}
                aria-disabled={o.disabled || undefined}
                onMouseEnter={() => !o.disabled && setActive(i)}
                onMouseDown={(e) => e.preventDefault()}
                onClick={() => choose(i)}
                className={`flex cursor-pointer items-center gap-2 rounded-md px-2 py-1.5 text-[12px] ${
                  o.disabled ? "cursor-not-allowed text-ink-500" : i === active ? "bg-ink-600 text-white" : "text-ink-100"
                } ${i === selected ? a.sel : ""}`}
              >
                <span className="min-w-0 flex-1 truncate">{o.label}</span>
                {o.hint && <span className="shrink-0 font-mono text-[10px] text-ink-300">{o.hint}</span>}
                {i === selected && <span aria-hidden>✓</span>}
              </li>
            ))}
          </ul>,
          document.body,
        )}
    </>
  );
}
