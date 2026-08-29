/**
 * L1: start(config) dispatch in index.js.
 *
 * Contract pinned here (not changed on purpose): only keys that differ from
 * the defaults produce native calls; values are passed through unvalidated;
 * a native error during configuration propagates out of start().
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { loadIndexWith } = require("../helpers/fake-native.js");

function hookWith(options) {
  const { SelectionHook, natives } = loadIndexWith(options);
  const hook = new SelectionHook();
  const errors = [];
  hook.on("error", (err) => errors.push(err));
  return { SelectionHook, hook, native: natives[0], errors };
}

test("config: start() without config only calls native start", () => {
  const { hook, native } = hookWith();
  assert.equal(hook.start(), true);
  assert.deepEqual(native.methods(), ["start"]);
  assert.equal(hook.isRunning(), true);
});

test("config: enableClipboard false calls disableClipboard before start", () => {
  const { hook, native } = hookWith();
  hook.start({ enableClipboard: false });
  assert.deepEqual(native.methods(), ["disableClipboard", "start"]);
});

test("config: values equal to the defaults produce no native calls", () => {
  const { SelectionHook, hook, native } = hookWith();
  hook.start({
    debug: false,
    enableMouseMoveEvent: false,
    enableClipboard: true,
    selectionPassiveMode: false,
    clipboardMode: SelectionHook.FilterMode.DEFAULT,
    globalFilterMode: SelectionHook.FilterMode.DEFAULT,
  });
  assert.deepEqual(native.methods(), ["start"]);
});

test("config: clipboardFilterList alone uses the default mode", () => {
  const { SelectionHook, hook, native } = hookWith();
  hook.start({ clipboardFilterList: ["a.exe"] });
  assert.deepEqual(native.calls[0], {
    method: "setClipboardMode",
    args: [SelectionHook.FilterMode.DEFAULT, ["a.exe"]],
  });
});

test("config: globalFilterMode alone uses the default (empty) list", () => {
  const { SelectionHook, hook, native } = hookWith();
  hook.start({ globalFilterMode: SelectionHook.FilterMode.EXCLUDE_LIST });
  assert.deepEqual(native.calls[0], {
    method: "setGlobalFilterMode",
    args: [SelectionHook.FilterMode.EXCLUDE_LIST, []],
  });
});

test("config: mouse move and passive mode are dispatched", () => {
  const { hook, native } = hookWith();
  hook.start({ enableMouseMoveEvent: true, selectionPassiveMode: true });
  assert.deepEqual(native.calls.slice(0, 2), [
    { method: "enableMouseMoveEvent", args: [] },
    { method: "setSelectionPassiveMode", args: [true] },
  ]);
});

test("config: contract - invalid values are passed through unvalidated", () => {
  const { hook, native } = hookWith();
  assert.equal(hook.start({ clipboardMode: 99 }), true);
  assert.deepEqual(native.callsTo("setClipboardMode")[0].args, [99, []]);
});

test("config: contract - native error during configuration propagates out of start()", () => {
  const { hook, native } = hookWith();
  native.throwOn("setClipboardMode", new Error("bad mode"));
  assert.throws(() => hook.start({ clipboardMode: 2 }), /bad mode/);
  assert.equal(hook.isRunning(), false);
  assert.deepEqual(native.callsTo("start"), []);
});

test("config: debug true enables error events for non-fatal failures", (t) => {
  // debug mode logs the non-fatal error via console.error; silence it, nothing asserts on it
  t.mock.method(console, "error", () => {});
  const { hook, errors } = hookWith();
  hook.start({ debug: true });
  hook.setClipboardMode(99);
  assert.equal(errors.length, 1);
  assert.match(errors[0].message, /Invalid clipboard mode/);
});

test("config: debug false (default) suppresses non-fatal error events", () => {
  const { hook, errors } = hookWith();
  hook.start();
  hook.setClipboardMode(99);
  assert.deepEqual(errors, []);
});
