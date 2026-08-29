/**
 * Gate: every side-effect-free public API returns the documented type, both
 * before start() (parent process) and while running (scenario child).
 *
 * Excluded on purpose: writeToClipboard (side effect, see clipboard.test.js),
 * macRequestProcessTrust (may show a permission dialog), start / stop
 * (covered by lifecycle.test.js).
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { requires, isMac, isLinux } = require("../helpers/capabilities.js");
const { loadSelectionHook, withHook } = require("../helpers/native.js");
const { runScenario, describeScenario } = require("../helpers/scenario.js");

test("api-smoke: not started", (t) => {
  if (!requires(t, "native")) return;
  const SelectionHook = loadSelectionHook();
  const F = SelectionHook.FilterMode;
  const L = SelectionHook.FineTunedListType;

  withHook((hook, errors) => {
    assert.equal(hook.isRunning(), false);
    assert.equal(hook.enableMouseMoveEvent(), true);
    assert.equal(hook.disableMouseMoveEvent(), true);
    assert.equal(hook.enableClipboard(), true);
    assert.equal(hook.disableClipboard(), true);
    assert.equal(hook.setClipboardMode(F.DEFAULT, []), true);
    assert.equal(hook.setClipboardMode(F.INCLUDE_LIST, ["a.exe"]), true);
    assert.equal(hook.setGlobalFilterMode(F.EXCLUDE_LIST, ["b.exe"]), true);
    assert.equal(hook.setFineTunedList(L.EXCLUDE_CLIPBOARD_CURSOR_DETECT, ["c.exe"]), true);
    assert.equal(hook.setSelectionPassiveMode(true), true);
    assert.equal(hook.setSelectionPassiveMode(false), true);

    // not running -> null without touching native
    assert.equal(hook.getCurrentSelection(), null);

    const clipboard = hook.readFromClipboard();
    if (isLinux) assert.equal(clipboard, null);
    else assert.ok(clipboard === null || typeof clipboard === "string");

    assert.equal(typeof hook.macIsProcessTrusted(), "boolean");
    if (!isMac) assert.equal(hook.macIsProcessTrusted(), false);

    const envInfo = hook.linuxGetEnvInfo();
    if (isLinux) {
      assert.equal(typeof envInfo, "object");
      assert.notEqual(envInfo, null);
      t.diagnostic(`linuxGetEnvInfo: ${JSON.stringify(envInfo)}`);
    } else {
      assert.equal(envInfo, null);
    }

    assert.deepEqual(errors, []);
  });
});

test("api-smoke: started", async (t) => {
  if (!requires(t, "native")) return;
  const r = await runScenario("api-smoke-started");
  assert.equal(r.timedOut, false, describeScenario("api-smoke-started", r));
  assert.equal(r.code, 0, describeScenario("api-smoke-started", r));
  assert.equal(r.result && r.result.ok, true, describeScenario("api-smoke-started", r));

  for (const [name, value] of Object.entries(r.result.calls)) {
    assert.equal(value, true, `${name}() returned ${value} while running`);
  }
  assert.ok(["null", "object"].includes(r.result.selectionType), `getCurrentSelection: ${r.result.selectionType}`);
  if (r.result.selectionType === "object") assert.equal(r.result.selectionTextType, "string");
  if (isLinux) assert.equal(r.result.clipboardType, "null");
  else assert.ok(["null", "string"].includes(r.result.clipboardType));
  assert.equal(r.result.stopped, true);
});
