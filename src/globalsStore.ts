import { create } from "zustand";
import { api } from "./api";
import { useStore } from "./store";
import type { FsLed, GlobalSettings } from "./types";
import { t, type Key } from "./i18n";

const STASH_KEY = "pulsedeck.geqStash";
const BANDS = 5;

const errText = (e: unknown) => (typeof e === "string" ? e : e instanceof Error ? e.message : JSON.stringify(e));
const fail = (e: unknown) => useStore.getState().toast("error", errText(e));

function loadStash(): number[] | null {
  try {
    const v = JSON.parse(localStorage.getItem(STASH_KEY) ?? "null") as unknown;
    return Array.isArray(v) && v.length === BANDS && v.every((x) => typeof x === "number") ? (v as number[]) : null;
  } catch {
    return null;
  }
}
function saveStash(v: number[] | null) {
  try {
    if (v) localStorage.setItem(STASH_KEY, JSON.stringify(v));
    else localStorage.removeItem(STASH_KEY);
  } catch {
    /* no storage: bypass memory lasts only for this window */
  }
}

/** Colour the ring shows when the footswitch is inactive and "dim" is selected (1/24 of active). */
export function dimColor(hex: string, divisor: number): string {
  const n = parseInt(hex, 16);
  const ch = [(n >> 16) & 255, (n >> 8) & 255, n & 255].map((v) => Math.floor(v / divisor));
  return ch.map((v) => v.toString(16).padStart(2, "0")).join("");
}

interface GlobalsStore {
  open: boolean;
  loading: boolean;
  data: GlobalSettings | null;
  geq: number[];
  /** values remembered while the EQ is bypassed (the pedal has no EQ bypass command) */
  stash: number[] | null;
  show: () => Promise<void>;
  close: () => void;
  reload: () => Promise<void>;
  setBand: (band: number, db: number) => void;
  resetEq: () => Promise<void>;
  toggleEq: () => Promise<void>;
  setHpf: (hz: number) => Promise<void>;
  applyQuick: (key: string) => Promise<void>;
  setLed: (key: string, value: string | number) => Promise<void>;
  setFsLed: (fs: number, patch: Partial<FsLed>) => Promise<void>;
  save: () => Promise<void>;
}

const timers = new Map<string, ReturnType<typeof setTimeout>>();
const later = (key: string, fn: () => void, ms = 90) => {
  clearTimeout(timers.get(key));
  timers.set(key, setTimeout(fn, ms));
};

export const useGlobals = create<GlobalsStore>((set, get) => {
  const patch = (fn: (d: GlobalSettings) => Partial<GlobalSettings>) => {
    const d = get().data;
    if (d) set({ data: { ...d, ...fn(d), dirty: true } });
  };
  return {
    open: false,
    loading: false,
    data: null,
    geq: [0, 0, 0, 0, 0],
    stash: null,

    show: async () => {
      set({ open: true });
      await get().reload();
    },
    close: () => {
      timers.forEach((t) => clearTimeout(t));
      timers.clear();
      set({ open: false });
    },
    reload: async () => {
      set({ loading: true });
      try {
        const data = await api.globalsGet();
        const geq = data.geq ?? [0, 0, 0, 0, 0];
        // all bands flat + a remembered shape = the user bypassed the EQ earlier
        const stash = geq.every((v) => v === 0) ? loadStash() : null;
        if (!stash) saveStash(null);
        set({ data, geq, stash, loading: false });
      } catch (e) {
        set({ loading: false, data: null });
        fail(e);
      }
    },

    setBand: (band, db) => {
      if (get().stash) return;
      const geq = get().geq.slice();
      geq[band] = db;
      set({ geq });
      patch(() => ({ geq }));
      later(`geq${band}`, () => {
        api.setGlobalEq(band, db).catch((e) => {
          fail(e);
          void get().reload();
        });
      });
    },
    resetEq: async () => {
      timers.forEach((t, k) => k.startsWith("geq") && clearTimeout(t));
      saveStash(null);
      const geq = [0, 0, 0, 0, 0];
      set({ geq, stash: null });
      patch(() => ({ geq }));
      try {
        await api.setGlobalEqAll(geq);
      } catch (e) {
        fail(e);
        await get().reload();
      }
    },
    toggleEq: async () => {
      const { stash, geq } = get();
      const next = stash ?? [0, 0, 0, 0, 0];
      const target = stash ? stash : [0, 0, 0, 0, 0];
      timers.forEach((t, k) => k.startsWith("geq") && clearTimeout(t));
      if (stash) {
        saveStash(null);
        set({ stash: null, geq: next });
      } else {
        saveStash(geq);
        set({ stash: geq, geq: target });
      }
      patch(() => ({ geq: target }));
      try {
        await api.setGlobalEqAll(target);
      } catch (e) {
        fail(e);
        await get().reload();
      }
    },
    setHpf: async (hz) => {
      patch(() => ({ hpf: hz }));
      try {
        await api.setInputHpf(hz);
      } catch (e) {
        fail(e);
        await get().reload();
      }
    },

    applyQuick: async (key) => {
      const d = get().data;
      if (!d || d.quick_active === key) return;
      const q = d.schema.footswitch_quick_setups.find((x) => x.key === key);
      if (!q) return;
      const name = t(`fsq.${q.key}.label` as Key);
      if (key !== "factory" && !(await api.ask(t("g.fs_confirm_body", { desc: t(`fsq.${q.key}.desc` as Key) }), t("g.fs_confirm_title", { name })))) return;
      try {
        const data = await api.setFootswitchMode(key);
        set({ data });
        useStore.getState().toast("ok", t("g.fs_applied", { name }));
      } catch (e) {
        fail(e);
        await get().reload();
      }
    },

    setLed: async (key, value) => {
      const d = get().data;
      if (!d?.led) return;
      patch((x) => ({ led: { ...x.led!, [key]: value } }));
      const send = async () => {
        try {
          await api.setLedConfig(key, String(value));
        } catch (e) {
          fail(e);
          await get().reload();
        }
      };
      if (key === "bright") later("bright", () => void send(), 120);
      else await send();
    },
    setFsLed: async (fs, p) => {
      const cur = get().data?.fsled?.[fs];
      if (!cur) return;
      const led = { ...cur, ...p };
      patch((x) => ({ fsled: x.fsled!.map((l, i) => (i === fs ? led : l)) }));
      later(`fsled${fs}`, () => {
        api.setFootswitchLed(fs, led).catch((e) => {
          fail(e);
          void get().reload();
        });
      }, 120);
    },

    save: async () => {
      try {
        await api.saveGlobals();
        const d = get().data;
        if (d) set({ data: { ...d, dirty: false } });
        useStore.getState().toast("ok", t("g.saved_toast"));
      } catch (e) {
        fail(e);
      }
    },
  };
});
