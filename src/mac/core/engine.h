/**
 * @file
 * @brief macOS text-selection retrieval pipeline declarations.
 *
 * This layer owns no N-API state. It receives a borrowed NSRunningApplication and a
 * value-only configuration snapshot from selection_hook.mm and never reads the addon
 * instance, so the retrieval logic can be read and changed without the hook lifecycle.
 */
#pragma once

#import <ApplicationServices/ApplicationServices.h>
#import <Cocoa/Cocoa.h>

#import <cstdint>
#import <string>
#import <vector>

#import "types.h"

/**
 * @brief Implementation of macOS text selection retrieval: AXAPI, then clipboard.
 *
 * @note SelectionEngine stores per-call context in members and is not reentrant.
 * selection_hook.mm serializes calls through its is_processing gate; keep one call at
 * a time per instance.
 * @note The engine runs on the JavaScript thread today. GetFrontApp() in lib/utils.mm
 * pumps the caller's NSRunLoop, which is why the caller, not the engine, resolves
 * frontApp. Moving retrieval to another thread must revisit that call first.
 * @warning frontApp is borrowed and never retained (the addon is compiled without ARC).
 * Do not store it in members that outlive GetSelectedText().
 */
class SelectionEngine
{
  public:
    SelectionEngine() = default;

    /**
     * @brief Run the macOS retrieval pipeline.
     * @param[in] frontApp Borrowed frontmost application resolved by the caller.
     * @param[out] selectionInfo Cleared when retrieval begins and populated on success.
     * @param[in] config Immutable configuration snapshot for this call.
     * @param[out] endingClipboardSequence Optional retrieval-local NSPasteboard change
     * count. Written on every return; the caller stores it as the baseline that the next
     * mouse gesture compares against.
     * @return true only when a non-empty text selection was retrieved.
     * @note The order is program-name filter -> AXAPI -> clipboard fallback. isFullscreen
     * is resolved only after a successful retrieval.
     * @warning Preserve the order and the same-process clipboard rejection. Changing
     * either changes supported applications and requires rerunning the macOS
     * application matrix (native, Chromium, Electron) and clipboard restoration tests.
     */
    bool GetSelectedText(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo,
                         const SelectionConfigSnapshot &config, int64_t *endingClipboardSequence = nullptr);

  private:
    /**
     * @brief Retrieve from the focused element, its children, then up to 10 ancestors.
     * @note Falls back to the focused window when the app reports no focused element.
     * @warning The ancestor walk exists for Chromium selections that span multiple nodes
     * (issue #18). When every strategy fails it enables AXEnhancedUserInterface (Chrome)
     * and AXManualAccessibility (Electron) on the application so the next retrieval can
     * succeed; this happens on every failed call. docs/GUIDE.md documents the side
     * effect, so keep the docs in sync when changing it.
     */
    bool GetTextViaAXAPI(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo);

    /**
     * @brief Backup / Cmd+C / poll / read / restore fallback.
     * @note Refuses to run against config_->runningPid: the JS thread is blocked in the
     * poll and could not handle its own key event.
     * @warning This is a complete backup/copy/read/restore transaction. Every exit after
     * the copy key is sent must restore the backup and refresh clipboardSequence_ (the
     * restore bumps changeCount). The poll blocks the JS thread for up to ~100 ms.
     */
    bool GetTextViaClipboard(NSRunningApplication *frontApp, TextSelectionInfo &selectionInfo);
    bool ShouldProcessViaClipboard(const std::string &programName) const;

    /** Read kAXSelectedText, falling back to kAXValue sliced by kAXSelectedTextRange. */
    bool GetSelectedTextFromElement(AXUIElementRef element, std::string &text);

    /**
     * @warning The size and origin sanity thresholds are compatibility filters for apps
     * that report bogus bounds. Revalidate against native, Chromium, and Electron apps
     * before changing them.
     */
    bool SetTextRangeCoordinates(AXUIElementRef element, TextSelectionInfo &selectionInfo);

    bool IsInFilterList(const std::string &programName, const std::vector<std::string> &filterList) const;

    /**
     * @brief Post Cmd+C with Core Graphics events.
     * @param[in] pid Target process; 0 posts to the HID event tap (frontmost app) instead.
     * @note Requires accessibility permission to reach another process.
     */
    bool SendCopyKey(pid_t pid = 0) const;

    const SelectionConfigSnapshot *config_ = nullptr;
    int64_t clipboardSequence_ = 0;
};
