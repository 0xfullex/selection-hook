/**
 * Clipboard API.
 *
 * "clipboard: platform contract" is gate tier: read-only on Windows / macOS,
 * false / null contract on Linux (index.js short-circuits, native untouched).
 *
 * The round-trip tests write to the real clipboard, so they are local tier
 * (CI always, dev machines opt-in with SELECTION_HOOK_TEST_LOCAL=1) and skip
 * on Linux. Their names are registered in SKIP_ALLOWLIST (test/lib/gate.js)
 * and must not be renamed. Only plain-text content is restored afterwards.
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { requires, localTier, isLinux } = require("../helpers/capabilities.js");
const { withHook } = require("../helpers/native.js");

function roundTrip(t, text) {
  withHook((hook, errors) => {
    const original = hook.readFromClipboard();
    try {
      assert.equal(hook.writeToClipboard(text), true, "writeToClipboard returned false");
      assert.equal(hook.readFromClipboard(), text);
    } finally {
      if (typeof original === "string") {
        hook.writeToClipboard(original);
      } else {
        t.diagnostic("clipboard held non-text content before the test; it was not restored");
      }
    }
    assert.deepEqual(errors, []);
  });
}

test("clipboard: platform contract", (t) => {
  if (!requires(t, "native")) return;
  withHook((hook, errors) => {
    if (isLinux) {
      assert.equal(hook.writeToClipboard("selection-hook"), false);
      assert.equal(hook.readFromClipboard(), null);
    } else {
      const value = hook.readFromClipboard();
      assert.ok(value === null || typeof value === "string", `readFromClipboard: ${typeof value}`);
    }
    assert.deepEqual(errors, []);
  });
});

test("clipboard: round-trip", (t) => {
  if (!requires(t, "native", "clipboard")) return;
  if (!localTier(t)) return;
  roundTrip(t, "selection-hook clipboard round-trip");
});

test("clipboard: round-trip unicode multiline", (t) => {
  if (!requires(t, "native", "clipboard")) return;
  if (!localTier(t)) return;
  roundTrip(t, "line one\nline two\n\u4e2d\u6587 \u00e9\u00e8 \u{1F600} tab\there");
});
