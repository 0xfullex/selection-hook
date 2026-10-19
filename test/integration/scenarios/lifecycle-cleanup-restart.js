/**
 * cleanup() drops the native instance; a later start() must recreate it.
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
  const firstStart = hook.start({ enableClipboard: false });
  hook.cleanup();
  const runningAfterCleanup = hook.isRunning();

  // cleanup() removed all listeners, re-attach before starting again
  hook.on("error", () => {});
  const secondStart = hook.start({ enableClipboard: false });
  const runningAfterSecondStart = hook.isRunning();
  const stopped = hook.stop();
  hook.cleanup();

  return { firstStart, runningAfterCleanup, secondStart, runningAfterSecondStart, stopped };
});
