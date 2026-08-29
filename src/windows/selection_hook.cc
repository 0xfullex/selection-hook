/**
 * @file
 * @brief Windows N-API entry, input hooks, gesture classification, and retrieval coordination.
 *
 * This file is the platform skeleton rather than the retrieval implementation. It owns
 * the addon instance lifecycle, JavaScript callbacks, low-level input hook thread, and
 * event-time mouse snapshots.
 *
 * Main components:
 * - SelectionHook: N-API surface, lifecycle, and result conversion.
 * - Hook thread: event capture only; it must not call UIA, MSAA, or clipboard retrieval.
 * - Automatic path: value snapshots dispatched to the MTA worker in core/.
 * - Synchronous path: getCurrentSelection() retains its calling-thread UIA client.
 * - Retrieval implementation: UIA/MSAA/clipboard compatibility logic in core/engine.*.
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */
#define WIN32_LEAN_AND_MEAN 1

#include <ShellScalingApi.h>  // For SetProcessDpiAwareness
#include <UIAutomation.h>     // For UI Automation
#include <napi.h>
#include <shellapi.h>  // For SHQueryUserNotificationState
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "core/engine.h"
#include "core/types.h"
#include "core/worker.h"
#include "lib/keyboard.h"
#include "lib/string_pool.h"
#include "lib/utils.h"

#pragma comment(lib, "UIAutomationCore.lib")  // For UI Automation
// #pragma comment(lib, "Shcore.lib") // For SetProcessDpiAwareness
#pragma comment(lib, "Shell32.lib")  // For SHQueryUserNotificationState
#pragma comment(lib, "User32.lib")   // For SetProcessDPIAware

// Mouse&Keyboard hook constants
constexpr int DEFAULT_MOUSE_EVENT_QUEUE_SIZE = 512;
constexpr int DEFAULT_KEYBOARD_EVENT_QUEUE_SIZE = 128;

// Mouse interaction constants
constexpr int MIN_DRAG_DISTANCE = 8;
constexpr DWORD MAX_DRAG_TIME_MS = 8000;
constexpr int DOUBLE_CLICK_MAX_DISTANCE = 3;
static DWORD DOUBLE_CLICK_TIME_MS = 500;

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

/**
 * Structure to store mouse event information
 */
struct MouseEventContext
{
    WPARAM event;             ///< Windows message identifier (e.g. WM_LBUTTONDOWN)
    LONG ptX;                 ///< X coordinate of mouse position
    LONG ptY;                 ///< Y coordinate of mouse position
    DWORD mouseData;          ///< Additional mouse event data
    DWORD timestampMs;        ///< Event time in ms (GetTickCount based, wraps every ~49.7 days)
    DWORD buttonMask;         ///< Mouse button bitmap AT EVENT TIME
                              ///< Bit 0: L, 1: R, 2: M, 3: X1, 4: X2
    DWORD modifierMask;       ///< Modifier keys AT EVENT TIME
                              ///< Bit 0: SHIFT, 1: CONTROL, 2: ALT
    HCURSOR cursorHandle;     ///< Cursor handle at event time, left button down/up only
    HWND windowHandler;       ///< Window under the event point, left button down/up only
    RECT windowRect;          ///< Event-time window rectangle
    DWORD clipboardSequence;  ///< Clipboard sequence at event time, left button down only
};

/**
 * Structure to store keyboard event information
 */
struct KeyboardEventContext
{
    WPARAM event;    ///< Windows message identifier (e.g. WM_KEYDOWN)
    DWORD vkCode;    ///< Virtual key code
    DWORD scanCode;  ///< Hardware scan code
    DWORD flags;     ///< Additional flags for the key event
};

class SelectionHook;

/**
 * @brief Lifetime indirection retained by automatic-result TSFN callbacks.
 *
 * @warning Do not store SelectionHook directly in the TSFN context. Destruction clears
 * owner first so a queued or closing callback cannot dereference a dead addon instance.
 */
struct SelectionResultOwner
{
    std::atomic<SelectionHook *> owner{nullptr};
};

struct SelectionResultTsfnContext
{
    std::shared_ptr<SelectionResultOwner> owner;
};

//=============================================================================
// SelectionHook Class Declaration
//=============================================================================
/**
 * @brief Windows N-API wrapper and lifecycle coordinator.
 *
 * Mouse/keyboard hook callbacks capture event-time state, the wrapper classifies gestures
 * and bridges results to JS, and core/ owns text retrieval. Automatic retrieval is sent
 * to SelectionWorker; getCurrentSelection() remains on the synchronous calling path.
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

    // Selection coordination. Retrieval implementation lives in core/engine.*.
    bool GetSelectedText(HWND hwnd, TextSelectionInfo &selectionInfo);
    SelectionConfigSnapshot BuildSelectionConfigSnapshot(bool triggeredByUser) const;
    bool EnqueueAutomaticSelection(HWND hwnd, SelectionDetectType detectionType);
    bool IsRetrievalInFlight() const;
    bool StartSelectionWorker(Napi::Env env, Napi::Function callback, std::string &error);
    void StopSelectionWorker();
    static void CallSelectionResultJs(napi_env env, napi_value jsCallback, void *context, void *data);
    static void FinalizeSelectionResultTsfn(napi_env env, void *finalizeData, void *finalizeHint);
    bool ShouldProcessGetSelection();  // check if we should get text based on system state
    Napi::Object CreateSelectionResultObject(Napi::Env env, const TextSelectionInfo &selectionInfo);

    // Helper method for processing string arrays
    void ProcessStringArrayToList(const Napi::Array &array, std::vector<std::string> &targetList);

    // Mouse and keyboard event handling methods
    void MouseKeyboardHookThreadProc();
    void StopMouseKeyboardHookThread();
    void ReleaseThreadSafeFunctions();
    static LRESULT CALLBACK MouseHookCallback(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardHookCallback(int nCode, WPARAM wParam, LPARAM lParam);
    static void ProcessMouseEvent(Napi::Env env, Napi::Function function, MouseEventContext *mouseEvent);
    static void ProcessKeyboardEvent(Napi::Env env, Napi::Function function, KeyboardEventContext *keyboardEvent);

    // DPI awareness helper method
    void EnableDpiAwareness();

    bool com_initialized_by_us = false;  // Flag indicating whether COM was initialized by this module

    // Thread communication
    Napi::ThreadSafeFunction mouse_tsfn;
    Napi::ThreadSafeFunction keyboard_tsfn;

    // Automatic events use the MTA worker; the public synchronous API keeps the UIA
    // client created on the calling/main thread for API and capability compatibility.
    std::unique_ptr<SelectionWorker> selection_worker;
    std::shared_ptr<RetrievalGates> retrieval_gates = std::make_shared<RetrievalGates>();
    std::shared_ptr<SelectionResultOwner> selection_result_owner = std::make_shared<SelectionResultOwner>();
    std::atomic<uint64_t> selection_generation{0};
    std::atomic<uint64_t> selection_sequence{0};

    std::atomic<bool> running{false};
    std::atomic<bool> mouse_keyboard_running{false};

    std::thread mouse_keyboard_hook_thread;
    HANDLE mouse_keyboard_ready_event = NULL;
    HANDLE mouse_keyboard_stop_event = NULL;

    // Mouse button bitmap, updated on the hook thread before any filtering so the
    // state survives events that are skipped or dropped. Snapshotted into every
    // MouseEventContext; ProcessMouseEvent must read the snapshot, not this atomic.
    // Bit 0: LBUTTON, Bit 1: RBUTTON, Bit 2: MBUTTON, Bit 3: XBUTTON1, Bit 4: XBUTTON2
    std::atomic<DWORD> mouse_button_pressing_flag{0};

    // Mouse gesture tracking state. Instance members (not function-local statics)
    // so a stop()/start() cycle does not inherit the previous session's state.
    POINT last_last_mouse_up_pos = {0, 0};
    POINT last_mouse_up_pos = {0, 0};
    DWORD last_mouse_up_time = 0;
    POINT last_mouse_down_pos = {0, 0};
    DWORD last_mouse_down_time = 0;
    bool is_last_valid_click = false;
    HWND last_window_handler = nullptr;
    RECT last_window_rect = {0};
    // True only while the public synchronous GetCurrentSelection call is active.
    bool is_triggered_by_user = false;

    std::atomic<bool> is_enabled_mouse_move_event{false};
    // Event-time cursor snapshots used by the automatic clipboard heuristic.
    HCURSOR mouse_down_cursor = NULL;
    HCURSOR mouse_up_cursor = NULL;

    // Calling-thread UIA client retained for synchronous getCurrentSelection().
    IUIAutomation *pUIAutomation = nullptr;
    // Last synchronous UIA focused control type, used by clipboard cursor exceptions.
    CONTROLTYPEID uia_control_type = UIA_WindowControlTypeId;

    // Passive mode disables automatic retrieval; getCurrentSelection() remains available.
    bool is_selection_passive_mode = false;
    bool is_enabled_clipboard = true;  // Enable by default
    // Shared/revisioned because automatic and synchronous retrieval can now overlap.
    std::shared_ptr<ClipboardSequenceState> clipboard_sequence_state = std::make_shared<ClipboardSequenceState>();

    // clipboard filter mode
    FilterMode clipboard_filter_mode = FilterMode::Default;
    std::vector<std::string> clipboard_filter_list;

    // global filter mode
    FilterMode global_filter_mode = FilterMode::Default;
    std::vector<std::string> global_filter_list;

    // Fine-tuned compatibility lists for nonstandard application behavior.
    // Custom-cursor applications can bypass clipboard cursor rejection (for example,
    // Adobe Acrobat).
    std::vector<std::string> ftl_exclude_clipboard_cursor_detect;
    // Applications that update clipboard content several times can request a delayed read.
    std::vector<std::string> ftl_include_clipboard_delay_read;
};

// Static member initialization
Napi::FunctionReference SelectionHook::constructor;

// Static pointer for callbacks
static SelectionHook *currentInstance = nullptr;

/**
 * @brief Initialize the addon instance and calling-thread COM/UIA client.
 * @note This UIA client is reserved for synchronous getCurrentSelection(). The automatic
 * worker creates and owns a separate MTA client when start() is called.
 */
SelectionHook::SelectionHook(const Napi::CallbackInfo &info) : Napi::ObjectWrap<SelectionHook>(info)
{
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);

    currentInstance = this;
    selection_result_owner->owner.store(this, std::memory_order_release);

    // Get the system's double-click time
    DOUBLE_CLICK_TIME_MS = GetDoubleClickTime();

    // Set process DPI awareness based on Windows version
    EnableDpiAwareness();

    // Initialize COM for the synchronous calling-thread UIA client.
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    com_initialized_by_us = false;

#ifdef _DEBUG
    APTTYPE apartmentType = APTTYPE_CURRENT;
    APTTYPEQUALIFIER apartmentQualifier = APTTYPEQUALIFIER_NONE;
    const HRESULT apartmentHr = CoGetApartmentType(&apartmentType, &apartmentQualifier);
    fprintf(stderr,
            "[selection-hook] main COM initialization: thread=%lu init=0x%08lx apartment=0x%08lx type=%d "
            "qualifier=%d\n",
            GetCurrentThreadId(), static_cast<unsigned long>(hr), static_cast<unsigned long>(apartmentHr),
            static_cast<int>(apartmentType), static_cast<int>(apartmentQualifier));
#endif

    if (hr == RPC_E_CHANGED_MODE)
    {
        // COM already initialized with different thread model (common in Electron)
    }
    else if (hr == S_FALSE)
    {
        // COM already initialized, but reference count incremented
        com_initialized_by_us = true;
    }
    else if (SUCCEEDED(hr))
    {
        com_initialized_by_us = true;
    }
    else
    {
        Napi::Error::New(env, "Failed to initialize COM library").ThrowAsJavaScriptException();
        return;
    }

    // Initialize the synchronous UI Automation client on this same COM apartment.
    hr = CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                          (void **)&pUIAutomation);
    if (FAILED(hr))
    {
        Napi::Error::New(env, "Failed to initialize UI Automation").ThrowAsJavaScriptException();

        if (com_initialized_by_us)
        {
            CoUninitialize();
            com_initialized_by_us = false;
        }
        return;
    }
}

/**
 * @brief Stop native producers before releasing callbacks and calling-thread COM state.
 */
SelectionHook::~SelectionHook()
{
    // Clear current instance first to prevent TSFN callbacks from using a dying object
    if (currentInstance == this)
    {
        currentInstance = nullptr;
    }
    selection_result_owner->owner.store(nullptr, std::memory_order_release);

    running = false;
    mouse_keyboard_running = false;
    selection_generation.fetch_add(1, std::memory_order_acq_rel);

    // The hook thread must stop before the thread-safe functions it uses are released.
    StopMouseKeyboardHookThread();
    StopSelectionWorker();
    ReleaseThreadSafeFunctions();

    // Release the synchronous UIA client on its owning thread.
    if (pUIAutomation)
    {
        pUIAutomation->Release();
        pUIAutomation = nullptr;
    }

    // Clean up COM if we initialized it
    if (com_initialized_by_us)
    {
        CoUninitialize();
        com_initialized_by_us = false;
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
                     InstanceMethod("readFromClipboard", &SelectionHook::ReadFromClipboard)});

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();

    exports.Set("TextSelectionHook", func);
    return exports;
}

/**
 * @brief Start hook capture and the automatic MTA retrieval worker.
 * @param[in] info First argument is the existing JavaScript event callback.
 * @warning Every partial-start failure must stop native producers before releasing their
 * TSFNs. Publish running=true only after both worker and hook-thread readiness succeed.
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

    // Create thread-safe functions from the JavaScript callback.
    Napi::Function callback = info[0u].As<Napi::Function>();

    // Set up mouse and keyboard hooks
    if (!mouse_keyboard_running)
    {
        // Create thread-safe function for mouse events
        mouse_tsfn = Napi::ThreadSafeFunction::New(env,
                                                   callback,  // Same callback as text selection
                                                   "MouseEventCallback", DEFAULT_MOUSE_EVENT_QUEUE_SIZE, 1);

        // Create thread-safe function for keyboard events
        keyboard_tsfn = Napi::ThreadSafeFunction::New(env,
                                                      callback,  // Same callback as text selection
                                                      "KeyboardEventCallback", DEFAULT_KEYBOARD_EVENT_QUEUE_SIZE, 1);

        // Set running flag
        mouse_keyboard_running = true;
    }

    retrieval_gates = std::make_shared<RetrievalGates>();
    std::string workerError;
    if (!StartSelectionWorker(env, callback, workerError))
    {
        mouse_keyboard_running = false;
        ReleaseThreadSafeFunctions();
        Napi::Error::New(env, workerError).ThrowAsJavaScriptException();
        return;
    }

    // Reset gesture state so a restarted session does not inherit stale state
    last_last_mouse_up_pos = {0, 0};
    last_mouse_up_pos = {0, 0};
    last_mouse_up_time = 0;
    last_mouse_down_pos = {0, 0};
    last_mouse_down_time = 0;
    is_last_valid_click = false;
    last_window_handler = nullptr;
    last_window_rect = {0};
    mouse_button_pressing_flag.store(0);

    // Create fresh synchronization events for this thread session. The stop event
    // is manual-reset so a request remains visible until the hook thread observes it.
    mouse_keyboard_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    mouse_keyboard_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!mouse_keyboard_ready_event || !mouse_keyboard_stop_event)
    {
        mouse_keyboard_running = false;
        StopMouseKeyboardHookThread();
        StopSelectionWorker();
        ReleaseThreadSafeFunctions();
        Napi::Error::New(env, "Failed to create mouse keyboard hook thread").ThrowAsJavaScriptException();
        return;
    }

    try
    {
        mouse_keyboard_hook_thread = std::thread(&SelectionHook::MouseKeyboardHookThreadProc, this);
    }
    catch (...)
    {
        mouse_keyboard_running = false;
        StopMouseKeyboardHookThread();
        StopSelectionWorker();
        ReleaseThreadSafeFunctions();
        Napi::Error::New(env, "Failed to create mouse keyboard hook thread").ThrowAsJavaScriptException();
        return;
    }

    // Wait until PeekMessage has created the hook thread's message queue.
    if (WaitForSingleObject(mouse_keyboard_ready_event, INFINITE) != WAIT_OBJECT_0)
    {
        mouse_keyboard_running = false;
        StopMouseKeyboardHookThread();
        StopSelectionWorker();
        ReleaseThreadSafeFunctions();
        Napi::Error::New(env, "Failed to initialize mouse keyboard hook thread").ThrowAsJavaScriptException();
        return;
    }

    running = true;
}

/**
 * @brief Stop native producers and invalidate queued automatic results.
 * @warning Preserve teardown order: clear running and advance generation, stop the hook
 * thread, stop the selection worker, then release hook TSFNs. Reordering can permit late
 * callbacks or release a callback while a native producer still uses it.
 */
void SelectionHook::Stop(const Napi::CallbackInfo &info)
{
    // Do nothing if not running
    if (!running)
    {
        return;
    }

    // Stop producing requests before aborting and joining the selection worker.
    running = false;
    selection_generation.fetch_add(1, std::memory_order_acq_rel);
    mouse_keyboard_running = false;
    StopMouseKeyboardHookThread();
    StopSelectionWorker();
    ReleaseThreadSafeFunctions();
}

/**
 * @brief N-API synchronous query for the selection under the active mouse window.
 * @return Existing selection result object, or null when no text is available.
 * @note This method intentionally uses the calling-thread UIA client and returns before
 * JavaScript continues. Automatic events alone use the MTA worker.
 * @warning A Promise, cache-only result, or self-window rejection would change public
 * semantics or capability. Routing this call through a worker with a synchronous wait
 * also requires a separate reentrancy, deadlock, and compatibility review.
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
        // use GetWindowUnderMouse() to get because some key(like alt) may blur the foreground window
        // HWND hwnd = GetForegroundWindow();
        HWND hwnd = GetWindowUnderMouse();
        if (!hwnd)
        {
            return env.Null();
        }

        // Get selected text
        TextSelectionInfo selectionInfo;
        is_triggered_by_user = true;
        if (!GetSelectedText(hwnd, selectionInfo) || IsTrimmedEmpty(selectionInfo.text))
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

    // Get clipboard mode from first argument
    int mode = info[0u].As<Napi::Number>().Int32Value();
    global_filter_mode = static_cast<FilterMode>(mode);

    Napi::Array listArray = info[1u].As<Napi::Array>();

    // Use helper method to process the array
    ProcessStringArrayToList(listArray, global_filter_list);
}

/**
 * NAPI: Set fine-tuned list based on type
 */
void SelectionHook::SetFineTunedList(const Napi::CallbackInfo &info)
{
    Napi::Env env = info.Env();
    // Validate arguments
    if (info.Length() < 2 || !info[0u].IsNumber() || !info[1u].IsArray())
    {
        Napi::TypeError::New(env, "Number and Array expected as arguments").ThrowAsJavaScriptException();
        return;
    }

    // Get fine-tuned list type from first argument
    int listType = info[0u].As<Napi::Number>().Int32Value();
    FineTunedListType ftlType = static_cast<FineTunedListType>(listType);

    Napi::Array listArray = info[1u].As<Napi::Array>();

    // Select the appropriate list based on type
    std::vector<std::string> *targetList = nullptr;
    switch (ftlType)
    {
        case FineTunedListType::ExcludeClipboardCursorDetect:
            targetList = &ftl_exclude_clipboard_cursor_detect;
            break;
        case FineTunedListType::IncludeClipboardDelayRead:
            targetList = &ftl_include_clipboard_delay_read;
            break;
        default:
            Napi::TypeError::New(env, "Invalid FineTunedListType").ThrowAsJavaScriptException();
            return;
    }

    // Use helper method to process the array
    ProcessStringArrayToList(listArray, *targetList);
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
        std::wstring wideText = StringPool::Utf8ToWide(utf8Text);

        // Main-thread clipboard APIs never wait for an automatic retrieval.
        bool result = SelectionEngine::TryWriteClipboard(wideText);
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
        std::wstring clipboardContent;
        bool result = SelectionEngine::TryReadClipboard(clipboardContent);

        if (!result)
        {
            return env.Null();
        }

        // Convert to UTF-8 and return as string
        std::string utf8Text = StringPool::WideToUtf8(clipboardContent);
        return Napi::String::New(env, utf8Text);
    }
    catch (const std::exception &e)
    {
        Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
        return env.Null();
    }
}

/**
 * @brief Normalize a JavaScript string array into a lowercase UTF-8 program list.
 * @param[in] array JavaScript array to process.
 * @param[out] targetList Destination list, cleared before valid string entries are added.
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
            std::transform(programName.begin(), programName.end(), programName.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            // Add to the target list (store as UTF-8)
            targetList.push_back(programName);
        }
    }
}

/**
 * Process mouse event on main thread and detect text selection
 */
void SelectionHook::ProcessMouseEvent(Napi::Env env, Napi::Function function, MouseEventContext *pMouseEvent)
{
    if (!currentInstance || !currentInstance->ShouldProcessGetSelection())
    {
        delete pMouseEvent;
        return;
    }

    auto mEvent = pMouseEvent->event;
    auto nMouseData = pMouseEvent->mouseData;
    POINT currentPos = {pMouseEvent->ptX, pMouseEvent->ptY};

    // Event-time snapshots. These must be copied out before the delete below, and
    // must be used instead of querying the current system state further down: this
    // function runs on the main thread, arbitrarily later than the event itself.
    DWORD eventTime = pMouseEvent->timestampMs;
    DWORD eventButtonMask = pMouseEvent->buttonMask;
    DWORD eventModifierMask = pMouseEvent->modifierMask;
    HCURSOR eventCursor = pMouseEvent->cursorHandle;
    HWND eventWindowHandler = pMouseEvent->windowHandler;
    RECT eventWindowRect = pMouseEvent->windowRect;
    DWORD eventClipboardSequence = pMouseEvent->clipboardSequence;

    delete pMouseEvent;

    bool shouldDetectSelection = false;
    auto detectionType = SelectionDetectType::None;  // 0=not set, 1=drag, 2=double click
    auto mouseType = "";
    auto mouseButton = MouseButton::None;
    auto mouseFlag = 0;

    // Process different mouse events
    switch (mEvent)
    {
        case WM_LBUTTONDOWN:
        {
            mouseType = "mouse-down";
            mouseButton = MouseButton::Left;

            currentInstance->last_mouse_down_time = eventTime;
            currentInstance->last_mouse_down_pos = currentPos;

            currentInstance->last_window_handler = eventWindowHandler;
            currentInstance->last_window_rect = eventWindowRect;

            // Store mouse down cursor when clipboard is enabled
            if (currentInstance->is_enabled_clipboard)
            {
                currentInstance->mouse_down_cursor = eventCursor;
            }

            // Store clipboard sequence number when mouse down
            currentInstance->clipboard_sequence_state->Store(eventClipboardSequence);

            break;
        }
        case WM_LBUTTONUP:
        {
            mouseType = "mouse-up";
            mouseButton = MouseButton::Left;

            // Gesture timing MUST use the event timestamp, not GetTickCount() here.
            // Events can sit in the TSFN queue for an arbitrarily long time when the
            // main thread stalls; measuring here would collapse their real interval.
            DWORD currentTime = eventTime;

            if (!currentInstance->is_selection_passive_mode)
            {
                // Calculate distance between current position and mouse down position
                int dx = currentPos.x - currentInstance->last_mouse_down_pos.x;
                int dy = currentPos.y - currentInstance->last_mouse_down_pos.y;
                double distance = sqrt(dx * dx + dy * dy);

                bool isCurrentValidClick =
                    (currentTime - currentInstance->last_mouse_down_time) <= DOUBLE_CLICK_TIME_MS;

                if ((currentTime - currentInstance->last_mouse_down_time) > MAX_DRAG_TIME_MS)
                {
                    shouldDetectSelection = false;
                }
                // Check for drag selection
                else if (distance >= MIN_DRAG_DISTANCE)
                {
                    if (eventWindowHandler && eventWindowHandler == currentInstance->last_window_handler)
                    {
                        if (!HasWindowMoved(eventWindowRect, currentInstance->last_window_rect))
                        {
                            shouldDetectSelection = true;
                            detectionType = SelectionDetectType::Drag;
                        }
                    }
                }
                // Check for double-click selection
                else if (currentInstance->is_last_valid_click && isCurrentValidClick &&
                         distance <= DOUBLE_CLICK_MAX_DISTANCE)
                {
                    int dx = currentPos.x - currentInstance->last_mouse_up_pos.x;
                    int dy = currentPos.y - currentInstance->last_mouse_up_pos.y;
                    double distance = sqrt(dx * dx + dy * dy);

                    if (distance <= DOUBLE_CLICK_MAX_DISTANCE &&
                        (currentInstance->last_mouse_down_time - currentInstance->last_mouse_up_time) <=
                            DOUBLE_CLICK_TIME_MS)
                    {
                        // check whether it's a maximized/restored behavior of the window
                        if (eventWindowHandler && eventWindowHandler == currentInstance->last_window_handler)
                        {
                            if (!HasWindowMoved(eventWindowRect, currentInstance->last_window_rect))
                            {
                                shouldDetectSelection = true;
                                detectionType = SelectionDetectType::DoubleClick;
                            }
                        }
                    }
                }

                // Check if shift key is pressed when mouse up, it's a way to select text
                if (!shouldDetectSelection)
                {
                    // Use modifier flags captured at event time (not current system state)
                    bool isShiftPressed = (eventModifierMask & 0x01) != 0;
                    bool isCtrlPressed = (eventModifierMask & 0x02) != 0;
                    bool isAltPressed = (eventModifierMask & 0x04) != 0;
                    if (isShiftPressed && !isCtrlPressed && !isAltPressed)
                    {
                        shouldDetectSelection = true;
                        detectionType = SelectionDetectType::ShiftClick;
                    }
                }

                if (shouldDetectSelection && currentInstance->is_enabled_clipboard)
                {
                    currentInstance->mouse_up_cursor = eventCursor;
                }

                currentInstance->is_last_valid_click = isCurrentValidClick;
            }

            currentInstance->last_last_mouse_up_pos = currentInstance->last_mouse_up_pos;

            currentInstance->last_mouse_up_time = currentTime;
            currentInstance->last_mouse_up_pos = currentPos;
            break;
        }

        case WM_MOUSEMOVE:
            mouseType = "mouse-move";

            // Read the per-event snapshot, NOT the instance atomic. Reading the atomic
            // would rewrite the button state of queued events with the state at drain
            // time, turning a held-button drag into None.
            mouseButton = [flag = eventButtonMask]
            {
                if (flag & 0x01)
                    return MouseButton::Left;
                if (flag & 0x02)
                    return MouseButton::Right;
                if (flag & 0x04)
                    return MouseButton::Middle;
                if (flag & 0x08)
                    return MouseButton::Back;
                if (flag & 0x10)
                    return MouseButton::Forward;
                return MouseButton::None;
            }();

            break;

        case WM_RBUTTONDOWN:
            mouseType = "mouse-down";
            mouseButton = MouseButton::Right;
            break;

        case WM_RBUTTONUP:
            mouseType = "mouse-up";
            mouseButton = MouseButton::Right;
            break;

        case WM_MBUTTONUP:
            mouseType = "mouse-up";
            mouseButton = MouseButton::Middle;
            break;

        case WM_MBUTTONDOWN:
            mouseType = "mouse-down";
            mouseButton = MouseButton::Middle;
            break;

        case WM_XBUTTONUP:
        case WM_XBUTTONDOWN:
        {
            mouseType = mEvent == WM_XBUTTONUP ? "mouse-up" : "mouse-down";

            // Determine which X button was pressed
            bool isXButton1 = (HIWORD(nMouseData) == XBUTTON1);
            mouseButton = isXButton1 ? MouseButton::Back : MouseButton::Forward;

            break;
        }

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            mouseType = "mouse-wheel";
            mouseButton = (mEvent == WM_MOUSEWHEEL) ? MouseButton::WheelVertical : MouseButton::WheelHorizontal;
            mouseFlag = GET_WHEEL_DELTA_WPARAM(nMouseData) > 0 ? 1 : -1;
            break;

        default:
            mouseType = "unknown";
            mouseButton = MouseButton::Unknown;
            break;
    }

    // Check for text selection
    if (shouldDetectSelection)
    {
        HWND hwnd = GetForegroundWindow();
        if (hwnd)
            currentInstance->EnqueueAutomaticSelection(hwnd, detectionType);
    }

    // Create and emit mouse event object
    if (mouseType[0] != '\0')
    {
        Napi::Object resultObj = Napi::Object::New(env);
        resultObj.Set(Napi::String::New(env, "type"), Napi::String::New(env, "mouse-event"));
        resultObj.Set(Napi::String::New(env, "action"), Napi::String::New(env, mouseType));
        resultObj.Set(Napi::String::New(env, "x"), Napi::Number::New(env, currentPos.x));
        resultObj.Set(Napi::String::New(env, "y"), Napi::Number::New(env, currentPos.y));
        resultObj.Set(Napi::String::New(env, "button"), Napi::Number::New(env, static_cast<int>(mouseButton)));
        resultObj.Set(Napi::String::New(env, "flag"), Napi::Number::New(env, mouseFlag));
        function.Call({resultObj});
    }
}

/**
 * Process keyboard event on main thread
 */
void SelectionHook::ProcessKeyboardEvent(Napi::Env env, Napi::Function function, KeyboardEventContext *pKeyboardEvent)
{
    if (!currentInstance || !currentInstance->ShouldProcessGetSelection())
    {
        delete pKeyboardEvent;
        return;
    }

    auto kEvent = pKeyboardEvent->event;
    auto vkCode = pKeyboardEvent->vkCode;
    auto scanCode = pKeyboardEvent->scanCode;
    auto flags = pKeyboardEvent->flags;

    delete pKeyboardEvent;

    auto eventType = "";
    // Check if any system key (Ctrl, Alt, Win) is being pressed
    // Cost ~50us
    auto isSysKey = (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                    (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);

    // Determine event type
    switch (kEvent)
    {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            eventType = "key-down";
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            eventType = "key-up";
            break;
    }

    // Create and emit keyboard event object
    if (eventType[0] != '\0')
    {
        // Convert virtual key code to MDN KeyboardEvent.key value
        // cost: ~5us, max 100us (<5%)
        std::string uniKey = convertKeyCodeToUniKey(vkCode, scanCode, flags);

        Napi::Object resultObj = Napi::Object::New(env);
        resultObj.Set(Napi::String::New(env, "type"), Napi::String::New(env, "keyboard-event"));
        resultObj.Set(Napi::String::New(env, "action"), Napi::String::New(env, eventType));
        resultObj.Set(Napi::String::New(env, "uniKey"), Napi::String::New(env, uniKey));
        resultObj.Set(Napi::String::New(env, "vkCode"), Napi::Number::New(env, vkCode));
        resultObj.Set(Napi::String::New(env, "sys"), Napi::Boolean::New(env, isSysKey));
        resultObj.Set(Napi::String::New(env, "scanCode"), Napi::Number::New(env, scanCode));
        resultObj.Set(Napi::String::New(env, "flags"), Napi::Number::New(env, flags));
        function.Call({resultObj});
    }
}

/**
 * @brief Execute the existing synchronous selection pipeline.
 * @param[in] hwnd Borrowed target window handle.
 * @param[out] selectionInfo Native selection result.
 * @return true when non-empty text was retrieved.
 * @note Automatic retrieval may run concurrently with this call, using a separate UIA
 * client. Only the clipboard transaction is mutually exclusive and this path fails fast.
 */
bool SelectionHook::GetSelectedText(HWND hwnd, TextSelectionInfo &selectionInfo)
{
    if (!hwnd)
        return false;

    // This is the public synchronous getCurrentSelection() path. It intentionally does
    // not wait for or reject an independent automatic UIA/MSAA retrieval. The gate only
    // prevents reentrant synchronous calls and preserves input-event suppression.
    bool expected = false;
    if (!retrieval_gates->syncRetrievalInFlight.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;

    struct SyncGateGuard
    {
        std::shared_ptr<RetrievalGates> gates;
        ~SyncGateGuard() { gates->syncRetrievalInFlight.store(false, std::memory_order_release); }
    } gateGuard{retrieval_gates};

    SelectionConfigSnapshot config = BuildSelectionConfigSnapshot(true);
    SelectionEngine engine(pUIAutomation);
    DWORD endingClipboardSequence = config.clipboardSequence;
    CONTROLTYPEID endingControlType = uia_control_type;
    // Keep the established synchronous caller-thread UIA client. Clipboard contention
    // is fail-fast so this API never waits behind the automatic worker's transaction.
    const bool result = engine.GetSelectedText(hwnd, selectionInfo, config, ClipboardLockMode::TryOnly, nullptr,
                                               nullptr, &endingClipboardSequence, &endingControlType);
    // Store with a new revision so an older automatic request cannot overwrite it when
    // that worker later completes.
    clipboard_sequence_state->Store(endingClipboardSequence);
    uia_control_type = endingControlType;
    return result;
}

/**
 * @brief Copy all mutable selection settings into a value-only retrieval snapshot.
 * @param[in] triggeredByUser true for getCurrentSelection(), false for automatic events.
 * @return Configuration safe to use without accessing SelectionHook from another thread.
 */
SelectionConfigSnapshot SelectionHook::BuildSelectionConfigSnapshot(bool triggeredByUser) const
{
    // Copy every retrieval input at dispatch time. The MTA worker must not read mutable
    // vectors, cursors, or filter settings directly from the JS-owned instance.
    SelectionConfigSnapshot config;
    config.clipboardEnabled = is_enabled_clipboard;
    config.triggeredByUser = triggeredByUser;
    config.clipboardFilterMode = clipboard_filter_mode;
    config.clipboardFilterList = clipboard_filter_list;
    config.globalFilterMode = global_filter_mode;
    config.globalFilterList = global_filter_list;
    config.excludeClipboardCursorDetect = ftl_exclude_clipboard_cursor_detect;
    config.includeClipboardDelayRead = ftl_include_clipboard_delay_read;
    config.mouseDownCursor = mouse_down_cursor;
    config.mouseUpCursor = mouse_up_cursor;
    config.clipboardSequence = clipboard_sequence_state->Load().value;
    return config;
}

/**
 * @brief Capture event state and enqueue one automatic selection retrieval.
 * @param[in] hwnd Foreground retrieval HWND chosen by the historical call site.
 * @param[in] detectionType Mouse gesture that triggered retrieval.
 * @return false when stopped, already retrieving, or unable to enqueue.
 * @warning Keep the auto gate set until worker-side retrieval cleanup. Releasing it from
 * the JS callback loses the gate when a TSFN is aborted or its result is discarded.
 */
bool SelectionHook::EnqueueAutomaticSelection(HWND hwnd, SelectionDetectType detectionType)
{
    if (!hwnd || !running.load(std::memory_order_acquire) || !selection_worker)
        return false;

    // Preserve the original one-retrieval-at-a-time behavior. New low-level hook events
    // remain suppressed while this gate is set; no queue of stale selections is built.
    bool expected = false;
    if (!retrieval_gates->autoRetrievalInFlight.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;

    // retrievalHwnd is the same foreground-window value used by the old synchronous
    // call site. Passing a WindowFromPoint child HWND would change UIA/MSAA behavior.
    SelectionRequest request;
    request.sequence = selection_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    request.generation = selection_generation.load(std::memory_order_acquire);
    request.retrievalHwnd = hwnd;
    request.detectionType = detectionType;
    request.mouseDownPos = last_mouse_down_pos;
    request.mouseUpPos = last_mouse_up_pos;
    request.previousMouseUpPos = last_last_mouse_up_pos;
    request.config = BuildSelectionConfigSnapshot(false);
    // Capture value and revision together. The worker may publish its final sequence
    // only if no newer mouse-down or synchronous retrieval updated the shared state.
    const ClipboardSequenceSnapshot clipboardSequence = clipboard_sequence_state->Load();
    request.config.clipboardSequence = clipboardSequence.value;
    request.clipboardSequenceState = clipboard_sequence_state;
    request.clipboardSequenceRevision = clipboardSequence.revision;

    if (!selection_worker->Enqueue(std::move(request)))
    {
        retrieval_gates->autoRetrievalInFlight.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

bool SelectionHook::IsRetrievalInFlight() const
{
    return retrieval_gates && retrieval_gates->IsAnyRetrievalInFlight();
}

/**
 * @brief Create the result TSFN and start the automatic MTA worker for this generation.
 * @param[in] env Current Node-API environment.
 * @param[in] callback Existing event callback used to emit text-selection results.
 * @param[out] error Initialization error on failure.
 * @return true after the worker's COM/UIA readiness handshake succeeds.
 */
bool SelectionHook::StartSelectionWorker(Napi::Env env, Napi::Function callback, std::string &error)
{
    // Each start/stop session gets a new generation. Results queued by an older session
    // are discarded even if their TSFN callback reaches JS after a restart.
    const uint64_t generation = selection_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    napi_value resourceName = nullptr;
    napi_status status = napi_create_string_utf8(env, "TextSelectionCallback", NAPI_AUTO_LENGTH, &resourceName);
    if (status != napi_ok)
    {
        error = "Failed to create selection callback resource name";
        return false;
    }

    auto *context = new SelectionResultTsfnContext{selection_result_owner};
    napi_threadsafe_function resultTsfn = nullptr;
    // The callback queue is intentionally unbounded, but the auto gate permits only one
    // active request. The worker uses nonblocking delivery and owns a separate TSFN ref.
    status = napi_create_threadsafe_function(env, callback, nullptr, resourceName, 0, 1, context,
                                             FinalizeSelectionResultTsfn, context, CallSelectionResultJs, &resultTsfn);
    if (status != napi_ok)
    {
        delete context;
        error = "Failed to create selection result callback";
        return false;
    }

    try
    {
        selection_worker = std::make_unique<SelectionWorker>();
    }
    catch (...)
    {
        napi_release_threadsafe_function(resultTsfn, napi_tsfn_abort);
        error = "Failed to allocate selection MTA worker";
        return false;
    }

    if (!selection_worker->Start(generation, resultTsfn, retrieval_gates, error))
    {
        selection_worker.reset();
        return false;
    }
    return true;
}

/** Stop automatic retrieval before the hook TSFNs and addon instance are released. */
void SelectionHook::StopSelectionWorker()
{
    if (selection_worker)
    {
        selection_worker->Stop();
        selection_worker.reset();
    }
}

/**
 * @brief Convert a value-only worker result and invoke the existing JS event callback.
 * @warning Generation, running state, and owner lifetime checks must precede all access
 * to SelectionHook. Results arriving after stop/restart are intentionally discarded.
 */
void SelectionHook::CallSelectionResultJs(napi_env env, napi_value jsCallback, void *context, void *data)
{
    std::unique_ptr<SelectionWorkerResult> result(static_cast<SelectionWorkerResult *>(data));
    if (!result || !env || !jsCallback || !context)
        return;

    auto *tsfnContext = static_cast<SelectionResultTsfnContext *>(context);
    // Stop/destruction clears owner and bumps generation before a stale result can emit.
    SelectionHook *owner = tsfnContext->owner->owner.load(std::memory_order_acquire);
    if (!owner || !owner->running.load(std::memory_order_acquire) ||
        owner->selection_generation.load(std::memory_order_acquire) != result->generation)
        return;

    Napi::Env napiEnv(env);
    Napi::Object resultObject = owner->CreateSelectionResultObject(napiEnv, result->selection);
    napi_value receiver = nullptr;
    napi_get_global(env, &receiver);
    napi_value argument = resultObject;
    napi_call_function(env, receiver, jsCallback, 1, &argument, nullptr);
}

void SelectionHook::FinalizeSelectionResultTsfn(napi_env env, void *finalizeData, void *finalizeHint)
{
    (void)env;
    (void)finalizeHint;
    delete static_cast<SelectionResultTsfnContext *>(finalizeData);
}

/**
 * @brief Check whether the current Windows notification state allows selection work.
 * @return true when selection retrieval may proceed.
 * @note SHQueryUserNotificationState costs about 0.3 ms on measured systems, so the
 * result is cached for 10 seconds. Query failure intentionally defaults to true.
 */
bool SelectionHook::ShouldProcessGetSelection()
{
    static bool lastResult = true;
    static DWORD lastCheckTime = 0;

    // If less than 10 seconds have passed, return cached result
    if (GetTickCount() - lastCheckTime < 10000)
    {
        return lastResult;
    }

    QUERY_USER_NOTIFICATION_STATE state;
    HRESULT hr = SHQueryUserNotificationState(&state);

    // Update timestamp
    lastCheckTime = GetTickCount();

    if (FAILED(hr))
    {
        lastResult = true;  // If we can't determine state, allow text selection by default
        return lastResult;
    }

    // Don't get text in these states:
    // QUNS_BUSY (2) - System is busy
    // QUNS_RUNNING_D3D_FULL_SCREEN (3) - Running in full-screen mode
    // QUNS_PRESENTATION_MODE (4) - Presentation mode
    lastResult = state != QUNS_RUNNING_D3D_FULL_SCREEN && state != QUNS_PRESENTATION_MODE;
    return lastResult;
}

/**
 * Create JavaScript object with selection result
 */
Napi::Object SelectionHook::CreateSelectionResultObject(Napi::Env env, const TextSelectionInfo &selectionInfo)
{
    Napi::Object resultObj = Napi::Object::New(env);

    std::string utf8Text = StringPool::WideToUtf8(selectionInfo.text);
    std::string utf8Program = StringPool::WideToUtf8(selectionInfo.programName);

    resultObj.Set(Napi::String::New(env, "type"), Napi::String::New(env, "text-selection"));
    resultObj.Set(Napi::String::New(env, "text"), Napi::String::New(env, utf8Text));

    resultObj.Set(Napi::String::New(env, "programName"), Napi::String::New(env, utf8Program));
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

    // Current mouse position
    resultObj.Set(Napi::String::New(env, "mouseStartX"), Napi::Number::New(env, selectionInfo.mousePosStart.x));
    resultObj.Set(Napi::String::New(env, "mouseStartY"), Napi::Number::New(env, selectionInfo.mousePosStart.y));

    resultObj.Set(Napi::String::New(env, "mouseEndX"), Napi::Number::New(env, selectionInfo.mousePosEnd.x));
    resultObj.Set(Napi::String::New(env, "mouseEndY"), Napi::Number::New(env, selectionInfo.mousePosEnd.y));

    return resultObj;
}

/**
 * Stop and join the mouse/keyboard hook thread, then close its session events.
 */
void SelectionHook::StopMouseKeyboardHookThread()
{
    if (mouse_keyboard_stop_event)
    {
        SetEvent(mouse_keyboard_stop_event);
    }

    if (mouse_keyboard_hook_thread.joinable())
    {
        mouse_keyboard_hook_thread.join();
    }

    if (mouse_keyboard_ready_event)
    {
        CloseHandle(mouse_keyboard_ready_event);
        mouse_keyboard_ready_event = NULL;
    }

    if (mouse_keyboard_stop_event)
    {
        CloseHandle(mouse_keyboard_stop_event);
        mouse_keyboard_stop_event = NULL;
    }
}

/**
 * Release all JavaScript callbacks after the hook thread has stopped using them.
 */
void SelectionHook::ReleaseThreadSafeFunctions()
{
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
 * Thread procedure for mouse and keyboard hooks
 */
void SelectionHook::MouseKeyboardHookThreadProc()
{
    MSG msg = {};

    // Force Windows to create this thread's message queue before Start() returns.
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    SetEvent(mouse_keyboard_ready_event);

    HHOOK mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookCallback, NULL, 0);
    HHOOK keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardHookCallback, NULL, 0);

    if (!mouseHook)
    {
        fprintf(stderr, "[selection-hook] SetWindowsHookEx(WH_MOUSE_LL) failed, error: %lu\n", GetLastError());
    }
    if (!keyboardHook)
    {
        fprintf(stderr, "[selection-hook] SetWindowsHookEx(WH_KEYBOARD_LL) failed, error: %lu\n", GetLastError());
    }

    // Wait for either a sticky stop request or input for the hook message queue.
    DWORD waitFlags = MWMO_INPUTAVAILABLE;
    while (true)
    {
        HANDLE stopEvent = mouse_keyboard_stop_event;
        DWORD waitResult = MsgWaitForMultipleObjectsEx(1, &stopEvent, INFINITE, QS_ALLINPUT, waitFlags);

        if (waitResult == WAIT_OBJECT_0)
        {
            break;
        }

        if (waitResult == WAIT_OBJECT_0 + 1)
        {
            BOOL foundMessage = PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);
            if (foundMessage && msg.message == WM_QUIT)
            {
                break;
            }

            // Avoid spinning if the wakeup only dispatched a sent/internal message.
            waitFlags = foundMessage ? MWMO_INPUTAVAILABLE : 0;
            continue;
        }

        if (waitResult == WAIT_FAILED)
        {
            fprintf(stderr, "[selection-hook] MsgWaitForMultipleObjectsEx failed, error: %lu\n", GetLastError());
        }
        break;
    }

    if (mouseHook != NULL)
    {
        UnhookWindowsHookEx(mouseHook);
        mouseHook = NULL;
    }

    if (keyboardHook != NULL)
    {
        UnhookWindowsHookEx(keyboardHook);
        keyboardHook = NULL;
    }

    // TSFN lifecycle is managed by Stop()/destructor on the main thread
}

/**
 * Static callback for mouse hook events
 */
LRESULT CALLBACK SelectionHook::MouseHookCallback(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && currentInstance)
    {
        MSLLHOOKSTRUCT *pMouseInfo = (MSLLHOOKSTRUCT *)lParam;

        // Update the button bitmap BEFORE any filtering, so the state stays correct
        // even when this event is skipped by a retrieval gate / the mouse-move switch,
        // or dropped later by a full TSFN queue.
        switch (wParam)
        {
            case WM_LBUTTONDOWN:
                currentInstance->mouse_button_pressing_flag.fetch_or(0x01u);
                break;
            case WM_LBUTTONUP:
                currentInstance->mouse_button_pressing_flag.fetch_and(~0x01u);
                break;
            case WM_RBUTTONDOWN:
                currentInstance->mouse_button_pressing_flag.fetch_or(0x02u);
                break;
            case WM_RBUTTONUP:
                currentInstance->mouse_button_pressing_flag.fetch_and(~0x02u);
                break;
            case WM_MBUTTONDOWN:
                currentInstance->mouse_button_pressing_flag.fetch_or(0x04u);
                break;
            case WM_MBUTTONUP:
                currentInstance->mouse_button_pressing_flag.fetch_and(~0x04u);
                break;
            case WM_XBUTTONDOWN:
                currentInstance->mouse_button_pressing_flag.fetch_or(HIWORD(pMouseInfo->mouseData) == XBUTTON1 ? 0x08u
                                                                                                               : 0x10u);
                break;
            case WM_XBUTTONUP:
                currentInstance->mouse_button_pressing_flag.fetch_and(
                    ~(HIWORD(pMouseInfo->mouseData) == XBUTTON1 ? 0x08u : 0x10u));
                break;
            default:
                break;
        }

        // If not WM_MOUSEMOVE or WM_MOUSEMOVE has been requested, process event
        if (!currentInstance->IsRetrievalInFlight() &&
            !(wParam == WM_MOUSEMOVE && !currentInstance->is_enabled_mouse_move_event))
        {
            // Capture the remaining gesture inputs at event time. These queries are
            // restricted to left button down/up because no other event consumes them.
            // Windows measurements over 20,000 calls put each query below 1 us.
            HCURSOR cursorHandle = NULL;
            HWND windowHandler = NULL;
            RECT windowRect = {0};
            DWORD clipboardSequence = 0;
            if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP)
            {
                CURSORINFO cursorInfo = {sizeof(CURSORINFO)};
                if (GetCursorInfo(&cursorInfo))
                {
                    cursorHandle = cursorInfo.hCursor;
                }

                windowHandler = WindowFromPoint(pMouseInfo->pt);
                if (!windowHandler)
                {
                    windowHandler = GetForegroundWindow();
                }
                if (windowHandler && !GetWindowRect(windowHandler, &windowRect))
                {
                    windowHandler = NULL;
                    windowRect = {0};
                }

                if (wParam == WM_LBUTTONDOWN)
                {
                    clipboardSequence = GetClipboardSequenceNumber();
                }
            }

            // Prepare data to be processed
            auto pMouseEvent = new MouseEventContext();
            pMouseEvent->event = wParam;
            pMouseEvent->ptX = pMouseInfo->pt.x;
            pMouseEvent->ptY = pMouseInfo->pt.y;
            pMouseEvent->mouseData = pMouseInfo->mouseData;

            // Event time, taken from the hook struct instead of GetTickCount() at drain
            // time. Both are ms since system start, so the base is unchanged.
            pMouseEvent->timestampMs = pMouseInfo->time;

            // Snapshot the bitmap AFTER the update above, so a mouse-down event
            // already carries its own bit
            pMouseEvent->buttonMask = currentInstance->mouse_button_pressing_flag.load();

            // Snapshot modifier keys at event time; reading them in ProcessMouseEvent
            // would see the state after the queue drains, by which point the user may
            // have released Shift
            DWORD modifierMask = 0;
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
                modifierMask |= 0x01;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
                modifierMask |= 0x02;
            if (GetAsyncKeyState(VK_MENU) & 0x8000)
                modifierMask |= 0x04;
            pMouseEvent->modifierMask = modifierMask;
            pMouseEvent->cursorHandle = cursorHandle;
            pMouseEvent->windowHandler = windowHandler;
            pMouseEvent->windowRect = windowRect;
            pMouseEvent->clipboardSequence = clipboardSequence;

            // Process event on non-blocking thread
            if (currentInstance->mouse_tsfn.NonBlockingCall(pMouseEvent, ProcessMouseEvent) != napi_ok)
            {
                delete pMouseEvent;  // Queue full or closing, callback won't fire, prevent leak
            }
        }
    }

    // Allow event to continue
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

/**
 * Static callback for keyboard hook events
 */
LRESULT CALLBACK SelectionHook::KeyboardHookCallback(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && currentInstance && !currentInstance->IsRetrievalInFlight())
    {
        KBDLLHOOKSTRUCT *pKeyInfo = (KBDLLHOOKSTRUCT *)lParam;
        auto pKeyboardEvent = new KeyboardEventContext();
        pKeyboardEvent->event = wParam;
        pKeyboardEvent->vkCode = pKeyInfo->vkCode;
        pKeyboardEvent->scanCode = pKeyInfo->scanCode;
        pKeyboardEvent->flags = pKeyInfo->flags;

        if (currentInstance->keyboard_tsfn.NonBlockingCall(pKeyboardEvent, ProcessKeyboardEvent) != napi_ok)
        {
            delete pKeyboardEvent;  // Queue full or closing, callback won't fire, prevent leak
        }
    }

    // Pass to next hook
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

/**
 * Enable DPI awareness based on Windows version
 * For Windows 8.1 and above: use SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE)
 * For older versions: use SetProcessDPIAware()
 */
void SelectionHook::EnableDpiAwareness()
{
    // Define the SetProcessDpiAwareness function pointer type
    typedef HRESULT(WINAPI * SetProcessDpiAwareness_t)(PROCESS_DPI_AWARENESS);

    // Try to load the new function from Shcore.dll
    HMODULE shcore = LoadLibraryA("Shcore.dll");
    if (shcore)
    {
        SetProcessDpiAwareness_t SetProcessDpiAwareness_func =
            (SetProcessDpiAwareness_t)GetProcAddress(shcore, "SetProcessDpiAwareness");

        if (SetProcessDpiAwareness_func != nullptr)
        {
            // Windows 8.1+, use SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE)
            // we don't check the result, because it's not important
            HRESULT result = SetProcessDpiAwareness_func(PROCESS_PER_MONITOR_DPI_AWARE);
        }
        else
        {
            // fallback to old method
            SetProcessDPIAware();
        }
        FreeLibrary(shcore);
    }
    else
    {
        // Windows 7+
        SetProcessDPIAware();
    }
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
