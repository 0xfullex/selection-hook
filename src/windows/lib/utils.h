/**
 * Utility functions for text selection hook
 */
#pragma once

#include <windows.h>

#include <string>
#include <vector>

/**
 * Check if string is empty after trimming whitespace
 */
bool IsTrimmedEmpty(const std::wstring &text);

/**
 * Get window under mouse point, including floating tool windows
 */
HWND GetWindowUnderMouse();

/**
 * Check if window has moved since mouse down
 */
bool HasWindowMoved(const RECT &currentWindowRect, const RECT &lastWindowRect);

/**
 * Get program name (executable path) from a window handle
 */
bool GetProgramNameFromHwnd(HWND hwnd, std::wstring &programName);
