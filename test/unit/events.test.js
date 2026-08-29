/**
 * L1: native event objects are reshaped by index.js before being emitted.
 */

const test = require("node:test");
const assert = require("node:assert/strict");

const { loadIndexWith } = require("../helpers/fake-native.js");

function startedHook(options) {
  const { SelectionHook, natives } = loadIndexWith(options);
  const hook = new SelectionHook();
  hook.on("error", () => {});
  hook.start();
  return { SelectionHook, hook, native: natives[0] };
}

function collect(hook, event) {
  const received = [];
  hook.on(event, (data) => received.push(data));
  return received;
}

const rawSelection = {
  type: "text-selection",
  text: "hello",
  programName: "app",
  startTopX: 1,
  startTopY: 2,
  startBottomX: 3,
  startBottomY: 4,
  endTopX: 5,
  endTopY: 6,
  endBottomX: 7,
  endBottomY: 8,
  mouseStartX: 9,
  mouseStartY: 10,
  mouseEndX: 11,
  mouseEndY: 12,
};

test("events: text-selection is reshaped into point objects with defaults", () => {
  const { hook, native } = startedHook({ platform: "win32" });
  const received = collect(hook, "text-selection");
  native.push(rawSelection);
  assert.deepEqual(received, [
    {
      text: "hello",
      programName: "app",
      startTop: { x: 1, y: 2 },
      startBottom: { x: 3, y: 4 },
      endTop: { x: 5, y: 6 },
      endBottom: { x: 7, y: 8 },
      mousePosStart: { x: 9, y: 10 },
      mousePosEnd: { x: 11, y: 12 },
      method: 0,
      posLevel: 0,
    },
  ]);
});

test("events: text-selection keeps method and posLevel when present", () => {
  const { SelectionHook, hook, native } = startedHook({ platform: "win32" });
  const received = collect(hook, "text-selection");
  native.push({ ...rawSelection, method: SelectionHook.SelectionMethod.UIA, posLevel: 3 });
  assert.equal(received[0].method, SelectionHook.SelectionMethod.UIA);
  assert.equal(received[0].posLevel, 3);
});

test("events: text-selection carries isFullscreen only on darwin", () => {
  const mac = startedHook({ platform: "darwin" });
  const macReceived = collect(mac.hook, "text-selection");
  mac.native.push({ ...rawSelection, isFullscreen: true });
  assert.equal(macReceived[0].isFullscreen, true);

  const win = startedHook({ platform: "win32" });
  const winReceived = collect(win.hook, "text-selection");
  win.native.push({ ...rawSelection, isFullscreen: true });
  assert.equal("isFullscreen" in winReceived[0], false);
});

test("events: mouse-wheel includes flag, other mouse events do not", () => {
  const { hook, native } = startedHook();
  const wheel = collect(hook, "mouse-wheel");
  const down = collect(hook, "mouse-down");
  native.push({ type: "mouse-event", action: "mouse-wheel", x: 1, y: 2, button: 0, flag: 120, extra: 1 });
  native.push({ type: "mouse-event", action: "mouse-down", x: 3, y: 4, button: 1, flag: 5 });
  assert.deepEqual(wheel, [{ x: 1, y: 2, button: 0, flag: 120 }]);
  assert.deepEqual(down, [{ x: 3, y: 4, button: 1 }]);
});

test("events: keyboard scanCode is only attached when provided", () => {
  const { hook, native } = startedHook();
  const received = collect(hook, "key-down");
  native.push({ type: "keyboard-event", action: "key-down", uniKey: "a", vkCode: 65, sys: false, flags: 0, scanCode: 30 });
  native.push({ type: "keyboard-event", action: "key-down", uniKey: "b", vkCode: 66, sys: true, flags: 1 });
  assert.deepEqual(received[0], { uniKey: "a", vkCode: 65, sys: false, flags: 0, scanCode: 30 });
  assert.deepEqual(received[1], { uniKey: "b", vkCode: 66, sys: true, flags: 1 });
  assert.equal("scanCode" in received[1], false);
});

test("events: status and error are passed through", () => {
  const { hook, native } = startedHook();
  const statuses = collect(hook, "status");
  const errors = collect(hook, "error");
  native.push({ type: "status", status: "custom" });
  native.push({ type: "error", error: "native says no" });
  assert.deepEqual(statuses, ["custom"]);
  assert.equal(errors.length, 1);
  assert.ok(errors[0] instanceof Error);
  assert.equal(errors[0].message, "native says no");
});

test("events: nothing is emitted after stop()", () => {
  const { hook, native } = startedHook();
  const received = collect(hook, "text-selection");
  const callback = native.callback;
  hook.stop();
  callback(rawSelection);
  assert.deepEqual(received, []);
});

test("events: unknown type and null payload are ignored silently", () => {
  const { hook, native } = startedHook();
  const anything = [];
  for (const name of ["text-selection", "status", "error", "mouse-down", "key-down"]) {
    hook.on(name, () => anything.push(name));
  }
  assert.doesNotThrow(() => native.push({ type: "mystery" }));
  assert.doesNotThrow(() => native.push(null));
  assert.doesNotThrow(() => native.push({}));
  assert.deepEqual(anything, []);
});
