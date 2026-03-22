/**
 * Common definitions for Linux Selection Hook
 *
 * This file contains shared structures, enums, and types used across
 * the main implementation and protocol-specific files.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Linux input constants for ModifierState
#include <linux/input.h>

// Sentinel value for unreliable/unavailable screen coordinates.
// Used when coordinate source (e.g. libevdev) cannot provide actual screen positions.
constexpr int INVALID_COORDINATE = -99999;

// Common Point structure for coordinates
struct Point
{
    int x = 0;
    int y = 0;
    bool valid = false;  // true only when constructed with coordinates

    Point() = default;
    Point(int x, int y) : x(x), y(y), valid(true) {}
};

// Window rectangle for movement detection
struct WindowRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// Check if window has moved/resized with 2 pixel tolerance (matches Windows implementation)
inline bool HasWindowMoved(const WindowRect &current, const WindowRect &last)
{
    return (abs(current.x - last.x) > 2 || abs(current.y - last.y) > 2 || abs(current.width - last.width) > 2 ||
            abs(current.height - last.height) > 2);
}

// Display protocol enum
enum class DisplayProtocol
{
    Unknown = 0,
    X11 = 1,
    Wayland = 2
};

// Compositor type enum for cursor position query strategy.
//
// Values represent the compositor, not the desktop environment.
// Some compositors are bundled with a desktop environment (DE) and detected
// via XDG_CURRENT_DESKTOP; others are standalone and detected via their own
// environment variables.
//
//   Enum value     Compositor    DE             Detected via
//   ──────────     ──────────    ──────────     ────────────────────────────
//   KWin           KWin          KDE Plasma     XDG_CURRENT_DESKTOP="KDE"
//   Mutter         mutter        GNOME          XDG_CURRENT_DESKTOP="GNOME"
//   Hyprland       Hyprland      (standalone)   HYPRLAND_INSTANCE_SIGNATURE
//   Sway           sway          (standalone)   SWAYSOCK
//   Wlroots        (various)     (standalone)   XDG_CURRENT_DESKTOP="wlroots"
//   CosmicComp     cosmic-comp   COSMIC         XDG_CURRENT_DESKTOP="COSMIC"
//
// "Wlroots" is a catch-all for wlroots-based compositors (labwc, river, etc.)
// that identify themselves via XDG_CURRENT_DESKTOP but don't have a dedicated
// detection path.
enum class CompositorType
{
    Unknown = 0,
    KWin = 1,       // KDE Plasma's compositor (kwin_wayland)
    Mutter = 2,     // GNOME's compositor (mutter / gnome-shell)
    Hyprland = 3,   // Standalone tiling compositor
    Sway = 4,       // Standalone i3-compatible compositor
    Wlroots = 5,    // Generic wlroots-based compositors (labwc, river, etc.)
    CosmicComp = 6  // System76 COSMIC's compositor (cosmic-comp)
};

// Linux environment information (cached at construction time)
struct LinuxEnvInfo
{
    DisplayProtocol displayProtocol = DisplayProtocol::Unknown;
    CompositorType compositorType = CompositorType::Unknown;
    bool hasInputDeviceAccess = false;
    bool isRoot = false;
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
    Primary = 22,  // primary selection
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

// Filter mode enum
enum class FilterMode
{
    Default = 0,      // trigger anyway
    IncludeList = 1,  // only trigger when the program name is in the include list
    ExcludeList = 2   // only trigger when the program name is not in the exclude list
};

// Keyboard modifier flags (bitmask for KeyboardEventContext::flags)
constexpr int MODIFIER_SHIFT = 0x01;
constexpr int MODIFIER_CTRL = 0x02;
constexpr int MODIFIER_ALT = 0x04;
constexpr int MODIFIER_META = 0x08;

// Shared modifier key state tracking for X11 and Wayland protocols
struct ModifierState
{
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool super = false;

    int GetFlags() const
    {
        int flags = 0;
        if (shift)
            flags |= MODIFIER_SHIFT;
        if (ctrl)
            flags |= MODIFIER_CTRL;
        if (alt)
            flags |= MODIFIER_ALT;
        if (super)
            flags |= MODIFIER_META;
        return flags;
    }

    void UpdateFromKeyCode(unsigned int code, bool is_press)
    {
        switch (code)
        {
            case KEY_LEFTCTRL:
            case KEY_RIGHTCTRL:
                ctrl = is_press;
                break;
            case KEY_LEFTSHIFT:
            case KEY_RIGHTSHIFT:
                shift = is_press;
                break;
            case KEY_LEFTALT:
            case KEY_RIGHTALT:
                alt = is_press;
                break;
            case KEY_LEFTMETA:
            case KEY_RIGHTMETA:
                super = is_press;
                break;
        }
    }
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

    Point mousePosStart;  ///< Mouse position when selection started (screen coordinates)
    Point mousePosEnd;    ///< Mouse position when selection ended (screen coordinates)

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
    int code;   ///< Linux KEY_* code from <linux/input-event-codes.h>
    int value;  ///< Key value (0=release, 1=press, 2=repeat)
    int flags;  ///< Modifier bitmask (MODIFIER_SHIFT/CTRL/ALT/META)
};

// Structure for selection change event (XFixes on X11, data-control on Wayland)
struct SelectionChangeContext
{
    uint64_t timestamp_ms;
};

// Input monitoring callback function types
typedef void (*MouseEventCallback)(void *context, MouseEventContext *mouseEvent);
typedef void (*KeyboardEventCallback)(void *context, KeyboardEventContext *keyboardEvent);
typedef void (*SelectionEventCallback)(void *context, SelectionChangeContext *selectionEvent);

// Protocol abstraction base class
// Abstract base class for protocol-specific implementations
class ProtocolBase
{
  public:
    virtual ~ProtocolBase() = default;

    // Protocol identification
    virtual DisplayProtocol GetProtocol() const = 0;

    // Initialization and cleanup
    virtual bool Initialize() = 0;
    virtual void Cleanup() = 0;

    // Window management
    virtual uint64_t GetActiveWindow() = 0;
    virtual bool GetProgramNameFromWindow(uint64_t window, std::string &programName) = 0;
    virtual bool GetWindowRect(uint64_t window, WindowRect &rect) = 0;

    // Text selection
    virtual bool GetTextViaPrimary(std::string &text) = 0;

    // Clipboard operations
    virtual bool WriteClipboard(const std::string &text) = 0;
    virtual bool ReadClipboard(std::string &text) = 0;

    // Modifier key state query (for Shift+Click detection etc.)
    virtual int GetModifierFlags() = 0;

    // Get current mouse cursor position (screen coordinates)
    virtual Point GetCurrentMousePosition() { return Point(); }

    // Set environment info from top-level detection
    virtual void SetEnvInfo(const LinuxEnvInfo &info) { (void)info; }

    // Input monitoring (for mouse and keyboard events)
    virtual bool InitializeInputMonitoring(MouseEventCallback mouseCallback, KeyboardEventCallback keyboardCallback,
                                           SelectionEventCallback selectionCallback, void *context) = 0;
    virtual void CleanupInputMonitoring() = 0;
    virtual bool StartInputMonitoring() = 0;
    virtual void StopInputMonitoring() = 0;
};

// Forward declarations for protocol implementations
class X11Protocol;
class WaylandProtocol;

// Protocol detection and factory functions
DisplayProtocol DetectDisplayProtocol();
std::unique_ptr<ProtocolBase> CreateProtocol(DisplayProtocol protocol);

// Factory function declarations for protocol implementations
extern std::unique_ptr<ProtocolBase> CreateX11Protocol();
extern std::unique_ptr<ProtocolBase> CreateWaylandProtocol();
