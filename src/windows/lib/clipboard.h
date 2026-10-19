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

/** Detailed outcome of restoring the backupable formats, including an empty original. */
enum class ClipboardRestoreStatus
{
    Restored,
    Partial,
    OpenFailed,
    EmptyFailed,
    CloseFailed
};

/** Counts every backed-up format attempted; Windows owns only successfully set handles. */
struct ClipboardRestoreResult
{
    ClipboardRestoreStatus status = ClipboardRestoreStatus::Restored;
    size_t restoredFormats = 0;
    size_t failedFormats = 0;
};

/**
 * @brief Restore every backupable format, continuing after individual failures.
 * @param[in] backup Snapshot; an originally empty clipboard is restored to empty.
 * @param[in] isClipboardOpened True when the caller already holds OpenClipboard.
 * @param[out] result Optional detailed result. Partial means at least one format failed,
 * or the nonempty original had no backupable formats.
 * @return true only if every backed-up format was restored and all owned API steps succeeded.
 * @note No retry is hidden here. The ordinary engine transaction opens with bounded retries;
 * the legacy caller retains its original single OpenClipboard attempt and HasData guard.
 */
bool RestoreClipboard(const ClipboardBackup &backup, bool isClipboardOpened = false,
                      ClipboardRestoreResult *result = nullptr);
