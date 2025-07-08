/**
 * Common definitions for Linux Selection Hook
 *
 * This file contains shared structures, enums, and types used across
 * the main implementation and protocol-specific files.
 */

#ifndef LINUX_SELECTION_HOOK_COMMON_H
#define LINUX_SELECTION_HOOK_COMMON_H

#include <cstdint>
#include <string>

// Undefine X11 None macro that conflicts with our enum
#ifdef None
#undef None
#endif

// Common Point structure for coordinates
struct Point
{
    int x = 0;
    int y = 0;
    Point() = default;
    Point(int x, int y) : x(x), y(y) {}
};

// Display protocol enum
enum class DisplayProtocol
{
    Unknown = 0,
    X11 = 1,
    Wayland = 2
};

// Text selection detection type enum
enum class SelectionDetectType
{
    None = 0,
    Drag = 1,
    DoubleClick = 2,
    ShiftClick = 3
};

// Text selection method enum
enum class SelectionMethod
{
    None = 0,
    X11Selection = 21,
    Clipboard = 99
};

// Position level enum for text selection tracking
enum class SelectionPositionLevel
{
    None = 0,         // No position information available
    MouseSingle = 1,  // Only current mouse cursor position is known
    MouseDual = 2,    // Mouse start and end positions are known
    Full = 3,         // selection first paragraph's start and last paragraph's end coordinates are known
    Detailed = 4      // Detailed selection coordinates including all needed corner points
};

// Copy key type enum for SendCopyKey function
enum class CopyKeyType
{
    CtrlInsert = 0,
    CtrlC = 1
};

// Filter mode enum
enum class FilterMode
{
    Default = 0,      // trigger anyway
    IncludeList = 1,  // only trigger when the program name is in the include list
    ExcludeList = 2   // only trigger when the program name is not in the exclude list
};

// Fine-tuned list type enum
enum class FineTunedListType
{
    ExcludeClipboardCursorDetect = 0,
    IncludeClipboardDelayRead = 1
};

// Mouse button enum
enum class MouseButton
{
    None = -1,
    Unknown = 99,
    Left = 0,
    Middle = 1,
    Right = 2,
    Back = 3,
    Forward = 4,
    WheelVertical = 0,
    WheelHorizontal = 1
};

// Structure to store text selection information
struct TextSelectionInfo
{
    std::string text;         ///< Selected text content (UTF-8)
    std::string programName;  ///< program name that triggered the selection

    Point startTop;     ///< First paragraph left-top (screen coordinates)
    Point startBottom;  ///< First paragraph left-bottom (screen coordinates)
    Point endTop;       ///< Last paragraph right-top (screen coordinates)
    Point endBottom;    ///< Last paragraph right-bottom (screen coordinates)

    Point mousePosStart;  ///< Current mouse position (screen coordinates)
    Point mousePosEnd;    ///< Mouse down position (screen coordinates)

    SelectionMethod method;
    SelectionPositionLevel posLevel;

    TextSelectionInfo() : method(SelectionMethod::None), posLevel(SelectionPositionLevel::None) {}

    void clear()
    {
        text.clear();
        programName.clear();
        startTop = Point();
        startBottom = Point();
        endTop = Point();
        endBottom = Point();
        mousePosStart = Point();
        mousePosEnd = Point();
        method = SelectionMethod::None;
        posLevel = SelectionPositionLevel::None;
    }
};

// Structure to store mouse event information
struct MouseEventContext
{
    int type;    ///< Linux input event type (EV_KEY, EV_REL, etc.)
    int code;    ///< Event code (BTN_LEFT, REL_X, etc.)
    int value;   ///< Event value
    Point pos;   ///< Mouse position (calculated)
    int button;  ///< Mouse button
    int flag;    ///< Mouse extra flag (eg. wheel direction)
};

// Structure to store keyboard event information
struct KeyboardEventContext
{
    int type;   ///< Linux input event type (EV_KEY)
    int code;   ///< Key code
    int value;  ///< Key value (0=release, 1=press, 2=repeat)
    int flags;  ///< Event flags
};

// Protocol abstraction interface
// Contains function pointers for protocol-specific operations
struct ProtocolInterface
{
    // Protocol identification
    DisplayProtocol protocol;

    // Initialization and cleanup
    bool (*Initialize)(void** context);
    void (*Cleanup)(void* context);

    // Window management
    uint64_t (*GetActiveWindow)(void* context);
    bool (*GetProgramNameFromWindow)(void* context, uint64_t window, std::string& programName);

    // Text selection
    bool (*GetSelectedTextFromSelection)(void* context, std::string& text);
    bool (*SetTextRangeCoordinates)(void* context, uint64_t window, TextSelectionInfo& selectionInfo);

    // Clipboard operations
    bool (*WriteClipboard)(void* context, const std::string& text);
    bool (*ReadClipboard)(void* context, std::string& text);

    // Key operations
    void (*SendCopyKey)(void* context, CopyKeyType type);
    bool (*ShouldKeyInterruptViaClipboard)(void* context);

    // Protocol context data
    void* context;
};

// Protocol detection and initialization functions
DisplayProtocol DetectDisplayProtocol();
bool InitializeX11Protocol(ProtocolInterface* protocol);
bool InitializeWaylandProtocol(ProtocolInterface* protocol);

#endif  // LINUX_SELECTION_HOOK_COMMON_H