/**
 * Keyboard Utilities for Windows - Header File
 *
 * Provides functions to convert Windows virtual key codes to MDN Web API
 * KeyboardEvent.key values for cross-platform compatibility.
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */

#pragma once

#include <windows.h>

#include <string>
#include <unordered_map>

/**
 * Convert Windows virtual key code to MDN KeyboardEvent.key value
 *
 * This function converts Windows virtual key codes (VK_*) to their corresponding
 * MDN Web API KeyboardEvent.key string values according to the specification at:
 * https://developer.mozilla.org/en-US/docs/Web/API/UI_Events/Keyboard_event_key_values
 *
 * @param vkCode The Windows virtual key code (e.g., VK_A, VK_RETURN, etc.)
 * @param scanCode The hardware scan code (optional, used for extended key detection)
 * @param flags Additional flags from the keyboard hook (e.g., LLKHF_EXTENDED)
 * @return The corresponding KeyboardEvent.key string value (e.g., "a", "Enter", "Control")
 *         Returns "Unidentified" for unknown or unmappable keys
 */
std::string convertKeyCodeToUniKey(DWORD vkCode, DWORD scanCode = 0, DWORD flags = 0);
