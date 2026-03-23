/**
 * Clipboard utility functions for text selection hook on macOS
 */

#import <Cocoa/Cocoa.h>

#import <cstdint>
#import <string>
#import <utility>
#import <vector>

/**
 * A single pasteboard item with all its type representations
 */
struct ClipboardItemData
{
    std::vector<std::pair<std::string, std::vector<uint8_t>>> representations;
};

/**
 * Complete pasteboard backup (possibly multiple items)
 */
struct ClipboardBackup
{
    std::vector<ClipboardItemData> items;
    bool valid = false;

    bool HasData() const { return valid && !items.empty(); }
};

/**
 * Reads text from clipboard
 * @param content [out] The string to store clipboard content
 * @return true if successful, false otherwise
 */
bool ReadClipboard(std::string &content);

/**
 * Writes text to clipboard
 * @param content The string to write to clipboard
 * @return true if successful, false otherwise
 */
bool WriteClipboard(const std::string &content);

/**
 * Gets the current change count of clipboard
 * @return The change count of NSPasteboard
 */
int64_t GetClipboardSequence();

/**
 * Backs up all formats from the general pasteboard
 * @return ClipboardBackup containing all items and their type data
 */
ClipboardBackup BackupClipboard();

/**
 * Restores all formats to the general pasteboard from a backup
 * @param backup The backup to restore
 * @return true if at least some data was restored, false on complete failure
 */
bool RestoreClipboard(const ClipboardBackup &backup);
