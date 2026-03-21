#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

/**
 * UTF-8 conversion buffer pool to reduce memory allocations
 */
class StringPool
{
  public:
    /**
     * Convert wide string to UTF-8 using pooled buffers
     */
    static std::string WideToUtf8(const std::wstring &wstr);

    /**
     * Convert UTF-8 string to wide string using pooled buffers
     */
    static std::wstring Utf8ToWide(const std::string &utf8);

  private:
    /**
     * Get or create an appropriately sized buffer
     */
    static std::string &GetBuffer(size_t minSize);

    /**
     * Get or create an appropriately sized wide buffer
     */
    static std::wstring &GetWideBuffer(size_t minSize);
};