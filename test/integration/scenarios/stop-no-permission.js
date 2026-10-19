/**
 * Single start() -> stop() cycle.
 *
 * On a macOS host without accessibility permission this exercises the path
 * where no event tap can be created: the event thread's run loop returns
 * immediately and stop() must still post to a run loop that is kept alive by
 * the CFRetain() taken in the event thread (issue #23 hardening). On other
 * hosts it is a plain start/stop smoke run.
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
  const trusted = process.platform === "darwin" ? hook.macIsProcessTrusted() : null;
  const started = hook.start({ enableClipboard: false });
  const runningAfterStart = hook.isRunning();
  const stopped = hook.stop();
  const runningAfterStop = hook.isRunning();
  hook.cleanup();
  return { trusted, started, runningAfterStart, stopped, runningAfterStop };
});
