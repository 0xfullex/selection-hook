/**
 * @file
 * @brief Value types shared by the macOS selection hook and retrieval engine.
 *
 * Keep this file free of N-API objects and of owned Objective-C or CoreFoundation
 * references. Everything here is copied by value between selection_hook.mm and
 * core/engine.mm.
 */
#pragma once

#import <ApplicationServices/ApplicationServices.h>

#import <sys/types.h>
#import <cstdint>
#import <string>
#import <vector>

// Text selection detection type enum
enum class SelectionDetectType
{
    None = 0,
    Drag = 1,
    DoubleClick = 2,
    ShiftClick = 3
};

// Text selection method enum
/**
 * Retrieval method reported through the JavaScript API (SelectionMethod in index.js).
 * @warning Numeric values are public API and are shared across platforms (Windows uses
 * 1-3, macOS 11, clipboard 99 everywhere). Do not renumber or reuse values.
 */
enum class SelectionMethod
{
    None = 0,
    AXAPI = 11,
    Clipboard = 99
};

/**
 * Position level enum for text selection tracking
 * @warning Numeric values are exposed to JavaScript (posLevel) and must remain stable.
 * macOS currently produces None, MouseSingle, MouseDual, and Full; Detailed is reserved.
 */
enum class SelectionPositionLevel
{
    None = 0,         // No position information available
    MouseSingle = 1,  // Only current mouse cursor position is known
    MouseDual = 2,    // Mouse start and end positions are known
    Full = 3,         // selection first paragraph's start and last paragraph's end
                      // coordinates are known
    Detailed = 4      // Detailed selection coordinates including all needed corner points
};

/** @warning Numeric values are accepted from JavaScript configuration and must remain stable. */
enum class FilterMode
{
    Default = 0,      // trigger anyway
    IncludeList = 1,  // only trigger when the program name is in the include list
    ExcludeList = 2   // only trigger when the program name is not in the exclude list
};

/**
 * Structure to store text selection information
 *
 * All CGPoint values are Quartz screen coordinates: origin at the top-left of the main
 * display, in points (not pixels). AX bounds and CGEvent locations both use this space,
 * so text and mouse positions are directly comparable. Do not convert them with AppKit
 * (NSScreen / NSWindow), whose origin is bottom-left.
 */
struct TextSelectionInfo
{
    std::string text;         ///< Selected text content (UTF-8)
    std::string programName;  ///< program name that triggered the selection (bundle identifier)

    CGPoint startTop;     ///< First paragraph left-top (screen coordinates)
    CGPoint startBottom;  ///< First paragraph left-bottom (screen coordinates)
    CGPoint endTop;       ///< Last paragraph right-top (screen coordinates)
    CGPoint endBottom;    ///< Last paragraph right-bottom (screen coordinates)

    CGPoint mousePosStart;  ///< Mouse position when selection started (screen coordinates)
    CGPoint mousePosEnd;    ///< Mouse position when selection ended (screen coordinates)

    SelectionMethod method;
    SelectionPositionLevel posLevel;

    bool isFullscreen;  ///< Whether the current app's front window is in fullscreen mode, only for macOS

    TextSelectionInfo() : method(SelectionMethod::None), posLevel(SelectionPositionLevel::None)
    {
        startTop = CGPointZero;
        startBottom = CGPointZero;
        endTop = CGPointZero;
        endBottom = CGPointZero;
        mousePosStart = CGPointZero;
        mousePosEnd = CGPointZero;
    }

    void clear()
    {
        text.clear();
        programName.clear();
        startTop = CGPointZero;
        startBottom = CGPointZero;
        endTop = CGPointZero;
        endBottom = CGPointZero;
        mousePosStart = CGPointZero;
        mousePosEnd = CGPointZero;
        method = SelectionMethod::None;
        posLevel = SelectionPositionLevel::None;
        isFullscreen = false;
    }
};

/**
 * @brief Immutable inputs captured for one retrieval.
 *
 * selection_hook.mm copies its mutable settings into this snapshot before each call so
 * the engine never reads the addon instance. There is no worker thread on macOS today;
 * the snapshot exists to keep core/ free of SelectionHook state.
 */
struct SelectionConfigSnapshot
{
    bool clipboardEnabled = true;
    FilterMode clipboardFilterMode = FilterMode::Default;
    std::vector<std::string> clipboardFilterList;
    FilterMode globalFilterMode = FilterMode::Default;
    std::vector<std::string> globalFilterList;
    /** NSPasteboard changeCount sampled at mouse-down time (see MouseEventContext). */
    int64_t clipboardSequence = 0;
    /** Own process id. The clipboard fallback refuses to copy from this process. */
    pid_t runningPid = 0;
};
