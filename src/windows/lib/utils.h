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

/**
 * Check whether a window was created by the calling thread.
 *
 * @param[in] hwnd Window to test.
 * @return true when hwnd belongs to the calling thread; false for nullptr and for an
 * invalid handle, because GetWindowThreadProcessId reports thread 0 in that case and no
 * running thread has id 0.
 * @note Windows posts synthesized input to the message queue of the thread that created
 * the destination window, and that thread must run its message loop before the keystroke
 * reaches the window procedure. This is therefore the exact condition under which a
 * caller that blocks without pumping messages can never observe its own injected key.
 * @warning Evaluate this on the thread that is about to block; never cache the result or
 * the thread id. A thread id is unique only until that thread exits, and the MTA worker
 * can be abandoned and recreated, so a stored id can later match an unrelated thread.
 */
bool IsWindowOnCallingThread(HWND hwnd);
