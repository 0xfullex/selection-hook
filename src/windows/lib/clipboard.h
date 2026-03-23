/**
 * Clipboard utility functions for text selection hook
 */
#pragma once

#include <windows.h>

#include <string>
#include <vector>

/**
 * A single clipboard format entry: format ID + raw data blob
 */
struct ClipboardFormatEntry
{
    UINT format;
    std::vector<BYTE> data;
    SIZE_T dataSize;
};

/**
 * Complete clipboard state snapshot for backup/restore
 */
struct ClipboardBackup
{
    std::vector<ClipboardFormatEntry> formats;
    bool isEmpty = true;

    bool HasData() const { return !isEmpty && !formats.empty(); }
};

/**
 * Reads text from clipboard
 * @param content [out] The string to store clipboard content
 * @param isClipboardOpened If true, assumes clipboard is already opened
 * @return true if successful, false otherwise
 */
bool ReadClipboard(std::wstring &content, bool isClipboardOpened = false);

/**
 * Writes text to clipboard
 * @param content The string to write to clipboard
 * @return true if successful, false otherwise
 */
bool WriteClipboard(const std::wstring &content);

/**
 * Backs up ALL clipboard formats into a ClipboardBackup structure
 * @param backup [out] The structure to store all clipboard formats
 * @param isClipboardOpened If true, assumes clipboard is already opened
 * @return true if successful (even if clipboard was empty), false on failure
 */
bool BackupClipboard(ClipboardBackup &backup, bool isClipboardOpened = false);

/**
 * Restores all clipboard formats from a ClipboardBackup structure
 * @param backup The backup to restore from
 * @return true if successful, false on failure
 */
bool RestoreClipboard(const ClipboardBackup &backup);
