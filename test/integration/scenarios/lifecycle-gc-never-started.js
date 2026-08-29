/**
 * A hook that was constructed but never started can be garbage collected
 * and the process exits normally. Runs with --expose-gc.
 */
// Scenario scripts must only run under runScenario(); a bare `node --test`
// would otherwise execute this file (and call start()) without a watchdog.
if (process.env.SELECTION_HOOK_SCENARIO !== "1") {
  console.error("scenario must run via runScenario()");
  process.exit(2);
}

const { runScenarioBody } = require("../../helpers/scenario-runtime.js");

runScenarioBody((SelectionHook) => {
  if (typeof global.gc !== "function") throw new Error("global.gc missing: run with --expose-gc");
  let hook = new SelectionHook();
  hook.on("error", () => {});
  hook = null;
  global.gc();
  global.gc();
  return { collected: true };
});
