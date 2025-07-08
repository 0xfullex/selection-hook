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
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
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

/**
 * X11 Protocol Class Implementation
 */
class X11Protocol : public ProtocolBase
{
  private:
    Display* display;
    int screen;
    Window root;

    // XInput2 related
    int xi_opcode;
    bool xi_initialized;
    // std::vector<int> xi_devices;

    // epoll monitoring
    int epoll_fd;
    int x11_fd;

    // Thread management
    std::atomic<bool> input_monitoring_running;
    std::thread input_monitoring_thread;

    // Callback functions
    MouseEventCallback mouse_callback;
    KeyboardEventCallback keyboard_callback;
    void* callback_context;

    // Current mouse position tracking
    Point current_mouse_pos;

    // Helper methods
    bool InitializeXInput2();
    void CleanupXInput2();
    bool SetupXInput2DeviceMonitoring();
    void InputMonitoringThreadProc();
    void ProcessXInput2Event(XGenericEventCookie* cookie);
    // bool RefreshXInput2Devices();

  public:
    X11Protocol()
        : display(nullptr),
          screen(0),
          root(0),
          xi_opcode(-1),
          xi_initialized(false),
          epoll_fd(-1),
          x11_fd(-1),
          input_monitoring_running(false),
          mouse_callback(nullptr),
          keyboard_callback(nullptr),
          callback_context(nullptr),
          current_mouse_pos(0, 0)
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
    bool GetSelectedTextFromSelection(std::string& text) override
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

    bool SetTextRangeCoordinates(uint64_t window, TextSelectionInfo& selectionInfo) override
    {
        if (!display || !window)
            return false;

        // TODO: Implement X11-specific coordinate retrieval
        // This would involve getting the selection bounds from the X11 server
        // For now, return false to indicate no coordinates available
        return false;
    }

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

    // Key operations
    void SendCopyKey(CopyKeyType type) override
    {
        if (!display)
            return;

        KeySym keysym = (type == CopyKeyType::CtrlInsert) ? XK_Insert : XK_c;
        KeyCode keycode = XKeysymToKeycode(display, keysym);
        KeyCode ctrl_keycode = XKeysymToKeycode(display, XK_Control_L);

        if (keycode != 0 && ctrl_keycode != 0)
        {
            // Press Ctrl
            XTestFakeKeyEvent(display, ctrl_keycode, True, 0);
            // Press key
            XTestFakeKeyEvent(display, keycode, True, 0);
            // Release key
            XTestFakeKeyEvent(display, keycode, False, 0);
            // Release Ctrl
            XTestFakeKeyEvent(display, ctrl_keycode, False, 0);
            XFlush(display);
        }
    }

    bool ShouldKeyInterruptViaClipboard() override
    {
        if (!display)
            return false;

        // TODO: Implement X11-specific key state checking
        // This would involve checking modifier keys and other key states
        // For now, return false
        return false;
    }

    // Input monitoring implementation using XInput2
    bool InitializeInputMonitoring(MouseEventCallback mouseCallback, KeyboardEventCallback keyboardCallback,
                                   void* context) override
    {
        if (!display)
            return false;

        // Store callback functions
        mouse_callback = mouseCallback;
        keyboard_callback = keyboardCallback;
        callback_context = context;

        // Initialize XInput2
        if (!InitializeXInput2())
            return false;

        // Setup epoll for X11 connection monitoring
        epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0)
        {
            CleanupXInput2();
            return false;
        }

        // Get X11 connection file descriptor
        x11_fd = ConnectionNumber(display);

        // Add X11 fd to epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = x11_fd;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, x11_fd, &ev) < 0)
        {
            close(epoll_fd);
            epoll_fd = -1;
            CleanupXInput2();
            return false;
        }

        // Setup XInput2 device monitoring
        if (!SetupXInput2DeviceMonitoring())
        {
            close(epoll_fd);
            epoll_fd = -1;
            CleanupXInput2();
            return false;
        }

        return true;
    }

    void CleanupInputMonitoring() override
    {
        // Stop monitoring first
        StopInputMonitoring();

        // Close epoll file descriptor
        if (epoll_fd >= 0)
        {
            close(epoll_fd);
            epoll_fd = -1;
        }

        // Cleanup XInput2
        CleanupXInput2();

        // Reset callback functions
        mouse_callback = nullptr;
        keyboard_callback = nullptr;
        callback_context = nullptr;
    }

    bool StartInputMonitoring() override
    {
        if (!display || !xi_initialized || input_monitoring_running)
            return false;

        if (epoll_fd < 0 || x11_fd < 0)
            return false;

        input_monitoring_running = true;
        input_monitoring_thread = std::thread(&X11Protocol::InputMonitoringThreadProc, this);
        return true;
    }

    void StopInputMonitoring() override
    {
        input_monitoring_running = false;
        if (input_monitoring_thread.joinable())
        {
            input_monitoring_thread.join();
        }
    }

    // X11-specific methods
    Display* GetDisplay() const { return display; }
    Window GetRootWindow() const { return root; }
    int GetScreen() const { return screen; }

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

// XInput2 helper methods implementation
bool X11Protocol::InitializeXInput2()
{
    if (!display)
        return false;

    // Check if XInput2 extension is available
    int event, error;
    if (!XQueryExtension(display, "XInputExtension", &xi_opcode, &event, &error))
    {
        return false;
    }

    // Check XInput2 version
    int major = 2, minor = 0;
    if (XIQueryVersion(display, &major, &minor) == BadRequest)
    {
        return false;
    }

    xi_initialized = true;
    // return RefreshXInput2Devices();
    return true;
}

void X11Protocol::CleanupXInput2()
{
    if (xi_initialized)
    {
        // xi_devices.clear();
        xi_initialized = false;
    }
}

// bool X11Protocol::RefreshXInput2Devices()
// {
//     if (!display || !xi_initialized)
//         return false;

//     // xi_devices.clear();

//     // Get all XI2 devices
//     int ndevices;
//     XIDeviceInfo* devices = XIQueryDevice(display, XIAllDevices, &ndevices);
//     if (!devices)
//         return false;

//     // Find all master and slave devices that can generate events
//     for (int i = 0; i < ndevices; i++)
//     {
//         XIDeviceInfo* device = &devices[i];

//         // We want master devices and slave devices that are attached
//         if (device->use == XIMasterPointer || device->use == XIMasterKeyboard)
//         {
//             xi_devices.push_back(device->deviceid);
//         }
//     }

//     XIFreeDeviceInfo(devices);
//     return !xi_devices.empty();
// }

bool X11Protocol::SetupXInput2DeviceMonitoring()
{
    if (!display || !xi_initialized)
        return false;

    // Set up event mask for raw input events
    XIEventMask eventmask;
    unsigned char mask[XIMaskLen(XI_LASTEVENT)] = {0};

    // Enable raw events for mouse and keyboard
    XISetMask(mask, XI_RawMotion);
    XISetMask(mask, XI_RawButtonPress);
    XISetMask(mask, XI_RawButtonRelease);
    XISetMask(mask, XI_RawKeyPress);
    XISetMask(mask, XI_RawKeyRelease);

    // Enable hierarchy change events for hotplug support
    // XI_HierarchyChanged is introduced in XInput2 2.3, for now we don't need it
    // XISetMask(mask, XI_HierarchyChanged);

    eventmask.deviceid = XIAllDevices;
    eventmask.mask_len = sizeof(mask);
    eventmask.mask = mask;

    // Select events on root window
    if (XISelectEvents(display, root, &eventmask, 1) != Success)
    {
        return false;
    }

    XFlush(display);
    return true;
}

void X11Protocol::InputMonitoringThreadProc()
{
    if (!display || epoll_fd < 0)
        return;

    const int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];

    while (input_monitoring_running)
    {
        // Wait for X11 events with timeout (10ms)
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, 10);

        if (num_events < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        if (num_events == 0)
            continue;

        // Process X11 events
        for (int i = 0; i < num_events; i++)
        {
            if (events[i].data.fd == x11_fd && (events[i].events & EPOLLIN))
            {
                // Process pending X11 events
                while (XPending(display) > 0)
                {
                    XEvent event;
                    XNextEvent(display, &event);

                    // Handle XInput2 events
                    if (event.type == GenericEvent && event.xcookie.extension == xi_opcode)
                    {
                        if (XGetEventData(display, &event.xcookie))
                        {
                            ProcessXInput2Event(&event.xcookie);
                            XFreeEventData(display, &event.xcookie);
                        }
                    }
                }
            }
        }
    }
}

void X11Protocol::ProcessXInput2Event(XGenericEventCookie* cookie)
{
    if (!cookie || !cookie->data)
        return;

    switch (cookie->evtype)
    {
        case XI_RawMotion:
        {
            XIRawEvent* raw_event = (XIRawEvent*)cookie->data;
            if (mouse_callback)
            {
                // Update current mouse position based on raw delta
                double dx = 0, dy = 0;
                if (raw_event->raw_values)
                {
                    if (XIMaskIsSet(raw_event->valuators.mask, 0))
                        dx = raw_event->raw_values[0];
                    if (XIMaskIsSet(raw_event->valuators.mask, 1))
                        dy = raw_event->raw_values[1];
                }

                current_mouse_pos.x += static_cast<int>(dx);
                current_mouse_pos.y += static_cast<int>(dy);

                // Generate REL_X event if dx != 0
                if (dx != 0)
                {
                    MouseEventContext* mouseEvent = new MouseEventContext();
                    mouseEvent->type = EV_REL;
                    mouseEvent->code = REL_X;
                    mouseEvent->value = static_cast<int>(dx);
                    mouseEvent->pos = current_mouse_pos;
                    mouseEvent->button = static_cast<int>(MouseButton::None);
                    mouseEvent->flag = 0;

                    mouse_callback(callback_context, mouseEvent);
                }

                // Generate REL_Y event if dy != 0
                if (dy != 0)
                {
                    MouseEventContext* mouseEvent = new MouseEventContext();
                    mouseEvent->type = EV_REL;
                    mouseEvent->code = REL_Y;
                    mouseEvent->value = static_cast<int>(dy);
                    mouseEvent->pos = current_mouse_pos;
                    mouseEvent->button = static_cast<int>(MouseButton::None);
                    mouseEvent->flag = 0;

                    mouse_callback(callback_context, mouseEvent);
                }
            }
            break;
        }
        case XI_RawButtonPress:
        case XI_RawButtonRelease:
        {
            printf("XI_RawButtonPress/Release: %d\n", cookie->evtype);

            XIRawEvent* raw_event = (XIRawEvent*)cookie->data;

            if (mouse_callback)
            {
                MouseEventContext* mouseEvent = new MouseEventContext();
                mouseEvent->value = (cookie->evtype == XI_RawButtonPress) ? 1 : 0;
                mouseEvent->pos = current_mouse_pos;

                // Map X11 button numbers to Linux input event codes
                switch (raw_event->detail)
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
                        mouseEvent->code = raw_event->detail;
                        mouseEvent->button = static_cast<int>(MouseButton::Unknown);
                        mouseEvent->flag = 0;
                        break;
                }

                mouse_callback(callback_context, mouseEvent);
            }
            break;
        }
        case XI_RawKeyPress:
        case XI_RawKeyRelease:
        {
            XIRawEvent* raw_event = (XIRawEvent*)cookie->data;

            if (keyboard_callback)
            {
                KeyboardEventContext* keyboardEvent = new KeyboardEventContext();
                keyboardEvent->type = EV_KEY;
                keyboardEvent->code = raw_event->detail;
                keyboardEvent->value = (cookie->evtype == XI_RawKeyPress) ? 1 : 0;
                keyboardEvent->flags = 0;  // TODO: Add modifier flags

                keyboard_callback(callback_context, keyboardEvent);
            }
            break;
        }
            // case XI_HierarchyChanged:
            // {
            //     // Handle device hotplug events
            //     XIHierarchyEvent* hierarchy_event = (XIHierarchyEvent*)cookie->data;
            //     for (int i = 0; i < hierarchy_event->num_info; i++)
            //     {
            //         XIHierarchyInfo* info = &hierarchy_event->info[i];
            //         if (info->flags & (XIDeviceEnabled | XIDeviceDisabled | XISlaveAdded | XISlaveRemoved))
            //         {
            //             // Refresh device list when devices are added/removed
            //             RefreshXInput2Devices();
            //             // Re-setup monitoring for new devices
            //             SetupXInput2DeviceMonitoring();
            //             break;
            //         }
            //     }
            //     break;
            // }
    }
}

// Factory function to create X11Protocol instance
std::unique_ptr<ProtocolBase> CreateX11Protocol()
{
    return std::make_unique<X11Protocol>();
}
