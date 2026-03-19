# Linux Protocol Implementations

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
| **No text range coordinates** | `startTop`, `startBottom`, `endTop`, `endBottom` are always `{x: 0, y: 0}`. Retrieving selection bounding rectangles is not implemented on Linux. `posLevel` will be `MOUSE_SINGLE` or `MOUSE_DUAL` at most, never `SEL_FULL`. |

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

**Input group requirement (Wayland only):**

Wayland's security model prevents applications from intercepting global input events via the display server. We use libevdev to read directly from `/dev/input/event*` devices, which requires the user to be in the `input` group:

```bash
sudo usermod -aG input $USER
# Then re-login for the change to take effect
```

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

## API Behavior on Linux

The following APIs have different behavior on Linux compared to Windows/macOS:

| API | X11 | Wayland | Notes |
|---|---|---|---|
| `writeToClipboard()` | Returns `false` | Returns `false` | Blocked at JS layer. Use host app's clipboard API. |
| `readFromClipboard()` | Returns `null` | Returns `null` | Blocked at JS layer. Use host app's clipboard API. |
| `enableClipboard()` / `disableClipboard()` | No effect | No effect | Clipboard fallback not implemented on Linux |
| `setClipboardMode()` | No effect | No effect | Clipboard fallback not implemented on Linux |
| `setFineTunedList()` | No effect | No effect | Windows only |
| `setGlobalFilterMode()` | ✅ Works | ⚠️ Ineffective | `programName` is always empty on Wayland, so program-based filtering cannot match |
| `programName` in events | ✅ Via `WM_CLASS` | Always `""` | Wayland security model restriction |
| `startTop/startBottom/endTop/endBottom` | Always `(0,0)` | Always `(0,0)` | Selection bounding rectangles not implemented |
| `posLevel` | `MOUSE_SINGLE` or `MOUSE_DUAL` | `MOUSE_SINGLE` or `MOUSE_DUAL` | Never reaches `SEL_FULL` on Linux |
| `mousePosStart` / `mousePosEnd` | ✅ Screen coordinates | Compositor-dependent | See compositor compatibility table |
