# API Reference

### Constructor

```javascript
const hook = new SelectionHook();
```

Creates a new SelectionHook instance and initializes the native module. The native instance is created immediately in the constructor, so query methods (e.g., `linuxGetEnvInfo()`, `macIsProcessTrusted()`) and configuration methods (e.g., `enableClipboard()`, `setGlobalFilterMode()`) can be called before `start()`.

### Methods

#### **`start(config?: SelectionConfig): boolean`**

Start monitoring text selections.

Configuration methods can be called before `start()` to pre-configure the hook. If `start()` is called with a config object, the config values will override any pre-start settings that differ from defaults.

Config options (with default values):

```javascript
{
  debug?: false,                 // Enable debug logging
  enableMouseMoveEvent?: false,  // Enable mouse move tracking, can be set in runtime
  enableClipboard?: true,        // Enable clipboard fallback, can be set in runtime
  selectionPassiveMode?: false,  // Enable passive mode, can be set in runtime
  clipboardMode?: SelectionHook.FilterMode.DEFAULT,    // Clipboard mode, can be set in runtime
  clipboardFilterList?: string[]                       // Program list for clipboard mode, can be set in runtime
  globalFilterMode?: SelectionHook.FilterMode.DEFAULT, // Global filter mode, can be set in runtime
  globalFilterList?: string[]                          // Global program list for global filter mode, can be set in runtime
}
```

see [`SelectionHook.FilterMode`](#selectionhookfiltermode) for details

**For _Linux_**:
`enableClipboard`, `clipboardMode`, and `clipboardFilterList` have no effect on Linux (clipboard fallback is not implemented). On Wayland, `globalFilterMode`/`globalFilterList` are ineffective because `programName` is always empty. See [`docs/LINUX.md`](LINUX.md) for full details.

**For _macOS_**:
macOS requires accessibility permissions for the selection-hook to function properly. Please ensure that the user has enabled accessibility permissions before calling start().

- **Node**: use `selection-hook`'s `macIsProcessTrusted` and `macRequestProcessTrust` to check whether the permission is granted.
- **Electron**: use `electon`'s `systemPreferences.isTrustedAccessibilityClient` to check whether the permission is granted.

#### **`stop(): boolean`**

Stop monitoring text selections.

#### **`getCurrentSelection(): TextSelectionData | null`**

Get the current text selection if any exists.

#### **`enableMouseMoveEvent(): boolean`**

Enable mouse move events (high CPU usage). Disabled by default. Can be called before `start()`.

#### **`disableMouseMoveEvent(): boolean`**

Disable mouse move events. Disabled by default. Can be called before `start()`.

#### **`enableClipboard(): boolean`**

Enable clipboard fallback for text selection. Enabled by default. No effect on Linux (uses PRIMARY selection instead of clipboard fallback). Can be called before `start()`.

#### **`disableClipboard(): boolean`**

Disable clipboard fallback for text selection. Enabled by default. No effect on Linux. Can be called before `start()`.

#### **`setClipboardMode(mode: ClipboardMode, programList?: string[]): boolean`**

Configure how clipboard fallback works with different programs. See `SelectionHook.FilterMode` constants for details. No effect on Linux. Can be called before `start()`.

#### **`setGlobalFilterMode(mode: FilterMode, programList?: string[]): boolean`**

Configure which applications should trigger text selection events. You can include or exclude specific applications from the selection monitoring. See `SelectionHook.FilterMode` constants for details. On Linux Wayland, `programName` is always empty so program-based filtering will not work. Can be called before `start()`.

#### **`setFineTunedList(listType: FineTunedListType, programList?: string[]): boolean`**

_Windows Only_

Configure fine-tuned lists for specific application behaviors. Can be called before `start()`. This allows you to customize how the selection hook behaves with certain applications that may have unique characteristics.

For example, you can add `acrobat.exe` to those lists to enable text seleted in Acrobat.

List types:

- `EXCLUDE_CLIPBOARD_CURSOR_DETECT`: Exclude cursor detection for clipboard operations
- `INCLUDE_CLIPBOARD_DELAY_READ`: Include delay when reading clipboard content

See `SelectionHook.FineTunedListType` constants for details.

#### **`setSelectionPassiveMode(passive: boolean): boolean`**

Set passive mode for selection (only triggered by getCurrentSelection, `text-selection` event will not be emitted). Can be called before `start()`.

#### **`writeToClipboard(text: string): boolean`**

Write text to the system clipboard. This is useful for implementing custom copy functions. Can be called before `start()`.

Not supported on Linux. Host applications should use their own clipboard API (e.g., Electron clipboard).

#### **`readFromClipboard(): string | null`**

Read text from the system clipboard. Returns clipboard text content as string, or null if clipboard is empty or contains non-text data. Can be called before `start()`.

Not supported on Linux. Host applications should use their own clipboard API (e.g., Electron clipboard).

#### **`macIsProcessTrusted(): boolean`**

_macOS Only_

Check if the process is trusted for accessibility. Can be called before `start()`. If the process is not trusted, selection-hook will still run, but it won't respond to any events. Make sure to guide the user through the authorization process before calling start().

#### **`macRequestProcessTrust(): boolean`**

_macOS Only_

Try to request accessibility permissions. Can be called before `start()`. This MAY show a dialog to the user if permissions are not granted.

Note: The return value indicates the current permission status, not the request result.

#### **`linuxGetEnvInfo(): LinuxEnvInfo | null`**

_Linux Only_

Get Linux environment information. Can be called before `start()`. Returns an object with display protocol, compositor type, input device access status, and root status. All values are detected once at construction time and cached. Returns `null` on non-Linux platforms.

```javascript
const info = hook.linuxGetEnvInfo();
// info = {
//   displayProtocol: 2,       // SelectionHook.DisplayProtocol.WAYLAND
//   compositorType: 1,        // SelectionHook.CompositorType.KWIN
//   hasInputDeviceAccess: true, // user can access input devices
//   isRoot: false
// }
```

See [`LinuxEnvInfo`](#linuxenvinfo) for the full structure, and [`SelectionHook.CompositorType`](#selectionhookcompositortype) for compositor constants.

#### **`isRunning(): boolean`**

Check if selection-hook is currently running.

#### **`cleanup()`**

Release resources and stop monitoring.

### Events

#### **`text-selection`**

Emitted when text is selected, see [`TextSelectionData`](#textselectiondata) for `data` structure

```javascript
hook.on("text-selection", (data: TextSelectionData) => {
  // data contains selection information
});
```

#### **`mouse-move`**, **`mouse-up`**, **`mouse-down`**

Mouse events, see [`MouseEventData`](#mouseeventdata) for `data` structure

```javascript
hook.on("mouse-XXX", (data: MouseEventData) => {
  // data contains mouse coordinates and button info
});
```

#### **`mouse-wheel`**

Mouse wheel events, see [`MouseWheelEventData`](#mousewheeleventdata) for `data` structure

```javascript
hook.on("mouse-wheel", (data: MouseWheelEventData) => {
  // data contains wheel direction info
});
```

#### **`key-down`**, **`key-up`**

Keyboard events, see [`KeyboardEventData`](#keyboardeventdata) for `data` structure

```javascript
hook.on("key-XXX", (data: KeyboardEventData) => {
  // data contains key code and modifier info
});
```

#### **`status`**

Hook status changes

```javascript
hook.on("status", (status: string) => {
  // status is a string, e.g. "started", "stopped"
});
```

#### **`error`**

Error events
Only display errors when `debug` set to `true` when `start()`

```javascript
hook.on("error", (error: Error) => {
  // error is an Error object
});
```

### Data Structure

**Note**: All coordinates are in physical coordinates (virtual screen coordinates) in Windows. You can use `screen.screenToDipPoint(point)` in Electron to convert the point to logical coordinates. In macOS and Linux X11, you don't need to do extra work. On Linux Wayland, coordinate accuracy depends on the compositor (see [Linux platform details](LINUX.md)).

#### `TextSelectionData`

Represents text selection information including content, source application, and coordinates.

| Property        | Type              | Description                                                   |
| --------------- | ----------------- | ------------------------------------------------------------- |
| `text`          | `string`          | The selected text content                                     |
| `programName`   | `string`          | Name of the application where selection occurred. Always empty on Linux Wayland. |
| `startTop`      | `Point`           | First paragraph's top-left coordinates (px)                   |
| `startBottom`   | `Point`           | First paragraph's bottom-left coordinates (px)                |
| `endTop`        | `Point`           | Last paragraph's top-right coordinates (px)                   |
| `endBottom`     | `Point`           | Last paragraph's bottom-right coordinates (px)                |
| `mousePosStart` | `Point`           | Mouse position when selection started (px)                    |
| `mousePosEnd`   | `Point`           | Mouse position when selection ended (px)                      |
| `method`        | `SelectionMethod` | Indicates which method was used to detect the text selection. |
| `posLevel`      | `PositionLevel`   | Indicates which positional data is provided.                  |
| `isFullscreen`  | `boolean`         | _macOS Only_ Whether the window is in fullscreen mode         |

Type `Point` is `{ x: number; y: number }`

**Linux coordinate availability:** On Linux, `startTop`/`startBottom`/`endTop`/`endBottom` are always `-99999` ([`INVALID_COORDINATE`](#selectionhookinvalid_coordinate)) because selection bounding rectangles are not available. On Linux Wayland, `mousePosStart`/`mousePosEnd` may also be `-99999` when the coordinate source (libevdev) cannot provide actual screen positions — see [Linux platform details](LINUX.md) for the compositor-dependent fallback chain.

When `PositionLevel` is:

- `MOUSE_SINGLE`：only `mousePosStart` and `mousePosEnd` is provided, and `mousePosStart` equals `mousePosEnd`
- `MOUSE_DUAL`: only `mousePosStart` and `mousePosEnd` is provided. On Linux Wayland, drag selection can achieve `MOUSE_DUAL` when the compositor provides accurate cursor positions at both mouse-down and mouse-up.
- `SEL_FULL`: all the mouse position and paragraph's coordinates are provided

#### `MouseEventData`

Contains mouse click/movement information in screen coordinates.

| Property | Type     | Description                                                                                                                     |
| -------- | -------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `x`      | `number` | Horizontal pointer position (px)                                                                                                |
| `y`      | `number` | Vertical pointer position (px)                                                                                                  |
| `button` | `number` | Same as WebAPIs' MouseEvent.button <br /> `0`=Left, `1`=Middle, `2`=Right, `3`=Back, `4`=Forward <br /> `-1`=None, `99`=Unknown |

On Linux Wayland, `x`/`y` may be `-99999` ([`INVALID_COORDINATE`](#selectionhookinvalid_coordinate)) because the input source (libevdev) cannot provide actual screen positions. See [Linux platform details](LINUX.md).

If `button != -1` when `mouse-move`, which means it's dragging.

#### `MouseWheelEventData`

Describes mouse wheel scrolling events.

| Property | Type     | Description                         |
| -------- | -------- | ----------------------------------- |
| `button` | `number` | `0`=Vertical, `1`=Horizontal scroll |
| `flag`   | `number` | `1`=Up/Right, `-1`=Down/Left        |

#### `KeyboardEventData`

Represents keyboard key presses/releases.

| Property   | Type      | Description                                                                 |
| ---------- | --------- | --------------------------------------------------------------------------- |
| `uniKey`   | `string`  | Unified key name, refer to MDN `KeyboardEvent.key`, converted from `vkCode` |
| `vkCode`   | `number`  | Virtual key code. Definitions and values vary by platform.                 |
| `sys`      | `boolean` | Whether modifier keys (Ctrl/Alt/Win/⌘/⌥/Super/Fn) are pressed simultaneously |
| `scanCode` | `number?` | Hardware scan code. (_Windows Only_)                                        |
| `flags`    | `number`  | Additional state flags. On Linux: modifier bitmask (0x01=Shift, 0x02=Ctrl, 0x04=Alt, 0x08=Meta) |
| `type`     | `string?` | Internal event type                                                         |
| `action`   | `string?` | `"key-down"` or `"key-up"`                                                  |

About vkCode:

- Windows: VK\_\* values of `vkCode`
- macOS: kVK\_\* values of `kCGKeyboardEventKeycode`
- Linux: KEY\_\* values from `<linux/input-event-codes.h>`

### Constants

#### **`SelectionHook.INVALID_COORDINATE`**

Sentinel value (`-99999`) indicating that a coordinate is unavailable or unreliable. On Linux Wayland, mouse event coordinates and selection position coordinates may be set to this value when the input source (libevdev) cannot provide actual screen positions. Check coordinate fields against `SelectionHook.INVALID_COORDINATE` before using them for UI positioning.

```javascript
if (data.mousePosEnd.x !== SelectionHook.INVALID_COORDINATE) {
  // Position is reliable, use it
}
```

#### **`SelectionHook.SelectionMethod`**

Indicates which method was used to detect the text selection:

- `NONE`: No selection detected
- `UIA`: UI Automation (_Windows_)
- `ACCESSIBLE`: Accessibility interface (_Windows_)
- `AXAPI`: Accessibility API (_macOS_)
- `ATSPI`: Assistive Technology Service Provider Interface (_Linux_, reserved — not currently used, reserved for future implementation)
- `PRIMARY`: Primary Selection (_Linux_)
- `CLIPBOARD`: Clipboard fallback

#### **`SelectionHook.PositionLevel`**

Indicates which positional data is provided:

- `NONE`: No position information
- `MOUSE_SINGLE`: Only single mouse position
- `MOUSE_DUAL`: Mouse start and end positions (when dragging to select)
- `SEL_FULL`: Full selection coordinates (see [`TextSelectionData`](#textselectiondata) structure for details). Not available on Linux.
- `SEL_DETAILED`: Detailed selection coordinates (reserved for future use)

#### **`SelectionHook.FilterMode`**

Before version `v0.9.16`, this variable was named `ClipboardMode`

- `DEFAULT`: The filter mode is disabled
- `INCLUDE_LIST`: Only the programs in list will pass the filter
- `EXCLUDE_LIST`: Only the progrmas NOT in in list will pass the filter

#### **`SelectionHook.FineTunedListType`**

_Windows Only_

Defines types for fine-tuned application behavior lists:

- `EXCLUDE_CLIPBOARD_CURSOR_DETECT`: Exclude cursor detection for clipboard operations. Useful for applications with custom cursors (e.g., Adobe Acrobat) where cursor shape detection may not work reliably.
- `INCLUDE_CLIPBOARD_DELAY_READ`: Include delay when reading clipboard content. Useful for applications that modify clipboard content multiple times in quick succession (e.g., Adobe Acrobat).

#### **`SelectionHook.DisplayProtocol`**

_Linux Only_

Defines the display protocol types used on Linux systems:

- `UNKNOWN`: No protocol detected or not applicable
- `X11`: X11 windowing system protocol
- `WAYLAND`: Wayland display server protocol

#### **`SelectionHook.CompositorType`**

_Linux Only_

Identifies the compositor. Values represent the **compositor**, not the desktop environment (DE). DE-bundled compositors are detected via `XDG_CURRENT_DESKTOP` (each DE uses exactly one compositor); standalone compositors are detected via their own environment variables.

| Constant | Compositor | Desktop Environment | Detected via | Cursor method |
|---|---|---|---|---|
| `UNKNOWN` | — | — | — | XWayland fallback |
| `KWIN` | KWin (`kwin_wayland`) | KDE Plasma | `XDG_CURRENT_DESKTOP` contains "KDE" | KWin Scripting DBus |
| `MUTTER` | mutter (`gnome-shell`) | GNOME | `XDG_CURRENT_DESKTOP` contains "GNOME" | XWayland fallback |
| `HYPRLAND` | Hyprland | (standalone) | `HYPRLAND_INSTANCE_SIGNATURE` env var | Native IPC socket |
| `SWAY` | sway | (standalone) | `SWAYSOCK` env var | XWayland fallback |
| `WLROOTS` | various (labwc, river, …) | (standalone) | `XDG_CURRENT_DESKTOP` contains "wlroots" | XWayland fallback |
| `COSMIC_COMP` | cosmic-comp | COSMIC (System76) | `XDG_CURRENT_DESKTOP` contains "COSMIC" | XWayland fallback |

#### `LinuxEnvInfo`

_Linux Only_

Returned by `linuxGetEnvInfo()`. Contains cached Linux environment information detected at construction time.

| Property | Type | Description |
|---|---|---|
| `displayProtocol` | `number` | Display protocol (`SelectionHook.DisplayProtocol`) |
| `compositorType` | `number` | Compositor type (`SelectionHook.CompositorType`) |
| `hasInputDeviceAccess` | `boolean` | Whether the user can access input devices (needed for Wayland libevdev input monitoring). Checks `input` group, ACLs, capabilities, and actual device access. Always `true` on X11. |
| `isRoot` | `boolean` | Whether the process is running as root |

### TypeScript Support

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
