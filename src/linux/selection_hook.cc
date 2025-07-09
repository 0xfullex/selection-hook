/**
 * Text Selection Hook for Linux
 *
 * A Node Native Module that captures text selection events across applications
 * on Linux using X11/Wayland libraries.
 *
 * Main components:
 * - TextSelectionHook class: Core implementation of the module
 * - Text selection detection: Uses X11/Wayland APIs
 * - Event monitoring: Linux event system for input monitoring
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

// Headers for input monitoring - now handled by protocol layer

// Linux input constants for event processing
#include <linux/input.h>

// Undefine X11 None macro that conflicts with our enum
#ifdef None
#undef None
#endif

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
    bool GetTextViaPrimary(uint64_t window, TextSelectionInfo &selectionInfo);
    bool GetTextViaClipboard(uint64_t window, TextSelectionInfo &selectionInfo);
    bool ShouldProcessGetSelection();
    bool ShouldProcessViaClipboard(const std::string &programName);
    Napi::Object CreateSelectionResultObject(Napi::Env env, const TextSelectionInfo &selectionInfo);

    // Helper methods
    bool IsInFilterList(const std::string &programName, const std::vector<std::string> &filterList);
    void ProcessStringArrayToList(const Napi::Array &array, std::vector<std::string> &targetList);

    // Mouse and keyboard event handling methods
    static void ProcessMouseEvent(Napi::Env env, Napi::Function function, MouseEventContext *mouseEvent);
    static void ProcessKeyboardEvent(Napi::Env env, Napi::Function function, KeyboardEventContext *keyboardEvent);

    // Input monitoring callback methods
    static void OnMouseEventCallback(void *context, MouseEventContext *mouseEvent);
    static void OnKeyboardEventCallback(void *context, KeyboardEventContext *keyboardEvent);

    // Protocol interface for X11/Wayland abstraction
    std::unique_ptr<ProtocolBase> protocol;

    // Current display protocol (X11 or Wayland)
    DisplayProtocol current_display_protocol;

    // Mouse position tracking
    Point current_mouse_pos;

    // Thread communication
    Napi::ThreadSafeFunction tsfn;
    Napi::ThreadSafeFunction mouse_tsfn;
    Napi::ThreadSafeFunction keyboard_tsfn;

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
    if (tsfn || mouse_tsfn || keyboard_tsfn)
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

    // Initialize input monitoring via protocol
    if (!protocol->InitializeInputMonitoring(&SelectionHook::OnMouseEventCallback,
                                             &SelectionHook::OnKeyboardEventCallback, this))
    {
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

    // First try selection (primary selection)
    if (GetTextViaPrimary(window, selectionInfo))
    {
        selectionInfo.method = SelectionMethod::Primary;
        is_processing.store(false);
        return true;
    }

    // Last resort: try to get text using clipboard and Ctrl+C if enabled
    // if (ShouldProcessViaClipboard(selectionInfo.programName) && GetTextViaClipboard(window, selectionInfo))
    // {
    //     selectionInfo.method = SelectionMethod::Clipboard;
    //     is_processing.store(false);
    //     return true;
    // }

    is_processing.store(false);
    return false;
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
 * Process mouse event on main thread and detect text selection
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
    auto mouseType = pMouseEvent->type;
    auto mouseCode = pMouseEvent->code;
    auto mouseValue = pMouseEvent->value;
    MouseButton mouseButton = static_cast<MouseButton>(pMouseEvent->button);
    auto mouseFlag = pMouseEvent->flag;

    // Static variables for tracking mouse events
    static Point lastLastMouseUpPos = Point(0, 0);
    static Point lastMouseUpPos = Point(0, 0);
    static uint64_t lastMouseUpTime = 0;
    static Point lastMouseDownPos = Point(0, 0);
    static uint64_t lastMouseDownTime = 0;
    static bool isLastValidClick = false;

    bool shouldDetectSelection = false;
    auto detectionType = SelectionDetectType::None;
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

                lastMouseDownTime = currentTime;
                lastMouseDownPos = currentPos;
                currentInstance->clipboard_sequence = 0;  // TODO: Implement clipboard sequence
            }
            else if (mouseValue == 0)  // Release
            {
                mouseTypeStr = "mouse-up";
                mouseButton = MouseButton::Left;

                if (!currentInstance->is_selection_passive_mode)
                {
                    // Calculate distance between current position and mouse down position
                    double dx = currentPos.x - lastMouseDownPos.x;
                    double dy = currentPos.y - lastMouseDownPos.y;
                    double distance = sqrt(dx * dx + dy * dy);

                    bool isCurrentValidClick = (currentTime - lastMouseDownTime) <= DOUBLE_CLICK_TIME_MS;

                    if ((currentTime - lastMouseDownTime) > MAX_DRAG_TIME_MS)
                    {
                        shouldDetectSelection = false;
                    }
                    // Check for drag selection
                    else if (distance >= MIN_DRAG_DISTANCE)
                    {
                        shouldDetectSelection = true;
                        detectionType = SelectionDetectType::Drag;
                    }
                    // Check for double-click selection
                    else if (isLastValidClick && isCurrentValidClick && distance <= DOUBLE_CLICK_MAX_DISTANCE)
                    {
                        double dx2 = currentPos.x - lastMouseUpPos.x;
                        double dy2 = currentPos.y - lastMouseUpPos.y;
                        double distance2 = sqrt(dx2 * dx2 + dy2 * dy2);

                        if (distance2 <= DOUBLE_CLICK_MAX_DISTANCE &&
                            (lastMouseDownTime - lastMouseUpTime) <= DOUBLE_CLICK_TIME_MS)
                        {
                            shouldDetectSelection = true;
                            detectionType = SelectionDetectType::DoubleClick;
                        }
                    }

                    isLastValidClick = isCurrentValidClick;
                }

                lastLastMouseUpPos = lastMouseUpPos;
                lastMouseUpTime = currentTime;
                lastMouseUpPos = currentPos;
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

    // Check for text selection
    if (shouldDetectSelection)
    {
        printf("shouldDetectSelection: %d\n", shouldDetectSelection);

        TextSelectionInfo selectionInfo;
        uint64_t activeWindow = currentInstance->protocol->GetActiveWindow();

        if (currentInstance->GetSelectedText(activeWindow, selectionInfo) && !selectionInfo.text.empty())
        {
            switch (detectionType)
            {
                case SelectionDetectType::Drag:
                {
                    selectionInfo.mousePosStart = lastMouseDownPos;
                    selectionInfo.mousePosEnd = lastMouseUpPos;

                    if (selectionInfo.posLevel == SelectionPositionLevel::None)
                        selectionInfo.posLevel = SelectionPositionLevel::MouseDual;
                    break;
                }
                case SelectionDetectType::DoubleClick:
                {
                    selectionInfo.mousePosStart = lastMouseUpPos;
                    selectionInfo.mousePosEnd = lastMouseUpPos;

                    if (selectionInfo.posLevel == SelectionPositionLevel::None)
                        selectionInfo.posLevel = SelectionPositionLevel::MouseSingle;
                    break;
                }
                default:
                    break;
            }

            auto callback = [selectionInfo](Napi::Env env, Napi::Function jsCallback)
            {
                Napi::Object resultObj = currentInstance->CreateSelectionResultObject(env, selectionInfo);
                jsCallback.Call({resultObj});
            };

            // Check if still running before calling ThreadSafeFunction
            if (currentInstance->running.load() && currentInstance->tsfn)
            {
                currentInstance->tsfn.NonBlockingCall(callback);
            }
        }
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
 * Process keyboard event on main thread
 */
void SelectionHook::ProcessKeyboardEvent(Napi::Env env, Napi::Function function, KeyboardEventContext *pKeyboardEvent)
{
    if (!pKeyboardEvent)
    {
        delete pKeyboardEvent;
        return;
    }

    auto keyType = pKeyboardEvent->type;
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
    // TODO: Implement proper modifier key state tracking
    bool isSysKey = false;

    // TODO: Convert Linux key code to universal key string
    std::string uniKey = "";

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
