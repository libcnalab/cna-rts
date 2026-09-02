// SPDX-License-Identifier: MIT
#include "JobSystem.hpp"

#include <algorithm>

namespace CnaRts
{
    JobSystem::JobSystem(int workerCount)
    {
        if (workerCount <= 0)
        {
            workerCount = static_cast<int>(std::thread::hardware_concurrency());
            if (workerCount <= 0) workerCount = 1;
        }
        chunkCount_ = workerCount;

        // One fewer thread than chunks: the calling thread takes a chunk too, so a four-core
        // machine runs four chunks on four cores instead of four chunks on four workers while
        // the caller blocks.
        workers_.reserve(static_cast<std::size_t>(chunkCount_ - 1));
        for (int i = 1; i < chunkCount_; ++i)
            workers_.emplace_back([this, i] { WorkerLoop(i); });
    }

    JobSystem::~JobSystem()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shuttingDown_ = true;
            ++generation_;
        }
        startSignal_.notify_all();
        for (std::thread& worker : workers_)
            if (worker.joinable()) worker.join();
    }

    void JobSystem::RunChunks(std::unique_lock<std::mutex>& lock, std::uint64_t generation)
    {
        // The claim and the read of `body_` happen together, under the lock, and that pairing is
        // the whole correctness argument.
        //
        // The obvious lock-free version -- read `body_` once, then claim chunks with an atomic
        // counter -- is wrong in a way that only shows up under load. A worker that reads the
        // pointer, is descheduled, and wakes after the caller has moved on to the *next*
        // ParallelChunks call will claim a chunk belonging to the new batch and run it through
        // the old batch's function object, which by then is a destroyed temporary in a frame
        // that has returned. Checking the generation at claim time is what rules that out: a
        // successful claim implies `remaining_` is still above zero, which implies the caller is
        // still blocked inside the call that lent us the function.
        while (generation_ == generation && nextChunk_ < chunkCount_)
        {
            const int chunk = nextChunk_++;
            const std::function<void(int, std::int32_t, std::int32_t)>* body = body_;
            const std::int32_t count = itemCount_;

            std::int32_t begin = 0, end = 0;
            ChunkRange(count, chunk, chunkCount_, begin, end);

            lock.unlock();
            if (begin < end && body != nullptr) (*body)(chunk, begin, end);
            lock.lock();

            if (--remaining_ == 0) doneSignal_.notify_all();
        }
    }

    void JobSystem::WorkerLoop(int)
    {
        std::uint64_t seenGeneration = 0;
        while (true)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            startSignal_.wait(lock, [&] { return shuttingDown_ || generation_ != seenGeneration; });
            if (shuttingDown_) return;
            seenGeneration = generation_;
            RunChunks(lock, seenGeneration);
        }
    }

    void JobSystem::ParallelChunks(std::int32_t count,
                                   const std::function<void(int, std::int32_t, std::int32_t)>& body)
    {
        if (count <= 0) return;
        if (chunkCount_ == 1)
        {
            body(0, 0, count);
            return;
        }

        std::uint64_t generation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body_ = &body;
            itemCount_ = count;
            remaining_ = chunkCount_;
            nextChunk_ = 0;
            generation = ++generation_;
        }
        startSignal_.notify_all();

        // The caller is a worker too, so no core sits idle waiting for its own work.
        std::unique_lock<std::mutex> lock(mutex_);
        RunChunks(lock, generation);
        doneSignal_.wait(lock, [&] { return remaining_ == 0; });
        body_ = nullptr;
    }
}
