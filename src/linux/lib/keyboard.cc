/**
 * Keyboard Utilities for Linux
 *
 * Provides functions to convert Linux input event key codes to MDN Web API
 * KeyboardEvent.key values for cross-platform compatibility.
 *
 * Reference: https://developer.mozilla.org/en-US/docs/Web/API/UI_Events/Keyboard_event_key_values
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */

#include "keyboard.h"

#include <linux/input.h>

#include <unordered_map>

#include "../common.h"

// Static mapping for Linux key codes to MDN KeyboardEvent.key values
// MDN standard: modifier keys return the same value regardless of left/right
static const std::unordered_map<unsigned int, std::string> keyCodeMap = {
    // Letter keys (A-Z) - lowercase by default, uppercase handled in converter
    {KEY_A, "a"},
    {KEY_B, "b"},
    {KEY_C, "c"},
    {KEY_D, "d"},
    {KEY_E, "e"},
    {KEY_F, "f"},
    {KEY_G, "g"},
    {KEY_H, "h"},
    {KEY_I, "i"},
    {KEY_J, "j"},
    {KEY_K, "k"},
    {KEY_L, "l"},
    {KEY_M, "m"},
    {KEY_N, "n"},
    {KEY_O, "o"},
    {KEY_P, "p"},
    {KEY_Q, "q"},
    {KEY_R, "r"},
    {KEY_S, "s"},
    {KEY_T, "t"},
    {KEY_U, "u"},
    {KEY_V, "v"},
    {KEY_W, "w"},
    {KEY_X, "x"},
    {KEY_Y, "y"},
    {KEY_Z, "z"},

    // Number keys (0-9)
    {KEY_1, "1"},
    {KEY_2, "2"},
    {KEY_3, "3"},
    {KEY_4, "4"},
    {KEY_5, "5"},
    {KEY_6, "6"},
    {KEY_7, "7"},
    {KEY_8, "8"},
    {KEY_9, "9"},
    {KEY_0, "0"},

    // Punctuation and symbols
    {KEY_MINUS, "-"},
    {KEY_EQUAL, "="},
    {KEY_LEFTBRACE, "["},
    {KEY_RIGHTBRACE, "]"},
    {KEY_BACKSLASH, "\\"},
    {KEY_SEMICOLON, ";"},
    {KEY_APOSTROPHE, "'"},
    {KEY_GRAVE, "`"},
    {KEY_COMMA, ","},
    {KEY_DOT, "."},
    {KEY_SLASH, "/"},

    // Special keys
    {KEY_ENTER, "Enter"},
    {KEY_TAB, "Tab"},
    {KEY_SPACE, " "},
    {KEY_BACKSPACE, "Backspace"},
    {KEY_ESC, "Escape"},
    {KEY_DELETE, "Delete"},
    {KEY_INSERT, "Insert"},

    // Navigation keys
    {KEY_HOME, "Home"},
    {KEY_END, "End"},
    {KEY_PAGEUP, "PageUp"},
    {KEY_PAGEDOWN, "PageDown"},
    {KEY_UP, "ArrowUp"},
    {KEY_DOWN, "ArrowDown"},
    {KEY_LEFT, "ArrowLeft"},
    {KEY_RIGHT, "ArrowRight"},

    // Modifier keys - MDN standard (no left/right distinction in uniKey)
    {KEY_LEFTSHIFT, "Shift"},
    {KEY_RIGHTSHIFT, "Shift"},
    {KEY_LEFTCTRL, "Control"},
    {KEY_RIGHTCTRL, "Control"},
    {KEY_LEFTALT, "Alt"},
    {KEY_RIGHTALT, "Alt"},
    {KEY_LEFTMETA, "Meta"},
    {KEY_RIGHTMETA, "Meta"},

    // Lock keys
    {KEY_CAPSLOCK, "CapsLock"},
    {KEY_NUMLOCK, "NumLock"},
    {KEY_SCROLLLOCK, "ScrollLock"},

    // Function keys
    {KEY_F1, "F1"},
    {KEY_F2, "F2"},
    {KEY_F3, "F3"},
    {KEY_F4, "F4"},
    {KEY_F5, "F5"},
    {KEY_F6, "F6"},
    {KEY_F7, "F7"},
    {KEY_F8, "F8"},
    {KEY_F9, "F9"},
    {KEY_F10, "F10"},
    {KEY_F11, "F11"},
    {KEY_F12, "F12"},
    {KEY_F13, "F13"},
    {KEY_F14, "F14"},
    {KEY_F15, "F15"},
    {KEY_F16, "F16"},
    {KEY_F17, "F17"},
    {KEY_F18, "F18"},
    {KEY_F19, "F19"},
    {KEY_F20, "F20"},
    {KEY_F21, "F21"},
    {KEY_F22, "F22"},
    {KEY_F23, "F23"},
    {KEY_F24, "F24"},

    // Numeric keypad
    {KEY_KP0, "0"},
    {KEY_KP1, "1"},
    {KEY_KP2, "2"},
    {KEY_KP3, "3"},
    {KEY_KP4, "4"},
    {KEY_KP5, "5"},
    {KEY_KP6, "6"},
    {KEY_KP7, "7"},
    {KEY_KP8, "8"},
    {KEY_KP9, "9"},
    {KEY_KPDOT, "."},
    {KEY_KPPLUS, "+"},
    {KEY_KPMINUS, "-"},
    {KEY_KPASTERISK, "*"},
    {KEY_KPSLASH, "/"},
    {KEY_KPENTER, "Enter"},
    {KEY_KPCOMMA, ","},

    // System keys
    {KEY_SYSRQ, "PrintScreen"},
    {KEY_PAUSE, "Pause"},
    {KEY_COMPOSE, "ContextMenu"},

    // Media keys
    {KEY_MUTE, "AudioVolumeMute"},
    {KEY_VOLUMEDOWN, "AudioVolumeDown"},
    {KEY_VOLUMEUP, "AudioVolumeUp"},
    {KEY_NEXTSONG, "MediaTrackNext"},
    {KEY_PREVIOUSSONG, "MediaTrackPrevious"},
    {KEY_STOPCD, "MediaStop"},
    {KEY_PLAYPAUSE, "MediaPlayPause"},
};

// Shifted symbol mapping for number row and punctuation
static const std::unordered_map<std::string, std::string> shiftedSymbols = {
    {"1", "!"},  {"2", "@"}, {"3", "#"},  {"4", "$"}, {"5", "%"}, {"6", "^"}, {"7", "&"},
    {"8", "*"},  {"9", "("}, {"0", ")"},  {"-", "_"}, {"=", "+"}, {"[", "{"}, {"]", "}"},
    {"\\", "|"}, {";", ":"}, {"'", "\""}, {"`", "~"}, {",", "<"}, {".", ">"}, {"/", "?"},
};

/**
 * Convert Linux key code to MDN KeyboardEvent.key value
 * @param keyCode The Linux key code (KEY_*)
 * @param flags Modifier flags bitmask
 * @return The corresponding KeyboardEvent.key string value
 */
std::string convertKeyCodeToUniKey(unsigned int keyCode, unsigned int flags)
{
    auto it = keyCodeMap.find(keyCode);
    if (it == keyCodeMap.end())
    {
        return "Unidentified";
    }

    std::string key = it->second;

    // Handle case sensitivity for letter keys based on Shift state
    if (key.length() == 1 && key[0] >= 'a' && key[0] <= 'z')
    {
        bool shiftPressed = (flags & MODIFIER_SHIFT) != 0;
        // TODO: CapsLock state tracking could be added here if needed
        if (shiftPressed)
        {
            key[0] = key[0] - 'a' + 'A';
        }
        return key;
    }

    // Handle shifted symbols for number row and punctuation
    if ((flags & MODIFIER_SHIFT) != 0)
    {
        auto shiftIt = shiftedSymbols.find(key);
        if (shiftIt != shiftedSymbols.end())
        {
            return shiftIt->second;
        }
    }

    return key;
}
