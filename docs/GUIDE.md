# Guide

**Part of [selection-hook](https://github.com/0xfullex/selection-hook)** — A Node.js native module for monitoring text selections across applications.

For the full API reference, see [API.md](API.md). For platform-specific details, see [Windows](WINDOWS.md) and [Linux](LINUX.md).

---

- [Understanding Selection Events](#understanding-selection-events) — `method`, `posLevel`, coordinate validity
- [Platform Setup](#platform-setup) — [Windows](#windows), [macOS](#macos), [Linux](#linux)
- [Linux: Wayland Decision Tree & Degradation](#linux-wayland-decision-tree--degradation) — decision tree, consumer action table, environment detection
- [Electron Integration](#electron-integration) — main process, TypeScript, coordinates, clipboard, lifecycle, Wayland
- [Configuration](#configuration) — `start()` config, global filtering, clipboard fallback
- [Passive Mode & Trigger Patterns](#passive-mode--trigger-patterns) — modifier key trigger, shortcut trigger
- [Best Practices](#best-practices)

---

## Understanding Selection Events

When a user selects text in any application, selection-hook emits a `text-selection` event with a [`TextSelectionData`](API.md#textselectiondata) object. Two key fields help you understand and use the event data:

### Selection Method

The `method` field tells you **how** the text was obtained:

| Constant | Platform | Meaning |
|---|---|---|
| `SelectionMethod.UIA` | Windows | UI Automation (modern apps) |
| `SelectionMethod.ACCESSIBLE` | Windows | IAccessible (legacy apps) |
| `SelectionMethod.AXAPI` | macOS | Accessibility API |
| `SelectionMethod.PRIMARY` | Linux | PRIMARY selection (X11/Wayland) |
| `SelectionMethod.CLIPBOARD` | Windows, macOS | Clipboard fallback (Ctrl+C / Cmd+C) |

In most cases, you do not need to handle different methods differently — the text content is the same regardless of how it was obtained.

### Position Level

The `posLevel` field tells you **which coordinate fields are available**:

| Level | Constant | Available Coordinates |
|---|---|---|
| 0 | `PositionLevel.NONE` | None — no position data |
| 1 | `PositionLevel.MOUSE_SINGLE` | `mousePosStart` and `mousePosEnd` (equal — double-click or single point) |
| 2 | `PositionLevel.MOUSE_DUAL` | `mousePosStart` and `mousePosEnd` (different — drag selection) |
| 3 | `PositionLevel.SEL_FULL` | All mouse positions + paragraph coordinates (`startTop`/`startBottom`/`endTop`/`endBottom`) |

Use `posLevel` to decide how to position UI elements (e.g., a floating toolbar) relative to the selection:

```javascript
hook.on("text-selection", (data) => {
  let anchorPoint;

  switch (data.posLevel) {
    case SelectionHook.PositionLevel.NONE:
      // No coordinates available — use fallback (e.g., screen center or cursor query)
      break;

    case SelectionHook.PositionLevel.MOUSE_SINGLE:
      // Single point — show UI near the mouse position
      anchorPoint = { x: data.mousePosEnd.x, y: data.mousePosEnd.y + 16 };
      break;

    case SelectionHook.PositionLevel.MOUSE_DUAL:
      // Drag selection — show UI near the end of the drag
      anchorPoint = { x: data.mousePosEnd.x, y: data.mousePosEnd.y + 16 };
      break;

    case SelectionHook.PositionLevel.SEL_FULL:
      // Full paragraph coordinates — show UI below the last line
      anchorPoint = { x: data.endBottom.x, y: data.endBottom.y + 4 };
      break;
  }
});
```

### Coordinate Validity

On some platforms, coordinate fields may be unavailable. Always check against `INVALID_COORDINATE` before using coordinates for positioning:

```javascript
if (data.mousePosEnd.x !== SelectionHook.INVALID_COORDINATE) {
  // Coordinates are valid — use them
  showToolbar(data.mousePosEnd.x, data.mousePosEnd.y);
}
```

> **When are coordinates invalid?**
> - **Linux:** `startTop`/`startBottom`/`endTop`/`endBottom` are always `-99999` (text range coordinates are not available on Linux)
> - **Linux Wayland:** `mousePosStart`/`mousePosEnd` may also be `-99999` depending on the compositor. See [Linux: Wayland Decision Tree](#linux-wayland-decision-tree--degradation)

---

## Platform Setup

### Windows

No special setup required. Selection-hook works out of the box on Windows 7+.

Some applications with custom cursors or special clipboard behavior may need additional configuration via [`setFineTunedList()`](API.md#setfinetunedlistlisttype-programlist-boolean). See [Windows Platform Details](WINDOWS.md) for the full explanation of when and how to configure this.

**Coordinates** are in physical (virtual screen) coordinates. In Electron, convert with `screen.screenToDipPoint()`. See [Coordinate Handling](#coordinate-handling).

### macOS

macOS requires **Accessibility permissions** before selection-hook can respond to events. The hook will start successfully even without permissions, but it will not detect any selections or input events until permissions are granted.

**In Node.js:**

```javascript
const hook = new SelectionHook();

if (!hook.macIsProcessTrusted()) {
  // Returns current status, may show a system dialog
  hook.macRequestProcessTrust();
  console.log("Please grant Accessibility permission in System Settings, then restart.");
  process.exit(0);
}

hook.start();
```

**In Electron:**

```javascript
const { systemPreferences } = require("electron");

if (!systemPreferences.isTrustedAccessibilityClient(false)) {
  // Show prompt to user
  systemPreferences.isTrustedAccessibilityClient(true);
  // Guide user to System Settings > Privacy & Security > Accessibility
}
```

**Chrome/Electron accessibility activation:**

On macOS, Chrome/Chromium-based browsers and Electron apps do not expose their accessibility tree by default. When selection-hook detects that the Accessibility API cannot retrieve text from the focused application, it automatically sets `AXEnhancedUserInterface` (for Chrome) and `AXManualAccessibility` (for Electron apps) to enable AXAPI access. This activation is done only once per application and only when the default AXAPI attempt fails.

**Side effect:** Enabling `AXEnhancedUserInterface` forces Chrome to build its full accessibility tree, which may cause a slight performance overhead in Chrome (increased memory usage and slower rendering in some scenarios). This is a known trade-off — without it, AXAPI cannot read selected text from Chrome at all.

**Other macOS notes:**
- `setFineTunedList()` has no effect on macOS
- Coordinates are in logical coordinates — no conversion needed
- The `isFullscreen` field in `TextSelectionData` is only available on macOS

### Linux

**X11** works well with no special setup. All features are supported.

**Wayland** has platform-level limitations that require runtime detection and degradation handling. See the [decision tree below](#linux-wayland-decision-tree--degradation) for a complete guide.

For full platform documentation, see [Linux Platform Details](LINUX.md).

---

## Linux: Wayland Decision Tree & Degradation

On Wayland, selection-hook's capabilities depend on several runtime conditions. Use `linuxGetEnvInfo()` to detect the environment and handle degradation gracefully.

### Decision Tree

```
const info = hook.linuxGetEnvInfo();

1. info.displayProtocol === X11?
   └─→ Everything works normally. No special handling needed.

2. info.displayProtocol === WAYLAND:

   2a. info.compositorType === MUTTER (GNOME)?
       └─→ ❌ Selection monitoring NOT supported
            Mutter does not implement the data-control protocol.
            → Inform user: this feature is unavailable on GNOME Wayland.

   2b. info.hasInputDeviceAccess === false?
       └─→ ⚠️ Degraded mode (data-control debounce):
            - Text selection still works, but with a slight delay
            - Mouse/keyboard events are NOT available
            - posLevel is at most MOUSE_SINGLE
            → Prompt user: run `sudo usermod -aG input $USER` and re-login.

   2c. info.hasInputDeviceAccess === true:
       └─→ ✅ Full functionality available.
            Cursor position accuracy depends on compositor:
            - KWIN, HYPRLAND → accurate screen coordinates
            - SWAY, WLROOTS, COSMIC → XWayland fallback (may freeze)
            → Check INVALID_COORDINATE on coordinates.

   2d. [Electron only] Running under XWayland?
       └─→ selection-hook and Electron may use different coordinate spaces.
            → See Electron Integration section.
```

### Consumer Action Table

| Condition | Impact | What to Do |
|---|---|---|
| GNOME Wayland | Selection monitoring unavailable | Inform user; suggest switching to X11 session or another compositor |
| No input device access | No mouse/keyboard events; selection slightly delayed | Prompt user to join the `input` group |
| XWayland fallback compositor | Cursor coordinates may freeze over native Wayland windows | Check `INVALID_COORDINATE`; fall back to `screen.getCursorScreenPoint()` in Electron |
| `programName` always empty | `setGlobalFilterMode()` with program names has no effect | Skip program-name-based filtering on Wayland |
| No text range coordinates | `posLevel` is at most `MOUSE_DUAL`; `startTop`/`endBottom` always `-99999` | Adapt UI positioning to work without paragraph coordinates |

### Environment Detection Example

```javascript
const SelectionHook = require("selection-hook");
const hook = new SelectionHook();

if (process.platform === "linux") {
  const info = hook.linuxGetEnvInfo();

  if (info.displayProtocol === SelectionHook.DisplayProtocol.WAYLAND) {
    // Check compositor support
    if (info.compositorType === SelectionHook.CompositorType.MUTTER) {
      console.warn("Selection monitoring is not supported on GNOME Wayland.");
      // Disable the feature or inform the user
    }

    // Check input device access
    if (!info.hasInputDeviceAccess) {
      console.warn(
        "Limited functionality: no input device access.\n" +
        "Run: sudo usermod -aG input $USER\n" +
        "Then log out and log back in."
      );
    }
  }
}
```

---

## Electron Integration

### Main Process Only

Selection-hook is a native Node.js addon and **must run in the Electron main process**. Forward events to the renderer via IPC:

```javascript
// Main process
const SelectionHook = require("selection-hook");
const { ipcMain, BrowserWindow } = require("electron");

const hook = new SelectionHook();

hook.on("text-selection", (data) => {
  const win = BrowserWindow.getFocusedWindow();
  if (win) {
    win.webContents.send("selection:text-selected", data);
  }
});

hook.start();
```

### Module Loading with TypeScript

For TypeScript projects, import types separately from the runtime module:

```typescript
import type {
  SelectionHookConstructor,
  SelectionHookInstance,
  TextSelectionData,
} from "selection-hook";

const SelectionHook: SelectionHookConstructor = require("selection-hook");
const hook: SelectionHookInstance = new SelectionHook();
```

### Coordinate Handling

Coordinates differ by platform. In Electron, use this pattern:

```javascript
const { screen } = require("electron");

hook.on("text-selection", (data) => {
  if (data.endBottom.x === SelectionHook.INVALID_COORDINATE) {
    // Coordinates unavailable (Linux) — use cursor position as fallback
    const cursor = screen.getCursorScreenPoint();
    positionToolbar(cursor.x, cursor.y);
    return;
  }

  // Windows: convert physical → logical coordinates
  const point = process.platform === "win32"
    ? screen.screenToDipPoint({ x: data.endBottom.x, y: data.endBottom.y })
    : { x: data.endBottom.x, y: data.endBottom.y };

  positionToolbar(point.x, point.y);
});
```

### Linux Clipboard Workaround

`writeToClipboard()` and `readFromClipboard()` return `false`/`null` on Linux. Use Electron's clipboard API instead:

```javascript
const { clipboard } = require("electron");

function writeClipboard(text) {
  if (process.platform === "linux") {
    clipboard.writeText(text);
    return true;
  }
  return hook.writeToClipboard(text);
}
```

### Lifecycle Management

Tie hook lifecycle to the Electron app lifecycle:

```javascript
const { app } = require("electron");

app.on("will-quit", () => {
  hook.stop();
  hook.cleanup();
});
```

### Wayland: XWayland Recommendation

On Linux Wayland, Electron's `BrowserWindow.setPosition()` and `getBounds()` do not work correctly under native Wayland. It is recommended to run Electron in XWayland mode:

```bash
your-electron-app --ozone-platform=x11
```

> **Note:** `app.commandLine.appendSwitch("ozone-platform", "x11")` does **not** work — the ozone platform is initialized before application JavaScript runs. You must set this flag via command line or `.desktop` file.

> **Electron 38+:** The default ozone platform is `auto` (native Wayland). `ELECTRON_OZONE_PLATFORM_HINT` is removed in Electron 39+. Use the `--ozone-platform=x11` command line flag.

See [Linux Platform Details — Hint for Electron Applications](LINUX.md#hint-for-electron-applications) for full details.

---

## Configuration

### start() Config vs. Individual Methods

You can configure selection-hook in two ways:

**Option 1:** Pass a config object to `start()`:

```javascript
hook.start({
  debug: true,
  enableClipboard: false,
  globalFilterMode: SelectionHook.FilterMode.EXCLUDE_LIST,
  globalFilterList: ["terminal.exe", "cmd.exe"],
});
```

**Option 2:** Call individual methods before or after `start()`:

```javascript
hook.disableClipboard();
hook.setGlobalFilterMode(SelectionHook.FilterMode.EXCLUDE_LIST, ["terminal.exe", "cmd.exe"]);
hook.start();
```

Both approaches produce the same result. Configuration methods can be called at any time — before `start()`, after `start()`, or even after `stop()` and before the next `start()`.

### Global Filtering

Control which applications trigger selection events:

```javascript
// Only monitor selections in specific apps
hook.setGlobalFilterMode(SelectionHook.FilterMode.INCLUDE_LIST, [
  "chrome.exe", "firefox.exe", "code.exe"
]);

// Monitor all apps except terminals
hook.setGlobalFilterMode(SelectionHook.FilterMode.EXCLUDE_LIST, [
  "WindowsTerminal.exe", "cmd.exe", "powershell.exe"
]);
```

> **Linux Wayland:** `programName` is always empty on Wayland, so program-based filtering has no effect. See [Linux Platform Details](LINUX.md).

### Clipboard Fallback

Clipboard fallback is enabled by default and is used as a last resort on Windows and macOS when native APIs fail. For full details on how clipboard fallback works and how to configure it for specific applications, see [Windows Platform Details](WINDOWS.md).

Key configuration methods:
- `enableClipboard()` / `disableClipboard()` — toggle clipboard fallback globally
- `setClipboardMode(mode, list)` — control which apps use clipboard fallback
- `setFineTunedList(type, list)` — handle app-specific clipboard edge cases (Windows only)

> **Linux:** Clipboard fallback is not implemented on Linux. `writeToClipboard()` returns `false` and `readFromClipboard()` returns `null`. Host applications should use their own clipboard API (e.g., Electron's `clipboard` module).

---

## Passive Mode & Trigger Patterns

In passive mode, `text-selection` events are **not emitted**. Instead, you retrieve selections on demand using `getCurrentSelection()`. This is useful for trigger-based workflows where the user explicitly requests the current selection.

### Example: Modifier Key Trigger

```javascript
// Enable passive mode — no automatic text-selection events
hook.setSelectionPassiveMode(true);
hook.start();

// Listen for a modifier key hold
let keyDownTime = 0;

hook.on("key-down", (data) => {
  // Check for Ctrl key (Windows vkCode: 162/163)
  if (data.vkCode === 162 || data.vkCode === 163) {
    if (keyDownTime === 0) keyDownTime = Date.now();

    // Trigger after holding for 500ms
    if (Date.now() - keyDownTime > 500) {
      const selection = hook.getCurrentSelection();
      if (selection) {
        console.log("Selected text:", selection.text);
      }
      keyDownTime = -1; // Prevent re-trigger
    }
  }
});

hook.on("key-up", (data) => {
  if (data.vkCode === 162 || data.vkCode === 163) {
    keyDownTime = 0;
  }
});
```

### Example: Shortcut Trigger

```javascript
hook.setSelectionPassiveMode(true);
hook.start();

// External shortcut triggers this function
function onShortcutPressed() {
  const selection = hook.getCurrentSelection();
  if (selection) {
    processSelection(selection);
  }
}
```

---

## Best Practices

- **Always call `cleanup()` before exit.** This releases native resources and stops event monitoring. In Electron, call it in the `will-quit` event.

- **Handle the `error` event.** General errors are only emitted when `debug: true` is set. Fatal errors (startup/shutdown failures) are always emitted.

  ```javascript
  hook.on("error", (error) => {
    console.error("SelectionHook error:", error.message);
  });
  ```

- **Avoid `enableMouseMoveEvent()` unless needed.** Mouse move events fire at high frequency and cause significant CPU usage. Only enable when you specifically need cursor tracking.

- **Use a cross-platform coordinate pattern.** Always check `INVALID_COORDINATE` first, then convert by platform:

  ```javascript
  function getLogicalPoint(point) {
    if (point.x === SelectionHook.INVALID_COORDINATE) return null;
    if (process.platform === "win32") {
      // Electron: convert physical → logical
      return screen.screenToDipPoint(point);
    }
    return point; // macOS and Linux: already logical/screen coordinates
  }
  ```
