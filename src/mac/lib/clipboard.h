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
 * @note Only NSPasteboardTypeString is read. A pasteboard holding just an image or a
 * file returns false, which the fallback treats as "copy failed".
 */
bool ReadClipboard(std::string &content);

/**
 * Writes text to clipboard
 * @param content The string to write to clipboard
 * @return true if successful, false otherwise
 * @note Rejects empty strings and replaces every existing pasteboard type (clearContents
 * first). Bumps changeCount, so callers tracking the sequence must resample afterwards.
 */
bool WriteClipboard(const std::string &content);

/**
 * Gets the current change count of clipboard
 * @return The change count of NSPasteboard
 * @note changeCount increments on every pasteboard write by any process, including this
 * library's own restore. The fallback compares it to the mouse-down baseline to detect a
 * copy made by the user; see SelectionEngine::GetTextViaClipboard.
 */
int64_t GetClipboardSequence();

/**
 * Backs up all formats from the general pasteboard
 * @return ClipboardBackup containing all items and their type data
 * @note Resolves every promised type via dataForType:, so a promise-backed item (e.g. a
 * large image from a design tool) is materialised in memory here.
 */
ClipboardBackup BackupClipboard();

/**
 * Restores all formats to the general pasteboard from a backup
 * @param backup The backup to restore
 * @return true if at least some data was restored, false on complete failure
 * @warning Writes with NSPasteboardContentsCurrentHostOnly so the restore is not pushed
 * to Universal Clipboard / Handoff. This is deliberate: a synced restore would replace
 * whatever the user copied on their other devices. Bumps changeCount.
 */
bool RestoreClipboard(const ClipboardBackup &backup);
