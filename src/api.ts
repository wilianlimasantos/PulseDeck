import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import { confirm, open, save } from "@tauri-apps/plugin-dialog";
import type {
  AmpEq, BackupPreview, BackupSummary, ConnectionStatus, LibItem, Level, Listing, LoadReport, MemoryUsage, Progress,
  ProtocolInfo, Setlist, SlotKind, Snapshot, TuneReading, TunerState, UploadOutcome, BleDevice, TransportMode, TransportState, FsLed, GlobalSettings,
} from "./types";
import { mock } from "./mock";
import { t } from "./i18n";

export const inTauri = typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;

async function call<T>(cmd: string, args?: Record<string, unknown>): Promise<T> {
  if (!inTauri) return mock.call<T>(cmd, args);
  return invoke<T>(cmd, args);
}

export const api = {
  snapshot: () => call<Snapshot>("get_snapshot"),
  protocolInfo: () => call<ProtocolInfo>("protocol_info"),
  setPreset: (slot: number) => call<void>("set_preset", { slot }),
  setScene: (scene: number) => call<void>("set_scene", { scene }),
  setParam: (id: number, index: number, value: number) => call<number>("block_set_param", { id, index, value }),
  setBlockOn: (id: number, on: boolean) => call<void>("block_set_on", { id, on }),
  setAmpOn: (on: boolean) => call<void>("amp_set_on", { on }),
  setAmpEq: (eq: AmpEq) => call<AmpEq>("amp_set_eq", { eq }),
  addBlock: (bank: "P" | "Q", position: number, typeName: string) =>
    call<number>("block_add", { bank, position, typeName }),
  removeBlock: (id: number) => call<void>("block_remove", { id }),
  moveBlock: (id: number, bank: "P" | "Q", position: number) => call<void>("block_move", { id, bank, position }),
  loadCapture: (slot: number) => call<void>("capture_load", { slot }),
  setAmpPresent: (present: boolean) => call<void>("amp_set_present", { present }),
  savePreset: (slot: number, name: string) => call<void>("save_preset", { slot, name }),
  saveScene: (scene: number) => call<void>("save_scene", { scene }),
  exportBackup: (path: string) => call<BackupSummary>("export_backup", { path }),
  restoreBackup: (path: string) => call<BackupSummary>("restore_backup", { path }),
  pickBackupSavePath: async (): Promise<string | null> => {
    if (!inTauri) return null;
    const stamp = new Date().toISOString().slice(0, 10);
    return save({ defaultPath: `pulsedeck-backup-${stamp}.zip`, filters: [{ name: t("bk.filter_name"), extensions: ["zip"] }] });
  },
  pickBackupOpenPath: async (): Promise<string | null> => {
    if (!inTauri) return null;
    const r = await open({ multiple: false, filters: [{ name: t("bk.filter_name"), extensions: ["zip"] }] });
    return typeof r === "string" ? r : null;
  },
  inspectBackup: (path: string) => call<BackupPreview>("inspect_backup", { path }),
  memoryUsage: () => call<MemoryUsage>("memory_usage"),
  slotRename: (kind: SlotKind, slot: number, name: string) => call<string[]>("slot_rename", { kind, slot, name }),
  slotDelete: (kind: SlotKind, slot: number) => call<void>("slot_delete", { kind, slot }),
  libraryList: () => call<Setlist[]>("library_list"),
  libraryCreateSetlist: (name: string) => call<void>("library_create_setlist", { name }),
  libraryDeleteSetlist: (name: string) => call<void>("library_delete_setlist", { name }),
  librarySave: (setlist: string, name: string) => call<LibItem>("library_save", { setlist, name }),
  libraryLoad: (path: string, saveSlot: number | null) => call<LoadReport>("library_load", { path, saveSlot }),
  libraryDelete: (path: string) => call<void>("library_delete", { path }),
  libraryImport: (path: string, setlist: string) => call<LibItem>("library_import", { path, setlist }),
  pickSlotFile: async (kind: SlotKind, defaultPath?: string): Promise<string | null> => {
    if (!inTauri) return null;
    const filters = kind === "ir"
      ? [{ name: t("pick.ir"), extensions: ["wav"] }]
      : [{ name: t("pick.capture"), extensions: ["nam", "a2l"] }];
    const r = await open({ multiple: false, filters, defaultPath });
    return typeof r === "string" ? r : null;
  },
  pickPackageFile: async (): Promise<string | null> => {
    if (!inTauri) return null;
    const r = await open({ multiple: false, filters: [{ name: t("pk.filter_name"), extensions: ["pulsedeck"] }] });
    return typeof r === "string" ? r : null;
  },
  tunerSet: (on: boolean, mute: boolean, refHz: number) => call<TunerState>("tuner_set", { on, mute, refHz }),
  globalsGet: () => call<GlobalSettings>("get_global_settings"),
  setGlobalEq: (band: number, value: number) => call<number>("set_global_eq", { band, value }),
  setGlobalEqAll: (values: number[]) => call<number[]>("set_global_eq_all", { values }),
  setInputHpf: (hz: number) => call<void>("set_input_hpf", { hz }),
  setFootswitchMode: (setup: string) => call<GlobalSettings>("set_footswitch_mode", { setup }),
  setLedConfig: (key: string, value: string) => call<void>("set_led_config", { key, value }),
  setFootswitchLed: (fs: number, led: FsLed) => call<void>("set_footswitch_led", { fs, led }),
  saveGlobals: () => call<void>("save_global_settings"),
  transportGet: () => call<TransportState>("transport_get"),
  transportSet: (mode: TransportMode) => call<TransportState>("transport_set", { mode }),
  bleScan: (all: boolean) => call<BleDevice[]>("ble_scan", { all }),
  bleConnect: (id: string, name: string | null) => call<TransportState>("ble_connect", { id, name }),
  bleDisconnect: () => call<TransportState>("ble_disconnect"),
  tunerRead: () => call<TuneReading>("tuner_read"),
  ask: async (message: string, title: string): Promise<boolean> =>
    inTauri ? confirm(message, { title, kind: "warning" }) : window.confirm(message),
  defaultFolder: () => call<string>("default_folder"),
  listFolder: (path: string) => call<Listing>("list_folder", { path }),
  upload: (path: string, slot: number | null) => call<UploadOutcome>("upload_file", { path, slot }),
  pickFolder: async (): Promise<string | null> => {
    if (!inTauri) return null;
    const r = await open({ directory: true, multiple: false });
    return typeof r === "string" ? r : null;
  },
};

export interface Listeners {
  snapshot: (s: Snapshot) => void;
  status: (s: ConnectionStatus) => void;
  meter: (l: Level) => void;
  progress: (p: Progress) => void;
}

export async function subscribe(l: Listeners): Promise<() => void> {
  if (!inTauri) return mock.subscribe(l);
  const offs: UnlistenFn[] = await Promise.all([
    listen<Snapshot>("pedal:snapshot", (e) => l.snapshot(e.payload)),
    listen<ConnectionStatus>("pedal:status", (e) => l.status(e.payload)),
    listen<Level>("pedal:meter", (e) => l.meter(e.payload)),
    listen<Progress>("pedal:progress", (e) => l.progress(e.payload)),
  ]);
  return () => offs.forEach((f) => f());
}

/** Transport (USB / Bluetooth) changes pushed by the backend. */
export async function subscribeTransport(fn: (t: TransportState) => void): Promise<() => void> {
  if (!inTauri) return mock.subscribeTransport(fn);
  const off = await listen<TransportState>("pedal:transport", (e) => fn(e.payload));
  return off;
}
