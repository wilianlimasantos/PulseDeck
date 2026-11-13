// Browser-only stand-in for the Rust backend (used by `npm run dev` outside Tauri, for UI work).
import type { BleDevice, Block, EffectType, Level, Listing, Progress, Setlist, Snapshot, TransportState, UploadOutcome, GlobalSettings, FsLed } from "./types";
import type { Listeners } from "./api";

const fx = (id: number, name: string, category: string, parameters: [string, number, number, number][]): EffectType => ({
  id, name, category, parameters: parameters.map(([n, min, max, d], index) => ({ index, name: n, min, max, default: d })),
});
const effects: EffectType[] = [
  fx(1, "gate", "dynamics", [["umbral_db", -90, 0, -70], ["release_ms", 5, 1000, 120]]),
  fx(2, "boost", "drive", [["ganancia_db", -12, 24, 6]]),
  fx(3, "overdrive", "drive", [["drive_db", 0, 40, 18], ["tono_hz", 1000, 9000, 4000], ["nivel_db", -30, 0, -8], ["os_n", 0, 3, 1], ["limpio_pct", 0, 100, 0]]),
  fx(5, "delay", "delay", [["tiempo_ms", 20, 1200, 380], ["feedback_pct", 0, 95, 35], ["mezcla_pct", 0, 100, 25]]),
  fx(7, "ir", "cab", [["mezcla_pct", 0, 100, 100], ["nivel_db", -30, 12, 0], ["ir_slot", 0, 8, 0], ["largo_ms", 0, 85, 21]]),
  fx(27, "reverb2", "reverb", [["mezcla_pct", 0, 100, 20], ["decay_ms", 100, 9000, 1800]]),
  fx(11, "chorus", "mod", [["velocidad_chz", 5, 1000, 80], ["profundidad_pct", 0, 100, 40]]),
  fx(4, "eq", "eq", [["graves_db", -15, 15, 0], ["medios_db", -15, 15, 0], ["medios_hz", 200, 5000, 800], ["agudos_db", -15, 15, 0]]),
];
const mk = (id: number, bank: "P" | "Q", position: number, type_name: string, on = true): Block => ({
  id, bank, position, type_name, on, ticks: 40, lane: 0, group: null, irlen: null,
  params: effects.find((e) => e.name === type_name)!.parameters.map((p) => p.default),
});

let nextId = 20;
let tick = 0;
const library: Setlist[] = [{ name: "Geral", items: [] }, { name: "Ensaio", items: [] }, { name: "Show", items: [] }];

const snap: Snapshot = {
  unsaved: { preset: false, scene: false },
  status: { state: "connected", port: "/dev/ttyACM0", model: "Tank-G", version: "M5.26", firmware_number: 526, flags: ["rel", "amx", "ble"], pong: "PONG tankg_fw M5.26 rel amx ble" },
  effects,
  chain: {
    blocks: [mk(1, "P", 0, "gate"), mk(2, "P", 1, "boost"), mk(3, "P", 2, "overdrive", false), mk(4, "Q", 0, "ir"), mk(5, "Q", 1, "delay"), mk(6, "Q", 2, "reverb2")],
    amp_present: true, amp_on: true, cpu: { used: 640, budget: 1000 },
  },
  presets: { presets: Array.from({ length: 32 }, (_, slot) => ({ slot, block_count: 5, name: slot === 0 ? "Clean Demo" : slot === 1 ? "Crunch Demo" : `Preset ${String(slot + 1).padStart(2, "0")}`, color: null, scene_mask: 15 })), current: 0 },
  scenes: { stored: [true, true, true, false], current: 0 },
  ampeq: { volume: 0, bass: 20, mid: -10, treble: 30, presence: 10, drive: 0 },
  level: { in_db10: -300, out_db10: -250, cpu: { used: 640, budget: 1000 }, chain_generation: 1, preset: 0, scene: 0, amp_on: true },
  captures: { entries: [{ slot: 0, bytes: 2200, name: "Demo Plexi", trim_db: 0, input_dbu: 12 }, { slot: 1, bytes: 1900, name: "Dumble Clean", trim_db: null, input_dbu: null }], current: 0, slots: 15 },
  irs: { entries: [{ slot: 0, bytes: 8000, name: "4x12 Greenback", trim_db: null, input_dbu: null }], current: -1 as unknown as number, slots: 8 },
  model: "Demo Plexi",
  demo: true,
};
const listeners = new Set<Listeners>();
const emit = () => listeners.forEach((l) => l.snapshot(structuredClone(snap)));
const recompute = () => {
  snap.chain.cpu.used = 380 * Number(snap.chain.amp_on) + snap.chain.blocks.filter((b) => b.on).length * 55;
  snap.level.cpu = { ...snap.chain.cpu };
};

const files: Listing = {
  path: "/demo/Captures", parent: "/demo",
  entries: [
    { path: "/demo/Captures/Metal", name: "Metal", kind: "dir", size: 0, info: null, error: null },
    ...[
      ["Marshall Plexi 1959.nam", "amp", "Marshall", "Plexi"],
      ["Mesa Rectifier Full Rig.nam", "amp_cab", "Mesa", "Dual Rectifier"],
      ["Fender Twin Clean.nam", "amp_pedal_cab", "Fender", "Twin Reverb"],
      ["Vox AC30 Head.nam", "preamp", "Vox", "AC30"],
      ["Ibanez TS9.nam", "pedal", "Ibanez", "TS9"],
    ].map(([name, gt, make, model]) => ({
      path: `/demo/Captures/${name}`, name: name as string, kind: "nam" as const, size: 214000, error: null,
      info: { name: (name as string).replace(".nam", ""), gear_type: gt as string, class: gt!.includes("cab") ? "FULL_RIG" as const : gt === "pedal" ? "OTHER" as const : "AMP_ONLY" as const,
        input_level_dbu: 12.3, architecture: "WaveNet", sample_rate: 48000, gear_make: make as string, gear_model: model as string, tone_type: null, modeled_by: null },
    })),
    { path: "/demo/Captures/Celestion V30 4x12.wav", name: "Celestion V30 4x12.wav", kind: "wav", size: 192000, info: null, error: null },
    { path: "/demo/Captures/Broken.nam", name: "Broken.nam", kind: "nam", size: 12, info: null, error: "not a valid .nam (JSON)" },
  ],
};

let transport: TransportState = { mode: "usb", selected: null, connected_ble: false, error: null, note: null, chunk_bytes: 20 };
const transportListeners = new Set<(t: TransportState) => void>();
const pushTransport = () => transportListeners.forEach((f) => f({ ...transport }));
const fakeDevices: BleDevice[] = [
  { id: "AA:01", name: "Tank-G 4F2A", rssi: -48, services: ["6e400001-b5a3-f393-e0a9-e50e24dcca9e"], matches: true },
  { id: "AA:02", name: "UltraRig-91C0", rssi: -77, services: [], matches: true },
  { id: "AA:03", name: "Galaxy Buds", rssi: -60, services: [], matches: false },
];

const mockSchema: GlobalSettings["schema"] = {
  geq: { bands_hz: [100, 400, 1200, 3200, 7000], min_db: -12, max_db: 12 },
  hpf: { off: 0, min_hz: 20, max_hz: 120, step_hz: 5 },
  led: {
    bright: { min: 0, max: 100, step: 5 }, tuner: ["off", "needle", "strobe"], tap: [0, 1, 2], meter: ["off", "edit", "always"],
    hold: [0, 1], anim: [0, 1], idle: [0, 30, 60, 120], default_on_colors: ["28beff", "28ff5a", "ffaa1e", "e63cff"], dim_divisor: 24,
    palette: ["28beff", "28ff5a", "ffaa1e", "e63cff", "ff3b30", "ffffff", "ffe14a", "ff6ec7", "7a5cff", "00e5c0"],
  },
  footswitch_quick_setups: [
    { key: "factory", label: "Fábrica", desc: "Os footswitches funcionam como no firmware original da M-VAVE. Suas ações próprias ficam guardadas.", layout: "factory" },
    { key: "scenes", label: "Modo Cena", desc: "Footswitches 1–4 chamam as cenas 1–4 (segurar: ações extras).", layout: "scenes" },
    { key: "presets", label: "Modo Preset", desc: "1–2 trocam o preset, 3 troca a cena e 4 é tap tempo.", layout: "custom" },
    { key: "stompbox", label: "Stompbox", desc: "1–3 ligam e desligam blocos da cadeia; 4 é tap tempo.", layout: "custom" },
  ],
};
const mockGlobals: GlobalSettings = {
  geq: [0, 0, 0, 0, 0], hpf: 55, layout: "factory", fsa: null, quick_active: "factory",
  led: { bright: 100, tuner: "needle", tap: 1, meter: "edit", hold: 1, anim: 1, idle: 0 },
  fsled: mockSchema.led.default_on_colors.map((c) => ({ custom: false, on: c, off: "000000" })),
  schema: mockSchema, dirty: false,
};

export const mock = {
  subscribeTransport(fn: (t: TransportState) => void) {
    transportListeners.add(fn);
    return () => { transportListeners.delete(fn); };
  },
  subscribe(l: Listeners) {
    listeners.add(l);
    l.snapshot(structuredClone(snap));
    const t = setInterval(() => {
      const wob = Math.round(Math.sin(Date.now() / 900) * 25);
      const lvl: Level = { ...snap.level, cpu: { used: snap.chain.cpu.used + wob, budget: snap.chain.cpu.budget } };
      l.meter(lvl);
    }, 250);
    return () => { listeners.delete(l); clearInterval(t); };
  },
  async call<T>(cmd: string, a: Record<string, unknown> = {}): Promise<T> {
    const blk = (id: unknown) => snap.chain.blocks.find((b) => b.id === id);
    const renumber = () => (["P", "Q"] as const).forEach((bk) => snap.chain.blocks.filter((b) => b.bank === bk).forEach((b, i) => (b.position = i)));
    switch (cmd) {
      case "get_global_settings": return structuredClone(mockGlobals) as T;
      case "set_global_eq": mockGlobals.geq![a.band as number] = a.value as number; mockGlobals.dirty = true; return a.value as T;
      case "set_global_eq_all": mockGlobals.geq = [...(a.values as number[])]; mockGlobals.dirty = true; return mockGlobals.geq as T;
      case "set_input_hpf": mockGlobals.hpf = a.hz as number; mockGlobals.dirty = true; return undefined as T;
      case "set_footswitch_mode": {
        const q = mockSchema.footswitch_quick_setups.find((x) => x.key === a.setup);
        mockGlobals.quick_active = a.setup as string; mockGlobals.layout = q?.layout ?? null; mockGlobals.dirty = true;
        return structuredClone(mockGlobals) as T;
      }
      case "set_led_config": (mockGlobals.led as unknown as Record<string, unknown>)[a.key as string] = /^\d+$/.test(a.value as string) ? Number(a.value) : a.value; mockGlobals.dirty = true; return undefined as T;
      case "set_footswitch_led": mockGlobals.fsled![a.fs as number] = a.led as FsLed; mockGlobals.dirty = true; return undefined as T;
      case "save_global_settings": mockGlobals.dirty = false; return undefined as T;
      case "transport_get": return { ...transport } as T;
      case "transport_set": transport = { ...transport, mode: a.mode as TransportState["mode"], selected: null, connected_ble: false, error: null }; pushTransport(); return { ...transport } as T;
      case "ble_scan": await new Promise((r) => setTimeout(r, 1200)); return fakeDevices.filter((d) => a.all || d.matches) as T;
      case "ble_connect": transport = { ...transport, mode: "ble", selected: { id: a.id as string, name: a.name as string | null }, connected_ble: true, error: null }; pushTransport(); return { ...transport } as T;
      case "ble_disconnect": transport = { ...transport, selected: null, connected_ble: false }; pushTransport(); return { ...transport } as T;
      case "get_snapshot": return structuredClone(snap) as T;
      case "protocol_info":
        return { amp_eq_ranges: { volume: [-240, 120], bass: [-120, 120], mid: [-120, 120], treble: [-120, 120], presence: [-120, 120], drive: [-120, 120] },
          preset_count: 32, scene_count: 4, capture_slots: 15, ir_slots: 8, vendor_id: 0x3654 } as T;
      case "set_preset": snap.presets.current = a.slot as number; emit(); return undefined as T;
      case "set_scene": snap.scenes.current = a.scene as number; emit(); return undefined as T;
      case "block_set_param": { const b = blk(a.id); if (b) b.params[a.index as number] = a.value as number; snap.unsaved.scene = true; return a.value as T; }
      case "block_set_on": { const b = blk(a.id); if (b) b.on = a.on as boolean; recompute(); emit(); return undefined as T; }
      case "amp_set_on": snap.chain.amp_on = a.on as boolean; recompute(); emit(); return undefined as T;
      case "amp_set_eq": snap.ampeq = a.eq as Snapshot["ampeq"]; return snap.ampeq as T;
      case "block_add": {
        const e = effects.find((x) => x.name === a.typeName);
        if (!e) throw "Efeito desconhecido";
        const id = nextId++;
        const b = mk(id, a.bank as "P" | "Q", 99, e.name);
        const bank = snap.chain.blocks.filter((x) => x.bank === a.bank);
        const pos = Math.min(a.position as number, bank.length);
        b.position = pos - 0.5; snap.chain.blocks.push(b);
        snap.chain.blocks.sort((x, y) => (x.bank === y.bank ? x.position - y.position : x.bank < y.bank ? -1 : 1)); renumber(); recompute(); emit(); return id as T;
      }
      case "block_remove": snap.chain.blocks = snap.chain.blocks.filter((b) => b.id !== a.id); renumber(); recompute(); emit(); return undefined as T;
      case "block_move": {
        const b = blk(a.id); if (!b) return undefined as T;
        b.bank = a.bank as "P" | "Q"; b.position = (a.position as number) - 0.5;
        snap.chain.blocks.sort((x, y) => (x.bank === y.bank ? x.position - y.position : x.bank < y.bank ? -1 : 1)); renumber(); emit(); return undefined as T;
      }
      case "amp_set_present": snap.chain.amp_present = a.present as boolean; snap.unsaved = { preset: true, scene: true }; emit(); return undefined as T;
      case "save_preset": snap.unsaved.preset = false; emit(); return undefined as T;
      case "save_scene": snap.unsaved.scene = false; emit(); return undefined as T;
      case "export_backup": case "restore_backup": return { presets: 2, captures: 2, irs: 1, globals: 4, warnings: [] } as T;
      case "capture_load": snap.model = snap.captures.entries.find((c) => c.slot === a.slot)?.name ?? null; emit(); return undefined as T;
      case "inspect_backup": return { firmware: "tankg_fw M5.26", created_unix: 1_760_000_000, presets: 2, captures: 2, irs: 1, globals: 4, preset_names: ["01 · Clean Demo", "02 · Crunch"] } as T;
      case "memory_usage": return { captures: { "0": [0, 1] }, irs: { "0": [0] } } as T;
      case "slot_rename": {
        const lib = a.kind === "ir" ? snap.irs : snap.captures;
        const e = lib.entries.find((x) => x.slot === a.slot);
        if (e) e.name = a.name as string;
        emit(); return [] as T;
      }
      case "slot_delete": {
        const lib = a.kind === "ir" ? snap.irs : snap.captures;
        lib.entries = lib.entries.filter((x) => x.slot !== a.slot);
        emit(); return undefined as T;
      }
      case "library_list": return structuredClone(library) as T;
      case "library_create_setlist": library.push({ name: a.name as string, items: [] }); return undefined as T;
      case "library_delete_setlist": { const i = library.findIndex((s) => s.name === a.name); if (i >= 0) library.splice(i, 1); return undefined as T; }
      case "library_save": {
        const it = { path: `/demo/${a.setlist}/${a.name}.pulsedeck`, name: a.name as string, setlist: a.setlist as string, created_unix: Math.floor(Date.now() / 1000), firmware: "tankg_fw M5.26", has_capture: true, ir_count: 1, scenes: 2 };
        library.find((s) => s.name === a.setlist)?.items.push(it);
        return it as T;
      }
      case "library_load": snap.unsaved = { preset: a.saveSlot === null, scene: a.saveSlot === null }; emit(); return { capture_slot: 0, capture_uploaded: false, ir_slots: [0], irs_uploaded: 0, saved_slot: a.saveSlot, warnings: [] } as T;
      case "library_delete": library.forEach((s) => { s.items = s.items.filter((i) => i.path !== a.path); }); return undefined as T;
      case "library_import": return undefined as T;
      case "tuner_set": return { on: a.on, mute: a.mute, ref_hz: a.refHz } as T;
      case "tuner_read": {
        tick += 1;
        const cents = Math.sin(tick / 25) * 40;
        return { state: "note", name: "A", octave: 2, hz: 110 * Math.pow(2, cents / 1200), cents, level_db: -21, onset: 0 } as T;
      }
      case "default_folder": return "/demo/Captures" as T;
      case "list_folder": return structuredClone(files) as T;
      case "upload_file": {
        const f = files.entries.find((e) => e.path === a.path);
        const steps: Progress[] = [{ stage: "Convertendo .nam → A2L1", done: 0, total: 1 }, { stage: "Enviando captura", done: 50, total: 100 }, { stage: "Concluído", done: 1, total: 1 }];
        for (const p of steps) { listeners.forEach((l) => l.progress(p)); await new Promise((r) => setTimeout(r, 350)); }
        const out: UploadOutcome = { kind: f?.kind === "wav" ? "ir" : "capture", slot: 2, name: f?.info?.name ?? "IR", bytes: 1532, detail: "ok", class: f?.info?.class ?? null, input_level_dbu: f?.info?.input_level_dbu ?? null };
        if (out.kind === "capture") { snap.captures.entries.push({ slot: 2, bytes: 1532, name: out.name, trim_db: null, input_dbu: null }); snap.model = out.name; emit(); }
        return out as T;
      }
      default: throw new Error(`mock: ${cmd}`);
    }
  },
};
