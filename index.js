/**
 * Node Selection Hook
 *
 * This module provides a Node.js interface for monitoring text selections
 * across applications on Windows, macOS, and Linux using UI Automation and Accessibility APIs.
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */

const EventEmitter = require("events");
const gypBuild = require("node-gyp-build");
const path = require("path");

const isWindows = process.platform === "win32";
const isMac = process.platform === "darwin";
const isLinux = process.platform === "linux";

let nativeModule = null;
// Make debugFlag a private module variable to avoid global state issues
let _debugFlag = false;

try {
  if (!isWindows && !isMac && !isLinux) {
    throw new Error("[selection-hook] Only supports Windows, macOS, and Linux platforms");
  }
  nativeModule = gypBuild(path.resolve(__dirname));
} catch (err) {
  console.error("[selection-hook] Failed to load native module:", err.message);
}

class SelectionHook extends EventEmitter {
  #instance = null;
  #running = false;

  static SelectionMethod = {
    NONE: 0,
    UIA: 1,
    FOCUSCTL: 2,
    ACCESSIBLE: 3,
    AXAPI: 11,
    CLIPBOARD: 99,
  };

  static PositionLevel = {
    NONE: 0,
    MOUSE_SINGLE: 1,
    MOUSE_DUAL: 2,
    SEL_FULL: 3,
    SEL_DETAILED: 4,
  };

  static FilterMode = {
    DEFAULT: 0,
    INCLUDE_LIST: 1,
    EXCLUDE_LIST: 2,
  };

  static FineTunedListType = {
    EXCLUDE_CLIPBOARD_CURSOR_DETECT: 0,
    INCLUDE_CLIPBOARD_DELAY_READ: 1,
  };

  constructor() {
    if (!nativeModule) {
      throw new Error(
        "[selection-hook] Native module failed to load - only works on Windows, macOS, and Linux"
      );
    }
    super();
  }

  /**
   * Start monitoring text selections
   * @param {boolean} debug Enable debug logging
   * @returns {boolean} Success status
   */
  start(config = null) {
    const defaultConfig = this.#getDefaultConfig();

    _debugFlag = config?.debug ?? defaultConfig.debug;

    if (this.#running) {
      this.#logDebug("Text selection hook already running");
      return true;
    }

    if (!this.#instance) {
      try {
        this.#instance = new nativeModule.TextSelectionHook();
      } catch (err) {
        this.#handleError("Failed to create hook instance", err);
        return false;
      }
    }

    if (config) {
      this.#initByConfig(defaultConfig, config);
    }

    try {
      const callback = (data) => {
        try {
          if (!data || !data.type || !this.#running) return;

          switch (data.type) {
            case "text-selection":
              const formattedData = this.#formatSelectionData(data);
              if (formattedData) {
                this.emit("text-selection", formattedData);
              }
              break;
            case "mouse-event":
              this.emit(data.action, data);
              break;
            case "keyboard-event":
              this.emit(data.action, data);
              break;
            case "status":
              this.emit("status", data.status);
              break;
            case "error":
              this.emit("error", new Error(data.error));
              break;
          }
        } catch (err) {
          this.#handleError("Failed to process event data", err);
        }
      };

      this.#instance.start(callback);
      this.#running = true;
      this.emit("status", "started");
      return true;
    } catch (err) {
      this.#handleError("Failed to start hook", err, "fatal");
      return false;
    }
  }

  /**
   * Stop monitoring text selections, can start again using start()
   * @returns {boolean} Success status
   */
  stop() {
    if (!this.#instance || !this.#running) {
      this.#logDebug("Text selection hook not running");
      return true;
    }

    try {
      this.#instance.stop();
      this.#running = false;
      this.emit("status", "stopped");
      return true;
    } catch (err) {
      this.#handleError("Failed to stop hook", err, "fatal");
      this.#running = false;
      return false;
    }
  }

  /**
   * Get current text selection
   * @returns {object|null} Selection data or null
   */
  getCurrentSelection() {
    if (!this.#instance || !this.#running) {
      this.#logDebug("Text selection hook not running");
      return null;
    }

    try {
      const data = this.#instance.getCurrentSelection();
      return this.#formatSelectionData(data);
    } catch (err) {
      this.#handleError("Failed to get current selection", err);
      return null;
    }
  }

  /**
   * Enable mousemove events (high CPU usage)
   * @returns {boolean} Success status
   */
  enableMouseMoveEvent() {
    if (!this.#checkRunning()) return false;

    try {
      this.#instance.enableMouseMoveEvent();
      return true;
    } catch (err) {
      this.#handleError("Failed to enable mouse move events", err);
      return false;
    }
  }

  /**
   * Disable mousemove events
   * @returns {boolean} Success status
   */
  disableMouseMoveEvent() {
    if (!this.#checkRunning()) return false;

    try {
      this.#instance.disableMouseMoveEvent();
      return true;
    } catch (err) {
      this.#handleError("Failed to disable mouse move events", err);
      return false;
    }
  }

  /**
   * Enable clipboard fallback for text selection
   * Uses Ctrl+C as a last resort to get selected text
   * @returns {boolean} Success status
   */
  enableClipboard() {
    if (!this.#checkRunning()) return false;

    try {
      this.#instance.enableClipboard();
      return true;
    } catch (err) {
      this.#handleError("Failed to enable clipboard fallback", err);
      return false;
    }
  }

  /**
   * Disable clipboard fallback for text selection
   * Will not use Ctrl+C to get selected text
   * @returns {boolean} Success status
   */
  disableClipboard() {
    if (!this.#checkRunning()) return false;

    try {
      this.#instance.disableClipboard();
      return true;
    } catch (err) {
      this.#handleError("Failed to disable clipboard fallback", err);
      return false;
    }
  }

  /**
   * Set clipboard mode and program list for text selection
   *
   * Configures how the clipboard fallback mechanism works for different programs.
   * Mode can be:
   * - DEFAULT: Use clipboard for all programs
   * - INCLUDE_LIST: Only use clipboard for programs in the list
   * - EXCLUDE_LIST: Use clipboard for all programs except those in the list
   *
   * @param {number} mode - Clipboard mode (SelectionHook.ClipboardMode)
   * @param {string[]} programList - Array of program names to include/exclude.
   * @returns {boolean} Success status
   */
  setClipboardMode(mode, programList = []) {
    if (!this.#checkRunning()) return false;

    const validModes = Object.values(SelectionHook.FilterMode);
    if (!validModes.includes(mode)) {
      this.#handleError("Invalid clipboard mode", new Error("Invalid argument"));
      return false;
    }

    if (!Array.isArray(programList)) {
      this.#handleError("Program list must be an array", new Error("Invalid argument"));
      return false;
    }

    try {
      this.#instance.setClipboardMode(mode, programList);
      return true;
    } catch (err) {
      this.#handleError("Failed to set clipboard mode and list", err);
      return false;
    }
  }

  /**
   * Set global filter mode for text selection
   *
   * Configures how the global filter mechanism works for different programs.
   * Mode can be:
   * - DEFAULT: disable global filter
   * - INCLUDE_LIST: Only use global filter for programs in the list
   * - EXCLUDE_LIST: Use global filter for all programs except those in the list
   *
   * @param {number} mode - Filter mode (SelectionHook.FilterMode)
   * @param {string[]} programList - Array of program names to include/exclude
   * @returns {boolean} Success status
   */
  setGlobalFilterMode(mode, programList = []) {
    if (!this.#checkRunning()) return false;

    const validModes = Object.values(SelectionHook.FilterMode);
    if (!validModes.includes(mode)) {
      this.#handleError("Invalid filter mode", new Error("Invalid argument"));
      return false;
    }

    if (!Array.isArray(programList)) {
      this.#handleError("Program list must be an array", new Error("Invalid argument"));
      return false;
    }

    try {
      this.#instance.setGlobalFilterMode(mode, programList);
      return true;
    } catch (err) {
      this.#handleError("Failed to set global filter mode and list", err);
      return false;
    }
  }

  /**
   * Set fine-tuned list for specific behaviors
   *
   * Configures fine-tuned lists for specific application behaviors.
   * List types:
   * - EXCLUDE_CLIPBOARD_CURSOR_DETECT: Exclude cursor detection for clipboard operations
   * - INCLUDE_CLIPBOARD_DELAY_READ: Include delay when reading clipboard content
   *
   * @param {number} listType - Fine-tuned list type (SelectionHook.FineTunedListType)
   * @param {string[]} programList - Array of program names for the fine-tuned list
   * @returns {boolean} Success status
   */
  setFineTunedList(listType, programList = []) {
    if (!this.#checkRunning()) return false;

    const validTypes = Object.values(SelectionHook.FineTunedListType);
    if (!validTypes.includes(listType)) {
      this.#handleError("Invalid fine-tuned list type", new Error("Invalid argument"));
      return false;
    }

    if (!Array.isArray(programList)) {
      this.#handleError("Program list must be an array", new Error("Invalid argument"));
      return false;
    }

    try {
      this.#instance.setFineTunedList(listType, programList);
      return true;
    } catch (err) {
      this.#handleError("Failed to set fine-tuned list", err);
      return false;
    }
  }

  /**
   * Set selection passive mode
   * @param {boolean} passive - Passive mode
   * @returns {boolean} Success status
   */
  setSelectionPassiveMode(passive) {
    if (!this.#checkRunning()) return false;

    try {
      this.#instance.setSelectionPassiveMode(passive);
      return true;
    } catch (err) {
      this.#handleError("Failed to set selection passive mode", err);
      return false;
    }
  }

  /**
   * Write text to clipboard
   * @param {string} text - Text to write to clipboard
   * @returns {boolean} Success status
   */
  writeToClipboard(text) {
    if (!this.#checkRunning()) return false;

    if (typeof text !== "string") {
      this.#handleError("Text must be a string", new Error("Invalid argument"));
      return false;
    }

    try {
      return this.#instance.writeToClipboard(text);
    } catch (err) {
      this.#handleError("Failed to write text to clipboard", err);
      return false;
    }
  }

  /**
   * Read text from clipboard
   * @returns {string|null} Text from clipboard or null if empty or error
   */
  readFromClipboard() {
    if (!this.#checkRunning()) return null;

    try {
      return this.#instance.readFromClipboard();
    } catch (err) {
      this.#handleError("Failed to read text from clipboard", err);
      return null;
    }
  }

  /**
   * Check if the process is trusted for accessibility (macOS only)
   * @returns {boolean} True if the process is trusted for accessibility, false otherwise
   */
  macIsProcessTrusted() {
    if (!isMac) {
      this.#logDebug("Not supported on this platform");
      return false;
    }

    //don't need to be running
    if (!this.#instance) {
      this.#logDebug("Text selection hook instance not created");
      return false;
    }

    try {
      return this.#instance.macIsProcessTrusted();
    } catch (err) {
      this.#handleError("Failed to check macOS process trust status", err);
      return false;
    }
  }

  /**
   * Try to request accessibility permissions (macOS only)
   * This MAY show a dialog to the user if permissions are not granted
   * @returns {boolean} The current permission status, not the request result
   */
  macRequestProcessTrust() {
    if (!isMac) {
      this.#logDebug("Not supported on this platform");
      return false;
    }

    //don't need to be running
    if (!this.#instance) {
      this.#logDebug("Text selection hook instance not created");
      return false;
    }

    try {
      return this.#instance.macRequestProcessTrust();
    } catch (err) {
      this.#handleError("Failed to request macOS process trust", err);
      return false;
    }
  }

  /**
   * Check if hook is running
   * @returns {boolean} Running status
   */
  isRunning() {
    return this.#running;
  }

  /**
   * Release resources
   */
  cleanup() {
    this.stop();
    this.removeAllListeners();
    this.#instance = null;
  }

  #getDefaultConfig() {
    return {
      debug: false,
      enableMouseMoveEvent: false,
      enableClipboard: true,
      selectionPassiveMode: false,
      clipboardMode: SelectionHook.FilterMode.DEFAULT,
      globalFilterMode: SelectionHook.FilterMode.DEFAULT,
      clipboardFilterList: [],
      globalFilterList: [],
    };
  }

  #initByConfig(defaultConfig, userConfig) {
    const config = {};

    // Only keep values that exist in userConfig and differ from defaultConfig
    for (const key in userConfig) {
      if (userConfig[key] !== defaultConfig[key]) {
        config[key] = userConfig[key];
      }
    }

    // Apply the filtered config
    if (config.enableMouseMoveEvent !== undefined) {
      if (config.enableMouseMoveEvent) {
        this.#instance.enableMouseMoveEvent();
      } else {
        this.#instance.disableMouseMoveEvent();
      }
    }

    if (config.enableClipboard !== undefined) {
      if (config.enableClipboard) {
        this.#instance.enableClipboard();
      } else {
        this.#instance.disableClipboard();
      }
    }

    if (config.selectionPassiveMode !== undefined) {
      this.#instance.setSelectionPassiveMode(config.selectionPassiveMode);
    }

    if (config.clipboardMode !== undefined || config.clipboardFilterList !== undefined) {
      this.#instance.setClipboardMode(
        config.clipboardMode ?? defaultConfig.clipboardMode,
        config.clipboardFilterList ?? defaultConfig.clipboardFilterList
      );
    }

    if (config.globalFilterMode !== undefined || config.globalFilterList !== undefined) {
      this.#instance.setGlobalFilterMode(
        config.globalFilterMode ?? defaultConfig.globalFilterMode,
        config.globalFilterList ?? defaultConfig.globalFilterList
      );
    }
  }

  #formatSelectionData(data) {
    if (!data) return null;

    return {
      text: data.text,
      programName: data.programName,
      startTop: { x: data.startTopX, y: data.startTopY },
      startBottom: { x: data.startBottomX, y: data.startBottomY },
      endTop: { x: data.endTopX, y: data.endTopY },
      endBottom: { x: data.endBottomX, y: data.endBottomY },
      mousePosStart: { x: data.mouseStartX, y: data.mouseStartY },
      mousePosEnd: { x: data.mouseEndX, y: data.mouseEndY },
      method: data.method || 0,
      posLevel: data.posLevel || 0,
    };
  }

  // Private helper methods
  #checkRunning() {
    if (!this.#instance || !this.#running) {
      this.#logDebug("Text selection hook not running");
      return false;
    }
    return true;
  }

  // level:  "error" or "fatal"
  // fatal will always show the error message
  #handleError(message, err, level = "error") {
    if (!_debugFlag && level === "error") return;

    const errorMsg = `${message}: ${err.message}`;
    console.error("[selection-hook] ", errorMsg);

    if (err.stack) {
      console.error(err.stack);
    }

    this.emit("error", new Error(errorMsg));
  }

  #logDebug(message) {
    if (_debugFlag) {
      console.warn("[selection-hook] ", message);
    }
  }
}

module.exports = SelectionHook;
