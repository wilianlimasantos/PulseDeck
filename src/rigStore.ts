import { create } from "zustand";
import { api } from "./api";
import { useStore } from "./store";
import { presetCode } from "./fx";
import type { LibItem, LoadReport, MemoryUsage, Setlist, SlotKind } from "./types";
import { t } from "./i18n";

const errText = (e: unknown) => (typeof e === "string" ? e : e instanceof Error ? e.message : JSON.stringify(e));
const toast = (kind: "info" | "error" | "ok", text: string) => useStore.getState().toast(kind, text);

/** Run a long pedal operation with the shared busy flag and progress bar; errors become toasts. */
async function withBusy<T>(stage: string, fn: () => Promise<T>): Promise<T | undefined> {
  if (useStore.getState().busy) return undefined;
  useStore.setState({ busy: true, progress: { stage, done: 0, total: 1 } });
  try {
    return await fn();
  } catch (e) {
    toast("error", errText(e));
    return undefined;
  } finally {
    useStore.setState({ busy: false });
    setTimeout(() => !useStore.getState().busy && useStore.setState({ progress: null }), 1500);
  }
}

/** Operations that load other presets / switch scenes throw away edits that are not saved yet. */
async function confirmDiscard(what: string): Promise<boolean> {
  const u = useStore.getState().snap?.unsaved;
  if (!u || (!u.preset && !u.scene)) return true;
  return api.ask(t("rig.discard", { what }), t("unsaved.title"));
}

export type SidebarTab = "files" | "library" | "slots";

interface RigState {
  memoryOpen: boolean;
  usage: MemoryUsage | null;
  sidebarTab: SidebarTab;
  setlists: Setlist[];
  /** target preset slot when loading from the library; null = only apply, do not write to flash */
  saveSlot: number | null;

  setMemoryOpen: (open: boolean) => void;
  loadUsage: () => Promise<void>;
  renameSlot: (kind: SlotKind, slot: number, name: string) => Promise<boolean>;
  deleteSlot: (kind: SlotKind, slot: number, name: string) => Promise<void>;
  sendToSlot: (kind: SlotKind, slot: number, occupiedBy: string | null) => Promise<void>;

  setSidebarTab: (t: SidebarTab) => void;
  setSaveSlot: (s: number | null) => void;
  refreshLibrary: () => Promise<void>;
  saveToLibrary: (setlist: string, name: string) => Promise<void>;
  loadFromLibrary: (item: LibItem) => Promise<void>;
  deleteFromLibrary: (item: LibItem) => Promise<void>;
  createSetlist: (name: string) => Promise<boolean>;
  deleteSetlist: (name: string) => Promise<void>;
  importPackage: (setlist: string) => Promise<void>;
}

export function describeLoad(r: LoadReport): string {
  const parts: string[] = [];
  if (r.capture_slot !== null) parts.push(t("rig.load_capture", { n: r.capture_slot + 1 }) + (r.capture_uploaded ? t("rig.load_sent") : t("rig.load_had")));
  if (r.ir_slots.length) parts.push(t("rig.load_ir", { slots: r.ir_slots.map((s) => s + 1).join(", ") }) + (r.irs_uploaded ? t("rig.load_irs_sent", { n: r.irs_uploaded }) : t("rig.load_had")));
  parts.push(r.saved_slot !== null ? t("rig.load_saved", { code: presetCode(r.saved_slot) }) : t("rig.load_applied"));
  return parts.join(" · ");
}

export const useRigStore = create<RigState>((set, get) => ({
  memoryOpen: false,
  usage: null,
  sidebarTab: "files",
  setlists: [],
  saveSlot: null,

  setMemoryOpen: (memoryOpen) => set(memoryOpen ? { memoryOpen } : { memoryOpen, usage: null }),

  async loadUsage() {
    if (!(await confirmDiscard(t("rig.usage_confirm")))) return;
    const usage = await withBusy(t("st.analyzing"), () => api.memoryUsage());
    if (usage) set({ usage });
  },

  async renameSlot(kind, slot, name) {
    const out = await withBusy(t("st.renaming"), () => api.slotRename(kind, slot, name));
    if (!out) return false;
    out.forEach((w) => toast("info", w));
    toast("ok", t("rig.renamed", { n: slot + 1 }));
    return true;
  },

  async deleteSlot(kind, slot, name) {
    const usedBy = (kind === "ir" ? get().usage?.irs : get().usage?.captures)?.[String(slot)] ?? [];
    const warn = usedBy.length ? t("rig.in_use", { list: usedBy.map(presetCode).join(", "), what: kind === "ir" ? t("kind.ir") : t("kind.capture_lc") }) : "";
    const ok = await api.ask(t("rig.delete_confirm", { name, n: slot + 1, warn }), t("rig.delete_title"));
    if (!ok) return;
    const done = await withBusy(t("st.deleting"), async () => { await api.slotDelete(kind, slot); return true; });
    if (done) {
      toast("ok", t("rig.freed", { n: slot + 1 }));
      set((s) => {
        if (!s.usage) return {};
        const key = kind === "ir" ? "irs" : "captures";
        const { [String(slot)]: _gone, ...rest } = s.usage[key];
        void _gone;
        return { usage: { ...s.usage, [key]: rest } };
      });
    }
  },

  async sendToSlot(kind, slot, occupiedBy) {
    const folder = useStore.getState().folder ?? undefined;
    const path = await api.pickSlotFile(kind, folder);
    if (!path) return;
    if (occupiedBy !== null) {
      const ok = await api.ask(t("rig.overwrite_confirm", { n: slot + 1, name: occupiedBy }), t("rig.overwrite_title"));
      if (!ok) return;
    }
    const out = await withBusy(t("st.preparing"), () => api.upload(path, slot));
    if (out) toast("ok", t("toast.written", { kind: out.kind === "ir" ? t("kind.ir") : t("kind.capture"), name: out.name, slot: out.slot + 1 }));
  },

  setSidebarTab: (sidebarTab) => {
    set({ sidebarTab });
    if (sidebarTab === "library") void get().refreshLibrary();
  },
  setSaveSlot: (saveSlot) => set({ saveSlot }),

  async refreshLibrary() {
    try {
      set({ setlists: await api.libraryList() });
    } catch (e) {
      toast("error", errText(e));
    }
  },

  async saveToLibrary(setlist, name) {
    if (!(await confirmDiscard(t("rig.lib_save_confirm")))) return;
    const item = await withBusy(t("st.reading_preset"), () => api.librarySave(setlist, name));
    if (item) {
      toast("ok", t("rig.lib_saved", { name: item.name, list: item.setlist }));
      await get().refreshLibrary();
    }
  },

  async loadFromLibrary(item) {
    const slot = get().saveSlot;
    const target = slot === null ? null : useStore.getState().snap?.presets.presets.find((p) => p.slot === slot)?.name;
    const what = slot === null
      ? t("rig.lib_apply", { name: item.name })
      : t("rig.lib_write", { name: item.name, code: presetCode(slot), over: target ? t("rig.lib_overwrites", { name: target }) : "" });
    const u = useStore.getState().snap?.unsaved;
    const dirty = u && (u.preset || u.scene) ? t("rig.lib_dirty") : "";
    if (!(await api.ask(what + dirty, t("rig.lib_load_title")))) return;
    const rep = await withBusy(t("st.preparing"), () => api.libraryLoad(item.path, slot));
    if (rep) {
      toast(rep.warnings.length ? "info" : "ok", `“${item.name}”: ${describeLoad(rep)}`);
      rep.warnings.slice(0, 3).forEach((w) => toast("error", w));
    }
  },

  async deleteFromLibrary(item) {
    if (!(await api.ask(t("rig.lib_delete_confirm", { name: item.name }), t("rig.lib_delete_title")))) return;
    try {
      await api.libraryDelete(item.path);
      await get().refreshLibrary();
    } catch (e) {
      toast("error", errText(e));
    }
  },

  async createSetlist(name) {
    try {
      await api.libraryCreateSetlist(name);
      await get().refreshLibrary();
      return true;
    } catch (e) {
      toast("error", errText(e));
      return false;
    }
  },

  async deleteSetlist(name) {
    try {
      await api.libraryDeleteSetlist(name);
      await get().refreshLibrary();
    } catch (e) {
      toast("error", errText(e));
    }
  },

  async importPackage(setlist) {
    const path = await api.pickPackageFile();
    if (!path) return;
    try {
      const it = await api.libraryImport(path, setlist);
      if (it) toast("ok", t("rig.imported", { name: it.name, list: setlist }));
      await get().refreshLibrary();
    } catch (e) {
      toast("error", errText(e));
    }
  },
}));
