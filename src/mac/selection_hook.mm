/**
 * Text Selection Hook for macOS
 *
 * A Node Native Module that captures text selection events across applications
 * on macOS using Accessibility APIs (AX API).
 *
 * Main components:
 * - TextSelectionHook class: Core implementation of the module
 * - Text selection detection: Uses Accessibility API
 * - Event monitoring: Mac event system for input monitoring
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

#import <napi.h>

#import <atomic>
#import <string>
#import <thread>

#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>

#import "lib/clipboard.h"
#import "lib/keyboard.h"
#import "lib/utils.h"

// Mouse&Keyboard hook constants
constexpr int DEFAULT_MOUSE_EVENT_QUEUE_SIZE = 512;
constexpr int DEFAULT_KEYBOARD_EVENT_QUEUE_SIZE = 128;

// Mouse interaction constants
constexpr int MIN_DRAG_DISTANCE = 8;
constexpr uint64_t MAX_DRAG_TIME_MS = 15000;
constexpr int DOUBLE_CLICK_MAX_DISTANCE = 3;
static uint64_t DOUBLE_CLICK_TIME_MS = 500;

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
    AXAPI = 11,
    Clipboard = 99
};

/**
 * Position level enum for text selection tracking
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
    // Note: Wheel values are used only in scroll wheel event context
    // They share the same numeric values as Left/Middle but have different semantic meaning
    WheelVertical = 0,   // Same as Left, but used for vertical wheel events
    WheelHorizontal = 1  // Same as Middle, but used for horizontal wheel events
};

enum class FilterMode
{
    Default = 0,      // trigger anyway
    IncludeList = 1,  // only trigger when the program name is in the include list
    ExcludeList = 2   // only trigger when the program name is not in the exclude list
};

/**
 * Structure to store text selection information
 */
struct TextSelectionInfo
{
    std::string text;         ///< Selected text content (UTF-8)
    std::string programName;  ///< program name that triggered the selection

    CGPoint startTop;     ///< First paragraph left-top (screen coordinates)
    CGPoint startBottom;  ///< First paragraph left-bottom (screen coordinates)
    CGPoint endTop;       ///< Last paragraph right-top (screen coordinates)
    CGPoint endBottom;    ///< Last paragraph right-bottom (screen coordinates)

    CGPoint mousePosStart;  ///< Current mouse position (screen coordinates)
    CGPoint mousePosEnd;    ///< Mouse down position (screen coordinates)

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
 * Structure to store mouse event information
 */
struct MouseEventContext
{
    CGEventType type;  ///< Mac event type
    CGPoint pos;       ///< Mouse position
    int64_t button;    ///< Mouse button
    int64_t flag;      ///< Mouse extra flag (eg. wheel direction)
};

/**
 * Structure to store keyboard event information
 */
struct KeyboardEventContext
{
    CGEventType event;   ///< Mac event type
    CGKeyCode keyCode;   ///< Virtual key code
    CGEventFlags flags;  ///< Event flags
};

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
    Napi::Value MacIsProcessTrusted(const Napi::CallbackInfo &info);
    Napi::Value MacRequestProcessTrust(const Napi::CallbackInfo &info);
    Napi::Object CreateSelectionResultObject(Napi::Env env, const TextSelectionInfo &selectionInfo);

    // Core functionality methods
    bool GetSelectedText(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo);
    bool GetTextViaAXAPI(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo);
    bool GetTextViaClipboard(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo);
    bool ShouldProcessViaClipboard(const std::string &programName);
    // Get selected text from an accessibility element
    bool GetSelectedTextFromElement(AXUIElementRef element, std::string &text);
    // Set text range coordinates from an accessibility element
    bool SetTextRangeCoordinates(AXUIElementRef element, TextSelectionInfo &selectionInfo);

    bool IsInFilterList(const std::string &programName, const std::vector<std::string> &filterList);
    bool SendCopyKey(pid_t pid = 0);

    // Mouse and keyboard event handling methods
    void StartMouseKeyboardEventThread();
    void StopMouseKeyboardEventThread();
    void MouseKeyboardEventThreadProc();

    static CGEventRef MouseEventCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon);
    static CGEventRef KeyboardEventCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon);
    static void ProcessMouseEvent(Napi::Env env, Napi::Function function, MouseEventContext *mouseEvent);
    static void ProcessKeyboardEvent(Napi::Env env, Napi::Function function, KeyboardEventContext *keyboardEvent);

    // Thread communication
    Napi::ThreadSafeFunction tsfn;
    Napi::ThreadSafeFunction mouse_tsfn;
    Napi::ThreadSafeFunction keyboard_tsfn;

    std::atomic<bool> running{false};
    std::atomic<bool> mouse_keyboard_running{false};
    // current App(myself) process pid
    pid_t running_pid = 0;

    CFRunLoopRef eventRunLoop = nullptr;

    CFMachPortRef mouseEventTap = nullptr;
    CFMachPortRef keyboardEventTap = nullptr;
    CFRunLoopSourceRef mouseRunLoopSource = nullptr;
    CFRunLoopSourceRef keyboardRunLoopSource = nullptr;

    std::thread event_thread;

    bool is_triggered_by_user = false;  // user use GetCurrentSelection

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

    // the text selection is processing, we should ignore some events
    std::atomic<bool> is_processing{false};
};

// Static member initialization
Napi::FunctionReference SelectionHook::constructor;

// Static pointer for callbacks
static SelectionHook *currentInstance = nullptr;

/**
 * Constructor - initializes Mac accessibility
 */
SelectionHook::SelectionHook(const Napi::CallbackInfo &info) : Napi::ObjectWrap<SelectionHook>(info)
{
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);

    currentInstance = this;
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

    // Stop event monitoring
    StopMouseKeyboardEventThread();

    // Release thread-safe functions if they exist
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
                     InstanceMethod("macIsProcessTrusted", &SelectionHook::MacIsProcessTrusted),
                     InstanceMethod("macRequestProcessTrust", &SelectionHook::MacRequestProcessTrust)});

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

    // Don't start if already running - check both flags and thread state
    if (running || mouse_keyboard_running || event_thread.joinable())
    {
        Napi::Error::New(env, "Text selection hook is already running").ThrowAsJavaScriptException();
        return;
    }

    try
    {
        // Create thread-safe function from JavaScript callback
        Napi::Function callback = info[0u].As<Napi::Function>();

        // Create thread-safe function for text selection
        tsfn = Napi::ThreadSafeFunction::New(env, callback, "TextSelectionCallback", 0, 1,
                                             [this](Napi::Env) { running = false; });

        // Create thread-safe function for mouse events
        mouse_tsfn = Napi::ThreadSafeFunction::New(env,
                                                   callback,  // Same callback as text selection
                                                   "MouseEventCallback", DEFAULT_MOUSE_EVENT_QUEUE_SIZE, 1,
                                                   [this](Napi::Env) { mouse_keyboard_running = false; });

        // Create thread-safe function for keyboard events
        keyboard_tsfn = Napi::ThreadSafeFunction::New(env,
                                                      callback,  // Same callback as text selection
                                                      "KeyboardEventCallback", DEFAULT_KEYBOARD_EVENT_QUEUE_SIZE, 1,
                                                      [this](Napi::Env) { mouse_keyboard_running = false; });

        // Set flags before starting thread to ensure proper synchronization
        mouse_keyboard_running = true;
        running = true;

        // Enable mouse move events by default when starting
        is_enabled_mouse_move_event = false;

        // Start event monitoring - this should be last to ensure all resources are ready
        StartMouseKeyboardEventThread();
    }
    catch (const std::exception &e)
    {
        // Clean up on error
        running = false;
        mouse_keyboard_running = false;

        if (tsfn)
        {
            tsfn.Release();
        }
        if (mouse_tsfn)
        {
            mouse_tsfn.Release();
        }
        if (keyboard_tsfn)
        {
            keyboard_tsfn.Release();
        }

        Napi::Error::New(env, std::string("Failed to start text selection hook: ") + e.what())
            .ThrowAsJavaScriptException();
        return;
    }
}

/**
 * NAPI: Stop monitoring text selections
 */
void SelectionHook::Stop(const Napi::CallbackInfo &info)
{
    // Do nothing if not running
    if (!running && !mouse_keyboard_running && !event_thread.joinable())
    {
        return;
    }

    // Set running flags to false first to signal shutdown
    running = false;
    mouse_keyboard_running = false;

    // Stop event monitoring thread first (this will wait for thread to finish)
    StopMouseKeyboardEventThread();

    // Now safely release thread-safe functions after thread has stopped
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

    Napi::Array includeArray = info[1u].As<Napi::Array>();
    uint32_t length = includeArray.Length();

    // Clear existing list
    clipboard_filter_list.clear();

    // Process each string in the array
    for (uint32_t i = 0; i < length; i++)
    {
        Napi::Value value = includeArray.Get(i);
        if (value.IsString())
        {
            // Get the UTF-8 string
            std::string programName = value.As<Napi::String>().Utf8Value();

            // Convert to lowercase
            std::transform(programName.begin(), programName.end(), programName.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            // Add to the include list (store as UTF-8)
            clipboard_filter_list.push_back(programName);
        }
    }
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

    Napi::Array includeArray = info[1u].As<Napi::Array>();
    uint32_t length = includeArray.Length();

    // Clear existing list
    global_filter_list.clear();

    // Process each string in the array
    for (uint32_t i = 0; i < length; i++)
    {
        Napi::Value value = includeArray.Get(i);
        if (value.IsString())
        {
            // Get the UTF-8 string
            std::string programName = value.As<Napi::String>().Utf8Value();

            // Convert to lowercase
            std::transform(programName.begin(), programName.end(), programName.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            // Add to the include list (store as UTF-8)
            global_filter_list.push_back(programName);
        }
    }
}

/**
 * NAPI: Set fine-tuned list based on type
 * NOTE: Implement this when macOS needs it
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
        std::string utf8Text = info[0].As<Napi::String>().Utf8Value();

        // Write to clipboard using the extracted function
        bool result = WriteClipboard(utf8Text);
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
        bool result = ReadClipboard(clipboardContent);

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
 * NAPI: Check if the process is trusted for accessibility
 */
Napi::Value SelectionHook::MacIsProcessTrusted(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();

    try
    {
        // Check accessibility permissions
        bool isTrusted = AXIsProcessTrusted();
        return Napi::Boolean::New(env, isTrusted);
    }
    catch (const std::exception &e)
    {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

/**
 * NAPI: Try to request accessibility permissions
 * The return value indicates the current permission status, not the request result
 */
Napi::Value SelectionHook::MacRequestProcessTrust(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();

    try
    {
        // Request accessibility permissions
        // Note: This will show a dialog to the user, but it is not guaranteed by OS
        // see:
        // https://developer.apple.com/documentation/applicationservices/1459186-axisprocesstrustedwithoptions?language=objc
        CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault, (const void **)&kAXTrustedCheckOptionPrompt,
                                                     (const void **)&kCFBooleanTrue, 1, &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);
        bool isTrusted = AXIsProcessTrustedWithOptions(options);
        CFRelease(options);

        return Napi::Boolean::New(env, isTrusted);
    }
    catch (const std::exception &e)
    {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

/**
 * Check if program name is in the filter list
 */
bool SelectionHook::IsInFilterList(const std::string &programName, const std::vector<std::string> &filterList)
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
 * NAPI: Get the currently selected text from the active window
 */
Napi::Value SelectionHook::GetCurrentSelection(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();

    try
    {
        NSRunningApplication *frontApp = GetFrontApp();
        if (!frontApp)
        {
            return env.Null();
        }

        // Get selected text
        TextSelectionInfo selectionInfo;
        is_triggered_by_user = true;
        if (!GetSelectedText(frontApp, selectionInfo) || IsTrimmedEmpty(selectionInfo.text))
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
 * Get selected text from the active application using multiple methods
 */
bool SelectionHook::GetSelectedText(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo)
{
    if (!frontApp)
        return false;

    if (is_processing.load())
        return false;
    else
        is_processing.store(true);

    // Initialize structure
    selectionInfo.clear();

    if (!GetProgramNameFromFrontApp(frontApp, selectionInfo.programName))
    {
        // if bunldeId is not found,
        // Could be system process, command line tool, background app or unsigned app
        // We don't handle these cases
        return false;
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

    is_processing.store(false);

    if (result)
        selectionInfo.isFullscreen = IsWindowFullscreen(frontApp);

    return result;
}

/**
 * Get text selection via Accessibility API
 */
bool SelectionHook::GetTextViaAXAPI(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo)
{
    if (!frontApp)
    {
        return false;
    }

    AXUIElementRef appElement = GetAppElementFromFrontApp(frontApp);
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

    // if we can't get text by AXAPI, we have to do final try for special cases
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
 */
bool SelectionHook::GetTextViaClipboard(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo)
{
    // currently, we don't support clipboard of the same process, we will fix it in future time
    if (frontApp.processIdentifier == running_pid)
        return false;

    int64_t newClipboardSequence = GetClipboardSequence();

    // Check if clipboard sequence number has changed since mouse down
    // if it's changed, it means user has copied something, we can read it directly
    if (newClipboardSequence != clipboard_sequence)
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
    clipboard_sequence = newClipboardSequence;
    // Store current clipboard content to restore later
    std::string originalContent;
    bool hasOriginalContent = ReadClipboard(originalContent);

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
        if (newClipboardSequence != clipboard_sequence)
        {
            clipboardChanged = true;
            break;
        }
    }

    if (!clipboardChanged)
        return false;

    // Read the new clipboard content
    std::string newContent;
    if (!ReadClipboard(newContent) || IsTrimmedEmpty(newContent))
    {
        // Restore original clipboard if possible
        if (hasOriginalContent)
        {
            WriteClipboard(originalContent);
        }

        return false;
    }

    // Store the copied text
    selectionInfo.text = newContent;

    // Restore original clipboard content
    if (hasOriginalContent && originalContent != newContent)
    {
        WriteClipboard(originalContent);
    }

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

    if (!result)
        return false;

    return true;
}

/**
 * Send copy key combination (Cmd+C) using Core Graphics events
 *
 * @param pid The process ID to send the event to. If 0 (default not set),
 * the event will be sent to the current process.
 */
bool SelectionHook::SendCopyKey(pid_t pid)
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
 */
bool SelectionHook::GetSelectedTextFromElement(AXUIElementRef element, std::string &text)
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
    }

    return false;
}

/**
 * Get bounds/coordinates of a text selection from an accessibility element and set coordinates in selectionInfo
 */
bool SelectionHook::SetTextRangeCoordinates(AXUIElementRef element, TextSelectionInfo &selectionInfo)
{
    if (!element)
        return false;

    // Strategy 1: Try to get precise coordinates by splitting the selection into smaller ranges
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
    resultObj.Set(Napi::String::New(env, "isFullscreen"), Napi::Boolean::New(env, selectionInfo.isFullscreen));

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

    // Current mouse position
    resultObj.Set(Napi::String::New(env, "mouseStartX"), Napi::Number::New(env, selectionInfo.mousePosStart.x));
    resultObj.Set(Napi::String::New(env, "mouseStartY"), Napi::Number::New(env, selectionInfo.mousePosStart.y));

    resultObj.Set(Napi::String::New(env, "mouseEndX"), Napi::Number::New(env, selectionInfo.mousePosEnd.x));
    resultObj.Set(Napi::String::New(env, "mouseEndY"), Napi::Number::New(env, selectionInfo.mousePosEnd.y));

    return resultObj;
}

/**
 * Start event thread
 */
void SelectionHook::StartMouseKeyboardEventThread()
{
    if (event_thread.joinable())
    {
        return;  // Already running
    }

    event_thread = std::thread(&SelectionHook::MouseKeyboardEventThreadProc, this);
}

/**
 * Stop event thread
 */
void SelectionHook::StopMouseKeyboardEventThread()
{
    // Check if thread is joinable before attempting to stop
    if (!event_thread.joinable())
    {
        eventRunLoop = nullptr;
        mouseEventTap = nullptr;
        keyboardEventTap = nullptr;
        mouseRunLoopSource = nullptr;
        keyboardRunLoopSource = nullptr;
        return;
    }

    // Stop the run loop to allow the thread to exit
    // CFRunLoopStop is thread-safe
    if (eventRunLoop)
    {
        CFRunLoopStop(eventRunLoop);
    }

    // Wait for the event thread to finish
    event_thread.join();
}

/**
 * Event thread function - runs the event monitoring loop
 */
void SelectionHook::MouseKeyboardEventThreadProc()
{
    // Store the run loop reference for cleanup
    eventRunLoop = CFRunLoopGetCurrent();

    // Create mouse event tap for global mouse monitoring
    CGEventMask mouseMask = CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
                            CGEventMaskBit(kCGEventLeftMouseDragged) | CGEventMaskBit(kCGEventRightMouseDown) |
                            CGEventMaskBit(kCGEventRightMouseUp) | CGEventMaskBit(kCGEventRightMouseDragged) |
                            CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) |
                            CGEventMaskBit(kCGEventOtherMouseDragged) | CGEventMaskBit(kCGEventMouseMoved) |
                            CGEventMaskBit(kCGEventScrollWheel);

    mouseEventTap = CGEventTapCreate(kCGSessionEventTap, kCGTailAppendEventTap, kCGEventTapOptionListenOnly, mouseMask,
                                     MouseEventCallback, this);

    if (mouseEventTap)
    {
        mouseRunLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, mouseEventTap, 0);
        CFRunLoopAddSource(eventRunLoop, mouseRunLoopSource, kCFRunLoopDefaultMode);
        CGEventTapEnable(mouseEventTap, true);
    }

    // Create keyboard event tap for global keyboard monitoring
    CGEventMask keyboardMask =
        CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged);

    keyboardEventTap = CGEventTapCreate(kCGSessionEventTap, kCGTailAppendEventTap, kCGEventTapOptionListenOnly,
                                        keyboardMask, KeyboardEventCallback, this);

    if (keyboardEventTap)
    {
        keyboardRunLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, keyboardEventTap, 0);
        CFRunLoopAddSource(eventRunLoop, keyboardRunLoopSource, kCFRunLoopDefaultMode);
        CGEventTapEnable(keyboardEventTap, true);
    }

    // save current process pid
    running_pid = getpid();

    // Run the event loop - this will block until CFRunLoopStop() is called
    CFRunLoopRun();

    // Cleanup after run loop finishes
    if (mouseEventTap)
    {
        CGEventTapEnable(mouseEventTap, false);
        CFRunLoopRemoveSource(eventRunLoop, mouseRunLoopSource, kCFRunLoopDefaultMode);
        CFRelease(mouseEventTap);
        CFRelease(mouseRunLoopSource);
        mouseEventTap = nullptr;
        mouseRunLoopSource = nullptr;
    }

    if (keyboardEventTap)
    {
        CGEventTapEnable(keyboardEventTap, false);
        CFRunLoopRemoveSource(eventRunLoop, keyboardRunLoopSource, kCFRunLoopDefaultMode);
        CFRelease(keyboardEventTap);
        CFRelease(keyboardRunLoopSource);
        keyboardEventTap = nullptr;
        keyboardRunLoopSource = nullptr;
    }

    eventRunLoop = nullptr;
}

/**
 * Process mouse event on main thread and detect text selection
 */
void SelectionHook::ProcessMouseEvent(Napi::Env env, Napi::Function function, MouseEventContext *pMouseEvent)
{
    if (!pMouseEvent)
    {
        return;
    }

    // Get current time in milliseconds
    auto currentTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    CGPoint currentPos = pMouseEvent->pos;
    auto mouseType = pMouseEvent->type;
    MouseButton mouseButton = static_cast<MouseButton>(pMouseEvent->button);
    auto mouseFlag = pMouseEvent->flag;

    // Static variables for tracking mouse events
    static CGPoint lastLastMouseUpPos = CGPointZero;  // Last last mouse up position
    static CGPoint lastMouseUpPos = CGPointZero;      // Last mouse up position
    static uint64_t lastMouseUpTime = 0;              // Last mouse up time
    static CGPoint lastMouseDownPos = CGPointZero;    // Last mouse down position
    static uint64_t lastMouseDownTime = 0;            // Last mouse down time
    static bool isLastValidClick = false;
    static bool isLastMouseDownValidCursor = false;

    bool shouldDetectSelection = false;

    auto detectionType = SelectionDetectType::None;  // 0=not set, 1=drag, 2=double click, 3=shift click
    std::string mouseTypeStr = "";
    int mouseFlagValue = 0;

    // Process different mouse events
    switch (mouseType)
    {
        case kCGEventLeftMouseDown:
        {
            mouseTypeStr = "mouse-down";

            lastMouseDownTime = currentTime;
            lastMouseDownPos = currentPos;
            isLastMouseDownValidCursor = isIBeamCursor([NSCursor currentSystemCursor]);
            currentInstance->clipboard_sequence = GetClipboardSequence();
            break;
        }
        case kCGEventLeftMouseUp:
        {
            mouseTypeStr = "mouse-up";

            if (!currentInstance->is_selection_passive_mode)
            {
                // Calculate distance between current position and mouse down position
                double dx = currentPos.x - lastMouseDownPos.x;
                double dy = currentPos.y - lastMouseDownPos.y;
                double distance = sqrt(dx * dx + dy * dy);

                bool isCurrentValidClick = (currentTime - lastMouseDownTime) <= DOUBLE_CLICK_TIME_MS;
                bool isValidCursor = isLastMouseDownValidCursor || isIBeamCursor([NSCursor currentSystemCursor]);

                if ((currentTime - lastMouseDownTime) > MAX_DRAG_TIME_MS)
                {
                    shouldDetectSelection = false;
                }
                // Check for drag selection
                else if (distance >= MIN_DRAG_DISTANCE)
                {
                    // Only support IBeamCursor for now
                    if (isValidCursor)
                    {
                        shouldDetectSelection = true;
                        detectionType = SelectionDetectType::Drag;
                    }
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
                        // Only support IBeamCursor for now
                        if (isValidCursor)
                        {
                            shouldDetectSelection = true;
                            detectionType = SelectionDetectType::DoubleClick;
                        }
                    }
                }

                // Check if shift key is pressed when mouse up, it's a way to select text
                if (!shouldDetectSelection)
                {
                    // Get current event flags to check for shift key
                    CGEventRef currentEvent = CGEventCreate(nullptr);
                    if (currentEvent)
                    {
                        CGEventFlags flags = CGEventGetFlags(currentEvent);
                        bool isShiftPressed = (flags & kCGEventFlagMaskShift) != 0;
                        bool isCtrlPressed = (flags & kCGEventFlagMaskControl) != 0;
                        bool isCmdPressed = (flags & kCGEventFlagMaskCommand) != 0;
                        bool isOptionPressed = (flags & kCGEventFlagMaskAlternate) != 0;

                        if (isShiftPressed && !isCtrlPressed && !isCmdPressed && !isOptionPressed)
                        {
                            // Only support IBeamCursor for now
                            if (isValidCursor)
                            {
                                shouldDetectSelection = true;
                                detectionType = SelectionDetectType::ShiftClick;
                            }
                        }
                        CFRelease(currentEvent);
                    }
                }
                isLastValidClick = isCurrentValidClick;
            }

            lastLastMouseUpPos = lastMouseUpPos;
            lastMouseUpTime = currentTime;
            lastMouseUpPos = currentPos;
            break;
        }

        case kCGEventRightMouseDown:
        case kCGEventOtherMouseDown:
            mouseTypeStr = "mouse-down";
            break;

        case kCGEventRightMouseUp:
        case kCGEventOtherMouseUp:
            mouseTypeStr = "mouse-up";
            break;

        case kCGEventMouseMoved:
        case kCGEventLeftMouseDragged:
        case kCGEventRightMouseDragged:
        case kCGEventOtherMouseDragged:
            mouseTypeStr = "mouse-move";
            break;

        case kCGEventScrollWheel:
            mouseTypeStr = "mouse-wheel";
            mouseFlagValue = mouseFlag;  // Use the flag value from the event
            break;

        default:
            mouseTypeStr = "unknown";
            break;
    }

    // Check for text selection
    if (shouldDetectSelection)
    {
        TextSelectionInfo selectionInfo;
        NSRunningApplication *frontApp = GetFrontApp();

        if (currentInstance->GetSelectedText(frontApp, selectionInfo) && !IsTrimmedEmpty(selectionInfo.text))
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
                case SelectionDetectType::ShiftClick:
                {
                    selectionInfo.mousePosStart = lastLastMouseUpPos;
                    selectionInfo.mousePosEnd = lastMouseUpPos;

                    if (selectionInfo.posLevel == SelectionPositionLevel::None)
                        selectionInfo.posLevel = SelectionPositionLevel::MouseDual;
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

            currentInstance->tsfn.NonBlockingCall(callback);
        }
    }

    // Create and emit mouse event object
    if (!mouseTypeStr.empty())
    {
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
        return;
    }

    // Add event type string
    std::string eventTypeStr;
    CGKeyCode vkCode = pKeyboardEvent->keyCode;

    switch (pKeyboardEvent->event)
    {
        case kCGEventKeyDown:
            eventTypeStr = "key-down";
            break;
        case kCGEventKeyUp:
            eventTypeStr = "key-up";
            break;
        case kCGEventFlagsChanged:
        {
            // For flags changed events, we need to track previous flags state
            // to determine which modifier key was pressed or released
            static CGEventFlags previousFlags = 0;
            CGEventFlags currentFlags = pKeyboardEvent->flags;
            CGEventFlags changedFlags = currentFlags ^ previousFlags;

            // Determine which modifier key changed and map to virtual key code
            // Based on Carbon HIToolbox/Events.h definitions
            if (changedFlags & kCGEventFlagMaskCommand)
            {
                vkCode = kVK_Command;
                eventTypeStr = (currentFlags & kCGEventFlagMaskCommand) ? "key-down" : "key-up";
            }
            else if (changedFlags & kCGEventFlagMaskShift)
            {
                vkCode = kVK_Shift;
                eventTypeStr = (currentFlags & kCGEventFlagMaskShift) ? "key-down" : "key-up";
            }
            else if (changedFlags & kCGEventFlagMaskAlternate)
            {
                vkCode = kVK_Option;
                eventTypeStr = (currentFlags & kCGEventFlagMaskAlternate) ? "key-down" : "key-up";
            }
            else if (changedFlags & kCGEventFlagMaskControl)
            {
                vkCode = kVK_Control;
                eventTypeStr = (currentFlags & kCGEventFlagMaskControl) ? "key-down" : "key-up";
            }
            else if (changedFlags & kCGEventFlagMaskSecondaryFn)
            {
                vkCode = kVK_Function;
                eventTypeStr = (currentFlags & kCGEventFlagMaskSecondaryFn) ? "key-down" : "key-up";
            }
            else if (changedFlags & kCGEventFlagMaskAlphaShift)
            {
                vkCode = kVK_CapsLock;
                // CapsLock is a toggle key, so we treat it differently
                // When CapsLock changes, we consider it as key-down
                eventTypeStr = "key-down";
            }
            else
            {
                // If we can't determine which modifier changed, we just return
                // This might happen in edge cases or when multiple modifiers change simultaneously
                delete pKeyboardEvent;
                return;
            }

            // Update previous flags for next comparison
            previousFlags = currentFlags;
            break;
        }
        default:
            delete pKeyboardEvent;
            return;
    }

    // Check if ctrl/cmd/options/fn key was pressed
    bool isSysKey = (pKeyboardEvent->flags & (kCGEventFlagMaskCommand | kCGEventFlagMaskControl |
                                              kCGEventFlagMaskAlternate | kCGEventFlagMaskSecondaryFn)) != 0;

    // Cost: ~1us
    std::string uniKey = convertKeyCodeToUniKey(vkCode, pKeyboardEvent->flags);

    // Create JavaScript object for keyboard event
    Napi::Object keyboardEventObj = Napi::Object::New(env);
    keyboardEventObj.Set(Napi::String::New(env, "type"), Napi::String::New(env, "keyboard-event"));
    keyboardEventObj.Set(Napi::String::New(env, "action"), Napi::String::New(env, eventTypeStr));
    keyboardEventObj.Set(Napi::String::New(env, "uniKey"), Napi::String::New(env, uniKey));
    keyboardEventObj.Set(Napi::String::New(env, "vkCode"), Napi::Number::New(env, vkCode));
    keyboardEventObj.Set(Napi::String::New(env, "sys"), Napi::Boolean::New(env, isSysKey));
    keyboardEventObj.Set(Napi::String::New(env, "flags"), Napi::Number::New(env, pKeyboardEvent->flags));
    function.Call({keyboardEventObj});

    delete pKeyboardEvent;
}

/**
 * Mouse event callback
 */
CGEventRef SelectionHook::MouseEventCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon)
{
    SelectionHook *hook = static_cast<SelectionHook *>(refcon);
    if (!hook || !hook->mouse_keyboard_running || !hook->mouse_tsfn)
        return event;

    // Skip mouse move events if disabled to reduce CPU usage
    if (type == kCGEventMouseMoved && !hook->is_enabled_mouse_move_event)
        return event;

    // Get additional mouse data
    int64_t button = -1;
    int64_t flag = 0;

    switch (type)
    {
        case kCGEventScrollWheel:
        {
            int64_t deltaY = CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1);
            int64_t deltaX = CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis2);

            // Priority: vertical scrolling takes precedence over horizontal
            if (deltaY != 0)
            {
                button = 0;  // vertical
                flag = (deltaY > 0) ? 1 : -1;
            }
            else if (deltaX != 0)
            {
                button = 1;  // horizontal
                flag = (deltaX > 0) ? 1 : -1;
            }
            else
            {
                // No scrolling detected, skip this event
                return event;
            }
            break;
        }
        case kCGEventLeftMouseDown:
        case kCGEventRightMouseDown:
        case kCGEventOtherMouseDown:
        case kCGEventLeftMouseUp:
        case kCGEventRightMouseUp:
        case kCGEventOtherMouseUp:
            button = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
            break;

        case kCGEventLeftMouseDragged:
        case kCGEventRightMouseDragged:
        case kCGEventOtherMouseDragged:
            button = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
            break;

        default:
            break;
    }

    // Create mouse event context
    MouseEventContext *mouseEventCtx =
        new MouseEventContext{.type = type, .pos = CGEventGetLocation(event), .button = button, .flag = flag};

    // Send to main thread for processing
    hook->mouse_tsfn.NonBlockingCall(mouseEventCtx, ProcessMouseEvent);

    return event;
}

/**
 * Keyboard event callback
 */
CGEventRef SelectionHook::KeyboardEventCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon)
{
    SelectionHook *hook = static_cast<SelectionHook *>(refcon);
    if (!hook || !hook->mouse_keyboard_running || !hook->keyboard_tsfn)
        return event;

    // Get key code
    CGKeyCode keyCode = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);

    // Get modifier flags
    CGEventFlags flags = CGEventGetFlags(event);

    // Create keyboard event context
    KeyboardEventContext *keyboardEventCtx =
        new KeyboardEventContext{.event = type, .keyCode = keyCode, .flags = flags};

    // Send to main thread for processing
    hook->keyboard_tsfn.NonBlockingCall(keyboardEventCtx, ProcessKeyboardEvent);

    return event;
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
