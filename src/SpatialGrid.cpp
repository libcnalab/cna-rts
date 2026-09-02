// SPDX-License-Identifier: MIT
#include "SpatialGrid.hpp"

#include <algorithm>

namespace CnaRts
{
    void SpatialGrid::Configure(int tiles, int cellShift, int chunkCount)
    {
        cellShift_ = std::clamp(cellShift, 1, 8);
        cellsPerSide_ = std::max(1, tiles >> cellShift_);
        cellCount_ = static_cast<std::int32_t>(cellsPerSide_) * cellsPerSide_;
        chunkCount_ = std::max(1, chunkCount);

        cellStart_.assign(static_cast<std::size_t>(cellCount_) + 1, 0);
        chunkOffsets_.assign(static_cast<std::size_t>(chunkCount_) * cellCount_, 0);
        cellTeamCount_.assign(static_cast<std::size_t>(cellCount_) * kMaxTeams, 0);
    }

    void SpatialGrid::Build(JobSystem& jobs, std::int32_t count,
                            const Fixed* posX, const Fixed* posY, const std::uint8_t* team)
    {
        sorted_.resize(static_cast<std::size_t>(count));
        sortedSlot_.resize(static_cast<std::size_t>(count));
        sortedPos_.resize(static_cast<std::size_t>(count));
        cellOfUnit_.resize(static_cast<std::size_t>(count));
        std::fill(chunkOffsets_.begin(), chunkOffsets_.end(), 0);

        const int cellsPerSide = cellsPerSide_;
        const int cellShift = cellShift_;
        const std::int32_t cellCount = cellCount_;

        // --- Pass 1: bucket every unit and count, per chunk, into that chunk's own row. ---
        jobs.ParallelChunks(count, [&](int chunk, std::int32_t begin, std::int32_t end) {
            std::int32_t* counts = chunkOffsets_.data() + static_cast<std::size_t>(chunk) * cellCount;
            for (std::int32_t i = begin; i < end; ++i)
            {
                const std::int32_t tx = posX[i].ToInt() >> cellShift;
                const std::int32_t ty = posY[i].ToInt() >> cellShift;
                const int cx = tx < 0 ? 0 : (tx >= cellsPerSide ? cellsPerSide - 1 : static_cast<int>(tx));
                const int cy = ty < 0 ? 0 : (ty >= cellsPerSide ? cellsPerSide - 1 : static_cast<int>(ty));
                const std::int32_t cell = static_cast<std::int32_t>(cy) * cellsPerSide + cx;
                cellOfUnit_[static_cast<std::size_t>(i)] = cell;
                ++counts[cell];
            }
        });

        // --- Pass 2: cell totals, then the exclusive prefix over cells. ---
        // The prefix itself is sequential over cells (it is a serial dependency by definition),
        // but summing the per-chunk rows into the totals is not, and that is the part whose cost
        // grows with the worker count.
        jobs.ParallelChunks(cellCount, [&](int, std::int32_t begin, std::int32_t end) {
            for (std::int32_t c = begin; c < end; ++c)
            {
                std::int32_t total = 0;
                for (int chunk = 0; chunk < chunkCount_; ++chunk)
                    total += chunkOffsets_[static_cast<std::size_t>(chunk) * cellCount + c];
                cellStart_[static_cast<std::size_t>(c)] = total;
            }
        });

        std::int32_t running = 0;
        for (std::int32_t c = 0; c < cellCount; ++c)
        {
            const std::int32_t population = cellStart_[static_cast<std::size_t>(c)];
            cellStart_[static_cast<std::size_t>(c)] = running;
            running += population;
        }
        cellStart_[static_cast<std::size_t>(cellCount)] = running;

        // --- Pass 3: turn each chunk's counts into that chunk's write cursor. ---
        // Chunk c's block inside a cell begins after every *lower-numbered* chunk's block, which
        // is what makes the sorted order depend on the unit index alone.
        jobs.ParallelChunks(cellCount, [&](int, std::int32_t begin, std::int32_t end) {
            for (std::int32_t c = begin; c < end; ++c)
            {
                std::int32_t cursor = cellStart_[static_cast<std::size_t>(c)];
                for (int chunk = 0; chunk < chunkCount_; ++chunk)
                {
                    std::int32_t& slot = chunkOffsets_[static_cast<std::size_t>(chunk) * cellCount + c];
                    const std::int32_t population = slot;
                    slot = cursor;
                    cursor += population;
                }
            }
        });

        // --- Pass 4: scatter the indices, and the positions alongside them. ---
        jobs.ParallelChunks(count, [&](int chunk, std::int32_t begin, std::int32_t end) {
            std::int32_t* cursors = chunkOffsets_.data() + static_cast<std::size_t>(chunk) * cellCount;
            for (std::int32_t i = begin; i < end; ++i)
            {
                const auto at = static_cast<std::size_t>(
                    cursors[cellOfUnit_[static_cast<std::size_t>(i)]]++);
                sorted_[at] = i;
                sortedSlot_[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(at);
                sortedPos_[at] = GridPoint{posX[i], posY[i]};
            }
        });

        // --- Pass 5: the per-team tally of each cell. Fog of war rasterises from it and the
        // combat pass reads its enemy count from it, so neither has to walk the units again. ---
        jobs.ParallelChunks(cellCount, [&](int, std::int32_t begin, std::int32_t end) {
            for (std::int32_t c = begin; c < end; ++c)
            {
                std::uint16_t tally[kMaxTeams] = {};
                const std::int32_t from = cellStart_[static_cast<std::size_t>(c)];
                const std::int32_t to   = cellStart_[static_cast<std::size_t>(c) + 1];
                for (std::int32_t k = from; k < to; ++k)
                {
                    std::uint16_t& slot =
                        tally[team[sorted_[static_cast<std::size_t>(k)]] & (kMaxTeams - 1)];
                    if (slot < 0xFFFF) ++slot;
                }
                for (int t = 0; t < kMaxTeams; ++t)
                    cellTeamCount_[static_cast<std::size_t>(c) * kMaxTeams + t] = tally[t];
            }
        });
    }
}
