/**
 * Gate: start / stop / cleanup / GC lifecycle semantics that do not depend on
 * OS permissions. Anything that calls start() runs in a watchdog child.
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { requires } = require("../helpers/capabilities.js");
const { withHook } = require("../helpers/native.js");
const { runScenario, describeScenario } = require("../helpers/scenario.js");

async function expectScenarioOk(name, options) {
  const r = await runScenario(name, options);
  assert.equal(r.timedOut, false, describeScenario(name, r));
  assert.equal(r.code, 0, describeScenario(name, r));
  assert.equal(r.result && r.result.ok, true, describeScenario(name, r));
  return r.result;
}

test("lifecycle: stop / isRunning / cleanup before start", (t) => {
  if (!requires(t, "native")) return;
  withHook((hook) => {
    assert.equal(hook.isRunning(), false);
    assert.equal(hook.stop(), true);
    assert.doesNotThrow(() => hook.cleanup());
    assert.equal(hook.isRunning(), false);
  });
});

test("lifecycle: second start() is a no-op returning true", async (t) => {
  if (!requires(t, "native")) return;
  const result = await expectScenarioOk("lifecycle-double-start");
  assert.equal(result.first, true);
  assert.equal(result.second, true);
  assert.equal(result.running, true);
  assert.deepEqual(result.statuses, ["started", "stopped"]);
});

test("lifecycle: start -> stop -> start -> stop", async (t) => {
  if (!requires(t, "native")) return;
  const result = await expectScenarioOk("lifecycle-restart");
  assert.deepEqual(result.results, [true, true, true, true]);
  assert.deepEqual(result.running, [true, false, true, false]);
  assert.deepEqual(result.statuses, ["started", "stopped", "started", "stopped"]);
});

test("lifecycle: start() after cleanup() recreates the native instance", async (t) => {
  if (!requires(t, "native")) return;
  const result = await expectScenarioOk("lifecycle-cleanup-restart");
  assert.equal(result.firstStart, true);
  assert.equal(result.runningAfterCleanup, false);
  assert.equal(result.secondStart, true);
  assert.equal(result.runningAfterSecondStart, true);
  assert.equal(result.stopped, true);
});

test("lifecycle: never-started hook is collectable and process exits", async (t) => {
  if (!requires(t, "native")) return;
  const result = await expectScenarioOk("lifecycle-gc-never-started", { execArgv: ["--expose-gc"] });
  assert.equal(result.collected, true);
});

test("lifecycle: stopped hook is collectable and process exits", async (t) => {
  if (!requires(t, "native")) return;
  const result = await expectScenarioOk("lifecycle-gc-after-stop", { execArgv: ["--expose-gc"] });
  assert.equal(result.collected, true);
});
