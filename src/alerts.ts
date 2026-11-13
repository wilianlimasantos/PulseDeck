import type { Key, Params } from "./i18n";
import type { Chain, Cpu, GearClass } from "./types";

/** CPU usage above this percentage raises the alert (spec: > 85%). */
export const CPU_ALERT_THRESHOLD = 85;
/** Hysteresis: the alert clears only once usage falls back under this. */
export const CPU_ALERT_CLEAR = 80;

/** Name of the impulse-response block type in the effect catalog. */
export const IR_TYPE = "ir";

export function cpuPercent(cpu: Cpu): number {
  return cpu.budget > 0 ? (cpu.used * 100) / cpu.budget : 0;
}

export function nextCpuHigh(prev: boolean, pct: number): boolean {
  if (pct > CPU_ALERT_THRESHOLD) return true;
  if (pct <= CPU_ALERT_CLEAR) return false;
  return prev;
}

export const irBlocks = (chain: Chain) => chain.blocks.filter((b) => b.type_name === IR_TYPE);
export const activeIrBlocks = (chain: Chain) => irBlocks(chain).filter((b) => b.on);

export type AlertAction = "disable_ir" | "enable_ir" | "add_ir";

export interface UxAlert {
  id: "full_rig_with_ir" | "amp_only_without_ir";
  level: "warn";
  /** i18n keys: the text is translated when rendered, so a language switch updates it */
  title: Key;
  message: Key;
  params: Params;
  action: { kind: AlertAction; label: Key };
}

/**
 * Smart alert for a freshly loaded `.nam`:
 * - FULL_RIG while an IR block is active -> suggest turning the IR off (saves CPU, avoids double cab);
 * - AMP_ONLY with no active IR block -> suggest an IR (the amp alone sounds raw).
 */
export function uploadAlert(cls: GearClass | null, chain: Chain, captureName?: string): UxAlert | null {
  const who = captureName ? `“${captureName}”` : null;
  if (cls === "FULL_RIG" && activeIrBlocks(chain).length > 0) {
    return {
      id: "full_rig_with_ir",
      level: "warn",
      title: "alert.full_title",
      message: "alert.full_msg",
      params: { who: who ?? "" },
      action: { kind: "disable_ir", label: "alert.full_action" },
    };
  }
  if (cls === "AMP_ONLY" && activeIrBlocks(chain).length === 0) {
    const hasOff = irBlocks(chain).length > 0;
    return {
      id: "amp_only_without_ir",
      level: "warn",
      title: "alert.amp_title",
      message: "alert.amp_msg",
      params: { who: who ?? "" },
      action: hasOff
        ? { kind: "enable_ir", label: "alert.amp_on" }
        : { kind: "add_ir", label: "alert.amp_add" },
    };
  }
  return null;
}
