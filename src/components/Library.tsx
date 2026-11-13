import { useEffect, useMemo, useState } from "react";
import { presetCode } from "../fx";
import { useRigStore } from "../rigStore";
import { useStore } from "../store";
import type { LibItem } from "../types";
import CustomSelect, { type SelectOption } from "./CustomSelect";
import { useLang, useT } from "../i18n";

const date = (unix: number, lang: string) => (unix ? new Date(unix * 1000).toLocaleDateString(lang) : "");

function Tag({ children, tone = "ink" }: { children: React.ReactNode; tone?: "ink" | "amber" | "sky" | "rig" }) {
  const c = {
    ink: "border-ink-500 text-ink-200",
    amber: "border-amber/50 bg-amber/10 text-amber",
    sky: "border-sky-400/40 bg-sky-400/10 text-sky-300",
    rig: "border-rig/50 bg-rig/10 text-rig",
  }[tone];
  return <span className={`rounded border px-1.5 py-px text-[9px] font-bold tracking-wide ${c}`}>{children}</span>;
}

function Item({ item, canLoad }: { item: LibItem; canLoad: boolean }) {
  const t = useT();
  const lang = useLang((s) => s.lang);
  const load = useRigStore((s) => s.loadFromLibrary);
  const del = useRigStore((s) => s.deleteFromLibrary);
  const busy = useStore((s) => s.busy);
  return (
    <div className="group flex items-center gap-2 rounded-lg px-2 py-1.5 hover:bg-ink-700" title={t("lib.item_title", { path: item.path, fw: item.firmware || "?" })}>
      <div className="min-w-0 flex-1">
        <div className="truncate">{item.name}</div>
        <div className="mt-0.5 flex flex-wrap items-center gap-1">
          {item.has_capture && <Tag tone="amber">{t("lib.capture")}</Tag>}
          {item.ir_count > 0 && <Tag tone="sky">IR{item.ir_count > 1 ? ` ×${item.ir_count}` : ""}</Tag>}
          <Tag>{t(item.scenes === 1 ? "lib.scenes_one" : "lib.scenes_many", { n: item.scenes })}</Tag>
          <span className="text-[10px] text-ink-300">{date(item.created_unix, lang)}</span>
        </div>
      </div>
      <button
        disabled={!canLoad || busy}
        onClick={() => void load(item)}
        aria-label={t("lib.load_aria", { name: item.name })}
        className="shrink-0 rounded-md border border-ink-500 px-2 py-1 text-[11px] font-medium text-ink-100 transition hover:border-pulse hover:text-pulse disabled:cursor-not-allowed disabled:opacity-30"
      >
        {t("lib.load")}
      </button>
      <button
        disabled={busy}
        onClick={() => void del(item)}
        aria-label={t("lib.delete_aria", { name: item.name })}
        className="shrink-0 rounded-md px-1.5 py-1 text-ink-300 opacity-0 transition hover:text-danger focus:opacity-100 group-hover:opacity-100"
      >
        ✕
      </button>
    </div>
  );
}

export default function Library() {
  const t = useT();
  const setlists = useRigStore((s) => s.setlists);
  const saveSlot = useRigStore((s) => s.saveSlot);
  const { refreshLibrary, saveToLibrary, createSetlist, deleteSetlist, importPackage, setSaveSlot } = useRigStore.getState();
  const connected = useStore((s) => s.snap?.status.state === "connected");
  const busy = useStore((s) => s.busy);
  const presets = useStore((s) => s.snap?.presets);
  const count = useStore((s) => s.info?.preset_count ?? 32);

  const current = presets?.presets.find((p) => p.slot === presets.current)?.name ?? "";
  const [name, setName] = useState("");
  const [target, setTarget] = useState("Geral");
  const [newList, setNewList] = useState("");
  const [creating, setCreating] = useState(false);
  const [q, setQ] = useState("");

  useEffect(() => { void refreshLibrary(); }, [refreshLibrary]);
  useEffect(() => { if (!name) setName(current); }, [current, name]);
  useEffect(() => {
    if (setlists.length && !setlists.some((s) => s.name === target)) setTarget(setlists[0].name);
  }, [setlists, target]);

  const listOptions: SelectOption<string>[] = setlists.map((s) => ({ value: s.name, label: s.name, hint: String(s.items.length) }));
  const slotOptions: SelectOption<number>[] = useMemo(
    () => [
      { value: -1, label: t("lib.only_apply") },
      ...Array.from({ length: count }, (_, i) => ({
        value: i,
        label: `${presetCode(i)} · ${presets?.presets.find((p) => p.slot === i)?.name ?? t("lib.free")}`,
      })),
    ],
    [count, presets, t],
  );

  const ql = q.trim().toLowerCase();
  const submit = () => {
    const n = name.trim() || current || t("lib.preset_fallback");
    void saveToLibrary(target, n).then(() => setName(""));
  };

  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="space-y-2 p-3">
        <div className="text-[10px] uppercase tracking-wider text-ink-300">{t("lib.save_current")}</div>
        <input
          value={name}
          onChange={(e) => setName(e.target.value)}
          onKeyDown={(e) => e.key === "Enter" && connected && !busy && submit()}
          maxLength={60}
          placeholder={current || t("lib.name_ph")}
          aria-label={t("lib.name_aria")}
          className="w-full rounded-md border border-ink-600 bg-ink-900 px-2 py-1.5 text-[12px] outline-none focus:border-pulse"
        />
        <div className="flex gap-2">
          <div className="min-w-0 flex-1">
            <CustomSelect ariaLabel={t("lib.dest_folder")} value={target} options={listOptions} onChange={setTarget} />
          </div>
          <button
            disabled={!connected || busy}
            onClick={submit}
            className="shrink-0 rounded-md bg-pulse px-2.5 py-1.5 text-[12px] font-semibold text-ink-950 transition hover:brightness-110 disabled:opacity-40"
          >
            {t("lib.save")}
          </button>
        </div>

        <div className="pt-1 text-[10px] uppercase tracking-wider text-ink-300">{t("lib.load_into")}</div>
        <CustomSelect
          ariaLabel={t("lib.load_into_aria")}
          value={saveSlot ?? -1}
          options={slotOptions}
          onChange={(v) => setSaveSlot(v < 0 ? null : v)}
        />

        <input
          value={q}
          onChange={(e) => setQ(e.target.value)}
          placeholder={t("fb.filter_ph")}
          aria-label={t("lib.filter_aria")}
          className="w-full rounded-md border border-ink-600 bg-ink-900 px-2 py-1.5 text-[12px] outline-none focus:border-pulse"
        />
      </div>

      <div className="min-h-0 flex-1 space-y-2 overflow-y-auto px-2 pb-3">
        {setlists.map((s) => {
          const items = s.items.filter((i) => !ql || i.name.toLowerCase().includes(ql));
          if (ql && items.length === 0) return null;
          return (
            <section key={s.name} aria-label={t("lib.folder_aria", { name: s.name })}>
              <div className="flex items-center gap-1 px-1 pb-0.5">
                <h3 className="flex-1 truncate text-[10px] font-semibold uppercase tracking-[0.16em] text-ink-300">
                  {s.name} <span className="font-normal">· {s.items.length}</span>
                </h3>
                <button onClick={() => void importPackage(s.name)} className="rounded px-1 text-[10px] text-ink-300 hover:text-pulse" title={t("lib.import_title")}>{t("lib.import")}</button>
                {s.items.length === 0 && s.name !== "Geral" && (
                  <button onClick={() => void deleteSetlist(s.name)} aria-label={t("lib.remove_folder", { name: s.name })} className="rounded px-1 text-[10px] text-ink-300 hover:text-danger">✕</button>
                )}
              </div>
              {items.length === 0 ? (
                <div className="px-2 py-1.5 text-[11px] text-ink-500">{t("lib.empty_folder")}</div>
              ) : (
                items.map((i) => <Item key={i.path} item={i} canLoad={!!connected} />)
              )}
            </section>
          );
        })}
        {setlists.length === 0 && <div className="p-4 text-center text-ink-300">{t("lib.empty")}</div>}
      </div>

      <div className="border-t border-ink-700 p-2">
        {creating ? (
          <form
            className="flex gap-1.5"
            onSubmit={(e) => {
              e.preventDefault();
              if (!newList.trim()) return;
              void createSetlist(newList.trim()).then((ok) => {
                if (ok) { setTarget(newList.trim()); setNewList(""); setCreating(false); }
              });
            }}
          >
            <input
              autoFocus
              value={newList}
              onChange={(e) => setNewList(e.target.value)}
              onKeyDown={(e) => e.key === "Escape" && setCreating(false)}
              maxLength={60}
              placeholder={t("lib.new_name_ph")}
              aria-label={t("lib.new_name_aria")}
              className="min-w-0 flex-1 rounded-md border border-ink-600 bg-ink-900 px-2 py-1 text-[12px] outline-none focus:border-pulse"
            />
            <button type="submit" className="rounded-md border border-ink-500 px-2 text-[11px] hover:border-pulse hover:text-pulse">{t("lib.create")}</button>
          </form>
        ) : (
          <button onClick={() => setCreating(true)} className="w-full rounded-md border border-dashed border-ink-500 py-1.5 text-[11px] text-ink-300 hover:border-pulse hover:text-pulse">
            {t("lib.new_folder")}
          </button>
        )}
      </div>
    </div>
  );
}
