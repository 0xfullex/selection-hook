/**
 * Text Selection Hook - Test Application
 *
 * This test file demonstrates the functionality of the Node-API native module
 * for monitoring text selections across applications on Windows, macOS, and Linux.
 *
 * Features demonstrated:
 * - Text selection detection (drag and double-click)
 * - Mouse and keyboard event monitoring
 * - Interactive controls for testing different features
 */

// ===========================
// === Module Dependencies ===
// ===========================
const SelectionHook = require("../index.js");

// ===========================
// === Configuration ========
// ===========================

/**
 * Configuration flags for event display
 * Toggle these using keyboard shortcuts during runtime
 */
const config = {
  showMouseMoveEvents: false, // High CPU usage when enabled
  showMouseEvents: false, // Mouse clicks and wheel events
  showKeyboardEvents: false, // Keyboard key press/release events
  clipboardFallbackEnabled: false, // Use clipboard as fallback for text selection
  clipboardMode: 0, // Default clipboard mode (see SelectionHook.FilterMode)
  globalFilterMode: 0, // Default global filter mode (see SelectionHook.FilterMode)
  passiveModeEnabled: false, // Passive mode for text selection
  fineTunedListEnabled: false, // Fine-tuned list for specific application behaviors
  hookStarted: true, // Track if the hook is started (default: ON)
};

// Add variables for Ctrl key hold detection
let ctrlKeyPressTime = null;
let ctrlKeyTriggered = false; // Flag to prevent multiple triggers
const CTRL_HOLD_THRESHOLD = 500; // 500ms threshold for Ctrl key hold

// Program list for clipboard mode and global filter
const programList = ["cursor.exe", "vscode.exe", "notepad.exe"];

// Fine-tuned list for specific application behaviors
const fineTunedList = ["acrobat.exe", "wps.exe", "cajviewer.exe"];

/**
 * ANSI color codes for console output formatting
 */
const colors = {
  info: "\x1b[36m%s\x1b[0m", // Cyan - Used for general information
  success: "\x1b[32m%s\x1b[0m", // Green - Used for successful operations and selections
  warning: "\x1b[33m%s\x1b[0m", // Yellow - Used for position data and toggles
  error: "\x1b[31m%s\x1b[0m", // Red - Used for errors
  highlight: "\x1b[35m%s\x1b[0m", // Magenta - Used for keyboard events
};

// ===========================
// === Initialize Hook ======
// ===========================

// Create instance of the hook
const hook = new SelectionHook();

// Exit if initialization failed
if (!hook) {
  console.error(colors.error, "Failed to initialize text selection hook");
  process.exit(1);
}

// ===========================
// === Core Functions =======
// ===========================

/**
 * Main entry point - initializes and starts the application
 */
function main() {
  printWelcomeMessage();
  setupEventListeners();
  startHook();
  setupInputHandlers();
}

/**
 * Display welcome message and usage instructions
 */
function printWelcomeMessage() {
  console.log(colors.info, "=== Text Selection Hook Test ===");
  console.log("Testing mouse action detection for text selection");

  console.log("\nPlease perform the following actions:");
  console.log("1. Drag to select text (distance > 8px) - should log 'action_drag_selection'");
  console.log("2. Double-click to select text - should log 'action_dblclick_selection'");

  console.log("\nCoordinates Information:");
  console.log("  - startTop/startBottom: First paragraph's left-top and left-bottom points");
  console.log("  - endTop/endBottom: Last paragraph's right-top and right-bottom points");
  console.log("  - mousePosStart: Initial mouse position when selection started");
  console.log("  - mousePosEnd: Final mouse position when selection ended");

  console.log("\nKeyboard Controls:");
  console.log("  S - Toggle start/stop hook (default: ON)");
  console.log("  M - Toggle mouse move events (default: OFF)");
  console.log("  E - Toggle other mouse events (default: OFF)");
  console.log("  K - Toggle keyboard events (default: OFF)");
  console.log("  C - Display current selection");
  console.log("  B - Toggle clipboard fallback (default: OFF)");
  console.log("  L - Toggle clipboard mode (DEFAULT → EXCLUDE_LIST → INCLUDE_LIST)");
  console.log("  F - Toggle global filter mode (DEFAULT → EXCLUDE_LIST → INCLUDE_LIST)");
  console.log("  P - Toggle passive mode (default: OFF)");
  console.log("  T - Toggle fine-tuned list (default: OFF)");
  console.log("  I - Show Linux environment info");
  console.log("  W - Write text 'Test clipboard write from selection-hook' to clipboard");
  console.log("  R - Read text from clipboard");
  console.log("  A - Check accessibility permissions (macOS only)");
  console.log("  Q - Request accessibility permissions (macOS only)");
  console.log("  ? - Show help");
  console.log("  Ctrl+C - Exit program");
  console.log("\nIn passive mode, press & hold Ctrl key to trigger text selection");
}

/**
 * Start the hook and begin listening for events
 */
function startHook() {
  if (hook.start({ debug: true })) {
    console.log(colors.success, "Text selection listener started successfully");
    console.log(colors.info, "Mouse move events are disabled by default (press 'M' to toggle)");
  } else {
    console.error(colors.error, "Failed to start text selection listener");
  }
}

/**
 * Clean up resources and exit the application
 */
function cleanup() {
  console.log(colors.info, "\nStopping text selection listener...");

  // Make sure to disable mouse move events before stopping to reduce resource usage
  if (config.showMouseMoveEvents) {
    hook.disableMouseMoveEvent();
  }

  // Disable clipboard fallback if enabled
  if (config.clipboardFallbackEnabled) {
    hook.disableClipboard();
  }

  hook.stop();
  hook.cleanup();
  process.exit(0);
}

// ===========================
// === Event Listeners ======
// ===========================

/**
 * Set up all event listeners for the hook
 */
function setupEventListeners() {
  // Text selection events - our primary functionality
  hook.on("text-selection", (selectionData) => {
    showSelection(selectionData);
  });

  // Status events from the native module
  hook.on("status", (status) => {
    console.log(colors.info, "Listener status:", status);
  });

  // Setup mouse events
  setupMouseEventListeners();

  // Setup keyboard events
  setupKeyboardEventListeners();

  // Error events
  hook.on("error", (error) => {
    console.error(colors.error, "Error:", error.message);
  });
}

/**
 * Set up mouse-related event listeners
 */
function setupMouseEventListeners() {
  // Mouse move events (high CPU usage - disabled by default)
  hook.on("mouse-move", (eventData) => {
    if (config.showMouseMoveEvents) {
      console.log(
        colors.warning,
        "Mouse move:",
        `x: ${eventData.x}, y: ${eventData.y}, button: ${eventData.button}`
      );
    }
  });

  // Mouse button events
  hook.on("mouse-up", (eventData) => {
    if (config.showMouseEvents) {
      console.log(
        colors.warning,
        "Mouse up:",
        `button: ${eventData.button}, x: ${eventData.x}, y: ${eventData.y}`
      );
    }
  });

  hook.on("mouse-down", (eventData) => {
    if (config.showMouseEvents) {
      console.log(
        colors.warning,
        "Mouse down:",
        `button: ${eventData.button}, x: ${eventData.x}, y: ${eventData.y}`
      );
    }
  });

  // Mouse wheel events
  hook.on("mouse-wheel", (eventData) => {
    if (config.showMouseEvents) {
      console.log(
        colors.warning,
        "Mouse wheel:",
        `button: ${eventData.button}, direction: ${eventData.flag > 0 ? "up/right" : "down/left"}`
      );
    }
  });
}

/**
 * Set up keyboard-related event listeners
 */
function setupKeyboardEventListeners() {
  // Key down events
  hook.on("key-down", (eventData) => {
    if (config.showKeyboardEvents) {
      console.log(
        colors.highlight,
        "Key down:",
        `uniKey: ${eventData.uniKey}, vkCode: ${eventData.vkCode}, scanCode: ${
          eventData.scanCode
        }, flags: ${eventData.flags}${eventData.sys ? ", system key" : ""}`
      );
    }

    // Special handling for Ctrl key to get current selection in passive mode
    if (eventData.uniKey === "Control" && config.passiveModeEnabled) {
      const currentTime = Date.now();

      // Initialize press time if not set
      if (ctrlKeyPressTime === null) {
        ctrlKeyPressTime = currentTime;
        ctrlKeyTriggered = false; // Reset trigger flag
        return;
      }

      // Check if held long enough and not already triggered
      const holdDuration = currentTime - ctrlKeyPressTime;
      if (holdDuration >= CTRL_HOLD_THRESHOLD && !ctrlKeyTriggered) {
        console.log(
          colors.info,
          `Ctrl held for ${holdDuration}ms, requesting current selection in passive mode`
        );
        const selectionData = hook.getCurrentSelection();
        if (selectionData) {
          showSelection(selectionData);
        } else {
          console.log(colors.warning, "No selection data available");
        }
        ctrlKeyTriggered = true; // Set flag to prevent further triggers
      }
    }
  });

  // Key up events
  hook.on("key-up", (eventData) => {
    if (config.showKeyboardEvents) {
      console.log(
        colors.highlight,
        "Key up:",
        `uniKey: ${eventData.uniKey}, vkCode: ${eventData.vkCode}, scanCode: ${
          eventData.scanCode
        }, flags: ${eventData.flags}${eventData.sys ? ", system key" : ""}`
      );
    }

    // Reset variables when Ctrl is released
    if (eventData.uniKey === "Control" && config.passiveModeEnabled) {
      ctrlKeyPressTime = null;
      ctrlKeyTriggered = false;
    }
  });
}

// ===========================
// === User Input Handling ==
// ===========================

/**
 * Configure handlers for user input
 */
function setupInputHandlers() {
  // Set up standard input in raw mode for immediate key handling
  process.stdin.setRawMode(true);
  process.stdin.resume();
  process.stdin.on("data", handleKeyPress);

  // Handle Ctrl+C for graceful exit
  process.on("SIGINT", cleanup);
}

/**
 * Process keyboard input from the user
 * @param {Buffer} key - The key buffer from stdin
 */
function handleKeyPress(key) {
  const keyStr = key.toString();

  // Exit on Ctrl+C
  if (keyStr === "\u0003") {
    cleanup();
    return;
  }

  switch (keyStr.toLowerCase()) {
    case "s": // Toggle start/stop hook
      toggleHookStartStop();
      break;

    case "m": // Toggle mouse move events
      toggleMouseMoveEvents();
      break;

    case "e": // Toggle other mouse events
      config.showMouseEvents = !config.showMouseEvents;
      console.log(colors.success, `Mouse events: ${config.showMouseEvents ? "ON" : "OFF"}`);
      break;

    case "k": // Toggle keyboard events
      config.showKeyboardEvents = !config.showKeyboardEvents;
      console.log(colors.success, `Keyboard events: ${config.showKeyboardEvents ? "ON" : "OFF"}`);
      break;

    case "c": // Display current selection
      const selectionData = hook.getCurrentSelection();
      if (selectionData) {
        console.log(colors.success, "Current selection retrieved");
        showSelection(selectionData);
      } else {
        console.log(colors.warning, "No current selection available");
      }
      break;

    case "b": // Toggle clipboard fallback
      config.clipboardFallbackEnabled = !config.clipboardFallbackEnabled;

      if (config.clipboardFallbackEnabled) {
        // Enable clipboard fallback in the native module
        const success = hook.enableClipboard();
        if (success) {
          console.log(colors.success, "Clipboard fallback: ENABLED");
        } else {
          config.clipboardFallbackEnabled = false;
          console.log(colors.error, "Failed to enable clipboard fallback");
        }
      } else {
        // Disable clipboard fallback in the native module
        const success = hook.disableClipboard();
        if (success) {
          console.log(colors.warning, "Clipboard fallback: DISABLED");
        } else {
          console.log(colors.error, "Failed to disable clipboard fallback");
        }
      }
      break;

    case "l": // Toggle clipboard mode
      toggleClipboardMode();
      break;

    case "f": // Toggle global filter mode
      toggleGlobalFilterMode();
      break;

    case "p": // Toggle passive mode
      togglePassiveMode();
      break;

    case "t": // Toggle fine-tuned list
      toggleFineTunedList();
      break;

    case "i": {
      // Show Linux environment info
      const envInfo = hook.linuxGetEnvInfo();
      if (envInfo) {
        const protocolNames = { 0: "Unknown", 1: "X11", 2: "Wayland" };
        const compositorNames = {
          0: "Unknown", 1: "KWin", 2: "Mutter", 3: "Hyprland",
          4: "Sway", 5: "Wlroots", 6: "CosmicComp",
        };
        console.log(colors.info, "=== Linux Environment Info ===");
        console.log(colors.info, `  Display Protocol: ${protocolNames[envInfo.displayProtocol] || envInfo.displayProtocol}`);
        console.log(colors.info, `  Compositor Type:  ${compositorNames[envInfo.compositorType] || envInfo.compositorType}`);
        console.log(colors.info, `  Input Device Access: ${envInfo.hasInputDeviceAccess}`);
        console.log(colors.info, `  Is Root: ${envInfo.isRoot}`);
      } else {
        console.log(colors.warning, "Linux environment info is only available on Linux");
      }
      break;
    }

    case "w":
      // Write test text to clipboard
      const testText = "Test clipboard write from selection-hook";
      const success = hook.writeToClipboard(testText);
      if (success) {
        console.log(colors.success, `Successfully wrote text to clipboard: "${testText}"`);
      } else {
        console.log(colors.error, "Failed to write text to clipboard");
      }
      break;

    case "r":
      // Read text from clipboard
      const clipboardText = hook.readFromClipboard();
      if (clipboardText) {
        console.log(colors.success, `Text read from clipboard: "${clipboardText}"`);
      } else {
        console.log(colors.warning, "Failed to read text from clipboard");
      }
      break;

    case "a":
      // Check accessibility permissions (macOS only)
      if (process.platform === "darwin") {
        const isTrusted = hook.macIsProcessTrusted();
        if (isTrusted) {
          console.log(colors.success, "Process is trusted for accessibility on macOS");
        } else {
          console.log(colors.warning, "Process is NOT trusted for accessibility on macOS");
          console.log(
            colors.info,
            "You may need to grant accessibility permissions in System Preferences"
          );
        }
      } else {
        console.log(colors.info, "Accessibility permission check is only available on macOS");
      }
      break;

    case "q":
      // Request accessibility permissions (macOS only)
      if (process.platform === "darwin") {
        console.log(colors.info, "Requesting accessibility permissions...");
        const currentStatus = hook.macRequestProcessTrust();
        if (currentStatus) {
          console.log(colors.success, "Process is already trusted for accessibility on macOS");
        } else {
          console.log(colors.warning, "Process is not trusted for accessibility on macOS");
          console.log(colors.info, "A system dialog may have appeared to grant permissions");
          console.log(
            colors.info,
            "Please check System Preferences > Security & Privacy > Accessibility"
          );
        }
      } else {
        console.log(colors.info, "Accessibility permission request is only available on macOS");
      }
      break;

    case "?":
      // Show help
      printWelcomeMessage();
      break;

    default:
      break;
  }
}

/**
 * Toggle hook start/stop
 * Controls whether the hook is actively listening for events
 */
function toggleHookStartStop() {
  config.hookStarted = !config.hookStarted;

  if (config.hookStarted) {
    // Start the hook
    const success = hook.start({ debug: true });
    if (success) {
      console.log(colors.success, "Text selection hook: STARTED");
    } else {
      config.hookStarted = false; // Revert the toggle
      console.log(colors.error, "Failed to start text selection hook");
    }
  } else {
    // Stop the hook
    const success = hook.stop();
    if (success) {
      console.log(colors.warning, "Text selection hook: STOPPED");
    } else {
      config.hookStarted = true; // Revert the toggle
      console.log(colors.error, "Failed to stop text selection hook");
    }
  }
}

/**
 * Toggle mouse move event tracking
 * Mouse move events use significant CPU resources, so they're disabled by default
 */
function toggleMouseMoveEvents() {
  config.showMouseMoveEvents = !config.showMouseMoveEvents;

  if (config.showMouseMoveEvents) {
    // Enable mouse move events in the native module
    const success = hook.enableMouseMoveEvent();
    if (success) {
      console.log(colors.success, "Mouse move events: ENABLED");
    } else {
      config.showMouseMoveEvents = false;
      console.log(colors.error, "Failed to enable mouse move events");
    }
  } else {
    // Disable mouse move events in the native module
    const success = hook.disableMouseMoveEvent();
    if (success) {
      console.log(colors.warning, "Mouse move events: DISABLED");
    } else {
      console.log(
        colors.error,
        "Failed to disable mouse move events, but events won't be displayed"
      );
    }
  }
}

/**
 * Toggle passive mode for text selection
 * In passive mode, the hook doesn't automatically monitor selections
 * and requires manual triggering (e.g., with Alt key)
 */
function togglePassiveMode() {
  config.passiveModeEnabled = !config.passiveModeEnabled;

  // Set passive mode in the native module
  const success = hook.setSelectionPassiveMode(config.passiveModeEnabled);
  if (success) {
    console.log(
      colors.success,
      `Passive mode: ${config.passiveModeEnabled ? "ENABLED" : "DISABLED"}`
    );
    if (config.passiveModeEnabled) {
      console.log(colors.info, "Press & hold Ctrl key to trigger text selection manually");
    }
  } else {
    config.passiveModeEnabled = !config.passiveModeEnabled; // Revert the toggle
    console.log(colors.error, "Failed to set passive mode");
  }
}

/**
 * Toggle fine-tuned list for specific application behaviors
 */
function toggleFineTunedList() {
  config.fineTunedListEnabled = !config.fineTunedListEnabled;

  const programList = config.fineTunedListEnabled ? fineTunedList : [];

  // Set fine-tuned list for both types simultaneously
  const excludeClipboardSuccess = hook.setFineTunedList(
    SelectionHook.FineTunedListType.EXCLUDE_CLIPBOARD_CURSOR_DETECT,
    programList
  );

  const includeDelaySuccess = hook.setFineTunedList(
    SelectionHook.FineTunedListType.INCLUDE_CLIPBOARD_DELAY_READ,
    programList
  );

  if (excludeClipboardSuccess && includeDelaySuccess) {
    console.log(
      colors.success,
      `Fine-tuned list: ${config.fineTunedListEnabled ? "ENABLED" : "DISABLED"}`
    );
    if (config.fineTunedListEnabled) {
      console.log(colors.info, `Programs in fine-tuned list: ${fineTunedList.join(", ")}`);
      console.log(
        colors.info,
        "Applied to both EXCLUDE_CLIPBOARD_CURSOR_DETECT and INCLUDE_CLIPBOARD_DELAY_READ"
      );
    }
  } else {
    config.fineTunedListEnabled = !config.fineTunedListEnabled; // Revert the toggle
    console.log(colors.error, "Failed to set fine-tuned list");
    if (!excludeClipboardSuccess) {
      console.log(colors.error, "- EXCLUDE_CLIPBOARD_CURSOR_DETECT failed");
    }
    if (!includeDelaySuccess) {
      console.log(colors.error, "- INCLUDE_CLIPBOARD_DELAY_READ failed");
    }
  }
}

/**
 * Get clipboard mode name from numeric value
 * @param {number} mode - Clipboard mode value
 * @returns {string} Mode name
 */
function getFilterModeName(mode) {
  const modeNames = {
    [SelectionHook.FilterMode.DEFAULT]: "DEFAULT",
    [SelectionHook.FilterMode.EXCLUDE_LIST]: "EXCLUDE_LIST",
    [SelectionHook.FilterMode.INCLUDE_LIST]: "INCLUDE_LIST",
  };
  return modeNames[mode] || "UNKNOWN";
}

/**
 * Toggle clipboard mode (DEFAULT -> EXCLUDE_LIST -> INCLUDE_LIST -> DEFAULT)
 */
function toggleClipboardMode() {
  // Rotate through the modes
  config.clipboardMode = (config.clipboardMode + 1) % 3;

  // Apply the new mode if clipboard fallback is enabled
  const success = hook.setClipboardMode(config.clipboardMode, programList);
  if (success) {
    console.log(
      colors.success,
      `Clipboard mode set to ${getFilterModeName(config.clipboardMode)} for ${programList.join(
        ", "
      )}`
    );
  } else {
    console.log(colors.error, "Failed to set clipboard mode");
  }
}

/**
 * Toggle global filter mode (DEFAULT -> EXCLUDE_LIST -> INCLUDE_LIST -> DEFAULT)
 */
function toggleGlobalFilterMode() {
  // Rotate through the modes
  config.globalFilterMode = (config.globalFilterMode + 1) % 3;

  // Apply the new mode
  const success = hook.setGlobalFilterMode(config.globalFilterMode, programList);
  if (success) {
    console.log(
      colors.success,
      `Global filter mode set to ${getFilterModeName(
        config.globalFilterMode
      )} for ${programList.join(", ")}`
    );
  } else {
    console.log(colors.error, "Failed to set global filter mode");
  }
}

// ===========================
// === Utility Functions ====
// ===========================

/**
 * Display text selection data
 * @param {Object} selectionData - Selection data from the hook
 */
function showSelection(selectionData) {
  if (!selectionData) {
    return;
  }

  console.log(colors.info, `=== Detected selected text (${selectionData.text.length}) ===`);

  console.log(selectionData.text.replace(/\r\n/g, "\n").replace(/\r(?!\n)/g, "\n"));

  // Selection method and position level maps
  const methodMap = {
    0: "None",
    1: "UI Automation",
    2: "Focus Control",
    3: "Accessibility",
    11: "AXAPI",
    21: "AT-SPI",
    22: "Primary Selection",
    99: "Clipboard",
  };

  const posLevelMap = {
    0: "None",
    1: "Mouse Single",
    2: "Mouse Dual",
    3: "Selection Full",
    4: "Selection Detailed",
  };

  console.log(colors.warning, "=== Selection Information ===");
  console.log(colors.warning, "Program Name:", selectionData.programName);
  console.log(
    colors.warning,
    "Method: ",
    `${methodMap[selectionData.method] || selectionData.method}`
  );
  console.log(
    colors.warning,
    "Position Level: ",
    `${posLevelMap[selectionData.posLevel] || selectionData.posLevel}`
  );

  // Display all available coordinates
  if (selectionData.posLevel >= 1) {
    console.log(
      colors.warning,
      "Mouse: ",
      `(${selectionData.mousePosStart.x}, ${selectionData.mousePosStart.y}) -> (${selectionData.mousePosEnd.x}, ${selectionData.mousePosEnd.y})`
    );
  }
  if (selectionData.posLevel >= 3) {
    console.log(
      colors.warning,
      `   startTop(${selectionData.startTop.x}, ${selectionData.startTop.y})\t   endTop(${selectionData.endTop.x}, ${selectionData.endTop.y})`
    );
    console.log(
      colors.warning,
      `startBottom(${selectionData.startBottom.x}, ${selectionData.startBottom.y})\tendBottom(${selectionData.endBottom.x}, ${selectionData.endBottom.y})`
    );
  }
}

// ===========================
// === Start Application ====
// ===========================

// Launch the application
main();
