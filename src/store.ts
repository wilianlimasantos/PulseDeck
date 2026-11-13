import { create } from "zustand";
import { api, subscribe } from "./api";
import { cpuPercent, IR_TYPE, nextCpuHigh, uploadAlert, type AlertAction, type UxAlert } from "./alerts";
import type {
  AmpEq, Block, FileEntry, GearClass, Listing, Level, Progress, ProtocolInfo, Snapshot,
} from "./types";
import { t, useLang } from "./i18n";
import { localizeBackend } from "./i18n/backend";

export interface Toast { id: number; kind: "info" | "error" | "ok"; text: string }

export interface LoadedNam { name: string; cls: GearClass | null }

interface State {
  ready: boolean;
  snap: Snapshot | null;
  info: ProtocolInfo | null;
  cpuHigh: boolean;
  progress: Progress | null;
  busy: boolean;
  toasts: Toast[];
  /** last `.nam` sent: drives the smart IR alerts (re-evaluated against the live chain) */
  loaded: LoadedNam | null;
  dismissedAlert: string | null;
  listing: Listing | null;
  folder: string | null;
  captureSlot: number | null; // null = auto
  irSlot: number | null;

  init: () => Promise<void>;
  toast: (kind: Toast["kind"], text: string) => void;
  dismissToast: (id: number) => void;
  setPreset: (slot: number) => Promise<void>;
  setScene: (scene: number) => Promise<void>;
  setParam: (b: Block, index: number, value: number) => void;
  setBlockOn: (id: number, on: boolean) => Promise<void>;
  setAmpOn: (on: boolean) => Promise<void>;
  setAmpEq: (eq: AmpEq) => void;
  addBlock: (bank: "P" | "Q", typeName: string, position?: number) => Promise<void>;
  removeBlock: (id: number) => Promise<void>;
  moveBlock: (id: number, bank: "P" | "Q", position: number) => Promise<void>;
  loadCapture: (slot: number) => Promise<void>;
  openFolder: (path: string) => Promise<void>;
  sendFile: (f: FileEntry) => Promise<void>;
  runAlertAction: (kind: AlertAction) => Promise<void>;
  dismissAlert: () => void;
  setSlots: (capture: number | null, ir: number | null) => void;
  saveDialog: boolean;
  setSaveDialog: (open: boolean) => void;
  savePreset: () => Promise<void>;
  savePresetAs: (slot: number, name: string) => Promise<void>;
  saveScene: () => Promise<void>;
  setAmpPresent: (present: boolean) => Promise<void>;
  exportBackup: () => Promise<void>;
  restoreBackup: () => Promise<void>;
}

let toastSeq = 1;

// ---- throttled live parameter sends: at most one FXP in flight per knob, last value wins ----
const pending = new Map<string, number>();
const inflight = new Set<string>();
/** values the user is still sending: snapshots must not move their knobs back */
const optimistic = new Map<string, number>();

function pumpParam(key: string, id: number, index: number, fail: (e: unknown) => void, done: () => void) {
  if (inflight.has(key) || !pending.has(key)) return;
  const value = pending.get(key)!;
  pending.delete(key);
  inflight.add(key);
  api
    .setParam(id, index, value)
    .catch(fail)
    .finally(() => {
      inflight.delete(key);
      if (pending.has(key)) pumpParam(key, id, index, fail, done);
      else {
        optimistic.delete(key);
        done();
      }
    });
}

function mergeSnapshot(s: Snapshot): Snapshot {
  if (optimistic.size === 0) return s;
  const blocks = s.chain.blocks.map((b) => {
    let params = b.params;
    optimistic.forEach((v, key) => {
      const [id, idx] = key.split(":").map(Number);
      if (id === b.id) {
        params = params.slice();
        params[idx] = v;
      }
    });
    return params === b.params ? b : { ...b, params };
  });
  return { ...s, chain: { ...s.chain, blocks } };
}

const errText = (e: unknown) => (typeof e === "string" ? e : e instanceof Error ? e.message : JSON.stringify(e));

export const useStore = create<State>((set, get) => {
  const fail = (e: unknown) => get().toast("error", errText(e));
  const applySnapshot = (raw: Snapshot) => {
    const snap = mergeSnapshot(raw);
    const high = nextCpuHigh(get().cpuHigh, cpuPercent(snap.level.cpu));
    set({ snap, cpuHigh: high });
  };
  const applyMeter = (l: Level) => {
    const s = get().snap;
    if (!s) return;
    set({
      snap: { ...s, level: l, chain: { ...s.chain, cpu: l.cpu.budget > 0 ? l.cpu : s.chain.cpu } },
      cpuHigh: nextCpuHigh(get().cpuHigh, cpuPercent(l.cpu)),
    });
  };
  const guard = async (fn: () => Promise<unknown>) => {
    try {
      await fn();
    } catch (e) {
      fail(e);
    }
  };

  return {
    ready: false, snap: null, info: null, cpuHigh: false, progress: null, busy: false, toasts: [],
    saveDialog: false, loaded: null, dismissedAlert: null, listing: null, folder: null, captureSlot: null, irSlot: null,

    async init() {
      const [snap, info] = await Promise.all([api.snapshot(), api.protocolInfo()]);
      set({ snap, info, ready: true });
      await subscribe({
        snapshot: applySnapshot,
        status: (status) => {
          const s = get().snap;
          if (s) set({ snap: { ...s, status } });
        },
        meter: applyMeter,
        progress: (progress) => set({ progress: { ...progress, stage: localizeBackend(progress.stage, useLang.getState().lang) } }),
      });
      const folder = localStorage.getItem("pulsedeck.folder") || (await api.defaultFolder());
      await get().openFolder(folder).catch(async () => get().openFolder(await api.defaultFolder()));
    },

    toast(kind, text) {
      const id = toastSeq++;
      set((s) => ({ toasts: [...s.toasts, { id, kind, text: localizeBackend(text, useLang.getState().lang) }] }));
      setTimeout(() => get().dismissToast(id), kind === "error" ? 7000 : 3500);
    },
    dismissToast: (id) => set((s) => ({ toasts: s.toasts.filter((t) => t.id !== id) })),

    async setPreset(slot) {
      const s = get().snap;
      if (s) set({ snap: { ...s, presets: { ...s.presets, current: slot } } });
      await guard(() => api.setPreset(slot));
    },
    async setScene(scene) {
      const s = get().snap;
      if (s) set({ snap: { ...s, scenes: { ...s.scenes, current: scene } } });
      await guard(() => api.setScene(scene));
    },

    setParam(b, index, value) {
      const key = `${b.id}:${index}`;
      optimistic.set(key, value);
      pending.set(key, value);
      const s = get().snap;
      if (s) {
        const blocks = s.chain.blocks.map((x) =>
          x.id === b.id ? { ...x, params: x.params.map((p, i) => (i === index ? value : p)) } : x,
        );
        set({ snap: { ...s, chain: { ...s.chain, blocks } } });
      }
      pumpParam(key, b.id, index, fail, () => {});
    },

    async setBlockOn(id, on) {
      const s = get().snap;
      if (s) set({ snap: { ...s, chain: { ...s.chain, blocks: s.chain.blocks.map((b) => (b.id === id ? { ...b, on } : b)) } } });
      await guard(() => api.setBlockOn(id, on));
    },
    async setAmpOn(on) {
      const s = get().snap;
      if (s) set({ snap: { ...s, chain: { ...s.chain, amp_on: on } } });
      await guard(() => api.setAmpOn(on));
    },
    setAmpEq(eq) {
      const s = get().snap;
      if (s) set({ snap: { ...s, ampeq: eq } });
      scheduleAmpEq(eq, fail);
    },

    async addBlock(bank, typeName, position = 99) {
      await guard(() => api.addBlock(bank, position, typeName));
    },
    async removeBlock(id) {
      await guard(() => api.removeBlock(id));
    },
    async moveBlock(id, bank, position) {
      await guard(() => api.moveBlock(id, bank, position));
    },
    async loadCapture(slot) {
      await guard(() => api.loadCapture(slot));
    },

    async openFolder(path) {
      const listing = await api.listFolder(path);
      localStorage.setItem("pulsedeck.folder", listing.path);
      set({ listing, folder: listing.path });
    },

    async sendFile(f) {
      if (get().busy) return;
      set({ busy: true, progress: { stage: t("st.preparing"), done: 0, total: 1 }, dismissedAlert: null });
      try {
        const slot = f.kind === "wav" ? get().irSlot : get().captureSlot;
        const out = await api.upload(f.path, slot);
        get().toast("ok", t("toast.sent", { kind: out.kind === "ir" ? t("kind.ir") : t("kind.capture"), name: out.name, slot: out.slot + 1 }));
        if (out.kind === "capture") set({ loaded: { name: out.name, cls: out.class } });
      } catch (e) {
        fail(e);
      } finally {
        set({ busy: false });
        setTimeout(() => get().busy || set({ progress: null }), 1500);
      }
    },

    async runAlertAction(kind) {
      const chain = get().snap?.chain;
      if (!chain) return;
      if (kind === "disable_ir") {
        for (const b of chain.blocks.filter((b) => b.type_name === IR_TYPE && b.on)) await get().setBlockOn(b.id, false);
      } else if (kind === "enable_ir") {
        const b = chain.blocks.find((b) => b.type_name === IR_TYPE);
        if (b) await get().setBlockOn(b.id, true);
      } else {
        await get().addBlock("Q", IR_TYPE, 0);
      }
    },
    dismissAlert: () => set({ dismissedAlert: currentAlert(get())?.id ?? null }),
    setSlots: (captureSlot, irSlot) => set({ captureSlot, irSlot }),

    setSaveDialog: (saveDialog) => set({ saveDialog }),

    async setAmpPresent(present) {
      await guard(() => api.setAmpPresent(present));
    },

    async savePresetAs(slot, name) {
      try {
        await api.savePreset(slot, name);
        set({ saveDialog: false });
        get().toast("ok", t("toast.preset_saved", { n: slot + 1 }));
      } catch (e) {
        fail(e);
      }
    },
    async savePreset() {
      const snap = get().snap;
      const cur = snap?.presets.current;
      if (!snap || cur === null || cur === undefined || cur < 0) return set({ saveDialog: true });
      const name = snap.presets.presets.find((p) => p.slot === cur)?.name ?? t("preset.default_name", { n: cur + 1 });
      await get().savePresetAs(cur, name);
    },
    async saveScene() {
      const scene = get().snap?.scenes.current;
      if (scene === null || scene === undefined || scene < 0) return get().toast("error", t("toast.pick_scene"));
      try {
        await api.saveScene(scene);
        get().toast("ok", t("toast.scene_saved", { n: scene + 1 }));
      } catch (e) {
        fail(e);
      }
    },

    async exportBackup() {
      if (get().busy) return;
      const s = get().snap;
      if (s && (s.unsaved.preset || s.unsaved.scene)) {
        const ok = await api.ask(t("bk.export_confirm"), t("unsaved.title"));
        if (!ok) return;
      }
      const path = await api.pickBackupSavePath();
      if (!path) return;
      set({ busy: true, progress: { stage: t("st.reading"), done: 0, total: 1 } });
      try {
        const r = await api.exportBackup(path);
        get().toast("ok", t("bk.saved", { presets: r.presets, captures: r.captures, irs: r.irs, globals: r.globals }));
        r.warnings.forEach((w) => get().toast("info", w));
      } catch (e) {
        fail(e);
      } finally {
        set({ busy: false });
        setTimeout(() => get().busy || set({ progress: null }), 1500);
      }
    },

    async restoreBackup() {
      if (get().busy) return;
      const path = await api.pickBackupOpenPath();
      if (!path) return;
      let preview;
      try {
        preview = await api.inspectBackup(path);
      } catch (e) {
        return fail(e);
      }
      const when = preview.created_unix ? new Date(preview.created_unix * 1000).toLocaleString(useLang.getState().lang) : t("bk.unknown_date");
      const ok = await api.ask(
        t("bk.restore_confirm", { when, fw: preview.firmware ? ` (${preview.firmware})` : "", presets: preview.presets, captures: preview.captures, irs: preview.irs, globals: preview.globals }),
        t("bk.restore_title"),
      );
      if (!ok) return;
      set({ busy: true, progress: { stage: t("st.restoring"), done: 0, total: 1 } });
      try {
        const r = await api.restoreBackup(path);
        get().toast(r.warnings.length ? "info" : "ok", t("bk.restored", { presets: r.presets, captures: r.captures, irs: r.irs }) + (r.warnings.length ? t("bk.restored_warn", { n: r.warnings.length }) : ""));
        r.warnings.slice(0, 4).forEach((w) => get().toast("error", w));
      } catch (e) {
        fail(e);
      } finally {
        set({ busy: false });
        setTimeout(() => get().busy || set({ progress: null }), 1500);
      }
    },
  };
});

// ---- amp EQ: same throttling idea, whole struct per command ----
let eqPending: AmpEq | null = null;
let eqBusy = false;
function scheduleAmpEq(eq: AmpEq, fail: (e: unknown) => void) {
  eqPending = eq;
  if (eqBusy) return;
  eqBusy = true;
  const run = async () => {
    while (eqPending) {
      const v = eqPending;
      eqPending = null;
      try {
        await api.setAmpEq(v);
      } catch (e) {
        fail(e);
      }
    }
    eqBusy = false;
  };
  void run();
}

export function currentAlert(s: Pick<State, "loaded" | "snap" | "dismissedAlert">): UxAlert | null {
  if (!s.loaded || !s.snap) return null;
  const a = uploadAlert(s.loaded.cls, s.snap.chain, s.loaded.name);
  return a && a.id !== s.dismissedAlert ? a : null;
}
