/**
 * Keyboard Utilities for Windows
 *
 * Provides functions to convert Windows virtual key codes to MDN Web API
 * KeyboardEvent.key values for cross-platform compatibility.
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */

#include "keyboard.h"

// Pre-allocated string constants to avoid repeated allocations
// MDN standard: no left/right distinction in uniKey values
static const std::string UNIDENTIFIED_KEY = "Unidentified";
static const std::string CONTROL_KEY = "Control";
static const std::string ALT_KEY = "Alt";
static const std::string SHIFT_KEY = "Shift";
static const std::string META_KEY = "Meta";

// Character constants for better performance
static constexpr char SHIFTED_NUMBERS[] = ")!@#$%^&*(";

// Static mapping for virtual key codes to MDN KeyboardEvent.key values
// MDN standard: modifier keys return the same value regardless of left/right
static const std::unordered_map<DWORD, const std::string*> vkCodeMaps = {
    // Control keys
    {VK_LCONTROL, &CONTROL_KEY},
    {VK_RCONTROL, &CONTROL_KEY},
    {VK_CONTROL, &CONTROL_KEY},

    // Alt keys
    {VK_LMENU, &ALT_KEY},
    {VK_RMENU, &ALT_KEY},
    {VK_MENU, &ALT_KEY},

    // Shift keys
    {VK_LSHIFT, &SHIFT_KEY},
    {VK_RSHIFT, &SHIFT_KEY},
    {VK_SHIFT, &SHIFT_KEY},

    // Windows keys
    {VK_LWIN, &META_KEY},
    {VK_RWIN, &META_KEY},

    // Lock keys
    {VK_CAPITAL, new std::string("CapsLock")},
    {VK_NUMLOCK, new std::string("NumLock")},
    {VK_SCROLL, new std::string("ScrollLock")},

    // Function keys
    {VK_F1, new std::string("F1")},
    {VK_F2, new std::string("F2")},
    {VK_F3, new std::string("F3")},
    {VK_F4, new std::string("F4")},
    {VK_F5, new std::string("F5")},
    {VK_F6, new std::string("F6")},
    {VK_F7, new std::string("F7")},
    {VK_F8, new std::string("F8")},
    {VK_F9, new std::string("F9")},
    {VK_F10, new std::string("F10")},
    {VK_F11, new std::string("F11")},
    {VK_F12, new std::string("F12")},
    {VK_F13, new std::string("F13")},
    {VK_F14, new std::string("F14")},
    {VK_F15, new std::string("F15")},
    {VK_F16, new std::string("F16")},
    {VK_F17, new std::string("F17")},
    {VK_F18, new std::string("F18")},
    {VK_F19, new std::string("F19")},
    {VK_F20, new std::string("F20")},
    {VK_F21, new std::string("F21")},
    {VK_F22, new std::string("F22")},
    {VK_F23, new std::string("F23")},
    {VK_F24, new std::string("F24")},

    // Navigation keys
    {VK_HOME, new std::string("Home")},
    {VK_END, new std::string("End")},
    {VK_PRIOR, new std::string("PageUp")},
    {VK_NEXT, new std::string("PageDown")},
    {VK_UP, new std::string("ArrowUp")},
    {VK_DOWN, new std::string("ArrowDown")},
    {VK_LEFT, new std::string("ArrowLeft")},
    {VK_RIGHT, new std::string("ArrowRight")},

    // Editing keys
    {VK_INSERT, new std::string("Insert")},
    {VK_DELETE, new std::string("Delete")},
    {VK_BACK, new std::string("Backspace")},

    // Whitespace keys
    {VK_SPACE, new std::string(" ")},
    {VK_TAB, new std::string("Tab")},
    {VK_RETURN, new std::string("Enter")},

    // Escape key
    {VK_ESCAPE, new std::string("Escape")},

    // Print Screen
    {VK_SNAPSHOT, new std::string("PrintScreen")},

    // Pause/Break
    {VK_PAUSE, new std::string("Pause")},

    // Context menu
    {VK_APPS, new std::string("ContextMenu")},

    // Numeric keypad keys
    {VK_NUMPAD0, new std::string("0")},
    {VK_NUMPAD1, new std::string("1")},
    {VK_NUMPAD2, new std::string("2")},
    {VK_NUMPAD3, new std::string("3")},
    {VK_NUMPAD4, new std::string("4")},
    {VK_NUMPAD5, new std::string("5")},
    {VK_NUMPAD6, new std::string("6")},
    {VK_NUMPAD7, new std::string("7")},
    {VK_NUMPAD8, new std::string("8")},
    {VK_NUMPAD9, new std::string("9")},
    {VK_DECIMAL, new std::string(".")},
    {VK_ADD, new std::string("+")},
    {VK_SUBTRACT, new std::string("-")},
    {VK_MULTIPLY, new std::string("*")},
    {VK_DIVIDE, new std::string("/")},
    {VK_SEPARATOR, new std::string(",")},
    {VK_CLEAR, new std::string("Clear")},

    // Media keys
    {VK_VOLUME_MUTE, new std::string("AudioVolumeMute")},
    {VK_VOLUME_DOWN, new std::string("AudioVolumeDown")},
    {VK_VOLUME_UP, new std::string("AudioVolumeUp")},
    {VK_MEDIA_NEXT_TRACK, new std::string("MediaTrackNext")},
    {VK_MEDIA_PREV_TRACK, new std::string("MediaTrackPrevious")},
    {VK_MEDIA_STOP, new std::string("MediaStop")},
    {VK_MEDIA_PLAY_PAUSE, new std::string("MediaPlayPause")},

    // Browser keys
    {VK_BROWSER_BACK, new std::string("BrowserBack")},
    {VK_BROWSER_FORWARD, new std::string("BrowserForward")},
    {VK_BROWSER_REFRESH, new std::string("BrowserRefresh")},
    {VK_BROWSER_STOP, new std::string("BrowserStop")},
    {VK_BROWSER_SEARCH, new std::string("BrowserSearch")},
    {VK_BROWSER_FAVORITES, new std::string("BrowserFavorites")},
    {VK_BROWSER_HOME, new std::string("BrowserHome")},

    // Application launcher keys
    {VK_LAUNCH_MAIL, new std::string("LaunchMail")},
    {VK_LAUNCH_MEDIA_SELECT, new std::string("LaunchMediaPlayer")},
    {VK_LAUNCH_APP1, new std::string("LaunchApplication1")},
    {VK_LAUNCH_APP2, new std::string("LaunchApplication2")}};

// Optimized OEM key mappings with pre-allocated strings
struct OemKeyMapping
{
    const std::string normal;
    const std::string shifted;
};

static const std::unordered_map<DWORD, OemKeyMapping> oemKeyMaps = {
    {VK_OEM_1, {";", ":"}},      {VK_OEM_PLUS, {"=", "+"}}, {VK_OEM_COMMA, {",", "<"}}, {VK_OEM_MINUS, {"-", "_"}},
    {VK_OEM_PERIOD, {".", ">"}}, {VK_OEM_2, {"/", "?"}},    {VK_OEM_3, {"`", "~"}},     {VK_OEM_4, {"[", "{"}},
    {VK_OEM_5, {"\\", "|"}},     {VK_OEM_6, {"]", "}"}},    {VK_OEM_7, {"'", "\""}},    {VK_OEM_102, {"\\", "|"}}};

// Cached key state to reduce API calls
struct KeyStateCache
{
    bool isShiftPressed;
    bool isCapsLockOn;
    DWORD lastUpdateTime;

    void update()
    {
        DWORD currentTime = GetTickCount();
        // Only update if more than 15ms have passed to avoid excessive API calls
        if (currentTime - lastUpdateTime > 15)
        {
            SHORT shiftState = GetAsyncKeyState(VK_SHIFT);
            SHORT lshiftState = GetAsyncKeyState(VK_LSHIFT);
            SHORT rshiftState = GetAsyncKeyState(VK_RSHIFT);
            isShiftPressed = (shiftState & 0x8000) || (lshiftState & 0x8000) || (rshiftState & 0x8000);
            isCapsLockOn = GetKeyState(VK_CAPITAL) & 0x0001;
            lastUpdateTime = currentTime;
        }
    }
};

static thread_local KeyStateCache keyStateCache = {false, false, 0};

/**
 * Convert Windows virtual key code to MDN KeyboardEvent.key value
 * @param vkCode The Windows virtual key code
 * @param scanCode The hardware scan code (optional, for extended key detection)
 * @param flags Additional flags from the keyboard hook
 * @return The corresponding KeyboardEvent.key string value
 */
std::string convertVkCodeToUniKey(DWORD vkCode, DWORD scanCode, DWORD flags)
{
    // First, check if the key is in our static mapping
    auto it = vkCodeMaps.find(vkCode);
    if (it != vkCodeMaps.end())
    {
        return *(it->second);
    }

    // Handle alphanumeric keys and symbols with optimized logic
    if ((vkCode >= 'A' && vkCode <= 'Z') || (vkCode >= '0' && vkCode <= '9') ||
        (vkCode >= VK_OEM_1 && vkCode <= VK_OEM_3) || (vkCode >= VK_OEM_4 && vkCode <= VK_OEM_8) ||
        vkCode == VK_OEM_102)
    {
        // Update cached key state
        keyStateCache.update();

        // Handle letter keys with optimized logic
        if (vkCode >= 'A' && vkCode <= 'Z')
        {
            // XOR logic: uppercase if either Shift is pressed OR CapsLock is on, but not both
            bool shouldBeUppercase = keyStateCache.isShiftPressed ^ keyStateCache.isCapsLockOn;

            if (shouldBeUppercase)
            {
                // Use static char to avoid string allocation for single characters
                static char upperChar[2] = {0, 0};
                upperChar[0] = static_cast<char>(vkCode);
                return std::string(upperChar, 1);
            }
            else
            {
                static char lowerChar[2] = {0, 0};
                lowerChar[0] = static_cast<char>(vkCode + ('a' - 'A'));
                return std::string(lowerChar, 1);
            }
        }
        // Handle number keys with optimized logic
        else if (vkCode >= '0' && vkCode <= '9')
        {
            if (keyStateCache.isShiftPressed)
            {
                // Use pre-allocated array for shifted numbers
                static char shiftedChar[2] = {0, 0};
                shiftedChar[0] = SHIFTED_NUMBERS[vkCode - '0'];
                return std::string(shiftedChar, 1);
            }
            else
            {
                static char normalChar[2] = {0, 0};
                normalChar[0] = static_cast<char>(vkCode);
                return std::string(normalChar, 1);
            }
        }
        // Handle OEM keys with optimized lookup
        else
        {
            auto oemIt = oemKeyMaps.find(vkCode);
            if (oemIt != oemKeyMaps.end())
            {
                return keyStateCache.isShiftPressed ? oemIt->second.shifted : oemIt->second.normal;
            }
        }
    }

    // For unhandled keys, return pre-allocated constant
    return UNIDENTIFIED_KEY;
}