/**
 * Utility functions for text selection hook on macOS
 */

#import "utils.h"

/**
 * Check if string is empty after trimming whitespace
 */
bool IsTrimmedEmpty(const std::string &text)
{
    return std::all_of(text.cbegin(), text.cend(), [](unsigned char c) { return std::isspace(c); });
}

NSRunningApplication *GetFrontApp()
{
    // we need to run the run loop to update the frontmost application
    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate distantPast]];

    NSWorkspace *workspace = [NSWorkspace sharedWorkspace];
    return workspace.frontmostApplication;
}

/**
 * Get the currently focused application
 */
AXUIElementRef GetAppElementFromFrontApp(NSRunningApplication *frontApp)
{
    if (!frontApp)
        return nullptr;

    pid_t pid = frontApp.processIdentifier;

    AXUIElementRef appElement = AXUIElementCreateApplication(pid);

    return appElement;
}

/**
 * Get the currently focused element within an application
 */
AXUIElementRef GetFocusedElementFromAppElement(AXUIElementRef appElement)
{
    if (!appElement)
        return nullptr;

    AXUIElementRef focusedElement = nullptr;
    AXError error =
        AXUIElementCopyAttributeValue(appElement, kAXFocusedUIElementAttribute, (CFTypeRef *)&focusedElement);

    if (error != kAXErrorSuccess || !focusedElement)
    {
        return nullptr;
    }

    return focusedElement;
}

/**
 * Get the focused window from the frontmost application
 */
AXUIElementRef GetFrontWindowElementFromAppElement(AXUIElementRef appElement)
{
    if (!appElement)
        return nullptr;

    // Get the front window directly
    AXUIElementRef frontWindow = nullptr;
    AXError error = AXUIElementCopyAttributeValue(appElement, kAXFocusedWindowAttribute, (CFTypeRef *)&frontWindow);

    if (error == kAXErrorSuccess && frontWindow)
    {
        return frontWindow;
    }

    return nullptr;
}

/**
 * Get program name (bundle identifier) from the active application
 */
bool GetProgramNameFromFrontApp(NSRunningApplication *frontApp, std::string &programName)
{
    @autoreleasepool
    {
        if (!frontApp)
            return false;

        // Try to get bundle identifier first
        NSString *bundleId = frontApp.bundleIdentifier;
        if (bundleId && bundleId.length > 0)
        {
            programName = std::string([bundleId UTF8String]);
            return true;
        }

        return false;
    }
}

/**
 * Check if cursor is I-beam cursor by comparing hotSpot
 */
bool isIBeamCursor(NSCursor *cursor)
{
    if (!cursor)
        return false;

    NSPoint hotSpot = [cursor hotSpot];

    return NSEqualPoints(hotSpot, {4.0, 9.0}) || NSEqualPoints(hotSpot, {16.0, 16.0});
}