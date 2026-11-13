import { useRigStore, type SidebarTab } from "../rigStore";
import FileBrowser from "./FileBrowser";
import Library from "./Library";
import SlotsTab from "./SlotsTab";
import ProgressBar from "./ProgressBar";
import { useT, type Key } from "../i18n";

const TABS: Array<[SidebarTab, Key]> = [["files", "sidebar.files"], ["library", "sidebar.library"], ["slots", "sidebar.slots"]];

export default function Sidebar() {
  const t = useT();
  const tab = useRigStore((s) => s.sidebarTab);
  const setTab = useRigStore((s) => s.setSidebarTab);
  return (
    <aside className="flex w-[304px] shrink-0 flex-col border-r border-ink-700 bg-ink-850">
      <div role="tablist" aria-label={t("sidebar.label")} className="flex gap-1 border-b border-ink-700 p-2">
        {TABS.map(([k, label]) => (
          <button
            key={k}
            role="tab"
            id={`tab-${k}`}
            aria-selected={tab === k}
            aria-controls={`panel-${k}`}
            onClick={() => setTab(k)}
            className={`flex-1 rounded-md px-1 py-1.5 text-[10px] font-semibold uppercase tracking-[0.1em] transition ${
              tab === k ? "bg-ink-600 text-white" : "text-ink-300 hover:bg-ink-700"
            }`}
          >
            {t(label)}
          </button>
        ))}
      </div>
      <div role="tabpanel" id={`panel-${tab}`} aria-labelledby={`tab-${tab}`} className="flex min-h-0 flex-1 flex-col">
        {tab === "files" ? <FileBrowser /> : tab === "library" ? <Library /> : <SlotsTab />}
      </div>
      <ProgressBar className="border-t border-ink-700 p-3" />
    </aside>
  );
}
