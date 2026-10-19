/**
 * Scenario for issue #23 (macOS): stop() called right after start() must not
 * block forever. The native event thread fulfils its run-loop promise before
 * it actually enters CFRunLoopRun(); a stop request delivered in that window
 * used to be silently discarded, leaving stop() blocked in join() forever.
 *
 * Linux uses the same immediate-stop pressure to guard the XRecord startup
 * handshake. Before the fix, stop could disable the context before the worker
 * enabled it, then wait forever for the worker to leave XRecordEnableContext().
 * On Wayland's no-input fallback, changing the debounce shutdown predicate
 * without its condition-variable mutex could likewise lose the wake between
 * the worker's predicate check and wait, leaving stop() blocked in join().
 *
 * Requires input-monitoring: without event taps the run loop returns at once
 * and the race cannot be triggered.
 */
// Scenario scripts must only run under runScenario(); a bare `node --test`
// would otherwise execute this file (and call start()) without a watchdog.
if (process.env.SELECTION_HOOK_SCENARIO !== "1") {
  console.error("scenario must run via runScenario()");
  process.exit(2);
}

const { runScenarioBody } = require("../../helpers/scenario-runtime.js");

const FRESH_INSTANCE_CYCLES = 100;
const SAME_INSTANCE_CYCLES = 50;

runScenarioBody((SelectionHook) => {
  // Phase 1: fresh instance, stop() immediately after start()
  for (let i = 0; i < FRESH_INSTANCE_CYCLES; i++) {
    const hook = new SelectionHook();
    hook.on("error", () => {});
    if (!hook.start({ enableClipboard: false })) throw new Error(`start() returned false on fresh instance cycle ${i}`);
    hook.stop();
    hook.cleanup();
  }

  // Phase 2: same instance restarted repeatedly
  const hook = new SelectionHook();
  hook.on("error", () => {});
  for (let i = 0; i < SAME_INSTANCE_CYCLES; i++) {
    if (!hook.start({ enableClipboard: false })) throw new Error(`start() returned false on same instance cycle ${i}`);
    hook.stop();
  }
  hook.cleanup();

  return { freshCycles: FRESH_INSTANCE_CYCLES, sameCycles: SAME_INSTANCE_CYCLES };
});
