import { useEffect, useRef } from "react";

interface Props {
  label: string;
  value: number;
  min: number;
  max: number;
  def: number;
  display: string;
  hue?: number;
  size?: number;
  disabled?: boolean;
  onChange: (v: number) => void;
}

const START = -135;
const SWEEP = 270;

function polar(cx: number, cy: number, r: number, deg: number) {
  const a = ((deg - 90) * Math.PI) / 180;
  return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
}
function arc(cx: number, cy: number, r: number, from: number, to: number) {
  const [x1, y1] = polar(cx, cy, r, from);
  const [x2, y2] = polar(cx, cy, r, to);
  return `M ${x1} ${y1} A ${r} ${r} 0 ${to - from > 180 ? 1 : 0} 1 ${x2} ${y2}`;
}

/** Rotary control: vertical drag (shift = fine), wheel, arrow keys, double-click = default. */
export default function Knob({ label, value, min, max, def, display, hue = 170, size = 46, disabled, onChange }: Props) {
  const ref = useRef<HTMLDivElement>(null);
  const drag = useRef<{ y: number; v: number } | null>(null);
  const latest = useRef({ value, min, max, onChange, disabled });
  latest.current = { value, min, max, onChange, disabled };

  const clamp = (v: number) => Math.min(max, Math.max(min, Math.round(v)));
  const frac = max > min ? (value - min) / (max - min) : 0;
  const angle = START + frac * SWEEP;
  const c = size / 2;
  const r = c - 5;
  const color = disabled ? "var(--color-ink-500)" : `oklch(0.8 0.15 ${hue})`;

  // wheel needs a non-passive listener to stop the surrounding panel from scrolling
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const onWheel = (e: WheelEvent) => {
      const { value, min, max, onChange, disabled } = latest.current;
      if (disabled) return;
      e.preventDefault();
      const step = Math.max(1, Math.round((max - min) / 100)) * (e.shiftKey ? 1 : 2);
      const nv = Math.min(max, Math.max(min, value + (e.deltaY < 0 ? step : -step)));
      if (nv !== value) onChange(nv);
    };
    el.addEventListener("wheel", onWheel, { passive: false });
    return () => el.removeEventListener("wheel", onWheel);
  }, []);

  return (
    <div className="flex flex-col items-center gap-0.5" style={{ width: size + 14 }} title={`${label}: ${display}`}>
      <div
        ref={ref}
        role="slider"
        aria-label={label}
        aria-valuemin={min}
        aria-valuemax={max}
        aria-valuenow={value}
        aria-valuetext={display}
        tabIndex={disabled ? -1 : 0}
        className={`touch-none ${disabled ? "cursor-not-allowed opacity-60" : "cursor-ns-resize"} rounded-full`}
        style={{ width: size, height: size }}
        onPointerDown={(e) => {
          if (disabled) return;
          e.currentTarget.setPointerCapture(e.pointerId);
          drag.current = { y: e.clientY, v: value };
        }}
        onPointerMove={(e) => {
          const d = drag.current;
          if (!d) return;
          const per = (max - min) / (e.shiftKey ? 900 : 200);
          const nv = clamp(d.v + (d.y - e.clientY) * per);
          if (nv !== latest.current.value) onChange(nv);
        }}
        onPointerUp={() => (drag.current = null)}
        onPointerCancel={() => (drag.current = null)}
        onDoubleClick={() => !disabled && onChange(clamp(def))}
        onKeyDown={(e) => {
          if (disabled) return;
          const big = Math.max(1, Math.round((max - min) / 20));
          const k: Record<string, number> = { ArrowUp: 1, ArrowDown: -1, PageUp: big, PageDown: -big };
          if (k[e.key] !== undefined) {
            e.preventDefault();
            onChange(clamp(value + k[e.key]));
          } else if (e.key === "Home") onChange(min);
          else if (e.key === "End") onChange(max);
        }}
      >
        <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`}>
          <circle cx={c} cy={c} r={r - 5} fill="var(--color-ink-700)" stroke="var(--color-ink-600)" />
          <path d={arc(c, c, r, START, START + SWEEP)} stroke="var(--color-ink-600)" strokeWidth={3} fill="none" strokeLinecap="round" />
          {frac > 0.001 && <path d={arc(c, c, r, START, angle)} stroke={color} strokeWidth={3} fill="none" strokeLinecap="round" />}
          {(() => {
            const [x1, y1] = polar(c, c, r - 14 < 4 ? 4 : r - 14, angle);
            const [x2, y2] = polar(c, c, r - 6, angle);
            return <line x1={x1} y1={y1} x2={x2} y2={y2} stroke={disabled ? "var(--color-ink-300)" : "#fff"} strokeWidth={2} strokeLinecap="round" />;
          })()}
        </svg>
      </div>
      <div className="w-full truncate text-center text-[10px] leading-tight text-ink-200">{label}</div>
      <div className="w-full truncate text-center font-mono text-[10px] leading-tight text-ink-300">{display}</div>
    </div>
  );
}
