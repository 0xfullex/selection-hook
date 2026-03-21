# Linux Protocol Implementations

**Part of [selection-hook](https://github.com/0xfullex/selection-hook)** — A Node.js native module for monitoring text selections across applications.

---

This directory contains the X11 and Wayland protocol implementations for selection-hook on Linux.

## Architecture

```
protocols/
├── x11.cc              # X11 protocol: XRecord (input) + XFixes (PRIMARY selection)
├── wayland.cc          # Wayland protocol: libevdev (input) + data-control (PRIMARY selection)
└── wayland/            # Pre-generated Wayland protocol C bindings
```

Selection text on Linux is obtained exclusively via **PRIMARY selection** — the text is available immediately when the user selects it (no Ctrl+C needed). This is fundamentally different from the Windows/macOS approach which uses UI Automation, Accessibility APIs, and clipboard fallback.

## Platform Limitations

### Common to All Linux (X11 & Wayland)

| Limitation | Details |
|---|---|
| **Clipboard read/write disabled** | `writeToClipboard()` and `readFromClipboard()` return false on Linux. X11's lazy clipboard model requires the owner to keep a window alive and respond to `SelectionRequest` events, which is unreliable in a library context. Host applications should use their own clipboard API (e.g., Electron's `clipboard` module). |
| **No clipboard fallback** | The Ctrl+C clipboard fallback mechanism (used on Windows/macOS as a last resort) is not implemented on Linux. Text is obtained solely via PRIMARY selection. |
| **No text range coordinates** | `startTop`, `startBottom`, `endTop`, `endBottom` are always `-99999` (`INVALID_COORDINATE`). Selection bounding rectangles are not available on Linux. `posLevel` will be `MOUSE_SINGLE` or `MOUSE_DUAL` at most, never `SEL_FULL`. |

### X11 Specific

| Feature | Status | Notes |
|---|---|---|
| Selection monitoring | ✅ Working | XFixes `SelectionNotify` on PRIMARY selection |
| Input events (mouse/keyboard) | ✅ Working | XRecord extension |
| Cursor position | ✅ Accurate | `XQueryPointer` — screen coordinates |
| Program name | ✅ Working | `WM_CLASS` property |
| Window rect | ✅ Working | `XGetWindowAttributes` + `XTranslateCoordinates` |

### Wayland Specific

| Feature | Status | Notes |
|---|---|---|
| Selection monitoring | ✅ Working | `ext-data-control-v1` or `wlr-data-control-unstable-v1 v2+` (see compositor table) |
| Input events (mouse/keyboard) | ✅ Working | libevdev on `/dev/input/event*` — requires `input` group membership |
| Cursor position | Compositor-dependent | See compositor compatibility table below |
| Program name | ❌ Always empty | Wayland security model does not expose window information |
| Window rect | ❌ Always unavailable | Wayland does not expose global window coordinates |

**Left-handed mouse support (Wayland only):**

On Wayland, libevdev reads raw physical button codes from `/dev/input/event*`, bypassing libinput's left-handed button swap. selection-hook monitors both `BTN_LEFT` and `BTN_RIGHT` for gesture detection (drag, double-click, shift+click), so left-handed users who swap mouse buttons via system settings will have selection detection work correctly with their primary (physical right) button. The existing gesture-selection correlation mechanism naturally filters out right-click context menu actions that don't produce text selections.

On X11, XRecord captures post-swap logical events, so left-handed mode works without any special handling.

**Input device access (Wayland only):**

Wayland's security model prevents applications from intercepting global input events via the display server. We use libevdev to read directly from `/dev/input/event*` devices, which requires the user to have access to these devices. The most common way is to join the `input` group:

```bash
sudo usermod -aG input $USER
# Then re-login for the change to take effect
```

Other methods that also grant access include systemd-logind ACLs (often set automatically for the active session), custom udev rules, and Linux capabilities. `hasInputDeviceAccess` checks all of these.

You can check whether the current user has input device access programmatically:

```javascript
const info = hook.linuxGetEnvInfo();
if (info && !info.hasInputDeviceAccess) {
  console.warn('User does not have input device access. Run: sudo usermod -aG input $USER');
}
```

**Fallback without input device access (Wayland):**

When input devices are not accessible, selection-hook falls back to **data-control debounce mode** (Path C). In this mode, text selection is detected solely via the Wayland data-control protocol events with a 300ms debounce. This means:

- Mouse/keyboard events will **not** be emitted
- Selection detection still works but with slightly higher latency (~300ms after the user finishes selecting)
- `posLevel` will be `MOUSE_SINGLE` (cursor position queried from compositor at the time of detection, or `-99999` if unavailable)
- `programName` remains empty (Wayland limitation)

### Wayland Compositor Compatibility

#### Selection Monitoring

Selection monitoring relies on Wayland data-control protocols. The library prefers `ext-data-control-v1` (standardized) and falls back to `wlr-data-control-unstable-v1 v2+` (wlroots-specific).

| Compositor | Protocol | Selection Monitoring |
|---|---|---|
| **KDE Plasma 5/6** (KWin) | wlr-data-control | ✅ Working |
| **Hyprland** | wlr-data-control | ✅ Working |
| **Sway** | wlr-data-control | ✅ Working |
| **wlroots-based** (labwc, river, etc.) | wlr-data-control | ✅ Working |
| **COSMIC** | ext-data-control | ✅ Working |
| **GNOME** (Mutter) | — | ❌ Not supported — Mutter does not implement data-control protocols |

#### Cursor Position

Wayland does not provide a standard API for global cursor position queries. We use compositor-specific IPC with a multi-level fallback chain:

1. **Compositor-native IPC** (if available)
2. **XWayland fallback** — `XQueryPointer` on the XWayland display
3. **libevdev accumulated** — device-relative deltas (last resort, inaccurate)

| Compositor | Method | Accuracy | Notes |
|---|---|---|---|
| **KDE Plasma 6** | ✅ KWin Scripting DBus | Accurate screen coordinates | Loads a JS script that reads `workspace.cursorPos` and calls back via DBus. Auto-detects per-script `run()` vs manager `start()` for different Plasma 6 builds |
| **KDE Plasma 5** | ✅ KWin Scripting DBus | Accurate screen coordinates | Same approach as Plasma 6, compatible with both KWin DBus API variants |
| **Hyprland** | ✅ Native IPC | Accurate screen coordinates | `hyprctl cursorpos` via Unix socket (`$HYPRLAND_INSTANCE_SIGNATURE`) |
| **Sway** | ⚠️ XWayland fallback | Partial | Coordinates may freeze when cursor is over native Wayland windows |
| **wlroots-based** (labwc, river, etc.) | ⚠️ XWayland fallback | Partial | Coordinates may freeze when cursor is over native Wayland windows |
| **COSMIC** | ⚠️ XWayland fallback | Partial | Coordinates may freeze when cursor is over native Wayland windows |
| **GNOME** (Mutter) | ⚠️ XWayland fallback | Partial | Coordinates may freeze when cursor is over native Wayland windows |

**XWayland fallback details:**
- Requires `DISPLAY` environment variable (XWayland must be running)
- Uses `XQueryPointer` on the XWayland X display
- Coordinates track correctly when the cursor is over XWayland windows, but may freeze at the last known position when the cursor moves over native Wayland windows
- If XWayland is unavailable, falls back to libevdev accumulated coordinates (device-relative, not screen coordinates)

**Coordinate unavailability (`INVALID_COORDINATE = -99999`):**

On Wayland, mouse event coordinates (`x`, `y`) come from libevdev hardware events (relative deltas or absolute hardware values) which do not represent actual screen positions. These coordinates are reported as `-99999` (`SelectionHook.INVALID_COORDINATE`) to clearly indicate they are unavailable. Always check coordinate fields against this sentinel value before using them for positioning.

For text selection events, the coordinate fallback chain works as follows:
- **Compositor IPC** (Hyprland, KDE): provides accurate screen coordinates → real values
- **XWayland**: provides accurate coordinates when cursor is over XWayland windows → real values
- **XWayland frozen**: detected when mouse-down and mouse-up queries return identical coordinates despite physical movement → `-99999`
- **No IPC, no XWayland**: libevdev values → `-99999`

For drag selections on Wayland, the library queries the compositor at both mouse-down and mouse-up, enabling `MOUSE_DUAL` position level when both queries succeed and coordinates differ (indicating the cursor actually moved between XWayland/compositor-tracked windows).

## API Behavior on Linux

The following APIs have different behavior on Linux compared to Windows/macOS:

| API | X11 | Wayland | Notes |
|---|---|---|---|
| `linuxGetEnvInfo()` | ✅ Returns env info | ✅ Returns env info | Can be called before `start()`. Returns `null` on non-Linux. Includes `displayProtocol`, `compositorType`, `hasInputDeviceAccess` (always `true` on X11), `isRoot` |
| `writeToClipboard()` | Returns `false` | Returns `false` | Blocked at JS layer. Use host app's clipboard API. |
| `readFromClipboard()` | Returns `null` | Returns `null` | Blocked at JS layer. Use host app's clipboard API. |
| `enableClipboard()` / `disableClipboard()` | No effect | No effect | Clipboard fallback not implemented on Linux |
| `setClipboardMode()` | No effect | No effect | Clipboard fallback not implemented on Linux |
| `setFineTunedList()` | No effect | No effect | Windows only |
| `setGlobalFilterMode()` | ✅ Works | ⚠️ Ineffective | `programName` is always empty on Wayland, so program-based filtering cannot match |
| `programName` in events | ✅ Via `WM_CLASS` | Always `""` | Wayland security model restriction |
| `startTop/startBottom/endTop/endBottom` | Always `-99999` | Always `-99999` | Selection bounding rectangles not available. Check against `INVALID_COORDINATE`. |
| `posLevel` | `MOUSE_SINGLE` or `MOUSE_DUAL` | `MOUSE_SINGLE` or `MOUSE_DUAL` | Wayland drag can achieve `MOUSE_DUAL` when compositor provides accurate positions at both mouse-down and mouse-up. Never reaches `SEL_FULL` on Linux. |
| `mousePosStart` / `mousePosEnd` | ✅ Screen coordinates | Compositor-dependent | May be `-99999` when unavailable. See compositor compatibility table. |

## Hint for Electron Applications

When using selection-hook in an **Electron** application on Wayland, it is recommended to run Electron in XWayland mode by adding the `--ozone-platform=x11` command line flag. This is because Electron itself has significant limitations under native Wayland:

- **`BrowserWindow.setPosition()` / `setBounds()`** — Not functional on Wayland. The Wayland protocol prohibits programmatically changing global window coordinates.
- **`BrowserWindow.getPosition()` / `getBounds()`** — Returns `[0, 0]` / `{x: 0, y: 0, ...}` on Wayland, as global window coordinates cannot be introspected.

These Electron-level restrictions make it difficult to implement features like positioning popup windows near selected text. Running under XWayland avoids these issues and also gives selection-hook accurate cursor coordinates via `XQueryPointer`.

> **Important:** `app.commandLine.appendSwitch('ozone-platform', 'x11')` does **NOT** work — ozone platform initialization occurs in Chromium's early startup, before the application JavaScript entry point executes. You must set this flag externally.

**Option 1** — Command line argument (recommended):

```bash
your-electron-app --ozone-platform=x11
```

**Option 2** — Wrapper script or `.desktop` file:

```ini
# In your .desktop file
Exec=your-electron-app --ozone-platform=x11 %U
```

**Option 3** — Environment variable (Electron < 38 only):

```bash
ELECTRON_OZONE_PLATFORM_HINT=x11 your-electron-app
```

> **Note:** Starting from Electron 38, the default `--ozone-platform` value is `auto`, meaning Electron will run as a native Wayland app in Wayland sessions. The `ELECTRON_OZONE_PLATFORM_HINT` environment variable has been removed in Electron 38 and will be ignored in Electron 39+. Use the `--ozone-platform=x11` command line flag instead.
