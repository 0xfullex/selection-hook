/**
 * Gate: the native .node for this platform loads and a native instance can be
 * constructed. This test never skips; a failure here means the build is broken.
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { isMac } = require("../helpers/capabilities.js");
const { loadSelectionHook, getLoadError } = require("../helpers/native.js");

test("native-load: index.js loads and a native instance can be constructed", (t) => {
  t.diagnostic(
    `node ${process.versions.node}, napi ${process.versions.napi}, ${process.platform}/${process.arch}`
  );

  const SelectionHook = loadSelectionHook();
  assert.ok(SelectionHook, `index.js failed to load: ${getLoadError() && getLoadError().message}`);

  const hook = new SelectionHook();
  assert.equal(hook.isRunning(), false);

  if (isMac) {
    const trusted = hook.macIsProcessTrusted();
    assert.equal(typeof trusted, "boolean");
    t.diagnostic(`macIsProcessTrusted: ${trusted}`);
  }

  hook.cleanup();
});
