/**
 * Utility functions for text selection hook on macOS
 */

#import <ApplicationServices/ApplicationServices.h>
#import <Cocoa/Cocoa.h>

#import <string>

/**
 * Check if string is empty after trimming whitespace
 */
bool IsTrimmedEmpty(const std::string &text);

/**
 * Get the currently focused application
 * @return Borrowed (autoreleased) frontmost application, or nil.
 * @note Pumps the calling thread's NSRunLoop once so NSWorkspace reports the current
 * frontmost app instead of a cached one. Call it on the thread that owns the retrieval
 * (the JS thread today); do not call it from the event tap thread.
 */
NSRunningApplication *GetFrontApp();

/**
 * Create the accessibility element for an application
 * @param[in] frontApp Application whose pid is wrapped.
 * @return +1 AXUIElementRef; the caller must CFRelease it. nullptr when frontApp is nil.
 */
AXUIElementRef GetAppElementFromFrontApp(NSRunningApplication *frontApp);

/**
 * Get the currently focused element within an application
 * @return +1 AXUIElementRef (kAXFocusedUIElement); the caller must CFRelease it. nullptr
 * when the app reports no focused element, the process is not trusted for
 * accessibility, or the app does not respond.
 */
AXUIElementRef GetFocusedElementFromAppElement(AXUIElementRef appElement);

/**
 * Get the focused window from the frontmost application
 * @return +1 AXUIElementRef (kAXFocusedWindow); the caller must CFRelease it. Used as
 * the fallback root when there is no focused element.
 */
AXUIElementRef GetFrontWindowElementFromAppElement(AXUIElementRef appElement);

/**
 * Get program name (bundle identifier) from the active application
 * @return false when the app has no bundle identifier (command line tools, unsigned or
 * background processes). The pipeline treats that as "not supported".
 * @note programName is the bundle identifier, so filter lists on macOS match against
 * strings such as "com.google.Chrome", not executable names.
 */
bool GetProgramNameFromFrontApp(NSRunningApplication *frontApp, std::string &programName);

/**
 * Check if cursor is I-beam cursor by comparing hotSpot
 * @warning [NSCursor currentSystemCursor] returns a fresh object, so it cannot be
 * compared with [NSCursor IBeamCursor] by identity; the hot spot is the only stable
 * signal. A macOS release that changes the I-beam artwork changes the hot spot and
 * silently disables drag / double-click detection. Add the new value, keep the old
 * ones, and verify on every macOS version still supported.
 */
bool IsIBeamCursor(NSCursor *cursor);

/**
 * Check if the current app's front window is in fullscreen mode
 * @note Reads the "AXFullScreen" window attribute, which is not in the public headers
 * but is exposed by AppKit windows. Apps that do not report it yield false.
 */
bool IsWindowFullscreen(NSRunningApplication *frontApp);
