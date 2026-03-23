/**
 * Clipboard utility functions for text selection hook on macOS
 */

#import "clipboard.h"

/**
 * Reads text from clipboard
 * @param content [out] The string to store clipboard content
 * @return true if successful, false otherwise
 */
bool ReadClipboard(std::string &content)
{
    content.clear();

    @autoreleasepool
    {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSString *string = [pasteboard stringForType:NSPasteboardTypeString];

        if (string != nil)
        {
            content = std::string([string UTF8String]);
            return true;
        }

        return false;
    }
}

/**
 * Writes text to clipboard
 * @param content The string to write to clipboard
 * @return true if successful, false otherwise
 */
bool WriteClipboard(const std::string &content)
{
    if (content.empty())
        return false;

    @autoreleasepool
    {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        // have to clearContents first.
        [pasteboard clearContents];

        NSString *contentString = [NSString stringWithUTF8String:content.c_str()];
        BOOL success = [pasteboard setString:contentString forType:NSPasteboardTypeString];

        return success == YES;
    }
}

/**
 * Gets the current change count of clipboard
 * @return The change count of NSPasteboard
 *
 * Cost: ~2 us
 */
int64_t GetClipboardSequence()
{
    @autoreleasepool
    {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        return static_cast<int64_t>([pasteboard changeCount]);
    }
}

/**
 * Backs up all formats from the general pasteboard.
 * Reads all items and all type representations into memory before any
 * modification, since NSPasteboardItem objects become stale after clearContents.
 */
ClipboardBackup BackupClipboard()
{
    ClipboardBackup backup;

    @autoreleasepool
    {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        NSArray<NSPasteboardItem *> *pasteboardItems = [pasteboard pasteboardItems];

        if (pasteboardItems == nil || [pasteboardItems count] == 0)
            return backup;

        for (NSPasteboardItem *item in pasteboardItems)
        {
            ClipboardItemData itemData;
            NSArray<NSPasteboardType> *types = [item types];

            for (NSPasteboardType type in types)
            {
                NSData *data = [item dataForType:type];
                if (data == nil)
                    continue; // Unresolvable promised type, skip

                const uint8_t *bytes = static_cast<const uint8_t *>([data bytes]);
                NSUInteger length = [data length];

                std::string typeStr([type UTF8String]);
                std::vector<uint8_t> dataVec(bytes, bytes + length);
                itemData.representations.push_back({std::move(typeStr), std::move(dataVec)});
            }

            // Skip items where all dataForType: returned nil
            if (!itemData.representations.empty())
                backup.items.push_back(std::move(itemData));
        }

        backup.valid = !backup.items.empty();
    }

    return backup;
}

/**
 * Restores all formats to the general pasteboard from a backup.
 * Uses prepareForNewContentsWithOptions to avoid triggering Universal Clipboard sync,
 * then writes all backed-up items via writeObjects.
 */
bool RestoreClipboard(const ClipboardBackup &backup)
{
    if (!backup.HasData())
        return true; // Nothing to restore

    @autoreleasepool
    {
        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];

        // Use prepareForNewContentsWithOptions instead of clearContents
        // to avoid triggering Universal Clipboard / Handoff sync
        [pasteboard prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];

        NSMutableArray<NSPasteboardItem *> *items = [NSMutableArray array];

        for (const auto &itemData : backup.items)
        {
            NSPasteboardItem *item = [[[NSPasteboardItem alloc] init] autorelease];

            for (const auto &rep : itemData.representations)
            {
                NSPasteboardType type = [NSString stringWithUTF8String:rep.first.c_str()];
                NSData *data = [NSData dataWithBytes:rep.second.data() length:rep.second.size()];
                [item setData:data forType:type];
            }

            [items addObject:item];
        }

        return [pasteboard writeObjects:items] == YES;
    }
}
