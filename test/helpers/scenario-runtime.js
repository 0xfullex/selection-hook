/**
 * Runtime support for scenario scripts (test/integration/scenarios/*.js).
 *
 * A scenario calls runScenarioBody(async (SelectionHook) => data). The
 * resolved data is printed as a single JSON line ({ ok: true, ...data }) and
 * the process exits 0; any thrown error becomes { ok: false, error } with
 * exit code 1. stdout is flushed before exiting so the last line survives on
 * Windows, where pipe writes are asynchronous.
 *
 * Scenario scripts must check the SELECTION_HOOK_SCENARIO sentinel as their
 * very first statement, before requiring this file.
 */

const path = require("node:path");

function finish(payload, code) {
  process.stdout.write(JSON.stringify(payload) + "\n", () => process.exit(code));
}

async function runScenarioBody(body) {
  try {
    const SelectionHook = require(path.join(__dirname, "..", "..", "index.js"));
    const data = (await body(SelectionHook)) || {};
    finish({ ok: true, ...data }, 0);
  } catch (err) {
    finish({ ok: false, error: err && err.stack ? err.stack : String(err) }, 1);
  }
}

module.exports = { runScenarioBody };
