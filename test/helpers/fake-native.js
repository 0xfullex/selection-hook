/**
 * Native isolation for L1 unit tests of index.js.
 *
 * loadIndexWith() loads a fresh copy of index.js with `node-gyp-build`
 * replaced (via Module._load) by a stub that hands out FakeNative instances,
 * and with process.platform overridden while the module initialises. No
 * production code is modified.
 */

const Module = require("node:module");
const path = require("node:path");

const INDEX_PATH = path.join(__dirname, "..", "..", "index.js");

/** Every method index.js can call on the native TextSelectionHook (13 public + 3 platform). */
const NATIVE_METHODS = [
  "start",
  "stop",
  "getCurrentSelection",
  "enableMouseMoveEvent",
  "disableMouseMoveEvent",
  "enableClipboard",
  "disableClipboard",
  "setClipboardMode",
  "setGlobalFilterMode",
  "setFineTunedList",
  "setSelectionPassiveMode",
  "writeToClipboard",
  "readFromClipboard",
  "macIsProcessTrusted",
  "macRequestProcessTrust",
  "linuxGetEnvInfo",
];

const DEFAULT_RETURNS = {
  getCurrentSelection: null,
  writeToClipboard: true,
  readFromClipboard: "fake clipboard",
  macIsProcessTrusted: true,
  macRequestProcessTrust: true,
  linuxGetEnvInfo: { displayProtocol: 1 },
};

class FakeNative {
  constructor() {
    /** @type {{ method: string, args: any[] }[]} */
    this.calls = [];
    this.callback = null;
    this.throws = new Map();
    this.returns = { ...DEFAULT_RETURNS };

    for (const method of NATIVE_METHODS) {
      this[method] = (...args) => {
        this.calls.push({ method, args });
        if (this.throws.has(method)) throw this.throws.get(method);
        if (method === "start") this.callback = args[0];
        if (method === "stop") this.callback = null;
        return this.returns[method];
      };
    }
  }

  /** Make `method` throw `error` on its next calls. */
  throwOn(method, error = new Error(`fake ${method} failure`)) {
    this.throws.set(method, error);
  }

  /** Deliver an event object to the callback registered by start(). */
  push(data) {
    if (!this.callback) throw new Error("push() called before start()");
    this.callback(data);
  }

  /** Method names called so far, in order. */
  methods() {
    return this.calls.map((c) => c.method);
  }

  /** Calls to one method. */
  callsTo(method) {
    return this.calls.filter((c) => c.method === method);
  }
}

/**
 * @param {object} [options]
 * @param {string} [options.platform] value for process.platform during load (default: current)
 * @param {boolean} [options.gypBuildThrows] make node-gyp-build throw
 * @param {boolean} [options.constructorThrows] make `new TextSelectionHook()` throw
 * @returns {{ SelectionHook: any, natives: FakeNative[] }}
 */
function loadIndexWith({ platform = process.platform, gypBuildThrows = false, constructorThrows = false } = {}) {
  const natives = [];

  function TextSelectionHook() {
    if (constructorThrows) throw new Error("fake constructor failure");
    const native = new FakeNative();
    natives.push(native);
    return native;
  }

  const fakeGypBuild = () => {
    if (gypBuildThrows) throw new Error("fake gyp build failure");
    return { TextSelectionHook };
  };

  const originalLoad = Module._load;
  const platformDescriptor = Object.getOwnPropertyDescriptor(process, "platform");

  Module._load = function (request, parent, isMain) {
    if (request === "node-gyp-build") return fakeGypBuild;
    return originalLoad.call(this, request, parent, isMain);
  };
  Object.defineProperty(process, "platform", { value: platform, configurable: true });

  try {
    delete require.cache[INDEX_PATH];
    const SelectionHook = require(INDEX_PATH);
    return { SelectionHook, natives };
  } finally {
    Module._load = originalLoad;
    Object.defineProperty(process, "platform", platformDescriptor);
    delete require.cache[INDEX_PATH];
  }
}

module.exports = { loadIndexWith, FakeNative, NATIVE_METHODS };
