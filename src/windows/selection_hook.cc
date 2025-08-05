/**
 * Text Selection Hook for Windows
 *
 * A Node Native Module that captures text selection events across applications
 * on Windows using UI Automation and Accessibility APIs.
 *
 * Main components:
 * - TextSelectionHook class: Core implementation of the module
 * - Text selection detection: Uses UIAutomation and IAccessible interfaces
 * - Mouse/keyboard hook: Low-level Windows hooks for input monitoring
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
#define WIN32_LEAN_AND_MEAN 1

#include <ShellScalingApi.h>  // For SetProcessDpiAwareness
#include <UIAutomation.h>     // For UI Automation
#include <napi.h>
#include <oleacc.h>    // For IAccessible
#include <shellapi.h>  // For SHQueryUserNotificationState
#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#include "lib/clipboard.h"
#include "lib/keyboard.h"
#include "lib/string_pool.h"
#include "lib/utils.h"

#pragma comment(lib, "Oleacc.lib")            // For IAccessible
#pragma comment(lib, "UIAutomationCore.lib")  // For UI Automation
// #pragma comment(lib, "Shcore.lib") // Fo/r SetProcessDpiAwareness
#pragma comment(lib, "Shell32.lib")  // For SHQueryUserNotificationState
#pragma comment(lib, "User32.lib")   // For SetProcessDPIAware

// UI Automation constants (if not defined)
#ifndef UIA_IsSelectionActivePropertyId
#define UIA_IsSelectionActivePropertyId 30034
#endif

// Mouse&Keyboard hook constants
constexpr int DEFAULT_MOUSE_EVENT_QUEUE_SIZE = 512;
constexpr int DEFAULT_KEYBOARD_EVENT_QUEUE_SIZE = 128;

// Mouse interaction constants
constexpr int MIN_DRAG_DISTANCE = 8;
constexpr DWORD MAX_DRAG_TIME_MS = 8000;
constexpr int DOUBLE_CLICK_MAX_DISTANCE = 3;
static DWORD DOUBLE_CLICK_TIME_MS = 500;

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
    UIA = 1,
    /** @deprecated This method has been removed */
    FocusControl = 2,
    Accessible = 3,
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
    Full = 3,         // selection first paragraph's start and last paragraph's end coordinates are known
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
    WheelVertical = 0,
    WheelHorizontal = 1
};

enum class FilterMode
{
    Default = 0,      // trigger anyway
    IncludeList = 1,  // only trigger when the program name is in the include list
    ExcludeList = 2   // only trigger when the program name is not in the exclude list
};

enum class FineTunedListType
{
    ExcludeClipboardCursorDetect = 0,
    IncludeClipboardDelayRead = 1
};

// Copy key type enum for SendCopyKey function
enum class CopyKeyType
{
    CtrlInsert = 0,
    CtrlC = 1
};

/**
 * Structure to store text selection information
 */
struct TextSelectionInfo
{
    std::wstring text;         ///< Selected text content
    std::wstring programName;  ///< program name that triggered the selection

    POINT startTop;     ///< First paragraph left-top (screen coordinates)
    POINT startBottom;  ///< First paragraph left-bottom (screen coordinates)
    POINT endTop;       ///< Last paragraph right-top (screen coordinates)
    POINT endBottom;    ///< Last paragraph right-bottom (screen coordinates)

    POINT mousePosStart;  ///< Current mouse position (screen coordinates)
    POINT mousePosEnd;    ///< Mouse down position (screen coordinates)

    SelectionMethod method;
    SelectionPositionLevel posLevel;

    TextSelectionInfo() : method(SelectionMethod::None), posLevel(SelectionPositionLevel::None)
    {
        startTop.x = startTop.y = 0;
        startBottom.x = startBottom.y = 0;
        endTop.x = endTop.y = 0;
        endBottom.x = endBottom.y = 0;
        mousePosStart.x = mousePosStart.y = 0;
        mousePosEnd.x = mousePosEnd.y = 0;
    }

    void clear()
    {
        text.clear();
        startTop.x = startTop.y = 0;
        startBottom.x = startBottom.y = 0;
        endTop.x = endTop.y = 0;
        endBottom.x = endBottom.y = 0;
        mousePosStart.x = mousePosStart.y = 0;
        mousePosEnd.x = mousePosEnd.y = 0;
        method = SelectionMethod::None;
        posLevel = SelectionPositionLevel::None;
    }
};

/**
 * Structure to store mouse event information
 */
struct MouseEventContext
{
    WPARAM event;     ///< Windows message identifier (e.g. WM_LBUTTONDOWN)
    LONG ptX;         ///< X coordinate of mouse position
    LONG ptY;         ///< Y coordinate of mouse position
    DWORD mouseData;  ///< Additional mouse event data
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

    // Core functionality methods
    bool GetSelectedText(HWND hwnd, TextSelectionInfo &selectionInfo);
    bool GetTextViaUIAutomation(HWND hwnd, TextSelectionInfo &selectionInfo);
    bool GetTextViaAccessible(HWND hwnd, TextSelectionInfo &selectionInfo);
    bool GetTextViaClipboard(HWND hwnd, TextSelectionInfo &selectionInfo);
    bool ShouldProcessGetSelection();  // check if we should get text based on system state
    bool ShouldProcessViaClipboard(HWND hwnd, std::wstring &programName);
    bool SetTextRangeCoordinates(IUIAutomationTextRange *pRange, TextSelectionInfo &selectionInfo);
    Napi::Object CreateSelectionResultObject(Napi::Env env, const TextSelectionInfo &selectionInfo);

    // Helper method for processing string arrays
    bool IsInFilterList(const std::wstring &programName, const std::vector<std::string> &filterList);
    void ProcessStringArrayToList(const Napi::Array &array, std::vector<std::string> &targetList);
    void SendCopyKey(CopyKeyType type);  // Keyboard input helper method
    bool ShouldKeyInterruptViaClipboard();

    // Mouse and keyboard event handling methods
    static DWORD WINAPI MouseKeyboardHookThreadProc(LPVOID lpParam);
    static LRESULT CALLBACK MouseHookCallback(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyboardHookCallback(int nCode, WPARAM wParam, LPARAM lParam);
    static void ProcessMouseEvent(Napi::Env env, Napi::Function function, MouseEventContext *mouseEvent);
    static void ProcessKeyboardEvent(Napi::Env env, Napi::Function function, KeyboardEventContext *keyboardEvent);

    // DPI awareness helper method
    void EnableDpiAwareness();

    bool com_initialized_by_us = false;  // Flag indicating whether COM was initialized by this module

    // Thread communication
    Napi::ThreadSafeFunction tsfn;
    Napi::ThreadSafeFunction mouse_tsfn;
    Napi::ThreadSafeFunction keyboard_tsfn;

    std::atomic<bool> running{false};
    std::atomic<bool> mouse_keyboard_running{false};

    HANDLE mouse_keyboard_hook_thread = NULL;
    DWORD mouse_keyboard_thread_id = 0;

    // the text selection is processing, we should ignore some events
    std::atomic<bool> is_processing{false};
    // user use GetCurrentSelection
    bool is_triggered_by_user = false;

    bool is_enabled_mouse_move_event = false;
    // the cursor of mouse down and mouse up, for clipboard detection
    HCURSOR mouse_down_cursor = NULL;
    HCURSOR mouse_up_cursor = NULL;

    // UI Automation objects
    IUIAutomation *pUIAutomation = nullptr;
    // the control type of the UI Automation focused element
    CONTROLTYPEID uia_control_type = UIA_WindowControlTypeId;

    // passive mode: only trigger when user call GetSelectionText
    bool is_selection_passive_mode = false;
    bool is_enabled_clipboard = true;  // Enable by default
    // Store clipboard sequence number when mouse down
    DWORD clipboard_sequence = 0;

    // clipboard filter mode
    FilterMode clipboard_filter_mode = FilterMode::Default;
    std::vector<std::string> clipboard_filter_list;

    // global filter mode
    FilterMode global_filter_mode = FilterMode::Default;
    std::vector<std::string> global_filter_list;

    // fine-tuned lists (ftl) for some apps
    //
    // we use cursor detection to determine if we should go on process clipboard detection,
    // but some apps will use self-defined cursor, you can exclude them to keep using clipboard. (eg. Adobe Acrobat)
    std::vector<std::string> ftl_exclude_clipboard_cursor_detect;
    // some apps will change the clipboard content many times, and it cost time
    // you can include them to wait a little bit (eg. Adobe Acrobat)
    std::vector<std::string> ftl_include_clipboard_delay_read;
};

// Static member initialization
Napi::FunctionReference SelectionHook::constructor;

// Static pointer for callbacks
static SelectionHook *currentInstance = nullptr;

/**
 * Constructor - initializes COM and UI Automation
 */
SelectionHook::SelectionHook(const Napi::CallbackInfo &info) : Napi::ObjectWrap<SelectionHook>(info)
{
    Napi::Env env = info.Env();
    Napi::HandleScope scope(env);

    currentInstance = this;

    // Get the system's double-click time
    DOUBLE_CLICK_TIME_MS = GetDoubleClickTime();

    // Set process DPI awareness based on Windows version
    EnableDpiAwareness();

    // Initialize COM with thread safety
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    com_initialized_by_us = false;

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

    // Initialize UI Automation
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
 * Destructor - cleans up resources and COM objects
 */
SelectionHook::~SelectionHook()
{
    // Stop worker thread
    bool was_running = running.exchange(false);
    if (was_running && tsfn)
    {
        tsfn.Release();
    }

    // Stop mouse/keyboard hooks
    bool mouse_keyboard_was_running = mouse_keyboard_running.exchange(false);
    if (mouse_keyboard_was_running)
    {
        if (mouse_keyboard_thread_id != 0)
        {
            PostThreadMessageW(mouse_keyboard_thread_id, WM_USER, NULL, NULL);
        }

        if (mouse_keyboard_hook_thread)
        {
            WaitForSingleObject(mouse_keyboard_hook_thread, 1000);
            CloseHandle(mouse_keyboard_hook_thread);
            mouse_keyboard_hook_thread = NULL;
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

    // Clear current instance if it's us
    if (currentInstance == this)
    {
        currentInstance = nullptr;
    }

    // Release UI Automation
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

    // Create thread-safe function from JavaScript callback
    Napi::Function callback = info[0u].As<Napi::Function>();

    tsfn = Napi::ThreadSafeFunction::New(env, callback, "TextSelectionCallback", 0, 1,
                                         [this](Napi::Env) { running = false; });

    // Set up mouse and keyboard hooks
    if (!mouse_keyboard_running)
    {
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

        // Set running flag
        mouse_keyboard_running = true;
    }

    // Create Windows thread for mouse/keyboard hooks
    mouse_keyboard_hook_thread =
        CreateThread(NULL, 0, MouseKeyboardHookThreadProc, this, CREATE_SUSPENDED, &mouse_keyboard_thread_id);

    if (!mouse_keyboard_hook_thread)
    {
        mouse_keyboard_running = false;
        mouse_tsfn.Release();
        keyboard_tsfn.Release();
        tsfn.Release();
        Napi::Error::New(env, "Failed to create mouse keyboard hook thread").ThrowAsJavaScriptException();
        return;
    }

    // Start the thread and set running flag
    ResumeThread(mouse_keyboard_hook_thread);
    running = true;
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

    // Set running flag to false and release thread-safe function
    running = false;
    if (tsfn)
    {
        tsfn.Release();
    }

    // Stop mouse and keyboard hooks
    if (mouse_keyboard_running)
    {
        mouse_keyboard_running = false;

        if (mouse_keyboard_thread_id != 0)
        {
            PostThreadMessageW(mouse_keyboard_thread_id, WM_USER, NULL, NULL);
        }

        // Wait for thread to finish with timeout
        if (mouse_keyboard_hook_thread)
        {
            WaitForSingleObject(mouse_keyboard_hook_thread, 1000);
            CloseHandle(mouse_keyboard_hook_thread);
            mouse_keyboard_hook_thread = NULL;
        }

        // Release thread-safe functions
        if (mouse_tsfn)
        {
            mouse_tsfn.Release();
        }
        if (keyboard_tsfn)
        {
            keyboard_tsfn.Release();
        }
    }
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

        // Write to clipboard using the extracted function
        bool result = WriteClipboard(wideText);
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
        bool result = ReadClipboard(clipboardContent);

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
 * Check if program name is in the filter list
 * @param programName The program name to check
 * @param filterList The list of program names to filter against
 * @return true if program name matches any item in the filter list
 */
bool SelectionHook::IsInFilterList(const std::wstring &programName, const std::vector<std::string> &filterList)
{
    // If filter list is empty, allow all
    if (filterList.empty())
    {
        return false;
    }

    // Convert program name to lowercase UTF-8 for case-insensitive comparison
    std::wstring lowerProgramName = programName;
    std::transform(lowerProgramName.begin(), lowerProgramName.end(), lowerProgramName.begin(), towlower);

    // Convert to UTF-8 for comparison with our list
    std::string utf8ProgramName = StringPool::WideToUtf8(lowerProgramName);

    // Check if program name is in the filter list
    for (const auto &filterItem : filterList)
    {
        if (utf8ProgramName.find(filterItem) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

/**
 * Helper method to process string array and populate target list
 * @param array The Napi::Array containing string values
 * @param targetList The target vector to populate with lowercase UTF-8 strings
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
    if (!currentInstance->ShouldProcessGetSelection())
    {
        delete pMouseEvent;
        return;
    }

    auto mEvent = pMouseEvent->event;
    auto nMouseData = pMouseEvent->mouseData;
    POINT currentPos = {pMouseEvent->ptX, pMouseEvent->ptY};

    delete pMouseEvent;

    static POINT lastLastMouseUpPos = {0, 0};  // Last last mouse up position
    static POINT lastMouseUpPos = {0, 0};      // Last mouse up position
    static DWORD lastMouseUpTime = 0;          // Last mouse up time
    static POINT lastMouseDownPos = {0, 0};    // Last mouse down position
    static DWORD lastMouseDownTime = 0;        // Last mouse down time

    static bool isLastValidClick = false;

    static HWND lastWindowHandler = nullptr;
    static RECT lastWindowRect = {0};

    // Mouse button pressing state flags (bit flags)
    // Bit 0: LBUTTON, Bit 1: RBUTTON, Bit 2: MBUTTON, Bit 3: XBUTTON1, Bit 4: XBUTTON2
    static DWORD mouseButtonPressingFlag = 0;

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
            // Set LBUTTON bit (bit 0)
            mouseButtonPressingFlag |= 0x01;

            lastMouseDownTime = GetTickCount();
            lastMouseDownPos = currentPos;

            lastWindowHandler = GetWindowUnderMouse();

            if (lastWindowHandler)
            {
                GetWindowRect(lastWindowHandler, &lastWindowRect);
            }

            // Store mouse down cursor when clipboard is enabled
            if (currentInstance->is_enabled_clipboard)
            {
                CURSORINFO ci = {sizeof(CURSORINFO)};
                GetCursorInfo(&ci);
                currentInstance->mouse_down_cursor = ci.hCursor;
            }

            // Store clipboard sequence number when mouse down
            currentInstance->clipboard_sequence = GetClipboardSequenceNumber();

            break;
        }
        case WM_LBUTTONUP:
        {
            mouseType = "mouse-up";
            mouseButton = MouseButton::Left;
            // Clear LBUTTON bit (bit 0)
            mouseButtonPressingFlag &= ~0x01;

            DWORD currentTime = GetTickCount();

            if (!currentInstance->is_selection_passive_mode)
            {
                // Calculate distance between current position and mouse down position
                int dx = currentPos.x - lastMouseDownPos.x;
                int dy = currentPos.y - lastMouseDownPos.y;
                double distance = sqrt(dx * dx + dy * dy);

                bool isCurrentValidClick = (currentTime - lastMouseDownTime) <= DOUBLE_CLICK_TIME_MS;

                if ((currentTime - lastMouseDownTime) > MAX_DRAG_TIME_MS)
                {
                    shouldDetectSelection = false;
                }
                // Check for drag selection
                else if (distance >= MIN_DRAG_DISTANCE)
                {
                    HWND hwnd = GetWindowUnderMouse();

                    if (hwnd && hwnd == lastWindowHandler)
                    {
                        RECT currentWindowRect;
                        GetWindowRect(hwnd, &currentWindowRect);

                        if (!HasWindowMoved(currentWindowRect, lastWindowRect))
                        {
                            shouldDetectSelection = true;
                            detectionType = SelectionDetectType::Drag;
                        }
                    }
                }
                // Check for double-click selection
                else if (isLastValidClick && isCurrentValidClick && distance <= DOUBLE_CLICK_MAX_DISTANCE)
                {
                    int dx = currentPos.x - lastMouseUpPos.x;
                    int dy = currentPos.y - lastMouseUpPos.y;
                    double distance = sqrt(dx * dx + dy * dy);

                    if (distance <= DOUBLE_CLICK_MAX_DISTANCE &&
                        (lastMouseDownTime - lastMouseUpTime) <= DOUBLE_CLICK_TIME_MS)
                    {
                        shouldDetectSelection = true;
                        detectionType = SelectionDetectType::DoubleClick;
                    }
                }

                // Check if shift key is pressed when mouse up, it's a way to select text
                if (!shouldDetectSelection)
                {
                    bool isShiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    bool isCtrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    bool isAltPressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                    if (isShiftPressed && !isCtrlPressed && !isAltPressed)
                    {
                        shouldDetectSelection = true;
                        detectionType = SelectionDetectType::ShiftClick;
                    }
                }

                if (shouldDetectSelection && currentInstance->is_enabled_clipboard)
                {
                    CURSORINFO ci = {sizeof(CURSORINFO)};
                    GetCursorInfo(&ci);
                    currentInstance->mouse_up_cursor = ci.hCursor;
                }

                isLastValidClick = isCurrentValidClick;
            }

            lastLastMouseUpPos = lastMouseUpPos;

            lastMouseUpTime = currentTime;
            lastMouseUpPos = currentPos;
            break;
        }

        case WM_MOUSEMOVE:
            mouseType = "mouse-move";

            // Check which buttons are currently pressed and set mouseButton accordingly
            mouseButton = [flag = mouseButtonPressingFlag]
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
            // Set RBUTTON bit (bit 1)
            mouseButtonPressingFlag |= 0x02;
            break;

        case WM_RBUTTONUP:
            mouseType = "mouse-up";
            mouseButton = MouseButton::Right;
            // Clear RBUTTON bit (bit 1)
            mouseButtonPressingFlag &= ~0x02;
            break;

        case WM_MBUTTONUP:
            mouseType = "mouse-up";
            mouseButton = MouseButton::Middle;
            // Clear MBUTTON bit (bit 2)
            mouseButtonPressingFlag &= ~0x04;
            break;

        case WM_MBUTTONDOWN:
            mouseType = "mouse-down";
            mouseButton = MouseButton::Middle;
            // Set MBUTTON bit (bit 2)
            mouseButtonPressingFlag |= 0x04;
            break;

        case WM_XBUTTONUP:
        case WM_XBUTTONDOWN:
        {
            mouseType = mEvent == WM_XBUTTONUP ? "mouse-up" : "mouse-down";

            // Determine which X button was pressed
            bool isXButton1 = (HIWORD(nMouseData) == XBUTTON1);
            mouseButton = isXButton1 ? MouseButton::Back : MouseButton::Forward;

            // Update button pressing flag
            DWORD buttonBit = isXButton1 ? 0x08 : 0x10;  // XBUTTON1 = bit 3, XBUTTON2 = bit 4

            if (mEvent == WM_XBUTTONDOWN)
                mouseButtonPressingFlag |= buttonBit;  // Set bit
            else
                mouseButtonPressingFlag &= ~buttonBit;  // Clear bit

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
        TextSelectionInfo selectionInfo;
        HWND hwnd = GetForegroundWindow();

        if (hwnd)
        {
            if (currentInstance->GetSelectedText(hwnd, selectionInfo) && !IsTrimmedEmpty(selectionInfo.text))
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
                }

                auto callback = [selectionInfo](Napi::Env env, Napi::Function jsCallback)
                {
                    Napi::Object resultObj = currentInstance->CreateSelectionResultObject(env, selectionInfo);
                    jsCallback.Call({resultObj});
                };

                currentInstance->tsfn.NonBlockingCall(callback);
            }
        }
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
    if (!currentInstance->ShouldProcessGetSelection())
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
        std::string uniKey = convertVkCodeToUniKey(vkCode, scanCode, flags);

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
 * Get selected text from the active window using multiple methods
 */
bool SelectionHook::GetSelectedText(HWND hwnd, TextSelectionInfo &selectionInfo)
{
    if (!hwnd)
        return false;

    if (is_processing.load())
        return false;
    else
        is_processing.store(true);

    // Initialize structure
    selectionInfo.clear();

    // Get program name and store it in selectionInfo
    if (!GetProgramNameFromHwnd(hwnd, selectionInfo.programName))
    {
        selectionInfo.programName = L"";

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

    // First try UI Automation (supported by modern applications)
    if (pUIAutomation && GetTextViaUIAutomation(hwnd, selectionInfo))
    {
        selectionInfo.method = SelectionMethod::UIA;
        is_processing.store(false);
        return true;
    }

    // Fall back to IAccessible interface (supported by older applications)
    if (GetTextViaAccessible(hwnd, selectionInfo))
    {
        selectionInfo.method = SelectionMethod::Accessible;
        is_processing.store(false);
        return true;
    }

    // Last resort: try to get text using clipboard and Ctrl+C if enabled
    if (ShouldProcessViaClipboard(hwnd, selectionInfo.programName) && GetTextViaClipboard(hwnd, selectionInfo))
    {
        selectionInfo.method = SelectionMethod::Clipboard;
        is_processing.store(false);
        return true;
    }

    is_processing.store(false);
    return false;
}

/**
 * Check if current system state allows text selection
 * time comsuming is 0.3ms, so uses a 10-second cache to reduce system calls
 * @return true if system is in a state that allows text selection
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
    lastResult = state != QUNS_RUNNING_D3D_FULL_SCREEN && state != QUNS_BUSY && state != QUNS_PRESENTATION_MODE;
    return lastResult;
}

/**
 * Check if we should process GetTextViaClipboard
 */
bool SelectionHook::ShouldProcessViaClipboard(HWND hwnd, std::wstring &programName)
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

    // if trigger by user, we cannot determine the cursor shape
    if (!is_triggered_by_user)
    {
        // decide by cursor shapes
        HCURSOR arrowCursor = LoadCursor(NULL, IDC_ARROW);
        HCURSOR beamCursor = LoadCursor(NULL, IDC_IBEAM);
        HCURSOR handCursor = LoadCursor(NULL, IDC_HAND);

        // when mouse down or up, any one of them is beamCursor, we can use clipboard
        // otherwise, we have to check the situation further
        if (currentInstance->mouse_down_cursor != beamCursor && currentInstance->mouse_up_cursor != beamCursor)
        {
            // not beam, not arrow, not hand: invalid text selection cursor
            if (currentInstance->mouse_up_cursor != arrowCursor && currentInstance->mouse_up_cursor != handCursor)
            {
                // only apps in the list can use clipboard (exclude cursor detection)
                if (IsInFilterList(programName, ftl_exclude_clipboard_cursor_detect))
                {
                    return true;
                }
                else
                {
                    return false;
                }
            }
            /**
             * not beam, but arrow or hand:
             *
             * uia_control_type exceptions (when the cursor is arrow or hand):
             *
             * https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-controltype-ids
             *
             * chrome devtools: UIA_GroupControlTypeId (50026)
             * chrome pages: UIA_DocumentControlTypeId (50030), UIA_TextControlTypeId (50020)
             */
            else if (uia_control_type != UIA_GroupControlTypeId && uia_control_type != UIA_DocumentControlTypeId &&
                     uia_control_type != UIA_TextControlTypeId)
            {
                return false;
            }
        }
    }

    return true;
}

/**
 * Get text selection via UI Automation interfaces
 */
bool SelectionHook::GetTextViaUIAutomation(HWND hwnd, TextSelectionInfo &selectionInfo)
{
    if (!pUIAutomation || !hwnd)
        return false;

    bool result = false;

    // init the control type to window
    uia_control_type = UIA_WindowControlTypeId;

    // Get the window element
    IUIAutomationElement *pElement = nullptr;
    HRESULT hr = pUIAutomation->ElementFromHandle(hwnd, &pElement);

    if (FAILED(hr) || !pElement)
        return false;

    // Get the focused element - using local scope to ensure proper cleanup
    {
        IUIAutomationElement *pFocusedElement = nullptr;
        hr = pUIAutomation->GetFocusedElement(&pFocusedElement);

        // Release window element as we don't need it anymore
        pElement->Release();
        pElement = nullptr;

        if (FAILED(hr) || !pFocusedElement)
            return false;

        // Get ControlType for future use
        CONTROLTYPEID controlType;
        hr = pFocusedElement->get_CurrentControlType(&controlType);
        if (SUCCEEDED(hr))
        {
            uia_control_type = controlType;
        }

        // Approach 1: Try with TextPattern - using local scope for cleanup
        {
            IUIAutomationTextPattern *pTextPattern = nullptr;
            hr = pFocusedElement->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
                                                      (void **)&pTextPattern);

            if (SUCCEEDED(hr) && pTextPattern)
            {
                // First approach: Get selection directly
                IUIAutomationTextRangeArray *pRanges = nullptr;
                hr = pTextPattern->GetSelection(&pRanges);

                if (SUCCEEDED(hr) && pRanges)
                {
                    int count = 0;
                    hr = pRanges->get_Length(&count);

                    if (SUCCEEDED(hr) && count > 0)
                    {
                        // Process each selection range
                        for (int i = 0; i < count && !result; i++)  // Continue until we find a valid selection
                        {
                            IUIAutomationTextRange *pRange = nullptr;
                            hr = pRanges->GetElement(i, &pRange);

                            if (SUCCEEDED(hr) && pRange)
                            {
                                BSTR bstr = nullptr;
                                hr = pRange->GetText(-1, &bstr);

                                if (SUCCEEDED(hr) && bstr)
                                {
                                    selectionInfo.text = std::wstring(bstr);

                                    if (!selectionInfo.text.empty())
                                    {
                                        // Get range coordinates
                                        result = SetTextRangeCoordinates(pRange, selectionInfo);
                                    }
                                    SysFreeString(bstr);
                                }
                                pRange->Release();
                            }
                        }
                    }
                    pRanges->Release();
                }

                // Second approach: Try to get document range if first approach failed
                if (!result)
                {
                    IUIAutomationTextRange *pDocRange = nullptr;
                    hr = pTextPattern->get_DocumentRange(&pDocRange);

                    if (SUCCEEDED(hr) && pDocRange)
                    {
                        bool hasSelection = false;

                        // First check if there is an active selection without expanding
                        {
                            // Check if there is a selection (by querying selection attributes)
                            VARIANT varSel;
                            VariantInit(&varSel);
                            BSTR bstr = nullptr;

                            HRESULT attrHr = pDocRange->GetAttributeValue(UIA_IsSelectionActivePropertyId, &varSel);
                            hr = pDocRange->GetText(-1, &bstr);

                            if (SUCCEEDED(hr) && SUCCEEDED(attrHr) && bstr && (varSel.vt == VT_BOOL) &&
                                (varSel.boolVal == VARIANT_TRUE))
                            {
                                // We have an active selection
                                std::wstring selectedText = std::wstring(bstr);
                                if (!selectedText.empty())
                                {
                                    selectionInfo.text = selectedText;
                                    if (SetTextRangeCoordinates(pDocRange, selectionInfo))
                                    {
                                        result = true;
                                        hasSelection = true;
                                    }
                                }
                            }

                            if (bstr)
                                SysFreeString(bstr);
                            VariantClear(&varSel);
                        }

                        // If no selection found, try expanding to document
                        if (!hasSelection)
                        {
                            hr = pDocRange->ExpandToEnclosingUnit(TextUnit_Document);

                            if (SUCCEEDED(hr))
                            {
                                BSTR bstr = nullptr;
                                hr = pDocRange->GetText(-1, &bstr);

                                if (SUCCEEDED(hr) && bstr)
                                {
                                    // Check for active selection
                                    VARIANT varSel;
                                    VariantInit(&varSel);
                                    hr = pDocRange->GetAttributeValue(UIA_IsSelectionActivePropertyId, &varSel);

                                    if (SUCCEEDED(hr) && (varSel.vt == VT_BOOL) && (varSel.boolVal == VARIANT_TRUE))
                                    {
                                        std::wstring docText = std::wstring(bstr);
                                        if (!docText.empty())
                                        {
                                            selectionInfo.text = docText;
                                            if (SetTextRangeCoordinates(pDocRange, selectionInfo))
                                            {
                                                result = true;
                                            }
                                        }
                                    }

                                    VariantClear(&varSel);
                                    SysFreeString(bstr);
                                }
                            }
                        }
                        pDocRange->Release();
                    }
                }
                pTextPattern->Release();
            }
        }

        // Third approach: Try with LegacyIAccessible pattern if other methods failed
        if (!result)
        {
            IUIAutomationLegacyIAccessiblePattern *pLegacyPattern = nullptr;
            hr = pFocusedElement->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                                      __uuidof(IUIAutomationLegacyIAccessiblePattern),
                                                      (void **)&pLegacyPattern);

            if (SUCCEEDED(hr) && pLegacyPattern)
            {
                // Create and initialize variant for CHILDID_SELF parameter
                VARIANT varSelf;
                VariantInit(&varSelf);
                varSelf.vt = VT_I4;
                varSelf.lVal = CHILDID_SELF;

                IAccessible *pAcc = nullptr;
                hr = pLegacyPattern->GetIAccessible(&pAcc);

                if (SUCCEEDED(hr) && pAcc)
                {
                    // Try to get selected text from IAccessible
                    VARIANT varSel;
                    VariantInit(&varSel);
                    hr = pAcc->get_accSelection(&varSel);

                    if (SUCCEEDED(hr) && varSel.vt != VT_EMPTY)
                    {
                        // Process selection based on variant type
                        if (varSel.vt == VT_BSTR && varSel.bstrVal)
                        {
                            selectionInfo.text = std::wstring(varSel.bstrVal);
                            if (!selectionInfo.text.empty())
                            {
                                result = true;
                            }
                        }
                        else if (varSel.vt == VT_DISPATCH && varSel.pdispVal)
                        {
                            // Handle IDispatch (object) selection
                            IAccessible *pSelAcc = nullptr;
                            HRESULT dispHr = varSel.pdispVal->QueryInterface(IID_IAccessible, (void **)&pSelAcc);

                            if (SUCCEEDED(dispHr) && pSelAcc)
                            {
                                VARIANT childSelf;
                                VariantInit(&childSelf);
                                childSelf.vt = VT_I4;
                                childSelf.lVal = CHILDID_SELF;

                                // Try get_accName first, then fall back to get_accValue
                                BSTR bstr = nullptr;
                                if (SUCCEEDED(pSelAcc->get_accName(childSelf, &bstr)) && bstr && SysStringLen(bstr) > 0)
                                {
                                    selectionInfo.text = std::wstring(bstr);
                                    result = !selectionInfo.text.empty();
                                }
                                else
                                {
                                    if (bstr)
                                        SysFreeString(bstr);
                                    if (SUCCEEDED(pSelAcc->get_accValue(childSelf, &bstr)) && bstr)
                                    {
                                        selectionInfo.text = std::wstring(bstr);
                                        result = !selectionInfo.text.empty();
                                    }
                                }

                                if (bstr)
                                    SysFreeString(bstr);
                                VariantClear(&childSelf);
                                pSelAcc->Release();
                            }
                        }
                        // Handle array selection if needed
                        else if ((varSel.vt & VT_ARRAY) && varSel.parray)
                        {
                            // Process array selection (implementation depends on specific requirements)
                            // This is a simplified example - expand as needed
                            SAFEARRAY *pArray = varSel.parray;
                            LONG lLower, lUpper;

                            if (SUCCEEDED(SafeArrayGetLBound(pArray, 1, &lLower)) &&
                                SUCCEEDED(SafeArrayGetUBound(pArray, 1, &lUpper)) && lLower <= lUpper)
                            {
                                // For simplicity, just process the first element
                                VARIANT varItem;
                                VariantInit(&varItem);

                                if (SUCCEEDED(SafeArrayGetElement(pArray, &lLower, &varItem)))
                                {
                                    if (varItem.vt == VT_DISPATCH && varItem.pdispVal)
                                    {
                                        IAccessible *pItemAcc = nullptr;
                                        if (SUCCEEDED(
                                                varItem.pdispVal->QueryInterface(IID_IAccessible, (void **)&pItemAcc)))
                                        {
                                            VARIANT itemChild;
                                            VariantInit(&itemChild);
                                            itemChild.vt = VT_I4;
                                            itemChild.lVal = CHILDID_SELF;

                                            BSTR bstr = nullptr;
                                            if (SUCCEEDED(pItemAcc->get_accValue(itemChild, &bstr)) && bstr)
                                            {
                                                selectionInfo.text = std::wstring(bstr);
                                                result = !selectionInfo.text.empty();
                                                SysFreeString(bstr);
                                            }

                                            VariantClear(&itemChild);
                                            pItemAcc->Release();
                                        }
                                    }
                                    VariantClear(&varItem);
                                }
                            }
                        }
                    }
                    VariantClear(&varSel);
                    pAcc->Release();
                }
                VariantClear(&varSelf);
                pLegacyPattern->Release();
            }
        }

        // Always release the focused element
        pFocusedElement->Release();
    }

    return result;
}

/**
 * Get text selection via IAccessible interface for legacy applications
 */
bool SelectionHook::GetTextViaAccessible(HWND hwnd, TextSelectionInfo &selectionInfo)
{
    if (!hwnd)
        return false;

    // Get IAccessible interface for the window
    IAccessible *pAcc = nullptr;
    HRESULT hr = AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible, (void **)&pAcc);

    if (FAILED(hr) || !pAcc)
    {
        return false;
    }

    pAcc->AddRef();

    // Define child identifier for window
    VARIANT varChild;
    VariantInit(&varChild);
    varChild.vt = VT_I4;
    varChild.lVal = CHILDID_SELF;

    // Approach 1: Try to get selected text using accSelection
    VARIANT varSel;
    VariantInit(&varSel);
    hr = pAcc->get_accSelection(&varSel);

    bool result = false;

    if (SUCCEEDED(hr) && varSel.vt != VT_EMPTY)
    {
        // Handle selection as an object
        if (varSel.vt == VT_DISPATCH && varSel.pdispVal)
        {
            IAccessible *pSelAcc = nullptr;
            hr = varSel.pdispVal->QueryInterface(IID_IAccessible, (void **)&pSelAcc);

            if (SUCCEEDED(hr) && pSelAcc)
            {
                VARIANT varSelChild;
                VariantInit(&varSelChild);
                varSelChild.vt = VT_I4;
                varSelChild.lVal = CHILDID_SELF;

                // Try to get text via accName
                BSTR bstr = nullptr;
                hr = pSelAcc->get_accName(varSelChild, &bstr);

                if (SUCCEEDED(hr) && bstr && SysStringLen(bstr) > 0)
                {
                    selectionInfo.text = std::wstring(bstr);
                    SysFreeString(bstr);
                    bstr = nullptr;
                }
                // Then try with accValue if accName failed
                else
                {
                    if (bstr)
                    {
                        SysFreeString(bstr);
                        bstr = nullptr;
                    }

                    hr = pSelAcc->get_accValue(varSelChild, &bstr);
                    if (SUCCEEDED(hr) && bstr)
                    {
                        selectionInfo.text = std::wstring(bstr);
                        SysFreeString(bstr);
                    }
                }

                if (!selectionInfo.text.empty())
                {
                    result = true;

                    // Try to get position information
                    LONG x = 0, y = 0, width = 0, height = 0;
                    if (SUCCEEDED(pSelAcc->accLocation(&x, &y, &width, &height, varSelChild)))
                    {
                        selectionInfo.startTop.x = x;
                        selectionInfo.startTop.y = y;
                        selectionInfo.startBottom.x = x;
                        selectionInfo.startBottom.y = y + height;

                        selectionInfo.endTop.x = x + width;
                        selectionInfo.endTop.y = y;
                        selectionInfo.endBottom.x = x + width;
                        selectionInfo.endBottom.y = y + height;

                        selectionInfo.posLevel = SelectionPositionLevel::Full;
                    }
                }

                pSelAcc->Release();
            }
        }
        // Handle selection as an array
        else if (varSel.vt == (VT_ARRAY | VT_VARIANT) || varSel.vt == (VT_ARRAY | VT_I4))
        {
            SAFEARRAY *pArray = varSel.parray;
            if (pArray)
            {
                LONG lLower, lUpper;
                if (SUCCEEDED(SafeArrayGetLBound(pArray, 1, &lLower)) &&
                    SUCCEEDED(SafeArrayGetUBound(pArray, 1, &lUpper)))
                {
                    // Process the first selected element
                    if (lLower <= lUpper)
                    {
                        VARIANT varItem;
                        VariantInit(&varItem);
                        if (SUCCEEDED(SafeArrayGetElement(pArray, &lLower, &varItem)))
                        {
                            if (varItem.vt == VT_DISPATCH && varItem.pdispVal)
                            {
                                IAccessible *pItemAcc = nullptr;
                                if (SUCCEEDED(varItem.pdispVal->QueryInterface(IID_IAccessible, (void **)&pItemAcc)))
                                {
                                    VARIANT varItemChild;
                                    VariantInit(&varItemChild);
                                    varItemChild.vt = VT_I4;
                                    varItemChild.lVal = CHILDID_SELF;

                                    BSTR bstr = nullptr;
                                    hr = pItemAcc->get_accValue(varItemChild, &bstr);
                                    if (SUCCEEDED(hr) && bstr)
                                    {
                                        selectionInfo.text = std::wstring(bstr);
                                        SysFreeString(bstr);
                                        result = !selectionInfo.text.empty();
                                    }

                                    pItemAcc->Release();
                                }
                            }
                            VariantClear(&varItem);
                        }
                    }
                }
            }
        }

        VariantClear(&varSel);
    }

    pAcc->Release();
    return result;
}

/**
 * Get text using clipboard and Ctrl+C as a last resort
 * Uses clipboard listener and simulated Ctrl+C to obtain selected text
 */
bool SelectionHook::GetTextViaClipboard(HWND hwnd, TextSelectionInfo &selectionInfo)
{
    if (!hwnd)
        return false;

    constexpr DWORD SLEEP_INTERVAL = 5;  // milliseconds

    // If is_triggered_by_user,
    // it means we initiated the copy action ourselves,
    // so we can skip the key check preprocessing
    if (!is_triggered_by_user)
    {
        // we will do the key check preprocessing
        // because user may press some keys but not intent to copy text
        // so we wait max about 200ms to check this

        bool isCtrlPressed = false;
        bool isCPressed = false;
        bool isXPressed = false;
        bool isVPressed = false;
        bool isCtrlPressing = false;
        bool isCPressing = false;
        bool isXPressing = false;
        bool isVPressing = false;
        int checkCount = 0;
        const int maxChecks = 5;

        while (checkCount < maxChecks)
        {
            // Check if clipboard sequence number has changed since mouse down
            // if it's changed, it means user has copied something, we can read it directly
            if (GetClipboardSequenceNumber() != clipboard_sequence)
            {
                // Try to read from clipboard directly
                if (!ReadClipboard(selectionInfo.text) || selectionInfo.text.empty())
                {
                    return false;
                }
                return true;
            }

            isCtrlPressing = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            isCPressing = (GetAsyncKeyState('C') & 0x8000) != 0;
            isXPressing = (GetAsyncKeyState('X') & 0x8000) != 0;
            isVPressing = (GetAsyncKeyState('V') & 0x8000) != 0;

            // if no key is pressing, we can break to go on
            if (!isCtrlPressing && !isCPressing && !isXPressing && !isVPressing)
            {
                break;
            }
            // if any of the keys are pressing,
            // but it's not triggered by user,
            // we will not process
            else if (!is_triggered_by_user)
            {
                return false;
            }

            if (isCtrlPressing)
                isCtrlPressed = true;
            if (isCPressing)
                isCPressed = true;
            if (isXPressing)
                isXPressed = true;
            if (isVPressing)
                isVPressed = true;

            checkCount++;
            Sleep(40);  // Small delay between checks
        }

        // wait for user copy timeout, still some key(Ctrl, C, X, V) is pressing
        if (checkCount >= maxChecks)
        {
            return false;
        }

        // if it's a user copy behavior, we will do nothing
        if (isCtrlPressed && (isCPressed || isXPressed || isVPressed))
        {
            return false;
        }
    }

    // The MFC ID_EDIT_COPY message will make JetBrains IDEs crash, so we abandon this method

    // Save existing clipboard content
    std::wstring existingContent;
    if (OpenClipboard(nullptr))
    {
        ReadClipboard(existingContent, true);
        EmptyClipboard();
        CloseClipboard();
    }
    else
    {
        // Failed to open clipboard, can't proceed safely
        return false;
    }

    bool isInDelayReadList = !selectionInfo.programName.empty() &&
                             IsInFilterList(selectionInfo.programName, ftl_include_clipboard_delay_read);

    // if the program is in the delay read list, we only process Ctrl+C
    if (!isInDelayReadList)
    {
        if (ShouldKeyInterruptViaClipboard())
        {
            return false;
        }

        /**
         * Ctrl+Insert
         *
         * First, we try to get the text with Ctrl+Insert
         * It's a safer way than Ctrl+C
         * but it may not work for some apps not supporting it,
         * and the shortcut may be used by other purposes
         */

        // renew the clipboard sequence number
        clipboard_sequence = GetClipboardSequenceNumber();

        // Send Ctrl+Insert to copy selected text
        SendCopyKey(CopyKeyType::CtrlInsert);

        // Wait for clipboard update with polling
        bool hasNewContent = false;
        // max wait time about 5m * 20 = 100ms
        for (int i = 0; i < 20; i++)
        {
            if (GetClipboardSequenceNumber() != clipboard_sequence)
            {
                hasNewContent = true;
                break;
            }
            Sleep(SLEEP_INTERVAL);
        }

        // Handle case when clipboard update was detected
        if (hasNewContent)
        {
            Sleep(10);

            bool readSuccess = ReadClipboard(selectionInfo.text);

            if (!existingContent.empty())
                WriteClipboard(existingContent);

            if (!readSuccess || selectionInfo.text.empty())
                return false;

            return true;
        }
    }

    if (ShouldKeyInterruptViaClipboard())
    {
        return false;
    }

    /**
     * Ctrl+C
     */

    // renew the clipboard sequence number
    clipboard_sequence = GetClipboardSequenceNumber();

    // Send Ctrl+C to copy selected text
    SendCopyKey(CopyKeyType::CtrlC);

    // Wait for clipboard update with polling
    bool hasNewContent = false;
    // max wait time about 5m * 36 = 180ms
    for (int i = 0; i < 36; i++)
    {
        if (GetClipboardSequenceNumber() != clipboard_sequence)
        {
            hasNewContent = true;
            break;
        }
        Sleep(SLEEP_INTERVAL);
    }

    // Handle case when no clipboard update was detected
    if (!hasNewContent)
    {
        if (!existingContent.empty())
            WriteClipboard(existingContent);
        return false;
    }

    // some apps will change the clipboard content many times after the first time GetClipboardSequenceNumber() changed
    // so we need to wait a little bit (eg. Adobe Acrobat) for those app in the delay read list
    if (isInDelayReadList)
    {
        Sleep(135);
    }
    Sleep(10);

    if (ShouldKeyInterruptViaClipboard())
    {
        return false;
    }

    // Read the new clipboard content
    bool readSuccess = ReadClipboard(selectionInfo.text);

    // Restore previous clipboard content
    if (!existingContent.empty())
        WriteClipboard(existingContent);

    if (!readSuccess || selectionInfo.text.empty())
        return false;

    return true;
}

/**
 * Send copy key combination based on type
 * @param type The type of copy key combination to send (CtrlInsert or CtrlC)
 */
void SelectionHook::SendCopyKey(CopyKeyType type)
{
    bool isCPressing = (GetAsyncKeyState('C') & 0x8000) != 0;
    bool isCtrlPressing = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool isAltPressing = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    bool isShiftPressing = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    // if Ctrl+C is pressing, means user is doing copy action, we will not send anything
    if (isCtrlPressing && isCPressing)
        return;

    WORD keyCode = (type == CopyKeyType::CtrlInsert) ? VK_INSERT : 'C';

    std::vector<INPUT> inputs;
    INPUT input = {};

    // if Alt is pressing, we need to release it first
    if (isAltPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_MENU;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }

    if (isShiftPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_SHIFT;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }

    // The following Ctrl+Insert or Ctrl+C key combinations are symmetric, meaning press and release events come in
    // pairs

    // Press the Ctrl key
    if (!isCtrlPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_RCONTROL;
        input.ki.dwFlags = 0;
        inputs.push_back(input);
    }

    // Press the key (Insert or C)
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = keyCode;
    input.ki.dwFlags = 0;
    inputs.push_back(input);

    // Release the key (Insert or C)
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = keyCode;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(input);

    // Release the Ctrl key
    if (!isCtrlPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_RCONTROL;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }

    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

/**
 * Check if some key is interrupted the copy process via clipboard
 * @return true if the key is interrupted via clipboard, false otherwise
 */
bool SelectionHook::ShouldKeyInterruptViaClipboard()
{
    // if it's not triggered by user, and ctrl is pressing, it means user is doing other action, we will not process
    // we will do the check every time when we have to copy text
    bool isCtrlPressing = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!is_triggered_by_user && isCtrlPressing)
    {
        return true;
    }
    return false;
}

/**
 * Get screen coordinates of a text range
 */
bool SelectionHook::SetTextRangeCoordinates(IUIAutomationTextRange *pRange, TextSelectionInfo &selectionInfo)
{
    if (!pRange)
        return false;

    // Get range's bounding rectangles
    SAFEARRAY *pRectArray = nullptr;
    HRESULT hr = pRange->GetBoundingRectangles(&pRectArray);

    if (SUCCEEDED(hr) && pRectArray)
    {
        // Access rect array data directly for better performance
        double *pRects = nullptr;
        SafeArrayAccessData(pRectArray, (void **)&pRects);

        LONG lowerBound, upperBound;
        SafeArrayGetLBound(pRectArray, 1, &lowerBound);
        SafeArrayGetUBound(pRectArray, 1, &upperBound);

        int rectCount = (upperBound - lowerBound + 1) / 4;

        // Find first rectangle with width > 1 pixel and height < 100 pixel
        int firstValidRectIndex = -1;
        for (int i = 0; i < rectCount; i++)
        {
            int rectIndex = i * 4;
            double width = pRects[rectIndex + 2];
            double height = pRects[rectIndex + 3];

            if (width > 1.0 && height < 100.0)
            {
                firstValidRectIndex = rectIndex;
                break;
            }
        }

        // Find last rectangle with width > 1 pixel and height < 100 pixel
        int lastValidRectIndex = -1;
        for (int i = rectCount - 1; i >= 0; i--)
        {
            int rectIndex = i * 4;
            double width = pRects[rectIndex + 2];
            double height = pRects[rectIndex + 3];

            if (width > 1.0 && height < 100.0)
            {
                lastValidRectIndex = rectIndex;
                break;
            }
        }

        // If we found valid rectangles
        if (firstValidRectIndex >= 0 && lastValidRectIndex >= 0)
        {
            // Use first valid rectangle's top-left as start position (first paragraph left-top)
            selectionInfo.startTop.x = static_cast<LONG>(pRects[firstValidRectIndex]);
            selectionInfo.startTop.y = static_cast<LONG>(pRects[firstValidRectIndex + 1]);

            // Set first paragraph's left-bottom point
            selectionInfo.startBottom.x = static_cast<LONG>(pRects[firstValidRectIndex]);
            selectionInfo.startBottom.y = static_cast<LONG>(pRects[firstValidRectIndex + 1] +
                                                            pRects[firstValidRectIndex + 3]);  // top + height = bottom

            // Use last valid rectangle's bottom-right as end position (last paragraph right-bottom)
            selectionInfo.endBottom.x =
                static_cast<LONG>(pRects[lastValidRectIndex] + pRects[lastValidRectIndex + 2]);  // left + width = right
            selectionInfo.endBottom.y = static_cast<LONG>(pRects[lastValidRectIndex + 1] +
                                                          pRects[lastValidRectIndex + 3]);  // top + height = bottom

            // Set last paragraph's right-top point
            selectionInfo.endTop.x =
                static_cast<LONG>(pRects[lastValidRectIndex] + pRects[lastValidRectIndex + 2]);  // right
            selectionInfo.endTop.y = static_cast<LONG>(pRects[lastValidRectIndex + 1]);          // top

            selectionInfo.posLevel = SelectionPositionLevel::Full;

            SafeArrayUnaccessData(pRectArray);
            SafeArrayDestroy(pRectArray);
            return true;
        }
    }

    return false;
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
 * Thread procedure for mouse and keyboard hooks
 */
DWORD WINAPI SelectionHook::MouseKeyboardHookThreadProc(LPVOID lpParam)
{
    SelectionHook *instance = static_cast<SelectionHook *>(lpParam);

    MSG msg;
    HHOOK mouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookCallback, NULL, 0);
    HHOOK keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardHookCallback, NULL, 0);

    // Message loop
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        if (msg.message != WM_USER)
            continue;

        // Unhook on exit message
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

        break;
    }

    instance->mouse_tsfn.Release();
    instance->keyboard_tsfn.Release();
    return GetLastError();
}

/**
 * Static callback for mouse hook events
 */
LRESULT CALLBACK SelectionHook::MouseHookCallback(int nCode, WPARAM wParam, LPARAM lParam)
{
    // If not WM_MOUSEMOVE or WM_MOUSEMOVE has been requested, process event
    if (nCode == HC_ACTION && currentInstance && !currentInstance->is_processing.load() &&
        !(wParam == WM_MOUSEMOVE && !currentInstance->is_enabled_mouse_move_event))
    {
        // Prepare data to be processed
        MSLLHOOKSTRUCT *pMouseInfo = (MSLLHOOKSTRUCT *)lParam;
        auto pMouseEvent = new MouseEventContext();
        pMouseEvent->event = wParam;
        pMouseEvent->ptX = pMouseInfo->pt.x;
        pMouseEvent->ptY = pMouseInfo->pt.y;
        pMouseEvent->mouseData = pMouseInfo->mouseData;

        // Process event on non-blocking thread
        currentInstance->mouse_tsfn.NonBlockingCall(pMouseEvent, ProcessMouseEvent);
    }

    // Allow event to continue
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

/**
 * Static callback for keyboard hook events
 */
LRESULT CALLBACK SelectionHook::KeyboardHookCallback(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && currentInstance && !currentInstance->is_processing.load())
    {
        KBDLLHOOKSTRUCT *pKeyInfo = (KBDLLHOOKSTRUCT *)lParam;
        auto pKeyboardEvent = new KeyboardEventContext();
        pKeyboardEvent->event = wParam;
        pKeyboardEvent->vkCode = pKeyInfo->vkCode;
        pKeyboardEvent->scanCode = pKeyInfo->scanCode;
        pKeyboardEvent->flags = pKeyInfo->flags;

        currentInstance->keyboard_tsfn.NonBlockingCall(pKeyboardEvent, ProcessKeyboardEvent);
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
    // 定义 SetProcessDpiAwareness 函数指针类型
    typedef HRESULT(WINAPI * SetProcessDpiAwareness_t)(PROCESS_DPI_AWARENESS);

    // 尝试从 Shcore.dll 加载新函数
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