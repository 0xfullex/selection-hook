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
 * X11 protocol context structure
 */
struct X11Context
{
    Display* display;
    int screen;
    Window root;

    X11Context() : display(nullptr), screen(0), root(0) {}
};

// X11 protocol function implementations
bool X11_Initialize(void** context);
void X11_Cleanup(void* context);
uint64_t X11_GetActiveWindow(void* context);
bool X11_GetProgramNameFromWindow(void* context, uint64_t window, std::string& programName);
bool X11_GetSelectedTextFromSelection(void* context, std::string& text);
bool X11_SetTextRangeCoordinates(void* context, uint64_t window, TextSelectionInfo& selectionInfo);
bool X11_WriteClipboard(void* context, const std::string& text);
bool X11_ReadClipboard(void* context, std::string& text);
void X11_SendCopyKey(void* context, CopyKeyType type);
bool X11_ShouldKeyInterruptViaClipboard(void* context);

/**
 * Initialize X11 protocol
 */
bool InitializeX11Protocol(ProtocolInterface* protocol)
{
    if (!protocol)
        return false;

    protocol->protocol = DisplayProtocol::X11;
    protocol->Initialize = X11_Initialize;
    protocol->Cleanup = X11_Cleanup;
    protocol->GetActiveWindow = X11_GetActiveWindow;
    protocol->GetProgramNameFromWindow = X11_GetProgramNameFromWindow;
    protocol->GetSelectedTextFromSelection = X11_GetSelectedTextFromSelection;
    protocol->SetTextRangeCoordinates = X11_SetTextRangeCoordinates;
    protocol->WriteClipboard = X11_WriteClipboard;
    protocol->ReadClipboard = X11_ReadClipboard;
    protocol->SendCopyKey = X11_SendCopyKey;
    protocol->ShouldKeyInterruptViaClipboard = X11_ShouldKeyInterruptViaClipboard;
    protocol->context = nullptr;

    // Initialize the X11 context
    return protocol->Initialize(&protocol->context);
}

/**
 * Initialize X11 connection
 */
bool X11_Initialize(void** context)
{
    X11Context* x11Context = new X11Context();

    // Initialize X11 connection
    x11Context->display = XOpenDisplay(nullptr);
    if (!x11Context->display)
    {
        delete x11Context;
        return false;
    }

    x11Context->screen = DefaultScreen(x11Context->display);
    x11Context->root = DefaultRootWindow(x11Context->display);

    *context = x11Context;
    return true;
}

/**
 * Cleanup X11 connection
 */
void X11_Cleanup(void* context)
{
    if (!context)
        return;

    X11Context* x11Context = static_cast<X11Context*>(context);

    if (x11Context->display)
    {
        XCloseDisplay(x11Context->display);
        x11Context->display = nullptr;
    }

    delete x11Context;
}

/**
 * Get the currently active window
 */
uint64_t X11_GetActiveWindow(void* context)
{
    if (!context)
        return 0;

    X11Context* x11Context = static_cast<X11Context*>(context);
    if (!x11Context->display)
        return 0;

    Window active_window = 0;
    Atom net_active_window = XInternAtom(x11Context->display, "_NET_ACTIVE_WINDOW", False);
    Atom type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(x11Context->display, x11Context->root, net_active_window, 0, 1, False, XA_WINDOW, &type,
                           &format, &nitems, &bytes_after, &data) == Success)
    {
        if (data)
        {
            active_window = *(Window*)data;
            XFree(data);
        }
    }

    return static_cast<uint64_t>(active_window);
}

/**
 * Get program name from window
 */
bool X11_GetProgramNameFromWindow(void* context, uint64_t window, std::string& programName)
{
    if (!context || !window)
        return false;

    X11Context* x11Context = static_cast<X11Context*>(context);
    if (!x11Context->display)
        return false;

    Window x11Window = static_cast<Window>(window);

    // Try to get WM_CLASS property first
    XClassHint classHint;
    if (XGetClassHint(x11Context->display, x11Window, &classHint))
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
    if (XFetchName(x11Context->display, x11Window, &window_name) && window_name)
    {
        programName = std::string(window_name);
        XFree(window_name);
        return true;
    }

    return false;
}

/**
 * Get selected text from X11 primary selection
 */
bool X11_GetSelectedTextFromSelection(void* context, std::string& text)
{
    if (!context)
        return false;

    X11Context* x11Context = static_cast<X11Context*>(context);
    if (!x11Context->display)
        return false;

    // Create a window to receive the selection
    Window window = XCreateSimpleWindow(x11Context->display, x11Context->root, 0, 0, 1, 1, 0, 0, 0);

    // Request the primary selection
    Atom selection = XInternAtom(x11Context->display, "PRIMARY", False);
    Atom utf8_string = XInternAtom(x11Context->display, "UTF8_STRING", False);
    Atom targets = XInternAtom(x11Context->display, "TARGETS", False);
    Atom property = XInternAtom(x11Context->display, "SELECTION_DATA", False);

    XConvertSelection(x11Context->display, selection, utf8_string, property, window, CurrentTime);
    XFlush(x11Context->display);

    // Wait for the selection notification
    XEvent event;
    bool success = false;

    // Wait up to 1 second for the selection
    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time < std::chrono::milliseconds(1000))
    {
        if (XCheckTypedWindowEvent(x11Context->display, window, SelectionNotify, &event))
        {
            if (event.xselection.property != 0)  // X11 None constant
            {
                // Get the selection data
                Atom actual_type;
                int actual_format;
                unsigned long nitems, bytes_after;
                unsigned char* data = nullptr;

                if (XGetWindowProperty(x11Context->display, window, property, 0, LONG_MAX, False, AnyPropertyType,
                                       &actual_type, &actual_format, &nitems, &bytes_after, &data) == Success)
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

    XDestroyWindow(x11Context->display, window);
    return success;
}

/**
 * Set text selection coordinates (placeholder implementation)
 */
bool X11_SetTextRangeCoordinates(void* context, uint64_t window, TextSelectionInfo& selectionInfo)
{
    if (!context || !window)
        return false;

    // TODO: Implement X11-specific coordinate retrieval
    // This would involve getting the selection bounds from the X11 server
    // For now, return false to indicate no coordinates available
    return false;
}

/**
 * Write to X11 clipboard
 */
bool X11_WriteClipboard(void* context, const std::string& text)
{
    if (!context)
        return false;

    X11Context* x11Context = static_cast<X11Context*>(context);
    if (!x11Context->display)
        return false;

    // Create a window to own the selection
    Window window = XCreateSimpleWindow(x11Context->display, x11Context->root, 0, 0, 1, 1, 0, 0, 0);

    Atom clipboard = XInternAtom(x11Context->display, "CLIPBOARD", False);

    // Set ourselves as the owner of the clipboard
    XSetSelectionOwner(x11Context->display, clipboard, window, CurrentTime);

    // Check if we successfully became the owner
    bool success = (XGetSelectionOwner(x11Context->display, clipboard) == window);

    if (success)
    {
        // Store the text for later retrieval
        Atom property = XInternAtom(x11Context->display, "CLIPBOARD_DATA", False);
        XChangeProperty(x11Context->display, window, property, XA_STRING, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(text.c_str()), text.length());
    }

    XFlush(x11Context->display);

    // Note: In a real implementation, we would need to handle SelectionRequest events
    // to provide the clipboard data when other applications request it
    // For now, we'll just destroy the window
    XDestroyWindow(x11Context->display, window);

    return success;
}

/**
 * Read from X11 clipboard
 */
bool X11_ReadClipboard(void* context, std::string& text)
{
    if (!context)
        return false;

    X11Context* x11Context = static_cast<X11Context*>(context);
    if (!x11Context->display)
        return false;

    // Create a window to receive the selection
    Window window = XCreateSimpleWindow(x11Context->display, x11Context->root, 0, 0, 1, 1, 0, 0, 0);

    // Request the clipboard selection
    Atom clipboard = XInternAtom(x11Context->display, "CLIPBOARD", False);
    Atom utf8_string = XInternAtom(x11Context->display, "UTF8_STRING", False);
    Atom property = XInternAtom(x11Context->display, "CLIPBOARD_DATA", False);

    XConvertSelection(x11Context->display, clipboard, utf8_string, property, window, CurrentTime);
    XFlush(x11Context->display);

    // Wait for the selection notification
    XEvent event;
    bool success = false;

    // Wait up to 1 second for the selection
    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time < std::chrono::milliseconds(1000))
    {
        if (XCheckTypedWindowEvent(x11Context->display, window, SelectionNotify, &event))
        {
            if (event.xselection.property != 0)  // X11 None constant
            {
                // Get the selection data
                Atom actual_type;
                int actual_format;
                unsigned long nitems, bytes_after;
                unsigned char* data = nullptr;

                if (XGetWindowProperty(x11Context->display, window, property, 0, LONG_MAX, False, AnyPropertyType,
                                       &actual_type, &actual_format, &nitems, &bytes_after, &data) == Success)
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

    XDestroyWindow(x11Context->display, window);
    return success;
}

/**
 * Send copy key combination using X11
 */
void X11_SendCopyKey(void* context, CopyKeyType type)
{
    if (!context)
        return;

    X11Context* x11Context = static_cast<X11Context*>(context);
    if (!x11Context->display)
        return;

    KeySym keysym = (type == CopyKeyType::CtrlInsert) ? XK_Insert : XK_c;
    KeyCode keycode = XKeysymToKeycode(x11Context->display, keysym);
    KeyCode ctrl_keycode = XKeysymToKeycode(x11Context->display, XK_Control_L);

    if (keycode != 0 && ctrl_keycode != 0)
    {
        // Press Ctrl
        XTestFakeKeyEvent(x11Context->display, ctrl_keycode, True, 0);
        // Press key
        XTestFakeKeyEvent(x11Context->display, keycode, True, 0);
        // Release key
        XTestFakeKeyEvent(x11Context->display, keycode, False, 0);
        // Release Ctrl
        XTestFakeKeyEvent(x11Context->display, ctrl_keycode, False, 0);
        XFlush(x11Context->display);
    }
}

/**
 * Check if key should interrupt clipboard operation
 */
bool X11_ShouldKeyInterruptViaClipboard(void* context)
{
    if (!context)
        return false;

    // TODO: Implement X11-specific key state checking
    // This would involve checking modifier keys and other key states
    // For now, return false
    return false;
}
