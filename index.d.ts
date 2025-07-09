/**
 * Node Selection Hook
 *
 * This module provides a Node.js interface for monitoring text selections
 * across applications on Windows using UI Automation and Accessibility APIs.
 */

import { EventEmitter } from "events";

/**
 * Text selection data returned by the native module
 *
 * Contains the selected text and its position information.
 * Position coordinates are in screen coordinates (pixels).
 */
export interface TextSelectionData {
  /** The selected text content */
  text: string;
  /** The program name that triggered the selection */
  programName: string;
  /** First paragraph's left-top point (x, y) in pixels */
  startTop: { x: number; y: number };
  /** First paragraph's left-bottom point (x, y) in pixels */
  startBottom: { x: number; y: number };
  /** Last paragraph's right-top point (x, y) in pixels */
  endTop: { x: number; y: number };
  /** Last paragraph's right-bottom point (x, y) in pixels */
  endBottom: { x: number; y: number };
  /** Current mouse position (x, y) in pixels */
  mousePosStart: { x: number; y: number };
  /** Mouse down position (x, y) in pixels */
  mousePosEnd: { x: number; y: number };
  /** Selection method identifier */
  method: (typeof SelectionHook.SelectionMethod)[keyof typeof SelectionHook.SelectionMethod];
  /** Position level identifier */
  posLevel: (typeof SelectionHook.PositionLevel)[keyof typeof SelectionHook.PositionLevel];
  /** Whether the current app's front window is in fullscreen mode, macOS only */
  isFullscreen?: boolean;
}

/**
 * Mouse event data structure
 *
 * Contains information about mouse events such as clicks and movements.
 * Coordinates are in screen coordinates (pixels).
 */
export interface MouseEventData {
  /** X coordinate of mouse pointer (px) */
  x: number;
  /** Y coordinate of mouse pointer (px) */
  y: number;
  /** Mouse button identifier,
   * same as WebAPIs' MouseEvent.button
   * Left = 0, Middle = 1, Right = 2, Back = 3, Forward = 4,
   * Unknown = -1
   */
  button: number;
}

/**
 * Mouse wheel event data structure
 *
 * Contains information about mouse wheel events.
 */
export interface MouseWheelEventData {
  /** Mouse wheel button type
   * 0: Vertical
   * 1: Horizontal
   */
  button: number;
  /** Mouse wheel direction flag
   * 1: Up/Right
   * -1: Down/Left
   */
  flag: number;
}

/**
 * Keyboard event data structure
 *
 * Contains information about keyboard events such as key presses and releases.
 */
export interface KeyboardEventData {
  /**
   * Unified key value of the vkCode. Same as MDN `KeyboardEvent.key` values.
   * Converted from platform-specific vkCode.
   *
   * Values defined in https://developer.mozilla.org/en-US/docs/Web/API/UI_Events/Keyboard_event_key_values
   */
  uniKey: string;
  /** Virtual key code. The value is different on different platforms.
   *
   * Windows: VK_* values of vkCode, refer to https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
   * macOS: kVK_* values of kCGKeyboardEventKeycode, defined in `HIToolbox/Events.h`
   */
  vkCode: number;
  /** Whether modifier keys (Alt/Ctrl/Win/⌘/⌥/Fn) are pressed simultaneously */
  sys: boolean;
  /** Keyboard scan code. Windows Only. */
  scanCode?: number;
  /** Additional key flags. Varies on different platforms. */
  flags: number;
  /** Internal event type identifier */
  type?: string;
  /** Specific keyboard action (e.g., "key-down", "key-up") */
  action?: string;
}

/**
 * Configuration interface for text selection monitoring
 *
 * Contains settings that control the behavior of the text selection hook
 * and its various features like mouse tracking and clipboard fallback.
 */
export interface SelectionConfig {
  /** Enable debug logging for warnings and errors */
  debug?: boolean;
  /** Enable high CPU usage mouse movement tracking */
  enableMouseMoveEvent?: boolean;
  /** Enable clipboard fallback for text selection */
  enableClipboard?: boolean;
  /** Enable passive mode where selection requires manual trigger */
  selectionPassiveMode?: boolean;
  /** Mode for clipboard fallback behavior */
  clipboardMode?: (typeof SelectionHook.FilterMode)[keyof typeof SelectionHook.FilterMode];
  /** List of program names for clipboard mode filtering */
  programList?: string[];
}

/**
 * SelectionHook - Main class for text selection monitoring
 *
 * This class provides methods to start/stop monitoring text selections
 * across applications on Windows and emits events when selections occur.
 */
declare class SelectionHook extends EventEmitter {
  static SelectionMethod: {
    NONE: 0;
    UIA: 1;
    FOCUSCTL: 2;
    ACCESSIBLE: 3;
    AXAPI: 11;
    CLIPBOARD: 99;
  };

  static PositionLevel: {
    NONE: 0;
    MOUSE_SINGLE: 1;
    MOUSE_DUAL: 2;
    SEL_FULL: 3;
    SEL_DETAILED: 4;
  };

  static FilterMode: {
    DEFAULT: 0;
    INCLUDE_LIST: 1;
    EXCLUDE_LIST: 2;
  };

  static FineTunedListType: {
    EXCLUDE_CLIPBOARD_CURSOR_DETECT: 0;
    INCLUDE_CLIPBOARD_DELAY_READ: 1;
  };

  /**
   * Start monitoring text selections
   *
   * Initiates the native hooks to listen for text selection events
   * across all applications. This must be called before any events
   * will be emitted.
   *
   * @param debug Enable console debug logging (warnings and errors)
   * @returns Success status (true if started successfully)
   */
  start(config?: SelectionConfig | null): boolean;

  /**
   * Stop monitoring text selections
   *
   * Stops the native hooks and prevents further events from being emitted.
   * This should be called when monitoring is no longer needed to free resources.
   *
   * @returns Success status (true if stopped successfully or wasn't running)
   */
  stop(): boolean;

  /**
   * Check if hook is running
   *
   * Determines if the selection monitoring is currently active.
   *
   * @returns Running status (true if monitoring is active)
   */
  isRunning(): boolean;

  /**
   * Get current text selection
   *
   * Retrieves the current text selection, if any exists.
   * Returns null if no text is currently selected or hook isn't running.
   *
   * @returns Current selection data or null if no selection exists
   */
  getCurrentSelection(): TextSelectionData | null;

  /**
   * Enable mousemove events (high CPU usage)
   *
   * Enables "mouse-move" events to be emitted when the mouse moves.
   * Note: This can cause high CPU usage due to frequent event firing.
   *
   * @returns Success status (true if enabled successfully)
   */
  enableMouseMoveEvent(): boolean;

  /**
   * Disable mousemove events
   *
   * Stops emitting "mouse-move" events to reduce CPU usage.
   * This is the default state after starting the hook.
   *
   * @returns Success status (true if disabled successfully)
   */
  disableMouseMoveEvent(): boolean;

  /**
   * Enable clipboard fallback for text selection
   *
   * Uses Ctrl+C as a last resort to get selected text when other methods fail.
   * This might modify clipboard contents.
   *
   * @returns Success status (true if enabled successfully)
   */
  enableClipboard(): boolean;

  /**
   * Disable clipboard fallback for text selection
   *
   * Will not use Ctrl+C to get selected text.
   * This preserves clipboard contents.
   *
   * @returns Success status (true if disabled successfully)
   */
  disableClipboard(): boolean;

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
   * @param {string[]} programList - Array of program names to include/exclude
   * @returns {boolean} Success status
   */
  setClipboardMode(
    mode: (typeof SelectionHook.FilterMode)[keyof typeof SelectionHook.FilterMode],
    programList?: string[]
  ): boolean;

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
  setGlobalFilterMode(
    mode: (typeof SelectionHook.FilterMode)[keyof typeof SelectionHook.FilterMode],
    programList?: string[]
  ): boolean;

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
  setFineTunedList(
    listType: (typeof SelectionHook.FineTunedListType)[keyof typeof SelectionHook.FineTunedListType],
    programList?: string[]
  ): boolean;

  /**
   * Set selection passive mode
   * @param {boolean} passive - Passive mode
   * @returns {boolean} Success status
   */
  setSelectionPassiveMode(passive: boolean): boolean;

  /**
   * Write text to clipboard
   * @param {string} text - Text to write to clipboard
   * @returns {boolean} Success status
   */
  writeToClipboard(text: string): boolean;

  /**
   * Read text from clipboard
   * @returns {string|null} Text from clipboard or null if empty or error
   */
  readFromClipboard(): string | null;

  /**
   * Check if the process is trusted for accessibility (macOS only)
   *
   * Checks whether the current process has accessibility permissions
   * required for text selection monitoring on macOS.
   *
   * @returns {boolean} True if the process is trusted for accessibility, false otherwise
   */
  macIsProcessTrusted(): boolean;

  /**
   * Try to request accessibility permissions (macOS only)
   *
   * This MAY show a dialog to the user if permissions are not granted.
   *
   * @returns {boolean} The current permission status, not the request result
   */
  macRequestProcessTrust(): boolean;

  /**
   * Release resources
   *
   * Stops monitoring and releases all native resources.
   * Should be called before the application exits to prevent memory leaks.
   */
  cleanup(): void;

  /**
   * Register event listeners
   *
   * The hook emits various events that can be listened for:
   */

  /**
   * Emitted when text is selected in any application
   * @param event The event name
   * @param listener Callback function receiving selection data
   */
  on(event: "text-selection", listener: (data: TextSelectionData) => void): this;

  on(event: "mouse-up", listener: (data: MouseEventData) => void): this;
  on(event: "mouse-down", listener: (data: MouseEventData) => void): this;
  on(event: "mouse-move", listener: (data: MouseEventData) => void): this;
  on(event: "mouse-wheel", listener: (data: MouseWheelEventData) => void): this;

  on(event: "key-down", listener: (data: KeyboardEventData) => void): this;
  on(event: "key-up", listener: (data: KeyboardEventData) => void): this;

  on(event: "status", listener: (status: string) => void): this;
  on(event: "error", listener: (error: Error) => void): this;

  // Same events available with once() for one-time listeners
  once(event: "text-selection", listener: (data: TextSelectionData) => void): this;
  once(event: "mouse-up", listener: (data: MouseEventData) => void): this;
  once(event: "mouse-down", listener: (data: MouseEventData) => void): this;
  once(event: "mouse-move", listener: (data: MouseEventData) => void): this;
  once(event: "mouse-wheel", listener: (data: MouseWheelEventData) => void): this;
  once(event: "key-down", listener: (data: KeyboardEventData) => void): this;
  once(event: "key-up", listener: (data: KeyboardEventData) => void): this;
  once(event: "status", listener: (status: string) => void): this;
  once(event: "error", listener: (error: Error) => void): this;
}

/**
 * Instance type for the SelectionHook class
 */
export type SelectionHookInstance = InstanceType<typeof SelectionHook>;

/**
 * Constructor type for the SelectionHook class
 */
export type SelectionHookConstructor = typeof SelectionHook;

// Export the SelectionHook class
export default SelectionHook;
