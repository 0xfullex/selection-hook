/**
 * Static architecture regression for issue #25: the Windows UIA TextPattern path must
 * read the selection through GetSelection only.
 *
 * Pre-fix behavior these checks guard against: when GetSelection returned no non-empty
 * range, TryGetTextViaTextPattern() fetched the DocumentRange and called
 * GetAttributeValue(30034), expanded the range to TextUnit_Document, and called it again.
 * 30034 is UIA_IsScrollPatternAvailablePropertyId, an element property rather than a text
 * attribute, so conforming providers never answered TRUE, and a provider that did would
 * have returned the whole document as the selection. Chromium evaluates document-range
 * attributes on its browser UI thread by walking the page: on a page of ~100 styled cards
 * each call took ~30 s and froze every window of Chrome or Edge, twice for every gesture
 * that ended without a selection, such as a scrollbar drag.
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

// The fix records its rationale in comments that name 30034 and GetAttributeValue, so
// the checks below inspect code only.
function code(text) {
  return text.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/.*$/gm, "");
}

function textPatternReader() {
  return code(
    between(
      source("src/windows/core/engine.cc"),
      "bool SelectionEngine::TryGetTextViaTextPattern",
      "TextRetrievalResult SelectionEngine::GetTextViaUIAutomation"
    )
  );
}

test("windows: the TextPattern reader still uses GetSelection", () => {
  assert.match(textPatternReader(), /textPattern->GetSelection\(&ranges\)/);
});

test("windows: the TextPattern reader never queries the document range", () => {
  const reader = textPatternReader();
  assert.doesNotMatch(reader, /get_DocumentRange/, "DocumentRange must not be fetched (issue #25)");
  assert.doesNotMatch(reader, /GetAttributeValue/, "no text attribute marks a range as selected");
  assert.doesNotMatch(reader, /ExpandToEnclosingUnit/, "expanding to TextUnit_Document walks the whole page");
});

test("windows: the misnamed 30034 constant is gone", () => {
  const engine = code(source("src/windows/core/engine.cc"));
  assert.doesNotMatch(engine, /UIA_IsSelectionActivePropertyId/);
  assert.doesNotMatch(engine, /\b30034\b/, "30034 is UIA_IsScrollPatternAvailablePropertyId");
});

test("windows: no caller can re-enable a document fallback", () => {
  const header = code(source("src/windows/core/engine.h"));
  assert.match(
    header,
    /bool TryGetTextViaTextPattern\(IUIAutomationElement \*element, TextSelectionInfo &selectionInfo\);/
  );
  assert.doesNotMatch(header, /allowDocumentFallback/);
  assert.doesNotMatch(code(source("src/windows/core/engine.cc")), /allowDocumentFallback/);
});
