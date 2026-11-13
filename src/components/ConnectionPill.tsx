import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { rssiBars, useBle } from "../bleStore";
import { useLang, useT } from "../i18n";
import { localizeBackend } from "../i18n/backend";
import { useStore } from "../store";
import type { BleDevice, TransportMode } from "../types";

export function BluetoothIcon({ className = "" }: { className?: string }) {
  return (
    <svg width="12" height="14" viewBox="0 0 12 16" aria-hidden className={className}>
      <path d="M2 4.5l8 7-4 3.5V1l4 3.5-8 7" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}

function Bars({ rssi }: { rssi: number | null }) {
  const n = rssiBars(rssi);
  return (
    <span className="flex items-end gap-[2px]" aria-hidden>
      {[1, 2, 3, 4].map((i) => (
        <span key={i} style={{ height: 3 + i * 3 }} className={`w-[3px] rounded-sm ${i <= n ? "bg-pulse" : "bg-ink-600"}`} />
      ))}
    </span>
  );
}

function DeviceRow({ d, active, busy, onConnect }: { d: BleDevice; active: boolean; busy: boolean; onConnect: () => void }) {
  const t = useT();
  return (
    <li className="flex items-center gap-3 rounded-lg px-2 py-2 hover:bg-ink-700">
      <Bars rssi={d.rssi} />
      <div className="min-w-0 flex-1 leading-tight">
        <div className="truncate text-[12px] font-medium">{d.name ?? t("conn.no_name")}</div>
        <div className="truncate font-mono text-[10px] text-ink-300">
          {d.rssi !== null ? `${d.rssi} dBm` : t("conn.signal")}{!d.matches && t("conn.not_pedal")}
        </div>
      </div>
      <button
        disabled={busy || active}
        onClick={onConnect}
        className="shrink-0 rounded-md border border-pulse/60 px-2.5 py-1 text-[11px] font-medium text-pulse hover:bg-pulse/10 disabled:cursor-not-allowed disabled:opacity-40"
      >
        {active ? t("conn.connected") : t("conn.connect")}
      </button>
    </li>
  );
}

export default function ConnectionPill() {
  const t = useT();
  const snap = useStore((s) => s.snap)!;
  const ble = useBle();
  const tr = ble.t;
  const lang = useLang((s) => s.lang);
  const lb = (s: string | null) => (s ? localizeBackend(s, lang) : s);
  const [open, setOpen] = useState(false);
  const [pos, setPos] = useState({ top: 0, left: 0 });
  const btn = useRef<HTMLButtonElement>(null);
  const panel = useRef<HTMLDivElement>(null);

  useEffect(() => { void useBle.getState().init(); }, []);

  const toggle = () => {
    const r = btn.current?.getBoundingClientRect();
    if (r) setPos({ top: r.bottom + 8, left: Math.max(8, Math.min(r.left, window.innerWidth - 348)) });
    setOpen((o) => !o);
  };

  useEffect(() => {
    if (!open) return;
    const close = (e: MouseEvent) => {
      const n = e.target as Node;
      if (!panel.current?.contains(n) && !btn.current?.contains(n)) setOpen(false);
    };
    const key = (e: KeyboardEvent) => {
      if (e.key === "Escape") { setOpen(false); btn.current?.focus(); }
    };
    const resize = () => setOpen(false);
    window.addEventListener("mousedown", close);
    window.addEventListener("keydown", key);
    window.addEventListener("resize", resize);
    return () => {
      window.removeEventListener("mousedown", close);
      window.removeEventListener("keydown", key);
      window.removeEventListener("resize", resize);
    };
  }, [open]);

  const st = snap.status;
  const isBle = tr.mode === "ble";
  const connecting = isBle && tr.selected !== null && st.state !== "connected";
  const [dot, text, sub] =
    st.state === "connected"
      ? ["bg-pulse shadow-[0_0_8px_var(--color-pulse)]", `${st.model} · ${st.version}`, isBle ? `Bluetooth · ${tr.selected?.name ?? st.port}` : st.port]
      : connecting
        ? ["bg-amber blink", t("conn.connecting_ble"), tr.selected?.name ?? ""]
        : isBle
          ? [tr.error ? "bg-amber" : "bg-ink-500", tr.error ? t("conn.ble_off") : t("conn.ble_pick"), lb(tr.error) ?? t("conn.click_search")]
          : st.state === "not_responding"
            ? ["bg-amber", t("conn.no_pong"), st.port]
            : ["bg-ink-500 blink", t("conn.searching_usb"), "VID 0x3654"];

  const title = st.state === "connected" ? st.pong : st.state === "not_responding" ? st.reason : (lb(tr.error) ?? "");
  const bleLive = isBle && st.state === "connected";

  return (
    <>
      <button
        ref={btn}
        onClick={toggle}
        aria-haspopup="dialog"
        aria-expanded={open}
        title={title}
        className="flex min-w-0 items-center gap-2 rounded-lg border border-ink-600 bg-ink-800 px-2.5 py-1.5 text-left outline-none hover:border-ink-300 focus-visible:border-pulse"
      >
        <span className={`h-2.5 w-2.5 shrink-0 rounded-full ${dot}`} />
        <div className="min-w-0 leading-tight">
          <div className="flex items-center gap-1.5 truncate text-[12px] font-medium">
            <span className="truncate">{text}</span>
            {isBle && <BluetoothIcon className={bleLive ? "shrink-0 text-pulse" : "shrink-0 text-ink-300"} />}
            {snap.demo && <span className="rounded bg-ink-600 px-1 text-[9px] uppercase text-ink-200">demo</span>}
          </div>
          <div className="hidden truncate font-mono text-[10px] text-ink-300 min-[1100px]:block">{sub}</div>
        </div>
        <span className="shrink-0 text-ink-300" aria-hidden>▾</span>
      </button>
      {open &&
        createPortal(
          <div
            ref={panel}
            role="dialog"
            aria-label={t("conn.dialog")}
            style={{ top: pos.top, left: pos.left }}
            className="fixed z-[90] w-[340px] max-w-[calc(100vw-16px)] rounded-xl border border-ink-600 bg-ink-800 p-3 shadow-2xl"
          >
            <div role="tablist" className="mb-3 grid grid-cols-2 gap-1 rounded-lg bg-ink-900 p-1">
              {(["usb", "ble"] as TransportMode[]).map((m) => (
                <button
                  key={m}
                  role="tab"
                  aria-selected={tr.mode === m}
                  onClick={() => tr.mode !== m && void ble.setMode(m)}
                  className={`flex items-center justify-center gap-1.5 rounded-md py-1.5 text-[12px] font-medium ${tr.mode === m ? "bg-ink-600 text-white" : "text-ink-300 hover:text-white"}`}
                >
                  {m === "ble" && <BluetoothIcon />}
                  {m === "usb" ? "USB" : "Bluetooth"}
                </button>
              ))}
            </div>

            {!isBle ? (
              <p className="px-1 text-[12px] leading-relaxed text-ink-300">{t("conn.usb_hint")}</p>
            ) : (
              <div>
                {bleLive && (
                  <div className="mb-3 flex items-center justify-between gap-2 rounded-lg border border-pulse/40 bg-pulse/10 px-3 py-2">
                    <div className="min-w-0 text-[12px] leading-tight">
                      <div className="flex items-center gap-1.5 font-medium text-pulse"><BluetoothIcon /> {t("conn.connected")}</div>
                      <div className="truncate text-ink-200">{tr.selected?.name ?? t("conn.pedal")} · {t("conn.packets", { n: tr.chunk_bytes })}</div>
                    </div>
                    <button onClick={() => void ble.disconnect()} className="shrink-0 rounded-md border border-ink-500 px-2.5 py-1 text-[11px] hover:border-ink-300">{t("conn.disconnect")}</button>
                  </div>
                )}
                {connecting && (
                  <div className="mb-3 flex items-center justify-between gap-2 rounded-lg border border-amber/40 px-3 py-2 text-[12px]">
                    <span>{t("conn.connecting_to", { name: tr.selected?.name ?? t("conn.device") })}</span>
                    <button onClick={() => void ble.disconnect()} className="rounded-md border border-ink-500 px-2.5 py-1 text-[11px] hover:border-ink-300">{t("conn.cancel")}</button>
                  </div>
                )}
                {tr.error && !bleLive && !connecting && (
                  <div role="alert" className="mb-3 rounded-lg border border-amber/40 bg-amber/10 px-3 py-2 text-[12px] leading-snug">{lb(tr.error)}</div>
                )}
                {tr.note && <div className="mb-3 rounded-lg border border-ink-600 px-3 py-2 text-[11px] leading-snug text-ink-200">{lb(tr.note)}</div>}

                <button
                  onClick={() => void ble.scan()}
                  disabled={ble.scanning}
                  className="mb-2 flex w-full items-center justify-center gap-2 rounded-lg border border-pulse/60 py-2 text-[12px] font-medium text-pulse hover:bg-pulse/10 disabled:opacity-60"
                >
                  <BluetoothIcon className={ble.scanning ? "blink" : ""} />
                  {ble.scanning ? t("conn.searching") : ble.scanned ? t("conn.search_again") : t("conn.search")}
                </button>

                {ble.scanError && <div role="alert" className="mb-2 rounded-lg border border-amber/40 bg-amber/10 px-3 py-2 text-[12px] leading-snug">{lb(ble.scanError)}</div>}
                {ble.scanned && !ble.scanError && ble.devices.length === 0 && (
                  <p className="px-1 py-2 text-[12px] text-ink-300">{t("conn.none_found")}</p>
                )}
                {ble.devices.length > 0 && (
                  <ul className="max-h-60 overflow-y-auto">
                    {ble.devices.map((d) => (
                      <DeviceRow
                        key={d.id}
                        d={d}
                        active={bleLive && tr.selected?.id === d.id}
                        busy={connecting}
                        onConnect={() => void ble.connect(d)}
                      />
                    ))}
                  </ul>
                )}
                <label className="mt-2 flex items-center gap-2 px-1 text-[11px] text-ink-300">
                  <input type="checkbox" checked={ble.showAll} onChange={(e) => ble.setShowAll(e.target.checked)} />
                  {t("conn.show_all")}
                </label>
                <p className="mt-2 px-1 text-[11px] leading-snug text-ink-300">{t("conn.ble_limit")}</p>
              </div>
            )}
          </div>,
          document.body,
        )}
    </>
  );
}
