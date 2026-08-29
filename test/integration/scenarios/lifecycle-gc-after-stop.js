/**
 * A hook that was started and then stopped can be garbage collected and the
 * process exits normally. Runs with --expose-gc.
 *
 * A started-but-not-stopped hook is intentionally NOT tested: its
 * ThreadSafeFunction is referenced, so it keeps the process alive and is not
 * collectable by design.
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
  if (!hook.start()) throw new Error("start() returned false");
  if (!hook.stop()) throw new Error("stop() returned false");
  hook = null;
  global.gc();
  global.gc();
  return { collected: true };
});
