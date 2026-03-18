/**
 * Text Selection Hook for Linux
 *
 * A Node Native Module that captures text selection events across applications
 * on Linux using X11/Wayland libraries.
 *
 * Main components:
 * - TextSelectionHook class: Core implementation of the module
 * - Text selection detection: AT-SPI (X11/Wayland), Primary Selection (X11)
 * - Event monitoring: XRecord (X11), libevdev (Wayland) for input monitoring
 * - Thread management: Background threads for hooks with thread-safe callbacks
 *
 * Features:
 * - Detect text selections via mouse drag, double-click, or keyboard
 * - Get selection coordinates and text content
 * - Monitor mouse and keyboard events
 * - Integration with Node.js via N-API
 *
 * Usage:
 * This module exposes a JavaScript API through index.js that allows
 * applications to monitor text selection events system-wide.
 *
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 *
 */

#include <napi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>

// Standard C headers
#include <cstdlib>
#include <cstring>

// Linux system headers for root detection
#include <sys/types.h>
#include <unistd.h>

// Include common definitions
#include "common.h"

// AT-SPI headers for accessibility-based text selection
#include <atspi/atspi.h>

// Keyboard utility for Linux key code conversion
#include "lib/keyboard.h"

// Headers for input monitoring - now handled by protocol layer

// Linux input constants for event processing
#include <linux/input.h>

// Undefine X11 None macro that conflicts with our enum
// #ifdef None
// #undef None
// #endif

// External function declarations from protocol implementations
extern std::unique_ptr<ProtocolBase> CreateX11Protocol();
extern std::unique_ptr<ProtocolBase> CreateWaylandProtocol();

/**
 * Factory function to create protocol instances
 */
std::unique_ptr<ProtocolBase> CreateProtocol(DisplayProtocol protocol)
{
    switch (protocol)
    {
        case DisplayProtocol::X11:
            return CreateX11Protocol();
        case DisplayProtocol::Wayland:
            return CreateWaylandProtocol();
        default:
            return nullptr;
    }
}

/**
 * Detect the current display protocol (X11 or Wayland)
 */
DisplayProtocol DetectDisplayProtocol()
{
    // Check for Wayland by looking for WAYLAND_DISPLAY environment variable
    const char *wayland_display = std::getenv("WAYLAND_DISPLAY");
    if (wayland_display && strlen(wayland_display) > 0)
    {
        return DisplayProtocol::Wayland;
    }

    // Check for X11 by looking for DISPLAY environment variable
    const char *x11_display = std::getenv("DISPLAY");
    if (x11_display && strlen(x11_display) > 0)
    {
        return DisplayProtocol::X11;
    }

    // Default to X11 if neither is clearly set
    return DisplayProtocol::X11;
}

// Mouse&Keyboard hook constants
constexpr int DEFAULT_MOUSE_EVENT_QUEUE_SIZE = 512;
constexpr int DEFAULT_KEYBOARD_EVENT_QUEUE_SIZE = 128;

// Mouse interaction constants
constexpr int MIN_DRAG_DISTANCE = 8;
constexpr uint64_t MAX_DRAG_TIME_MS = 8000;
constexpr int DOUBLE_CLICK_MAX_DISTANCE = 3;
static uint64_t DOUBLE_CLICK_TIME_MS = 500;

// XFixes correlation window: max time between mouse gesture and XFixes event to be considered related
constexpr uint64_t CORRELATION_WINDOW_MS = 500;

//=============================================================================
// TextSelectionHook Class Declaration
//=============================================================================
class SelectionHook : public Napi::ObjectWrap<SelectionHook>
{
  public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    SelectionHook(const Napi::CallbackInfo &info);
    ~SelectionHook();

  private:
    static Napi::FunctionReference constructor;

    // Node.js interface methods
    void Start(const Napi::CallbackInfo &info);
    void Stop(const Napi::CallbackInfo &info);
    void EnableMouseMoveEvent(const Napi::CallbackInfo &info);
    void DisableMouseMoveEvent(const Napi::CallbackInfo &info);
    void EnableClipboard(const Napi::CallbackInfo &info);
    void DisableClipboard(const Napi::CallbackInfo &info);
    void SetClipboardMode(const Napi::CallbackInfo &info);
    void SetGlobalFilterMode(const Napi::CallbackInfo &info);
    void SetFineTunedList(const Napi::CallbackInfo &info);
    void SetSelectionPassiveMode(const Napi::CallbackInfo &info);
    Napi::Value GetCurrentSelection(const Napi::CallbackInfo &info);
    Napi::Value WriteToClipboard(const Napi::CallbackInfo &info);
    Napi::Value ReadFromClipboard(const Napi::CallbackInfo &info);
    Napi::Value LinuxGetDisplayProtocol(const Napi::CallbackInfo &info);
    Napi::Value LinuxIsRoot(const Napi::CallbackInfo &info);

    // Core functionality methods
    bool GetSelectedText(uint64_t window, TextSelectionInfo &selectionInfo);
    bool GetTextViaATSPI(uint64_t window, TextSelectionInfo &selectionInfo);
    bool GetTextViaPrimary(uint64_t window, TextSelectionInfo &selectionInfo);
    bool GetTextViaClipboard(uint64_t window, TextSelectionInfo &selectionInfo);
    bool ShouldProcessGetSelection();
    bool ShouldProcessViaClipboard(const std::string &programName);
    Napi::Object CreateSelectionResultObject(Napi::Env env, const TextSelectionInfo &selectionInfo);

    // AT-SPI initialization flag
    static bool atspi_initialized;

    // Helper methods
    bool IsInFilterList(const std::string &programName, const std::vector<std::string> &filterList);
    void ProcessStringArrayToList(const Napi::Array &array, std::vector<std::string> &targetList);

    // Mouse and keyboard event handling methods
    static void ProcessMouseEvent(Napi::Env env, Napi::Function function, MouseEventContext *mouseEvent);
    static void ProcessKeyboardEvent(Napi::Env env, Napi::Function function, KeyboardEventContext *keyboardEvent);
    static void ProcessSelectionEvent(Napi::Env env, Napi::Function function, SelectionChangeContext *pEvent);

    // Input monitoring callback methods
    static void OnMouseEventCallback(void *context, MouseEventContext *mouseEvent);
    static void OnKeyboardEventCallback(void *context, KeyboardEventContext *keyboardEvent);
    static void OnSelectionEventCallback(void *context, SelectionChangeContext *selectionEvent);

    // Emit text selection event (shared by Path A and Path B)
    void EmitSelectionEvent(SelectionDetectType type, Point start, Point end);

    // Protocol interface for X11/Wayland abstraction
    std::unique_ptr<ProtocolBase> protocol;

    // Current display protocol (X11 or Wayland)
    DisplayProtocol current_display_protocol;

    // Mouse position tracking
    Point current_mouse_pos;

    // Mouse state tracking (for selection gesture detection)
    Point last_mouse_down_pos;
    uint64_t last_mouse_down_time = 0;
    Point last_mouse_up_pos;
    uint64_t last_mouse_up_time = 0;
    Point prev_mouse_up_pos;        // Previous mouse-up (for shift+click)
    uint64_t prev_mouse_up_time = 0;
    uint64_t last_window_handler = 0;
    WindowRect last_window_rect;
    bool is_last_valid_click = false;
    int last_mouse_up_modifier_flags = 0;

    // XFixes atomic timestamp - written by OnSelectionEventCallback (XFixes thread),
    // read by ProcessMouseEvent (main thread)
    std::atomic<uint64_t> last_xfixes_time{0};

    // Pending gesture for Path B (XFixes arrives after mouse-up)
    struct {
        bool active = false;
        SelectionDetectType type = SelectionDetectType::None;
        Point mousePosStart;
        Point mousePosEnd;
        uint64_t timestamp = 0;
    } pending_gesture;

    // Thread communication
    Napi::ThreadSafeFunction tsfn;
    Napi::ThreadSafeFunction mouse_tsfn;
    Napi::ThreadSafeFunction keyboard_tsfn;
    Napi::ThreadSafeFunction selection_tsfn;  // For XFixes events (Path B)

    std::atomic<bool> running{false};
    std::atomic<bool> mouse_keyboard_running{false};

    // the text selection is processing, we should ignore some events
    std::atomic<bool> is_processing{false};
    // user use GetCurrentSelection
    bool is_triggered_by_user = false;

    bool is_enabled_mouse_move_event = false;

    // passive mode: only trigger when user call GetSelectionText
    bool is_selection_passive_mode = false;
    bool is_enabled_clipboard = true;  // Enable by default
    // Store clipboard sequence number when mouse down
    int64_t clipboard_sequence = 0;

    // clipboard filter mode
    FilterMode clipboard_filter_mode = FilterMode::Default;
    std::vector<std::string> clipboard_filter_list;

    // global filter mode
    FilterMode global_filter_mode = FilterMode::Default;
    std::vector<std::string> global_filter_list;

    // fine-tuned lists (ftl) for some apps
    std::vector<std::string> ftl_exclude_clipboard_cursor_detect;
    std::vector<std::string> ftl_include_clipboard_delay_read;
};

// Static member initialization
Napi::FunctionReference SelectionHook::constructor;
bool SelectionHook::atspi_initialized = false;

// Static pointer for callbacks
static SelectionHook *currentInstance = nullptr;

/**
 * Constructor - initializes display protocol
 */
SelectionHook::SelectionHook(const Napi::CallbackInfo &info) : Napi::ObjectWrap<SelectionHook>(info)
{
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);

    currentInstance = this;

    // Detect and initialize display protocol
    current_display_protocol = DetectDisplayProtocol();

    protocol = CreateProtocol(current_display_protocol);
    if (!protocol)
    {
        Napi::Error::New(env, "Failed to create protocol interface").ThrowAsJavaScriptException();
        return;
    }

    if (!protocol->Initialize())
    {
        Napi::Error::New(env, "Failed to initialize display protocol").ThrowAsJavaScriptException();
        return;
    }

    // Get system double-click time (placeholder - Linux specific implementation needed)
    DOUBLE_CLICK_TIME_MS = 500;  // Default value

    // Initialize current mouse position
    current_mouse_pos = Point(0, 0);
}

/**
 * Destructor - cleans up resources
 */
SelectionHook::~SelectionHook()
{
    // Stop worker thread
    bool was_running = running.exchange(false);
    if (was_running && tsfn)
    {
        tsfn.Release();
    }

    // Stop input monitoring via protocol
    if (protocol)
    {
        protocol->StopInputMonitoring();
        protocol->CleanupInputMonitoring();
    }

    // Ensure mouse_keyboard_running is set to false
    mouse_keyboard_running = false;

    // Release thread-safe functions
    if (mouse_tsfn)
    {
        mouse_tsfn.Release();
    }
    if (keyboard_tsfn)
    {
        keyboard_tsfn.Release();
    }
    if (selection_tsfn)
    {
        selection_tsfn.Release();
    }

    // Clear current instance if it's us
    if (currentInstance == this)
    {
        currentInstance = nullptr;
    }

    // Cleanup protocol
    if (protocol)
    {
        protocol->Cleanup();
    }
}

/**
 * NAPI: Initialize and export the class to JavaScript
 */
Napi::Object SelectionHook::Init(Napi::Env env, Napi::Object exports)
{
    Napi::HandleScope scope(env);

    // Define class with JavaScript-accessible methods
    Napi::Function func =
        DefineClass(env, "TextSelectionHook",
                    {InstanceMethod("start", &SelectionHook::Start), InstanceMethod("stop", &SelectionHook::Stop),
                     InstanceMethod("enableMouseMoveEvent", &SelectionHook::EnableMouseMoveEvent),
                     InstanceMethod("disableMouseMoveEvent", &SelectionHook::DisableMouseMoveEvent),
                     InstanceMethod("enableClipboard", &SelectionHook::EnableClipboard),
                     InstanceMethod("disableClipboard", &SelectionHook::DisableClipboard),
                     InstanceMethod("setClipboardMode", &SelectionHook::SetClipboardMode),
                     InstanceMethod("setGlobalFilterMode", &SelectionHook::SetGlobalFilterMode),
                     InstanceMethod("setFineTunedList", &SelectionHook::SetFineTunedList),
                     InstanceMethod("setSelectionPassiveMode", &SelectionHook::SetSelectionPassiveMode),
                     InstanceMethod("getCurrentSelection", &SelectionHook::GetCurrentSelection),
                     InstanceMethod("writeToClipboard", &SelectionHook::WriteToClipboard),
                     InstanceMethod("readFromClipboard", &SelectionHook::ReadFromClipboard),
                     InstanceMethod("linuxGetDisplayProtocol", &SelectionHook::LinuxGetDisplayProtocol),
                     InstanceMethod("linuxIsRoot", &SelectionHook::LinuxIsRoot)});

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();

    exports.Set("TextSelectionHook", func);
    return exports;
}

/**
 * NAPI: Start monitoring text selections
 */
void SelectionHook::Start(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();

    // Validate callback parameter
    if (info.Length() < 1 || !info[0u].IsFunction())
    {
        Napi::TypeError::New(env, "Function expected as first argument").ThrowAsJavaScriptException();
        return;
    }

    // Don't start if already running
    if (running)
    {
        Napi::Error::New(env, "Text selection hook is already running").ThrowAsJavaScriptException();
        return;
    }

    // Don't start if mouse/keyboard monitoring is already running
    if (mouse_keyboard_running)
    {
        Napi::Error::New(env, "Input monitoring is already running").ThrowAsJavaScriptException();
        return;
    }

    // Ensure ThreadSafeFunction objects are clean
    if (tsfn || mouse_tsfn || keyboard_tsfn || selection_tsfn)
    {
        Napi::Error::New(env, "ThreadSafeFunction objects are not clean").ThrowAsJavaScriptException();
        return;
    }

    // Create thread-safe function from JavaScript callback
    Napi::Function callback = info[0u].As<Napi::Function>();

    tsfn = Napi::ThreadSafeFunction::New(env, callback, "TextSelectionCallback", 0, 1,
                                         [this](Napi::Env) { running = false; });

    // Create thread-safe function for mouse events
    mouse_tsfn = Napi::ThreadSafeFunction::New(env, callback, "MouseEventCallback", DEFAULT_MOUSE_EVENT_QUEUE_SIZE, 1,
                                               [this](Napi::Env) { mouse_keyboard_running = false; });

    // Create thread-safe function for keyboard events
    keyboard_tsfn =
        Napi::ThreadSafeFunction::New(env, callback, "KeyboardEventCallback", DEFAULT_KEYBOARD_EVENT_QUEUE_SIZE, 1,
                                      [this](Napi::Env) { mouse_keyboard_running = false; });

    // Create thread-safe function for XFixes selection events (Path B)
    selection_tsfn =
        Napi::ThreadSafeFunction::New(env, callback, "SelectionEventCallback", 64, 1);

    // Initialize input monitoring via protocol
    if (!protocol->InitializeInputMonitoring(&SelectionHook::OnMouseEventCallback,
                                             &SelectionHook::OnKeyboardEventCallback,
                                             &SelectionHook::OnSelectionEventCallback, this))
    {
        selection_tsfn.Release();
        mouse_tsfn.Release();
        keyboard_tsfn.Release();
        tsfn.Release();
        Napi::Error::New(env, "Failed to initialize input monitoring").ThrowAsJavaScriptException();
        return;
    }

    // Start input monitoring
    try
    {
        if (!protocol->StartInputMonitoring())
        {
            throw std::runtime_error("Failed to start input monitoring");
        }

        // Set running flags only after successful start
        running = true;
        mouse_keyboard_running = true;
    }
    catch (const std::exception &e)
    {
        protocol->CleanupInputMonitoring();
        selection_tsfn.Release();
        mouse_tsfn.Release();
        keyboard_tsfn.Release();
        tsfn.Release();
        Napi::Error::New(env, "Failed to start input monitoring").ThrowAsJavaScriptException();
        return;
    }
}

/**
 * NAPI: Stop monitoring text selections
 */
void SelectionHook::Stop(const Napi::CallbackInfo &info)
{
    // Do nothing if not running
    if (!running)
    {
        return;
    }

    // Set running flags to false first
    running = false;
    mouse_keyboard_running = false;

    // Stop and cleanup input monitoring via protocol (this will wait for threads to finish)
    if (protocol)
    {
        protocol->CleanupInputMonitoring();
    }

    // Give a small delay to ensure any pending callbacks complete
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Release thread-safe functions after threads have stopped
    try
    {
        if (tsfn)
        {
            tsfn.Release();
            tsfn = nullptr;
        }
        if (mouse_tsfn)
        {
            mouse_tsfn.Release();
            mouse_tsfn = nullptr;
        }
        if (keyboard_tsfn)
        {
            keyboard_tsfn.Release();
            keyboard_tsfn = nullptr;
        }
        if (selection_tsfn)
        {
            selection_tsfn.Release();
            selection_tsfn = nullptr;
        }
    }
    catch (const std::exception &e)
    {
        // Log error but don't throw to prevent further issues
        printf("Error releasing ThreadSafeFunction: %s\n", e.what());
    }
}

/**
 * NAPI: Enable mouse move events
 */
void SelectionHook::EnableMouseMoveEvent(const Napi::CallbackInfo &info)
{
    is_enabled_mouse_move_event = true;
}

/**
 * NAPI: Disable mouse move events to reduce CPU usage
 */
void SelectionHook::DisableMouseMoveEvent(const Napi::CallbackInfo &info)
{
    is_enabled_mouse_move_event = false;
}

/**
 * NAPI: Enable clipboard fallback
 */
void SelectionHook::EnableClipboard(const Napi::CallbackInfo &info)
{
    is_enabled_clipboard = true;
}

/**
 * NAPI: Disable clipboard fallback
 */
void SelectionHook::DisableClipboard(const Napi::CallbackInfo &info)
{
    is_enabled_clipboard = false;
}

/**
 * NAPI: Set the clipboard filter mode & list
 */
void SelectionHook::SetClipboardMode(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    // Validate arguments
    if (info.Length() < 2 || !info[0u].IsNumber() || !info[1u].IsArray())
    {
        Napi::TypeError::New(env, "Number and Array expected as arguments").ThrowAsJavaScriptException();
        return;
    }

    // Get clipboard mode from first argument
    int mode = info[0u].As<Napi::Number>().Int32Value();
    clipboard_filter_mode = static_cast<FilterMode>(mode);

    Napi::Array listArray = info[1u].As<Napi::Array>();

    // Use helper method to process the array
    ProcessStringArrayToList(listArray, clipboard_filter_list);
}

/**
 * NAPI: Set the global filter mode & list
 */
void SelectionHook::SetGlobalFilterMode(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    // Validate arguments
    if (info.Length() < 2 || !info[0u].IsNumber() || !info[1u].IsArray())
    {
        Napi::TypeError::New(env, "Number and Array expected as arguments").ThrowAsJavaScriptException();
        return;
    }

    // Get global mode from first argument
    int mode = info[0u].As<Napi::Number>().Int32Value();
    global_filter_mode = static_cast<FilterMode>(mode);

    Napi::Array listArray = info[1u].As<Napi::Array>();

    // Use helper method to process the array
    ProcessStringArrayToList(listArray, global_filter_list);
}

/**
 * NAPI: Set fine-tuned list based on type
 * only for Windows now
 */
void SelectionHook::SetFineTunedList(const Napi::CallbackInfo &info)
{
    return;
}

/**
 * NAPI: Set selection passive mode
 */
void SelectionHook::SetSelectionPassiveMode(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    // Validate arguments
    if (info.Length() < 1 || !info[0u].IsBoolean())
    {
        Napi::TypeError::New(env, "Boolean expected as argument").ThrowAsJavaScriptException();
        return;
    }

    is_selection_passive_mode = info[0u].As<Napi::Boolean>().Value();
}

/**
 * NAPI: Get the currently selected text from the active window
 */
Napi::Value SelectionHook::GetCurrentSelection(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();

    try
    {
        if (!ShouldProcessGetSelection())
        {
            return env.Null();
        }

        // Get the currently active window
        uint64_t activeWindow = protocol->GetActiveWindow();
        if (!activeWindow)
        {
            return env.Null();
        }

        // Get selected text
        TextSelectionInfo selectionInfo;
        is_triggered_by_user = true;
        if (!GetSelectedText(activeWindow, selectionInfo) || selectionInfo.text.empty())
        {
            is_triggered_by_user = false;
            return env.Null();
        }

        is_triggered_by_user = false;

        return CreateSelectionResultObject(env, selectionInfo);
    }
    catch (const std::exception &e)
    {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        is_triggered_by_user = false;
        return env.Null();
    }
}

/**
 * NAPI: Write string to clipboard
 *
 * Linux WriteClipboard has limited reliability due to X11's lazy clipboard model:
 * The clipboard owner must keep a window alive and respond to SelectionRequest events
 * from other applications requesting the data. This requires an event loop or dedicated thread.
 *
 * For the JS API writeToClipboard(), the host application (e.g., Electron) should handle
 * clipboard writes at the JS layer using its own clipboard API.
 */
Napi::Value SelectionHook::WriteToClipboard(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();

    // Validate parameters
    if (info.Length() < 1 || !info[0].IsString())
    {
        Napi::TypeError::New(env, "String expected as argument").ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }

    try
    {
        // Get string from JavaScript
        std::string text = info[0].As<Napi::String>().Utf8Value();

        // Write to clipboard using protocol interface
        bool result = protocol->WriteClipboard(text);
        return Napi::Boolean::New(env, result);
    }
    catch (const std::exception &e)
    {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

/**
 * NAPI: Read string from clipboard
 */
Napi::Value SelectionHook::ReadFromClipboard(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();

    try
    {
        // Read from clipboard
        std::string clipboardContent;
        bool result = protocol->ReadClipboard(clipboardContent);

        if (!result)
        {
            return env.Null();
        }

        // Return as UTF-8 string
        return Napi::String::New(env, clipboardContent);
    }
    catch (const std::exception &e)
    {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        return env.Null();
    }
}

/**
 * NAPI: Get current display protocol
 */
Napi::Value SelectionHook::LinuxGetDisplayProtocol(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();

    try
    {
        // Return the current display protocol as a number
        return Napi::Number::New(env, static_cast<int>(current_display_protocol));
    }
    catch (const std::exception &e)
    {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        return env.Null();
    }
}

/**
 * NAPI: Check if the current process is running as root
 */
Napi::Value SelectionHook::LinuxIsRoot(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();

    try
    {
        // Check if the effective user ID is 0 (root)
        bool isRoot = (geteuid() == 0);
        return Napi::Boolean::New(env, isRoot);
    }
    catch (const std::exception &e)
    {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        return env.Null();
    }
}

/**
 * Get selected text from the active window using multiple methods
 */
bool SelectionHook::GetSelectedText(uint64_t window, TextSelectionInfo &selectionInfo)
{
    if (!window)
        return false;

    if (is_processing.load())
        return false;
    else
        is_processing.store(true);

    // Initialize structure
    selectionInfo.clear();

    // Get program name and store it in selectionInfo
    if (!protocol->GetProgramNameFromWindow(window, selectionInfo.programName))
    {
        selectionInfo.programName = "";

        // if no programName found, and global filter mode is include list, return false
        if (global_filter_mode == FilterMode::IncludeList)
        {
            is_processing.store(false);
            return false;
        }
    }
    // should filter by global filter list
    else if (global_filter_mode != FilterMode::Default)
    {
        bool isIn = IsInFilterList(selectionInfo.programName, global_filter_list);

        if ((global_filter_mode == FilterMode::IncludeList && !isIn) ||
            (global_filter_mode == FilterMode::ExcludeList && isIn))
        {
            is_processing.store(false);
            return false;
        }
    }

    // Primary Selection (X11 only, the only active method)
    if (current_display_protocol == DisplayProtocol::X11)
    {
        if (GetTextViaPrimary(window, selectionInfo))
        {
            selectionInfo.method = SelectionMethod::Primary;
            is_processing.store(false);
            return true;
        }
    }

    // AT-SPI is experimental and disabled due to unreliable screen coordinate conversion
    // across X11/Wayland, different toolkits, and multi-monitor/DPI configurations.
    // Code retained in GetTextViaATSPI() but not called.

    // Clipboard fallback is intentionally not implemented for now.

    is_processing.store(false);
    return false;
}

/**
 * Helper: recursively find a focused accessible element with a text selection
 */
static AtspiAccessible *FindFocusedAccessibleWithSelection(AtspiAccessible *root, int depth)
{
    if (!root || depth > 25)
        return nullptr;

    GError *error = nullptr;

    // Check if this element is focused
    AtspiStateSet *state_set = atspi_accessible_get_state_set(root);
    if (state_set)
    {
        bool is_focused = atspi_state_set_contains(state_set, ATSPI_STATE_FOCUSED);
        g_object_unref(state_set);

        if (is_focused)
        {
            // Check if it has a text interface with selections
            AtspiText *text_iface = atspi_accessible_get_text_iface(root);
            if (text_iface)
            {
                int n_selections = atspi_text_get_n_selections(text_iface, &error);
                if (error)
                {
                    g_error_free(error);
                    error = nullptr;
                }
                g_object_unref(text_iface);

                if (n_selections > 0)
                {
                    g_object_ref(root);
                    return root;
                }
            }
        }
    }

    // Recurse into children
    int child_count = atspi_accessible_get_child_count(root, &error);
    if (error)
    {
        g_error_free(error);
        return nullptr;
    }

    for (int i = 0; i < child_count; i++)
    {
        error = nullptr;
        AtspiAccessible *child = atspi_accessible_get_child_at_index(root, i, &error);
        if (error)
        {
            g_error_free(error);
            continue;
        }
        if (!child)
            continue;

        AtspiAccessible *found = FindFocusedAccessibleWithSelection(child, depth + 1);
        g_object_unref(child);

        if (found)
            return found;
    }

    return nullptr;
}

/**
 * Get selected text via AT-SPI accessibility interface
 * Works on both X11 and Wayland, provides text + coordinates
 */
bool SelectionHook::GetTextViaATSPI(uint64_t window, TextSelectionInfo &selectionInfo)
{
    // Initialize AT-SPI on first use
    if (!atspi_initialized)
    {
        int rc = atspi_init();
        if (rc != 0 && rc != 1)  // 0 = success, 1 = already initialized
        {
            printf("[AT-SPI] Failed to initialize (rc=%d)\n", rc);
            return false;
        }
        atspi_initialized = true;
    }

    AtspiAccessible *desktop = atspi_get_desktop(0);
    if (!desktop)
        return false;

    // Search through all applications for a focused element with text selection
    GError *error = nullptr;
    int app_count = atspi_accessible_get_child_count(desktop, &error);
    if (error)
    {
        g_error_free(error);
        g_object_unref(desktop);
        return false;
    }

    AtspiAccessible *focused = nullptr;
    for (int i = 0; i < app_count; i++)
    {
        error = nullptr;
        AtspiAccessible *app = atspi_accessible_get_child_at_index(desktop, i, &error);
        if (error)
        {
            g_error_free(error);
            continue;
        }
        if (!app)
            continue;

        focused = FindFocusedAccessibleWithSelection(app, 0);
        g_object_unref(app);

        if (focused)
            break;
    }

    g_object_unref(desktop);

    if (!focused)
        return false;

    // Get text interface
    AtspiText *text_iface = atspi_accessible_get_text_iface(focused);
    if (!text_iface)
    {
        g_object_unref(focused);
        return false;
    }

    // Get selection range
    error = nullptr;
    AtspiRange *range = atspi_text_get_selection(text_iface, 0, &error);
    if (error || !range)
    {
        if (error)
            g_error_free(error);
        g_object_unref(text_iface);
        g_object_unref(focused);
        return false;
    }

    int start_offset = range->start_offset;
    int end_offset = range->end_offset;
    g_free(range);

    if (start_offset == end_offset)
    {
        g_object_unref(text_iface);
        g_object_unref(focused);
        return false;
    }

    // Get selected text
    error = nullptr;
    char *selected_text = atspi_text_get_text(text_iface, start_offset, end_offset, &error);
    if (error || !selected_text)
    {
        if (error)
            g_error_free(error);
        g_object_unref(text_iface);
        g_object_unref(focused);
        return false;
    }

    selectionInfo.text = std::string(selected_text);
    g_free(selected_text);

    if (selectionInfo.text.empty())
    {
        g_object_unref(text_iface);
        g_object_unref(focused);
        return false;
    }

    // Get text selection coordinates
    error = nullptr;
    AtspiRect *start_rect = atspi_text_get_character_extents(text_iface, start_offset,
                                                              ATSPI_COORD_TYPE_SCREEN, &error);
    if (!error && start_rect)
    {
        int last_char = (end_offset > start_offset) ? (end_offset - 1) : end_offset;
        GError *error2 = nullptr;
        AtspiRect *end_rect = atspi_text_get_character_extents(text_iface, last_char,
                                                                ATSPI_COORD_TYPE_SCREEN, &error2);
        if (!error2 && end_rect)
        {
            selectionInfo.startTop = Point(start_rect->x, start_rect->y);
            selectionInfo.startBottom = Point(start_rect->x, start_rect->y + start_rect->height);
            selectionInfo.endTop = Point(end_rect->x + end_rect->width, end_rect->y);
            selectionInfo.endBottom = Point(end_rect->x + end_rect->width, end_rect->y + end_rect->height);
            selectionInfo.posLevel = SelectionPositionLevel::Full;
        }
        if (error2)
            g_error_free(error2);
        if (end_rect)
            g_free(end_rect);
    }
    if (error)
        g_error_free(error);
    if (start_rect)
        g_free(start_rect);

    g_object_unref(text_iface);
    g_object_unref(focused);

    return !selectionInfo.text.empty();
}

/**
 * Get text selection via protocol selection APIs
 */
bool SelectionHook::GetTextViaPrimary(uint64_t window, TextSelectionInfo &selectionInfo)
{
    if (!window)
        return false;

    // Try to get text from primary selection
    std::string selectedText;
    if (protocol->GetTextViaPrimary(selectedText) && !selectedText.empty())
    {
        selectionInfo.text = selectedText;

        // Try to get coordinates
        // if (protocol->SetTextRangeCoordinates(window, selectionInfo))
        // {
        //     selectionInfo.posLevel = SelectionPositionLevel::Full;
        // }
        // else
        // {
        //     selectionInfo.posLevel = SelectionPositionLevel::None;
        // }

        return true;
    }

    return false;
}

/**
 * Get text using clipboard and Ctrl+C as a last resort
 */
bool SelectionHook::GetTextViaClipboard(uint64_t window, TextSelectionInfo &selectionInfo)
{
    if (!window)
        return false;

    // // Store current clipboard content to restore later
    // std::string originalContent;
    // bool hasOriginalContent = protocol->ReadClipboard(originalContent);

    // // Send Ctrl+C to copy selected text
    // protocol->SendCopyKey(CopyKeyType::CtrlC);

    // // Wait a bit for the copy operation to complete
    // std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // // Read the new clipboard content
    // std::string newContent;
    // if (!protocol->ReadClipboard(newContent) || newContent.empty())
    // {
    //     // Restore original clipboard if possible
    //     if (hasOriginalContent)
    //     {
    //         protocol->WriteClipboard(originalContent);
    //     }
    //     return false;
    // }

    // // Store the copied text
    // selectionInfo.text = newContent;

    // // Restore original clipboard content
    // if (hasOriginalContent && originalContent != newContent)
    // {
    //     protocol->WriteClipboard(originalContent);
    // }

    // return true;
    return false;
}

/**
 * Check if current system state allows text selection
 */
bool SelectionHook::ShouldProcessGetSelection()
{
    // TODO: Implement Linux-specific logic to check system state
    // For now, always return true
    return true;
}

/**
 * Check if we should process GetTextViaClipboard
 */
bool SelectionHook::ShouldProcessViaClipboard(const std::string &programName)
{
    if (!is_enabled_clipboard)
        return false;

    bool result = false;
    switch (clipboard_filter_mode)
    {
        case FilterMode::Default:
            result = true;
            break;
        case FilterMode::IncludeList:
            result = IsInFilterList(programName, clipboard_filter_list);
            break;
        case FilterMode::ExcludeList:
            result = !IsInFilterList(programName, clipboard_filter_list);
            break;
    }

    return result;
}

/**
 * Check if program name is in the filter list
 */
bool SelectionHook::IsInFilterList(const std::string &programName, const std::vector<std::string> &filterList)
{
    // If filter list is empty, allow all
    if (filterList.empty())
    {
        return false;
    }

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
 * Helper method to process string array and populate target list
 */
void SelectionHook::ProcessStringArrayToList(const Napi::Array &array, std::vector<std::string> &targetList)
{
    uint32_t length = array.Length();

    // Clear existing list
    targetList.clear();

    // Process each string in the array
    for (uint32_t i = 0; i < length; i++)
    {
        Napi::Value value = array.Get(i);
        if (value.IsString())
        {
            // Get the UTF-8 string
            std::string programName = value.As<Napi::String>().Utf8Value();

            // Convert to lowercase
            std::transform(programName.begin(), programName.end(), programName.begin(), ::tolower);

            // Add to the target list
            targetList.push_back(programName);
        }
    }
}

/**
 * Create JavaScript object with selection result
 */
Napi::Object SelectionHook::CreateSelectionResultObject(Napi::Env env, const TextSelectionInfo &selectionInfo)
{
    Napi::Object resultObj = Napi::Object::New(env);

    resultObj.Set(Napi::String::New(env, "type"), Napi::String::New(env, "text-selection"));
    resultObj.Set(Napi::String::New(env, "text"), Napi::String::New(env, selectionInfo.text));
    resultObj.Set(Napi::String::New(env, "programName"), Napi::String::New(env, selectionInfo.programName));

    // Add method and position level information
    resultObj.Set(Napi::String::New(env, "method"), Napi::Number::New(env, static_cast<int>(selectionInfo.method)));
    resultObj.Set(Napi::String::New(env, "posLevel"), Napi::Number::New(env, static_cast<int>(selectionInfo.posLevel)));

    // First paragraph left-top point (start position)
    resultObj.Set(Napi::String::New(env, "startTopX"), Napi::Number::New(env, selectionInfo.startTop.x));
    resultObj.Set(Napi::String::New(env, "startTopY"), Napi::Number::New(env, selectionInfo.startTop.y));

    // Last paragraph right-bottom point (end position)
    resultObj.Set(Napi::String::New(env, "endBottomX"), Napi::Number::New(env, selectionInfo.endBottom.x));
    resultObj.Set(Napi::String::New(env, "endBottomY"), Napi::Number::New(env, selectionInfo.endBottom.y));

    // First paragraph left-bottom point
    resultObj.Set(Napi::String::New(env, "startBottomX"), Napi::Number::New(env, selectionInfo.startBottom.x));
    resultObj.Set(Napi::String::New(env, "startBottomY"), Napi::Number::New(env, selectionInfo.startBottom.y));

    // Last paragraph right-top point
    resultObj.Set(Napi::String::New(env, "endTopX"), Napi::Number::New(env, selectionInfo.endTop.x));
    resultObj.Set(Napi::String::New(env, "endTopY"), Napi::Number::New(env, selectionInfo.endTop.y));

    // Mouse positions
    resultObj.Set(Napi::String::New(env, "mouseStartX"), Napi::Number::New(env, selectionInfo.mousePosStart.x));
    resultObj.Set(Napi::String::New(env, "mouseStartY"), Napi::Number::New(env, selectionInfo.mousePosStart.y));
    resultObj.Set(Napi::String::New(env, "mouseEndX"), Napi::Number::New(env, selectionInfo.mousePosEnd.x));
    resultObj.Set(Napi::String::New(env, "mouseEndY"), Napi::Number::New(env, selectionInfo.mousePosEnd.y));

    return resultObj;
}

/**
 * Input monitoring callback methods
 */
void SelectionHook::OnMouseEventCallback(void *context, MouseEventContext *mouseEvent)
{
    SelectionHook *instance = static_cast<SelectionHook *>(context);
    if (!instance || !mouseEvent || !instance->mouse_keyboard_running.load() || !instance->mouse_tsfn)
    {
        delete mouseEvent;
        return;
    }

    // Update current mouse position
    instance->current_mouse_pos = mouseEvent->pos;

    instance->mouse_tsfn.NonBlockingCall(mouseEvent, ProcessMouseEvent);
}

void SelectionHook::OnKeyboardEventCallback(void *context, KeyboardEventContext *keyboardEvent)
{
    SelectionHook *instance = static_cast<SelectionHook *>(context);
    if (!instance || !keyboardEvent || !instance->mouse_keyboard_running.load() || !instance->keyboard_tsfn)
    {
        delete keyboardEvent;
        return;
    }

    instance->keyboard_tsfn.NonBlockingCall(keyboardEvent, ProcessKeyboardEvent);
}

/**
 * Process mouse event on main thread and detect text selection (bidirectional trigger with XFixes)
 */
void SelectionHook::ProcessMouseEvent(Napi::Env env, Napi::Function function, MouseEventContext *pMouseEvent)
{
    if (!pMouseEvent || !currentInstance)
    {
        delete pMouseEvent;
        return;
    }

    // Get current time in milliseconds
    auto currentTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    Point currentPos = pMouseEvent->pos;
    auto mouseCode = pMouseEvent->code;
    auto mouseValue = pMouseEvent->value;
    MouseButton mouseButton = static_cast<MouseButton>(pMouseEvent->button);

    std::string mouseTypeStr = "";
    int mouseFlagValue = 0;

    // Process different mouse events based on libevdev codes
    switch (mouseCode)
    {
        case BTN_LEFT:
            if (mouseValue == 1)  // Press
            {
                mouseTypeStr = "mouse-down";
                mouseButton = MouseButton::Left;

                // Update mouse-down state
                currentInstance->last_mouse_down_time = currentTime;
                currentInstance->last_mouse_down_pos = currentPos;
                currentInstance->clipboard_sequence = 0;

                // Clear pending gesture (prevent old pending from being triggered by new action)
                currentInstance->pending_gesture.active = false;

                // Record window handle and position at mouse-down for movement detection
                currentInstance->last_window_handler = currentInstance->protocol->GetActiveWindow();
                if (currentInstance->last_window_handler)
                {
                    currentInstance->protocol->GetWindowRect(currentInstance->last_window_handler,
                                                             currentInstance->last_window_rect);
                }
            }
            else if (mouseValue == 0)  // Release
            {
                mouseTypeStr = "mouse-up";
                mouseButton = MouseButton::Left;

                // Update mouse-up state (save previous values first)
                Point prevUp = currentInstance->last_mouse_up_pos;
                uint64_t prevUpTime = currentInstance->last_mouse_up_time;
                currentInstance->last_mouse_up_time = currentTime;
                currentInstance->last_mouse_up_pos = currentPos;
                currentInstance->last_mouse_up_modifier_flags = currentInstance->protocol->GetModifierFlags();

                if (!currentInstance->is_selection_passive_mode)
                {
                    // Gesture detection
                    auto detectionType = SelectionDetectType::None;

                    double dx = currentPos.x - currentInstance->last_mouse_down_pos.x;
                    double dy = currentPos.y - currentInstance->last_mouse_down_pos.y;
                    double distance = sqrt(dx * dx + dy * dy);

                    bool isCurrentValidClick =
                        (currentTime - currentInstance->last_mouse_down_time) <= DOUBLE_CLICK_TIME_MS;

                    if ((currentTime - currentInstance->last_mouse_down_time) > MAX_DRAG_TIME_MS)
                    {
                        // Too long drag, skip
                    }
                    // Check for drag selection
                    else if (distance >= MIN_DRAG_DISTANCE)
                    {
                        uint64_t upWindow = currentInstance->protocol->GetActiveWindow();
                        if (upWindow && upWindow == currentInstance->last_window_handler)
                        {
                            WindowRect currentWindowRect;
                            currentInstance->protocol->GetWindowRect(upWindow, currentWindowRect);
                            if (!HasWindowMoved(currentWindowRect, currentInstance->last_window_rect))
                            {
                                detectionType = SelectionDetectType::Drag;
                            }
                        }
                    }
                    // Check for double-click selection
                    else if (currentInstance->is_last_valid_click && isCurrentValidClick &&
                             distance <= DOUBLE_CLICK_MAX_DISTANCE)
                    {
                        double dx2 = currentPos.x - prevUp.x;
                        double dy2 = currentPos.y - prevUp.y;
                        double distance2 = sqrt(dx2 * dx2 + dy2 * dy2);

                        if (distance2 <= DOUBLE_CLICK_MAX_DISTANCE &&
                            (currentInstance->last_mouse_down_time - prevUpTime) <= DOUBLE_CLICK_TIME_MS)
                        {
                            uint64_t upWindow = currentInstance->protocol->GetActiveWindow();
                            if (upWindow && upWindow == currentInstance->last_window_handler)
                            {
                                WindowRect currentWindowRect;
                                currentInstance->protocol->GetWindowRect(upWindow, currentWindowRect);
                                if (!HasWindowMoved(currentWindowRect, currentInstance->last_window_rect))
                                {
                                    detectionType = SelectionDetectType::DoubleClick;
                                }
                            }
                        }
                    }

                    // Check shift+click selection
                    if (detectionType == SelectionDetectType::None)
                    {
                        int modFlags = currentInstance->last_mouse_up_modifier_flags;
                        bool isShiftPressed = (modFlags & MODIFIER_SHIFT) != 0;
                        bool isCtrlPressed = (modFlags & MODIFIER_CTRL) != 0;
                        bool isAltPressed = (modFlags & MODIFIER_ALT) != 0;
                        if (isShiftPressed && !isCtrlPressed && !isAltPressed)
                        {
                            detectionType = SelectionDetectType::ShiftClick;
                        }
                    }

                    // Bidirectional trigger: correlate gesture with XFixes
                    if (detectionType != SelectionDetectType::None)
                    {
                        uint64_t lastXfixes = currentInstance->last_xfixes_time.load();

                        // Determine mouse coordinates for the event
                        Point gestureStart, gestureEnd;
                        switch (detectionType)
                        {
                            case SelectionDetectType::Drag:
                                gestureStart = currentInstance->last_mouse_down_pos;
                                gestureEnd = currentPos;
                                break;
                            case SelectionDetectType::DoubleClick:
                                gestureStart = currentPos;
                                gestureEnd = currentPos;
                                break;
                            case SelectionDetectType::ShiftClick:
                                gestureStart = currentInstance->prev_mouse_up_pos;
                                gestureEnd = currentPos;
                                break;
                            default:
                                gestureStart = currentPos;
                                gestureEnd = currentPos;
                                break;
                        }

                        if (lastXfixes > 0 &&
                            (static_cast<uint64_t>(currentTime) - lastXfixes) < CORRELATION_WINDOW_MS)
                        {
                            // Path A: XFixes already arrived, fast path
                            currentInstance->last_xfixes_time.store(0);  // Consume
                            currentInstance->EmitSelectionEvent(detectionType, gestureStart, gestureEnd);
                        }
                        else
                        {
                            // Path B: Set pending, wait for XFixes
                            currentInstance->pending_gesture.active = true;
                            currentInstance->pending_gesture.type = detectionType;
                            currentInstance->pending_gesture.mousePosStart = gestureStart;
                            currentInstance->pending_gesture.mousePosEnd = gestureEnd;
                            currentInstance->pending_gesture.timestamp = currentTime;
                        }
                    }

                    currentInstance->is_last_valid_click = isCurrentValidClick;
                }

                currentInstance->prev_mouse_up_pos = prevUp;
                currentInstance->prev_mouse_up_time = prevUpTime;
            }
            break;

        case BTN_RIGHT:
            mouseTypeStr = (mouseValue == 1) ? "mouse-down" : "mouse-up";
            mouseButton = MouseButton::Right;
            break;

        case BTN_MIDDLE:
            mouseTypeStr = (mouseValue == 1) ? "mouse-down" : "mouse-up";
            mouseButton = MouseButton::Middle;
            break;

        case REL_WHEEL:
            mouseTypeStr = "mouse-wheel";
            mouseButton = MouseButton::WheelVertical;
            mouseFlagValue = mouseValue > 0 ? 1 : -1;
            break;

        case REL_HWHEEL:
            mouseTypeStr = "mouse-wheel";
            mouseButton = MouseButton::WheelHorizontal;
            mouseFlagValue = mouseValue > 0 ? 1 : -1;
            break;

        default:
            if (mouseCode == REL_X || mouseCode == REL_Y)
            {
                mouseTypeStr = "mouse-move";
                mouseButton = MouseButton::None;
            }
            else
            {
                mouseTypeStr = "unknown";
                mouseButton = MouseButton::Unknown;
            }
            break;
    }

    // Create and emit mouse event object
    if (!mouseTypeStr.empty())
    {
        // Filter mouse move events based on the flag
        if (mouseTypeStr == "mouse-move" && !currentInstance->is_enabled_mouse_move_event)
        {
            delete pMouseEvent;
            return;
        }

        Napi::Object resultObj = Napi::Object::New(env);
        resultObj.Set(Napi::String::New(env, "type"), Napi::String::New(env, "mouse-event"));
        resultObj.Set(Napi::String::New(env, "action"), Napi::String::New(env, mouseTypeStr));
        resultObj.Set(Napi::String::New(env, "x"), Napi::Number::New(env, currentPos.x));
        resultObj.Set(Napi::String::New(env, "y"), Napi::Number::New(env, currentPos.y));
        resultObj.Set(Napi::String::New(env, "button"), Napi::Number::New(env, static_cast<int>(mouseButton)));
        resultObj.Set(Napi::String::New(env, "flag"), Napi::Number::New(env, mouseFlagValue));
        function.Call({resultObj});
    }

    delete pMouseEvent;
}

/**
 * XFixes selection event callback (called from XFixes thread)
 */
void SelectionHook::OnSelectionEventCallback(void *context, SelectionChangeContext *event)
{
    SelectionHook *instance = static_cast<SelectionHook *>(context);
    if (!instance || !event)
    {
        delete event;
        return;
    }

    // Atomic write - executed in XFixes thread, read by Path A in main thread
    instance->last_xfixes_time.store(event->timestamp_ms);

    // Dispatch to main thread for Path B processing
    if (instance->running.load() && instance->selection_tsfn)
    {
        instance->selection_tsfn.NonBlockingCall(event, ProcessSelectionEvent);
    }
    else
    {
        delete event;
    }
}

/**
 * Process selection event on main thread (Path B: XFixes arrives after mouse-up)
 */
void SelectionHook::ProcessSelectionEvent(Napi::Env env, Napi::Function function, SelectionChangeContext *pEvent)
{
    if (!pEvent || !currentInstance)
    {
        delete pEvent;
        return;
    }

    if (currentInstance->pending_gesture.active)
    {
        // Use XFixes event timestamp (not wall clock "now") to avoid false expiry
        // under main-thread load when ThreadSafeFunction callback is delayed
        if ((pEvent->timestamp_ms - currentInstance->pending_gesture.timestamp) < CORRELATION_WINDOW_MS)
        {
            // Path B: pending gesture confirmed by XFixes
            currentInstance->last_xfixes_time.store(0);  // Consume
            currentInstance->EmitSelectionEvent(currentInstance->pending_gesture.type,
                                                currentInstance->pending_gesture.mousePosStart,
                                                currentInstance->pending_gesture.mousePosEnd);
            currentInstance->pending_gesture.active = false;
        }
        else
        {
            // Pending expired, clear it
            currentInstance->pending_gesture.active = false;
        }
    }
    // else: no pending gesture, skip (keyboard selection or external PRIMARY change)

    delete pEvent;
}

/**
 * Emit text selection event (shared by Path A and Path B)
 */
void SelectionHook::EmitSelectionEvent(SelectionDetectType type, Point start, Point end)
{
    if (is_selection_passive_mode || is_processing.load())
        return;

    uint64_t activeWindow = protocol->GetActiveWindow();
    if (!activeWindow)
        return;

    TextSelectionInfo selectionInfo;
    if (!GetSelectedText(activeWindow, selectionInfo) || selectionInfo.text.empty())
        return;

    // Set coordinates and posLevel based on detection type
    switch (type)
    {
        case SelectionDetectType::Drag:
            selectionInfo.mousePosStart = start;
            selectionInfo.mousePosEnd = end;
            if (selectionInfo.posLevel == SelectionPositionLevel::None)
                selectionInfo.posLevel = SelectionPositionLevel::MouseDual;
            break;
        case SelectionDetectType::DoubleClick:
            selectionInfo.mousePosStart = start;
            selectionInfo.mousePosEnd = end;
            if (selectionInfo.posLevel == SelectionPositionLevel::None)
                selectionInfo.posLevel = SelectionPositionLevel::MouseSingle;
            break;
        case SelectionDetectType::ShiftClick:
            selectionInfo.mousePosStart = start;
            selectionInfo.mousePosEnd = end;
            if (selectionInfo.posLevel == SelectionPositionLevel::None)
                selectionInfo.posLevel = SelectionPositionLevel::MouseDual;
            break;
        default:
            break;
    }

    auto callback = [selectionInfo](Napi::Env env, Napi::Function jsCallback)
    {
        Napi::Object resultObj = currentInstance->CreateSelectionResultObject(env, selectionInfo);
        jsCallback.Call({resultObj});
    };

    if (running.load() && tsfn)
    {
        tsfn.NonBlockingCall(callback);
    }
}

/**
 * Process keyboard event on main thread
 */
void SelectionHook::ProcessKeyboardEvent(Napi::Env env, Napi::Function function, KeyboardEventContext *pKeyboardEvent)
{
    if (!pKeyboardEvent)
    {
        delete pKeyboardEvent;
        return;
    }

    auto keyCode = pKeyboardEvent->code;
    auto keyValue = pKeyboardEvent->value;
    auto keyFlags = pKeyboardEvent->flags;

    std::string eventTypeStr;

    // Determine event type
    switch (keyValue)
    {
        case 0:  // Key release
            eventTypeStr = "key-up";
            break;
        case 1:  // Key press
            eventTypeStr = "key-down";
            break;
        case 2:  // Key repeat
            eventTypeStr = "key-down";
            break;
        default:
            eventTypeStr = "unknown";
            break;
    }

    // Check if any system key (Ctrl, Alt, Super) is being pressed
    bool isSysKey = (keyFlags & MODIFIER_CTRL) || (keyFlags & MODIFIER_ALT) || (keyFlags & MODIFIER_META);

    // Convert Linux key code to universal key string (MDN KeyboardEvent.key)
    std::string uniKey = convertKeyCodeToUniKey(keyCode, keyFlags);

    // Create and emit keyboard event object
    if (!eventTypeStr.empty())
    {
        Napi::Object resultObj = Napi::Object::New(env);
        resultObj.Set(Napi::String::New(env, "type"), Napi::String::New(env, "keyboard-event"));
        resultObj.Set(Napi::String::New(env, "action"), Napi::String::New(env, eventTypeStr));
        resultObj.Set(Napi::String::New(env, "uniKey"), Napi::String::New(env, uniKey));
        resultObj.Set(Napi::String::New(env, "vkCode"), Napi::Number::New(env, keyCode));
        resultObj.Set(Napi::String::New(env, "sys"), Napi::Boolean::New(env, isSysKey));
        resultObj.Set(Napi::String::New(env, "flags"), Napi::Number::New(env, keyFlags));
        function.Call({resultObj});
    }

    delete pKeyboardEvent;
}

//=============================================================================
// Module Initialization
//=============================================================================

/**
 * Initialize the native module
 */
Napi::Object InitAll(Napi::Env env, Napi::Object exports)
{
    return SelectionHook::Init(env, exports);
}

// Register the module with Node.js
NODE_API_MODULE(selection_hook, InitAll)
