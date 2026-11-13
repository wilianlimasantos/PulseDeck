import { create } from "zustand";
import { api } from "./api";
import { AudioPitchDetector, audioSupported, describeAudioError, listInputs, type AudioInput } from "./audioPitch";
import { clampRef, inTuneWithHysteresis, REF_DEFAULT, type NoteReading } from "./pitch";
import { useStore } from "./store";
import { t } from "./i18n";

export type TunerSource = "pedal" | "audio";

interface Prefs {
  source: TunerSource;
  mute: boolean;
  refHz: number;
  deviceId: string | null;
}

const KEY = "pulsedeck.tuner";

function loadPrefs(): Prefs {
  const d: Prefs = { source: "pedal", mute: true, refHz: REF_DEFAULT, deviceId: null };
  try {
    const p = JSON.parse(localStorage.getItem(KEY) ?? "null") as Partial<Prefs> | null;
    if (!p) return d;
    return {
      source: p.source === "audio" ? "audio" : "pedal",
      mute: p.mute ?? d.mute,
      refHz: clampRef(p.refHz ?? d.refHz),
      deviceId: typeof p.deviceId === "string" ? p.deviceId : null,
    };
  } catch {
    return d;
  }
}

function savePrefs(p: Prefs) {
  try {
    localStorage.setItem(KEY, JSON.stringify(p));
  } catch {
    /* storage unavailable: the tuner works without remembering */
  }
}

interface TunerState extends Prefs {
  open: boolean;
  reading: NoteReading | null;
  /** input level in dB (pedal: dBFS from TUNE, audio: RMS) */
  level: number | null;
  inTune: boolean;
  error: string | null;
  devices: AudioInput[];

  openTuner: () => Promise<void>;
  closeTuner: (notifyPedal?: boolean) => void;
  toggleTuner: () => void;
  setSource: (s: TunerSource) => void;
  setMute: (m: boolean) => void;
  setRef: (hz: number) => void;
  setDevice: (id: string | null) => void;
  refreshDevices: () => Promise<void>;
}

const isConnected = () => useStore.getState().snap?.status.state === "connected";
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

/** Bumped on every start / stop: a stale loop or callback sees a different token and quits. */
let run = 0;
let detector: AudioPitchDetector | null = null;
let refTimer: ReturnType<typeof setTimeout> | null = null;

export const useTuner = create<TunerState>((set, get) => {
  const prefs = (): Prefs => {
    const { source, mute, refHz, deviceId } = get();
    return { source, mute, refHz, deviceId };
  };
  const push = (token: number, r: NoteReading | null, level: number | null) => {
    if (token !== run) return;
    set({ reading: r, level, inTune: r ? inTuneWithHysteresis(get().inTune, r.cents) : false });
  };
  const stopSource = () => {
    detector?.stop();
    detector = null;
  };

  /** Poll the pedal's `TUNE` (one command in flight at a time). */
  const pollPedal = async (token: number) => {
    let fails = 0;
    let lastNote = 0;
    while (token === run && get().open && get().source === "pedal") {
      try {
        const r = await api.tunerRead();
        if (token !== run) return;
        fails = 0;
        if (r.state === "off") {
          // the pedal left tuner mode by itself (footswitch): follow it
          get().closeTuner(false);
          return;
        }
        if (r.state === "note") {
          lastNote = performance.now();
          push(token, { name: r.name, octave: r.octave, hz: r.hz, cents: r.cents }, r.level_db);
        } else if (performance.now() - lastNote > 500) {
          push(token, null, r.level_db);
        }
      } catch {
        if (token !== run) return;
        if (++fails >= 3) {
          set({ source: "audio", error: t("tn.pedal_silent") });
          void start(token);
          return;
        }
      }
      await sleep(40);
    }
  };

  const start = async (token: number) => {
    stopSource();
    if (get().source === "pedal") {
      void pollPedal(token);
      return;
    }
    const d = new AudioPitchDetector({
      deviceId: get().deviceId,
      getA4: () => get().refHz,
      onReading: (r, level) => push(token, r, level),
    });
    try {
      await d.start();
      if (token !== run) {
        d.stop();
        return;
      }
      detector = d;
      void get().refreshDevices(); // labels are only available after the permission was granted
    } catch (e) {
      d.stop();
      if (token === run) set({ error: describeAudioError(e) });
    }
  };

  /** Keep the pedal's tuner mode (mute, reference) in step with the UI. */
  const syncPedal = async () => {
    if (!get().open || !isConnected()) return;
    try {
      await api.tunerSet(true, get().mute, get().refHz);
    } catch (e) {
      useStore.getState().toast("error", typeof e === "string" ? e : e instanceof Error ? e.message : t("tn.fail"));
    }
  };

  return {
    ...loadPrefs(),
    open: false,
    reading: null,
    level: null,
    inTune: false,
    error: null,
    devices: [],

    async openTuner() {
      if (get().open) return;
      const connected = isConnected();
      const token = ++run;
      // the pedal's reading needs a pedal; without one the tuner runs on the system audio
      set({
        open: true,
        reading: null,
        level: null,
        inTune: false,
        error: null,
        source: connected ? get().source : "audio",
      });
      if (connected) await syncPedal();
      if (token !== run) return;
      await start(token);
    },

    closeTuner(notifyPedal = true) {
      if (!get().open) return;
      run++;
      stopSource();
      if (refTimer) clearTimeout(refTimer);
      set({ open: false, reading: null, level: null, inTune: false });
      if (notifyPedal && isConnected()) void api.tunerSet(false, get().mute, get().refHz).catch(() => undefined);
    },

    toggleTuner() {
      if (get().open) get().closeTuner();
      else void get().openTuner();
    },

    setSource(source) {
      if (source === "pedal" && !isConnected()) return;
      set({ source, error: null, reading: null, level: null, inTune: false });
      savePrefs(prefs());
      if (get().open) void start(++run);
    },

    setMute(mute) {
      set({ mute });
      savePrefs(prefs());
      void syncPedal();
    },

    setRef(hz) {
      set({ refHz: clampRef(hz) });
      savePrefs(prefs());
      if (refTimer) clearTimeout(refTimer);
      refTimer = setTimeout(() => void syncPedal(), 200); // the pedal computes with it: do not flood it
    },

    setDevice(deviceId) {
      set({ deviceId, error: null });
      savePrefs(prefs());
      if (get().open && get().source === "audio") void start(++run);
    },

    async refreshDevices() {
      if (!audioSupported()) return;
      try {
        set({ devices: await listInputs() });
      } catch {
        /* enumeration is a convenience */
      }
    },
  };
});

// pedal unplugged while its tuner is the source: carry on with the system audio
useStore.subscribe((s) => {
  const tuner = useTuner.getState();
  if (tuner.open && tuner.source === "pedal" && s.snap && s.snap.status.state !== "connected") {
    useTuner.getState().setSource("audio");
    useTuner.setState({ error: t("tn.pedal_gone") });
  }
});
