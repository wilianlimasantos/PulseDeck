import { hasKey, t, type Key } from "./i18n";
import type { ParamDef } from "./types";

const SUFFIXES: Array<[string, string]> = [
  ["_db", " dB"],
  ["_hz", " Hz"],
  ["_ms", " ms"],
  ["_pct", "%"],
  ["_chz", " Hz"],
  ["_x10", ":1"],
  ["_st", " st"],
  ["_ct", " ct"],
  ["_n", ""],
];

/** Human label for a parameter name such as `umbral_db`. */
export function paramLabel(name: string): string {
  let base = name;
  for (const [s] of SUFFIXES) if (base.endsWith(s)) { base = base.slice(0, -s.length); break; }
  const key = `p.${base}`;
  if (hasKey(key)) return t(key as Key);
  const text = base.replace(/_/g, " ").trim();
  return text.charAt(0).toUpperCase() + text.slice(1);
}

/** Display text for a raw parameter value using the unit suffix of its name (protocol: parameter_name_suffix_units). */
export function formatParam(p: Pick<ParamDef, "name">, v: number): string {
  if (p.name.endsWith("_chz")) return `${(v / 100).toFixed(2)} Hz`;
  if (p.name.endsWith("_x10")) return `${(v / 10).toFixed(1)}:1`;
  if (p.name.endsWith("_hz") && Math.abs(v) >= 1000) return `${(v / 1000).toFixed(v % 1000 === 0 ? 0 : 2)} kHz`;
  for (const [s, u] of SUFFIXES) if (p.name.endsWith(s)) return `${v}${u}`;
  return String(v);
}

export function categoryHue(cat: string): number {
  const hues: Record<string, number> = {
    dynamics: 38, drive: 18, dist: 350, amp: 28, cab: 200, eq: 150, mod: 280, delay: 215, reverb: 250,
  };
  return hues[cat] ?? 170;
}

/** Translated category name (falls back to the raw id for categories the firmware adds later). */
export function categoryLabel(cat: string): string {
  const key = `cat.${cat}`;
  return hasKey(key) ? t(key as Key) : cat;
}

/** "1A".."8D" display convention: bank = floor(slot/4)+1, letter = A..D. */
export function presetCode(slot: number): string {
  return `${Math.floor(slot / 4) + 1}${"ABCD"[slot % 4]}`;
}

/** Amp models (amp_clean, amp_brit, amp_hi...) are not offered in the P/Q banks. */
export const isAmpCategory = (cat: string) => cat === "amp";

/** True when keyboard shortcuts must not fire: the user is typing or operating a form control. */
export function isTypingTarget(t: EventTarget | null): boolean {
  const el = t as HTMLElement | null;
  if (!el || !el.tagName) return false;
  const tag = el.tagName;
  return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT" || el.isContentEditable;
}

export type ShortcutAction = { type: "scene"; scene: number } | { type: "preset"; delta: -1 | 1 } | { type: "tuner" };

/** Map a key event to a shortcut: 1-4 = scenes, Left/Right = previous/next preset, T = tuner. */
export function shortcutFor(e: Pick<KeyboardEvent, "key" | "ctrlKey" | "metaKey" | "altKey" | "shiftKey" | "repeat" | "target">): ShortcutAction | null {
  if (e.ctrlKey || e.metaKey || e.altKey || e.shiftKey || e.repeat || isTypingTarget(e.target)) return null;
  if (e.key >= "1" && e.key <= "4") return { type: "scene", scene: Number(e.key) - 1 };
  if (e.key === "t" || e.key === "T") return { type: "tuner" };
  if (e.key === "ArrowLeft") return { type: "preset", delta: -1 };
  if (e.key === "ArrowRight") return { type: "preset", delta: 1 };
  return null;
}
