/**
 * @file
 * @brief Bounded, allocation-free evidence for a user's clipboard shortcut.
 */
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <initializer_list>

/** @return Process-local tag applied to every input of an ordinary clipboard fallback. */
inline ULONG_PTR ClipboardActivityInputTag()
{
    static const char tag = 0;
    return reinterpret_cast<ULONG_PTR>(&tag);
}

/** Identifies one transaction serialized by the existing process clipboard mutex. */
using ClipboardTransactionToken = uint32_t;

/**
 * @param[in] sequence Candidate sequence number.
 * @param[in] previous Earlier sequence number to compare against.
 * @return True only for a known, strictly later sequence within half the DWORD range.
 * @note Unsigned subtraction handles wraparound; zero and half-range ambiguity fail closed.
 */
inline bool ClipboardSequenceIsLater(DWORD sequence, DWORD previous)
{
    const uint32_t distance = static_cast<uint32_t>(sequence - previous);
    return sequence != 0 && previous != 0 && distance != 0 && distance < 0x80000000u;
}

/** Evidence is sticky until EndTransaction, including after hook invalidation. */
struct ClipboardCopyIntent
{
    bool pendingCopy = false;
    bool userCopyObserved = false;
};

/**
 * @brief Tracks physical copy/cut shortcuts without blocking the low-level hook.
 * @note Only the clipboard-mutex holder may begin/end transactions or observe a sequence.
 * The hook thread alone updates modifiers. Injected events never update physical state.
 * @warning The engine may preserve a candidate only when no copy input was injected.
 * Sequence ordering cannot identify which injected or user write changed the clipboard.
 * Do not add an owner/process filter: it rejects focused applications when retrieval
 * targets the window under the mouse in another application.
 */
class ClipboardActivityState
{
  public:
    /**
     * @brief Seed held modifiers before the installed hooks begin dispatching events.
     * @note Each start owns a fresh state; invalidated states are never reactivated.
     */
    void Activate()
    {
        uint32_t modifiers = 0;
        for (int key : {VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN})
        {
            if ((GetAsyncKeyState(key) & 0x8000) != 0)
                modifiers |= ModifierBit(key);
        }
        modifiers_.store(modifiers, std::memory_order_release);
        active_.store(true, std::memory_order_release);
    }

    /** @brief Disable new evidence while retaining any complete user-copy evidence. */
    void Invalidate()
    {
        active_.store(false, std::memory_order_seq_cst);
        transaction_.fetch_and(~Trusted, std::memory_order_seq_cst);
    }

    /** @return Fresh token; callers must already own the process clipboard mutex. */
    ClipboardTransactionToken BeginTransaction()
    {
        generation_ = (generation_ + 1) & GenerationMask;
        if (generation_ == 0)
            generation_ = 1;
        transaction_.store((uint64_t{generation_} << 32) | (active_.load(std::memory_order_seq_cst) ? Trusted : 0),
                           std::memory_order_seq_cst);
        // Invalidate can race the store above. Never revive trust after it completes.
        if (!active_.load(std::memory_order_seq_cst))
            transaction_.fetch_and(~Trusted, std::memory_order_seq_cst);
        return generation_;
    }

    /** @param[in] token Current transaction; disarm before releasing the clipboard mutex. */
    void EndTransaction(ClipboardTransactionToken token)
    {
        if (Matches(transaction_.load(std::memory_order_acquire), token))
            transaction_.store(0, std::memory_order_release);
    }

    /**
     * @brief Record a physical key event before retrieval/TSFN filtering.
     * @param[in] down True for keydown, false for keyup.
     * @param[in] key Windows virtual key code.
     * @param[in] flags Low-level keyboard flags, including LLKHF_INJECTED.
     * @param[in] extraInfo Windows input tag; our own tag is excluded independently.
     * @note One CAS bounds hook work and prevents an old event entering a new transaction.
     */
    void RecordKey(bool down, DWORD key, DWORD flags, ULONG_PTR extraInfo)
    {
        // Capture the transaction before processing this event. A callback paused across
        // Begin/End must not attach its old key to whichever transaction is current later.
        uint64_t current = transaction_.load(std::memory_order_acquire);
        if (!active_.load(std::memory_order_acquire) || (flags & LLKHF_INJECTED) != 0 ||
            extraInfo == ClipboardActivityInputTag())
            return;

        const uint32_t bit = ModifierBit(key);
        if (bit != 0)
        {
            if (down)
                modifiers_.fetch_or(bit, std::memory_order_acq_rel);
            else
                modifiers_.fetch_and(~bit, std::memory_order_acq_rel);
            return;
        }
        if (!down || (key != 'C' && key != 'X' && key != VK_INSERT && key != VK_DELETE))
            return;

        const DWORD sequence = GetClipboardSequenceNumber();
        const uint32_t modifiers = modifiers_.load(std::memory_order_acquire);
        // A missed physical keyup must not turn an ordinary key into copy evidence.
        const bool ctrl = (modifiers & CtrlMask) != 0;
        const bool shift = (modifiers & ShiftMask) != 0;
        const bool ctrlHeld =
            ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_LCONTROL) | GetAsyncKeyState(VK_RCONTROL)) & 0x8000) !=
            0;
        const bool shiftHeld =
            ((GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_LSHIFT) | GetAsyncKeyState(VK_RSHIFT)) & 0x8000) != 0;
        const bool altOrWin = (modifiers & (AltMask | WinMask)) != 0;
        const bool copy = ctrl && ctrlHeld && !shift && !altOrWin && (key == 'C' || key == 'X' || key == VK_INSERT);
        const bool cut = shift && shiftHeld && !ctrl && !altOrWin && key == VK_DELETE;
        if (!copy && !cut)
            return;

        if ((current & Trusted) != 0 && (current & Pending) == 0 && (current & TokenMask) != 0)
            transaction_.compare_exchange_strong(current, current | Pending | sequence, std::memory_order_acq_rel);
    }

    /**
     * @brief Confirm a candidate observed while unlocked, then verified on opening.
     * @param[in] token Current transaction.
     * @param[in] sequence Candidate matching the sequence immediately after OpenClipboard.
     * @param[in] clearSequence Sequence sampled after closing our clear.
     * @note The engine must exclude transactions with injected input and verify stability.
     */
    void ObserveSequence(ClipboardTransactionToken token, DWORD sequence, DWORD clearSequence)
    {
        uint64_t current = transaction_.load(std::memory_order_acquire);
        if (!Matches(current, token) || (current & (Trusted | Pending)) != (Trusted | Pending) ||
            !active_.load(std::memory_order_acquire))
            return;
        const DWORD keySequence = static_cast<DWORD>(current);
        if (!ClipboardSequenceIsLater(sequence, keySequence) || !ClipboardSequenceIsLater(sequence, clearSequence))
            return;
        // Invalidate clears Trusted atomically, so a racing confirmation fails closed.
        transaction_.compare_exchange_strong(current, current | Observed, std::memory_order_acq_rel);
    }

    /** @param[in] token Current transaction. @return Its sticky pending and confirmed evidence. */
    ClipboardCopyIntent CaptureIntent(ClipboardTransactionToken token) const
    {
        const uint64_t current = transaction_.load(std::memory_order_acquire);
        if (!Matches(current, token))
            return {};
        return {(current & Pending) != 0, (current & Observed) != 0};
    }

  private:
    static constexpr uint32_t GenerationMask = 0x1fffffff;
    static constexpr uint64_t TokenMask = uint64_t{GenerationMask} << 32;
    static constexpr uint64_t Trusted = uint64_t{1} << 61;
    static constexpr uint64_t Observed = uint64_t{1} << 62;
    static constexpr uint64_t Pending = uint64_t{1} << 63;
    static constexpr uint32_t CtrlMask = 7, ShiftMask = 7 << 3, AltMask = 7 << 6, WinMask = 3 << 9;

    static bool Matches(uint64_t state, ClipboardTransactionToken token)
    {
        return token != 0 && ((state & TokenMask) >> 32) == token;
    }

    static uint32_t ModifierBit(DWORD key)
    {
        switch (key)
        {
            case VK_CONTROL:
                return 1;
            case VK_LCONTROL:
                return 2;
            case VK_RCONTROL:
                return 4;
            case VK_SHIFT:
                return 1 << 3;
            case VK_LSHIFT:
                return 1 << 4;
            case VK_RSHIFT:
                return 1 << 5;
            case VK_MENU:
                return 1 << 6;
            case VK_LMENU:
                return 1 << 7;
            case VK_RMENU:
                return 1 << 8;
            case VK_LWIN:
                return 1 << 9;
            case VK_RWIN:
                return 1 << 10;
            default:
                return 0;
        }
    }

    std::atomic<bool> active_{false};
    std::atomic<uint32_t> modifiers_{0};
    std::atomic<uint64_t> transaction_{0};
    uint32_t generation_ = 0;
};
