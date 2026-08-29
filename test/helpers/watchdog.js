/**
 * Run a script in a child Node process with a hard timeout.
 *
 * A synchronous hang inside native code (for example a stop() that never
 * returns, issue #23) cannot be interrupted from inside the same process, so
 * anything that calls start() runs here and is SIGKILLed on timeout.
 */

const { spawn } = require("node:child_process");

/**
 * @param {object} options
 * @param {string} options.script absolute path of the script to run
 * @param {string[]} [options.args] script arguments
 * @param {string[]} [options.execArgv] node flags (e.g. --expose-gc)
 * @param {number} [options.timeoutMs]
 * @param {NodeJS.ProcessEnv} [options.env] full environment for the child
 * @returns {Promise<{ code: number|null, signal: string|null, stdout: string, stderr: string, timedOut: boolean }>}
 */
function runInChildWithTimeout({ script, args = [], execArgv = [], timeoutMs = 30000, env = process.env }) {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [...execArgv, script, ...args], {
      env,
      stdio: ["ignore", "pipe", "pipe"],
    });

    let stdout = "";
    let stderr = "";
    let timedOut = false;

    child.stdout.on("data", (d) => {
      stdout += d;
    });
    child.stderr.on("data", (d) => {
      stderr += d;
    });

    const timer = setTimeout(() => {
      timedOut = true;
      child.kill("SIGKILL");
    }, timeoutMs);

    // "close" fires after stdio is fully drained, unlike "exit".
    child.on("close", (code, signal) => {
      clearTimeout(timer);
      resolve({ code, signal, stdout, stderr, timedOut });
    });

    child.on("error", (err) => {
      clearTimeout(timer);
      resolve({ code: null, signal: null, stdout, stderr: stderr + String(err), timedOut });
    });
  });
}

/** Last `n` non-empty lines of `text`, for failure messages. */
function tail(text, n = 8) {
  return text
    .split(/\r?\n/)
    .filter((l) => l.trim() !== "")
    .slice(-n)
    .join("\n");
}

module.exports = { runInChildWithTimeout, tail };
