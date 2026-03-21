/**
 * Utility functions for text selection hook on Linux
 */

#include "utils.h"

#include <algorithm>
#include <cctype>

/**
 * Check if string is empty after trimming whitespace
 */
bool IsTrimmedEmpty(const std::string &text)
{
    return std::all_of(text.cbegin(), text.cend(), [](unsigned char c) { return std::isspace(c); });
}
