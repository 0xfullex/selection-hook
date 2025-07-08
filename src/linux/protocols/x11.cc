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
#include <X11/keysym.h>

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

  public:
    X11Protocol() : display(nullptr), screen(0), root(0) {}

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

    // Input monitoring implementation (placeholder for future XInput2 implementation)
    bool InitializeInputMonitoring(MouseEventCallback mouseCallback, KeyboardEventCallback keyboardCallback,
                                   void* context) override
    {
        // TODO: Implement X11-specific input monitoring using XInput2
        // This would involve setting up XInput2 device monitoring
        // For now, return false to indicate not implemented
        return false;
    }

    void CleanupInputMonitoring() override
    {
        // TODO: Implement X11-specific input monitoring cleanup
        // This would involve cleaning up XInput2 resources
    }

    bool StartInputMonitoring() override
    {
        // TODO: Implement X11-specific input monitoring start
        // This would start the XInput2 event monitoring
        return false;
    }

    void StopInputMonitoring() override
    {
        // TODO: Implement X11-specific input monitoring stop
        // This would stop the XInput2 event monitoring
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

// Factory function to create X11Protocol instance
std::unique_ptr<ProtocolBase> CreateX11Protocol()
{
    return std::make_unique<X11Protocol>();
}
