/**
 * @file
 * @brief MTA worker lifecycle, COM ownership, TSFN handoff, and bounded shutdown.
 *
 * Automatic retrieval moved here for issue #17: a UIA provider call made from an
 * Electron main thread could block while an owned file dialog was active. The public
 * getCurrentSelection() API intentionally remains synchronous on its calling thread.
 *
 * Copyright (c) 2025 0xfullex (https://github.com/0xfullex/selection-hook)
 * Licensed under the MIT License
 */
#define WIN32_LEAN_AND_MEAN 1

#include "worker.h"

#include <UIAutomation.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <memory>

#include "../lib/utils.h"

namespace
{
// Start must not return until the worker can guarantee a valid MTA/UIA context.
constexpr DWORD workerReadyTimeoutMs = 5000;
// Accessibility providers are external code and may never return. Do not reproduce the
// original main-thread freeze by joining such a worker without a bound.
constexpr DWORD workerStopTimeoutMs = 2000;
// Controlled abandonment is exceptional and intentionally bounded to avoid unbounded
// process-lifetime threads if a provider repeatedly hangs.
constexpr unsigned int maxAbandonedWorkers = 4;
std::atomic<unsigned int> abandonedWorkerCount{0};

class AutoGateGuard
{
  public:
    explicit AutoGateGuard(const std::shared_ptr<RetrievalGates> &gates) : gates_(gates) {}
    ~AutoGateGuard()
    {
        // Gate release belongs to native retrieval completion, not the JS callback.
        // TSFN abort/closing paths may legitimately discard the callback.
        if (gates_)
            gates_->autoRetrievalInFlight.store(false, std::memory_order_release);
    }

  private:
    std::shared_ptr<RetrievalGates> gates_;
};

const char *StageName(RetrievalStage stage)
{
    switch (stage)
    {
        case RetrievalStage::Idle:
            return "idle";
        case RetrievalStage::ProgramName:
            return "program-name";
        case RetrievalStage::UIAutomation:
            return "uia";
        case RetrievalStage::Accessible:
            return "msaa";
        case RetrievalStage::Clipboard:
            return "clipboard";
        case RetrievalStage::ClipboardPoll:
            return "clipboard-poll";
        case RetrievalStage::Complete:
            return "complete";
    }
    return "unknown";
}
}  // namespace

struct SelectionWorker::State
{
    // Lock order when both are required: napiMutex, then mutex. ThreadMain must release
    // mutex before acquiring napiMutex. Inverting this order can deadlock Stop().

    // Request, readiness, and completion synchronization.
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable readyCv;
    std::condition_variable completionCv;
    std::unique_ptr<SelectionRequest> pendingRequest;
    bool ready = false;
    bool completed = false;
    // napi_create_threadsafe_function contributes the main-thread ownership. Start()
    // separately acquires one reference for the worker.
    bool mainTsfnOwned = true;
    HRESULT initializationResult = E_PENDING;

    // Serializes TSFN call/release with the transition to controlled abandonment.
    std::mutex napiMutex;
    std::atomic<bool> accepting{true};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> abandoned{false};
    std::atomic<napi_threadsafe_function> resultTsfn{nullptr};
    // Everything below is native/shared state that remains valid even if SelectionHook
    // is destroyed while an abandoned provider call is still blocked.
    std::shared_ptr<RetrievalGates> gates;
    std::atomic<RetrievalStage> stage{RetrievalStage::Idle};
    std::atomic<HWND> retrievalHwnd{NULL};
    std::atomic<ULONGLONG> retrievalStartedAt{0};
    uint64_t generation = 0;
};

SelectionWorker::~SelectionWorker()
{
    Stop();
}

bool SelectionWorker::Start(uint64_t generation, napi_threadsafe_function resultTsfn,
                            const std::shared_ptr<RetrievalGates> &gates, std::string &error)
{
    if (!resultTsfn || !gates)
    {
        error = "Invalid selection worker state";
        return false;
    }
    if (abandonedWorkerCount.load(std::memory_order_acquire) >= maxAbandonedWorkers)
    {
        napi_release_threadsafe_function(resultTsfn, napi_tsfn_abort);
        error = "Selection worker abandoned-thread limit reached";
        return false;
    }

    state_ = std::make_shared<State>();
    state_->generation = generation;
    state_->resultTsfn.store(resultTsfn, std::memory_order_release);
    state_->gates = gates;

    // The worker owns a distinct TSFN reference so normal shutdown can abort the main
    // producer while the native thread finishes its current retrieval.
    const napi_status acquireStatus = napi_acquire_threadsafe_function(resultTsfn);
    if (acquireStatus != napi_ok)
    {
        napi_release_threadsafe_function(resultTsfn, napi_tsfn_abort);
        state_.reset();
        error = "Failed to acquire selection result callback";
        return false;
    }

    try
    {
        thread_ = std::thread(&SelectionWorker::ThreadMain, state_, resultTsfn);
    }
    catch (...)
    {
        napi_release_threadsafe_function(resultTsfn, napi_tsfn_release);
        napi_release_threadsafe_function(resultTsfn, napi_tsfn_abort);
        state_.reset();
        error = "Failed to create selection MTA worker";
        return false;
    }

    // Do not report start success before CoInitializeEx and CUIAutomation creation have
    // succeeded on the actual worker thread.
    std::unique_lock<std::mutex> lock(state_->mutex);
    const bool ready = state_->readyCv.wait_for(lock, std::chrono::milliseconds(workerReadyTimeoutMs),
                                                [state = state_] { return state->ready; });
    const HRESULT initializationResult = state_->initializationResult;
    lock.unlock();
    if (!ready)
    {
        error = "Timed out initializing selection MTA worker";
        Stop();
        return false;
    }
    if (FAILED(initializationResult))
    {
        error = "Failed to initialize selection MTA worker COM/UIA";
        Stop();
        return false;
    }
    return true;
}

bool SelectionWorker::Enqueue(SelectionRequest request)
{
    const std::shared_ptr<State> state = state_;
    if (!state || !state->accepting.load(std::memory_order_acquire) ||
        state->stopRequested.load(std::memory_order_acquire))
        return false;

    // Reject rather than queue a backlog. The hook historically suppressed new input
    // while selection retrieval was in flight, and the auto gate preserves that rule.
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->accepting.load(std::memory_order_relaxed) || state->pendingRequest)
        return false;
    state->pendingRequest = std::make_unique<SelectionRequest>(std::move(request));
    state->cv.notify_one();
    return true;
}

void SelectionWorker::Stop()
{
    const std::shared_ptr<State> state = state_;
    if (!state)
        return;

    // First prevent new work, then discard a request that has not started. A running
    // engine observes stopRequested between stages and during clipboard polling.
    state->accepting.store(false, std::memory_order_release);
    state->stopRequested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->pendingRequest.reset();
    }
    state->gates->autoRetrievalInFlight.store(false, std::memory_order_release);
    state->cv.notify_all();

    // Clear and abort the externally callable TSFN handle before waiting. The napiMutex
    // makes this mutually exclusive with the worker's final nonblocking call.
    napi_threadsafe_function resultTsfn = nullptr;
    {
        std::lock_guard<std::mutex> apiLock(state->napiMutex);
        resultTsfn = state->resultTsfn.exchange(nullptr, std::memory_order_acq_rel);
        if (resultTsfn && state->mainTsfnOwned)
        {
            napi_release_threadsafe_function(resultTsfn, napi_tsfn_abort);
            state->mainTsfnOwned = false;
        }
    }

    // Normal path: bounded wait followed by a real join.
    std::unique_lock<std::mutex> lock(state->mutex);
    bool completed = state->completionCv.wait_for(lock, std::chrono::milliseconds(workerStopTimeoutMs),
                                                  [state] { return state->completed; });
    lock.unlock();
    if (completed)
    // Resolve the completion-timeout race while holding both locks used by completion
    // and TSFN dispatch. Once abandoned is set, the worker may only do native cleanup.
    {
        if (thread_.joinable())
            thread_.join();
        state_.reset();
        return;
    }

    {
        std::lock_guard<std::mutex> apiLock(state->napiMutex);
        std::lock_guard<std::mutex> stateLock(state->mutex);
        completed = state->completed;
        if (!completed)
            state->abandoned.store(true, std::memory_order_release);
    }
    if (completed)
    {
        if (thread_.joinable())
            thread_.join();
        state_.reset();
        return;
    }

    const ULONGLONG startedAt = state->retrievalStartedAt.load(std::memory_order_acquire);
    const ULONGLONG elapsed = startedAt == 0 ? 0 : GetTickCount64() - startedAt;
    fprintf(stderr,
            "[selection-hook] selection worker stop timed out: generation=%llu stage=%s hwnd=%p elapsed=%llu ms; "
            "entering controlled abandoned state\n",
            static_cast<unsigned long long>(state->generation), StageName(state->stage.load()),
            state->retrievalHwnd.load(), static_cast<unsigned long long>(elapsed));
    // A detached thread may resume after the Node environment and addon instance are
    // gone. Pin the DLL so its machine code remains mapped; shared State owns all data
    // the thread may still access.
    PinCurrentModule();
    abandonedWorkerCount.fetch_add(1, std::memory_order_acq_rel);
    if (thread_.joinable())
        thread_.detach();
    state_.reset();
}

void SelectionWorker::ThreadMain(const std::shared_ptr<State> &state, napi_threadsafe_function workerTsfn)
{
    // UIA client creation, every use, Release(), and CoUninitialize() all occur on this
    // dedicated MTA thread. Raw COM interfaces never cross the thread boundary.
    // This thread is created by the addon, so its apartment is fully controlled.
    // RPC_E_CHANGED_MODE is an initialization failure rather than an acceptable mode.
    HRESULT initializationResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = initializationResult == S_OK || initializationResult == S_FALSE;
    IUIAutomation *automation = nullptr;
    if (comInitialized)
    {
        // Keep CUIAutomation rather than switching to CUIAutomation8: proxy-provider
        // behavior can differ, which could silently change application compatibility.
        // Any replacement requires the complete UIA/MSAA/clipboard application matrix.
        initializationResult = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                                __uuidof(IUIAutomation), reinterpret_cast<void **>(&automation));
    }

#ifdef _DEBUG
    fprintf(stderr, "[selection-hook] selection MTA worker ready: thread=%lu hr=0x%08lx\n", GetCurrentThreadId(),
            static_cast<unsigned long>(initializationResult));
#endif

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->initializationResult = initializationResult;
        state->ready = true;
    }
    state->readyCv.notify_all();

    if (SUCCEEDED(initializationResult) && automation)
    {
        SelectionEngine engine(automation);
        while (true)
        {
            std::unique_ptr<SelectionRequest> request;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->cv.wait(lock,
                               [state]
                               {
                                   return state->stopRequested.load(std::memory_order_acquire) ||
                                          state->pendingRequest != nullptr;
                               });
                if (state->stopRequested.load(std::memory_order_acquire))
                    break;
                request = std::move(state->pendingRequest);
            }
            if (!request)
                continue;

            // Requests and results contain only copied values and borrowed handles.
            std::unique_ptr<SelectionWorkerResult> result = std::make_unique<SelectionWorkerResult>();
            result->sequence = request->sequence;
            result->generation = request->generation;
            result->detectionType = request->detectionType;
            state->retrievalHwnd.store(request->retrievalHwnd, std::memory_order_release);
            state->retrievalStartedAt.store(GetTickCount64(), std::memory_order_release);
            state->stage.store(RetrievalStage::Idle, std::memory_order_release);

            {
                // The guard clears the auto gate on every result, error, or stop path,
                // before any best-effort TSFN delivery is attempted.
                AutoGateGuard gateGuard(state->gates);
                DWORD endingClipboardSequence = request->config.clipboardSequence;
                // The HWND is a borrowed event-time value and can disappear before the
                // worker runs. Invalid handles preserve the existing no-selection result.
                if (!state->stopRequested.load(std::memory_order_acquire) && IsWindow(request->retrievalHwnd))
                {
                    result->found = engine.GetSelectedText(request->retrievalHwnd, result->selection, request->config,
                                                           ClipboardLockMode::InterruptibleWait, &state->stopRequested,
                                                           &state->stage, &endingClipboardSequence);
                }
                if (request->clipboardSequenceState)
                {
                    // Publish while the auto gate is still held. Revision checking keeps
                    // this older result from overwriting a newer mouse-down/sync value.
                    // Do not move this after the AutoGateGuard scope: a new retrieval may
                    // start as soon as the gate opens and must observe the final sequence.
                    request->clipboardSequenceState->PublishIfUnchanged(endingClipboardSequence,
                                                                        request->clipboardSequenceRevision);
                }

                // UIA coordinates take precedence. Mouse positions only fill the same
                // fallback levels used by the original synchronous event path.
                if (result->found && !IsTrimmedEmpty(result->selection.text))
                {
                    switch (request->detectionType)
                    {
                        case SelectionDetectType::Drag:
                            result->selection.mousePosStart = request->mouseDownPos;
                            result->selection.mousePosEnd = request->mouseUpPos;
                            if (result->selection.posLevel == SelectionPositionLevel::None)
                                result->selection.posLevel = SelectionPositionLevel::MouseDual;
                            break;
                        case SelectionDetectType::DoubleClick:
                            result->selection.mousePosStart = request->mouseUpPos;
                            result->selection.mousePosEnd = request->mouseUpPos;
                            if (result->selection.posLevel == SelectionPositionLevel::None)
                                result->selection.posLevel = SelectionPositionLevel::MouseSingle;
                            break;
                        case SelectionDetectType::ShiftClick:
                            result->selection.mousePosStart = request->previousMouseUpPos;
                            result->selection.mousePosEnd = request->mouseUpPos;
                            if (result->selection.posLevel == SelectionPositionLevel::None)
                                result->selection.posLevel = SelectionPositionLevel::MouseDual;
                            break;
                        case SelectionDetectType::None:
                            break;
                    }
                }
            }

            const ULONGLONG elapsed = GetTickCount64() - state->retrievalStartedAt.load(std::memory_order_acquire);
#ifdef _DEBUG
            fprintf(stderr,
                    "[selection-hook] selection MTA retrieval: generation=%llu sequence=%llu thread=%lu "
                    "stage=%s found=%d elapsed=%llu ms\n",
                    static_cast<unsigned long long>(request->generation),
                    static_cast<unsigned long long>(request->sequence), GetCurrentThreadId(),
                    StageName(state->stage.load()), result->found ? 1 : 0, static_cast<unsigned long long>(elapsed));
#else
            if (elapsed > 2000)
                fprintf(stderr, "[selection-hook] selection retrieval exceeded 2000 ms: elapsed=%llu ms\n",
                        static_cast<unsigned long long>(elapsed));
#endif

            if (!result->found || IsTrimmedEmpty(result->selection.text) ||
                state->stopRequested.load(std::memory_order_acquire))
                continue;

            // Nonblocking delivery keeps the worker independent of JS progress. The
            // result is deleted locally if the TSFN is closing or its queue rejects it.
            std::lock_guard<std::mutex> apiLock(state->napiMutex);
            napi_threadsafe_function resultTsfn = state->resultTsfn.load(std::memory_order_acquire);
            if (!state->abandoned.load(std::memory_order_acquire) &&
                !state->stopRequested.load(std::memory_order_acquire) && resultTsfn)
            {
                SelectionWorkerResult *rawResult = result.release();
                if (napi_call_threadsafe_function(resultTsfn, rawResult, napi_tsfn_nonblocking) != napi_ok)
                    delete rawResult;
            }
        }
        // Release the UIA client before leaving its COM apartment.
        automation->Release();
    }
    else if (automation)
    {
        automation->Release();
    }

    if (comInitialized)
        CoUninitialize();

    {
        std::lock_guard<std::mutex> apiLock(state->napiMutex);
        // In the abandoned path the Node environment may already be destroyed, so no
        // N-API call is safe. Its worker TSFN reference is intentionally process-bounded.
        if (!state->abandoned.load(std::memory_order_acquire))
            napi_release_threadsafe_function(workerTsfn, napi_tsfn_release);
    }
    // Signal completion last, after all normal COM and TSFN cleanup is finished.
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->completed = true;
    }
    state->completionCv.notify_all();
}

void SelectionWorker::PinCurrentModule()
{
    // GET_MODULE_HANDLE_EX_FLAG_PIN is permanent for the process. Use it only after a
    // confirmed stop timeout, never on the normal joined path.
    HMODULE module = nullptr;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
    if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&SelectionWorker::PinCurrentModule), &module))
    {
        fprintf(stderr, "[selection-hook] failed to pin addon module for abandoned worker: error=%lu\n",
                GetLastError());
    }
}
