import { cpuPercent, CPU_ALERT_THRESHOLD } from "../alerts";
import { currentAlert, useStore } from "../store";
import { useT } from "../i18n";

export default function AlertsBar() {
  const t = useT();
  const cpu = useStore((s) => s.snap!.level.cpu);
  const high = useStore((s) => s.cpuHigh);
  const alert = useStore(currentAlert);
  const run = useStore((s) => s.runAlertAction);
  const dismiss = useStore((s) => s.dismissAlert);
  if (!high && !alert) return null;
  return (
    <div className="flex shrink-0 flex-col gap-1 border-b border-ink-700 bg-ink-850 px-4 py-2" role="alert">
      {high && (
        <div className="flex items-center gap-3 rounded-lg border border-danger/50 bg-danger/10 px-3 py-2 text-[12px]">
          <span className="blink text-danger">●</span>
          <div className="flex-1">
            <b className="text-danger">{t("alert.dsp", { pct: cpuPercent(cpu).toFixed(0) })}</b> ({cpu.used}/{cpu.budget}) — {t("alert.dsp_body", { limit: CPU_ALERT_THRESHOLD })}
          </div>
        </div>
      )}
      {alert && (
        <div className="flex items-center gap-3 rounded-lg border border-amber/50 bg-amber/10 px-3 py-2 text-[12px]">
          <span className="text-amber">▲</span>
          <div className="flex-1"><b className="text-amber">{t(alert.title)}.</b> {t(alert.message, { ...alert.params, who: alert.params.who || t("alert.who_default") })}</div>
          <button onClick={() => run(alert.action.kind)} className="rounded-md bg-amber px-2.5 py-1 font-semibold text-ink-950 hover:brightness-110">{t(alert.action.label)}</button>
          <button onClick={dismiss} aria-label={t("alert.dismiss")} className="px-1 text-ink-200 hover:text-white">✕</button>
        </div>
      )}
    </div>
  );
}
