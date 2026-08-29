/**
 * @file
 * @brief Windows UIA, MSAA, and clipboard text-retrieval implementation.
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */
#define WIN32_LEAN_AND_MEAN 1

#include "engine.h"

#include <oleacc.h>
#include <windows.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <mutex>
#include <vector>

#include "../lib/clipboard.h"
#include "../lib/string_pool.h"
#include "../lib/utils.h"

#pragma comment(lib, "Oleacc.lib")  // For IAccessible.

// UI Automation constant missing from some older Windows SDKs.
#ifndef UIA_IsSelectionActivePropertyId
#define UIA_IsSelectionActivePropertyId 30034
#endif

namespace
{
// Clipboard fallback is a process-wide transaction: backup, clear, synthesize copy,
// read, and restore must not interleave with another retrieval or public clipboard API.
std::mutex g_clipboard_mutex;

/**
 * Remove the object replacement character (U+FFFC) from extracted text.
 *
 * UIA TextPattern providers use this marker for embedded objects while native copy
 * normally omits it. Chromium also wraps the marker in line breaks. Remove at most one
 * adjacent line break on each side so inline icons do not leave blank lines, while
 * unrelated line breaks remain untouched.
 */
void RemoveObjectReplacementChars(std::wstring &text)
{
    if (text.empty())
        return;

    constexpr wchar_t objectReplacementChar = 0xFFFC;
    if (text.find(objectReplacementChar) == std::wstring::npos)
        return;

    std::wstring cleaned;
    cleaned.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] != objectReplacementChar)
        {
            cleaned += text[i];
            continue;
        }

        // Drop one CRLF, LF, or CR immediately before the marker.
        if (!cleaned.empty() && cleaned.back() == L'\n')
        {
            cleaned.pop_back();
            if (!cleaned.empty() && cleaned.back() == L'\r')
                cleaned.pop_back();
        }
        else if (!cleaned.empty() && cleaned.back() == L'\r')
        {
            cleaned.pop_back();
        }

        // Skip one CRLF, LF, or CR immediately after the marker.
        if (i + 1 < text.size() && text[i + 1] == L'\r')
        {
            i++;
            if (i + 1 < text.size() && text[i + 1] == L'\n')
                i++;
        }
        else if (i + 1 < text.size() && text[i + 1] == L'\n')
        {
            i++;
        }
    }

    text.swap(cleaned);
}
}  // namespace

SelectionEngine::SelectionEngine(IUIAutomation *automation) : automation_(automation)
{
#ifdef _DEBUG
    ownerThreadId_ = GetCurrentThreadId();
#endif
}

bool SelectionEngine::TryReadClipboard(std::wstring &content)
{
    // Main-thread callers preserve the old immediate-failure behavior during an
    // automatic clipboard fallback; they must not block the JS event loop.
    std::unique_lock<std::mutex> lock(g_clipboard_mutex, std::try_to_lock);
    return lock.owns_lock() && ReadClipboard(content);
}

bool SelectionEngine::TryWriteClipboard(const std::wstring &content)
{
    std::unique_lock<std::mutex> lock(g_clipboard_mutex, std::try_to_lock);
    return lock.owns_lock() && WriteClipboard(content);
}

/** Run the full text retrieval pipeline without changing its compatibility order. */
bool SelectionEngine::GetSelectedText(HWND hwnd, TextSelectionInfo &selectionInfo,
                                      const SelectionConfigSnapshot &config, ClipboardLockMode clipboardLockMode,
                                      const std::atomic<bool> *stopToken, std::atomic<RetrievalStage> *stage,
                                      DWORD *endingClipboardSequence, CONTROLTYPEID *endingControlType)
{
#ifdef _DEBUG
    assert(ownerThreadId_ == GetCurrentThreadId());
#endif
    config_ = &config;
    stopToken_ = stopToken;
    stage_ = stage;
    clipboardLockMode_ = clipboardLockMode;
    clipboardSequence_ = config.clipboardSequence;
    controlType_ = UIA_WindowControlTypeId;
    if (!hwnd || IsStopping())
        return false;
    selectionInfo.clear();

    // Program-name filtering happens before every retrieval method, as it did in the
    // original single-file implementation.
    SetStage(RetrievalStage::ProgramName);
    if (!GetProgramNameFromHwnd(hwnd, selectionInfo.programName))
    {
        selectionInfo.programName.clear();
        if (config.globalFilterMode == FilterMode::IncludeList)
            return false;
    }
    else if (config.globalFilterMode != FilterMode::Default)
    {
        const bool isIn = IsInFilterList(selectionInfo.programName, config.globalFilterList);
        if ((config.globalFilterMode == FilterMode::IncludeList && !isIn) ||
            (config.globalFilterMode == FilterMode::ExcludeList && isIn))
            return false;
    }

    bool hasNonTextSelection = false;
    // Prefer UIA for modern applications.
    if (!IsStopping() && automation_)
    {
        SetStage(RetrievalStage::UIAutomation);
        const TextRetrievalResult uiaResult = GetTextViaUIAutomation(hwnd, selectionInfo);
        if (uiaResult == TextRetrievalResult::TextFound)
        {
            selectionInfo.method = SelectionMethod::UIA;
            SetStage(RetrievalStage::Complete);
            if (endingClipboardSequence)
                *endingClipboardSequence = clipboardSequence_;
            if (endingControlType)
                *endingControlType = controlType_;
            return true;
        }
        hasNonTextSelection = uiaResult == TextRetrievalResult::NonTextSelection;
    }

    // Fall back to MSAA/IAccessible for legacy applications and compatibility providers.
    if (!IsStopping())
    {
        SetStage(RetrievalStage::Accessible);
        const TextRetrievalResult accessibleResult = GetTextViaAccessible(hwnd, selectionInfo);
        if (accessibleResult == TextRetrievalResult::TextFound)
        {
            selectionInfo.method = SelectionMethod::Accessible;
            SetStage(RetrievalStage::Complete);
            if (endingClipboardSequence)
                *endingClipboardSequence = clipboardSequence_;
            if (endingControlType)
                *endingControlType = controlType_;
            return true;
        }
        if (accessibleResult == TextRetrievalResult::NonTextSelection)
            hasNonTextSelection = true;
    }

    // Never synthesize a copy shortcut for an object selection. Apart from changing the
    // clipboard, copying selected UI objects can invoke application-specific actions.
    if (hasNonTextSelection || IsStopping())
        return false;

    // Clipboard copy is the last resort and remains subject to its original filters.
    SetStage(RetrievalStage::Clipboard);
    if (ShouldProcessViaClipboard(selectionInfo.programName) && GetTextViaClipboard(hwnd, selectionInfo))
    {
        selectionInfo.method = SelectionMethod::Clipboard;
        SetStage(RetrievalStage::Complete);
        if (endingClipboardSequence)
            *endingClipboardSequence = clipboardSequence_;
        if (endingControlType)
            *endingControlType = controlType_;
        return true;
    }

    SetStage(RetrievalStage::Complete);
    if (endingClipboardSequence)
        *endingClipboardSequence = clipboardSequence_;
    if (endingControlType)
        *endingControlType = controlType_;
    return false;
}

/** Case-insensitive substring match against the normalized UTF-8 program list. */
bool SelectionEngine::IsInFilterList(const std::wstring &programName, const std::vector<std::string> &filterList) const
{
    if (filterList.empty())
        return false;

    // Lists are stored as lowercase UTF-8 by the JS configuration path.
    std::wstring lowerProgramName = programName;
    std::transform(lowerProgramName.begin(), lowerProgramName.end(), lowerProgramName.begin(), towlower);
    const std::string utf8ProgramName = StringPool::WideToUtf8(lowerProgramName);
    for (const auto &filterItem : filterList)
    {
        if (utf8ProgramName.find(filterItem) != std::string::npos)
            return true;
    }
    return false;
}

/** Apply clipboard enablement, program filters, cursor heuristics, and UIA exceptions. */
bool SelectionEngine::ShouldProcessViaClipboard(const std::wstring &programName) const
{
    if (!config_ || !config_->clipboardEnabled)
        return false;

    bool result = false;
    switch (config_->clipboardFilterMode)
    {
        case FilterMode::Default:
            result = true;
            break;
        case FilterMode::IncludeList:
            result = IsInFilterList(programName, config_->clipboardFilterList);
            break;
        case FilterMode::ExcludeList:
            result = !IsInFilterList(programName, config_->clipboardFilterList);
            break;
    }
    if (!result)
        return false;

    // A direct getCurrentSelection() call has no reliable mouse cursor context, so the
    // cursor heuristic applies only to automatic mouse-driven retrieval.
    if (!config_->triggeredByUser)
    {
        const HCURSOR arrowCursor = LoadCursor(NULL, IDC_ARROW);
        const HCURSOR beamCursor = LoadCursor(NULL, IDC_IBEAM);
        const HCURSOR handCursor = LoadCursor(NULL, IDC_HAND);
        // An I-beam at either end is enough. Custom cursors are rejected unless the
        // application is explicitly listed to bypass cursor detection (for example,
        // Adobe Acrobat).
        if (config_->mouseDownCursor != beamCursor && config_->mouseUpCursor != beamCursor)
        {
            if (config_->mouseUpCursor != arrowCursor && config_->mouseUpCursor != handCursor)
                return IsInFilterList(programName, config_->excludeClipboardCursorDetect);

            // Arrow/hand cursors can still represent text in Chromium. Keep the known
            // UIA control-type exceptions: DevTools uses Group (50026), while pages
            // commonly expose Document (50030) or Text (50020) controls.
            // https://learn.microsoft.com/windows/win32/winauto/uiauto-controltype-ids
            if (controlType_ != UIA_GroupControlTypeId && controlType_ != UIA_DocumentControlTypeId &&
                controlType_ != UIA_TextControlTypeId)
                return false;
        }
    }
    return true;
}

/** Distinguish a selected text role from UI-object selections reported by MSAA. */
TextRetrievalResult SelectionEngine::ClassifyAccessibleRole(IAccessible *accessible, const VARIANT &child)
{
    if (!accessible)
        return TextRetrievalResult::Unavailable;

    TextRetrievalResult result = TextRetrievalResult::Unavailable;
    VARIANT role;
    VariantInit(&role);
    const HRESULT hr = accessible->get_accRole(child, &role);
    if (hr == S_OK && role.vt == VT_I4)
    {
        // ROLE_SYSTEM_TEXT is not proof that text is available; it only permits later
        // fallback. Other concrete UI roles in this range represent object selection.
        if (role.lVal != ROLE_SYSTEM_TEXT && role.lVal >= ROLE_SYSTEM_TITLEBAR &&
            role.lVal <= ROLE_SYSTEM_OUTLINEBUTTON)
            result = TextRetrievalResult::NonTextSelection;
    }
    VariantClear(&role);
    return result;
}

/** Interpret the VARIANT forms returned by IAccessible::get_accSelection. */
TextRetrievalResult SelectionEngine::ProcessAccessibleSelection(IAccessible *accessible,
                                                                TextSelectionInfo &selectionInfo)
{
    if (!accessible)
        return TextRetrievalResult::Unavailable;

    TextRetrievalResult result = TextRetrievalResult::Unavailable;
    VARIANT selection;
    VariantInit(&selection);
    const HRESULT hr = accessible->get_accSelection(&selection);
    if (SUCCEEDED(hr))
    {
        if (selection.vt == VT_BSTR && selection.bstrVal)
        {
            // BSTRs are length-prefixed and may contain embedded NULs. SysStringLen
            // preserves the provider's complete result instead of truncating it.
            selectionInfo.text.assign(selection.bstrVal, SysStringLen(selection.bstrVal));
            RemoveObjectReplacementChars(selectionInfo.text);
            if (!selectionInfo.text.empty())
                result = TextRetrievalResult::TextFound;
        }
        // Object/enumerated selections are deliberately classified separately so the
        // caller does not fall through to a simulated copy keystroke.
        else if (selection.vt == VT_UNKNOWN || (selection.vt & VT_ARRAY) != 0)
        {
            result = TextRetrievalResult::NonTextSelection;
        }
        else if (selection.vt == VT_I4)
        {
            result = ClassifyAccessibleRole(accessible, selection);
        }
        else if (selection.vt == VT_DISPATCH && selection.pdispVal)
        {
            IAccessible *selectedAccessible = nullptr;
            const HRESULT queryHr =
                selection.pdispVal->QueryInterface(IID_IAccessible, reinterpret_cast<void **>(&selectedAccessible));
            if (queryHr == S_OK && selectedAccessible)
            {
                VARIANT childSelf;
                VariantInit(&childSelf);
                childSelf.vt = VT_I4;
                childSelf.lVal = CHILDID_SELF;
                result = ClassifyAccessibleRole(selectedAccessible, childSelf);
                selectedAccessible->Release();
            }
        }
    }
    VariantClear(&selection);
    return result;
}

/**
 * Read selection through a UIA TextPattern.
 *
 * When allowDocumentFallback is false, only GetSelection is safe. The active-selection
 * attribute on an ancestor document can be unreliable and could expose the whole page.
 */
bool SelectionEngine::TryGetTextViaTextPattern(IUIAutomationElement *element, TextSelectionInfo &selectionInfo,
                                               bool allowDocumentFallback)
{
    if (!element || IsStopping())
        return false;

    // Ancestor walk-up calls this with allowDocumentFallback=false. IsSelectionActive
    // is only trustworthy on the focused element; using an ancestor document range can
    // return the entire page as though it were the selection.
    bool result = false;
    IUIAutomationTextPattern *textPattern = nullptr;
    HRESULT hr = element->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
                                              reinterpret_cast<void **>(&textPattern));
    if (SUCCEEDED(hr) && textPattern)
    {
        // First approach: ask the provider for its explicit selection ranges.
        IUIAutomationTextRangeArray *ranges = nullptr;
        hr = textPattern->GetSelection(&ranges);
        if (SUCCEEDED(hr) && ranges)
        {
            int count = 0;
            hr = ranges->get_Length(&count);
            if (SUCCEEDED(hr) && count > 0)
            {
                // Providers may return multiple ranges; keep the first non-empty range
                // with usable screen coordinates, matching the historical behavior.
                for (int i = 0; i < count && !result && !IsStopping(); i++)
                {
                    IUIAutomationTextRange *range = nullptr;
                    hr = ranges->GetElement(i, &range);
                    if (SUCCEEDED(hr) && range)
                    {
                        BSTR text = nullptr;
                        hr = range->GetText(-1, &text);
                        if (SUCCEEDED(hr) && text)
                        {
                            // UIA returns BSTR. Preserve its explicit length, including
                            // embedded NULs, instead of relying on null termination.
                            selectionInfo.text.assign(text, SysStringLen(text));
                            RemoveObjectReplacementChars(selectionInfo.text);
                            if (!selectionInfo.text.empty())
                                result = SetTextRangeCoordinates(range, selectionInfo);
                            SysFreeString(text);
                        }
                        range->Release();
                    }
                }
            }
            ranges->Release();
        }

        // Some providers expose the active selection only through DocumentRange.
        if (!result && allowDocumentFallback && !IsStopping())
        {
            IUIAutomationTextRange *documentRange = nullptr;
            hr = textPattern->get_DocumentRange(&documentRange);
            if (SUCCEEDED(hr) && documentRange)
            {
                bool hasSelection = false;
                // Query the selection attribute before GetText(-1). This avoids reading
                // the entire document when there is no active selection.
                VARIANT active;
                VariantInit(&active);
                const HRESULT attributeHr = documentRange->GetAttributeValue(UIA_IsSelectionActivePropertyId, &active);
                if (SUCCEEDED(attributeHr) && active.vt == VT_BOOL && active.boolVal == VARIANT_TRUE && !IsStopping())
                {
                    BSTR text = nullptr;
                    hr = documentRange->GetText(-1, &text);
                    if (SUCCEEDED(hr) && text)
                    {
                        std::wstring selectedText(text, SysStringLen(text));
                        RemoveObjectReplacementChars(selectedText);
                        if (!selectedText.empty())
                        {
                            selectionInfo.text = std::move(selectedText);
                            if (SetTextRangeCoordinates(documentRange, selectionInfo))
                            {
                                result = true;
                                hasSelection = true;
                            }
                        }
                        SysFreeString(text);
                    }
                }
                VariantClear(&active);

                // Compatibility fallback for providers that report the active flag only
                // after expanding the range to TextUnit_Document.
                if (!hasSelection && !IsStopping())
                {
                    hr = documentRange->ExpandToEnclosingUnit(TextUnit_Document);
                    if (SUCCEEDED(hr))
                    {
                        VariantInit(&active);
                        hr = documentRange->GetAttributeValue(UIA_IsSelectionActivePropertyId, &active);
                        if (SUCCEEDED(hr) && active.vt == VT_BOOL && active.boolVal == VARIANT_TRUE && !IsStopping())
                        {
                            BSTR text = nullptr;
                            hr = documentRange->GetText(-1, &text);
                            if (SUCCEEDED(hr) && text)
                            {
                                std::wstring documentText(text, SysStringLen(text));
                                RemoveObjectReplacementChars(documentText);
                                if (!documentText.empty())
                                {
                                    selectionInfo.text = std::move(documentText);
                                    result = SetTextRangeCoordinates(documentRange, selectionInfo);
                                }
                                SysFreeString(text);
                            }
                        }
                        VariantClear(&active);
                    }
                }
                documentRange->Release();
            }
        }
        textPattern->Release();
    }
    return result;
}

/** Retrieve selection through UI Automation and its legacy compatibility pattern. */
TextRetrievalResult SelectionEngine::GetTextViaUIAutomation(HWND hwnd, TextSelectionInfo &selectionInfo)
{
#ifdef _DEBUG
    assert(ownerThreadId_ == GetCurrentThreadId());
#endif
    if (!automation_ || !hwnd || IsStopping())
        return TextRetrievalResult::Unavailable;

    TextRetrievalResult result = TextRetrievalResult::Unavailable;
    TextRetrievalResult legacyResult = TextRetrievalResult::Unavailable;
    controlType_ = UIA_WindowControlTypeId;

    // Preserve the original ElementFromHandle probe before querying the focused element.
    // Some providers do not expose usable accessibility data for an invalid target HWND.
    IUIAutomationElement *windowElement = nullptr;
    HRESULT hr = automation_->ElementFromHandle(hwnd, &windowElement);
    if (FAILED(hr) || !windowElement)
        return TextRetrievalResult::Unavailable;

    // Selection is normally owned by the focused element, not necessarily by the HWND
    // passed to ElementFromHandle.
    IUIAutomationElement *focusedElement = nullptr;
    hr = automation_->GetFocusedElement(&focusedElement);
    windowElement->Release();
    if (FAILED(hr) || !focusedElement || IsStopping())
    {
        if (focusedElement)
            focusedElement->Release();
        return TextRetrievalResult::Unavailable;
    }

    CONTROLTYPEID controlType = UIA_WindowControlTypeId;
    hr = focusedElement->get_CurrentControlType(&controlType);
    if (SUCCEEDED(hr))
        controlType_ = controlType;

    // Approach 1: native UIA TextPattern on the focused element.
    if (TryGetTextViaTextPattern(focusedElement, selectionInfo, true))
        result = TextRetrievalResult::TextFound;

    // Approach 2: UIA's LegacyIAccessible bridge.
    if (result != TextRetrievalResult::TextFound && !IsStopping())
    {
        IUIAutomationLegacyIAccessiblePattern *legacyPattern = nullptr;
        hr = focusedElement->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                                                 __uuidof(IUIAutomationLegacyIAccessiblePattern),
                                                 reinterpret_cast<void **>(&legacyPattern));
        if (SUCCEEDED(hr) && legacyPattern)
        {
            IAccessible *accessible = nullptr;
            hr = legacyPattern->GetIAccessible(&accessible);
            if (SUCCEEDED(hr) && accessible)
            {
                legacyResult = ProcessAccessibleSelection(accessible, selectionInfo);
                if (legacyResult == TextRetrievalResult::TextFound)
                    result = TextRetrievalResult::TextFound;
                accessible->Release();
            }
            legacyPattern->Release();
        }
    }

    // Approach 3: walk ancestors and retry TextPattern. In Chromium/Edge a tabindex
    // container can receive focus while an ancestor Document owns the selection.
    // See issue #18 and PR #14.
    if (result != TextRetrievalResult::TextFound && !IsStopping())
    {
        IUIAutomationTreeWalker *treeWalker = nullptr;
        hr = automation_->get_ControlViewWalker(&treeWalker);
        if (SUCCEEDED(hr) && treeWalker)
        {
            // The Document element is normally only a few levels above the focus owner.
            constexpr int maxWalkUpLevels = 10;
            IUIAutomationElement *current = focusedElement;
            current->AddRef();
            for (int level = 0; level < maxWalkUpLevels && result != TextRetrievalResult::TextFound && !IsStopping();
                 level++)
            {
                IUIAutomationElement *parent = nullptr;
                hr = treeWalker->GetParentElement(current, &parent);
                current->Release();
                current = nullptr;
                if (FAILED(hr) || !parent)
                    break;
                current = parent;
                // Ancestors may use GetSelection only; document fallback is unsafe here.
                if (TryGetTextViaTextPattern(current, selectionInfo, false))
                    result = TextRetrievalResult::TextFound;
            }
            if (current)
                current->Release();
            treeWalker->Release();
        }
    }

    if (result != TextRetrievalResult::TextFound && legacyResult == TextRetrievalResult::NonTextSelection)
        result = TextRetrievalResult::NonTextSelection;
    focusedElement->Release();
    return result;
}

/** Retrieve selection through the legacy window IAccessible client object. */
TextRetrievalResult SelectionEngine::GetTextViaAccessible(HWND hwnd, TextSelectionInfo &selectionInfo)
{
#ifdef _DEBUG
    assert(ownerThreadId_ == GetCurrentThreadId());
#endif
    if (!hwnd || IsStopping())
        return TextRetrievalResult::Unavailable;

    // Query the client object, which is where legacy applications expose selection.
    IAccessible *accessible = nullptr;
    const HRESULT hr =
        AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible, reinterpret_cast<void **>(&accessible));
    if (FAILED(hr) || !accessible)
        return TextRetrievalResult::Unavailable;
    const TextRetrievalResult result = ProcessAccessibleSelection(accessible, selectionInfo);
    accessible->Release();
    return result;
}

/** Extract first/last usable UIA bounding rectangles in physical screen coordinates. */
bool SelectionEngine::SetTextRangeCoordinates(IUIAutomationTextRange *range, TextSelectionInfo &selectionInfo)
{
    if (!range || IsStopping())
        return false;

    SAFEARRAY *rectArray = nullptr;
    const HRESULT hr = range->GetBoundingRectangles(&rectArray);
    if (FAILED(hr) || !rectArray)
        return false;

    // Access the SAFEARRAY directly; it stores repeating left/top/width/height values.
    double *rects = nullptr;
    const HRESULT accessHr = SafeArrayAccessData(rectArray, reinterpret_cast<void **>(&rects));
    if (FAILED(accessHr) || !rects)
    {
        SafeArrayDestroy(rectArray);
        return false;
    }

    LONG lowerBound = 0;
    LONG upperBound = -1;
    SafeArrayGetLBound(rectArray, 1, &lowerBound);
    SafeArrayGetUBound(rectArray, 1, &upperBound);
    const int rectCount = (upperBound - lowerBound + 1) / 4;
    int firstValid = -1;
    int lastValid = -1;
    // Ignore near-zero-width artifacts and implausibly tall provider rectangles. These
    // thresholds are historical compatibility behavior, not general layout validation.
    for (int i = 0; i < rectCount; i++)
    {
        const int index = i * 4;
        if (rects[index + 2] > 1.0 && rects[index + 3] < 100.0)
        {
            firstValid = index;
            break;
        }
    }
    for (int i = rectCount - 1; i >= 0; i--)
    {
        const int index = i * 4;
        if (rects[index + 2] > 1.0 && rects[index + 3] < 100.0)
        {
            lastValid = index;
            break;
        }
    }

    bool found = false;
    // The first valid rectangle supplies the left edge of the first line; the last
    // supplies the right edge of the last line.
    if (firstValid >= 0 && lastValid >= 0)
    {
        selectionInfo.startTop.x = static_cast<LONG>(rects[firstValid]);
        selectionInfo.startTop.y = static_cast<LONG>(rects[firstValid + 1]);
        selectionInfo.startBottom.x = static_cast<LONG>(rects[firstValid]);
        selectionInfo.startBottom.y = static_cast<LONG>(rects[firstValid + 1] + rects[firstValid + 3]);
        selectionInfo.endBottom.x = static_cast<LONG>(rects[lastValid] + rects[lastValid + 2]);
        selectionInfo.endBottom.y = static_cast<LONG>(rects[lastValid + 1] + rects[lastValid + 3]);
        selectionInfo.endTop.x = static_cast<LONG>(rects[lastValid] + rects[lastValid + 2]);
        selectionInfo.endTop.y = static_cast<LONG>(rects[lastValid + 1]);
        selectionInfo.posLevel = SelectionPositionLevel::Full;
        found = true;
    }
    // Always unlock before destroying the SAFEARRAY, including the no-valid-rect case.
    SafeArrayUnaccessData(rectArray);
    SafeArrayDestroy(rectArray);
    return found;
}

/**
 * Last-resort retrieval using a saved clipboard and synthesized copy shortcuts.
 *
 * Every early exit after a non-empty backup must restore the user's clipboard.
 */
bool SelectionEngine::GetTextViaClipboard(HWND hwnd, TextSelectionInfo &selectionInfo)
{
    if (!hwnd || IsStopping())
        return false;

    // Clipboard extraction is one indivisible save/copy/read/restore transaction.
    std::unique_lock<std::mutex> clipboardLock(g_clipboard_mutex, std::defer_lock);
    if (clipboardLockMode_ == ClipboardLockMode::TryOnly)
    {
        if (!clipboardLock.try_lock())
            return false;
    }
    else
    {
        // Only the worker may wait, and every wait remains stop-responsive.
        while (!clipboardLock.try_lock())
        {
            if (IsStopping())
                return false;
            Sleep(5);
        }
    }

    // Compatibility timings inherited from the original clipboard path. Before changing
    // these polling windows, revalidate slow clipboard providers, user Ctrl+C/X/V races,
    // JetBrains IDEs, browsers, Office, PDF readers, and delay-list applications.
    constexpr DWORD sleepInterval = 5;
    // Direct user-triggered retrieval skips this preprocessing because the library, not
    // a mouse gesture, initiated the action. Automatic retrieval must avoid interfering
    // with a user's Ctrl+C, Ctrl+X, or Ctrl+V already in progress.
    if (!config_->triggeredByUser)
    {
        bool ctrlPressed = false;
        bool cPressed = false;
        bool xPressed = false;
        bool vPressed = false;
        int checkCount = 0;
        constexpr int maxChecks = 5;
        while (checkCount < maxChecks && !IsStopping())
        {
            // A sequence change since mouse-down means the application/user already
            // copied the selection; read it directly without injecting another key.
            if (GetClipboardSequenceNumber() != clipboardSequence_)
                return ReadClipboard(selectionInfo.text) && !selectionInfo.text.empty();

            const bool ctrlPressing = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool cPressing = (GetAsyncKeyState('C') & 0x8000) != 0;
            const bool xPressing = (GetAsyncKeyState('X') & 0x8000) != 0;
            const bool vPressing = (GetAsyncKeyState('V') & 0x8000) != 0;
            if (!ctrlPressing && !cPressing && !xPressing && !vPressing)
                break;

            ctrlPressed = ctrlPressed || ctrlPressing;
            cPressed = cPressed || cPressing;
            xPressed = xPressed || xPressing;
            vPressed = vPressed || vPressing;
            checkCount++;
            if (!SleepInterruptibly(40))
                return false;
        }
        // Five 40 ms polls retain the historical ~200 ms intent-detection window.
        if (IsStopping() || checkCount >= maxChecks || (ctrlPressed && (cPressed || xPressed || vPressed)))
            return false;
    }

    // Do not replace this with WM_COMMAND/ID_EDIT_COPY. That approach is known to crash
    // JetBrains IDEs; synthesized keyboard input is the established compatibility path.

    // Preserve every backupable clipboard format, not only text. A fallback must be
    // transparent to users who have copied files, images, or rich content.
    ClipboardBackup clipboardBackup;
    if (!OpenClipboard(nullptr))
        return false;
    BackupClipboard(clipboardBackup, true);
    EmptyClipboard();
    CloseClipboard();

    const auto restoreBackup = [&]()
    {
        if (clipboardBackup.HasData())
        {
            RestoreClipboard(clipboardBackup);
            // Restoration itself changes the sequence number. Publish this final value
            // so the next gesture does not mistake our restore for a user's copy.
            clipboardSequence_ = GetClipboardSequenceNumber();
        }
    };

    // Delay-list applications skip Ctrl+Insert and need extra settling time after Ctrl+C.
    const bool delayRead = !selectionInfo.programName.empty() &&
                           IsInFilterList(selectionInfo.programName, config_->includeClipboardDelayRead);
    if (!delayRead)
    {
        if (IsStopping() || ShouldKeyInterruptViaClipboard())
        {
            restoreBackup();
            return false;
        }

        // Ctrl+Insert is tried first because it is less likely than Ctrl+C to collide
        // with application shortcuts, though some applications do not support it.
        clipboardSequence_ = GetClipboardSequenceNumber();
        SendCopyKey(CopyKeyType::CtrlInsert);
        bool hasNewContent = false;
        SetStage(RetrievalStage::ClipboardPoll);
        // Poll for up to about 100 ms (20 * 5 ms).
        for (int i = 0; i < 20 && !IsStopping(); i++)
        {
            if (GetClipboardSequenceNumber() != clipboardSequence_)
            {
                hasNewContent = true;
                break;
            }
            if (!SleepInterruptibly(sleepInterval))
                break;
        }
        SetStage(RetrievalStage::Clipboard);
        if (hasNewContent)
        {
            if (!SleepInterruptibly(10))
            {
                restoreBackup();
                return false;
            }
            const bool readSuccess = ReadClipboard(selectionInfo.text);
            restoreBackup();
            return readSuccess && !selectionInfo.text.empty();
        }
    }

    if (IsStopping() || ShouldKeyInterruptViaClipboard())
    {
        restoreBackup();
        return false;
    }

    // Ctrl+C is the final fallback.
    clipboardSequence_ = GetClipboardSequenceNumber();
    SendCopyKey(CopyKeyType::CtrlC);
    bool hasNewContent = false;
    SetStage(RetrievalStage::ClipboardPoll);
    // Poll for up to about 180 ms (36 * 5 ms).
    for (int i = 0; i < 36 && !IsStopping(); i++)
    {
        if (GetClipboardSequenceNumber() != clipboardSequence_)
        {
            hasNewContent = true;
            break;
        }
        if (!SleepInterruptibly(sleepInterval))
            break;
    }
    SetStage(RetrievalStage::Clipboard);
    if (!hasNewContent || IsStopping())
    {
        restoreBackup();
        return false;
    }

    // Some applications update the clipboard several times after the first sequence
    // change. Adobe Acrobat is the original reason for the 135 ms delay list.
    if (delayRead && !SleepInterruptibly(135))
    {
        restoreBackup();
        return false;
    }
    if (!SleepInterruptibly(10) || ShouldKeyInterruptViaClipboard())
    {
        restoreBackup();
        return false;
    }

    const bool readSuccess = ReadClipboard(selectionInfo.text);
    restoreBackup();
    return readSuccess && !selectionInfo.text.empty();
}

/** Inject Ctrl+Insert or Ctrl+C while respecting modifiers already held by the user. */
void SelectionEngine::SendCopyKey(CopyKeyType type) const
{
    const bool cPressing = (GetAsyncKeyState('C') & 0x8000) != 0;
    const bool ctrlPressing = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool altPressing = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shiftPressing = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    // A real Ctrl+C already in progress belongs to the user; never duplicate it.
    if (ctrlPressing && cPressing)
        return;

    const WORD keyCode = type == CopyKeyType::CtrlInsert ? VK_INSERT : 'C';
    std::vector<INPUT> inputs;
    INPUT input = {};
    // Release modifiers that would change the meaning of Ctrl+Insert/C. Compatibility
    // behavior does not synthesize modifier key-down events afterward; changing this
    // requires physical modifier-state regression testing.
    if (altPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_MENU;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
    if (shiftPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_SHIFT;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
    // Keep the injected shortcut symmetric. Do not release Ctrl if the user already
    // held it before this sequence.
    if (!ctrlPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_RCONTROL;
        input.ki.dwFlags = 0;
        inputs.push_back(input);
    }
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = keyCode;
    input.ki.dwFlags = 0;
    inputs.push_back(input);
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(input);
    if (!ctrlPressing)
    {
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_RCONTROL;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

/** Return true when a new user Ctrl action should abort automatic clipboard retrieval. */
bool SelectionEngine::ShouldKeyInterruptViaClipboard() const
{
    // Recheck before each synthetic copy step. Ctrl pressed during an automatic
    // retrieval means the user started another action and the fallback must yield.
    return config_ && !config_->triggeredByUser && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

bool SelectionEngine::SleepInterruptibly(DWORD milliseconds) const
{
    // UIA/MSAA calls cannot be hard-cancelled, but every library-controlled clipboard
    // delay is split into short waits so Stop() can complete promptly.
    constexpr DWORD interval = 5;
    DWORD elapsed = 0;
    while (elapsed < milliseconds)
    {
        if (IsStopping())
            return false;
        const DWORD wait = std::min(interval, milliseconds - elapsed);
        Sleep(wait);
        elapsed += wait;
    }
    return !IsStopping();
}

bool SelectionEngine::IsStopping() const
{
    return stopToken_ && stopToken_->load(std::memory_order_acquire);
}

void SelectionEngine::SetStage(RetrievalStage stage) const
{
    if (stage_)
        stage_->store(stage, std::memory_order_release);
}
