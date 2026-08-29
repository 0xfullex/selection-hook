/**
 * Static architecture regression for issue #17.
 *
 * The pre-fix implementation called GetSelectedText() directly inside
 * ProcessMouseEvent(), which runs on the Node/Electron JavaScript thread. These
 * checks deliberately need no Windows desktop and would fail against that
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

test("issue #17: automatic Windows retrieval is dispatched to an MTA worker", () => {
  const hook = source("src/windows/selection_hook.cc");
  const mouseCallback = between(
    hook,
    "void SelectionHook::ProcessMouseEvent",
    "void SelectionHook::ProcessKeyboardEvent"
  );

  assert.match(mouseCallback, /EnqueueAutomaticSelection\(hwnd, detectionType\)/);
  assert.doesNotMatch(mouseCallback, /GetSelectedText\s*\(/);

  const worker = source("src/windows/core/worker.cc");
  assert.match(worker, /CoInitializeEx\(nullptr, COINIT_MULTITHREADED\)/);
  assert.match(worker, /__uuidof\(CUIAutomation\)/);
});

test("issue #17: fallback order and public synchronous API stay unchanged", () => {
  const engine = source("src/windows/core/engine.cc");
  const retrieval = between(
    engine,
    "bool SelectionEngine::GetSelectedText",
    "bool SelectionEngine::IsInFilterList"
  );
  const uia = retrieval.indexOf("GetTextViaUIAutomation");
  const accessible = retrieval.indexOf("GetTextViaAccessible");
  const clipboard = retrieval.indexOf("GetTextViaClipboard");
  assert.ok(uia >= 0 && accessible > uia && clipboard > accessible, "fallback order changed");

  const types = source("index.d.ts");
  assert.match(types, /getCurrentSelection\(\):\s*TextSelectionData\s*\|\s*null/);
});

test("issue #17: abort cleanup and retrieval gates do not depend on a JS callback", () => {
  const hook = source("src/windows/selection_hook.cc");
  assert.match(hook, /napi_create_threadsafe_function/);
  assert.match(hook, /if \(!result \|\| !env \|\| !jsCallback \|\| !context\)/);

  const worker = source("src/windows/core/worker.cc");
  assert.match(worker, /AutoGateGuard gateGuard/);

  const types = source("src/windows/core/types.h");
  assert.match(types, /autoRetrievalInFlight\.load[\s\S]*\|\|[\s\S]*syncRetrievalInFlight\.load/);
});

test("issue #17: automatic clipboard sequence is published before the retrieval gate opens", () => {
  const types = source("src/windows/core/types.h");
  assert.match(types, /class ClipboardSequenceState/);
  const sequenceState = between(types, "class ClipboardSequenceState", "struct SelectionRequest");
  assert.match(sequenceState, /void Store[\s\S]*\+\+revision_/);
  assert.match(sequenceState, /if \(revision_ == expectedRevision\)[\s\S]*value_ = value/);

  const hook = source("src/windows/selection_hook.cc");
  assert.match(hook, /request\.clipboardSequenceState = clipboard_sequence_state/);
  assert.match(hook, /request\.clipboardSequenceRevision = clipboardSequence\.revision/);

  const worker = source("src/windows/core/worker.cc");
  const gatedRetrieval = between(worker, "AutoGateGuard gateGuard", "const ULONGLONG elapsed");
  const retrieval = gatedRetrieval.indexOf("engine.GetSelectedText");
  const publish = gatedRetrieval.indexOf("clipboardSequenceState->PublishIfUnchanged");
  assert.ok(retrieval >= 0 && publish > retrieval, "clipboard sequence must publish inside the worker gate");
});
