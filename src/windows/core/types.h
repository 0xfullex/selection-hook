/**
 * @file
 * @brief Value types shared by the Windows selection hook, engine, and MTA worker.
 *
 * Keep this file free of N-API objects and owned COM interface pointers. Requests cross
 * threads by value; COM objects remain on the thread that created them.
 */
#pragma once

#define WIN32_LEAN_AND_MEAN 1

// UIAutomation.h assumes COM's interface macro has already been declared.
// clang-format off
#include <ole2.h>
#include <UIAutomation.h>
#include <windows.h>
// clang-format on

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// How the mouse gesture identified a possible text selection.
enum class SelectionDetectType
{
    None = 0,
    Drag = 1,
    DoubleClick = 2,
    ShiftClick = 3
};

/**
 * Retrieval method reported through the existing JavaScript API.
 * @warning Numeric values are public API. Do not renumber or reuse deprecated entries.
 */
enum class SelectionMethod
{
    None = 0,
    UIA = 1,
    /** @deprecated Kept only to preserve the existing numeric API contract. */
    FocusControl = 2,
    Accessible = 3,
    Clipboard = 99
};

// NonTextSelection is distinct from Unavailable: selecting an object must stop the
// pipeline before clipboard fallback, or a simulated copy could copy the object.
enum class TextRetrievalResult
{
    TextFound,
    Unavailable,
    NonTextSelection
};

/**
 * Amount of screen-position information available for the selection.
 * @warning Numeric values are exposed to JavaScript and must remain stable.
 */
enum class SelectionPositionLevel
{
    None = 0,         // No position information is available.
    MouseSingle = 1,  // Only the current mouse position is known.
    MouseDual = 2,    // Mouse selection start and end positions are known.
    Full = 3,         // First and last text-line edge coordinates are known.
    Detailed = 4      // All detailed selection corner points are known.
};

/** @warning Numeric values are accepted from JavaScript configuration and must remain stable. */
enum class FilterMode
{
    Default = 0,      // Do not filter by program name.
    IncludeList = 1,  // Process only matching programs.
    ExcludeList = 2   // Process only non-matching programs.
};

/**
 * Compatibility lists for applications whose behavior differs from Windows conventions.
 * @warning Numeric values are accepted from JavaScript configuration and must remain stable.
 */
enum class FineTunedListType
{
    ExcludeClipboardCursorDetect = 0,  // Allow listed apps to bypass cursor rejection.
    IncludeClipboardDelayRead = 1      // Wait for listed apps to finish clipboard updates.
};

// Copy shortcut used by the clipboard fallback.
enum class CopyKeyType
{
    CtrlInsert = 0,
    CtrlC = 1
};

// Native selection result. All POINT values use physical screen coordinates.
struct TextSelectionInfo
{
    std::wstring text;         ///< Selected text content.
    std::wstring programName;  ///< Program that owns the selection window.

    POINT startTop;       ///< First text line, left-top.
    POINT startBottom;    ///< First text line, left-bottom.
    POINT endTop;         ///< Last text line, right-top.
    POINT endBottom;      ///< Last text line, right-bottom.
    POINT mousePosStart;  ///< Mouse position where selection started.
    POINT mousePosEnd;    ///< Mouse position where selection ended.

    SelectionMethod method;
    SelectionPositionLevel posLevel;

    TextSelectionInfo() : method(SelectionMethod::None), posLevel(SelectionPositionLevel::None)
    {
        startTop.x = startTop.y = 0;
        startBottom.x = startBottom.y = 0;
        endTop.x = endTop.y = 0;
        endBottom.x = endBottom.y = 0;
        mousePosStart.x = mousePosStart.y = 0;
        mousePosEnd.x = mousePosEnd.y = 0;
    }

    void clear()
    {
        text.clear();
        programName.clear();
        startTop.x = startTop.y = 0;
        startBottom.x = startBottom.y = 0;
        endTop.x = endTop.y = 0;
        endBottom.x = endBottom.y = 0;
        mousePosStart.x = mousePosStart.y = 0;
        mousePosEnd.x = mousePosEnd.y = 0;
        method = SelectionMethod::None;
        posLevel = SelectionPositionLevel::None;
    }
};

/**
 * @brief Immutable inputs captured for one retrieval.
 *
 * The main thread copies configuration into each automatic request so the worker never
 * reads vectors being modified by JS. HCURSOR values are comparison-only handles and
 * are never owned or destroyed here.
 */
struct SelectionConfigSnapshot
{
    bool clipboardEnabled = true;
    bool triggeredByUser = false;
    FilterMode clipboardFilterMode = FilterMode::Default;
    std::vector<std::string> clipboardFilterList;
    FilterMode globalFilterMode = FilterMode::Default;
    std::vector<std::string> globalFilterList;
    std::vector<std::string> excludeClipboardCursorDetect;
    std::vector<std::string> includeClipboardDelayRead;
    HCURSOR mouseDownCursor = NULL;
    HCURSOR mouseUpCursor = NULL;
    DWORD clipboardSequence = 0;
};

/** Clipboard value and revision captured atomically under ClipboardSequenceState's lock. */
struct ClipboardSequenceSnapshot
{
    DWORD value = 0;
    uint64_t revision = 0;
};

/**
 * @brief Versioned clipboard sequence shared by automatic and synchronous retrieval.
 *
 * @warning Worker writeback must use PublishIfUnchanged. Replacing it with Store can
 * overwrite a newer mouse-down or synchronous value and make a later gesture consume
 * clipboard content produced by the library itself.
 */
class ClipboardSequenceState
{
  public:
    /** @return Value and revision from one locked observation. */
    ClipboardSequenceSnapshot Load() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return {value_, revision_};
    }

    /** Record a newer main-thread observation and advance its revision. */
    void Store(DWORD value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = value;
        ++revision_;
    }

    /**
     * Publish worker-local state only when no newer main-thread observation exists.
     * @note This intentionally does not advance revision: it completes the operation
     * that began at expectedRevision rather than creating a newer observation.
     */
    void PublishIfUnchanged(DWORD value, uint64_t expectedRevision)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A late automatic retrieval must not overwrite a newer observation made by
        // the main thread while that retrieval was running.
        if (revision_ == expectedRevision)
            value_ = value;
    }

  private:
    mutable std::mutex mutex_;
    DWORD value_ = 0;
    uint64_t revision_ = 0;
};

/**
 * @brief Value-only work item passed to the MTA thread.
 *
 * retrievalHwnd is borrowed and must be validated with IsWindow immediately before use.
 * This type must not acquire ownership of COM interfaces or N-API values.
 */
struct SelectionRequest
{
    uint64_t sequence = 0;
    uint64_t generation = 0;
    HWND retrievalHwnd = NULL;
    SelectionDetectType detectionType = SelectionDetectType::None;
    POINT mouseDownPos = {0, 0};
    POINT mouseUpPos = {0, 0};
    POINT previousMouseUpPos = {0, 0};
    std::shared_ptr<ClipboardSequenceState> clipboardSequenceState;
    uint64_t clipboardSequenceRevision = 0;
    SelectionConfigSnapshot config;
};

/**
 * @brief Value-only result returned through the TSFN.
 *
 * It intentionally contains no COM or N-API object, so normal and abandoned worker
 * cleanup cannot touch JS-owned state.
 */
struct SelectionWorkerResult
{
    uint64_t sequence = 0;
    uint64_t generation = 0;
    SelectionDetectType detectionType = SelectionDetectType::None;
    bool found = false;
    TextSelectionInfo selection;
};

/**
 * @brief Independent automatic and synchronous retrieval gates.
 *
 * IsAnyRetrievalInFlight preserves historical input-event suppression while either path
 * is active.
 *
 * @warning Do not collapse these into one mutually exclusive gate. Doing so would make
 * synchronous getCurrentSelection() return null merely because automatic UIA/MSAA is
 * running, which would reduce the existing public API capability.
 */
struct RetrievalGates
{
    std::atomic<bool> autoRetrievalInFlight{false};
    std::atomic<bool> syncRetrievalInFlight{false};

    bool IsAnyRetrievalInFlight() const
    {
        return autoRetrievalInFlight.load(std::memory_order_acquire) ||
               syncRetrievalInFlight.load(std::memory_order_acquire);
    }
};
