import { create } from "zustand";
import { api, subscribeTransport } from "./api";
import type { BleDevice, TransportMode, TransportState } from "./types";

interface BleStore {
  t: TransportState;
  devices: BleDevice[];
  scanning: boolean;
  scanned: boolean;
  showAll: boolean;
  scanError: string | null;
  init: () => Promise<void>;
  setMode: (m: TransportMode) => Promise<void>;
  scan: () => Promise<void>;
  setShowAll: (v: boolean) => void;
  connect: (d: BleDevice) => Promise<void>;
  disconnect: () => Promise<void>;
}

let started = false;

export const useBle = create<BleStore>((set, get) => ({
  t: { mode: "usb", selected: null, connected_ble: false, error: null, note: null, chunk_bytes: 20 },
  devices: [],
  scanning: false,
  scanned: false,
  showAll: false,
  scanError: null,
  init: async () => {
    if (started) return;
    started = true;
    set({ t: await api.transportGet() });
    await subscribeTransport((t) => set({ t }));
  },
  setMode: async (m) => {
    set({ t: await api.transportSet(m), devices: [], scanned: false, scanError: null });
  },
  scan: async () => {
    if (get().scanning) return;
    set({ scanning: true, scanError: null });
    try {
      set({ devices: await api.bleScan(get().showAll), scanned: true });
    } catch (e) {
      set({ scanError: String(e), devices: [], scanned: true });
    } finally {
      set({ scanning: false });
    }
  },
  setShowAll: (showAll) => {
    set({ showAll });
    void get().scan();
  },
  connect: async (d) => set({ t: await api.bleConnect(d.id, d.name) }),
  disconnect: async () => set({ t: await api.bleDisconnect() }),
}));

/** Signal strength as 0..4 bars from RSSI (dBm). */
export function rssiBars(rssi: number | null): number {
  if (rssi === null) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  if (rssi >= -88) return 1;
  return 0;
}
