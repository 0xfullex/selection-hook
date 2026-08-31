/**
 * Regression scenario for issue #24. Terminating a Node.js Worker destroys its
 * Node-API environment while the native hook and its TSFNs are still active.
 * Before the fixes, macOS and Linux could hang or abort and Windows could
 * access-violate when SelectionHook::~SelectionHook() released a TSFN already
 * being torn down.
 */
if (process.env.SELECTION_HOOK_SCENARIO !== "1") {
  console.error("scenario must run via runScenario()");
  process.exit(2);
}

const path = require("node:path");
const { isMainThread, parentPort, Worker } = require("node:worker_threads");

if (!isMainThread) {
  const SelectionHook = require(path.join(__dirname, "..", "..", "..", "index.js"));
  const hook = new SelectionHook();
  hook.on("error", () => {});
  if (!hook.start()) throw new Error("start() returned false");

  // Keep the wrapper reachable until worker.terminate() begins environment
  // teardown. The test intentionally does not call stop() or cleanup().
  globalThis.issue24Hook = hook;
  parentPort.postMessage({ ready: true });
} else {
  const worker = new Worker(__filename);
  const timer = setTimeout(() => {
    process.stdout.write(JSON.stringify({ ok: false, error: "worker termination timed out" }) + "\n", () =>
      process.exit(1)
    );
  }, 10000);

  worker.once("message", async ({ ready }) => {
    if (!ready) return;
    try {
      const workerExitCode = await worker.terminate();
      clearTimeout(timer);
      process.stdout.write(JSON.stringify({ ok: true, workerExitCode }) + "\n", () => process.exit(0));
    } catch (error) {
      clearTimeout(timer);
      process.stdout.write(
        JSON.stringify({ ok: false, error: error && error.stack ? error.stack : String(error) }) + "\n",
        () => process.exit(1)
      );
    }
  });

  worker.once("error", (error) => {
    clearTimeout(timer);
    process.stdout.write(JSON.stringify({ ok: false, error: error.stack || String(error) }) + "\n", () =>
      process.exit(1)
    );
  });
}
