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
| Cursor position | ✅ Accurate | `XQueryPointer` — screen coordinates (see [Coordinate Systems](#coordinate-systems-and-hidpi-scaling)) |
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

When input devices are not accessible, selection-hook falls back to **data-control debounce mode** (Path C). In this mode, text selection is detected solely via the Wayland data-control protocol events with a short debounce interval. This means:

- Mouse/keyboard events will **not** be emitted
- Selection detection still works but with slightly higher latency (a short delay after the user finishes selecting)
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

Wayland's security model does not provide a standard API for querying global cursor position. To maximize coordinate availability, selection-hook uses every available method with a multi-level fallback chain:

1. **Compositor-native IPC** — asks the compositor directly (KDE, Hyprland)
2. **XWayland fallback** — `XQueryPointer` on the XWayland X display
3. **Unavailable** — returns `-99999` (`INVALID_COORDINATE`)

| Compositor | Method | Accuracy | Notes |
|---|---|---|---|
| **KDE Plasma 6** | ✅ KWin Scripting DBus | Accurate | Loads a JS script that reads `workspace.cursorPos` and calls back via DBus. Auto-detects per-script `run()` vs manager `start()` for different Plasma 6 builds |
| **KDE Plasma 5** | ✅ KWin Scripting DBus | Accurate | Same approach as Plasma 6, compatible with both KWin DBus API variants |
| **Hyprland** | ✅ Native IPC | Accurate | `hyprctl cursorpos` via Unix socket (`$HYPRLAND_INSTANCE_SIGNATURE`) |
| **Sway** | ⚠️ XWayland fallback | Partial | Coordinates may freeze when cursor is over native Wayland windows |
| **wlroots-based** (labwc, river, etc.) | ⚠️ XWayland fallback | Partial | Coordinates may freeze when cursor is over native Wayland windows |
| **COSMIC** | ⚠️ XWayland fallback | Partial | Coordinates may freeze when cursor is over native Wayland windows |
| **GNOME** (Mutter) | ⚠️ XWayland fallback | Partial | Coordinates may freeze when cursor is over native Wayland windows |

**Compositor IPC vs XWayland — different coverage:**

- **Compositor IPC** queries the compositor itself, so it works regardless of which window the cursor is on — XWayland windows, native Wayland windows, desktop, panel, etc.
- **XWayland `XQueryPointer`** only receives pointer events when the cursor is over XWayland windows. When the cursor moves to a native Wayland window, XWayland stops receiving pointer updates and `XQueryPointer` returns the last known position (frozen).

This is why compositor IPC is preferred when available — it provides accurate coordinates globally.

**XWayland fallback details:**
- Requires `DISPLAY` environment variable (XWayland must be running)
- Coordinates track correctly when the cursor is over XWayland windows, but may freeze at the last known position when the cursor moves over native Wayland windows
- If XWayland is unavailable, coordinates are reported as `-99999`

**Coordinate unavailability (`INVALID_COORDINATE = -99999`):**

On Wayland, mouse event coordinates (`x`, `y`) come from libevdev hardware events (relative deltas or absolute hardware values) which do not represent actual screen positions. These coordinates are always reported as `-99999` (`SelectionHook.INVALID_COORDINATE`). Always check coordinate fields against this sentinel value before using them for positioning.

For text selection events, the coordinate fallback chain works as follows:
- **Compositor IPC** (Hyprland, KDE): accurate coordinates → real values
- **XWayland**: accurate coordinates when cursor is over XWayland windows → real values
- **XWayland frozen**: detected when mouse-down and mouse-up queries return identical coordinates despite physical movement → `-99999`
- **No IPC, no XWayland**: → `-99999`

For drag selections on Wayland, the library queries the compositor at both mouse-down and mouse-up, enabling `MOUSE_DUAL` position level when both queries succeed and coordinates differ (indicating the cursor actually moved between XWayland/compositor-tracked windows).

## Coordinate Systems and HiDPI Scaling

selection-hook returns **screen coordinates** on all platforms — the raw values from the display system. On standard (1x) displays, screen coordinates equal logical coordinates. On HiDPI displays with scaling, you may need to convert to **logical coordinates (DIP)** for correct UI positioning.

### X11

On X11, screen coordinates come from `XQueryPointer` — the cursor position relative to the root window in the X server's coordinate space. Whether these match logical coordinates depends on how scaling is configured:

| Scaling method | Screen coordinate range | Example (1920×1080 native display) |
|---|---|---|
| No scaling (100%) | Same as native resolution = logical | 0–1920, 0–1080 |
| `xrandr --scale` (e.g., 2×2) | Scaled virtual resolution (larger than native) | 0–3840, 0–2160 |
| `Xft.dpi` only (e.g., 192) | Same as native resolution | 0–1920, 0–1080 |
| KDE app-level scaling (`QT_SCREEN_SCALE_FACTORS`) | Same as native resolution | 0–1920, 0–1080 |

- **`xrandr --scale`** changes the X11 virtual resolution. GNOME on X11 uses this for fractional scaling. Screen coordinates are in the enlarged virtual space.
- **`Xft.dpi`** and **app-level scaling** (`GDK_SCALE`, `QT_SCREEN_SCALE_FACTORS`) only affect how applications render — the X11 coordinate space stays the same.
- Desktop environments often combine multiple methods (e.g., GNOME uses `xrandr --scale` + `Xft.dpi` together).

In all cases, Electron's `screen.screenToDipPoint()` correctly converts these screen coordinates to logical coordinates (DIP).

### Wayland

As described [above](#cursor-position), the two available cursor position sources on Wayland return coordinates in **different coordinate spaces**:

- **Compositor IPC** (KDE, Hyprland): returns **logical coordinates** — the compositor's own DPI-independent coordinate space
- **XWayland `XQueryPointer`**: returns **screen coordinates** — the XWayland X11 server's coordinate space, which depends on how the compositor configures XWayland scaling

On HiDPI displays, these two spaces can differ. For example, on KDE at 150% with a 3840×2160 display, the compositor's logical space is 2560×1440 while XWayland's screen space is 3840×2160. If selection-hook returned these different coordinate spaces as-is, consumers would get inconsistent coordinates depending on which source was used internally.

To solve this, selection-hook **normalizes all Wayland coordinates to screen coordinates** (the XWayland X11 coordinate space). Compositor IPC logical coordinates are automatically multiplied by the detected XWayland scale factor to match the XWayland screen space. The scale factor is computed using the same signals that Electron/Chromium reads: `Xft.dpi` and `GDK_SCALE`.

| Source | Raw coordinates | After normalization |
|---|---|---|
| Compositor IPC (KDE, Hyprland) | Logical coordinates | × xwayland_scale → screen coordinates |
| XWayland `XQueryPointer` | Screen coordinates | No conversion needed |

When there is no HiDPI scaling (or compositor upscaling mode is used), `xwayland_scale = 1.0` and the normalization is a no-op.

This ensures that on Wayland, coordinates behave the same as on X11 — consumers can uniformly use `screen.screenToDipPoint()` to convert to logical coordinates (DIP), regardless of whether the session is X11 or Wayland.

### Converting screen coordinates to logical coordinates (DIP)

#### In Electron

Electron provides `screen.screenToDipPoint(point)` on **Windows and Linux** (not available on macOS — macOS screen coordinates are already logical). When using `--ozone-platform=x11` (recommended for Electron on Wayland), this function correctly converts coordinates from both X11 and Wayland sessions:

- **X11 session:** Converts screen coordinates to logical coordinates (DIP) using the detected scale factor
- **Wayland session with `--ozone-platform=x11`:** selection-hook returns coordinates in XWayland screen space (after internal conversion), and `screenToDipPoint()` converts them to DIP using the same scale factor

Recommended cross-platform pattern:

```javascript
const { screen } = require("electron");

function getLogicalPoint(point) {
  if (point.x === SelectionHook.INVALID_COORDINATE) return null;
  if (process.platform === "darwin") {
    return point; // macOS: screen coordinates are already logical
  }
  // Windows & Linux: convert screen coordinates → logical coordinates (DIP)
  // Works uniformly across X11 and Wayland sessions
  return screen.screenToDipPoint(point);
}
```

> **Note:** `screen.screenToDipPoint()` was added for Linux in Electron 35.3.0. On macOS, the method is `undefined` — do not call it unconditionally across all platforms.

#### Outside Electron (reference)

For non-Electron environments on X11, you can compute logical coordinates manually. Electron/Chromium determines the scale factor as:

```
scale_factor = gdk_monitor_get_scale_factor × (Xft.dpi / 96.0)
```

Then converts per-display:

```
logical_point = display_dip_origin + (screen_point - display_screen_origin) / scale_factor
```

Where `display_screen_origin` is the display's origin in X11 screen coordinate space (from XRandR), and `display_dip_origin` is the display's origin in logical coordinate space. For single-monitor setups, both origins are typically `(0, 0)`, simplifying the formula to `logical_point = screen_point / scale_factor`.

On Wayland, selection-hook already converts compositor IPC coordinates to XWayland screen space internally. The same `screen_point / scale_factor` formula applies when using `--ozone-platform=x11`.

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
| `mousePosStart` / `mousePosEnd` | ✅ Screen coordinates | Compositor-dependent | May be `-99999` when unavailable. See compositor compatibility table and [Coordinate Systems](#coordinate-systems-and-hidpi-scaling). |

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
