/**
 * Static architecture regression for the macOS retrieval hardening.
 *
 * The pre-fix implementation in src/mac:
 * - entered the is_processing gate with a non-atomic load()/store(true) pair and
 *   cleared it with a bare store(false) at the end of the function, so an
 *   exception caught by GetCurrentSelection() left the gate locked and every
 *   later gesture or getCurrentSelection() call silently returned nothing;
 * - assigned running_pid on the event thread, so getCurrentSelection() before
 *   start() saw pid 0 and the same-process clipboard rejection did not fire;
 * - carried a write-only is_triggered_by_user member;
 * - released the AXUIElementCreateApplication result without a null check.
 *
 * These checks deliberately need no macOS desktop and fail against that
 * implementation.
 */

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const ROOT = path.resolve(__dirname, "..", "..");

function source(relativePath) {
  return fs.readFileSync(path.join(ROOT, relativePath), "utf8");
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex + start.length);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
}

test("mac: retrieval gate is entered atomically and released by an RAII guard", () => {
  const hook = source("src/mac/selection_hook.mm");
  const wrapper = between(
    hook,
    "bool SelectionHook::GetSelectedText",
    "SelectionConfigSnapshot SelectionHook::BuildSelectionConfigSnapshot"
  );

  assert.match(wrapper, /is_processing\.compare_exchange_strong\(expected, true/);
  assert.match(wrapper, /struct \w+Guard[\s\S]*?~\w+Guard\(\)[\s\S]*?store\(false\)/);
  // The gate must not be touched directly once the guard owns it.
  assert.doesNotMatch(wrapper, /is_processing\.load\(\)/);
  assert.doesNotMatch(wrapper, /is_processing\.store\(/);
});

test("mac: running_pid is fixed at construction, not assigned on the event thread", () => {
  const hook = source("src/mac/selection_hook.mm");
  assert.match(hook, /const pid_t running_pid = getpid\(\);/);

  const threadProc = between(
    hook,
    "void SelectionHook::MouseKeyboardEventThreadProc",
    "void SelectionHook::ProcessMouseEvent"
  );
  assert.doesNotMatch(threadProc, /running_pid/);
});

test("mac: write-only is_triggered_by_user state is gone", () => {
  const hook = source("src/mac/selection_hook.mm");
  assert.doesNotMatch(hook, /is_triggered_by_user/);
});

test("mac: AX application element is null-checked before use", () => {
  const engine = source("src/mac/core/engine.mm");
  const axapi = between(engine, "bool SelectionEngine::GetTextViaAXAPI", "bool SelectionEngine::GetTextViaClipboard");
  assert.match(axapi, /GetAppElementFromFrontApp\(frontApp\);\s*if \(!appElement\)/);

  const utils = source("src/mac/lib/utils.mm");
  const fullscreen = utils.slice(utils.indexOf("bool IsWindowFullscreen"));
  assert.match(fullscreen, /GetAppElementFromFrontApp\(frontApp\);\s*if \(!appElement\)/);
});
