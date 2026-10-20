/**
 * Static architecture regression: the Windows clipboard fallback must refuse to
 * synthesize a copy shortcut for a window created by the calling thread.
 *
 * Pre-fix behavior these checks guard against: GetTextViaClipboard() called
 * SendCopyKey() with no thread check. Windows posts synthesized input to the message
 * queue of the thread that created the destination window, so when getCurrentSelection()
 * ran on the thread owning that window, the Sleep() poll loops held the only thread that
 * could dispatch the keystroke. The retrieval blocked for the full Ctrl+Insert (100 ms)
 * plus Ctrl+C (180 ms) budget and always returned null, and the still-queued keys reached
 * the window after restoreBackup() had already restored the user's clipboard, so the
 * application performed a real copy that silently overwrote it.
 *
 * The guard is thread-scoped rather than process-scoped on purpose: automatic retrieval
 * runs on the MTA worker, which never owns the target window, and must keep working for
 * same-process selections.
 *
 * These checks deliberately need no Windows desktop and would fail against the pre-fix
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

function clipboardFallback() {
  return between(
    source("src/windows/core/engine.cc"),
    "bool SelectionEngine::GetTextViaClipboard",
    "bool SelectionEngine::SendCopyKey"
  );
}

test("windows: the calling-thread check is declared and compares window thread affinity", () => {
  const header = source("src/windows/lib/utils.h");
  assert.match(header, /bool IsWindowOnCallingThread\(HWND hwnd\);/);

  const body = between(source("src/windows/lib/utils.cc"), "bool IsWindowOnCallingThread", "\n}");
  assert.match(body, /GetWindowThreadProcessId\(hwnd, nullptr\)/);
  assert.match(body, /GetCurrentThreadId\(\)/);
});

test("windows: the clipboard fallback rejects a window owned by the calling thread", () => {
  const fallback = clipboardFallback();
  assert.match(
    fallback,
    /if \(IsWindowOnCallingThread\(hwnd\)\)\s*\n\s*return false;/,
    "GetTextViaClipboard() must return early for a window owned by the calling thread"
  );
});

test("windows: the guard runs before the clipboard transaction and any synthesized copy", () => {
  const fallback = clipboardFallback();
  const guard = fallback.indexOf("IsWindowOnCallingThread(hwnd)");
  const lock = fallback.indexOf("clipboardLock");
  const backup = fallback.indexOf("BackupClipboard(");
  const copyKey = fallback.indexOf("SendCopyKey(");

  assert.ok(guard >= 0, "missing calling-thread guard");
  assert.ok(lock > guard, "the guard must run before the clipboard mutex is taken");
  assert.ok(backup > guard, "the guard must run before the user's clipboard is backed up");
  assert.ok(copyKey > guard, "the guard must run before any copy shortcut is synthesized");
});

test("windows: the guard stays thread-scoped and unconditional", () => {
  const fallback = clipboardFallback();
  const guardLine = between(fallback, "if (IsWindowOnCallingThread(hwnd))", ";");

  assert.doesNotMatch(
    guardLine,
    /triggeredByUser/,
    "the guard must not be limited to getCurrentSelection(); it describes the calling thread"
  );

  const utils = between(source("src/windows/lib/utils.cc"), "bool IsWindowOnCallingThread", "\n}");
  assert.doesNotMatch(
    utils,
    /GetCurrentProcessId/,
    "a process-scoped check would also reject the MTA worker, which retrieves same-process selections safely"
  );
});
