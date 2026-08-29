/**
 * Unit tests for test/lib/tap-summary.js
 *
 * parseTap() decides what the skip policy and the release gate see, so its
 * behaviour is pinned here against TAP text in the exact shape produced by
 * the Node 24 `--test-reporter=tap` reporter.
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { parseTap, mergeSummaries } = require("../lib/tap-summary.js");

test("tap-summary: empty text yields empty lists", () => {
  assert.deepEqual(parseTap(""), { passed: [], failed: [], skipped: [] });
});

test("tap-summary: passed tests are listed by name", () => {
  const tap = [
    "TAP version 13",
    "# Subtest: alpha",
    "ok 1 - alpha",
    "  ---",
    "  duration_ms: 0.5",
    "  type: 'test'",
    "  ...",
    "# Subtest: beta",
    "ok 2 - beta",
    "1..2",
  ].join("\n");
  assert.deepEqual(parseTap(tap).passed, ["alpha", "beta"]);
});

test("tap-summary: failed tests are listed by name", () => {
  const tap = ["ok 1 - fine", "not ok 2 - broken", "  ---", "  error: 'x'", "  ..."].join("\n");
  const summary = parseTap(tap);
  assert.deepEqual(summary.passed, ["fine"]);
  assert.deepEqual(summary.failed, ["broken"]);
});

test("tap-summary: skipped tests keep their reason", () => {
  const tap = "ok 1 - cap # SKIP needs input-monitoring: accessibility not granted";
  assert.deepEqual(parseTap(tap), {
    passed: [],
    failed: [],
    skipped: [{ name: "cap", reason: "needs input-monitoring: accessibility not granted" }],
  });
});

test("tap-summary: skip without reason yields empty reason", () => {
  const summary = parseTap("ok 1 - bare # SKIP");
  assert.deepEqual(summary.skipped, [{ name: "bare", reason: "" }]);
});

test("tap-summary: nested subtests are parsed at any indentation", () => {
  const tap = [
    "# Subtest: parent",
    "    # Subtest: child pass",
    "    ok 1 - child pass",
    "    # Subtest: child skip",
    "    ok 2 - child skip # SKIP local tier: set SELECTION_HOOK_TEST_LOCAL=1",
    "    # Subtest: child fail",
    "    not ok 3 - child fail",
    "    1..3",
    "not ok 1 - parent",
  ].join("\n");
  const summary = parseTap(tap);
  assert.deepEqual(summary.passed, ["child pass"]);
  assert.deepEqual(summary.failed, ["child fail", "parent"]);
  assert.deepEqual(summary.skipped, [
    { name: "child skip", reason: "local tier: set SELECTION_HOOK_TEST_LOCAL=1" },
  ]);
});

test("tap-summary: escaped hash in names is unescaped and not treated as a directive", () => {
  const summary = parseTap("ok 1 - issue \\#23 regression");
  assert.deepEqual(summary.passed, ["issue #23 regression"]);
  assert.deepEqual(summary.skipped, []);
});

test("tap-summary: diagnostics and yaml blocks are ignored", () => {
  const tap = [
    "ok 1 - real",
    "  ---",
    "  error: 'not ok 9 - fake inside yaml'",
    "  stack: |-",
    "    ok 7 - also fake",
    "  ...",
    "# tests 1",
    "# pass 1",
  ].join("\n");
  assert.deepEqual(parseTap(tap), { passed: ["real"], failed: [], skipped: [] });
});

test("tap-summary: gate name is matched exactly, not by prefix", () => {
  const tap = ["ok 1 - stop-race: input-monitoring extra", "ok 2 - stop-race: input-monitoring"].join(
    "\n"
  );
  const summary = parseTap(tap);
  assert.equal(summary.passed.includes("stop-race: input-monitoring"), true);
  assert.equal(summary.passed.filter((n) => n === "stop-race: input-monitoring").length, 1);
});

test("tap-summary: crashed test file appears as a failure", () => {
  const tap = ["# Subtest: second.test.js", "not ok 2 - second.test.js", "  exitCode: 3"].join("\n");
  assert.deepEqual(parseTap(tap).failed, ["second.test.js"]);
});

test("tap-summary: mergeSummaries concatenates all three lists", () => {
  const a = { passed: ["a"], failed: [], skipped: [{ name: "s", reason: "r" }] };
  const b = { passed: ["b"], failed: ["f"], skipped: [] };
  assert.deepEqual(mergeSummaries(a, b), {
    passed: ["a", "b"],
    failed: ["f"],
    skipped: [{ name: "s", reason: "r" }],
  });
});
