/**
 * Minimal TAP parser for the Node.js test runner output.
 *
 * Only the subset emitted by `node --test --test-reporter=tap` (Node 24) is
 * handled: `ok` / `not ok` lines at any indentation, optional `# SKIP reason`
 * directives, and backslash-escaped `#` inside test names. YAML diagnostic
 * blocks (`---` ... `...`) are ignored so that error text can never be
 * mistaken for a test line.
 *
 * Pure function, no I/O: covered by test/unit/tap-summary.test.js.
 */

const TEST_LINE = /^\s*(not ok|ok)\s+\d+(?:\s+-\s*(.*))?$/;
const YAML_START = /^\s*---\s*$/;
const YAML_END = /^\s*\.\.\.\s*$/;
// A directive is " # SKIP" / " # TODO" whose "#" is not escaped.
const DIRECTIVE = /\s#\s*(SKIP|TODO)\b\s*(.*)$/i;

function unescapeName(name) {
  return name.replace(/\\([#\\])/g, "$1");
}

/**
 * @param {string} text TAP output
 * @returns {{ passed: string[], failed: string[], skipped: { name: string, reason: string }[] }}
 */
function parseTap(text) {
  const summary = { passed: [], failed: [], skipped: [] };
  if (!text) return summary;

  let inYaml = false;
  for (const rawLine of text.split(/\r?\n/)) {
    if (inYaml) {
      if (YAML_END.test(rawLine)) inYaml = false;
      continue;
    }
    if (YAML_START.test(rawLine)) {
      inYaml = true;
      continue;
    }

    const match = TEST_LINE.exec(rawLine);
    if (!match) continue;

    const ok = match[1] === "ok";
    let rest = match[2] || "";
    let directive = null;
    let reason = "";

    // Find the first unescaped " # " that starts a directive.
    for (let i = 0; i < rest.length; i++) {
      if (rest[i] !== "#" || (i > 0 && rest[i - 1] === "\\")) continue;
      const candidate = DIRECTIVE.exec(rest.slice(i - 1));
      if (candidate) {
        directive = candidate[1].toUpperCase();
        reason = candidate[2].trim();
        rest = rest.slice(0, i - 1);
        break;
      }
    }

    const name = unescapeName(rest.trim());

    if (!ok) {
      summary.failed.push(name);
    } else if (directive === "SKIP") {
      summary.skipped.push({ name, reason });
    } else {
      summary.passed.push(name);
    }
  }

  return summary;
}

/**
 * Concatenate several summaries (e.g. unit + integration) into one.
 */
function mergeSummaries(...summaries) {
  return {
    passed: summaries.flatMap((s) => s.passed),
    failed: summaries.flatMap((s) => s.failed),
    skipped: summaries.flatMap((s) => s.skipped),
  };
}

module.exports = { parseTap, mergeSummaries };
