#!/usr/bin/env node
/**
 * Cross-platform clang-format wrapper.
 *
 *   node scripts/format.js           format src/**\/*.{cc,mm,h} in place
 *   node scripts/format.js --check   fail if any file would change
 *
 * The binary is taken from the CLANG_FORMAT environment variable (default
 * "clang-format") so CI can pin a version, e.g. CLANG_FORMAT=clang-format-20.
 * Files matched by .clang-format-ignore are excluded by clang-format itself
 * (supported since clang-format 18).
 */

const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

const ROOT = path.resolve(__dirname, "..");
const SRC = path.join(ROOT, "src");
const EXTENSIONS = new Set([".cc", ".mm", ".h"]);

function collect(dir, out) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, entry.name);
    if (entry.isDirectory()) collect(p, out);
    else if (EXTENSIONS.has(path.extname(entry.name))) out.push(p);
  }
  return out;
}

function main() {
  const check = process.argv.includes("--check");
  const bin = process.env.CLANG_FORMAT || "clang-format";

  const version = spawnSync(bin, ["--version"], { encoding: "utf8" });
  if (version.error || version.status !== 0) {
    console.error(`[format] cannot run ${bin}: ${version.error ? version.error.message : version.stderr}`);
    return 2;
  }
  process.stdout.write(version.stdout);

  const files = collect(SRC, []).sort();
  if (files.length === 0) {
    console.error("[format] no source files found under src/");
    return 2;
  }

  const args = check ? ["--dry-run", "--Werror", ...files] : ["-i", ...files];
  const r = spawnSync(bin, args, { cwd: ROOT, stdio: "inherit" });
  if (r.status !== 0) {
    console.error(check ? "[format] check failed: run `npm run format`" : "[format] clang-format failed");
    return 1;
  }
  console.log(`[format] ${check ? "checked" : "formatted"} ${files.length} file(s)`);
  return 0;
}

process.exitCode = main();
