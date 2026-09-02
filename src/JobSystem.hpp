// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace CnaRts
{
    /**
     * @brief A persistent worker pool that splits an index range into a *fixed* set of chunks.
     *
     * Two properties the simulation depends on, neither of which a generic parallel-for gives:
     *
     * 1. **The partition does not depend on scheduling.** `ParallelChunks(n)` always cuts the
     *    range into `WorkerCount()` contiguous blocks at the same boundaries, so a body that
     *    writes only within its own block produces bit-identical output no matter which worker
     *    picks up which block or in what order. Work stealing would be faster on a ragged load
     *    and would make the checksum depend on the machine, which is the one thing this
     *    simulation may not do.
     *
     * 2. **No thread is created inside a tick.** `System::Threading::Tasks::Parallel::For` --
     *    sharp-runtime's own, faithfully ported from .NET -- launches one `std::async` per
     *    iteration, batched to the degree of parallelism. That is right for the coarse work the
     *    API is written for, and it is roughly 30 microseconds of thread creation per chunk per
     *    phase here: five phases at 30 Hz on sixteen chunks is milliseconds of pure overhead in
     *    a 33-millisecond budget. This pool pays that cost once, at startup.
     */
    class JobSystem
    {
    public:
        /** @param workerCount Threads to run bodies on; 0 asks the hardware. */
        explicit JobSystem(int workerCount = 0);
        ~JobSystem();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        /** @brief How many chunks every ParallelChunks call produces. */
        [[nodiscard]] int WorkerCount() const { return chunkCount_; }

        /**
         * @brief Runs @p body once per chunk, in parallel, and returns when all have finished.
         *
         * @param count Total number of items; chunk `c` covers `[begin, end)` of it.
         * @param body  Called as `body(chunkIndex, begin, end)`. Blocks are contiguous and the
         *              boundaries depend only on @p count and `WorkerCount()`.
         */
        void ParallelChunks(std::int32_t count,
                            const std::function<void(int, std::int32_t, std::int32_t)>& body);

        /** @brief The chunk boundaries `ParallelChunks` would use, for a caller that needs them
         *  ahead of time (per-chunk scratch sizing, prefix sums over per-chunk counts). */
        static void ChunkRange(std::int32_t count, int chunkIndex, int chunkCount,
                               std::int32_t& begin, std::int32_t& end)
        {
            const std::int32_t base = count / chunkCount;
            const std::int32_t rest = count % chunkCount;
            begin = base * chunkIndex + (chunkIndex < rest ? chunkIndex : rest);
            end   = begin + base + (chunkIndex < rest ? 1 : 0);
        }

    private:
        void WorkerLoop(int workerIndex);
        /** @brief Claims and runs chunks of @p generation until it has none left. Enters and
         *  leaves with @p lock held; releases it only around the body call. */
        void RunChunks(std::unique_lock<std::mutex>& lock, std::uint64_t generation);

        std::vector<std::thread> workers_;
        int chunkCount_ = 1;

        std::mutex mutex_;
        std::condition_variable startSignal_;
        std::condition_variable doneSignal_;

        const std::function<void(int, std::int32_t, std::int32_t)>* body_ = nullptr;
        std::int32_t itemCount_ = 0;
        std::uint64_t generation_ = 0;
        int nextChunk_ = 0;
        int remaining_ = 0;
        bool shuttingDown_ = false;
    };
}
