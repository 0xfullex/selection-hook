/**
 * String pool for text selection hook
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */

#include "string_pool.h"

#include <windows.h>

#include <algorithm>

// Converts wide string to UTF-8 using buffer pooling
std::string StringPool::WideToUtf8(const std::wstring &wstr)
{
    if (wstr.empty())
        return "";

    int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), NULL, 0, NULL, NULL);
    if (utf8Size <= 0)
        return "";

    // Add space for null terminator
    std::string &buffer = GetBuffer(utf8Size + 1);
    buffer[utf8Size] = '\0';

    int result = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.length()), &buffer[0], utf8Size,
                                     NULL, NULL);
    if (result <= 0)
        return "";

    return std::string(buffer.c_str(), utf8Size);
}

// Converts UTF-8 string to wide string using buffer pooling
std::wstring StringPool::Utf8ToWide(const std::string &utf8)
{
    if (utf8.empty())
        return L"";

    int wideSize = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.length()), NULL, 0);
    if (wideSize <= 0)
        return L"";

    // Add space for null terminator
    std::wstring &buffer = GetWideBuffer(wideSize + 1);
    buffer[wideSize] = L'\0';

    int result = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.length()), &buffer[0], wideSize);
    if (result <= 0)
        return L"";

    return std::wstring(buffer.c_str(), wideSize);
}

// Provides a reusable string buffer of sufficient size from the pool
std::string &StringPool::GetBuffer(size_t minSize)
{
    static std::unordered_map<size_t, std::string> pool;
    static std::mutex pool_mutex;

    std::lock_guard<std::mutex> lock(pool_mutex);

    // Find best fit buffer to reduce memory waste
    size_t bestSize = SIZE_MAX;

    for (const auto &pair : pool)
    {
        if (pair.first >= minSize && pair.first < bestSize)
        {
            bestSize = pair.first;
            if (bestSize == minSize)
                break;  // Perfect match
        }
    }

    if (bestSize != SIZE_MAX)
    {
        auto &buffer = pool[bestSize];
        buffer.resize(bestSize);
        return buffer;
    }

    // Create new buffer with power-of-2 sizing (but not smaller than minSize)
    size_t size = 1;
    while (size < minSize) size *= 2;

    pool[size] = std::string(size, '\0');
    return pool[size];
}

// Provides a reusable wide string buffer of sufficient size from the pool
std::wstring &StringPool::GetWideBuffer(size_t minSize)
{
    static std::unordered_map<size_t, std::wstring> pool;
    static std::mutex pool_mutex;

    std::lock_guard<std::mutex> lock(pool_mutex);

    // Find best fit buffer to reduce memory waste
    size_t bestSize = SIZE_MAX;

    for (const auto &pair : pool)
    {
        if (pair.first >= minSize && pair.first < bestSize)
        {
            bestSize = pair.first;
            if (bestSize == minSize)
                break;  // Perfect match
        }
    }

    if (bestSize != SIZE_MAX)
    {
        auto &buffer = pool[bestSize];
        buffer.resize(bestSize);
        return buffer;
    }

    // Create new buffer with power-of-2 sizing (but not smaller than minSize)
    size_t size = 1;
    while (size < minSize) size *= 2;

    pool[size] = std::wstring(size, L'\0');
    return pool[size];
}