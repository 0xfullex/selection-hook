/**
 * Regression tests for issue #23 (macOS): stop() right after start() hung forever.
 *
 * "stop-race: input-monitoring" is the release-gate test (test/lib/gate.js
 * GATE_TEST_NAME); it needs real input-monitoring so it skips on GitHub-hosted
 * macOS runners and is enforced by `npm run test:local` on a trusted dev machine.
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { requires } = require("../helpers/capabilities.js");
const { runScenario, describeScenario } = require("../helpers/scenario.js");

test("stop-race: input-monitoring", async (t) => {
  if (!requires(t, "native", "input-monitoring")) return;

  const r = await runScenario("stop-race", { timeoutMs: 30000 });
  assert.equal(r.timedOut, false, "stop() hung: " + describeScenario("stop-race", r));
  assert.equal(r.code, 0, describeScenario("stop-race", r));
  assert.equal(r.result && r.result.ok, true, describeScenario("stop-race", r));
  t.diagnostic(`fresh cycles ${r.result.freshCycles}, same-instance cycles ${r.result.sameCycles}`);
});

test("stop-race: single start/stop cycle", async (t) => {
  if (!requires(t, "native")) return;

  const r = await runScenario("stop-no-permission", { timeoutMs: 30000 });
  assert.equal(r.timedOut, false, "stop() hung: " + describeScenario("stop-no-permission", r));
  assert.equal(r.code, 0, describeScenario("stop-no-permission", r));
  assert.equal(r.result && r.result.ok, true, describeScenario("stop-no-permission", r));
  assert.equal(r.result.started, true);
  assert.equal(r.result.runningAfterStart, true);
  assert.equal(r.result.stopped, true);
  assert.equal(r.result.runningAfterStop, false);
  if (process.platform === "darwin") {
    t.diagnostic(`macIsProcessTrusted in child: ${r.result.trusted}`);
  }
});
