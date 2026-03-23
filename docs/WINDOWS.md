# Windows Platform Details

**Part of [selection-hook](https://github.com/0xfullex/selection-hook)** — A Node.js native module for monitoring text selections across applications.

---

## How Text Detection Works

Selection-hook uses a **three-tier fallback strategy** to extract selected text on Windows:

```
User selects text
    ↓
┌─────────────────────────┐
│ 1. UI Automation (UIA)  │  Modern apps (Chrome, Edge, VS Code, etc.)
└──────────┬──────────────┘
           ↓ fails
┌─────────────────────────┐
│ 2. IAccessible          │  Legacy apps (older Win32 applications)
└──────────┬──────────────┘
           ↓ fails
┌─────────────────────────┐
│ 3. Clipboard Fallback   │  Last resort — simulates Ctrl+C
└─────────────────────────┘
```

1. **UI Automation (UIA)** — Preferred method. Reads selected text directly from the application's UI automation tree. Works with modern applications that implement the UIA text pattern.
2. **IAccessible** — Falls back to the older Windows accessibility interface for legacy applications.
3. **Clipboard Fallback** — Last resort. Simulates keyboard shortcuts to copy selected text to the clipboard, then reads it. See [Clipboard Fallback](#clipboard-fallback) for details.

The `method` field in [`TextSelectionData`](API.md#textselectiondata) tells you which method was used:

| Method Constant | Value | Meaning |
|---|---|---|
| `SelectionMethod.UIA` | `1` | UI Automation |
| `SelectionMethod.ACCESSIBLE` | `3` | IAccessible |
| `SelectionMethod.CLIPBOARD` | `99` | Clipboard fallback |

You do not need to handle these differently in most cases — the text content and coordinates are provided in the same format regardless of method.

---

## Clipboard Fallback

The clipboard fallback is the most complex part of the Windows implementation. It is **enabled by default** and activates only when both UIA and IAccessible fail to extract the selected text.

### How It Works

1. **Saves** the current clipboard content
2. **Simulates Ctrl+Insert** (safer — less likely to conflict with application shortcuts)
3. **Waits** a short period, checking if the clipboard changed
4. If no change, **simulates Ctrl+C** as a second attempt
5. **Waits** for the clipboard to change
6. **Reads** the new clipboard content
7. **Restores** the original clipboard content

This process is designed to be non-destructive — the user's clipboard is saved and restored, so in the vast majority of cases, the clipboard operation is invisible to the user.

### Cursor Shape Detection

Before triggering the clipboard operation, selection-hook checks the **current cursor shape** to avoid unnecessary clipboard operations:

| Cursor Shape | Action |
|---|---|
| **I-beam** (text cursor) | Proceed with clipboard — likely a text selection |
| **Arrow** or **Hand** | Skip clipboard — likely not a text selection |
| **Custom cursor** | Depends on [fine-tuned list](#app-compatibility-setfinetunedlist) configuration |

This heuristic works well for most applications. However, some applications use **custom cursors** that do not match any standard shape, causing the clipboard fallback to be skipped even when the user has selected text. See [setFineTunedList()](#app-compatibility-setfinetunedlist) for how to handle these cases.

### Disabling Clipboard Fallback

If you want to guarantee zero clipboard interference:

```javascript
// Option 1: Disable at start
hook.start({ enableClipboard: false });

// Option 2: Disable at runtime
hook.disableClipboard();
```

When disabled, only UIA and IAccessible methods are used. Applications that rely solely on clipboard fallback will not have their selections detected.

---

## App Compatibility: setFineTunedList()

Some Windows applications have behaviors that interfere with the default clipboard fallback. `setFineTunedList()` provides per-application configuration to handle these edge cases.

### EXCLUDE_CLIPBOARD_CURSOR_DETECT (type 0)

**Problem:** Applications with custom cursors (e.g., Adobe Acrobat's PDF reading cursor, CAJViewer) cause cursor shape detection to skip the clipboard fallback, even though the user has selected text.

**Solution:** Add these applications to the `EXCLUDE_CLIPBOARD_CURSOR_DETECT` list to bypass cursor shape checking. The clipboard fallback will always be attempted for these applications.

```javascript
hook.setFineTunedList(
  SelectionHook.FineTunedListType.EXCLUDE_CLIPBOARD_CURSOR_DETECT,
  ["acrobat.exe", "cajviewer.exe"]
);
```

**When to use:** Text selection is not detected in an application, while other applications work fine. The application is known to use custom cursors.

### INCLUDE_CLIPBOARD_DELAY_READ (type 1)

**Problem:** Some applications modify the clipboard multiple times after a Ctrl+C operation (e.g., Adobe Acrobat writes plain text first, then overwrites with rich text). Reading the clipboard too early may return incomplete or intermediate content.

**Solution:** Add these applications to the `INCLUDE_CLIPBOARD_DELAY_READ` list. This adds an extra delay before reading the clipboard, allowing the application to finish writing.

```javascript
hook.setFineTunedList(
  SelectionHook.FineTunedListType.INCLUDE_CLIPBOARD_DELAY_READ,
  ["acrobat.exe"]
);
```

**When to use:** Selected text is detected but appears incomplete, truncated, or in an unexpected format. The application is known to perform multi-stage clipboard writes.

### Combining Both Lists

Applications like Adobe Acrobat may need both configurations:

```javascript
hook.setFineTunedList(
  SelectionHook.FineTunedListType.EXCLUDE_CLIPBOARD_CURSOR_DETECT,
  ["acrobat.exe", "cajviewer.exe"]
);

hook.setFineTunedList(
  SelectionHook.FineTunedListType.INCLUDE_CLIPBOARD_DELAY_READ,
  ["acrobat.exe"]
);
```

---

## Known Limitations

### Elevated (Administrator) Windows

Due to [User Interface Privilege Isolation (UIPI)](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/how-it-works), a non-elevated process cannot receive low-level hook events from windows running with administrator privileges. This means text selection is not detected when interacting with elevated applications (e.g., Task Manager, or apps launched via "Run as administrator").

**Workarounds:**
- **Focus monitoring (recommended):** Monitor global window focus change events at the application level to detect when focus moves to an elevated window, then dismiss the selection popup.
- **UIAccess:** A [UIAccess](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/user-account-control/how-it-works#uiaccess-for-ui-automation-applications) process can receive hook events across all integrity levels without running as administrator. Requires: `uiAccess="true"` in the executable manifest, a trusted digital signature, and installation in a secure location (`Program Files` or `Windows\System32`).

### Electron Same-Process Text Selection

When selection-hook runs on Electron's main thread, there is a latent risk with **same-process** text selection:

**Layer 1 — Clipboard fallback blocks the main thread:** The clipboard fallback simulates Ctrl+C and enters a blocking `Sleep()` poll loop waiting for the clipboard to change. If this runs on Electron's main thread, the main thread is blocked, so Electron cannot process the simulated keypress — resulting in a deadlock. This does not occur as long as UI Automation or IAccessible successfully extracts the text first.

**Layer 2 — `--disable-renderer-accessibility` forces the deadlock:** Some Electron apps use this flag to work around a [Chromium accessibility crash](https://issues.chromium.org/issues/40809069). However, it completely disables the renderer's accessibility tree, causing UI Automation and IAccessible to fail. This forces selection-hook into the clipboard fallback path, triggering the Layer 1 deadlock.

Text selection in **other applications** is not affected.

**Workaround:** Run selection-hook in a separate process using Electron's [`utilityProcess`](https://www.electronjs.org/docs/latest/api/utility-process) or `child_process`. This allows the Electron main thread to process simulated keypresses while selection-hook waits for the clipboard change in its own process.
