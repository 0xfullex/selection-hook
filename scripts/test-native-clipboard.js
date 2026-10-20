#!/usr/bin/env node
/** Build production clipboard code against Win32 fakes; never load the addon or access a desktop. */
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

if (process.platform !== "win32") {
  console.error("Windows only: native clipboard fixtures require the Windows compiler toolchain.");
  process.exit(1);
}
const group = process.argv[2];
if (group !== "restore" && group !== "activity") throw new Error("expected restore or activity group");
const root = path.resolve(__dirname, "..");
const fixture = path.join(root, "test/native/clipboard-restore");
const engine = fs.readFileSync(path.join(root, "src/windows/core/engine.cc"), "utf8");

// Extract complete production methods, rather than maintain a second implementation.
function method(signature) {
  const start = engine.indexOf(signature);
  if (start < 0) throw new Error(`production method missing: ${signature}`);
  if (engine.indexOf(signature, start + signature.length) !== -1) {
    throw new Error(`production method is not unique: ${signature}`);
  }
  const opening = engine.indexOf("{", start);
  let depth = 1;
  let end = opening + 1;
  for (; depth && end < engine.length; ++end) {
    if (engine[end] === "{") ++depth;
    if (engine[end] === "}") --depth;
  }
  if (depth) throw new Error(`incomplete production method: ${signature}`);
  return engine.slice(start, end);
}
fs.mkdirSync(path.join(fixture, "build"), { recursive: true });
const methods = [
  "bool SelectionEngine::GetTextViaClipboard(",
  "bool SelectionEngine::GetTextViaClipboardTransaction(",
  "bool SelectionEngine::GetTextViaClipboardLegacy(",
  "bool SelectionEngine::OpenClipboardWithRetry(",
  "bool SelectionEngine::ReadClipboardWithRetry(",
  "bool SelectionEngine::SendCopyKey(",
  "bool SelectionEngine::ShouldKeyInterruptViaClipboard(",
  "bool SelectionEngine::SleepInterruptibly(",
].map(method).join("\n\n") + "\n";
const generated = path.join(fixture, "build/production-engine.inc");
if (!fs.existsSync(generated) || fs.readFileSync(generated, "utf8") !== methods) fs.writeFileSync(generated, methods);

function run(command, args) {
  const result = spawnSync(command, args, { cwd: root, stdio: "inherit" });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status || 1);
}
const nodeGyp = require.resolve("node-gyp/bin/node-gyp.js");
const configuration = (process.env.npm_package_config_node_gyp_debug ?? process.env.npm_config_debug) ? "Debug" : "Release";
run(process.execPath, [nodeGyp, "configure", "build", `clipboard-${group}`, "--directory", fixture]);
run(path.join(fixture, `build/${configuration}/clipboard-${group}.exe`), []);
