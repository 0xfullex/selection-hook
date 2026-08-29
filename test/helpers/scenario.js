/**
 * Scenario runner: executes test/integration/scenarios/<name>.js in a watchdog
 * child process and returns the JSON object the scenario printed as its last
 * stdout line.
 *
 * Only scenario scripts may call SelectionHook#start(); *.test.js files spawn
 * them through runScenario() and assert on the result.
 */

const path = require("node:path");
const { runInChildWithTimeout, tail } = require("./watchdog.js");

const SCENARIO_DIR = path.join(__dirname, "..", "integration", "scenarios");
const SENTINEL = "SELECTION_HOOK_SCENARIO";

/**
 * @param {string} name scenario file name without extension
 * @param {{ execArgv?: string[], timeoutMs?: number, env?: NodeJS.ProcessEnv }} [options]
 * @returns {Promise<{ result: any, timedOut: boolean, code: number|null, signal: string|null, stdout: string, stderr: string }>}
 */
async function runScenario(name, { execArgv = [], timeoutMs = 30000, env = {} } = {}) {
  const script = path.join(SCENARIO_DIR, `${name}.js`);
  const r = await runInChildWithTimeout({
    script,
    execArgv,
    timeoutMs,
    // Merge, never replace: DISPLAY / WAYLAND_DISPLAY / PATH / CI /
    // SELECTION_HOOK_TEST_LOCAL must reach the child.
    env: { ...process.env, ...env, [SENTINEL]: "1" },
  });

  let result = null;
  const lines = r.stdout.split(/\r?\n/).filter((l) => l.trim() !== "");
  const last = lines[lines.length - 1];
  if (last) {
    try {
      result = JSON.parse(last);
    } catch {
      result = null;
    }
  }

  return { result, ...r };
}

/**
 * Human-readable description of a scenario outcome for assertion messages.
 */
function describeScenario(name, r) {
  const status = r.timedOut ? "TIMED OUT (killed)" : `exit code ${r.code} signal ${r.signal}`;
  return [
    `scenario "${name}": ${status}`,
    "--- stdout (tail) ---",
    tail(r.stdout),
    "--- stderr (tail) ---",
    tail(r.stderr),
  ].join("\n");
}

module.exports = { runScenario, describeScenario, SENTINEL };
