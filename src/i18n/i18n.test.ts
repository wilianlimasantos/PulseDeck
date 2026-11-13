import { describe, expect, it } from "vitest";
import { detectLang, translate } from "./index";
import { en } from "./en";
import { pt } from "./pt-BR";

describe("i18n", () => {
  it("detects the language: saved choice wins, then pt* -> pt-BR, else en", () => {
    expect(detectLang(null, "pt-BR")).toBe("pt-BR");
    expect(detectLang(null, "pt-PT")).toBe("pt-BR");
    expect(detectLang(null, "PT")).toBe("pt-BR");
    expect(detectLang(null, "en-US")).toBe("en");
    expect(detectLang(null, "de")).toBe("en");
    expect(detectLang(null, undefined)).toBe("en");
    expect(detectLang("en", "pt-BR")).toBe("en");
    expect(detectLang("pt-BR", "en-US")).toBe("pt-BR");
    expect(detectLang("klingon", "en-US")).toBe("en");
  });

  it("has the same keys in both languages and no empty text", () => {
    expect(Object.keys(en).sort()).toEqual(Object.keys(pt).sort());
    for (const d of [pt, en]) for (const v of Object.values(d)) expect(v.length).toBeGreaterThan(0);
  });

  it("uses the same {placeholders} in both languages", () => {
    const ph = (s: string) => (s.match(/\{\w+\}/g) ?? []).sort().join();
    for (const k of Object.keys(pt) as (keyof typeof pt)[]) expect(ph(en[k]), k).toBe(ph(pt[k]));
  });

  it("interpolates", () => {
    expect(translate("en", "scene.title", { n: 3 })).toContain("3");
    expect(translate("pt-BR", "scene.title", { n: 3 })).toContain("3");
  });
});

import { localizeBackend } from "./backend";

describe("backend messages", () => {
  it("leaves Portuguese untouched and translates known messages for English", () => {
    expect(localizeBackend("Lendo preset 3 — Lead", "pt-BR")).toBe("Lendo preset 3 — Lead");
    expect(localizeBackend("Lendo preset 3 — Lead", "en")).toBe("Reading preset 3 — Lead");
    expect(localizeBackend("Pedaleira desconectada", "en")).toBe("Pedal disconnected");
    expect(localizeBackend("Slot 40 fora do intervalo 1..15", "en")).toBe("Slot 40 out of range 1..15");
    expect(localizeBackend("mensagem desconhecida", "en")).toBe("mensagem desconhecida");
  });
  it("translates the Bluetooth large-transfer warning, including the operation name", () => {
    const pt =
      "Capturas (.nam) e IRs (.wav) move vários KB e, por Bluetooth, cada linha é fatiada em pacotes de ~20 bytes: fica muito lento e uma queda no meio pode deixar o slot incompleto. Conecte a pedaleira pelo cabo USB para essa operação.";
    const out = localizeBackend(pt, "en");
    expect(out.startsWith("Captures (.nam) and IRs (.wav): this moves several KB")).toBe(true);
    expect(out).toContain("USB cable");
  });
});
