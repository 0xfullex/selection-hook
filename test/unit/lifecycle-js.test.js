/**
 * L1: JS-side lifecycle and #handleError contract of index.js.
 *
 * #handleError contract (pinned):
 *   1. level "error" without debug: no error event
 *   2. level "error" with debug: error event
 *   3. level "fatal": error event always; with no listener EventEmitter throws
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { loadIndexWith } = require("../helpers/fake-native.js");

// Tests below deliberately drive index.js into paths that log via console.error
// (fatal errors always print, debug mode prints non-fatal ones). Silence it for
// the duration of the test so the expected stack traces do not look like real
// failures in CI logs. Nothing asserts on console output.
function silenceConsoleError(t) {
  t.mock.method(console, "error", () => {});
}

test("lifecycle-js: native load failure surfaces in the constructor", (t) => {
  silenceConsoleError(t);
  const { SelectionHook } = loadIndexWith({ gypBuildThrows: true });
  assert.throws(() => new SelectionHook(), /Native module failed to load/);
});

test("lifecycle-js: native constructor failure surfaces in the constructor", () => {
  const { SelectionHook } = loadIndexWith({ constructorThrows: true });
  assert.throws(() => new SelectionHook(), /Failed to create native instance: fake constructor failure/);
});

test("lifecycle-js: second start() returns true without a second native start", () => {
  const { SelectionHook, natives } = loadIndexWith();
  const hook = new SelectionHook();
  const statuses = [];
  hook.on("status", (s) => statuses.push(s));
  assert.equal(hook.start(), true);
  assert.equal(hook.start(), true);
  assert.equal(natives[0].callsTo("start").length, 1);
  assert.deepEqual(statuses, ["started"]);
});

test("lifecycle-js: stop() when not running returns true without calling native", () => {
  const { SelectionHook, natives } = loadIndexWith();
  const hook = new SelectionHook();
  assert.equal(hook.stop(), true);
  assert.deepEqual(natives[0].callsTo("stop"), []);
});

test("lifecycle-js: cleanup() stops, drops listeners and the native instance", () => {
  const { SelectionHook, natives } = loadIndexWith();
  const hook = new SelectionHook();
  hook.on("status", () => {});
  hook.start();
  hook.cleanup();
  assert.equal(hook.isRunning(), false);
  assert.equal(natives[0].callsTo("stop").length, 1);
  assert.equal(hook.listenerCount("status"), 0);
  // native instance dropped: setter fails without touching the old native
  assert.equal(hook.enableClipboard(), false);
  assert.deepEqual(natives[0].callsTo("enableClipboard"), []);
});

test("lifecycle-js: start() after cleanup() creates a new native instance", () => {
  const { SelectionHook, natives } = loadIndexWith();
  const hook = new SelectionHook();
  hook.start();
  hook.cleanup();
  assert.equal(hook.start(), true);
  assert.equal(natives.length, 2);
  assert.equal(natives[1].callsTo("start").length, 1);
  assert.equal(hook.isRunning(), true);
});

test("lifecycle-js: native start failure is fatal - false, error event, not running", (t) => {
  silenceConsoleError(t);
  const { SelectionHook, natives } = loadIndexWith();
  const hook = new SelectionHook();
  const errors = [];
  hook.on("error", (err) => errors.push(err));
  natives[0].throwOn("start", new Error("no hook"));
  assert.equal(hook.start(), false);
  assert.equal(hook.isRunning(), false);
  assert.equal(errors.length, 1);
  assert.match(errors[0].message, /Failed to start hook: no hook/);
});

test("lifecycle-js: native stop failure is fatal - false, error event, not running", (t) => {
  silenceConsoleError(t);
  const { SelectionHook, natives } = loadIndexWith();
  const hook = new SelectionHook();
  const errors = [];
  hook.on("error", (err) => errors.push(err));
  hook.start();
  natives[0].throwOn("stop", new Error("stuck"));
  assert.equal(hook.stop(), false);
  assert.equal(hook.isRunning(), false);
  assert.equal(errors.length, 1);
  assert.match(errors[0].message, /Failed to stop hook: stuck/);
});

test("lifecycle-js: handleError contract 1 - non-fatal without debug emits nothing", () => {
  const { SelectionHook } = loadIndexWith();
  const hook = new SelectionHook();
  const errors = [];
  hook.on("error", (err) => errors.push(err));
  hook.start();
  assert.equal(hook.setClipboardMode(99), false);
  assert.deepEqual(errors, []);
});

test("lifecycle-js: handleError contract 2 - non-fatal with debug emits an error event", (t) => {
  silenceConsoleError(t);
  const { SelectionHook } = loadIndexWith();
  const hook = new SelectionHook();
  const errors = [];
  hook.on("error", (err) => errors.push(err));
  hook.start({ debug: true });
  assert.equal(hook.setClipboardMode(99), false);
  assert.equal(errors.length, 1);
  assert.match(errors[0].message, /Invalid clipboard mode: Invalid argument/);
});

test("lifecycle-js: handleError contract 3 - fatal without an error listener throws", (t) => {
  silenceConsoleError(t);
  const { SelectionHook, natives } = loadIndexWith();
  const hook = new SelectionHook();
  hook.start();
  natives[0].throwOn("stop", new Error("stuck"));
  assert.throws(() => hook.stop(), /Failed to stop hook: stuck/);
});
