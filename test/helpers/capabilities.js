/**
 * Capability probes and tier helpers for integration tests.
 *
 * Skipping is only allowed through requires() / localTier(), and only for
 * tests registered in SKIP_ALLOWLIST (test/lib/gate.js). The reason strings
 * produced here are the ones that allowlist checks by prefix.
 */

const isWindows = process.platform === "win32";
const isMac = process.platform === "darwin";
const isLinux = process.platform === "linux";

const cache = new Map();

function tryLoadSelectionHook() {
  try {
    const SelectionHook = require("../../index.js");
    const probe = new SelectionHook();
    probe.cleanup();
    return { ok: true, SelectionHook };
  } catch (err) {
    return { ok: false, detail: err.message };
  }
}

const probes = {
  // The native .node can be loaded and a native instance constructed.
  // index.js swallows the load error, so constructing is the only real probe.
  native() {
    const r = tryLoadSelectionHook();
    return r.ok ? { ok: true } : { ok: false, detail: r.detail };
  },

  // Global input events can be observed: macOS accessibility permission,
  // an X11 / Wayland display on Linux, always assumed on Windows (phase 1).
  "input-monitoring"() {
    if (isMac) {
      const r = tryLoadSelectionHook();
      if (!r.ok) return { ok: false, detail: `native not loadable (${r.detail})` };
      const hook = new r.SelectionHook();
      const trusted = hook.macIsProcessTrusted();
      hook.cleanup();
      return trusted ? { ok: true } : { ok: false, detail: "accessibility permission not granted" };
    }
    if (isLinux) {
      if (process.env.DISPLAY || process.env.WAYLAND_DISPLAY) return { ok: true };
      return { ok: false, detail: "no DISPLAY or WAYLAND_DISPLAY" };
    }
    if (isWindows) return { ok: true };
    return { ok: false, detail: `unsupported platform ${process.platform}` };
  },

  // Native clipboard read/write exists (index.js short-circuits on Linux).
  clipboard() {
    if (isLinux) return { ok: false, detail: "not implemented on linux" };
    return { ok: true };
  },
};

function probe(name) {
  if (!probes[name]) throw new Error(`unknown capability: ${name}`);
  if (!cache.has(name)) cache.set(name, probes[name]());
  return cache.get(name);
}

/**
 * Skip the test unless every named capability is available (AND semantics).
 * Usage: `if (!requires(t, "native", "input-monitoring")) return;`
 * @returns {boolean} true when the test may continue
 */
function requires(t, ...names) {
  for (const name of names) {
    const r = probe(name);
    if (!r.ok) {
      t.skip(`needs ${name}: ${r.detail}`);
      return false;
    }
  }
  return true;
}

/**
 * Local tier: always runs in CI, opt-in on dev machines via SELECTION_HOOK_TEST_LOCAL=1.
 * @returns {boolean} true when the test may continue
 */
function localTier(t) {
  if (process.env.CI || process.env.SELECTION_HOOK_TEST_LOCAL === "1") return true;
  t.skip("local tier: set SELECTION_HOOK_TEST_LOCAL=1");
  return false;
}

module.exports = { isWindows, isMac, isLinux, requires, localTier, probe };
