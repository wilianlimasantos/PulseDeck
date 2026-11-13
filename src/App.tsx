import { useEffect } from "react";
import { useBle } from "./bleStore";
import { useStore } from "./store";
import SavePresetDialog from "./components/SavePresetDialog";
import Tuner from "./components/Tuner";
import { shortcutFor } from "./fx";
import { useTuner } from "./tunerStore";
import AlertsBar from "./components/AlertsBar";
import GlobalSettingsModal from "./components/GlobalSettingsModal";
import MemoryModal from "./components/MemoryModal";
import Sidebar from "./components/Sidebar";
import SignalChain from "./components/SignalChain";
import TopBar from "./components/TopBar";
import { useLang, useT } from "./i18n";
import { localizeBackend } from "./i18n/backend";

function Toasts() {
  const toasts = useStore((s) => s.toasts);
  const dismiss = useStore((s) => s.dismissToast);
  return (
    <div className="pointer-events-none fixed bottom-4 right-4 z-50 flex w-96 flex-col gap-2" aria-live="polite">
      {toasts.map((tt) => (
        <button
          key={tt.id}
          onClick={() => dismiss(tt.id)}
          className={`pointer-events-auto rounded-lg border px-3 py-2 text-left text-[12px] shadow-xl backdrop-blur ${
            tt.kind === "error" ? "border-danger/60 bg-ink-800/95 text-danger" : tt.kind === "ok" ? "border-pulse/50 bg-ink-800/95 text-pulse" : "border-ink-500 bg-ink-800/95"
          }`}
        >
          {tt.text}
        </button>
      ))}
    </div>
  );
}

function Disconnected() {
  const t = useT();
  const lang = useLang((s) => s.lang);
  const st = useStore((s) => s.snap!.status);
  const ble = useBle((s) => s.t.mode === "ble");
  return (
    <div className="flex h-full flex-col items-center justify-center gap-3 text-center text-ink-300">
      <svg width="64" height="64" viewBox="0 0 26 26" aria-hidden><path d="M2 14h5l3-8 4 14 3-6h6" fill="none" stroke="var(--color-ink-500)" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" /></svg>
      <div className="text-[15px] font-medium text-ink-100">
        {st.state === "not_responding" ? t("app.not_ultrarig") : ble ? t("app.connect_ble") : t("app.connect_usb")}
      </div>
      <div className="max-w-sm text-[12px]">
        {st.state === "not_responding"
          ? `${st.port}: ${localizeBackend(st.reason, lang)}`
          : ble ? t("app.connect_ble_hint") : t("app.connect_hint")}
      </div>
    </div>
  );
}

export default function App() {
  const t = useT();
  const ready = useStore((s) => s.ready);
  const init = useStore((s) => s.init);
  const connected = useStore((s) => s.snap?.status.state === "connected");

  useEffect(() => {
    void init().catch((e) => useStore.getState().toast("error", String(e)));
  }, [init]);

  // 1-4 = scenes, Left/Right = previous/next preset (not while typing in a field)
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const a = shortcutFor(e);
      const st = useStore.getState();
      if (!a) return;
      // T toggles the tuner (it also works without a pedal, on the system audio); any other modal blocks it
      if (a.type === "tuner") {
        if (st.saveDialog || document.querySelector('[aria-modal="true"]:not([data-tuner])')) return;
        e.preventDefault();
        useTuner.getState().toggleTuner();
        return;
      }
      if (st.snap?.status.state !== "connected" || st.saveDialog || document.querySelector('[aria-modal="true"]')) return;
      e.preventDefault();
      if (a.type === "scene") void st.setScene(a.scene);
      else {
        const n = st.info?.preset_count ?? 32;
        void st.setPreset((((st.snap.presets.current ?? 0) + a.delta) % n + n) % n);
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  if (!ready) return <div className="flex h-full items-center justify-center text-ink-300">{t("app.starting")}</div>;
  return (
    <div className="flex h-full flex-col">
      <TopBar />
      <AlertsBar />
      <div className="flex min-h-0 flex-1">
        <Sidebar />
        <main className="min-w-0 flex-1 bg-gradient-to-b from-ink-900 to-ink-950">{connected ? <SignalChain /> : <Disconnected />}</main>
      </div>
      <Toasts />
      <SavePresetDialog />
      <MemoryModal />
      <GlobalSettingsModal />
      <Tuner />
    </div>
  );
}
