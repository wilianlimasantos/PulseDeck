// Mirrors the JSON produced by the Rust backend (pulsedeck-core / src-tauri).

export type ConnectionStatus =
  | { state: "searching" }
  | { state: "not_responding"; port: string; reason: string }
  | {
      state: "connected";
      port: string;
      model: string;
      version: string;
      firmware_number: number | null;
      flags: string[];
      pong: string;
    };

export interface ParamDef {
  index: number;
  name: string;
  min: number;
  max: number;
  default: number;
}

export interface EffectType {
  id: number;
  name: string;
  category: string;
  cost?: number | null;
  parameters: ParamDef[];
}

export interface Block {
  id: number;
  bank: "P" | "Q";
  position: number;
  type_name: string;
  on: boolean;
  params: number[];
  ticks: number;
  lane: number;
  group: number | null;
  irlen: string | null;
}

export interface Cpu {
  used: number;
  budget: number;
}

export interface Chain {
  blocks: Block[];
  amp_present: boolean;
  amp_on: boolean;
  cpu: Cpu;
}

export interface PresetInfo {
  slot: number;
  block_count: number;
  name: string;
  color: string | null;
  scene_mask: number | null;
}

export interface PresetList {
  presets: PresetInfo[];
  current: number | null;
}

export interface Scenes {
  stored: [boolean, boolean, boolean, boolean];
  current: number | null;
}

export interface AmpEq {
  volume: number;
  bass: number;
  mid: number;
  treble: number;
  presence: number;
  drive: number;
}

export interface Level {
  in_db10: number | null;
  out_db10: number | null;
  cpu: Cpu;
  chain_generation: number | null;
  preset: number | null;
  scene: number | null;
  amp_on: boolean | null;
}

export interface LibEntry {
  slot: number;
  bytes: number;
  name: string;
  /** capture trim in dB (CLIST `trim=`) */
  trim_db: number | null;
  /** capture input level in dBu (CLIST `in=`) */
  input_dbu: number | null;
}

export type SlotKind = "capture" | "ir";

/** Presets (slot numbers, 0-based) using each capture / IR slot. Keys are slot numbers. */
export interface MemoryUsage {
  captures: Record<string, number[]>;
  irs: Record<string, number[]>;
}

export interface BackupPreview {
  firmware: string;
  created_unix: number;
  presets: number;
  captures: number;
  irs: number;
  globals: number;
  preset_names: string[];
}

export interface LibItem {
  path: string;
  name: string;
  setlist: string;
  created_unix: number;
  firmware: string;
  has_capture: boolean;
  ir_count: number;
  scenes: number;
}

export interface Setlist {
  name: string;
  items: LibItem[];
}

export interface LoadReport {
  capture_slot: number | null;
  capture_uploaded: boolean;
  ir_slots: number[];
  irs_uploaded: number;
  saved_slot: number | null;
  warnings: string[];
}

export interface Library {
  entries: LibEntry[];
  current: number | null;
  slots: number | null;
}

export interface Unsaved {
  preset: boolean;
  scene: boolean;
}

export interface BackupSummary {
  presets: number;
  captures: number;
  irs: number;
  globals: number;
  warnings: string[];
}

export interface Snapshot {
  unsaved: Unsaved;
  status: ConnectionStatus;
  effects: EffectType[];
  chain: Chain;
  presets: PresetList;
  scenes: Scenes;
  ampeq: AmpEq;
  level: Level;
  captures: Library;
  irs: Library;
  model: string | null;
  demo: boolean;
}

export interface ProtocolInfo {
  amp_eq_ranges: Record<string, [number, number]>;
  preset_count: number;
  scene_count: number;
  capture_slots: number;
  ir_slots: number;
  vendor_id: number;
}

export type GearClass = "FULL_RIG" | "AMP_ONLY" | "OTHER";

export interface NamInfo {
  name: string | null;
  gear_type: string | null;
  class: GearClass;
  input_level_dbu: number | null;
  architecture: string | null;
  sample_rate: number | null;
  gear_make: string | null;
  gear_model: string | null;
  tone_type: string | null;
  modeled_by: string | null;
}

export interface FileEntry {
  path: string;
  name: string;
  kind: "dir" | "nam" | "wav" | "a2l";
  size: number;
  info: NamInfo | null;
  error: string | null;
}

export interface Listing {
  path: string;
  parent: string | null;
  entries: FileEntry[];
}

export interface UploadOutcome {
  kind: "capture" | "ir";
  slot: number;
  name: string;
  bytes: number;
  detail: string;
  class: GearClass | null;
  input_level_dbu: number | null;
}

export interface Progress {
  stage: string;
  done: number;
  total: number;
}

export interface TunerState {
  on: boolean;
  mute: boolean;
  ref_hz: number;
}

/** One poll of the pedal's tuner (`TUNE`). */
export type TuneReading =
  | { state: "off" }
  | { state: "no_signal"; level_db: number | null }
  | { state: "note"; name: string; octave: number; hz: number; cents: number; level_db: number | null; onset: number | null };

export type TransportMode = "usb" | "ble";

export interface TransportState {
  mode: TransportMode;
  selected: { id: string; name: string | null } | null;
  connected_ble: boolean;
  error: string | null;
  note: string | null;
  chunk_bytes: number;
}

export interface BleDevice {
  id: string;
  name: string | null;
  rssi: number | null;
  services: string[];
  matches: boolean;
}

export type FsAction = [number, number, number];

export interface LedCfg {
  bright: number;
  tuner: string;
  tap: number;
  meter: string;
  hold: number;
  anim: number;
  idle: number;
}

export interface FsLed {
  custom: boolean;
  on: string;
  off: string;
}

export interface QuickSetupInfo {
  key: string;
  label: string;
  desc: string;
  layout: string;
}

export interface GlobalsSchema {
  geq: { bands_hz: number[]; min_db: number; max_db: number };
  hpf: { off: number; min_hz: number; max_hz: number; step_hz: number };
  led: {
    bright: { min: number; max: number; step: number };
    tuner: string[];
    tap: number[];
    meter: string[];
    hold: number[];
    anim: number[];
    idle: number[];
    default_on_colors: string[];
    dim_divisor: number;
    palette: string[];
  };
  footswitch_quick_setups: QuickSetupInfo[];
}

export interface GlobalSettings {
  geq: number[] | null;
  hpf: number | null;
  layout: string | null;
  fsa: FsAction[][] | null;
  quick_active: string | null;
  led: LedCfg | null;
  fsled: FsLed[] | null;
  schema: GlobalsSchema;
  dirty: boolean;
}
