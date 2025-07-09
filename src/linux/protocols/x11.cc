/**
 * X11 Protocol Implementation for Linux Selection Hook
 *
 * This file contains X11-specific implementations for text selection,
 * clipboard operations, and window management.
 */

#include <chrono>
#include <climits>
#include <cstring>
#include <string>
#include <thread>

// X11 headers
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/record.h>
#include <X11/keysym.h>

// epoll headers
#include <sys/epoll.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <thread>

// Linux input event constants
#include <linux/input.h>

// Undefine X11 None macro that conflicts with our enum
#ifdef None
#undef None
#endif

// Include common definitions
#include "../common.h"

// Forward declaration for SelectionHook from selection_hook.cc
class SelectionHook;

/**
 * X11 Protocol Class Implementation
 */
class X11Protocol : public ProtocolBase
{
  private:
    Display* display;
    int screen;
    Window root;

    // XRecord related
    XRecordContext record_context;
    XRecordRange* record_range;
    Display* record_display;   // Dedicated Display connection for XRecord
    Display* control_display;  // Separate Display connection for control operations
    bool record_initialized;

    // Thread management
    std::atomic<bool> input_monitoring_running;
    std::thread input_monitoring_thread;

    // Callback functions
    MouseEventCallback mouse_callback;
    KeyboardEventCallback keyboard_callback;
    void* callback_context;

    // Helper methods
    bool InitializeXRecord();
    void CleanupXRecord();
    bool SetupXRecordMonitoring();
    void XRecordMonitoringThreadProc();
    static void XRecordDataCallback(XPointer closure, XRecordInterceptData* data);
    void ProcessXRecordData(XRecordInterceptData* data);

  public:
    X11Protocol()
        : display(nullptr),
          screen(0),
          root(0),
          record_context(0),
          record_range(nullptr),
          record_display(nullptr),
          control_display(nullptr),
          record_initialized(false),
          input_monitoring_running(false),
          mouse_callback(nullptr),
          keyboard_callback(nullptr),
          callback_context(nullptr)
    {
    }

    ~X11Protocol() override { Cleanup(); }

    // Protocol identification
    DisplayProtocol GetProtocol() const override { return DisplayProtocol::X11; }

    // Initialization and cleanup
    bool Initialize() override
    {
        // Initialize X11 connection
        display = XOpenDisplay(nullptr);
        if (!display)
        {
            return false;
        }

        screen = DefaultScreen(display);
        root = DefaultRootWindow(display);

        return true;
    }

    void Cleanup() override
    {
        if (display)
        {
            XCloseDisplay(display);
            display = nullptr;
        }
    }

    // Window management
    uint64_t GetActiveWindow() override
    {
        if (!display)
            return 0;

        Window active_window = 0;
        Atom net_active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
        Atom type;
        int format;
        unsigned long nitems, bytes_after;
        unsigned char* data = nullptr;

        if (XGetWindowProperty(display, root, net_active_window, 0, 1, False, XA_WINDOW, &type, &format, &nitems,
                               &bytes_after, &data) == Success)
        {
            if (data)
            {
                active_window = *(Window*)data;
                XFree(data);
            }
        }

        return static_cast<uint64_t>(active_window);
    }

    bool GetProgramNameFromWindow(uint64_t window, std::string& programName) override
    {
        if (!display || !window)
            return false;

        Window x11Window = static_cast<Window>(window);

        // Try to get WM_CLASS property first
        XClassHint classHint;
        if (XGetClassHint(display, x11Window, &classHint))
        {
            if (classHint.res_name)
            {
                programName = std::string(classHint.res_name);
                XFree(classHint.res_name);
                if (classHint.res_class)
                    XFree(classHint.res_class);
                return true;
            }
            if (classHint.res_class)
                XFree(classHint.res_class);
        }

        // Fallback to window name
        char* window_name = nullptr;
        if (XFetchName(display, x11Window, &window_name) && window_name)
        {
            programName = std::string(window_name);
            XFree(window_name);
            return true;
        }

        return false;
    }

    // Text selection
    bool GetTextViaPrimary(std::string& text) override
    {
        if (!display)
            return false;

        // Create a window to receive the selection
        Window window = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);

        // Request the primary selection
        Atom selection = XInternAtom(display, "PRIMARY", False);
        Atom utf8_string = XInternAtom(display, "UTF8_STRING", False);
        Atom targets = XInternAtom(display, "TARGETS", False);
        Atom property = XInternAtom(display, "SELECTION_DATA", False);

        XConvertSelection(display, selection, utf8_string, property, window, CurrentTime);
        XFlush(display);

        // Wait for the selection notification
        XEvent event;
        bool success = false;

        // Wait up to 1 second for the selection
        auto start_time = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start_time < std::chrono::milliseconds(1000))
        {
            if (XCheckTypedWindowEvent(display, window, SelectionNotify, &event))
            {
                if (event.xselection.property != 0)  // X11 None constant
                {
                    // Get the selection data
                    Atom actual_type;
                    int actual_format;
                    unsigned long nitems, bytes_after;
                    unsigned char* data = nullptr;

                    if (XGetWindowProperty(display, window, property, 0, LONG_MAX, False, AnyPropertyType, &actual_type,
                                           &actual_format, &nitems, &bytes_after, &data) == Success)
                    {
                        if (data && nitems > 0)
                        {
                            text = std::string(reinterpret_cast<char*>(data), nitems);
                            success = true;
                        }
                        if (data)
                            XFree(data);
                    }
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        XDestroyWindow(display, window);
        return success;
    }

    // bool SetTextRangeCoordinates(uint64_t window, TextSelectionInfo& selectionInfo) override
    // {
    //     if (!display || !window)
    //         return false;

    //     // TODO: Implement X11-specific coordinate retrieval
    //     // This would involve getting the selection bounds from the X11 server
    //     // For now, return false to indicate no coordinates available
    //     return false;
    // }

    // Clipboard operations
    bool WriteClipboard(const std::string& text) override
    {
        if (!display)
            return false;

        // Create a window to own the selection
        Window window = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);

        Atom clipboard = XInternAtom(display, "CLIPBOARD", False);

        // Set ourselves as the owner of the clipboard
        XSetSelectionOwner(display, clipboard, window, CurrentTime);

        // Check if we successfully became the owner
        bool success = (XGetSelectionOwner(display, clipboard) == window);

        if (success)
        {
            // Store the text for later retrieval
            Atom property = XInternAtom(display, "CLIPBOARD_DATA", False);
            XChangeProperty(display, window, property, XA_STRING, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(text.c_str()), text.length());
        }

        XFlush(display);

        // Note: In a real implementation, we would need to handle SelectionRequest events
        // to provide the clipboard data when other applications request it
        // For now, we'll just destroy the window
        XDestroyWindow(display, window);

        return success;
    }

    bool ReadClipboard(std::string& text) override
    {
        if (!display)
            return false;

        // Create a window to receive the selection
        Window window = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);

        // Request the clipboard selection
        Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
        Atom utf8_string = XInternAtom(display, "UTF8_STRING", False);
        Atom property = XInternAtom(display, "CLIPBOARD_DATA", False);

        XConvertSelection(display, clipboard, utf8_string, property, window, CurrentTime);
        XFlush(display);

        // Wait for the selection notification
        XEvent event;
        bool success = false;

        // Wait up to 1 second for the selection
        auto start_time = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start_time < std::chrono::milliseconds(1000))
        {
            if (XCheckTypedWindowEvent(display, window, SelectionNotify, &event))
            {
                if (event.xselection.property != 0)  // X11 None constant
                {
                    // Get the selection data
                    Atom actual_type;
                    int actual_format;
                    unsigned long nitems, bytes_after;
                    unsigned char* data = nullptr;

                    if (XGetWindowProperty(display, window, property, 0, LONG_MAX, False, AnyPropertyType, &actual_type,
                                           &actual_format, &nitems, &bytes_after, &data) == Success)
                    {
                        if (data && nitems > 0)
                        {
                            text = std::string(reinterpret_cast<char*>(data), nitems);
                            success = true;
                        }
                        if (data)
                            XFree(data);
                    }
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        XDestroyWindow(display, window);
        return success;
    }

    // // Key operations
    // void SendCopyKey(CopyKeyType type) override
    // {
    //     if (!display)
    //         return;

    //     KeySym keysym = (type == CopyKeyType::CtrlInsert) ? XK_Insert : XK_c;
    //     KeyCode keycode = XKeysymToKeycode(display, keysym);
    //     KeyCode ctrl_keycode = XKeysymToKeycode(display, XK_Control_L);

    //     if (keycode != 0 && ctrl_keycode != 0)
    //     {
    //         // Press Ctrl
    //         XTestFakeKeyEvent(display, ctrl_keycode, True, 0);
    //         // Press key
    //         XTestFakeKeyEvent(display, keycode, True, 0);
    //         // Release key
    //         XTestFakeKeyEvent(display, keycode, False, 0);
    //         // Release Ctrl
    //         XTestFakeKeyEvent(display, ctrl_keycode, False, 0);
    //         XFlush(display);
    //     }
    // }

    // bool ShouldKeyInterruptViaClipboard() override
    // {
    //     if (!display)
    //         return false;

    //     // TODO: Implement X11-specific key state checking
    //     // This would involve checking modifier keys and other key states
    //     // For now, return false
    //     return false;
    // }

    // Input monitoring implementation using XRecord
    bool InitializeInputMonitoring(MouseEventCallback mouseCallback, KeyboardEventCallback keyboardCallback,
                                   void* context) override
    {
        if (!display)
            return false;

        // Store callback functions
        mouse_callback = mouseCallback;
        keyboard_callback = keyboardCallback;
        callback_context = context;

        // Initialize XRecord
        if (!InitializeXRecord())
            return false;

        // Setup XRecord monitoring
        if (!SetupXRecordMonitoring())
        {
            CleanupXRecord();
            return false;
        }

        return true;
    }

    void CleanupInputMonitoring() override
    {
        // Stop monitoring first
        StopInputMonitoring();

        // Cleanup XRecord
        CleanupXRecord();

        // Reset callback functions
        mouse_callback = nullptr;
        keyboard_callback = nullptr;
        callback_context = nullptr;
    }

    bool StartInputMonitoring() override
    {
        if (!display || !record_initialized || input_monitoring_running)
            return false;

        // Start monitoring thread
        input_monitoring_running = true;
        input_monitoring_thread = std::thread(&X11Protocol::XRecordMonitoringThreadProc, this);

        return true;
    }

    void StopInputMonitoring() override
    {
        // Signal the thread to stop
        input_monitoring_running = false;

        // Disable the XRecord context using the control display to unblock the monitoring thread
        if (control_display && record_context != 0)
        {
            XRecordDisableContext(control_display, record_context);
            XFlush(control_display);
        }

        // Wait for the thread to finish with a timeout
        if (input_monitoring_thread.joinable())
        {
            input_monitoring_thread.join();
        }
    }

    // X11-specific methods
    Display* GetDisplay() const { return display; }
    Window GetRootWindow() const { return root; }
    int GetScreen() const { return screen; }

    // Get current mouse position
    Point GetCurrentMousePosition()
    {
        if (!display)
            return Point(0, 0);

        Window root_return, child_return;
        int root_x, root_y, win_x, win_y;
        unsigned int mask_return;

        if (XQueryPointer(display, root, &root_return, &child_return, &root_x, &root_y, &win_x, &win_y, &mask_return))
        {
            return Point(root_x, root_y);
        }

        return Point(0, 0);
    }

    // X11-specific key sending functionality
    bool SendXTestKey(KeySym keysym, bool press = true)
    {
        if (!display)
            return false;

        KeyCode keycode = XKeysymToKeycode(display, keysym);
        if (keycode == 0)
            return false;

        XTestFakeKeyEvent(display, keycode, press ? True : False, 0);
        XFlush(display);
        return true;
    }
};

// XRecord helper methods implementation
bool X11Protocol::InitializeXRecord()
{
    if (!display)
        return false;

    // Check if XRecord extension is available
    int major_version, minor_version;
    if (!XRecordQueryVersion(display, &major_version, &minor_version))
    {
        return false;
    }

    // Create a dedicated display connection for XRecord
    record_display = XOpenDisplay(nullptr);
    if (!record_display)
    {
        return false;
    }

    // Create a separate display connection for control operations
    control_display = XOpenDisplay(nullptr);
    if (!control_display)
    {
        XCloseDisplay(record_display);
        record_display = nullptr;
        return false;
    }

    // Create a record range for input events
    record_range = XRecordAllocRange();
    if (!record_range)
    {
        XCloseDisplay(record_display);
        XCloseDisplay(control_display);
        record_display = nullptr;
        control_display = nullptr;
        return false;
    }

    // Set the range to capture keyboard and mouse events
    record_range->device_events.first = KeyPress;
    record_range->device_events.last = MotionNotify;

    // Create client specification - all clients
    XRecordClientSpec client_spec = XRecordAllClients;

    // Create a record context
    record_context = XRecordCreateContext(record_display, 0, &client_spec, 1, &record_range, 1);
    if (record_context == 0)
    {
        XFree(record_range);
        XCloseDisplay(record_display);
        XCloseDisplay(control_display);
        record_display = nullptr;
        control_display = nullptr;
        record_range = nullptr;
        return false;
    }

    record_initialized = true;
    return true;
}

void X11Protocol::CleanupXRecord()
{
    if (record_initialized)
    {
        // Free the context first
        if (record_context != 0)
        {
            if (control_display)
                XRecordFreeContext(control_display, record_context);
            record_context = 0;
        }

        // Free resources
        if (record_range)
        {
            XFree(record_range);
            record_range = nullptr;
        }

        if (record_display)
        {
            XCloseDisplay(record_display);
            record_display = nullptr;
        }

        if (control_display)
        {
            XCloseDisplay(control_display);
            control_display = nullptr;
        }

        record_initialized = false;
    }
}

bool X11Protocol::SetupXRecordMonitoring()
{
    if (!record_display || !record_initialized)
        return false;

    return true;
}

void X11Protocol::XRecordMonitoringThreadProc()
{
    if (!record_display || !record_initialized)
        return;

    // Enable XRecord context (this will block until disabled)
    // The thread will be interrupted when XRecordDisableContext is called from StopInputMonitoring
    while (input_monitoring_running && record_display && record_context != 0)
    {
        XRecordEnableContext(record_display, record_context, XRecordDataCallback, (XPointer)this);

        // If we reach here, it means XRecordEnableContext returned (was disabled)
        // Check if we should continue or exit
        if (!input_monitoring_running)
            break;
    }
}

// Static callback function for XRecord
void X11Protocol::XRecordDataCallback(XPointer closure, XRecordInterceptData* data)
{
    X11Protocol* instance = reinterpret_cast<X11Protocol*>(closure);
    if (instance && data)
    {
        instance->ProcessXRecordData(data);
    }
}

void X11Protocol::ProcessXRecordData(XRecordInterceptData* data)
{
    if (!data || !data->data)
        return;

    // Parse the X11 protocol data
    if (data->category == XRecordFromServer && data->data_len >= 8)
    {
        // Get the event type from the first byte
        unsigned char event_type = data->data[0] & 0x7f;  // Remove the send_event bit

        // Extract basic event data from the 8-byte protocol data
        unsigned char* event_data = data->data;

        switch (event_type)
        {
            case ButtonPress:
            case ButtonRelease:
            {
                if (mouse_callback)
                {
                    // Get current mouse position for button events
                    Point current_pos = GetCurrentMousePosition();

                    // For 8-byte data, try a simpler approach
                    // Just use current mouse position and button from second byte
                    unsigned char button = event_data[1];

                    MouseEventContext* mouseEvent = new MouseEventContext();
                    mouseEvent->value = (event_type == ButtonPress) ? 1 : 0;
                    mouseEvent->pos = current_pos;  // Use actual mouse position

                    // Map X11 button numbers to Linux input event codes
                    switch (button)
                    {
                        case 1:  // Left button
                            mouseEvent->type = EV_KEY;
                            mouseEvent->code = BTN_LEFT;
                            mouseEvent->button = static_cast<int>(MouseButton::Left);
                            mouseEvent->flag = 0;
                            break;
                        case 2:  // Middle button
                            mouseEvent->type = EV_KEY;
                            mouseEvent->code = BTN_MIDDLE;
                            mouseEvent->button = static_cast<int>(MouseButton::Middle);
                            mouseEvent->flag = 0;
                            break;
                        case 3:  // Right button
                            mouseEvent->type = EV_KEY;
                            mouseEvent->code = BTN_RIGHT;
                            mouseEvent->button = static_cast<int>(MouseButton::Right);
                            mouseEvent->flag = 0;
                            break;
                        case 4:  // Wheel up
                            mouseEvent->type = EV_REL;
                            mouseEvent->code = REL_WHEEL;
                            mouseEvent->value = 1;
                            mouseEvent->button = static_cast<int>(MouseButton::WheelVertical);
                            mouseEvent->flag = 1;
                            break;
                        case 5:  // Wheel down
                            mouseEvent->type = EV_REL;
                            mouseEvent->code = REL_WHEEL;
                            mouseEvent->value = -1;
                            mouseEvent->button = static_cast<int>(MouseButton::WheelVertical);
                            mouseEvent->flag = -1;
                            break;
                        case 6:  // Wheel left
                            mouseEvent->type = EV_REL;
                            mouseEvent->code = REL_HWHEEL;
                            mouseEvent->value = -1;
                            mouseEvent->button = static_cast<int>(MouseButton::WheelHorizontal);
                            mouseEvent->flag = -1;
                            break;
                        case 7:  // Wheel right
                            mouseEvent->type = EV_REL;
                            mouseEvent->code = REL_HWHEEL;
                            mouseEvent->value = 1;
                            mouseEvent->button = static_cast<int>(MouseButton::WheelHorizontal);
                            mouseEvent->flag = 1;
                            break;
                        case 8:  // Back button
                            mouseEvent->type = EV_KEY;
                            mouseEvent->code = BTN_BACK;
                            mouseEvent->button = static_cast<int>(MouseButton::Back);
                            mouseEvent->flag = 0;
                            break;
                        case 9:  // Forward button
                            mouseEvent->type = EV_KEY;
                            mouseEvent->code = BTN_FORWARD;
                            mouseEvent->button = static_cast<int>(MouseButton::Forward);
                            mouseEvent->flag = 0;
                            break;
                        default:
                            mouseEvent->type = EV_KEY;
                            mouseEvent->code = button;
                            mouseEvent->button = static_cast<int>(MouseButton::Unknown);
                            mouseEvent->flag = 0;
                            break;
                    }

                    mouse_callback(callback_context, mouseEvent);
                }
                break;
            }
            case MotionNotify:
            {
                if (mouse_callback)
                {
                    // Get actual mouse position for motion events
                    Point new_pos = GetCurrentMousePosition();

                    // Generate mouse move event with absolute position
                    MouseEventContext* mouseEvent = new MouseEventContext();
                    mouseEvent->type = EV_REL;
                    mouseEvent->code = REL_X;
                    mouseEvent->value = 0;  // No delta calculation without previous position
                    mouseEvent->pos = new_pos;
                    mouseEvent->button = static_cast<int>(MouseButton::None);
                    mouseEvent->flag = 0;

                    mouse_callback(callback_context, mouseEvent);
                }
                break;
            }
            case KeyPress:
            case KeyRelease:
            {
                if (keyboard_callback)
                {
                    // Extract keycode from protocol data
                    unsigned char keycode = event_data[1];

                    KeyboardEventContext* keyboardEvent = new KeyboardEventContext();
                    keyboardEvent->type = EV_KEY;
                    keyboardEvent->code = keycode;
                    keyboardEvent->value = (event_type == KeyPress) ? 1 : 0;
                    keyboardEvent->flags = 0;  // TODO: Add modifier flags

                    keyboard_callback(callback_context, keyboardEvent);
                }
                break;
            }
            default:
                break;
        }
    }

    // Free the data
    XRecordFreeData(data);
}

// Factory function to create X11Protocol instance
std::unique_ptr<ProtocolBase> CreateX11Protocol()
{
    return std::make_unique<X11Protocol>();
}
