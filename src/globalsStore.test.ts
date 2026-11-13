import { describe, expect, it } from "vitest";
import { dimColor } from "./globalsStore";

describe("dimColor", () => {
  it("divides each channel like the firmware's default inactive ring", () => {
    expect(dimColor("28beff", 24)).toBe("01070a");
    expect(dimColor("ffffff", 24)).toBe("0a0a0a");
    expect(dimColor("000000", 24)).toBe("000000");
  });
});
