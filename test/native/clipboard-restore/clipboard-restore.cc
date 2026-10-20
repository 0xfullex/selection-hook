/**
 * Regression: main left injected text when the original was empty, overwrote physical
 * user copies, opened only once, and reported individual format failures as success.
 * Production methods are extracted by the runner; the clipboard utility is included.
 */
#include "windows.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../../src/windows/core/clipboard_activity.h"
#include "../../../src/windows/lib/clipboard.cc"

struct Block
{
    std::vector<BYTE> bytes;
    bool emf = false;
    int locks = 0;
};
static std::map<UINT, Block *> clipboard;
static std::map<int, bool> held;
static DWORD sequence = 1;
static bool opened = false, stopping = false, sameThread = false;
static int openCalls = 0, failOpens = 0, emptyCalls = 0, closeCalls = 0;
static int allocCalls = 0, failAllocCall = 0, lockCalls = 0, failLockCall = 0;
static int liveBlocks = 0, wrongFrees = 0, activeLocks = 0;
static int failSetCount = 0, failedSetFormat = 0;
static bool failEmpty = false, failClose = false, failEmf = false;
static SIZE_T allocationExtra = 0;
static bool copyOnSend = true, ctrlInsertNoCopy = false;
static int sendInputResult = -1;
static std::vector<std::vector<INPUT>> sentInputs;
static std::vector<DWORD> waits;
static std::vector<size_t> injectionWaitOffsets;
static std::function<void()> onSleep, onSend, onEmpty, onClose, onGet, onOpen, onLock, onSequence;
static std::function<bool(int)> failOpenAt;
static std::shared_ptr<ClipboardActivityState> activity;

static Block *NewBlock(const std::vector<BYTE> &bytes, bool emf = false)
{
    ++liveBlocks;
    return new Block{bytes, emf};
}
static void FreeBlock(Block *p, bool emf)
{
    if (p->emf != emf)
        ++wrongFrees;
    activeLocks -= p->locks;
    delete p;
    --liveBlocks;
}
static void Clear()
{
    for (auto &item : clipboard)
        FreeBlock(item.second, item.second->emf);
    clipboard.clear();
}
static std::vector<BYTE> Bytes(const std::wstring &text)
{
    const auto *p = reinterpret_cast<const BYTE *>(text.c_str());
    return {p, p + (text.size() + 1) * sizeof(wchar_t)};
}
static void Copy(UINT format, const std::vector<BYTE> &bytes)
{
    Clear();
    clipboard[format] = NewBlock(bytes, format == CF_ENHMETAFILE);
    ++sequence;
}
static void CopyText(const std::wstring &text) { Copy(CF_UNICODETEXT, Bytes(text)); }
static bool HasBytes(UINT format, const std::vector<BYTE> &bytes)
{
    const auto it = clipboard.find(format);
    return it != clipboard.end() && it->second->bytes.size() >= bytes.size() &&
           std::equal(bytes.begin(), bytes.end(), it->second->bytes.begin());
}
static void Fire(std::function<void()> &callback)
{
    if (callback)
    {
        auto action = std::move(callback);
        callback = {};
        action();
    }
}
BOOL OpenClipboard(HWND owner)
{
    ++openCalls;
    if (owner != nullptr)
        ++wrongFrees;  // A transaction must retain main's null opener behavior.
    if (failOpens > 0)
    {
        --failOpens;
        return 0;
    }
    if ((failOpenAt && failOpenAt(openCalls)) || opened)
        return 0;
    opened = true;
    Fire(onOpen);
    return 1;
}
BOOL CloseClipboard()
{
    ++closeCalls;
    opened = false;
    Fire(onClose);
    return !failClose;
}
BOOL EmptyClipboard()
{
    ++emptyCalls;
    if (failEmpty)
        return 0;
    Clear();
    ++sequence;
    Fire(onEmpty);
    return 1;
}
HANDLE GetClipboardData(UINT format)
{
    Fire(onGet);
    auto it = clipboard.find(format);
    return it == clipboard.end() ? nullptr : it->second;
}
HANDLE SetClipboardData(UINT format, HANDLE handle)
{
    if (failedSetFormat == static_cast<int>(format) || failSetCount > 0)
    {
        if (failSetCount > 0)
            --failSetCount;
        return nullptr;
    }
    if (clipboard.count(format))
        FreeBlock(clipboard[format], clipboard[format]->emf);
    clipboard[format] = static_cast<Block *>(handle);
    ++sequence;
    return handle;
}
int CountClipboardFormats() { return static_cast<int>(clipboard.size()); }
UINT EnumClipboardFormats(UINT format)
{
    auto it = clipboard.upper_bound(format);
    return it == clipboard.end() ? 0 : it->first;
}
HGLOBAL GlobalAlloc(UINT, SIZE_T size)
{
    if (++allocCalls == failAllocCall)
        return nullptr;
    return NewBlock(std::vector<BYTE>(size + allocationExtra));
}
HGLOBAL GlobalFree(HGLOBAL handle)
{
    FreeBlock(static_cast<Block *>(handle), false);
    return nullptr;
}
SIZE_T GlobalSize(HGLOBAL handle) { return static_cast<Block *>(handle)->bytes.size(); }
void *GlobalLock(HGLOBAL handle)
{
    Fire(onLock);
    if (++lockCalls == failLockCall)
        return nullptr;
    ++activeLocks;
    auto *p = static_cast<Block *>(handle);
    ++p->locks;
    return p->bytes.data();
}
BOOL GlobalUnlock(HGLOBAL handle)
{
    --activeLocks;
    --static_cast<Block *>(handle)->locks;
    return 1;
}
UINT GetEnhMetaFileBits(HENHMETAFILE handle, UINT size, BYTE *out)
{
    auto &bytes = static_cast<Block *>(handle)->bytes;
    if (out)
        memcpy(out, bytes.data(), size);
    return static_cast<UINT>(bytes.size());
}
HENHMETAFILE SetEnhMetaFileBits(UINT size, const BYTE *bytes)
{
    return failEmf ? nullptr : NewBlock({bytes, bytes + size}, true);
}
BOOL DeleteEnhMetaFile(HENHMETAFILE handle)
{
    FreeBlock(static_cast<Block *>(handle), true);
    return 1;
}
int MultiByteToWideChar(UINT, DWORD, const char *text, int, wchar_t *out, int)
{
    int length = static_cast<int>(strlen(text)) + 1;
    if (out)
        for (int i = 0; i < length; ++i)
            out[i] = text[i];
    return length;
}
DWORD GetClipboardSequenceNumber() { Fire(onSequence); return sequence; }
short GetAsyncKeyState(int key) { return held[key] ? static_cast<short>(0x8000) : 0; }
void Sleep(DWORD ms)
{
    waits.push_back(ms);
    Fire(onSleep);
}
UINT SendInput(UINT count, INPUT *inputs, int)
{
    injectionWaitOffsets.push_back(waits.size());
    sentInputs.emplace_back(inputs, inputs + count);
    if (sendInputResult == 0)
    {
        Fire(onSend);
        return 0;
    }
    bool insert = false;
    for (UINT i = 0; i < count; ++i)
    {
        const auto &key = inputs[i].ki;
        insert = insert || key.wVk == VK_INSERT;
        if (activity)
            activity->RecordKey((key.dwFlags & KEYEVENTF_KEYUP) == 0, key.wVk, LLKHF_INJECTED,
                                key.dwExtraInfo);
    }
    if (copyOnSend && !(insert && ctrlInsertNoCopy))
        CopyText(L"selected");
    Fire(onSend);
    return sendInputResult < 0 ? count : static_cast<UINT>(sendInputResult);
}

static std::mutex g_clipboard_mutex;
enum class ClipboardLockMode { TryOnly, InterruptibleWait };
enum class RetrievalStage { Clipboard, ClipboardPoll };
enum class CopyKeyType { CtrlInsert, CtrlC };
struct TextSelectionInfo { std::wstring text, programName; };
struct TestConfig
{
    bool triggeredByUser = true;
    std::vector<std::string> includeClipboardDelayRead;
    std::shared_ptr<ClipboardActivityState> clipboardActivity;
};
bool IsWindowOnCallingThread(HWND) { return sameThread; }
class SelectionEngine
{
  public:
    TestConfig config;
    TestConfig *config_ = &config;
    ClipboardLockMode clipboardLockMode_ = ClipboardLockMode::TryOnly;
    DWORD clipboardSequence_ = 0;
    bool IsStopping() const { return stopping; }
    bool IsInFilterList(const std::wstring &name, const std::vector<std::string> &list) const
    { return name == L"delay" && !list.empty(); }
    void SetStage(RetrievalStage) {}
    bool GetTextViaClipboard(HWND, TextSelectionInfo &);
    bool GetTextViaClipboardTransaction(HWND, TextSelectionInfo &);
    bool GetTextViaClipboardLegacy(HWND, TextSelectionInfo &);
    bool OpenClipboardWithRetry() const;
    bool ReadClipboardWithRetry(std::wstring &) const;
    bool SendCopyKey(CopyKeyType, ULONG_PTR = 0) const;
    bool ShouldKeyInterruptViaClipboard() const;
    bool SleepInterruptibly(DWORD) const;
};
#include "build/production-engine.inc"

static int failures = 0;
static void Check(bool condition, const char *name)
{
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    if (!condition)
        ++failures;
}
static void Reset(bool hooks = true)
{
    Clear();
    held.clear();
    sequence = 1;
    opened = stopping = sameThread = failEmpty = failClose = failEmf = false;
    openCalls = failOpens = emptyCalls = closeCalls = 0;
    allocCalls = failAllocCall = lockCalls = failLockCall = 0;
    failSetCount = failedSetFormat = 0;
    allocationExtra = 0;
    copyOnSend = true;
    ctrlInsertNoCopy = false;
    sendInputResult = -1;
    sentInputs.clear();
    injectionWaitOffsets.clear();
    waits.clear();
    onSleep = onSend = onEmpty = onClose = onGet = onOpen = onLock = onSequence = {};
    failOpenAt = {};
    activity = std::make_shared<ClipboardActivityState>();
    if (hooks)
        activity->Activate();
}
static bool Run(SelectionEngine &engine, TextSelectionInfo &selection)
{
    engine.config.clipboardActivity = activity;
    return engine.GetTextViaClipboard(reinterpret_cast<HWND>(2), selection);
}
static void Shortcut(DWORD modifier = VK_LCONTROL, DWORD key = 'C')
{
    held[modifier] = true;
    held[modifier == VK_LSHIFT || modifier == VK_RSHIFT ? VK_SHIFT : VK_CONTROL] = true;
    activity->RecordKey(true, modifier, 0, 0);
    activity->RecordKey(true, key, 0, 0);
    activity->RecordKey(false, key, 0, 0);
    activity->RecordKey(false, modifier, 0, 0);
    held[modifier] = held[VK_CONTROL] = held[VK_SHIFT] = false;
}
static ClipboardBackup TwoFormats()
{
    ClipboardBackup backup;
    backup.isEmpty = false;
    backup.formats = {{CF_DIB, {1, 2}, 2}, {0xc101, {3, 4}, 2}};
    return backup;
}
int main()
{
    std::cout.setf(std::ios::unitbuf);
    // Round two: 0497638 cancelled retrieval on a pending shortcut and accepted
    // outstanding injected writes and its own CloseClipboard bump as user evidence.
    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s;
      onEmpty = [] { Shortcut(); };
      onSleep = [] { CopyText(L"user"); };
      Check(Run(e, s) && s.text == L"user" && sentInputs.empty() && HasBytes(CF_UNICODETEXT, Bytes(L"user")),
            "D1 pre-injection Ctrl+C+C returns and preserves user text without injection"); }
    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s;
      onEmpty = [] { Shortcut(); held[VK_CONTROL] = held['C'] = true; };
      onSleep = [] { CopyText(L"user"); };
      Check(Run(e, s) && s.text == L"user" && sentInputs.empty() && HasBytes(CF_UNICODETEXT, Bytes(L"user")),
            "D1 held Ctrl+C+C returns user text while both keys remain pressed"); }
    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s;
      onClose = [] { onSequence = [] { onSequence = [] { Shortcut(); CopyText(L"user"); }; }; };
      Check(Run(e, s) && s.text == L"user" && sentInputs.empty() && HasBytes(CF_UNICODETEXT, Bytes(L"user")),
            "D1 user write before initial pending poll baseline is returned and preserved"); }
    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s; int sleeps = 0; std::function<void()> advance;
      onEmpty = [] { Shortcut(); };
      advance = [&] { if (++sleeps == 20) onSequence = [] { CopyText(L"user"); };
                     else onSleep = advance; };
      onSleep = advance;
      Check(Run(e, s) && s.text == L"user" && sentInputs.empty() && HasBytes(CF_UNICODETEXT, Bytes(L"user")),
            "D1 pending write between poll windows is returned without consuming its baseline"); }
    Reset(); CopyText(L"old"); copyOnSend = false; held[VK_CONTROL] = held['C'] = true;
    { SelectionEngine e; TextSelectionInfo s;
      onSleep = [] { Shortcut(); CopyText(L"user"); };
      Check(Run(e, s) && s.text == L"user" && sentInputs.empty() && HasBytes(CF_UNICODETEXT, Bytes(L"user")),
            "D1 skipped held Ctrl+C injection leaves subsequent repeat write eligible as user copy"); }
    for (int inserted : {0, 1})
    {
        Reset(); CopyText(L"old"); copyOnSend = false; sendInputResult = inserted;
        SelectionEngine e; TextSelectionInfo s;
        onSleep = [] { Shortcut(); CopyText(L"user"); };
        Check(Run(e, s) && s.text == L"user" && HasBytes(CF_UNICODETEXT, Bytes(inserted == 0 ? L"user" : L"old")),
              inserted == 0 ? "zero SendInput insertion leaves user write eligible" :
                              "partial SendInput insertion remains conservatively outstanding");
    }
    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s;
      onSleep = [] { Shortcut(); CopyText(L"selected"); };
      Check(Run(e, s) && s.text == L"selected" && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "E1 outstanding injected result after key returns text but restores original"); }
    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s;
      onSend = [] { onSend = [] { Shortcut(); CopyText(L"first injected");
                                 onSleep = [] { CopyText(L"second injected"); }; }; };
      Check(Run(e, s) && s.text == L"second injected" && sentInputs.size() == 2 &&
                HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "E1 two outstanding injections each consume their own distinct write before user evidence"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s;
      onEmpty = [] { Shortcut(); onClose = [] { ++sequence; }; };
      Check(!Run(e, s) && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "E2 clear Close bump is our own sequence and cannot preserve empty content"); }
    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s;
      onEmpty = [] { Shortcut(); };
      onSleep = [] { CopyText(L"user"); onGet = [] { ++sequence; onLock = [] { ++sequence; }; }; };
      Check(Run(e, s) && s.text == L"user" && HasBytes(CF_UNICODETEXT, Bytes(L"user")),
            "user candidate survives sequence bump caused by our locked read"); }
    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s;
      onEmpty = [] { Shortcut(); };
      onSleep = [] { CopyText(L"user"); onGet = [] { sequence = 0; }; };
      Check(Run(e, s) && s.text == L"user" && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "unknown sequence during locked read cannot establish final stability"); }
    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s;
      onEmpty = [] { Shortcut(); };
      onSleep = [] { CopyText(L"user"); onGet = [] { onClose = [] { CopyText(L"external"); }; }; };
      Check(Run(e, s) && s.text == L"user" && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "external write after reading user candidate invalidates sticky preservation"); }
    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s;
      onEmpty = [] { Shortcut(); };
      onSleep = [] { CopyText(L"user"); onOpen = [] { ++sequence; }; };
      Check(Run(e, s) && s.text == L"user" && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "opening sequence different from unlocked candidate forces restoration"); }
    for (DWORD candidate : {DWORD{0}, DWORD{0x80000003u}})
    {
        Reset(); CopyText(L"old"); copyOnSend = false;
        SelectionEngine e; TextSelectionInfo s;
        onEmpty = [] { Shortcut(); };
        onSleep = [=] { CopyText(L"user"); sequence = candidate; };
        Check(Run(e, s) && s.text == L"user" && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
              "unknown or half-range ambiguous candidate returns text but restores original");
    }
    Reset(); CopyText(L"old"); sequence = 0xfffffffdu; copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s;
      onEmpty = [] { Shortcut(); };
      onSleep = [] { CopyText(L"user"); sequence = 1; };
      Check(Run(e, s) && s.text == L"user" && HasBytes(CF_UNICODETEXT, Bytes(L"user")),
            "strictly later user candidate remains valid across DWORD wraparound"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s;
      onSleep = [] { onClose = [] { failClose = true; }; };
      Check(Run(e, s) && s.text == L"selected" && e.clipboardSequence_ != sequence &&
                HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "engine restore Close failure keeps text without publishing success"); }
    Reset();
    { SelectionEngine e; TextSelectionInfo s;
      Check(Run(e, s) && s.text == L"selected" && clipboard.empty(), "empty original restored empty");
      Check(e.clipboardSequence_ == sequence, "successful empty restoration publishes final sequence"); }

    for (const auto shortcut : {std::pair<DWORD, DWORD>{VK_LCONTROL, 'C'}, {VK_RCONTROL, 'X'},
                                {VK_RCONTROL, VK_INSERT}, {VK_RSHIFT, VK_DELETE}})
    {
        Reset(); CopyText(L"old"); SelectionEngine e; TextSelectionInfo s;
        onSleep = [=] { Shortcut(shortcut.first, shortcut.second); CopyText(L"user"); };
        Check(Run(e, s) && HasBytes(CF_UNICODETEXT, Bytes(L"user")),
              "physical copy/cut keydown followed by sequence preserves user content");
        Check(e.clipboardSequence_ != sequence, "preserved user write is not published as our restoration");
    }
    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s; onSleep = [] { Shortcut(); };
      Check(!Run(e, s) && sentInputs.size() == 1 && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "pending copy with no write prevents second injection and restores original"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s; onEmpty = [] { Shortcut(); };
      Check(!Run(e, s) && sentInputs.empty() && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "pending copy before first injection stops all synthetic input"); }

    for (int failuresAtPhase : {1, 2, 3})
    {
        Reset(); CopyText(L"old"); SelectionEngine e; TextSelectionInfo s;
        failOpenAt = [=](int call) { return call == failuresAtPhase; };
        Check(Run(e, s) && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
              "backup/read/restore transient Open failure retries successfully");
        Check(waits.size() == 3 && openCalls == 4, "one Open retry adds only its separate five-ms gap");
    }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s;
      onSleep = [] { Shortcut(); CopyText(L"user"); failOpens = 5; };
      Check(!Run(e, s) && HasBytes(CF_UNICODETEXT, Bytes(L"user")),
            "failed first observe does not latch; later opened finish confirms user write"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s; onClose = [] { onClose = [] { failOpens = 5; }; };
      Check(Run(e, s) && s.text == L"selected" && openCalls == 7,
            "five failed restore opens remain bounded and retain already-read text");
      Check(e.clipboardSequence_ != sequence, "failed restore does not publish an unperformed restore"); }

    for (int mode : {0, 1, 2})
    {
        Reset(); auto backup = TwoFormats(); ClipboardRestoreResult result;
        if (mode == 0) failSetCount = 1;
        if (mode == 1) failAllocCall = 1;
        if (mode == 2) failLockCall = 1;
        Check(!RestoreClipboard(backup, false, &result) && result.status == ClipboardRestoreStatus::Partial &&
                  result.failedFormats == 1 && result.restoredFormats == 1 && HasBytes(0xc101, {3, 4}),
              "Set/Alloc/Lock failure reports partial and still restores later format");
    }
    Reset();
    { auto backup = TwoFormats(); ClipboardRestoreResult result; failOpens = 1;
      Check(!RestoreClipboard(backup, false, &result) && result.status == ClipboardRestoreStatus::OpenFailed,
            "restore reports OpenFailed precisely"); }
    Reset();
    { auto backup = TwoFormats(); ClipboardRestoreResult result; failEmpty = true;
      Check(!RestoreClipboard(backup, false, &result) && result.status == ClipboardRestoreStatus::EmptyFailed,
            "restore reports EmptyFailed precisely"); }
    Reset();
    { auto backup = TwoFormats(); ClipboardRestoreResult result; failClose = true;
      Check(!RestoreClipboard(backup, false, &result) && result.status == ClipboardRestoreStatus::CloseFailed,
            "restore reports CloseFailed precisely"); }
    Reset();
    { auto backup = TwoFormats(); backup.formats.insert(backup.formats.begin(), {CF_ENHMETAFILE, {7, 8}, 2});
      ClipboardRestoreResult result; failEmf = true;
      Check(!RestoreClipboard(backup, false, &result) && result.failedFormats == 1 && result.restoredFormats == 2,
            "EMF reconstruction failure is counted without dropping HGLOBAL formats"); }
    Reset();
    { auto backup = TwoFormats(); backup.formats.insert(backup.formats.begin(), {CF_ENHMETAFILE, {7, 8}, 2});
      ClipboardRestoreResult result; failedSetFormat = CF_ENHMETAFILE;
      Check(!RestoreClipboard(backup, false, &result) && result.failedFormats == 1 && wrongFrees == 0,
            "failed EMF Set uses DeleteEnhMetaFile and continues"); }
    Reset();
    { auto backup = TwoFormats(); allocationExtra = 8;
      Check(RestoreClipboard(backup) && HasBytes(CF_DIB, {1, 2}), "rounded GlobalAlloc capacity remains accepted"); }
    Reset();
    { ClipboardBackup backup; backup.isEmpty = false; ClipboardRestoreResult result; CopyText(L"injected");
      Check(!RestoreClipboard(backup, false, &result) && clipboard.empty() && result.status == ClipboardRestoreStatus::Partial,
            "nonempty unbackupable original still clears injection and reports partial"); }

    for (UINT format : {CF_DIB, CF_ENHMETAFILE, UINT{0xc001}, UINT{0xc101}, UINT{15}, CF_PRIVATEFIRST})
    {
        Reset(); Copy(format, {1, 2, 3});
        clipboard[CF_BITMAP] = NewBlock({4});
        clipboard[CF_PALETTE] = NewBlock({5});
        clipboard[CF_METAFILEPICT] = NewBlock({6});
        SelectionEngine e; TextSelectionInfo s;
        Check(Run(e, s) && s.text == L"selected", "images/EMF/Office/Explorer/unsupported formats never reject retrieval");
        Check(format == CF_PRIVATEFIRST ? clipboard.empty() : HasBytes(format, {1, 2, 3}),
              "skip-and-restore retains all backupable formats");
    }
    Reset(); CopyText(std::wstring(1, static_cast<wchar_t>(0xdc00)));
    { std::wstring text; Check(ReadClipboard(text) && text.size() == 1,
                              "main-compatible read does not reject lone surrogate"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s; onSleep = [] { activity->RecordKey(true, 'A', 0, 0); };
      Check(Run(e, s) && HasBytes(CF_UNICODETEXT, Bytes(L"old")), "ordinary input does not revoke restoration"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s; onSleep = [] { CopyText(L"staged second write"); };
      Check(Run(e, s) && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "second staged write without shortcut restores original regardless of owner"); }
    for (bool hooks : {false, true})
    {
        Reset(hooks); CopyText(L"old"); SelectionEngine e; TextSelectionInfo s;
        onSleep = [] { activity->Invalidate(); stopping = true; };
        Check(!Run(e, s) && HasBytes(CF_UNICODETEXT, Bytes(L"old")), "inactive hooks or stop still restore original");
    }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s;
      onSleep = [] { Shortcut(); CopyText(L"user"); onClose = [] { activity->Invalidate(); stopping = true; }; };
      Check(Run(e, s) && HasBytes(CF_UNICODETEXT, Bytes(L"user")), "complete observed copy remains preserved after stop"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s; onSleep = [] { failSetCount = 1; };
      Check(Run(e, s) && s.text == L"selected", "already-read text survives partial restore failure"); }
    Reset(); CopyText(L"old"); sameThread = true;
    { SelectionEngine e; TextSelectionInfo s;
      Check(!Run(e, s) && openCalls == 0 && sentInputs.empty(), "calling-thread guard precedes clipboard access"); }

    Reset(); CopyText(L"old"); copyOnSend = false;
    { SelectionEngine e; TextSelectionInfo s;
      Check(!Run(e, s) && sentInputs.size() == 2 && waits == std::vector<DWORD>(56, 5) &&
                injectionWaitOffsets == std::vector<size_t>({0, 20}),
            "unchanged CtrlInsert 20 and CtrlC 36 five-ms polls"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s;
      Check(Run(e, s) && waits == std::vector<DWORD>(2, 5), "unchanged ten-ms settle uses real production Sleep helper");
      Check(sentInputs.size() == 1 && sentInputs[0].size() == 4, "CtrlInsert retains symmetric modifier sequence");
      bool tagged = true;
      for (const auto &input : sentInputs[0]) tagged = tagged && input.ki.dwExtraInfo == ClipboardActivityInputTag();
      Check(tagged, "every ordinary synthetic input has own tag"); }
    Reset(); CopyText(L"old"); held[VK_CONTROL] = true;
    { SelectionEngine e; TextSelectionInfo s; e.config.triggeredByUser = false; e.clipboardSequence_ = sequence;
      Check(!Run(e, s) && waits == std::vector<DWORD>(40, 5) && sentInputs.empty(),
            "unchanged five by forty-ms automatic intent wait"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s; s.programName = L"delay"; e.config.includeClipboardDelayRead = {"delay"};
      Check(Run(e, s) && waits == std::vector<DWORD>(29, 5) && sentInputs.size() == 1 &&
                sentInputs[0][1].ki.wVk == 'C' && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "delay-list keeps restored content and CtrlC-only 135 plus ten-ms waits");
      bool untagged = true;
      for (const auto &input : sentInputs[0]) untagged = untagged && input.ki.dwExtraInfo == 0;
      Check(untagged, "legacy input default tag remains zero"); }
    Reset(); held[VK_CONTROL] = held[VK_MENU] = held[VK_SHIFT] = true;
    { SelectionEngine e; e.SendCopyKey(CopyKeyType::CtrlC);
      const auto &v = sentInputs[0];
      Check(v.size() == 4 && v[0].ki.wVk == VK_MENU && v[0].ki.dwFlags == KEYEVENTF_KEYUP &&
                v[1].ki.wVk == VK_SHIFT && v[1].ki.dwFlags == KEYEVENTF_KEYUP &&
                v[2].ki.wVk == 'C' && v[3].ki.dwFlags == KEYEVENTF_KEYUP,
            "legacy held Ctrl and released Alt/Shift input order unchanged"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s; e.config.triggeredByUser = false;
      failOpens = 1;
      Check(Run(e, s) && s.text == L"old" && sentInputs.empty() && emptyCalls == 0 &&
                waits == std::vector<DWORD>(1, 5), "automatic direct intent read uses the same bounded open retry"); }
    Reset(); CopyText(L"old"); failOpens = 5;
    { SelectionEngine e; TextSelectionInfo s;
      Check(!Run(e, s) && openCalls == 5 && emptyCalls == 0 && HasBytes(CF_UNICODETEXT, Bytes(L"old")) &&
                waits == std::vector<DWORD>(4, 5), "persistent backup contention is bounded without mutating clipboard"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s;
      onSleep = [] { CopyText(L"before key"); Shortcut(); };
      Check(Run(e, s) && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "clipboard change before key with no later write restores original"); }
    Reset();
    { SelectionEngine e; TextSelectionInfo s; onSleep = [] { activity->Invalidate(); stopping = true; };
      Check(!Run(e, s) && clipboard.empty(), "stop restores an originally empty clipboard to empty"); }
    Reset(); CopyText(L"old");
    { SelectionEngine e; TextSelectionInfo s; onSleep = [] { failEmpty = true; };
      Check(Run(e, s) && s.text == L"selected" && HasBytes(CF_UNICODETEXT, Bytes(L"old")),
            "restore Empty failure still attempts later Set and returns already-read text"); }
    Reset();
    { auto backup = TwoFormats(); ClipboardRestoreResult result; OpenClipboard(nullptr); failEmpty = true;
      Check(!RestoreClipboard(backup, true, &result) && result.status == ClipboardRestoreStatus::EmptyFailed &&
                result.restoredFormats == 2 && result.failedFormats == 0,
            "already-open restoration reports EmptyFailed while counting successful subsequent Set calls");
      CloseClipboard(); }
    Clear();
    Check(liveBlocks == 0 && activeLocks == 0 && wrongFrees == 0 && !opened,
          "all allocated handles and locks released with matching ownership");
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
