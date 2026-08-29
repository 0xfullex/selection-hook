/**
 * Text Selection Hook for macOS
 *
 * A Node Native Module that captures text selection events across applications
 * on macOS using Accessibility APIs (AX API).
 *
 * Main components:
 * - SelectionHook class (exported to JavaScript as TextSelectionHook): N-API
 *   surface, lifecycle, gesture detection, and result bridging
 * - Text selection detection: Uses Accessibility API (pipeline in core/engine.mm)
 * - Event monitoring: Mac event system for input monitoring
 * - Thread management: Background threads for hooks with thread-safe callbacks
 *
 * Threads:
 * - JS thread: every N-API method, ProcessMouseEvent/ProcessKeyboardEvent (TSFN
 *   callbacks), and the whole retrieval pipeline. Unlike Windows there is no
 *   worker thread; see the note above GetSelectedText() in ProcessMouseEvent.
 * - Event thread: MouseKeyboardEventThreadProc() owns a CFRunLoop with two
 *   listen-only CGEventTaps. Tap callbacks must not touch N-API or block; they
 *   snapshot event-time state into a context and hand it to a TSFN.
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
#import <future>
#import <string>
#import <thread>

#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>

#import "core/engine.h"
#import "core/types.h"
#import "lib/clipboard.h"
#import "lib/keyboard.h"
#import "lib/utils.h"

// Mouse&Keyboard hook constants
// TSFN queue depths. When a queue is full, NonBlockingCall fails and the tap callback
// drops that event instead of blocking the event thread (see MouseEventCallback).
constexpr int DEFAULT_MOUSE_EVENT_QUEUE_SIZE = 512;
constexpr int DEFAULT_KEYBOARD_EVENT_QUEUE_SIZE = 128;

// Mouse interaction constants (distances in screen points, times in ms).
// A mouse-up is classified as a selection gesture only when it satisfies these
// thresholds AND the cursor was an I-beam at mouse-down or mouse-up time.
constexpr int MIN_DRAG_DISTANCE = 8;          // Below this a press-release is a click, not a drag.
constexpr uint64_t MAX_DRAG_TIME_MS = 15000;  // A button held longer than this is not a selection.
constexpr int DOUBLE_CLICK_MAX_DISTANCE = 3;  // Max travel between the two clicks of a double-click.
static uint64_t DOUBLE_CLICK_TIME_MS = 500;   // Fixed; [NSEvent doubleClickInterval] is not consulted.

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

/**
 * Structure to store mouse event information
 */
struct MouseEventContext
{
    CGEventType type;           ///< Mac event type
    CGPoint pos;                ///< Mouse position
    int64_t button;             ///< Mouse button
    int64_t flag;               ///< Mouse extra flag (eg. wheel direction)
    CGEventFlags evFlags;       ///< Modifier flags captured at event time
    uint64_t timestampMs;       ///< Event time in ms (monotonic, since system boot),
                                ///< captured at event time so gesture timing is not
                                ///< distorted by main thread stalls
    bool cursorIBeam;           ///< Whether the system cursor was an I-beam at event time.
                                ///< Only sampled on left button down/up, false otherwise
    int64_t clipboardSequence;  ///< Clipboard change count at event time.
                                ///< Only sampled on left button down, -1 otherwise
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
/**
 * @brief macOS N-API wrapper and lifecycle coordinator.
 *
 * Event tap callbacks capture event-time state, the wrapper classifies gestures on the
 * JS thread and bridges results to JS, and core/ owns text retrieval. Both automatic
 * (gesture) and synchronous (getCurrentSelection) retrieval run on the JS thread.
 *
 * @warning A single static currentInstance backs the TSFN callbacks. Creating two live
 * instances is not supported: the second constructor silently repoints callbacks.
 */
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
    SelectionConfigSnapshot BuildSelectionConfigSnapshot() const;

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

    // running: selection TSFN is alive. mouse_keyboard_running: input TSFNs are alive
    // and tap callbacks may enqueue. Both are also cleared by the TSFN finalizers.
    std::atomic<bool> running{false};
    std::atomic<bool> mouse_keyboard_running{false};
    // current App(myself) process pid
    // Assigned on the event thread in MouseKeyboardEventThreadProc(), so it is 0 until
    // start() has run. Copied into SelectionConfigSnapshot::runningPid so the clipboard
    // fallback never sends Cmd+C to this process.
    pid_t running_pid = 0;

    CFRunLoopRef eventRunLoop = nullptr;
    std::promise<CFRunLoopRef> eventRunLoopPromise;
    std::future<CFRunLoopRef> eventRunLoopFuture;

    CFMachPortRef mouseEventTap = nullptr;
    CFMachPortRef keyboardEventTap = nullptr;
    CFRunLoopSourceRef mouseRunLoopSource = nullptr;
    CFRunLoopSourceRef keyboardRunLoopSource = nullptr;

    std::thread event_thread;

    // user use GetCurrentSelection
    // Set around getCurrentSelection() for parity with Windows, where the engine uses it
    // to relax cursor-shape checks. The macOS pipeline does not read it today.
    bool is_triggered_by_user = false;

    // Off by default; enableMouseMoveEvent() opts in. Checked on the event thread by
    // MouseEventCallback() to skip kCGEventMouseMoved before any allocation happens.
    bool is_enabled_mouse_move_event = false;

    // passive mode: only trigger when user call getCurrentSelection()
    // Gesture detection still tracks mouse state so a later switch back to active mode
    // does not start from stale positions; it only skips retrieval.
    bool is_selection_passive_mode = false;
    bool is_enabled_clipboard = true;  // Enable by default
    // Store clipboard sequence number when mouse down
    // NSPasteboard changeCount baseline for the clipboard fallback. Written from the
    // mouse-down snapshot in ProcessMouseEvent(), then refreshed by GetSelectedText()
    // after the fallback restores the pasteboard, so a change made by this library is
    // never mistaken for a copy made by the user.
    int64_t clipboard_sequence = 0;

    // clipboard filter mode
    FilterMode clipboard_filter_mode = FilterMode::Default;
    std::vector<std::string> clipboard_filter_list;

    // global filter mode
    FilterMode global_filter_mode = FilterMode::Default;
    std::vector<std::string> global_filter_list;

    // the text selection is processing
    // Reentrancy guard for GetSelectedText() only. Unlike the Windows retrieval gates,
    // nothing on macOS suppresses input events while this is set; the whole pipeline
    // runs synchronously on the JS thread, so it is normally never observed as true.
    std::atomic<bool> is_processing{false};

    // Mouse gesture tracking state. Instance members (not function-local statics)
    // so a stop()/start() cycle does not inherit the previous session's state.
    CGPoint last_last_mouse_up_pos = CGPointZero;
    CGPoint last_mouse_up_pos = CGPointZero;
    uint64_t last_mouse_up_time = 0;
    CGPoint last_mouse_down_pos = CGPointZero;
    uint64_t last_mouse_down_time = 0;
    bool is_last_valid_click = false;
    bool is_last_mouse_down_valid_cursor = false;

    // Previous modifier flags, used to derive key-down/key-up from flagsChanged
    CGEventFlags previous_flags = 0;
};

// Static member initialization
Napi::FunctionReference SelectionHook::constructor;

// Static pointer for callbacks
static SelectionHook *currentInstance = nullptr;

/**
 * Constructor - registers this instance for the static TSFN/event-tap callbacks.
 * Accessibility permission is not requested here; see MacRequestProcessTrust().
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
    // Clear current instance first to prevent queued TSFN callbacks
    // from dereferencing this object during teardown
    if (currentInstance == this)
    {
        currentInstance = nullptr;
    }

    // Signal shutdown
    bool was_running = running.exchange(false);
    mouse_keyboard_running = false;

    // Stop event monitoring thread (blocks until thread exits)
    StopMouseKeyboardEventThread();

    // Safe to release thread-safe functions only after thread has stopped
    if (was_running && tsfn)
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
        // Three TSFNs share one JS callback so each event class has its own bounded
        // queue: a burst of mouse-move events cannot push out keyboard or selection
        // results. The selection queue is unbounded (0) because at most one result is
        // produced per gesture.
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

        // Reset gesture state so a restarted session does not inherit stale state
        last_last_mouse_up_pos = CGPointZero;
        last_mouse_up_pos = CGPointZero;
        last_mouse_up_time = 0;
        last_mouse_down_pos = CGPointZero;
        last_mouse_down_time = 0;
        is_last_valid_click = false;
        is_last_mouse_down_valid_cursor = false;
        previous_flags = 0;

        // Set flags before starting thread to ensure proper synchronization
        mouse_keyboard_running = true;
        running = true;

        // Mouse move events are off by default when starting; enableMouseMoveEvent()
        // opts in. A restart therefore always begins with them disabled.
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
 * Intentionally a no-op: docs/GUIDE.md states setFineTunedList() has no effect on
 * macOS. Keep the method exported so index.js can call it unconditionally.
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
 * NAPI: Get the currently selected text from the active window
 *
 * Synchronous: runs the same AXAPI -> clipboard pipeline as a gesture, on the calling
 * thread, and returns null when nothing non-blank was retrieved. It works before
 * start() as long as accessibility permission is granted.
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
 *
 * Shared entry for automatic (mouse gesture) and synchronous (getCurrentSelection)
 * retrieval. Only the reentrancy gate and the clipboard sequence write-back live
 * here; the retrieval pipeline itself is SelectionEngine in core/engine.mm.
 */
bool SelectionHook::GetSelectedText(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo)
{
    if (!frontApp)
        return false;

    if (is_processing.load())
        return false;
    else
        is_processing.store(true);

    SelectionConfigSnapshot config = BuildSelectionConfigSnapshot();
    SelectionEngine engine;
    int64_t endingClipboardSequence = config.clipboardSequence;
    bool result = engine.GetSelectedText(frontApp, selectionInfo, config, &endingClipboardSequence);
    // The clipboard fallback refreshes the sequence after every restore. Store it so the
    // next gesture compares against the value produced by the library, not by the user.
    clipboard_sequence = endingClipboardSequence;

    is_processing.store(false);

    return result;
}

/**
 * @brief Copy all mutable selection settings into a value-only retrieval snapshot.
 * @return Configuration the engine can use without reading SelectionHook members.
 */
SelectionConfigSnapshot SelectionHook::BuildSelectionConfigSnapshot() const
{
    SelectionConfigSnapshot config;
    config.clipboardEnabled = is_enabled_clipboard;
    config.clipboardFilterMode = clipboard_filter_mode;
    config.clipboardFilterList = clipboard_filter_list;
    config.globalFilterMode = global_filter_mode;
    config.globalFilterList = global_filter_list;
    config.clipboardSequence = clipboard_sequence;
    config.runningPid = running_pid;
    return config;
}

/**
 * Create JavaScript object with selection result
 *
 * @warning The property names and numeric enum values form the public
 * TextSelectionData contract (index.d.ts, docs/API.md) and must match the Windows and
 * Linux builders. isFullscreen is a macOS-only addition.
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

    // Create a fresh promise/future pair for run loop synchronization
    eventRunLoopPromise = std::promise<CFRunLoopRef>();
    eventRunLoopFuture = eventRunLoopPromise.get_future();

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

    // Wait for the event thread to publish its run loop reference via promise/future.
    // The future only proves the reference was captured, NOT that the thread has
    // entered CFRunLoopRun() yet: the event taps are created in between, and
    // CFRunLoopStop() is a no-op on a run loop that is not currently running.
    // A direct CFRunLoopStop() from here could therefore be lost, leaving the
    // join() below blocked forever (issue #23).
    //
    // Instead, deliver the stop request to the event thread itself with
    // CFRunLoopPerformBlock(). The queued block is sticky: it survives until the
    // run loop actually runs, and it then calls CFRunLoopStop() from inside the
    // active run loop pass, where it is guaranteed to take effect.
    // The block must not capture `this`. A block that never executes (run loop
    // already exited) is released when the run loop itself is released below.
    //
    // The run loop object is kept alive by the CFRetain() taken in
    // MouseKeyboardEventThreadProc(), so `rl` stays valid even if the event
    // thread has already exited on its own (joinable() only means "not joined
    // yet", not "still running").
    CFRunLoopRef rl = nullptr;
    if (eventRunLoopFuture.valid())
    {
        rl = eventRunLoopFuture.get();
    }
    else
    {
        rl = eventRunLoop;
    }

    if (rl)
    {
        CFRunLoopPerformBlock(rl, kCFRunLoopDefaultMode, ^{ CFRunLoopStop(CFRunLoopGetCurrent()); });
        // CFRunLoopPerformBlock() only enqueues; wake the loop so the block runs now
        CFRunLoopWakeUp(rl);
    }

    // Wait for the event thread to finish
    event_thread.join();

    // Balance the CFRetain() taken by the event thread. This may be the last
    // reference, in which case it also frees a stop block that never executed.
    if (rl)
    {
        CFRelease(rl);
    }
}

/**
 * Event thread function - runs the event monitoring loop
 */
void SelectionHook::MouseKeyboardEventThreadProc()
{
    // Store the run loop reference and fulfill the promise for synchronization.
    // Retain the run loop: CoreFoundation releases a thread's run loop when the
    // thread exits, but StopMouseKeyboardEventThread() may still need to post to
    // it after this thread has already finished on its own (for example when no
    // event tap could be created and CFRunLoopRun() returned immediately).
    // The matching CFRelease() is done by StopMouseKeyboardEventThread() after join().
    eventRunLoop = CFRunLoopGetCurrent();
    CFRetain(eventRunLoop);
    eventRunLoopPromise.set_value(eventRunLoop);

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
 *
 * Runs on the JS thread as the mouse TSFN callback. It owns the gesture state machine
 * (drag / double-click / shift-click), triggers retrieval, and emits the mouse-event
 * object. Every timing and cursor decision below uses values captured at event time in
 * MouseEventCallback(), never the current system state.
 */
void SelectionHook::ProcessMouseEvent(Napi::Env env, Napi::Function function, MouseEventContext *pMouseEvent)
{
    if (!pMouseEvent)
    {
        return;
    }

    // Guard against callbacks executing after instance destruction
    if (!currentInstance)
    {
        delete pMouseEvent;
        return;
    }

    // Gesture timing MUST use the event timestamp, not wall clock "now".
    // Events can sit in the TSFN queue for an arbitrarily long time when the
    // main thread stalls; measuring here would collapse their real interval.
    auto currentTime = pMouseEvent->timestampMs;

    CGPoint currentPos = pMouseEvent->pos;
    auto mouseType = pMouseEvent->type;
    MouseButton mouseButton = static_cast<MouseButton>(pMouseEvent->button);
    auto mouseFlag = pMouseEvent->flag;

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

            currentInstance->last_mouse_down_time = currentTime;
            currentInstance->last_mouse_down_pos = currentPos;
            // Cursor shape and clipboard sequence come from the event-time snapshot.
            // Querying them here would observe the state after the queue drains,
            // by which point the pointer may have left the text area.
            currentInstance->is_last_mouse_down_valid_cursor = pMouseEvent->cursorIBeam;
            currentInstance->clipboard_sequence = pMouseEvent->clipboardSequence;
            break;
        }
        case kCGEventLeftMouseUp:
        {
            mouseTypeStr = "mouse-up";

            if (!currentInstance->is_selection_passive_mode)
            {
                // Calculate distance between current position and mouse down position
                double dx = currentPos.x - currentInstance->last_mouse_down_pos.x;
                double dy = currentPos.y - currentInstance->last_mouse_down_pos.y;
                double distance = sqrt(dx * dx + dy * dy);

                bool isCurrentValidClick =
                    (currentTime - currentInstance->last_mouse_down_time) <= DOUBLE_CLICK_TIME_MS;
                // Both terms are event-time snapshots, not current system state
                bool isValidCursor = currentInstance->is_last_mouse_down_valid_cursor || pMouseEvent->cursorIBeam;

                if ((currentTime - currentInstance->last_mouse_down_time) > MAX_DRAG_TIME_MS)
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
                else if (currentInstance->is_last_valid_click && isCurrentValidClick &&
                         distance <= DOUBLE_CLICK_MAX_DISTANCE)
                {
                    double dx2 = currentPos.x - currentInstance->last_mouse_up_pos.x;
                    double dy2 = currentPos.y - currentInstance->last_mouse_up_pos.y;
                    double distance2 = sqrt(dx2 * dx2 + dy2 * dy2);

                    if (distance2 <= DOUBLE_CLICK_MAX_DISTANCE &&
                        (currentInstance->last_mouse_down_time - currentInstance->last_mouse_up_time) <=
                            DOUBLE_CLICK_TIME_MS)
                    {
                        // Only support IBeamCursor for now
                        if (isValidCursor)
                        {
                            shouldDetectSelection = true;
                            detectionType = SelectionDetectType::DoubleClick;
                        }
                    }
                }

                // Check if shift key was pressed at event time, it's a way to select text
                if (!shouldDetectSelection)
                {
                    // Use modifier flags captured at event time (not current system state)
                    CGEventFlags flags = pMouseEvent->evFlags;
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
                }
                currentInstance->is_last_valid_click = isCurrentValidClick;
            }

            currentInstance->last_last_mouse_up_pos = currentInstance->last_mouse_up_pos;
            currentInstance->last_mouse_up_time = currentTime;
            currentInstance->last_mouse_up_pos = currentPos;
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
    // Retrieval runs synchronously on the JS thread here. The clipboard fallback can
    // block for ~105 ms, and an AX call into an unresponsive app blocks until the AX
    // messaging timeout. Windows moved this to a worker (issue #17); macOS has not.
    // If that is ever done here, GetFrontApp() must stay on this thread because it
    // pumps the caller's NSRunLoop (see lib/utils.mm).
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
                    selectionInfo.mousePosStart = currentInstance->last_mouse_down_pos;
                    selectionInfo.mousePosEnd = currentInstance->last_mouse_up_pos;

                    if (selectionInfo.posLevel == SelectionPositionLevel::None)
                        selectionInfo.posLevel = SelectionPositionLevel::MouseDual;
                    break;
                }
                case SelectionDetectType::DoubleClick:
                {
                    selectionInfo.mousePosStart = currentInstance->last_mouse_up_pos;
                    selectionInfo.mousePosEnd = currentInstance->last_mouse_up_pos;

                    if (selectionInfo.posLevel == SelectionPositionLevel::None)
                        selectionInfo.posLevel = SelectionPositionLevel::MouseSingle;
                    break;
                }
                case SelectionDetectType::ShiftClick:
                {
                    selectionInfo.mousePosStart = currentInstance->last_last_mouse_up_pos;
                    selectionInfo.mousePosEnd = currentInstance->last_mouse_up_pos;

                    if (selectionInfo.posLevel == SelectionPositionLevel::None)
                        selectionInfo.posLevel = SelectionPositionLevel::MouseDual;
                    break;
                }
                default:
                    break;
            }

            auto callback = [selectionInfo](Napi::Env env, Napi::Function jsCallback)
            {
                if (!currentInstance)
                    return;
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

    // Guard against callbacks executing after instance destruction
    if (!currentInstance)
    {
        delete pKeyboardEvent;
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
            // The keyCode already contains the correct left/right key info
            CGEventFlags currentFlags = pKeyboardEvent->flags;
            CGEventFlags changedFlags = currentFlags ^ currentInstance->previous_flags;

            // Determine key-down or key-up based on which flag changed
            // For Command/Shift/Option/Control: vkCode is already set from pKeyboardEvent->keyCode
            // which correctly distinguishes between left/right variants
            if (changedFlags & kCGEventFlagMaskCommand)
            {
                eventTypeStr = (currentFlags & kCGEventFlagMaskCommand) ? "key-down" : "key-up";
            }
            else if (changedFlags & kCGEventFlagMaskShift)
            {
                eventTypeStr = (currentFlags & kCGEventFlagMaskShift) ? "key-down" : "key-up";
            }
            else if (changedFlags & kCGEventFlagMaskAlternate)
            {
                eventTypeStr = (currentFlags & kCGEventFlagMaskAlternate) ? "key-down" : "key-up";
            }
            else if (changedFlags & kCGEventFlagMaskControl)
            {
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
            currentInstance->previous_flags = currentFlags;
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
 *
 * Runs on the event thread inside the CGEventTap dispatch. It must return quickly and
 * must not touch N-API: everything JS-visible is deferred through mouse_tsfn.
 *
 * @warning The tap is listen-only, so the returned event is never modified. Keep it
 * that way; an active tap would let a slow callback delay input for the whole session.
 */
CGEventRef SelectionHook::MouseEventCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon)
{
    SelectionHook *hook = static_cast<SelectionHook *>(refcon);
    if (!hook || !hook->mouse_keyboard_running || !hook->mouse_tsfn)
        return event;

    // Re-enable the event tap if it was disabled by macOS due to timeout or user input
    // macOS disables a tap whose callback is too slow (or on secure input) and reports
    // it with these pseudo-events. Without re-enabling, input monitoring stops silently
    // for the rest of the session.
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput)
    {
        CGEventTapEnable(hook->mouseEventTap, true);
        return event;
    }

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

    // Sample cursor shape and clipboard sequence here, at event time. Reading them
    // in ProcessMouseEvent would observe the state after the queue drains, and the
    // cursor shape has been measured to flip within 0.1s of real pointer movement.
    //
    // Sampling is deliberately restricted to the left button down/up events:
    // [NSCursor currentSystemCursor] costs ~0.1 ms per call (it crosses into
    // WindowServer). Calling it for every kCGEventMouseMoved would burn several
    // percent of a core on a high polling rate mouse and could accumulate into a
    // kCGEventTapDisabledByTimeout. Only those two events ever consult it.
    bool cursorIBeam = false;
    int64_t clipboardSequence = -1;
    if (type == kCGEventLeftMouseDown || type == kCGEventLeftMouseUp)
    {
        @autoreleasepool
        {
            cursorIBeam = IsIBeamCursor([NSCursor currentSystemCursor]);
        }
        if (type == kCGEventLeftMouseDown)
        {
            // Cheap by comparison (~0.6 us), but only mouse-down ever reads it
            clipboardSequence = GetClipboardSequence();
        }
    }

    // Create mouse event context with modifier flags, event time, cursor shape and
    // clipboard sequence all captured at event time
    MouseEventContext *mouseEventCtx = new MouseEventContext{.type = type,
                                                             .pos = CGEventGetLocation(event),
                                                             .button = button,
                                                             .flag = flag,
                                                             .evFlags = CGEventGetFlags(event),
                                                             .timestampMs = CGEventGetTimestamp(event) / 1000000ULL,
                                                             .cursorIBeam = cursorIBeam,
                                                             .clipboardSequence = clipboardSequence};

    // Send to main thread for processing
    if (hook->mouse_tsfn.NonBlockingCall(mouseEventCtx, ProcessMouseEvent) != napi_ok)
    {
        delete mouseEventCtx;  // Queue full or closing, callback won't fire, prevent leak
    }

    return event;
}

/**
 * Keyboard event callback
 *
 * Event-thread contract is the same as MouseEventCallback(): fast, no N-API, defer via
 * keyboard_tsfn. Modifier keys arrive as kCGEventFlagsChanged and are turned into
 * key-down/key-up by ProcessKeyboardEvent().
 */
CGEventRef SelectionHook::KeyboardEventCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon)
{
    SelectionHook *hook = static_cast<SelectionHook *>(refcon);
    if (!hook || !hook->mouse_keyboard_running || !hook->keyboard_tsfn)
        return event;

    // Re-enable the event tap if it was disabled by macOS due to timeout or user input
    // (see MouseEventCallback for why this is required)
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput)
    {
        CGEventTapEnable(hook->keyboardEventTap, true);
        return event;
    }

    // Get key code
    CGKeyCode keyCode = (CGKeyCode)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);

    // Get modifier flags
    CGEventFlags flags = CGEventGetFlags(event);

    // Create keyboard event context
    KeyboardEventContext *keyboardEventCtx =
        new KeyboardEventContext{.event = type, .keyCode = keyCode, .flags = flags};

    // Send to main thread for processing
    if (hook->keyboard_tsfn.NonBlockingCall(keyboardEventCtx, ProcessKeyboardEvent) != napi_ok)
    {
        delete keyboardEventCtx;  // Queue full or closing, callback won't fire, prevent leak
    }

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
