/**
 * Keyboard Utilities for Linux - Header File
 *
 * Provides functions to convert Linux input event key codes to MDN Web API
 * KeyboardEvent.key values for cross-platform compatibility.
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */

#pragma once

#include <string>

/**
 * Convert Linux key code to MDN KeyboardEvent.key value
 *
 * This function converts Linux input event key codes (KEY_*) to their corresponding
 * MDN Web API KeyboardEvent.key string values according to the specification at:
 * https://developer.mozilla.org/en-US/docs/Web/API/UI_Events/Keyboard_event_key_values
 *
 * @param keyCode The Linux key code (e.g., KEY_A, KEY_ENTER, etc.)
 * @param flags Modifier flags bitmask (MODIFIER_SHIFT, MODIFIER_CTRL, etc.)
 * @return The corresponding KeyboardEvent.key string value (e.g., "a", "Enter", "Control")
 *         Returns "Unidentified" for unknown or unmappable keys
 */
std::string convertKeyCodeToUniKey(unsigned int keyCode, unsigned int flags = 0);
