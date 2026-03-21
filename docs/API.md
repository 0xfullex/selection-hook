# API Reference

- [Constructor](#constructor)
- [Methods](#methods)
  - [Lifecycle](#lifecycle) — `start()`, `stop()`, `isRunning()`, `cleanup()`
  - [Selection](#selection) — `getCurrentSelection()`, `setSelectionPassiveMode()`
  - [Mouse Tracking](#mouse-tracking) — `enableMouseMoveEvent()`, `disableMouseMoveEvent()`
  - [Clipboard](#clipboard) — `enableClipboard()`, `disableClipboard()`, `setClipboardMode()`, `writeToClipboard()`, `readFromClipboard()`
  - [Filtering](#filtering) — `setGlobalFilterMode()`, `setFineTunedList()`
  - [Platform-Specific](#platform-specific) — `macIsProcessTrusted()`, `macRequestProcessTrust()`, `linuxGetEnvInfo()`
- [Events](#events) — `text-selection`, `mouse-move`, `mouse-up`, `mouse-down`, `mouse-wheel`, `key-down`, `key-up`, `status`, `error`
- [Types](#types) — `SelectionConfig`, `TextSelectionData`, `MouseEventData`, `MouseWheelEventData`, `KeyboardEventData`, `LinuxEnvInfo`, `Point`
- [Constants](#constants) — `INVALID_COORDINATE`, `SelectionMethod`, `PositionLevel`, `FilterMode`, `FineTunedListType`, `DisplayProtocol`, `CompositorType`
- [TypeScript Support](#typescript-support)

---

## Constructor

```javascript
const hook = new SelectionHook();
```

Creates a new SelectionHook instance and initializes the native module. The native instance is created immediately in the constructor, so query methods (e.g., `linuxGetEnvInfo()`, `macIsProcessTrusted()`) and configuration methods (e.g., `enableClipboard()`, `setGlobalFilterMode()`) can be called before `start()`.

---

## Methods

### Lifecycle

#### `start(config?): boolean`

Start monitoring text selections.

Configuration methods can be called before `start()` to pre-configure the hook. If `start()` is called with a config object, the config values will override any pre-start settings that differ from defaults.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `config` | [`SelectionConfig`](#selectionconfig) | No | — | Configuration options. See [`SelectionConfig`](#selectionconfig) for all available fields and defaults. |

**Returns:** `boolean` — `true` if started successfully.

See [`SelectionHook.FilterMode`](#selectionhookfiltermode) for filter mode details.

> **Linux:** `enableClipboard`, `clipboardMode`, and `clipboardFilterList` have no effect on Linux (clipboard fallback is not implemented). On Wayland, `globalFilterMode`/`globalFilterList` are ineffective because `programName` is always empty. See [Linux platform details](LINUX.md) for full details.

> **macOS:** macOS requires accessibility permissions for the selection-hook to function properly. Ensure the user has enabled accessibility permissions before calling `start()`.
> - **Node**: use `selection-hook`'s `macIsProcessTrusted()` and `macRequestProcessTrust()` to check and request permissions.
> - **Electron**: use Electron's `systemPreferences.isTrustedAccessibilityClient()` to check permissions.

#### `stop(): boolean`

Stop monitoring text selections.

**Returns:** `boolean` — `true` if stopped successfully.

#### `isRunning(): boolean`

Check if selection-hook is currently running.

**Returns:** `boolean` — `true` if monitoring is active.

#### `cleanup(): void`

Release resources and stop monitoring. Should be called before the application exits.

---

### Selection

#### `getCurrentSelection(): TextSelectionData | null`

Get the current text selection if any exists.

**Returns:** [`TextSelectionData`](#textselectiondata) `| null` — Current selection data, or `null` if no selection exists.

#### `setSelectionPassiveMode(passive): boolean`

Set passive mode for selection. In passive mode, `text-selection` events will not be emitted — selections are only retrieved via `getCurrentSelection()`. Can be called before `start()`.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `passive` | `boolean` | Yes | — | `true` to enable passive mode, `false` to disable. |

**Returns:** `boolean` — `true` if set successfully.

---

### Mouse Tracking

#### `enableMouseMoveEvent(): boolean`

Enable mouse move events. This causes high CPU usage due to frequent event firing. Disabled by default. Can be called before `start()`.

**Returns:** `boolean` — `true` if enabled successfully.

#### `disableMouseMoveEvent(): boolean`

Disable mouse move events. This is the default state. Can be called before `start()`.

**Returns:** `boolean` — `true` if disabled successfully.

---

### Clipboard

#### `enableClipboard(): boolean`

Enable clipboard fallback for text selection. Enabled by default. Can be called before `start()`.

**Returns:** `boolean` — `true` if enabled successfully.

> **Linux:** No effect on Linux. Linux uses PRIMARY selection instead of clipboard fallback.

#### `disableClipboard(): boolean`

Disable clipboard fallback for text selection. Clipboard is enabled by default. Can be called before `start()`.

**Returns:** `boolean` — `true` if disabled successfully.

> **Linux:** No effect on Linux.

#### `setClipboardMode(mode, programList?): boolean`

Configure how clipboard fallback works with different programs. See [`SelectionHook.FilterMode`](#selectionhookfiltermode) constants for details. Can be called before `start()`.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `mode` | [`FilterMode`](#selectionhookfiltermode) | Yes | — | Clipboard filter mode. |
| `programList` | `string[]` | No | `[]` | Program names to include or exclude. |

**Returns:** `boolean` — `true` if set successfully.

> **Linux:** No effect on Linux.

#### `writeToClipboard(text): boolean`

Write text to the system clipboard. Useful for implementing custom copy functions. Can be called before `start()`.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `text` | `string` | Yes | — | Text to write to clipboard. |

**Returns:** `boolean` — `true` if written successfully. Always returns `false` on Linux.

> **Platform:** Windows, macOS only. Not supported on Linux. Host applications should use their own clipboard API (e.g., Electron clipboard).

#### `readFromClipboard(): string | null`

Read text from the system clipboard. Can be called before `start()`.

**Returns:** `string | null` — Clipboard text content, or `null` if clipboard is empty or contains non-text data. Always returns `null` on Linux.

> **Platform:** Windows, macOS only. Not supported on Linux. Host applications should use their own clipboard API (e.g., Electron clipboard).

---

### Filtering

#### `setGlobalFilterMode(mode, programList?): boolean`

Configure which applications should trigger text selection events. You can include or exclude specific applications from selection monitoring. See [`SelectionHook.FilterMode`](#selectionhookfiltermode) constants for details. Can be called before `start()`.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `mode` | [`FilterMode`](#selectionhookfiltermode) | Yes | — | Global filter mode. |
| `programList` | `string[]` | No | `[]` | Program names to include or exclude. |

**Returns:** `boolean` — `true` if set successfully.

> **Linux:** On Wayland, `programName` is always empty so program-based filtering will not work.

#### `setFineTunedList(listType, programList?): boolean`

Configure fine-tuned lists for specific application behaviors. Can be called before `start()`. This allows you to customize how the selection hook behaves with certain applications that may have unique characteristics.

For example, you can add `acrobat.exe` to those lists to enable text selected in Acrobat.

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `listType` | [`FineTunedListType`](#selectionhookfinetunedlisttype) | Yes | — | Fine-tuned list type. |
| `programList` | `string[]` | No | `[]` | Program names for the fine-tuned list. |

**Returns:** `boolean` — `true` if set successfully.

> **Platform:** Windows only.

---

### Platform-Specific

#### macOS

#### `macIsProcessTrusted(): boolean`

Check if the process is trusted for accessibility. Can be called before `start()`. If the process is not trusted, selection-hook will still run, but it won't respond to any events. Make sure to guide the user through the authorization process before calling `start()`.

**Returns:** `boolean` — `true` if the process is trusted for accessibility.

> **Platform:** macOS only.

#### `macRequestProcessTrust(): boolean`

Try to request accessibility permissions. Can be called before `start()`. This MAY show a dialog to the user if permissions are not granted.

**Returns:** `boolean` — The current permission status, not the request result.

> **Platform:** macOS only.

#### Linux

#### `linuxGetEnvInfo(): LinuxEnvInfo | null`

Get Linux environment information. Can be called before `start()`. Returns an object with display protocol, compositor type, input device access status, and root status. All values are detected once at construction time and cached. Returns `null` on non-Linux platforms.

**Returns:** [`LinuxEnvInfo`](#linuxenvinfo) `| null` — Linux environment info, or `null` on non-Linux platforms.

See [`LinuxEnvInfo`](#linuxenvinfo) for the full structure, and [`SelectionHook.CompositorType`](#selectionhookcompositortype) for compositor constants.

```javascript
const info = hook.linuxGetEnvInfo();
// info = {
//   displayProtocol: 2,       // SelectionHook.DisplayProtocol.WAYLAND
//   compositorType: 1,        // SelectionHook.CompositorType.KWIN
//   hasInputDeviceAccess: true, // user can access input devices
//   isRoot: false
// }
```

> **Platform:** Linux only.

---

## Events

#### `text-selection`

Emitted when text is selected in any application. See [`TextSelectionData`](#textselectiondata) for the `data` structure.

```javascript
hook.on("text-selection", (data) => {
  // data contains selection information
});
```

#### `mouse-move`, `mouse-up`, `mouse-down`

Mouse events. See [`MouseEventData`](#mouseeventdata) for the `data` structure.

```javascript
hook.on("mouse-up", (data) => {
  // data contains mouse coordinates and button info
});
```

#### `mouse-wheel`

Mouse wheel events. See [`MouseWheelEventData`](#mousewheeleventdata) for the `data` structure.

```javascript
hook.on("mouse-wheel", (data) => {
  // data contains wheel direction info
});
```

#### `key-down`, `key-up`

Keyboard events. See [`KeyboardEventData`](#keyboardeventdata) for the `data` structure.

```javascript
hook.on("key-down", (data) => {
  // data contains key code and modifier info
});
```

#### `status`

Hook status changes.

```javascript
hook.on("status", (status) => {
  // status is a string, e.g. "started", "stopped"
});
```

#### `error`

Error events. Only emitted when `debug` is set to `true` in `start()`.

```javascript
hook.on("error", (error) => {
  // error is an Error object
});
```

---

## Types

> **Coordinate note:**
> - **Windows:** Coordinates are in physical coordinates (virtual screen coordinates). Use `screen.screenToDipPoint(point)` in Electron to convert to logical coordinates.
> - **macOS:** Coordinates are in logical coordinates. No conversion needed.
> - **Linux X11:** Coordinates are in screen coordinates. No conversion needed.
> - **Linux Wayland:** Coordinate accuracy depends on the compositor (see [Linux platform details](LINUX.md)).

### `Point`

Represents a 2D coordinate point.

```typescript
{ x: number; y: number }
```

---

### `SelectionConfig`

Configuration options for `start()`. All fields are optional. Can also be set individually via configuration methods before or after `start()`.

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `debug` | `boolean` | `false` | Enable debug logging. |
| `enableMouseMoveEvent` | `boolean` | `false` | Enable mouse move tracking. Can be set at runtime. |
| `enableClipboard` | `boolean` | `true` | Enable clipboard fallback. Can be set at runtime. |
| `selectionPassiveMode` | `boolean` | `false` | Enable passive mode. Can be set at runtime. |
| `clipboardMode` | [`FilterMode`](#selectionhookfiltermode) | `DEFAULT` | Clipboard filter mode. Can be set at runtime. |
| `clipboardFilterList` | `string[]` | `[]` | Program list for clipboard mode. Can be set at runtime. |
| `globalFilterMode` | [`FilterMode`](#selectionhookfiltermode) | `DEFAULT` | Global filter mode. Can be set at runtime. |
| `globalFilterList` | `string[]` | `[]` | Program list for global filter mode. Can be set at runtime. |

See [`SelectionHook.FilterMode`](#selectionhookfiltermode) for filter mode details.

---

### `TextSelectionData`

Represents text selection information including content, source application, and coordinates.

| Property | Type | Description |
|----------|------|-------------|
| `text` | `string` | The selected text content. |
| `programName` | `string` | Name of the application where selection occurred. Always empty on Linux Wayland. |
| `startTop` | [`Point`](#point) | First paragraph's top-left coordinates (px). |
| `startBottom` | [`Point`](#point) | First paragraph's bottom-left coordinates (px). |
| `endTop` | [`Point`](#point) | Last paragraph's top-right coordinates (px). |
| `endBottom` | [`Point`](#point) | Last paragraph's bottom-right coordinates (px). |
| `mousePosStart` | [`Point`](#point) | Mouse position when selection started (px). |
| `mousePosEnd` | [`Point`](#point) | Mouse position when selection ended (px). |
| `method` | [`SelectionMethod`](#selectionhookselectionmethod) | Indicates which method was used to detect the text selection. |
| `posLevel` | [`PositionLevel`](#selectionhookpositionlevel) | Indicates which positional data is provided. |
| `isFullscreen` | `boolean` | Whether the window is in fullscreen mode. _macOS only._ |

> **Linux:** `startTop`/`startBottom`/`endTop`/`endBottom` are always `-99999` ([`INVALID_COORDINATE`](#selectionhookinvalid_coordinate)) because selection bounding rectangles are not available. On Wayland, `mousePosStart`/`mousePosEnd` may also be `-99999` when the coordinate source (libevdev) cannot provide actual screen positions — see [Linux platform details](LINUX.md) for the compositor-dependent fallback chain.

When [`PositionLevel`](#selectionhookpositionlevel) is:

- `MOUSE_SINGLE`: only `mousePosStart` and `mousePosEnd` are provided, and `mousePosStart` equals `mousePosEnd`.
- `MOUSE_DUAL`: only `mousePosStart` and `mousePosEnd` are provided. On Linux Wayland, drag selection can achieve `MOUSE_DUAL` when the compositor provides accurate cursor positions at both mouse-down and mouse-up.
- `SEL_FULL`: all the mouse position and paragraph's coordinates are provided.

---

### `MouseEventData`

Contains mouse click/movement information in screen coordinates.

| Property | Type | Description |
|----------|------|-------------|
| `x` | `number` | Horizontal pointer position (px). |
| `y` | `number` | Vertical pointer position (px). |
| `button` | `number` | Same as WebAPIs' `MouseEvent.button`. `0`=Left, `1`=Middle, `2`=Right, `3`=Back, `4`=Forward, `-1`=None, `99`=Unknown. |

> **Linux:** On Wayland, `x`/`y` may be `-99999` ([`INVALID_COORDINATE`](#selectionhookinvalid_coordinate)) because the input source (libevdev) cannot provide actual screen positions. See [Linux platform details](LINUX.md).

If `button != -1` during a `mouse-move` event, it indicates dragging.

---

### `MouseWheelEventData`

Describes mouse wheel scrolling events.

| Property | Type | Description |
|----------|------|-------------|
| `button` | `number` | `0`=Vertical, `1`=Horizontal scroll. |
| `flag` | `number` | `1`=Up/Right, `-1`=Down/Left. |

---

### `KeyboardEventData`

Represents keyboard key presses/releases.

| Property | Type | Description |
|----------|------|-------------|
| `uniKey` | `string` | Unified key name, refer to MDN `KeyboardEvent.key`, converted from `vkCode`. |
| `vkCode` | `number` | Virtual key code. Definitions and values vary by platform (see below). |
| `sys` | `boolean` | Whether modifier keys (Ctrl/Alt/Win/⌘/⌥/Super/Fn) are pressed simultaneously. |
| `scanCode` | `number?` | Hardware scan code. _Windows only._ |
| `flags` | `number` | Additional state flags. On Linux: modifier bitmask (`0x01`=Shift, `0x02`=Ctrl, `0x04`=Alt, `0x08`=Meta). |
| `type` | `string?` | Internal event type. |
| `action` | `string?` | `"key-down"` or `"key-up"`. |

Platform-specific `vkCode` values:

- **Windows**: `VK_*` values of `vkCode`
- **macOS**: `kVK_*` values of `kCGKeyboardEventKeycode`
- **Linux**: `KEY_*` values from `<linux/input-event-codes.h>`

---

### `LinuxEnvInfo`

Returned by `linuxGetEnvInfo()`. Contains cached Linux environment information detected at construction time.

| Property | Type | Description |
|----------|------|-------------|
| `displayProtocol` | `number` | Display protocol ([`SelectionHook.DisplayProtocol`](#selectionhookdisplayprotocol)). |
| `compositorType` | `number` | Compositor type ([`SelectionHook.CompositorType`](#selectionhookcompositortype)). |
| `hasInputDeviceAccess` | `boolean` | Whether the user can access input devices (needed for Wayland libevdev input monitoring). Checks `input` group, ACLs, capabilities, and actual device access. Always `true` on X11. |
| `isRoot` | `boolean` | Whether the process is running as root. |

> **Platform:** Linux only.

---

## Constants

### `SelectionHook.INVALID_COORDINATE`

Sentinel value (`-99999`) indicating that a coordinate is unavailable or unreliable. On Linux Wayland, mouse event coordinates and selection position coordinates may be set to this value when the input source (libevdev) cannot provide actual screen positions. Check coordinate fields against this value before using them for UI positioning.

```javascript
if (data.mousePosEnd.x !== SelectionHook.INVALID_COORDINATE) {
  // Position is reliable, use it
}
```

---

### `SelectionHook.SelectionMethod`

Indicates which method was used to detect the text selection.

| Constant | Value | Platform | Description |
|----------|-------|----------|-------------|
| `NONE` | `0` | — | No selection detected. |
| `UIA` | `1` | Windows | UI Automation. |
| `ACCESSIBLE` | `3` | Windows | Accessibility interface. |
| `AXAPI` | `11` | macOS | Accessibility API. |
| `ATSPI` | `21` | Linux | Assistive Technology Service Provider Interface. Reserved — not currently used. |
| `PRIMARY` | `22` | Linux | Primary Selection. |
| `CLIPBOARD` | `99` | All | Clipboard fallback. |

---

### `SelectionHook.PositionLevel`

Indicates which positional data is provided.

| Constant | Value | Description |
|----------|-------|-------------|
| `NONE` | `0` | No position information. |
| `MOUSE_SINGLE` | `1` | Only single mouse position. |
| `MOUSE_DUAL` | `2` | Mouse start and end positions (when dragging to select). |
| `SEL_FULL` | `3` | Full selection coordinates. See [`TextSelectionData`](#textselectiondata) for details. Not available on Linux. |
| `SEL_DETAILED` | `4` | Detailed selection coordinates. Reserved for future use. |

---

### `SelectionHook.FilterMode`

| Constant | Value | Description |
|----------|-------|-------------|
| `DEFAULT` | `0` | The filter mode is disabled. |
| `INCLUDE_LIST` | `1` | Only the programs in the list will pass the filter. |
| `EXCLUDE_LIST` | `2` | Only the programs NOT in the list will pass the filter. |

---

### `SelectionHook.FineTunedListType`

Defines types for fine-tuned application behavior lists.

| Constant | Value | Description |
|----------|-------|-------------|
| `EXCLUDE_CLIPBOARD_CURSOR_DETECT` | `0` | Exclude cursor detection for clipboard operations. Useful for applications with custom cursors (e.g., Adobe Acrobat) where cursor shape detection may not work reliably. |
| `INCLUDE_CLIPBOARD_DELAY_READ` | `1` | Include delay when reading clipboard content. Useful for applications that modify clipboard content multiple times in quick succession (e.g., Adobe Acrobat). |

> **Platform:** Windows only.

---

### `SelectionHook.DisplayProtocol`

Defines the display protocol types used on Linux systems.

| Constant | Value | Description |
|----------|-------|-------------|
| `UNKNOWN` | `0` | No protocol detected or not applicable. |
| `X11` | `1` | X11 windowing system protocol. |
| `WAYLAND` | `2` | Wayland display server protocol. |

> **Platform:** Linux only.

---

### `SelectionHook.CompositorType`

Identifies the compositor. Values represent the **compositor**, not the desktop environment (DE). DE-bundled compositors are detected via `XDG_CURRENT_DESKTOP` (each DE uses exactly one compositor); standalone compositors are detected via their own environment variables.

| Constant | Compositor | Desktop Environment | Detected via | Cursor method |
|----------|------------|---------------------|--------------|---------------|
| `UNKNOWN` | — | — | — | XWayland fallback |
| `KWIN` | KWin (`kwin_wayland`) | KDE Plasma | `XDG_CURRENT_DESKTOP` contains "KDE" | KWin Scripting DBus |
| `MUTTER` | mutter (`gnome-shell`) | GNOME | `XDG_CURRENT_DESKTOP` contains "GNOME" | XWayland fallback |
| `HYPRLAND` | Hyprland | (standalone) | `HYPRLAND_INSTANCE_SIGNATURE` env var | Native IPC socket |
| `SWAY` | sway | (standalone) | `SWAYSOCK` env var | XWayland fallback |
| `WLROOTS` | various (labwc, river, ...) | (standalone) | `XDG_CURRENT_DESKTOP` contains "wlroots" | XWayland fallback |
| `COSMIC_COMP` | cosmic-comp | COSMIC (System76) | `XDG_CURRENT_DESKTOP` contains "COSMIC" | XWayland fallback |

> **Platform:** Linux only.

---

## TypeScript Support

This module includes TypeScript definitions. Import the module in TypeScript as:

```typescript
import {
  SelectionHookConstructor,
  SelectionHookInstance,
  SelectionConfig,
  TextSelectionData,
  MouseEventData,
  MouseWheelEventData,
  KeyboardEventData,
  LinuxEnvInfo,
} from "selection-hook";

// use `SelectionHookConstructor` for SelectionHook Class
const SelectionHook: SelectionHookConstructor = require("selection-hook");
// use `SelectionHookInstance` for SelectionHook instance
const hook: SelectionHookInstance = new SelectionHook();
```

See [`index.d.ts`](https://github.com/0xfullex/selection-hook/blob/main/index.d.ts) for details.
