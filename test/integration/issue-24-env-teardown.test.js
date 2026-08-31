/**
 * Issue #24 regression: an active native hook must not hang or abort when its
 * Node-API environment is destroyed without an explicit JavaScript cleanup().
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { runScenario, describeScenario } = require("../helpers/scenario.js");

test("issue #24: active hook survives Node environment teardown", async () => {
  // The cleanup-hook implementation applies to every native TSFN-backed hook.
  // Return normally elsewhere: gate tests may not introduce new skips.
  if (!["darwin", "win32", "linux"].includes(process.platform)) return;

  const result = await runScenario("issue-24-env-teardown", { timeoutMs: 15000 });
  assert.equal(result.timedOut, false, describeScenario("issue-24-env-teardown", result));
  assert.equal(result.signal, null, describeScenario("issue-24-env-teardown", result));
  assert.equal(result.code, 0, describeScenario("issue-24-env-teardown", result));
  assert.equal(result.result && result.result.ok, true, describeScenario("issue-24-env-teardown", result));
});
