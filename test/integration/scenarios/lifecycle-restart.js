/**
 * start -> stop -> start -> stop on one instance; isRunning() and the status
 * event sequence must track each transition.
 *
 * On Wayland without input-device access, this also guards the debounce-thread
 * shutdown race where an immediate stop could lose its condition-variable wake.
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
  const running = [];
  const results = [];

  results.push(hook.start());
  running.push(hook.isRunning());
  results.push(hook.stop());
  running.push(hook.isRunning());
  results.push(hook.start());
  running.push(hook.isRunning());
  results.push(hook.stop());
  running.push(hook.isRunning());

  hook.cleanup();
  return { results, running, statuses };
});
