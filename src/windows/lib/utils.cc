/**
 * Utility functions for text selection hook
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */

#include "utils.h"

#include <psapi.h>

/**
 * Check if string is empty after trimming whitespace
 */
bool IsTrimmedEmpty(const std::wstring &text)
{
    if (text.empty())
    {
        return true;
    }

    std::wstring trimmedText = text;
    trimmedText.erase(0, trimmedText.find_first_not_of(L" \t\n\r"));
    trimmedText.erase(trimmedText.find_last_not_of(L" \t\n\r") + 1);

    return trimmedText.empty();
}

/**
 * Get window under mouse point, including floating tool windows
 */
HWND GetWindowUnderMouse()
{
    POINT mousePos;
    if (!GetCursorPos(&mousePos))
    {
        return nullptr;
    }

    HWND hwnd = WindowFromPoint(mousePos);
    if (hwnd)
    {
        return hwnd;
    }

    return GetForegroundWindow();
}

/**
 * Check if window has moved since mouse down
 */
bool HasWindowMoved(const RECT &currentWindowRect, const RECT &lastWindowRect)
{
    // Compare the two rects directly with 2 pixel tolerance
    return (abs(currentWindowRect.left - lastWindowRect.left) > 2 ||
            abs(currentWindowRect.top - lastWindowRect.top) > 2 ||
            abs(currentWindowRect.right - lastWindowRect.right) > 2 ||
            abs(currentWindowRect.bottom - lastWindowRect.bottom) > 2);
}

/**
 * Get program name (executable path) from a window handle
 */
bool GetProgramNameFromHwnd(HWND hwnd, std::wstring &programName)
{
    if (!hwnd)
        return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);

    if (processId == 0)
        return false;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!hProcess)
        return false;

    wchar_t path[MAX_PATH] = {0};
    DWORD size = MAX_PATH;

    BOOL result = QueryFullProcessImageNameW(hProcess, 0, path, &size);
    CloseHandle(hProcess);

    if (!result || size == 0)
        return false;

    // Extract filename from path
    wchar_t *fileName = wcsrchr(path, L'\\');
    if (fileName)
        programName = fileName + 1;
    else
        programName = path;

    return true;
}