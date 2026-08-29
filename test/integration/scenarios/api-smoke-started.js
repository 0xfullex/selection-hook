/**
 * Call every side-effect-free API while the hook is running and report the
 * return values. Excluded on purpose: writeToClipboard (side effect),
 * macRequestProcessTrust (may show a permission dialog).
 */
// Scenario scripts must only run under runScenario(); a bare `node --test`
// would otherwise execute this file (and call start()) without a watchdog.
if (process.env.SELECTION_HOOK_SCENARIO !== "1") {
  console.error("scenario must run via runScenario()");
  process.exit(2);
}

const { runScenarioBody } = require("../../helpers/scenario-runtime.js");

runScenarioBody((SelectionHook) => {
  const hook = new SelectionHook();
  hook.on("error", () => {});
  if (!hook.start()) throw new Error("start() returned false");

  const F = SelectionHook.FilterMode;
  const L = SelectionHook.FineTunedListType;
  const selection = hook.getCurrentSelection();
  const clipboard = hook.readFromClipboard();

  const calls = {
    isRunning: hook.isRunning(),
    enableMouseMoveEvent: hook.enableMouseMoveEvent(),
    disableMouseMoveEvent: hook.disableMouseMoveEvent(),
    enableClipboard: hook.enableClipboard(),
    disableClipboard: hook.disableClipboard(),
    setClipboardModeDefault: hook.setClipboardMode(F.DEFAULT, []),
    setClipboardModeInclude: hook.setClipboardMode(F.INCLUDE_LIST, ["a.exe"]),
    setGlobalFilterMode: hook.setGlobalFilterMode(F.EXCLUDE_LIST, ["b.exe"]),
    setFineTunedList: hook.setFineTunedList(L.INCLUDE_CLIPBOARD_DELAY_READ, ["c.exe"]),
    setSelectionPassiveModeOn: hook.setSelectionPassiveMode(true),
    setSelectionPassiveModeOff: hook.setSelectionPassiveMode(false),
  };

  const stopped = hook.stop();
  hook.cleanup();

  return {
    calls,
    selectionType: selection === null ? "null" : typeof selection,
    selectionTextType: selection ? typeof selection.text : null,
    clipboardType: clipboard === null ? "null" : typeof clipboard,
    stopped,
  };
});
