#!/usr/bin/env node
/**
 * Unified test entry point.
 *
 *   node scripts/run-tests.js unit
 *   node scripts/run-tests.js integration
 *   node scripts/run-tests.js local [--no-rebuild]   (release gate, macOS only)
 *   node scripts/run-tests.js summary                (re-print test-results/*.tap from a previous run)
 *
 * unit / integration also append a markdown summary to $GITHUB_STEP_SUMMARY when
 * running inside GitHub Actions.
 *
 * Exit codes: 0 pass, 1 test failure or policy / gate violation,
 *             2 infrastructure error (missing tap, no test files, rebuild failed, wrong platform).
 *
 * This file deliberately lives in scripts/ (not test/) and is not named
 * test.js: a bare `node --test` would otherwise pick it up and recurse.
 */

const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");

const { parseTap, mergeSummaries } = require("../test/lib/tap-summary.js");
const { evaluateGate, checkSkipPolicy } = require("../test/lib/gate.js");

const ROOT = path.resolve(__dirname, "..");
const TEST_DIR = path.join(ROOT, "test");
const RESULTS_DIR = path.join(ROOT, "test-results");

const EXIT_PASS = 0;
const EXIT_FAIL = 1;
const EXIT_INFRA = 2;

const TIERS = {
  unit: { dir: "unit", extraArgs: [] },
  integration: { dir: "integration", extraArgs: ["--test-concurrency=1"] },
};

function log(msg = "") {
  process.stdout.write(msg + "\n");
}

function tapPath(tier) {
  return path.join(RESULTS_DIR, `${tier}.tap`);
}

/** Expand test/<dir>/*.test.js without relying on shell globbing. */
function listTestFiles(dir) {
  const abs = path.join(TEST_DIR, dir);
  if (!fs.existsSync(abs)) return [];
  return fs
    .readdirSync(abs)
    .filter((f) => f.endsWith(".test.js"))
    .sort()
    .map((f) => path.join(abs, f));
}

function readSummary(tier) {
  const p = tapPath(tier);
  if (!fs.existsSync(p)) return null;
  return parseTap(fs.readFileSync(p, "utf8"));
}

function printSummary(tier, summary) {
  log(`== ${tier}: pass ${summary.passed.length}  fail ${summary.failed.length}  skip ${summary.skipped.length} ==`);
  for (const name of summary.failed) log(`  FAIL  ${name}`);
  for (const { name, reason } of summary.skipped) log(`  SKIP  ${name}  (${reason || "no reason"})`);
}

function printViolations(title, violations) {
  log(`${title}:`);
  for (const v of violations) log(`  - ${v}`);
}

/**
 * Run one tier through `node --test`, write its tap file, apply the skip policy.
 * @returns {{ code: number, summary: object|null }}
 */
function runTier(tier) {
  const { dir, extraArgs } = TIERS[tier];
  const files = listTestFiles(dir);
  if (files.length === 0) {
    log(`[run-tests] no *.test.js files under test/${dir}`);
    return { code: EXIT_INFRA, summary: null };
  }

  fs.mkdirSync(RESULTS_DIR, { recursive: true });
  const tap = tapPath(tier);
  fs.rmSync(tap, { force: true });

  log(`[run-tests] ${tier}: ${files.length} file(s)`);
  const r = spawnSync(
    process.execPath,
    [
      "--test",
      "--test-reporter=spec",
      "--test-reporter-destination=stdout",
      "--test-reporter=tap",
      `--test-reporter-destination=${tap}`,
      ...extraArgs,
      ...files,
    ],
    { cwd: ROOT, stdio: "inherit", env: process.env }
  );

  const summary = readSummary(tier);
  if (!summary) {
    log(`[run-tests] ${tier}: tap output missing at ${tap} (node --test exit ${r.status})`);
    return { code: EXIT_INFRA, summary: null };
  }

  printSummary(tier, summary);

  const policy = checkSkipPolicy(summary, tier);
  if (!policy.ok) printViolations(`[run-tests] ${tier}: skip policy violations`, policy.violations);

  if (process.env.GITHUB_STEP_SUMMARY) {
    const md = [toMarkdown(tier, summary)];
    if (!policy.ok) {
      md.push("**Skip policy violations**", "", ...policy.violations.map((v) => `- ${v}`), "");
    }
    fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, md.join("\n") + "\n");
  }

  const failed = r.status !== 0 || summary.failed.length > 0 || !policy.ok;
  return { code: failed ? EXIT_FAIL : EXIT_PASS, summary };
}

function runRebuild() {
  log("[run-tests] rebuilding native module (build/ takes priority over prebuilds/)");
  const npm = process.platform === "win32" ? "npm.cmd" : "npm";
  const r = spawnSync(npm, ["run", "rebuild"], { cwd: ROOT, stdio: "inherit", shell: process.platform === "win32" });
  return r.status === 0;
}

function cmdLocal(args) {
  const quick = args.includes("--no-rebuild");
  const banner = "[run-tests] QUICK MODE: not a release gate (--no-rebuild)";

  if (process.platform !== "darwin") {
    log(`[run-tests] the release gate must run on macOS (issue #23 race), got ${process.platform}`);
    return EXIT_INFRA;
  }

  if (quick) {
    log(banner);
  } else if (!runRebuild()) {
    log("[run-tests] rebuild failed");
    return EXIT_INFRA;
  }

  process.env.SELECTION_HOOK_TEST_LOCAL = "1";

  const unit = runTier("unit");
  if (unit.code === EXIT_INFRA) return EXIT_INFRA;
  const integration = runTier("integration");
  if (integration.code === EXIT_INFRA) return EXIT_INFRA;

  const merged = mergeSummaries(unit.summary, integration.summary);
  const gate = evaluateGate(merged, { platform: process.platform });

  log("");
  if (gate.ok) {
    log("[run-tests] release gate: PASS");
  } else {
    printViolations("[run-tests] release gate: FAIL", gate.violations);
  }
  if (quick) log(banner);

  const ok = gate.ok && unit.code === EXIT_PASS && integration.code === EXIT_PASS;
  return ok ? EXIT_PASS : EXIT_FAIL;
}

function toMarkdown(tier, summary) {
  const lines = [
    `### ${tier}`,
    "",
    "| pass | fail | skip |",
    "|---|---|---|",
    `| ${summary.passed.length} | ${summary.failed.length} | ${summary.skipped.length} |`,
    "",
  ];
  if (summary.failed.length) {
    lines.push("**Failed**", "");
    for (const name of summary.failed) lines.push(`- \`${name}\``);
    lines.push("");
  }
  if (summary.skipped.length) {
    lines.push("**Skipped**", "");
    for (const { name, reason } of summary.skipped) lines.push(`- \`${name}\` - ${reason || "no reason"}`);
    lines.push("");
  }
  return lines.join("\n");
}

function cmdSummary() {
  let found = 0;
  for (const tier of Object.keys(TIERS)) {
    const summary = readSummary(tier);
    if (!summary) {
      log(`== ${tier}: no tap output ==`);
      continue;
    }
    found++;
    printSummary(tier, summary);
  }
  return found === 0 ? EXIT_INFRA : EXIT_PASS;
}

function main(argv) {
  const [command, ...rest] = argv;
  switch (command) {
    case "unit":
    case "integration":
      return runTier(command).code;
    case "local":
      return cmdLocal(rest);
    case "summary":
      return cmdSummary();
    default:
      log("usage: node scripts/run-tests.js <unit|integration|local [--no-rebuild]|summary>");
      return EXIT_INFRA;
  }
}

process.exitCode = main(process.argv.slice(2));
