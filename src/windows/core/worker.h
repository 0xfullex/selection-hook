/**
 * @file
 * @brief Dedicated MTA worker for automatic Windows text selection retrieval.
 *
 * The worker owns its COM apartment and UIA client. It accepts value-only requests and
 * returns value-only results so no COM interface or Napi::Value crosses threads.
 */
#pragma once

#include <napi.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "engine.h"
#include "types.h"

/**
 * @brief Long-lived single-thread MTA executor for automatic selection retrieval.
 *
 * @note The public getCurrentSelection() path does not use this worker and remains
 * synchronous on its calling thread.
 * @warning The worker's UIA client is created, used, and released inside ThreadMain.
 * Never pass that client or any other raw COM interface across the thread boundary.
 */
class SelectionWorker
{
  public:
    SelectionWorker() = default;
    ~SelectionWorker();

    SelectionWorker(const SelectionWorker &) = delete;
    SelectionWorker &operator=(const SelectionWorker &) = delete;

    /**
     * @brief Start the worker and wait for its COM/UIA readiness handshake.
     * @param[in] generation Start/stop generation copied into all results.
     * @param[in] resultTsfn TSFN created for this worker. After valid argument checks,
     * ownership transfers to this call on both success and failure paths.
     * @param[in] gates Shared retrieval gates that outlive controlled abandonment.
     * @param[out] error Human-readable initialization error on failure.
     * @return true only after the worker has created its MTA UIA client.
     */
    bool Start(uint64_t generation, napi_threadsafe_function resultTsfn, const std::shared_ptr<RetrievalGates> &gates,
               std::string &error);

    /**
     * @brief Submit a value-only automatic retrieval request.
     * @param[in] request Snapshot consumed by this call.
     * @return false when stopping or when the single pending slot is occupied.
     * @note SelectionHook's auto gate permits one active request, so callers must not use
     * the pending slot to build a backlog of stale mouse selections.
     */
    bool Enqueue(SelectionRequest request);

    /**
     * @brief Stop accepting work and join, or enter controlled abandonment on timeout.
     * @warning Accessibility calls have no reliable hard-cancel boundary. Do not replace
     * the bounded wait with an unlimited join or terminate the worker thread. The former
     * can freeze the host; the latter can corrupt COM/provider state.
     */
    void Stop();

  private:
    struct State;
    static void ThreadMain(const std::shared_ptr<State> &state, napi_threadsafe_function workerTsfn);
    static void PinCurrentModule();

    std::shared_ptr<State> state_;
    std::thread thread_;
};
