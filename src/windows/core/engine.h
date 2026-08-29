/**
 * @file
 * @brief Windows text-selection retrieval pipeline declarations.
 *
 * This layer owns no N-API state. A SelectionEngine is thread-affine because its
 * IUIAutomation client must be created, used, and released in the same COM apartment.
 */
#pragma once

// UIAutomation.h assumes COM's interface macro has already been declared.
// clang-format off
#include <ole2.h>
#include <UIAutomation.h>
#include <oleacc.h>
#include <windows.h>
// clang-format on

#include <atomic>
#include <string>

#include "types.h"

// The JavaScript/main-thread paths must never wait behind an automatic clipboard
// fallback. Only the worker uses an interruptible wait for exclusive clipboard access.
enum class ClipboardLockMode
{
    TryOnly,
    InterruptibleWait
};

// Coarse stage used for stop-time diagnostics when a provider does not return.
enum class RetrievalStage
{
    Idle,
    ProgramName,
    UIAutomation,
    Accessible,
    Clipboard,
    ClipboardPoll,
    Complete
};

/**
 * @brief Thread-affine implementation of Windows text selection retrieval.
 *
 * @note The automation pointer is borrowed. The caller must keep it alive and call every
 * engine method on the COM-apartment thread that created the UIA client.
 * @note SelectionEngine stores per-call context in members and is not reentrant. Use one
 * instance only from its owner thread and execute retrievals serially on that instance.
 * @warning Do not move UIA/MSAA interface pointers into request or result types. Create,
 * use, and release every raw COM interface on its owning thread.
 */
class SelectionEngine
{
  public:
    /**
     * @param[in] automation Borrowed UIA client for this thread. May be null to skip UIA.
     */
    explicit SelectionEngine(IUIAutomation *automation);

    /**
     * @brief Run the Windows compatibility retrieval pipeline.
     * @param[in] hwnd Borrowed target window handle; it must be valid for this call.
     * @param[out] selectionInfo Cleared when retrieval begins and populated on success.
     * @param[in] config Immutable configuration snapshot for this call.
     * @param[in] clipboardLockMode Fail-fast main-thread mode or stop-aware worker wait.
     * @param[in] stopToken Optional worker stop flag; null for the synchronous API.
     * @param[out] stage Optional coarse stage for stop-time diagnostics.
     * @param[out] endingClipboardSequence Optional retrieval-local clipboard sequence.
     * @param[out] endingControlType Optional focused UIA control type for cursor fallback.
     * @return true only when a non-empty text selection was retrieved.
     * @note The order is UIA -> MSAA/IAccessible -> clipboard fallback.
     * @warning Preserve the order, filtering, and NonTextSelection short-circuit. Changing
     * any of them changes supported applications and requires the Windows compatibility
     * matrix, mouse-selection states, and clipboard restoration tests to be rerun.
     */
    bool GetSelectedText(HWND hwnd, TextSelectionInfo &selectionInfo, const SelectionConfigSnapshot &config,
                         ClipboardLockMode clipboardLockMode, const std::atomic<bool> *stopToken,
                         std::atomic<RetrievalStage> *stage = nullptr, DWORD *endingClipboardSequence = nullptr,
                         CONTROLTYPEID *endingControlType = nullptr);

    /**
     * @brief Try to read text without waiting for an active fallback transaction.
     * @param[out] content Clipboard text when successful.
     * @return false when the process clipboard lock or Windows clipboard is unavailable.
     */
    static bool TryReadClipboard(std::wstring &content);

    /**
     * @brief Try to write text without waiting for an active fallback transaction.
     * @param[in] content Text to write.
     * @return false when the process clipboard lock or Windows clipboard is unavailable.
     */
    static bool TryWriteClipboard(const std::wstring &content);

  private:
    /** Retrieve from the focused UIA element, legacy bridge, then eligible ancestors. */
    TextRetrievalResult GetTextViaUIAutomation(HWND hwnd, TextSelectionInfo &selectionInfo);

    /**
     * @warning DocumentRange fallback is safe only for the focused element. Ancestors
     * must pass false or a provider may return an entire document as selected text.
     */
    bool TryGetTextViaTextPattern(IUIAutomationElement *element, TextSelectionInfo &selectionInfo,
                                  bool allowDocumentFallback);

    /** Retrieve from the legacy window client object. */
    TextRetrievalResult GetTextViaAccessible(HWND hwnd, TextSelectionInfo &selectionInfo);
    TextRetrievalResult ProcessAccessibleSelection(IAccessible *accessible, TextSelectionInfo &selectionInfo);
    TextRetrievalResult ClassifyAccessibleRole(IAccessible *accessible, const VARIANT &child);

    /**
     * @warning This is a complete backup/copy/read/restore transaction. Every exit after
     * a non-empty backup must restore it and update clipboardSequence_.
     */
    bool GetTextViaClipboard(HWND hwnd, TextSelectionInfo &selectionInfo);
    bool ShouldProcessViaClipboard(const std::wstring &programName) const;

    /**
     * @warning The width/height filters are compatibility thresholds. Revalidate UIA
     * coordinates across browsers, Office, PDF readers, and legacy apps before changing.
     */
    bool SetTextRangeCoordinates(IUIAutomationTextRange *range, TextSelectionInfo &selectionInfo);
    bool IsInFilterList(const std::wstring &programName, const std::vector<std::string> &filterList) const;
    bool ShouldKeyInterruptViaClipboard() const;
    bool SleepInterruptibly(DWORD milliseconds) const;
    bool IsStopping() const;

    /**
     * @warning The injected modifier sequence is compatibility behavior. Changes require
     * physical Alt/Shift/Ctrl state and copy-shortcut regression testing.
     */
    void SendCopyKey(CopyKeyType type) const;
    void SetStage(RetrievalStage stage) const;

    IUIAutomation *automation_ = nullptr;
    const SelectionConfigSnapshot *config_ = nullptr;
    const std::atomic<bool> *stopToken_ = nullptr;
    std::atomic<RetrievalStage> *stage_ = nullptr;
    ClipboardLockMode clipboardLockMode_ = ClipboardLockMode::TryOnly;
    DWORD clipboardSequence_ = 0;
    CONTROLTYPEID controlType_ = UIA_WindowControlTypeId;
#ifdef _DEBUG
    DWORD ownerThreadId_ = 0;
#endif
};
