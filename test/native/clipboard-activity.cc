/**
 * Regression: old activity tracking treated ordinary/injected input as restore revocation,
 * lost evidence on stop, and was not executed under npm test. Check remains active in Release.
 */
#include "clipboard-restore/windows.h"

#include <iostream>
#include <map>

#include "../../src/windows/core/clipboard_activity.h"

static std::map<int, bool> held;
static DWORD keySequence = 10;
static int sequenceQueries = 0, asyncQueries = 0;
DWORD GetClipboardSequenceNumber()
{
    ++sequenceQueries;
    return keySequence;
}
short GetAsyncKeyState(int key)
{
    ++asyncQueries;
    return held[key] ? static_cast<short>(0x8000) : 0;
}
static int failures = 0;
static void Check(bool condition, const char *name)
{
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    if (!condition)
        ++failures;
}
static void Key(ClipboardActivityState &state, DWORD key, DWORD sequence = 10, bool down = true,
                DWORD flags = 0, ULONG_PTR tag = 0)
{
    keySequence = sequence;
    if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL || key == VK_SHIFT ||
        key == VK_LSHIFT || key == VK_RSHIFT || key == VK_LMENU || key == VK_RMENU || key == VK_LWIN || key == VK_RWIN)
    {
        if ((flags & LLKHF_INJECTED) == 0 && tag != ClipboardActivityInputTag())
            held[key] = down;
    }
    state.RecordKey(down, key, flags, tag);
}

int main()
{
    {
        held.clear();
        ClipboardActivityState state;
        state.Activate();
        auto token = state.BeginTransaction();
        sequenceQueries = asyncQueries = 0;
        Key(state, VK_LCONTROL);
        Key(state, 'A');
        Key(state, 'A', 10, false);
        Key(state, 'C', 10, false);
        Check(sequenceQueries == 0 && asyncQueries == 0,
              "non-copy keys and modifier bookkeeping avoid clipboard and async queries");
        Key(state, 'C');
        Check(state.CaptureIntent(token).pendingCopy && sequenceQueries == 1 && asyncQueries != 0,
              "copy keydown alone queries sequence and physical modifier state");
    }
    // 0497638 trusted a seeded modifier after its physical state had been lost.
    {
        held.clear();
        ClipboardActivityState state;
        held[VK_RCONTROL] = true;
        state.Activate();
        held.clear();
        auto token = state.BeginTransaction();
        Key(state, 'C');
        Check(!state.CaptureIntent(token).pendingCopy, "E3 stale tracked Ctrl rejects plain C after lost release");
    }
    for (const auto shortcut : {std::pair<DWORD, DWORD>{VK_LCONTROL, 'C'}, {VK_RCONTROL, 'X'},
                                {VK_RCONTROL, VK_INSERT}, {VK_LSHIFT, VK_DELETE}})
    {
        held.clear();
        ClipboardActivityState state;
        state.Activate();
        auto token = state.BeginTransaction();
        Key(state, shortcut.first);
        Key(state, shortcut.second);
        Check(state.CaptureIntent(token).pendingCopy && !state.CaptureIntent(token).userCopyObserved,
              "standard physical shortcut immediately latches pending only");
        Key(state, shortcut.second, 10, false);
        Key(state, shortcut.first, 10, false);
        state.ObserveSequence(token, 11, 9);
        Check(state.CaptureIntent(token).userCopyObserved, "post-key sequence confirms released physical shortcut");
        Key(state, shortcut.first, 11);
        Key(state, shortcut.second, 11);
        state.ObserveSequence(token, 11, 9);
        Check(state.CaptureIntent(token).userCopyObserved, "repeated shortcut never overwrites first sticky evidence");
        state.Invalidate();
        Check(state.CaptureIntent(token).userCopyObserved, "stop retains previously complete user-copy evidence");
        state.EndTransaction(token);
        Check(!state.CaptureIntent(token).pendingCopy, "ended transaction discards all input evidence");
    }

    for (DWORD modifier : {VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN})
    {
        held.clear();
        ClipboardActivityState state;
        state.Activate();
        auto token = state.BeginTransaction();
        Key(state, VK_LCONTROL);
        Key(state, modifier);
        for (DWORD key : {DWORD{'C'}, DWORD{'X'}, DWORD{VK_INSERT}})
            Key(state, key);
        Check(!state.CaptureIntent(token).pendingCopy, "extra Shift/Alt/Win rejects Ctrl shortcuts");
    }
    for (DWORD modifier : {VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN})
    {
        held.clear();
        ClipboardActivityState state;
        state.Activate();
        auto token = state.BeginTransaction();
        Key(state, VK_RSHIFT);
        Key(state, modifier);
        Key(state, VK_DELETE);
        Check(!state.CaptureIntent(token).pendingCopy, "extra Ctrl/Alt/Win rejects Shift+Delete");
    }
    for (const auto event : {std::pair<DWORD, ULONG_PTR>{LLKHF_INJECTED, 0},
                             {0, ClipboardActivityInputTag()}, {LLKHF_INJECTED, 123}})
    {
        held.clear();
        ClipboardActivityState state;
        state.Activate();
        auto token = state.BeginTransaction();
        Key(state, VK_LCONTROL, 10, true, event.first, event.second);
        Key(state, 'C');
        Check(!state.CaptureIntent(token).pendingCopy, "injected Ctrl cannot turn physical C into user Ctrl+C");
        Key(state, VK_LCONTROL);
        Key(state, 'C', 10, true, event.first, event.second);
        Check(!state.CaptureIntent(token).pendingCopy, "own tag or any injected shortcut cannot form evidence");
    }
    {
        held.clear();
        ClipboardActivityState state;
        state.Activate();
        auto token = state.BeginTransaction();
        Key(state, 'C');
        Key(state, 'X');
        Key(state, VK_INSERT);
        Key(state, VK_DELETE);
        Key(state, VK_LCONTROL);
        Key(state, 'C', 10, false);
        Key(state, 'A');
        Check(!state.CaptureIntent(token).pendingCopy, "ordinary key and key-up do not form copy evidence");
        Key(state, VK_LCONTROL, 10, false);
        Key(state, 'C');
        Check(!state.CaptureIntent(token).pendingCopy, "released modifiers cannot leak into a later key");
    }
    {
        held.clear();
        ClipboardActivityState state;
        held[VK_RCONTROL] = true;
        state.Activate();
        held.clear();
        auto token = state.BeginTransaction();
        Key(state, VK_RCONTROL, 10, false, LLKHF_INJECTED, ClipboardActivityInputTag());
        Key(state, 'C');
        Check(!state.CaptureIntent(token).pendingCopy, "async release rejects stale seed despite ignored injected release");
    }
    {
        held.clear();
        ClipboardActivityState state;
        state.Activate();
        auto token = state.BeginTransaction();
        state.ObserveSequence(token, 11, 9);
        Key(state, VK_LCONTROL, 11);
        Key(state, 'C', 11);
        state.ObserveSequence(token, 11, 9);
        Check(!state.CaptureIntent(token).userCopyObserved, "sequence before the key cannot prove user copy");
        state.ObserveSequence(token, 12, 12);
        Check(!state.CaptureIntent(token).userCopyObserved, "own clear sequence cannot prove user copy");
        state.ObserveSequence(token, 0, 12);
        Check(!state.CaptureIntent(token).userCopyObserved, "unknown sequence fails closed");
        state.Invalidate();
        state.ObserveSequence(token, 13, 12);
        Check(!state.CaptureIntent(token).userCopyObserved, "stop prevents new confirmation of pending evidence");
        state.EndTransaction(token);
    }
    {
        held.clear();
        ClipboardActivityState state;
        state.Activate();
        auto old = state.BeginTransaction();
        Key(state, VK_LCONTROL);
        Key(state, 'C');
        state.EndTransaction(old);
        auto current = state.BeginTransaction();
        state.ObserveSequence(old, 11, 9);
        Check(!state.CaptureIntent(current).pendingCopy && !state.CaptureIntent(old).userCopyObserved,
              "a new transaction cannot inherit a released old shortcut or old observation");
        state.EndTransaction(old);
        Key(state, 'X', 11);
        Check(state.CaptureIntent(current).pendingCopy, "an old EndTransaction cannot disarm a current token");
    }
    {
        held.clear();
        ClipboardActivityState state;
        auto token = state.BeginTransaction();
        Key(state, VK_LCONTROL);
        Key(state, 'C');
        state.ObserveSequence(token, 11, 9);
        Check(!state.CaptureIntent(token).pendingCopy, "inactive or partially installed hooks default to restore");
        held.clear();
        ClipboardActivityState restarted;
        restarted.Activate();
        auto next = restarted.BeginTransaction();
        Key(restarted, 'C');
        Check(!restarted.CaptureIntent(next).pendingCopy, "fresh start cannot inherit previous physical modifiers");
    }
    {
        held.clear();
        ClipboardActivityState state;
        state.Activate();
        auto token = state.BeginTransaction();
        Key(state, VK_CONTROL, 0);
        Key(state, 'C', 0);
        state.ObserveSequence(token, 11, 9);
        Check(state.CaptureIntent(token).pendingCopy && !state.CaptureIntent(token).userCopyObserved,
              "unknown key sequence still stops injection but cannot preserve clipboard");
    }
    for (DWORD modifier : {VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN})
    {
        held.clear();
        ClipboardActivityState state;
        held[modifier] = true;
        state.Activate();
        held.clear();
        auto token = state.BeginTransaction();
        Key(state, modifier, 10, false);
        if (modifier != VK_LCONTROL && modifier != VK_RCONTROL)
            Key(state, VK_RCONTROL);
        Key(state, 'C');
        Check(state.CaptureIntent(token).pendingCopy == (modifier != VK_LCONTROL && modifier != VK_RCONTROL),
              "physical release clears each seeded left/right modifier deterministically");
    }
    {
        held.clear();
        ClipboardActivityState state;
        state.Activate();
        auto token = state.BeginTransaction();
        Key(state, VK_LCONTROL);
        Key(state, 'C', 10);
        Key(state, 'C', 11);
        state.ObserveSequence(token, 11, 9);
        Check(state.CaptureIntent(token).userCopyObserved, "repeat before first observation cannot replace first key sequence");
    }
    {
        held.clear();
        ClipboardActivityState state;
        state.Activate();
        auto token = state.BeginTransaction();
        Key(state, VK_LCONTROL);
        Key(state, VK_RCONTROL);
        Key(state, VK_LCONTROL, 10, false);
        Key(state, 'C');
        Check(state.CaptureIntent(token).pendingCopy, "releasing left Ctrl retains a physically held right Ctrl");
    }
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
