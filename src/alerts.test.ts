import { describe, expect, it } from "vitest";
import { cpuPercent, nextCpuHigh, uploadAlert } from "./alerts";
import type { Block, Chain } from "./types";

const blk = (id: number, type_name: string, on: boolean): Block => ({
  id, bank: "Q", position: id, type_name, on, params: [], ticks: 0, lane: 0, group: null, irlen: null,
});
const chain = (blocks: Block[]): Chain => ({ blocks, amp_present: true, amp_on: true, cpu: { used: 0, budget: 1000 } });

describe("upload alerts", () => {
  it("FULL_RIG + active IR -> suggest disabling IR", () => {
    const a = uploadAlert("FULL_RIG", chain([blk(1, "ir", true)]), "Plexi");
    expect(a?.id).toBe("full_rig_with_ir");
    expect(a?.action.kind).toBe("disable_ir");
  });
  it("FULL_RIG with IR off or absent -> no alert", () => {
    expect(uploadAlert("FULL_RIG", chain([blk(1, "ir", false)]))).toBeNull();
    expect(uploadAlert("FULL_RIG", chain([]))).toBeNull();
  });
  it("AMP_ONLY without active IR -> suggest enabling/adding", () => {
    expect(uploadAlert("AMP_ONLY", chain([]))?.action.kind).toBe("add_ir");
    expect(uploadAlert("AMP_ONLY", chain([blk(1, "ir", false)]))?.action.kind).toBe("enable_ir");
  });
  it("AMP_ONLY with active IR -> no alert", () => {
    expect(uploadAlert("AMP_ONLY", chain([blk(1, "ir", true)]))).toBeNull();
  });
  it("other / unknown class -> no alert", () => {
    expect(uploadAlert("OTHER", chain([]))).toBeNull();
    expect(uploadAlert(null, chain([]))).toBeNull();
  });
});

describe("cpu alert", () => {
  it("percent", () => {
    expect(cpuPercent({ used: 850, budget: 1000 })).toBe(85);
    expect(cpuPercent({ used: 5, budget: 0 })).toBe(0);
  });
  it("triggers strictly above 85% and has hysteresis", () => {
    expect(nextCpuHigh(false, 85)).toBe(false);
    expect(nextCpuHigh(false, 85.1)).toBe(true);
    expect(nextCpuHigh(true, 83)).toBe(true);
    expect(nextCpuHigh(true, 80)).toBe(false);
  });
});
