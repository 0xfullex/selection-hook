/**
 * Clipboard utility functions for text selection hook
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */

#include "clipboard.h"
#include <vector>

/**
 * Reads text from clipboard
 * @param content [out] The string to store clipboard content
 * @param isClipboardOpened If true, assumes clipboard is already opened
 * @return true if successful, false otherwise
 */
bool ReadClipboard(std::wstring &content, bool isClipboardOpened)
{
    if (!isClipboardOpened && !OpenClipboard(nullptr))
        return false;

    bool success = false;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData)
    {
        // Handle Unicode text (most common)
        wchar_t *pText = static_cast<wchar_t *>(GlobalLock(hData));
        if (pText)
        {
            content = pText;
            GlobalUnlock(hData);
            success = true;
        }
    }
    else
    {
        // Fallback to CF_TEXT if CF_UNICODETEXT is not available
        hData = GetClipboardData(CF_TEXT);
        if (hData)
        {
            char *pText = static_cast<char *>(GlobalLock(hData));
            if (pText)
            {
                // Convert ANSI text to wide string
                int length = MultiByteToWideChar(CP_ACP, 0, pText, -1, nullptr, 0);
                if (length > 0)
                {
                    std::vector<wchar_t> buffer(length);
                    MultiByteToWideChar(CP_ACP, 0, pText, -1, buffer.data(), length);
                    content = buffer.data();
                    success = true;
                }
                GlobalUnlock(hData);
            }
        }
    }

    if (!isClipboardOpened)
        CloseClipboard();

    return success;
}

/**
 * Writes text to clipboard
 * @param content The string to write to clipboard
 * @return true if successful, false otherwise
 */
bool WriteClipboard(const std::wstring &content)
{
    if (!OpenClipboard(nullptr))
        return false;

    EmptyClipboard();
    bool success = false;

    if (!content.empty())
    {
        size_t size = (content.size() + 1) * sizeof(wchar_t);
        HANDLE hData = GlobalAlloc(GMEM_MOVEABLE, size);
        if (hData)
        {
            wchar_t *pText = static_cast<wchar_t *>(GlobalLock(hData));
            if (pText)
            {
                memcpy(pText, content.c_str(), size);
                GlobalUnlock(hData);
                if (SetClipboardData(CF_UNICODETEXT, hData) != nullptr)
                {
                    success = true;
                }
                else
                {
                    GlobalFree(hData);
                }
            }
            else
            {
                GlobalFree(hData);
            }
        }
    }

    CloseClipboard();
    return success;
}