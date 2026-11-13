import { create } from "zustand";
import { en } from "./en";
import { pt } from "./pt-BR";

export type Lang = "pt-BR" | "en";
export type Key = Extract<keyof typeof pt, string>;
export type Params = Record<string, string | number>;

export const LANGS: { code: Lang; short: string }[] = [
  { code: "pt-BR", short: "PT-BR" },
  { code: "en", short: "EN" },
];

const DICTS: Record<Lang, Record<Key, string>> = { "pt-BR": pt, en };
const STORAGE_KEY = "pulsedeck.lang";

/** Saved choice, else the OS language: Portuguese -> pt-BR, anything else -> English. */
export function detectLang(saved: string | null, navLang: string | undefined): Lang {
  if (saved === "pt-BR" || saved === "en") return saved;
  return (navLang ?? "").toLowerCase().startsWith("pt") ? "pt-BR" : "en";
}

function initial(): Lang {
  let saved: string | null = null;
  try {
    saved = localStorage.getItem(STORAGE_KEY);
  } catch {
    /* storage blocked: fall back to the OS language */
  }
  return detectLang(saved, typeof navigator !== "undefined" ? navigator.language : undefined);
}

export const hasKey = (k: string): k is Key => k in pt;

export function translate(lang: Lang, key: Key, params?: Params): string {
  const text = DICTS[lang][key] ?? DICTS["pt-BR"][key] ?? key;
  return params ? text.replace(/\{(\w+)\}/g, (m, k: string) => (k in params ? String(params[k]) : m)) : text;
}

interface LangStore {
  lang: Lang;
  setLang: (l: Lang) => void;
}

const apply = (l: Lang) => {
  if (typeof document !== "undefined") document.documentElement.lang = l;
};

export const useLang = create<LangStore>((set) => {
  const lang = initial();
  apply(lang);
  return {
    lang,
    setLang: (l) => {
      apply(l);
      try {
        localStorage.setItem(STORAGE_KEY, l);
      } catch {
        /* not persisted: still applies for this session */
      }
      set({ lang: l });
    },
  };
});

/** Translate outside React (stores, alerts): uses the language active right now. */
export function t(key: Key, params?: Params): string {
  return translate(useLang.getState().lang, key, params);
}

/** Hook: returns `t` bound to the current language and re-renders the component when it changes. */
export function useT(): (key: Key, params?: Params) => string {
  const lang = useLang((s) => s.lang);
  return (key, params) => translate(lang, key, params);
}
