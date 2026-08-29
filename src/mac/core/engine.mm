/**
 * Text selection retrieval engine for macOS
 *
 * Owns the Accessibility API (AXAPI) and clipboard fallback pipeline that
 * previously lived in selection_hook.mm. It holds no N-API state; see engine.h
 * for the contract.
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */

#import "engine.h"

#import <algorithm>
#import <chrono>
#import <string>
#import <thread>

#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>

#import "../lib/clipboard.h"
#import "../lib/utils.h"

/**
 * Check if program name is in the filter list
 *
 * Case-insensitive substring match (not equality): a list entry "chrome" matches the
 * bundle id "com.google.Chrome". Entries are already lowercased by SetClipboardMode /
 * SetGlobalFilterMode in selection_hook.mm; only programName is lowercased here.
 */
bool SelectionEngine::IsInFilterList(const std::string &programName, const std::vector<std::string> &filterList) const
{
    // If filter list is empty, allow all
    if (filterList.empty())
        return false;

    // Convert program name to lowercase for case-insensitive comparison
    std::string lowerProgramName = programName;
    std::transform(lowerProgramName.begin(), lowerProgramName.end(), lowerProgramName.begin(), ::tolower);

    // Check if program name is in the filter list
    for (const auto &filterItem : filterList)
    {
        if (lowerProgramName.find(filterItem) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

/**
 * Get selected text from the active application using multiple methods
 */
bool SelectionEngine::GetSelectedText(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo,
                                      const SelectionConfigSnapshot &config, int64_t *endingClipboardSequence)
{
    // Per-call context. The clipboard sequence is retrieval-local until it is
    // published back to the owner through endingClipboardSequence.
    config_ = &config;
    clipboardSequence_ = config.clipboardSequence;

    if (!frontApp)
    {
        if (endingClipboardSequence)
            *endingClipboardSequence = clipboardSequence_;
        return false;
    }

    // Initialize structure
    selectionInfo.clear();

    if (!GetProgramNameFromFrontApp(frontApp, selectionInfo.programName))
    {
        // if bunldeId is not found,
        // Could be system process, command line tool, background app or unsigned app
        // We don't handle these cases
        if (endingClipboardSequence)
            *endingClipboardSequence = clipboardSequence_;
        return false;
    }
    // should filter by global filter list
    else if (config.globalFilterMode != FilterMode::Default)
    {
        bool isIn = IsInFilterList(selectionInfo.programName, config.globalFilterList);

        if ((config.globalFilterMode == FilterMode::IncludeList && !isIn) ||
            (config.globalFilterMode == FilterMode::ExcludeList && isIn))
        {
            if (endingClipboardSequence)
                *endingClipboardSequence = clipboardSequence_;
            return false;
        }
    }

    bool result = false;
    // First try Accessibility API (supported by modern applications)
    if (GetTextViaAXAPI(frontApp, selectionInfo))
    {
        selectionInfo.method = SelectionMethod::AXAPI;
        result = true;
    }

    // Last resort: try to get text using clipboard and Cmd+C if enabled
    if (!result && ShouldProcessViaClipboard(selectionInfo.programName) && GetTextViaClipboard(frontApp, selectionInfo))
    {
        selectionInfo.method = SelectionMethod::Clipboard;
        result = true;
    }

    if (endingClipboardSequence)
        *endingClipboardSequence = clipboardSequence_;

    if (result)
        selectionInfo.isFullscreen = IsWindowFullscreen(frontApp);

    return result;
}

/**
 * Get text selection via Accessibility API
 */
bool SelectionEngine::GetTextViaAXAPI(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo)
{
    if (!frontApp)
    {
        return false;
    }

    AXUIElementRef appElement = GetAppElementFromFrontApp(frontApp);
    if (!appElement)
        return false;  // CFRelease(NULL) below would abort

    AXUIElementRef focusedElement = GetFocusedElementFromAppElement(appElement);

    // if we can't find focusedElement, we'll fallback to find focusedWindow
    if (!focusedElement)
    {
        focusedElement = GetFrontWindowElementFromAppElement(appElement);

        if (!focusedElement)
        {
            CFRelease(appElement);
            return false;
        }
    }

    bool result = false;

    // Strategy 1: Try to get selected text from the focused element
    std::string selectedText;
    if (GetSelectedTextFromElement(focusedElement, selectedText))
    {
        if (!selectedText.empty() && !IsTrimmedEmpty(selectedText))
        {
            selectionInfo.text = selectedText;

            // Try to get selection bounds and set coordinates in selectionInfo
            if (!SetTextRangeCoordinates(focusedElement, selectionInfo))
            {
                // No position information available, but we have text
                selectionInfo.posLevel = SelectionPositionLevel::None;
            }

            result = true;
        }
    }

    // Strategy 2: If the focused element doesn't have selected text, try to traverse child elements
    if (!result)
    {
        CFArrayRef children = nullptr;
        AXError error = AXUIElementCopyAttributeValue(focusedElement, kAXChildrenAttribute, (CFTypeRef *)&children);

        if (error == kAXErrorSuccess && children)
        {
            CFIndex childCount = CFArrayGetCount(children);

            // Traverse child elements to find text selection
            for (CFIndex i = 0; i < childCount && !result; i++)
            {
                AXUIElementRef child = (AXUIElementRef)CFArrayGetValueAtIndex(children, i);
                if (child)
                {
                    std::string childText;
                    if (GetSelectedTextFromElement(child, childText))
                    {
                        if (!childText.empty() && !IsTrimmedEmpty(childText))
                        {
                            selectionInfo.text = childText;

                            // Try to get selection bounds from child and set coordinates in selectionInfo
                            if (!SetTextRangeCoordinates(child, selectionInfo))
                            {
                                selectionInfo.posLevel = SelectionPositionLevel::None;
                            }

                            result = true;
                        }
                    }
                }
            }

            CFRelease(children);
        }
    }

    // Strategy 3: Walk up the ancestor chain and retry
    // In web browsers (Chromium), a container with tabindex receives AX focus, and a selection
    // spanning multiple child nodes (e.g. across table rows) is only reported by an ancestor
    // (usually the AXWebArea). See issue #18.
    if (!result)
    {
        // The web area is usually within a few levels above the focused element
        const int maxWalkUpLevels = 10;

        AXUIElementRef current = focusedElement;
        CFRetain(current);

        for (int level = 0; level < maxWalkUpLevels && !result; level++)
        {
            AXUIElementRef parent = nullptr;
            AXError error = AXUIElementCopyAttributeValue(current, kAXParentAttribute, (CFTypeRef *)&parent);
            CFRelease(current);
            current = nullptr;

            if (error != kAXErrorSuccess || !parent)
                break;

            current = parent;

            std::string ancestorText;
            if (GetSelectedTextFromElement(current, ancestorText))
            {
                if (!ancestorText.empty() && !IsTrimmedEmpty(ancestorText))
                {
                    selectionInfo.text = ancestorText;

                    // Try to get selection bounds from the ancestor and set coordinates in selectionInfo
                    if (!SetTextRangeCoordinates(current, selectionInfo))
                    {
                        selectionInfo.posLevel = SelectionPositionLevel::None;
                    }

                    result = true;
                }
            }
        }

        if (current)
            CFRelease(current);
    }

    // if we can't get text by AXAPI, we have to do final try for special cases
    // Chromium-based browsers and Electron apps do not build their accessibility tree
    // until an assistive client asks for it. Setting these app-level attributes turns
    // the tree on; it helps the NEXT retrieval, not this one (this call already failed).
    // Side effect documented in docs/GUIDE.md: Chrome builds its full AX tree, which
    // costs memory and some rendering performance. The attributes are set on every
    // failed retrieval (idempotent, cheap), not only once per application.
    // Removing this block silently breaks Chrome, Edge, Electron, and VS Code support.
    if (!result)
    {
        // Chrome/Chromium: set "AXEnhancedUserInterface" to true to enable AXAPI
        AXUIElementSetAttributeValue(appElement, CFSTR("AXEnhancedUserInterface"), kCFBooleanTrue);
        // Electron Apps: set "AXManualAccessibility" to true to enable AXAPI
        AXUIElementSetAttributeValue(appElement, CFSTR("AXManualAccessibility"), kCFBooleanTrue);
    }

    // Clean up
    CFRelease(focusedElement);
    CFRelease(appElement);

    return result;
}

/**
 * Get text using clipboard and Cmd+C as a last resort
 *
 * Transaction: (1) if the pasteboard already changed since mouse-down, the user copied
 * something themselves and it is read directly; (2) otherwise back up ALL pasteboard
 * items, post Cmd+C to the target pid, poll changeCount, read, and restore.
 *
 * Invariants for anyone editing this function:
 * - Every exit after SendCopyKey() must restore the backup when HasData() and then
 *   refresh clipboardSequence_ from GetClipboardSequence(), because the restore itself
 *   bumps changeCount. Missing either makes the next gesture treat our own restore as a
 *   user copy and return stale pasteboard text.
 * - The poll below runs on the JS thread (std::this_thread::sleep_for). Its 10 x 10 ms
 *   budget is the upper bound on how long a gesture can block Node/Electron; do not
 *   lengthen it without moving retrieval off the JS thread.
 */
bool SelectionEngine::GetTextViaClipboard(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo)
{
    // currently, we don't support clipboard of the same process, we will fix it in future time
    // Reason: Cmd+C is posted to the target while this thread sleeps in the poll loop.
    // When the target is our own process, the JS thread that must handle the key event
    // is the one blocked here, so the copy can never happen (same shape as the Windows
    // issue #17 deadlock). Rejecting early avoids a guaranteed 100 ms stall.
    if (frontApp.processIdentifier == config_->runningPid)
        return false;

    int64_t newClipboardSequence = GetClipboardSequence();

    // Check if clipboard sequence number has changed since mouse down
    // if it's changed, it means user has copied something, we can read it directly
    if (newClipboardSequence != clipboardSequence_)
    {
        // Read the new clipboard content
        std::string newContent;
        if (ReadClipboard(newContent))
        {
            selectionInfo.text = newContent;
            return true;
        }
    }

    // no new copied message or read clipboard failed, we need to send Cmd+C to copy
    clipboardSequence_ = newClipboardSequence;
    // Store ALL current clipboard content to restore later (all formats, not just text)
    ClipboardBackup clipboardBackup = BackupClipboard();

    // Send Cmd+C to copy selected text
    if (!SendCopyKey(frontApp.processIdentifier))
        return false;

    // Check clipboard sequence number in a loop with 10ms interval
    // This gives the copy operation up to 100ms to complete
    bool clipboardChanged = false;
    for (int i = 0; i < 10; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        newClipboardSequence = GetClipboardSequence();
        if (newClipboardSequence != clipboardSequence_)
        {
            clipboardChanged = true;
            break;
        }
    }

    if (!clipboardChanged)
    {
        if (clipboardBackup.HasData())
        {
            RestoreClipboard(clipboardBackup);
            clipboardSequence_ = GetClipboardSequence();
        }
        return false;
    }

    // Read the new clipboard content
    std::string newContent;
    if (!ReadClipboard(newContent) || IsTrimmedEmpty(newContent))
    {
        // Restore original clipboard if possible (all formats)
        if (clipboardBackup.HasData())
        {
            RestoreClipboard(clipboardBackup);
            clipboardSequence_ = GetClipboardSequence();
        }

        return false;
    }

    // Store the copied text
    selectionInfo.text = newContent;

    // Restore original clipboard content (all formats)
    if (clipboardBackup.HasData())
    {
        RestoreClipboard(clipboardBackup);
        clipboardSequence_ = GetClipboardSequence();
    }

    return true;
}

/**
 * Check if we should process GetTextViaClipboard
 */
bool SelectionEngine::ShouldProcessViaClipboard(const std::string &programName) const
{
    if (!config_->clipboardEnabled)
        return false;

    bool result = false;
    switch (config_->clipboardFilterMode)
    {
        case FilterMode::Default:
            result = true;
            break;
        case FilterMode::IncludeList:
            result = IsInFilterList(programName, config_->clipboardFilterList);
            break;
        case FilterMode::ExcludeList:
            result = !IsInFilterList(programName, config_->clipboardFilterList);
            break;
    }

    if (!result)
        return false;

    return true;
}

/**
 * Send copy key combination (Cmd+C) using Core Graphics events
 *
 * @param pid The process ID to send the event to. If 0 (default not set),
 * the event is posted to the HID event tap (kCGHIDEventTap), i.e. it goes to
 * whatever application is frontmost, not to the current process.
 *
 * Requires accessibility permission for CGEventPostToPid to reach another
 * process. The key-down/key-up pair is separated by a short sleep so the target
 * sees a distinct press and release; do not collapse it into one post.
 */
bool SelectionEngine::SendCopyKey(pid_t pid) const
{
    // Create key down event for Cmd+C
    CGEventRef keyDownEvent = CGEventCreateKeyboardEvent(nullptr, kVK_ANSI_C, true);
    if (!keyDownEvent)
    {
        return false;
    }

    // Set Command key modifier flag
    CGEventSetFlags(keyDownEvent, kCGEventFlagMaskCommand);

    // Create key up event for Cmd+C
    CGEventRef keyUpEvent = CGEventCreateKeyboardEvent(nullptr, kVK_ANSI_C, false);
    if (!keyUpEvent)
    {
        CFRelease(keyDownEvent);
        return false;
    }

    // Set Command key modifier flag for key up event as well
    CGEventSetFlags(keyUpEvent, kCGEventFlagMaskCommand);

    // TODO Not to blink the EDIT menu
    // A Cmd+C delivered through CGEventPostToPid is handled as a menu key equivalent by
    // AppKit apps, which briefly highlights the Edit menu title. No fix known yet.
    if (pid != 0)
    {
        CGEventPostToPid(pid, keyDownEvent);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CGEventPostToPid(pid, keyUpEvent);
    }
    else
    {
        // Post the events to the system
        CGEventPost(kCGHIDEventTap, keyDownEvent);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CGEventPost(kCGHIDEventTap, keyUpEvent);
    }

    // Clean up
    CFRelease(keyDownEvent);
    CFRelease(keyUpEvent);

    return true;
}

/**
 * Get text selection from an accessibility element
 *
 * Two sources, in order:
 * 1. kAXSelectedText - most apps. CFNumber is accepted because numeric fields and
 *    steppers report their selection as a number rather than a string.
 * 2. kAXValue sliced by kAXSelectedTextRange - for providers that expose the full text
 *    and a range but not kAXSelectedText.
 * Returns true only for a non-empty result; blank-only text is filtered by the caller.
 */
bool SelectionEngine::GetSelectedTextFromElement(AXUIElementRef element, std::string &text)
{
    if (!element)
    {
        return false;
    }

    // Try to get selected text first
    CFTypeRef selectedTextRef = nullptr;
    AXError error = AXUIElementCopyAttributeValue(element, kAXSelectedTextAttribute, &selectedTextRef);

    if (error == kAXErrorSuccess && selectedTextRef)
    {
        CFTypeID typeID = CFGetTypeID(selectedTextRef);

        // Handle CFStringRef (most common case)
        if (typeID == CFStringGetTypeID())
        {
            CFStringRef selectedText = (CFStringRef)selectedTextRef;
            CFIndex length = CFStringGetLength(selectedText);
            if (length > 0)
            {
                CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
                char *buffer = new char[maxSize];

                if (CFStringGetCString(selectedText, buffer, maxSize, kCFStringEncodingUTF8))
                {
                    text = std::string(buffer);
                    delete[] buffer;
                    CFRelease(selectedTextRef);
                    return !text.empty();
                }

                delete[] buffer;
            }
        }
        // Handle CFNumberRef (for numeric input fields)
        else if (typeID == CFNumberGetTypeID())
        {
            CFNumberRef number = (CFNumberRef)selectedTextRef;

            if (CFNumberIsFloatType(number))
            {
                double doubleValue;
                if (CFNumberGetValue(number, kCFNumberDoubleType, &doubleValue))
                {
                    text = std::to_string(doubleValue);
                    CFRelease(selectedTextRef);
                    return !text.empty();
                }
            }
            else
            {
                long longValue;
                if (CFNumberGetValue(number, kCFNumberLongType, &longValue))
                {
                    text = std::to_string(longValue);
                    CFRelease(selectedTextRef);
                    return !text.empty();
                }
            }
        }
        // For unsupported types, just release and continue to next strategy
        CFRelease(selectedTextRef);
    }

    // If no selected text, try to get value and check if there's a selection range
    CFTypeRef valueRef = nullptr;
    error = AXUIElementCopyAttributeValue(element, kAXValueAttribute, &valueRef);

    if (error == kAXErrorSuccess && valueRef)
    {
        CFTypeID valueTypeID = CFGetTypeID(valueRef);

        // Convert different types to string for range processing
        if (valueTypeID == CFStringGetTypeID())
        {
            CFStringRef value = (CFStringRef)valueRef;
            //  Try to get selected text range
            CFRange selectedRange = {0, 0};
            AXValueRef rangeValue = nullptr;
            error = AXUIElementCopyAttributeValue(element, kAXSelectedTextRangeAttribute, (CFTypeRef *)&rangeValue);

            if (error == kAXErrorSuccess && rangeValue)
            {
                if (AXValueGetValue(rangeValue, kAXValueTypeCFRange, &selectedRange))
                {
                    // Validate and adjust the range bounds to prevent crashes
                    // Some providers report a kAXSelectedTextRange that lies partly or
                    // wholly outside kAXValue. CFStringCreateWithSubstring aborts the
                    // process on an out-of-range CFRange, so clamp first.
                    CFIndex valueLength = CFStringGetLength(value);

                    // Handle empty string case - no valid selection possible
                    if (valueLength <= 0)
                    {
                        CFRelease(rangeValue);
                        CFRelease(valueRef);
                        return false;
                    }

                    // Ensure location is within bounds
                    if (selectedRange.location < 0)
                    {
                        selectedRange.location = 0;
                    }
                    else if (selectedRange.location >= valueLength)
                    {
                        // For non-empty strings, clamp to last valid position
                        selectedRange.location = valueLength - 1;
                    }

                    // Ensure length is positive and doesn't exceed remaining string
                    if (selectedRange.length <= 0)
                    {
                        CFRelease(rangeValue);
                        CFRelease(valueRef);
                        return false;
                    }

                    if (selectedRange.location + selectedRange.length > valueLength)
                    {
                        selectedRange.length = valueLength - selectedRange.location;
                    }

                    // Final check: ensure we have a valid range
                    if (selectedRange.length <= 0)
                    {
                        CFRelease(rangeValue);
                        CFRelease(valueRef);
                        return false;
                    }

                    // Only proceed if we still have a valid range after adjustment
                    //  Extract selected substring
                    CFStringRef selectedSubstring =
                        CFStringCreateWithSubstring(kCFAllocatorDefault, value, selectedRange);
                    if (selectedSubstring)
                    {
                        CFIndex length = CFStringGetLength(selectedSubstring);
                        CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
                        char *buffer = new char[maxSize];

                        if (CFStringGetCString(selectedSubstring, buffer, maxSize, kCFStringEncodingUTF8))
                        {
                            text = std::string(buffer);
                            delete[] buffer;
                            CFRelease(selectedSubstring);
                            CFRelease(rangeValue);
                            CFRelease(valueRef);
                            return !text.empty();
                        }

                        delete[] buffer;
                        CFRelease(selectedSubstring);
                    }
                }
                CFRelease(rangeValue);
            }

            CFRelease(valueRef);
        }
        else
        {
            // Release valueRef for non-CFString types to prevent memory leak
            CFRelease(valueRef);
        }
    }

    return false;
}

/**
 * Get bounds/coordinates of a text selection from an accessibility element and set coordinates in selectionInfo
 *
 * Output coordinates are Quartz screen coordinates (top-left origin, points), the same
 * space as CGEvent locations, so they can be combined with mousePosStart/End directly.
 * On success posLevel is Full; on failure nothing in selectionInfo is modified.
 */
bool SelectionEngine::SetTextRangeCoordinates(AXUIElementRef element, TextSelectionInfo &selectionInfo)
{
    if (!element)
        return false;

    // Strategy 1: Try to get precise coordinates by splitting the selection into smaller ranges
    // Bounds of the first and last character give the start of the first line and the
    // end of the last line. Strategy 2 (bounds of the whole range) returns one bounding
    // box, whose right edge is wrong for a multi-line selection, so it is only a fallback.
    AXValueRef selectedRangeValue = nullptr;

    AXError error =
        AXUIElementCopyAttributeValue(element, kAXSelectedTextRangeAttribute, (CFTypeRef *)&selectedRangeValue);
    if (error == kAXErrorSuccess && selectedRangeValue)
    {
        CFRange selectedRange = {0, 0};
        if (AXValueGetValue(selectedRangeValue, kAXValueTypeCFRange, &selectedRange))
        {
            if (selectedRange.length > 0)
            {
                // Try to get bounds for first and last character of the selection
                CFRange firstCharRange = {selectedRange.location, 1};
                CFRange lastCharRange = {selectedRange.location + selectedRange.length - 1, 1};

                AXValueRef firstCharRangeValue = AXValueCreate(kAXValueTypeCFRange, &firstCharRange);
                AXValueRef lastCharRangeValue = AXValueCreate(kAXValueTypeCFRange, &lastCharRange);

                if (firstCharRangeValue && lastCharRangeValue)
                {
                    // Get bounds for first and last characters
                    AXValueRef firstCharBoundsValue = nullptr;
                    AXValueRef lastCharBoundsValue = nullptr;

                    AXError firstCharBoundsError = AXUIElementCopyParameterizedAttributeValue(
                        element, kAXBoundsForRangeParameterizedAttribute, firstCharRangeValue,
                        (CFTypeRef *)&firstCharBoundsValue);
                    AXError lastCharBoundsError = AXUIElementCopyParameterizedAttributeValue(
                        element, kAXBoundsForRangeParameterizedAttribute, lastCharRangeValue,
                        (CFTypeRef *)&lastCharBoundsValue);

                    if (firstCharBoundsError == kAXErrorSuccess && firstCharBoundsValue &&
                        lastCharBoundsError == kAXErrorSuccess && lastCharBoundsValue)
                    {
                        CGRect firstCharRect = CGRectZero;
                        CGRect lastCharRect = CGRectZero;

                        if (AXValueGetValue(firstCharBoundsValue, kAXValueTypeCGRect, &firstCharRect) &&
                            AXValueGetValue(lastCharBoundsValue, kAXValueTypeCGRect, &lastCharRect))
                        {
                            // Validate bounds similar to Windows version
                            // Compatibility thresholds: some apps answer with a zero-size
                            // rect, the whole element frame (height >= 100), negative
                            // coordinates, or huge sentinel values. Those would place
                            // the caller's popup off-screen, so fall through to Strategy
                            // 2 / mouse positions instead. Revalidate against native,
                            // Chromium, and Electron apps before changing the numbers.
                            if (firstCharRect.size.width > 1.0 && firstCharRect.size.height > 0 &&
                                firstCharRect.size.height < 100.0 && lastCharRect.size.width > 1.0 &&
                                lastCharRect.size.height > 0 && lastCharRect.size.height < 100.0 &&
                                firstCharRect.origin.x >= 0 && firstCharRect.origin.y >= 0 &&
                                lastCharRect.origin.x >= 0 && lastCharRect.origin.y >= 0 &&
                                firstCharRect.origin.x < 10000 && firstCharRect.origin.y < 10000 &&
                                lastCharRect.origin.x < 10000 && lastCharRect.origin.y < 10000)
                            {
                                // Set coordinates based on first and last character bounds
                                // First paragraph left-top (first character's top-left)
                                selectionInfo.startTop = CGPointMake(firstCharRect.origin.x, firstCharRect.origin.y);

                                // First paragraph left-bottom (first character's bottom-left)
                                selectionInfo.startBottom = CGPointMake(
                                    firstCharRect.origin.x, firstCharRect.origin.y + firstCharRect.size.height);

                                // Last paragraph right-top (last character's top-right)
                                selectionInfo.endTop =
                                    CGPointMake(lastCharRect.origin.x + lastCharRect.size.width, lastCharRect.origin.y);

                                // Last paragraph right-bottom (last character's bottom-right)
                                selectionInfo.endBottom = CGPointMake(lastCharRect.origin.x + lastCharRect.size.width,
                                                                      lastCharRect.origin.y + lastCharRect.size.height);

                                selectionInfo.posLevel = SelectionPositionLevel::Full;

                                // Clean up resources
                                CFRelease(firstCharBoundsValue);
                                CFRelease(lastCharBoundsValue);
                                CFRelease(firstCharRangeValue);
                                CFRelease(lastCharRangeValue);
                                CFRelease(selectedRangeValue);

                                return true;
                            }
                        }

                        if (firstCharBoundsValue)
                            CFRelease(firstCharBoundsValue);
                        if (lastCharBoundsValue)
                            CFRelease(lastCharBoundsValue);
                    }

                    if (firstCharRangeValue)
                        CFRelease(firstCharRangeValue);
                    if (lastCharRangeValue)
                        CFRelease(lastCharRangeValue);
                }
                else
                {
                    if (firstCharRangeValue)
                        CFRelease(firstCharRangeValue);
                    if (lastCharRangeValue)
                        CFRelease(lastCharRangeValue);
                }
            }
        }
        CFRelease(selectedRangeValue);
        selectedRangeValue = nullptr;
    }

    // Strategy 2: Try to get selected text range bounds using kAXBoundsForRangeParameterizedAttribute
    error = AXUIElementCopyAttributeValue(element, kAXSelectedTextRangeAttribute, (CFTypeRef *)&selectedRangeValue);
    if (error == kAXErrorSuccess && selectedRangeValue)
    {
        CFRange selectedRange = {0, 0};
        if (AXValueGetValue(selectedRangeValue, kAXValueTypeCFRange, &selectedRange))
        {
            if (selectedRange.length > 0)
            {
                // Get bounds for the text range
                AXValueRef boundsValue = nullptr;
                error = AXUIElementCopyParameterizedAttributeValue(element, kAXBoundsForRangeParameterizedAttribute,
                                                                   selectedRangeValue, (CFTypeRef *)&boundsValue);
                if (error == kAXErrorSuccess && boundsValue)
                {
                    CGRect rect = CGRectZero;
                    if (AXValueGetValue(boundsValue, kAXValueTypeCGRect, &rect))
                    {
                        // Validate the bounds - some apps return invalid coordinates
                        if (rect.size.width > 0 && rect.size.height > 0 && rect.origin.x >= 0 && rect.origin.y >= 0 &&
                            rect.origin.x < 10000 && rect.origin.y < 10000)  // Reasonable screen bounds
                        {
                            // Set position information based on bounds
                            selectionInfo.startTop = CGPointMake(rect.origin.x, rect.origin.y);
                            selectionInfo.startBottom = CGPointMake(rect.origin.x, rect.origin.y + rect.size.height);
                            selectionInfo.endTop = CGPointMake(rect.origin.x + rect.size.width, rect.origin.y);
                            selectionInfo.endBottom =
                                CGPointMake(rect.origin.x + rect.size.width, rect.origin.y + rect.size.height);
                            selectionInfo.posLevel = SelectionPositionLevel::Full;

                            CFRelease(boundsValue);
                            CFRelease(selectedRangeValue);
                            return true;
                        }
                    }
                    CFRelease(boundsValue);
                }
            }
        }
        CFRelease(selectedRangeValue);
    }

    return false;
}
