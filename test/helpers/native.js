/**
 * Native module access for parent-process integration tests.
 *
 * Only use these for calls that do NOT start the hook. Anything that calls
 * start() belongs in a scenario script run through runScenario().
 */

let loadError = null;

/**
 * @returns {typeof import("../../index.js") | null} the SelectionHook class, or null on load failure
 */
function loadSelectionHook() {
  try {
    return require("../../index.js");
  } catch (err) {
    loadError = err;
    return null;
  }
}

function getLoadError() {
  return loadError;
}

/**
 * Create a hook with an error listener attached, run `fn(hook, errors)`, and
 * always cleanup() afterwards.
 */
function withHook(fn) {
  const SelectionHook = loadSelectionHook();
  if (!SelectionHook) throw loadError;
  const hook = new SelectionHook();
  const errors = [];
  hook.on("error", (err) => errors.push(err));
  try {
    return fn(hook, errors);
  } finally {
    hook.cleanup();
  }
}

module.exports = { loadSelectionHook, getLoadError, withHook };
