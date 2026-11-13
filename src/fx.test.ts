import { describe, expect, it } from "vitest";
import { isAmpCategory, shortcutFor } from "./fx";

const ev = (key: string, extra: Record<string, unknown> = {}, tag = "DIV") =>
  ({ key, ctrlKey: false, metaKey: false, altKey: false, shiftKey: false, repeat: false, target: { tagName: tag, isContentEditable: false }, ...extra }) as never;

describe("shortcuts", () => {
  it("1-4 switch scenes", () => {
    expect(shortcutFor(ev("1"))).toEqual({ type: "scene", scene: 0 });
    expect(shortcutFor(ev("4"))).toEqual({ type: "scene", scene: 3 });
    expect(shortcutFor(ev("5"))).toBeNull();
  });
  it("T toggles the tuner, but not while typing", () => {
    expect(shortcutFor(ev("t"))).toEqual({ type: "tuner" });
    expect(shortcutFor(ev("T"))).toEqual({ type: "tuner" });
    expect(shortcutFor(ev("t", {}, "INPUT"))).toBeNull();
    expect(shortcutFor(ev("t", { ctrlKey: true }))).toBeNull();
  });
  it("arrows navigate presets", () => {
    expect(shortcutFor(ev("ArrowLeft"))).toEqual({ type: "preset", delta: -1 });
    expect(shortcutFor(ev("ArrowRight"))).toEqual({ type: "preset", delta: 1 });
  });
  it("ignored while typing, with modifiers or auto-repeat", () => {
    for (const tag of ["INPUT", "TEXTAREA", "SELECT"]) expect(shortcutFor(ev("1", {}, tag))).toBeNull();
    expect(shortcutFor(ev("1", { target: { tagName: "DIV", isContentEditable: true } }))).toBeNull();
    expect(shortcutFor(ev("1", { ctrlKey: true }))).toBeNull();
    expect(shortcutFor(ev("ArrowRight", { repeat: true }))).toBeNull();
  });
  it("amp category is filtered", () => {
    expect(isAmpCategory("amp")).toBe(true);
    expect(isAmpCategory("drive")).toBe(false);
  });
});
