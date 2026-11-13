import { describe, expect, it } from "vitest";
import { clampRef, formatCents, freqToNote, inTuneWithHysteresis, matchingString, median, noteToFreq, yin } from "./pitch";

const SR = 48000;
const tone = (f: number, n = 4096, harmonics = [1]) =>
  Float32Array.from({ length: n }, (_, i) => harmonics.reduce((s, h, k) => s + Math.sin((2 * Math.PI * f * h * i) / SR) / (k + 1), 0) * 0.3);

describe("yin", () => {
  it.each([82.41, 110, 146.83, 196, 246.94, 329.63, 440])("finds %s Hz within 0.1 cent-ish", (f) => {
    const p = yin(tone(f), SR)!;
    expect(p).not.toBeNull();
    expect(Math.abs(1200 * Math.log2(p.freq / f))).toBeLessThan(2);
    expect(p.probability).toBeGreaterThan(0.9);
  });

  it("keeps the fundamental when harmonics are strong (no octave error)", () => {
    const p = yin(tone(82.41, 4096, [1, 2, 3, 4]), SR)!;
    expect(Math.abs(1200 * Math.log2(p.freq / 82.41))).toBeLessThan(5);
  });

  it("returns null for silence and for noise", () => {
    expect(yin(new Float32Array(4096), SR)).toBeNull();
    let s = 1;
    const noise = Float32Array.from({ length: 4096 }, () => ((s = (s * 1664525 + 1013904223) >>> 0) / 2 ** 32 - 0.5) * 0.5);
    expect(yin(noise, SR)).toBeNull();
  });
});

describe("notes", () => {
  it("maps frequencies to notes and cents", () => {
    expect(freqToNote(440)).toMatchObject({ name: "A", octave: 4 });
    expect(freqToNote(82.41)).toMatchObject({ name: "E", octave: 2 });
    const sharp = freqToNote(440 * Math.pow(2, 10 / 1200))!;
    expect(sharp.cents).toBeCloseTo(10, 5);
    const flat = freqToNote(440 * Math.pow(2, -20 / 1200))!;
    expect(flat.cents).toBeCloseTo(-20, 5);
    expect(freqToNote(466.16)).toMatchObject({ name: "A#", octave: 4 });
    expect(freqToNote(0)).toBeNull();
  });

  it("honours the A4 reference", () => {
    // 432 Hz is exactly A4 when the reference is 432, and ~31.8 cents flat of A4 at 440
    expect(freqToNote(432, 432)!.cents).toBeCloseTo(0, 5);
    expect(freqToNote(432, 440)!.cents).toBeCloseTo(-31.77, 1);
    expect(noteToFreq("A", 4, 432)).toBeCloseTo(432, 9);
    expect(noteToFreq("E", 2, 440)).toBeCloseTo(82.407, 2);
  });

  it("clamps the reference to 432..445 Hz", () => {
    expect(clampRef(400)).toBe(432);
    expect(clampRef(500)).toBe(445);
    expect(clampRef(440.4)).toBe(440);
    expect(clampRef(NaN)).toBe(440);
  });

  it("in-tune zone is +-3 cents with hysteresis", () => {
    expect(inTuneWithHysteresis(false, 2.9)).toBe(true);
    expect(inTuneWithHysteresis(false, 3.5)).toBe(false);
    expect(inTuneWithHysteresis(true, 3.5)).toBe(true);
    expect(inTuneWithHysteresis(true, 5)).toBe(false);
  });

  it("helpers", () => {
    expect(median([3, 1, 2])).toBe(2);
    expect(median([4, 1, 3, 2])).toBe(2.5);
    expect(matchingString({ name: "B", octave: 3 })).toBe(4);
    expect(matchingString({ name: "C", octave: 3 })).toBe(-1);
    expect(formatCents(3.04)).toBe("+3.0");
    expect(formatCents(-12.26)).toBe("-12.3");
    expect(formatCents(0)).toBe("0.0");
  });
});
