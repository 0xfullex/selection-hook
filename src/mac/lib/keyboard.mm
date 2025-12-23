/**
 * Keyboard utility functions for macOS
 * Convert macOS virtual key codes to MDN WebAPI KeyboardEvent.key values
 *
 * Reference: https://developer.mozilla.org/en-US/docs/Web/API/UI_Events/Keyboard_event_key_values
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */

#import "keyboard.h"

// Static mapping table for macOS virtual key codes to MDN KeyboardEvent.key values
static const std::unordered_map<CGKeyCode, std::string> keyCodeMap = {
    // Letter keys (A-Z)
    {kVK_ANSI_A, "a"},
    {kVK_ANSI_B, "b"},
    {kVK_ANSI_C, "c"},
    {kVK_ANSI_D, "d"},
    {kVK_ANSI_E, "e"},
    {kVK_ANSI_F, "f"},
    {kVK_ANSI_G, "g"},
    {kVK_ANSI_H, "h"},
    {kVK_ANSI_I, "i"},
    {kVK_ANSI_J, "j"},
    {kVK_ANSI_K, "k"},
    {kVK_ANSI_L, "l"},
    {kVK_ANSI_M, "m"},
    {kVK_ANSI_N, "n"},
    {kVK_ANSI_O, "o"},
    {kVK_ANSI_P, "p"},
    {kVK_ANSI_Q, "q"},
    {kVK_ANSI_R, "r"},
    {kVK_ANSI_S, "s"},
    {kVK_ANSI_T, "t"},
    {kVK_ANSI_U, "u"},
    {kVK_ANSI_V, "v"},
    {kVK_ANSI_W, "w"},
    {kVK_ANSI_X, "x"},
    {kVK_ANSI_Y, "y"},
    {kVK_ANSI_Z, "z"},

    // Number keys (0-9)
    {kVK_ANSI_0, "0"},
    {kVK_ANSI_1, "1"},
    {kVK_ANSI_2, "2"},
    {kVK_ANSI_3, "3"},
    {kVK_ANSI_4, "4"},
    {kVK_ANSI_5, "5"},
    {kVK_ANSI_6, "6"},
    {kVK_ANSI_7, "7"},
    {kVK_ANSI_8, "8"},
    {kVK_ANSI_9, "9"},

    // Punctuation and symbols
    {kVK_ANSI_Minus, "-"},
    {kVK_ANSI_Equal, "="},
    {kVK_ANSI_LeftBracket, "["},
    {kVK_ANSI_RightBracket, "]"},
    {kVK_ANSI_Backslash, "\\"},
    {kVK_ANSI_Semicolon, ";"},
    {kVK_ANSI_Quote, "'"},
    {kVK_ANSI_Grave, "`"},
    {kVK_ANSI_Comma, ","},
    {kVK_ANSI_Period, "."},
    {kVK_ANSI_Slash, "/"},

    // Special keys
    {kVK_Return, "Enter"},
    {kVK_Tab, "Tab"},
    {kVK_Space, " "},
    {kVK_Delete, "Backspace"},
    {kVK_Escape, "Escape"},
    {kVK_ForwardDelete, "Delete"},

    // Navigation keys
    {kVK_Home, "Home"},
    {kVK_End, "End"},
    {kVK_PageUp, "PageUp"},
    {kVK_PageDown, "PageDown"},
    {kVK_LeftArrow, "ArrowLeft"},
    {kVK_RightArrow, "ArrowRight"},
    {kVK_UpArrow, "ArrowUp"},
    {kVK_DownArrow, "ArrowDown"},

    // Modifier keys
    {kVK_Shift, "Shift"},
    {kVK_RightShift, "RightShift"},
    {kVK_Control, "Control"},
    {kVK_RightControl, "RightControl"},
    {kVK_Option, "Alt"},
    {kVK_RightOption, "RightAlt"},
    {kVK_Command, "Meta"},
    {kVK_RightCommand, "RightMeta"},
    {kVK_CapsLock, "CapsLock"},

    // Function keys
    {kVK_F1, "F1"},
    {kVK_F2, "F2"},
    {kVK_F3, "F3"},
    {kVK_F4, "F4"},
    {kVK_F5, "F5"},
    {kVK_F6, "F6"},
    {kVK_F7, "F7"},
    {kVK_F8, "F8"},
    {kVK_F9, "F9"},
    {kVK_F10, "F10"},
    {kVK_F11, "F11"},
    {kVK_F12, "F12"},
    {kVK_F13, "F13"},
    {kVK_F14, "F14"},
    {kVK_F15, "F15"},
    {kVK_F16, "F16"},
    {kVK_F17, "F17"},
    {kVK_F18, "F18"},
    {kVK_F19, "F19"},
    {kVK_F20, "F20"},

    // Numeric keypad
    {kVK_ANSI_Keypad0, "0"},
    {kVK_ANSI_Keypad1, "1"},
    {kVK_ANSI_Keypad2, "2"},
    {kVK_ANSI_Keypad3, "3"},
    {kVK_ANSI_Keypad4, "4"},
    {kVK_ANSI_Keypad5, "5"},
    {kVK_ANSI_Keypad6, "6"},
    {kVK_ANSI_Keypad7, "7"},
    {kVK_ANSI_Keypad8, "8"},
    {kVK_ANSI_Keypad9, "9"},
    {kVK_ANSI_KeypadDecimal, "."},
    {kVK_ANSI_KeypadMultiply, "*"},
    {kVK_ANSI_KeypadPlus, "+"},
    {kVK_ANSI_KeypadClear, "Clear"},
    {kVK_ANSI_KeypadDivide, "/"},
    {kVK_ANSI_KeypadEnter, "Enter"},
    {kVK_ANSI_KeypadMinus, "-"},
    {kVK_ANSI_KeypadEquals, "="},

    // System keys
    {kVK_Help, "Help"},
    {kVK_VolumeUp, "AudioVolumeUp"},
    {kVK_VolumeDown, "AudioVolumeDown"},
    {kVK_Mute, "AudioVolumeMute"},

    // Additional special keys
    {kVK_ISO_Section, "§"},
    {kVK_JIS_Yen, "¥"},
    {kVK_JIS_Underscore, "_"},
    {kVK_JIS_KeypadComma, ","},
    {kVK_JIS_Eisu, "Eisu"},
    {kVK_JIS_Kana, "KanaMode"},

    // Additional system keys (removed undefined PC keyboard constants)
};

/**
 * Convert macOS virtual key code to MDN WebAPI KeyboardEvent.key value
 * @param keyCode The macOS virtual key code (CGKeyCode)
 * @param flags Event flags to determine modifier states
 * @return String representation of the key following MDN KeyboardEvent.key specification
 */
std::string convertKeyCodeToUniKey(CGKeyCode keyCode, CGEventFlags flags)
{
    // Look up the key code in the mapping table
    auto it = keyCodeMap.find(keyCode);
    if (it != keyCodeMap.end())
    {
        std::string key = it->second;

        // Handle case sensitivity for letter keys based on Shift and CapsLock state
        if (key.length() == 1 && key[0] >= 'a' && key[0] <= 'z')
        {
            bool shouldBeUppercase = false;

            // Check if Shift is pressed
            bool shiftPressed = (flags & kCGEventFlagMaskShift) != 0;
            // Check if CapsLock is active
            bool capsLockActive = (flags & kCGEventFlagMaskAlphaShift) != 0;

            // XOR logic: uppercase if exactly one of Shift or CapsLock is active
            shouldBeUppercase = shiftPressed != capsLockActive;

            if (shouldBeUppercase)
            {
                key[0] = key[0] - 'a' + 'A';
            }
        }

        // Handle shifted symbols for number row and punctuation
        if ((flags & kCGEventFlagMaskShift) != 0)
        {
            static const std::unordered_map<std::string, std::string> shiftedSymbols = {
                {"1", "!"},  {"2", "@"}, {"3", "#"},  {"4", "$"}, {"5", "%"}, {"6", "^"}, {"7", "&"},
                {"8", "*"},  {"9", "("}, {"0", ")"},  {"-", "_"}, {"=", "+"}, {"[", "{"}, {"]", "}"},
                {"\\", "|"}, {";", ":"}, {"'", "\""}, {"`", "~"}, {",", "<"}, {".", ">"}, {"/", "?"}};

            auto shiftIt = shiftedSymbols.find(key);
            if (shiftIt != shiftedSymbols.end())
            {
                key = shiftIt->second;
            }
        }

        return key;
    }

    // If key code is not found in the mapping table, return "Unidentified"
    // as per MDN specification
    return "Unidentified";
}