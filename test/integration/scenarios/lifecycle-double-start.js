/**
 * start() twice on the same instance: the second call is a no-op that
 * returns true and does not emit a second "started" status.
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
  const statuses = [];
  hook.on("status", (s) => statuses.push(s));
  const first = hook.start({ enableClipboard: false });
  const second = hook.start({ enableClipboard: false });
  const running = hook.isRunning();
  hook.stop();
  hook.cleanup();
  return { first, second, running, statuses };
});
