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
 */
NSRunningApplication *GetFrontApp();

/**
 * Get the currently focused application
 */
AXUIElementRef GetAppElementFromFrontApp(NSRunningApplication *frontApp);

/**
 * Get the currently focused element within an application
 */
AXUIElementRef GetFocusedElementFromAppElement(AXUIElementRef appElement);

/**
 * Get the focused window from the frontmost application
 */
AXUIElementRef GetFrontWindowElementFromAppElement(AXUIElementRef appElement);

/**
 * Get program name (bundle identifier or process name) from the active
 * application
 */
bool GetProgramNameFromFrontApp(NSRunningApplication *frontApp, std::string &programName);

/**
 * Check if cursor is I-beam cursor by comparing hotSpot
 */
bool isIBeamCursor(NSCursor *cursor);

/**
 * Check if the current app's front window is in fullscreen mode
 */
bool IsWindowFullscreen(NSRunningApplication *frontApp);
