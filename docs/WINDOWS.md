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

1. **UI Automation (UIA)** — Preferred method. Reads selected text directly from the application's UI automation tree. Works with modern applications that implement the UIA text pattern. If the focused element does not hold the selection (e.g. a web page container with `tabindex` receives focus), ancestors are also checked for the selection.
2. **IAccessible** — Falls back to the older Windows accessibility interface for legacy applications.
3. **Clipboard Fallback** — Last resort. Simulates keyboard shortcuts to copy selected text to the clipboard, then reads it. See [Clipboard Fallback](#clipboard-fallback) for details.

The `method` field in [`TextSelectionData`](API.md#textselectiondata) tells you which method was used:

| Method Constant | Value | Meaning |
|---|---|---|
| `SelectionMethod.UIA` | `1` | UI Automation |
| `SelectionMethod.ACCESSIBLE` | `3` | IAccessible |
| `SelectionMethod.CLIPBOARD` | `99` | Clipboard fallback |

You do not need to handle these differently in most cases — the text content and coordinates are provided in the same format regardless of method.

Automatic `text-selection` events run this pipeline on a dedicated Windows MTA worker, so UIA, IAccessible, and clipboard fallback do not block the JavaScript main thread. Ordinary Node.js, Electron, and other Windows Node-API hosts use the same implementation; no Electron-specific process or API is required. To preserve existing event semantics, new low-level hook events remain suppressed while either automatic or synchronous retrieval is active.

---

## Clipboard Fallback

The clipboard fallback is the most complex part of the Windows implementation. It is **enabled by default** and activates only when both UIA and IAccessible fail to extract the selected text.

### How It Works

For applications outside `INCLUDE_CLIPBOARD_DELAY_READ`, the fallback uses the following restore-first transaction:

1. **Saves** the current clipboard content
2. **Simulates Ctrl+Insert** (safer — less likely to conflict with application shortcuts)
3. **Waits** a short period, checking if the clipboard changed
4. If no change, **simulates Ctrl+C** as a second attempt
5. **Waits** for the clipboard to change
6. **Reads** the new clipboard content
7. **Attempts to restore** the original clipboard content

The original clipboard is restored on a best-effort basis, including restoring an originally empty clipboard to empty. All formats that can be backed up are saved; unsupported or unreadable formats are skipped without refusing text retrieval. Those skipped formats cannot be restored.

Once the fallback inserts any copy-shortcut input, including a partial insertion, it always attempts to restore the original clipboard and still returns text already read. A user's Ctrl+C after injection can therefore be overwritten by restoration. This is the deliberate cost of restore-first behavior.

The only preservation exception requires an observed physical **Ctrl+C**, **Ctrl+X**, **Ctrl+Insert**, or **Shift+Delete** keydown that occurs during the clipboard transaction (after the fallback starts; keys pressed earlier, for example while accessibility APIs are still being tried, do not count) and before any injection, with zero copy-shortcut inputs inserted in the transaction, followed by a confirmed clipboard sequence change. These shortcuts must have no extra modifiers; injected shortcuts do not qualify. A candidate sequence must be newer than both the sequence at keydown and the sequence after clearing and closing the clipboard, accounting for sequence wraparound. The candidate write must remain stable when the clipboard is opened, read, and the transaction completes. Sequence changes caused by our own locked read do not invalidate an otherwise valid user copy. Without confirmed stable evidence, the original clipboard is restored.

Once such a physical shortcut is recognized, no further copy shortcuts are injected in that transaction, even if the sequence has not changed yet. Explicit `getCurrentSelection()` calls continue polling within the original wait budget and return the user's text when it arrives, including while Ctrl+C remains held.

Automatic retrieval checks whether Ctrl is held at three keyboard-interruption checkpoints: before the Ctrl+Insert stage starts, before the Ctrl+C stage starts, and before reading in the Ctrl+C stage. If Ctrl is held at a reached checkpoint, retrieval aborts, emits no `text-selection` event, and attempts to restore the original clipboard. If Ctrl is not held at the reached checkpoints, a physical copy/cut during the transaction and before any injection follows the same rule as an explicit call: it stops further injection, continues polling within the original budget, returns the user's text, and preserves the confirmed write. This includes Shift+Delete and Ctrl+C whose Ctrl has already been released. Any actual copy-shortcut injection still requires restoration as described above.

Ordinary keys, mouse clicks, scrolling, and context-menu copy do not qualify as evidence of a user copy. In particular, content copied through a context menu during the transaction may be overwritten by restoration. If hook input cannot be observed reliably, restoration remains the default.

Each clipboard open in this transaction has at most five attempts, separated by 5 ms after a failed attempt. Restoration continues with the remaining saved formats if one format fails, and failure does not discard text already read. `stop()` or teardown during a transaction still attempts restoration, unless a user copy was already confirmed. Persistent Windows API failures can leave the clipboard unrestored or only partly restored; full restoration is not guaranteed.

Clipboard transactions are serialized. If automatic fallback is using the clipboard, public clipboard methods return their existing failure value immediately instead of waiting for the worker.

The fallback also refuses any window created by the calling thread, because that thread would have to dispatch the simulated keypress while it is blocked waiting for the clipboard. See [Same-Process Text Selection](#same-process-text-selection).

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

Applications on this list retain the legacy clipboard path unchanged. The restore-first behavior above, including empty-clipboard restoration, physical copy/cut detection, and bounded open retries, does not apply to them.

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

### Synchronous `getCurrentSelection()`

`getCurrentSelection()` remains synchronous for API compatibility and uses the calling thread's UI Automation client. An accessibility provider that does not return from a synchronous UIA call can therefore still block that caller. The MTA worker isolates automatic retrieval; it is not a hard-cancellation boundary for this explicit synchronous API. Microsoft specifically warns that a client interacting with [its own UI on the UI thread](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading) can become very slow or stop responding, so this risk is highest for same-process selections.

### Same-Process Text Selection

Selecting text inside the host application itself is supported. Only the clipboard fallback is restricted.

Windows delivers a synthesized keystroke to the [message queue of the thread that created the target window](https://learn.microsoft.com/en-us/windows/win32/winmsg/about-messages-and-message-queues), and only that thread's message loop can dispatch it. The clipboard fallback simulates Ctrl+Insert/Ctrl+C and then blocks in a `Sleep()` poll loop waiting for the clipboard to change, so it cannot run on the thread that owns the target window.

- **Automatic `text-selection` events are unaffected.** They run on the MTA worker, which owns no windows, so the main thread remains free to dispatch the keystroke. This also matches Microsoft's guidance to [make UI Automation calls from a separate MTA thread that owns no windows](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading) when a client automates its own UI. A main thread busy with a long JavaScript task can still make the fallback time out, but it cannot deadlock.
- **`getCurrentSelection()` runs on the calling thread.** When the window under the mouse was created by that same thread, the clipboard fallback is refused and the call returns `null` immediately instead of blocking. UIA and IAccessible still run normally, so this only affects selections that no accessibility provider can extract.

The refusal matters most for Electron apps launched with `--disable-renderer-accessibility`, a workaround for a [Chromium accessibility crash](https://issues.chromium.org/issues/40809069). That flag disables the renderer's accessibility tree and makes both UIA and IAccessible fail, leaving the clipboard fallback as the only remaining path.

**Workaround:** run selection-hook in a separate process, using Electron's [`utilityProcess`](https://www.electronjs.org/docs/latest/api/utility-process) or `child_process`. The host's main thread is then free to process the simulated keypress while selection-hook waits for the clipboard change in its own process.
