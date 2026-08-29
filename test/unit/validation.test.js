/**
 * L1: argument validation in index.js. Invalid arguments must return false
 * without reaching the native layer; valid ones are passed through unchanged.
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { loadIndexWith } = require("../helpers/fake-native.js");

function hookWith(options) {
  const { SelectionHook, natives } = loadIndexWith(options);
  const hook = new SelectionHook();
  hook.on("error", () => {});
  return { SelectionHook, hook, native: natives[0] };
}

test("validation: setClipboardMode rejects an unknown mode", () => {
  const { hook, native } = hookWith();
  assert.equal(hook.setClipboardMode(99), false);
  assert.deepEqual(native.callsTo("setClipboardMode"), []);
});

test("validation: setGlobalFilterMode rejects a non-numeric mode", () => {
  const { hook, native } = hookWith();
  assert.equal(hook.setGlobalFilterMode("x"), false);
  assert.deepEqual(native.callsTo("setGlobalFilterMode"), []);
});

test("validation: setFineTunedList rejects an unknown list type", () => {
  const { hook, native } = hookWith();
  assert.equal(hook.setFineTunedList(-1), false);
  assert.deepEqual(native.callsTo("setFineTunedList"), []);
});

test("validation: program list must be an array", () => {
  const { SelectionHook, hook, native } = hookWith();
  const F = SelectionHook.FilterMode;
  const L = SelectionHook.FineTunedListType;
  assert.equal(hook.setClipboardMode(F.DEFAULT, "a.exe"), false);
  assert.equal(hook.setGlobalFilterMode(F.DEFAULT, "a.exe"), false);
  assert.equal(hook.setFineTunedList(L.EXCLUDE_CLIPBOARD_CURSOR_DETECT, "a.exe"), false);
  assert.deepEqual(native.calls, []);
});

test("validation: writeToClipboard rejects a non-string", () => {
  const { hook, native } = hookWith({ platform: "win32" });
  assert.equal(hook.writeToClipboard(123), false);
  assert.deepEqual(native.callsTo("writeToClipboard"), []);
});

test("validation: valid arguments are passed through unchanged", () => {
  const { SelectionHook, hook, native } = hookWith({ platform: "win32" });
  const F = SelectionHook.FilterMode;
  const L = SelectionHook.FineTunedListType;

  assert.equal(hook.setClipboardMode(F.INCLUDE_LIST, ["a.exe"]), true);
  assert.equal(hook.setGlobalFilterMode(F.EXCLUDE_LIST, ["b.exe"]), true);
  assert.equal(hook.setFineTunedList(L.INCLUDE_CLIPBOARD_DELAY_READ, ["c.exe"]), true);
  assert.equal(hook.setSelectionPassiveMode(true), true);
  assert.equal(hook.writeToClipboard("text"), true);

  assert.deepEqual(native.calls, [
    { method: "setClipboardMode", args: [F.INCLUDE_LIST, ["a.exe"]] },
    { method: "setGlobalFilterMode", args: [F.EXCLUDE_LIST, ["b.exe"]] },
    { method: "setFineTunedList", args: [L.INCLUDE_CLIPBOARD_DELAY_READ, ["c.exe"]] },
    { method: "setSelectionPassiveMode", args: [true] },
    { method: "writeToClipboard", args: ["text"] },
  ]);
});

test("validation: program list defaults to an empty array", () => {
  const { SelectionHook, hook, native } = hookWith();
  assert.equal(hook.setClipboardMode(SelectionHook.FilterMode.DEFAULT), true);
  assert.deepEqual(native.callsTo("setClipboardMode")[0].args, [SelectionHook.FilterMode.DEFAULT, []]);
});

test("validation: clipboard is short-circuited on linux without touching native", () => {
  const { hook, native } = hookWith({ platform: "linux" });
  assert.equal(hook.writeToClipboard("text"), false);
  assert.equal(hook.readFromClipboard(), null);
  assert.deepEqual(native.calls, []);
});

test("validation: platform-only methods return their neutral value elsewhere", () => {
  const { hook, native } = hookWith({ platform: "win32" });
  assert.equal(hook.macIsProcessTrusted(), false);
  assert.equal(hook.macRequestProcessTrust(), false);
  assert.equal(hook.linuxGetEnvInfo(), null);
  assert.deepEqual(native.calls, []);
});
