/**
 * Static architecture regression for macOS mid-drag cursor sampling (PR #21).
 *
 * The pre-fix implementation in src/mac/selection_hook.mm sampled the cursor
 * shape only on left button down and up. The front application sets the cursor
 * asynchronously after it processes its own mouse events, and the listen-only
 * tap samples before the application has seen the event, so a drag that
 * pressed within the cursor-update latency of entering the text, or that
 * started and ended in the margin next to a paragraph, saw an arrow at both
 * endpoints and was never treated as a selection although it crossed text.
 *
 * The fix samples the cursor during kCGEventLeftMouseDragged on the event
 * thread, throttled and stopping at the first I-beam, and carries the result in
 * the mouse-up snapshot as extra evidence for the drag branch only.
 *
 * These checks deliberately need no macOS desktop and fail against the
 * pre-fix implementation.
 */

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const ROOT = path.resolve(__dirname, "..", "..");
const HOOK = fs.readFileSync(path.join(ROOT, "src", "mac", "selection_hook.mm"), "utf8");

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex + start.length);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
}

test("mac: mouse-up snapshot carries mid-drag cursor evidence", () => {
  const ctx = between(HOOK, "struct MouseEventContext", "struct KeyboardEventContext");
  assert.match(ctx, /bool dragSawIBeam;/);
});

test("mac: drag events are sampled on the event thread, throttled, stopping at the first I-beam", () => {
  const cb = between(
    HOOK,
    "CGEventRef SelectionHook::MouseEventCallback",
    "CGEventRef SelectionHook::KeyboardEventCallback"
  );

  // Sampling is gated on: dragged event, no hit yet, interval elapsed.
  assert.match(cb, /type == kCGEventLeftMouseDragged && !hook->drag_saw_ibeam &&[\s\S]*?DRAG_CURSOR_SAMPLE_INTERVAL_MS/);
  const dragged = between(cb, "type == kCGEventLeftMouseDragged && !hook->drag_saw_ibeam", "MouseEventContext *mouseEventCtx");
  assert.match(dragged, /hook->drag_saw_ibeam = IsIBeamCursor\(\[NSCursor currentSystemCursor\]\);/);
  // Without advancing the sample time every dragged event after the first interval would sample.
  assert.match(dragged, /hook->last_drag_cursor_sample_ms = timestampMs;/);
  // A new gesture starts with no evidence: the reset lives in the mouse-down branch itself.
  const down = between(cb, "if (type == kCGEventLeftMouseDown)", "else");
  assert.match(down, /hook->drag_saw_ibeam = false;/);
  // The mouse-up snapshot carries the evidence to the JS thread and clears it, so a
  // gesture whose mouse-down was never seen cannot inherit it.
  const up = between(cb, "dragSawIBeam = hook->drag_saw_ibeam;", "else if (type == kCGEventLeftMouseDragged");
  assert.match(up, /hook->drag_saw_ibeam = false;/);
  assert.match(cb, /\.dragSawIBeam = dragSawIBeam/);

  // The throttle bounds the per-second cost (~0.1 ms per sample).
  const interval = HOOK.match(/constexpr uint64_t DRAG_CURSOR_SAMPLE_INTERVAL_MS = (\d+);/);
  assert.ok(interval, "DRAG_CURSOR_SAMPLE_INTERVAL_MS must be a constexpr uint64_t");
  assert.ok(Number(interval[1]) >= 20, `sample interval ${interval[1]} ms would allow more than 50 samples/s`);
});

test("mac: only the drag branch consults mid-drag evidence", () => {
  const proc = between(HOOK, "void SelectionHook::ProcessMouseEvent", "void SelectionHook::ProcessKeyboardEvent");

  const drag = between(proc, "// Check for drag selection", "// Check for double-click selection");
  assert.match(drag, /if \(isValidDragCursor\)/);

  // Double-click and shift-click are stationary gestures and keep the endpoint-only rule.
  const rest = proc.slice(proc.indexOf("// Check for double-click selection"));
  assert.doesNotMatch(rest, /dragSawIBeam|isValidDragCursor/);

  // The JS thread never reads event-thread-owned state.
  assert.doesNotMatch(proc, /drag_saw_ibeam|last_drag_cursor_sample_ms/);
});

test("mac: mid-drag state is reset by start() before the event thread exists", () => {
  const start = between(HOOK, "void SelectionHook::Start(", "void SelectionHook::Stop(");
  assert.match(start, /drag_saw_ibeam = false;/);
  assert.match(start, /last_drag_cursor_sample_ms = 0;/);
  // Reset must happen before the thread is started.
  assert.ok(
    start.indexOf("drag_saw_ibeam = false;") < start.indexOf("StartMouseKeyboardEventThread();"),
    "gesture state must be reset before StartMouseKeyboardEventThread()"
  );
});
