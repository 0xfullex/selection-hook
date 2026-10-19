/**
 * Release-gate and skip-policy decisions.
 *
 * Both functions are pure (summary in, verdict out) so that the logic which
 * decides whether a run is green is itself covered by test/unit/gate.test.js.
 *
 * Tiers:
 *   gate  - must run everywhere, may never skip
 *   env   - runs where the capability exists, skips with "needs <cap>: ..."
 *   local - runs in CI and on opt-in dev machines, skips with "local tier: ..."
 */

/**
 * Exact name of the issue #23 regression test. The release gate requires this
 * name in the passed list, so it must never be renamed.
 */
const GATE_TEST_NAME = "stop-race: input-monitoring";

/**
 * Integration tests that are allowed to skip, keyed by exact test name, with
 * the reason prefixes each may use. Anything not listed here is gate tier and
 * must not skip. Add an entry when adding an env / local tier test.
 */
const SKIP_ALLOWLIST = {
  [GATE_TEST_NAME]: ["needs input-monitoring:"],
  "windows clipboard native: restore": ["needs windows-native-fixtures: Windows only"],
  "windows clipboard native: activity": ["needs windows-native-fixtures: Windows only"],
  "clipboard: round-trip": ["local tier:", "needs clipboard:"],
  "clipboard: round-trip unicode multiline": ["local tier:", "needs clipboard:"],
};

const INPUT_MONITORING_SKIP = "needs input-monitoring";

/**
 * Release gate (npm run test:local), evaluated on the merged unit + integration summary.
 *
 * Conditions:
 *   0. platform is darwin (the race in issue #23 only exists on macOS)
 *   1. no failed tests
 *   2. no test skipped for missing input-monitoring
 *   3. GATE_TEST_NAME is in the passed list (positive proof the race ran)
 *
 * @returns {{ ok: boolean, violations: string[] }}
 */
function evaluateGate(summary, { platform }) {
  const violations = [];

  if (platform !== "darwin") {
    violations.push(`condition 0: release gate must run on darwin, got ${platform}`);
  }

  for (const name of summary.failed) {
    violations.push(`condition 1: failed test: ${name}`);
  }

  for (const { name, reason } of summary.skipped) {
    if (reason.startsWith(INPUT_MONITORING_SKIP)) {
      violations.push(`condition 2: skipped for missing input-monitoring: ${name} (${reason})`);
    }
  }

  if (!summary.passed.includes(GATE_TEST_NAME)) {
    violations.push(`condition 3: "${GATE_TEST_NAME}" is not in the passed list`);
  }

  return { ok: violations.length === 0, violations };
}

/**
 * Skip policy applied by scripts/run-tests.js after every unit / integration run.
 *
 * @param {"unit"|"integration"} tier
 * @returns {{ ok: boolean, violations: string[] }}
 */
function checkSkipPolicy(summary, tier, { platform = process.platform } = {}) {
  if (tier !== "unit" && tier !== "integration") {
    throw new Error(`unknown tier: ${tier}`);
  }

  const violations = [];

  for (const { name, reason } of summary.skipped) {
    // These two executable fixtures require Win32 and a Windows compiler. No other
    // unit skip, and no Windows fixture skip on Windows, is permitted.
    const windowsFixture = name === "windows clipboard native: restore" || name === "windows clipboard native: activity";
    const allowedUnitPlatformSkip = platform !== "win32" && windowsFixture &&
      reason === "needs windows-native-fixtures: Windows only";
    if (tier === "unit" && !allowedUnitPlatformSkip) {
      violations.push(`unit tests must not skip: ${name} (${reason || "no reason"})`);
      continue;
    }

    const prefixes = SKIP_ALLOWLIST[name];
    if (!prefixes) {
      violations.push(`skip not allowed for gate-tier test: ${name} (${reason || "no reason"})`);
    } else if (!prefixes.some((p) => reason.startsWith(p))) {
      violations.push(
        `skip reason for "${name}" must start with one of [${prefixes.join(", ")}], got: ${reason || "no reason"}`
      );
    }
  }

  return { ok: violations.length === 0, violations };
}

module.exports = { GATE_TEST_NAME, SKIP_ALLOWLIST, evaluateGate, checkSkipPolicy };
