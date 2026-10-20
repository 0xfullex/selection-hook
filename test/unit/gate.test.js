/**
 * Unit tests for test/lib/gate.js
 *
 * evaluateGate() and checkSkipPolicy() decide whether a run counts as green,
 * so every condition is exercised in both its passing and failing form.
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { evaluateGate, checkSkipPolicy, SKIP_ALLOWLIST, GATE_TEST_NAME, WINDOWS_NATIVE_FIXTURES } = require("../lib/gate.js");

function summary({ passed = [], failed = [], skipped = [] } = {}) {
  return { passed, failed, skipped };
}

const greenSummary = () => summary({ passed: [GATE_TEST_NAME, "other"] });

// ---------------------------------------------------------------------------
// evaluateGate
// ---------------------------------------------------------------------------

test("gate: darwin, no failures, no permission skip, race test passed -> ok", () => {
  const result = evaluateGate(greenSummary(), { platform: "darwin" });
  assert.deepEqual(result, { ok: true, violations: [] });
});

test("gate: condition 0 - non-darwin platform is a violation", () => {
  const result = evaluateGate(greenSummary(), { platform: "linux" });
  assert.equal(result.ok, false);
  assert.equal(result.violations.length, 1);
  assert.match(result.violations[0], /darwin/);
  assert.match(result.violations[0], /linux/);
});

test("gate: condition 1 - any failed test is a violation", () => {
  const result = evaluateGate(summary({ passed: [GATE_TEST_NAME], failed: ["broken"] }), {
    platform: "darwin",
  });
  assert.equal(result.ok, false);
  assert.equal(result.violations.some((v) => v.includes("broken")), true);
});

test("gate: condition 2 - skip for missing input-monitoring is a violation", () => {
  const result = evaluateGate(
    summary({
      passed: [GATE_TEST_NAME],
      skipped: [{ name: "x", reason: "needs input-monitoring: accessibility not granted" }],
    }),
    { platform: "darwin" }
  );
  assert.equal(result.ok, false);
  assert.equal(result.violations.some((v) => v.includes("input-monitoring")), true);
});

test("gate: condition 2 - other skips are not gate violations by themselves", () => {
  const result = evaluateGate(
    summary({
      passed: [GATE_TEST_NAME],
      skipped: [{ name: "clipboard: round-trip", reason: "local tier: set SELECTION_HOOK_TEST_LOCAL=1" }],
    }),
    { platform: "darwin" }
  );
  assert.equal(result.ok, true);
});

test("gate: condition 3 - race test missing from passed list is a violation", () => {
  const result = evaluateGate(summary({ passed: ["other"] }), { platform: "darwin" });
  assert.equal(result.ok, false);
  assert.equal(result.violations.some((v) => v.includes(GATE_TEST_NAME)), true);
});

test("gate: condition 3 - a prefix-similar name does not satisfy the gate", () => {
  const result = evaluateGate(summary({ passed: [GATE_TEST_NAME + " extra"] }), {
    platform: "darwin",
  });
  assert.equal(result.ok, false);
});

test("gate: all violations are reported together", () => {
  const result = evaluateGate(
    summary({
      failed: ["f"],
      skipped: [{ name: "s", reason: "needs input-monitoring: x" }],
    }),
    { platform: "win32" }
  );
  assert.equal(result.violations.length, 4);
});

// ---------------------------------------------------------------------------
// checkSkipPolicy
// ---------------------------------------------------------------------------

test("skip-policy: unit tier with no skips -> ok", () => {
  assert.deepEqual(checkSkipPolicy(summary(), "unit"), { ok: true, violations: [] });
});

test("skip-policy: unit tier rejects any skip, even an allowlisted name", () => {
  const result = checkSkipPolicy(
    summary({ skipped: [{ name: GATE_TEST_NAME, reason: "needs input-monitoring: x" }] }),
    "unit"
  );
  assert.equal(result.ok, false);
  assert.equal(result.violations.length, 1);
});

test("skip-policy: integration accepts allowlisted name with allowed reason prefix", () => {
  const result = checkSkipPolicy(
    summary({
      skipped: [
        { name: GATE_TEST_NAME, reason: "needs input-monitoring: accessibility not granted" },
        { name: "clipboard: round-trip", reason: "local tier: set SELECTION_HOOK_TEST_LOCAL=1" },
      ],
    }),
    "integration"
  );
  assert.deepEqual(result, { ok: true, violations: [] });
});

test("skip-policy: integration rejects allowlisted name with wrong reason prefix", () => {
  const result = checkSkipPolicy(
    summary({ skipped: [{ name: GATE_TEST_NAME, reason: "local tier: x" }] }),
    "integration"
  );
  assert.equal(result.ok, false);
  assert.match(result.violations[0], /stop-race: input-monitoring/);
});

test("skip-policy: integration rejects allowed prefix on a name not in the allowlist", () => {
  const result = checkSkipPolicy(
    summary({ skipped: [{ name: "lifecycle: restart", reason: "needs input-monitoring: x" }] }),
    "integration"
  );
  assert.equal(result.ok, false);
  assert.match(result.violations[0], /lifecycle: restart/);
});

test("skip-policy: integration rejects a bare skip without reason", () => {
  const result = checkSkipPolicy(summary({ skipped: [{ name: GATE_TEST_NAME, reason: "" }] }), "integration");
  assert.equal(result.ok, false);
});

test("skip-policy: unknown tier is an error", () => {
  assert.throws(() => checkSkipPolicy(summary(), "nope"), /tier/);
});

test("skip-policy: allowlist covers the gate test name and the clipboard round-trip cases", () => {
  assert.deepEqual(Object.keys(SKIP_ALLOWLIST).sort(), [
    "clipboard: round-trip",
    "clipboard: round-trip unicode multiline",
    ...Object.values(WINDOWS_NATIVE_FIXTURES),
    GATE_TEST_NAME,
  ].sort());
  for (const prefixes of Object.values(SKIP_ALLOWLIST)) {
    assert.equal(Array.isArray(prefixes), true);
    assert.equal(prefixes.length > 0, true);
  }
});

for (const name of Object.values(WINDOWS_NATIVE_FIXTURES)) {
  test(`skip-policy: ${name} accepts only its non-Windows platform skip`, () => {
    const skipped = summary({ skipped: [{ name, reason: "needs windows-native-fixtures: Windows only" }] });
    for (const platform of ["darwin", "linux"]) {
      assert.equal(checkSkipPolicy(skipped, "unit", { platform }).ok, true);
    }
    assert.equal(checkSkipPolicy(skipped, "unit", { platform: "win32" }).ok, false);
    assert.equal(checkSkipPolicy(summary({ skipped: [{ name, reason: "local tier: optional" }] }),
      "unit", { platform: "linux" }).ok, false);
    assert.equal(checkSkipPolicy(summary({ skipped: [{ name, reason: "needs windows-native-fixtures: Windows only extra" }] }),
      "unit", { platform: "linux" }).ok, false);
    assert.equal(checkSkipPolicy(summary({ skipped: [{ name: name + " extra", reason: "needs windows-native-fixtures: Windows only" }] }),
      "unit", { platform: "linux" }).ok, false);
  });
}
