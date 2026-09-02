// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

#include "Fixed.hpp"
#include "JobSystem.hpp"

namespace CnaRts
{
    /** @brief Teams the whole program supports. Four keeps the per-cell tally one cache line. */
    constexpr int kMaxTeams = 4;

    /** @brief One unit's position in the cell-sorted arrays; interleaved so a neighbour read
     *  fetches both coordinates on one cache line rather than touching two streams. */
    struct GridPoint
    {
        Fixed x, y;
    };

    /**
     * @brief A uniform grid over the map, rebuilt from scratch every tick by counting sort.
     *
     * Rebuilding beats updating for this workload. An incremental structure -- a quadtree, or a
     * grid with per-cell insert/remove -- pays a pointer chase and a branch per moved entity and
     * fragments its memory as the swarm flows across the map; a counting sort is two linear
     * passes over a flat array and leaves every cell's members *contiguous*, which is what makes
     * the neighbour queries in the movement pass cache-friendly at a million entities.
     *
     * The sort is deterministic by construction: the per-chunk counts are summed in chunk order,
     * so a unit's position in `SortedUnits()` depends only on its own index and the counts, never
     * on which thread got there first.
     */
    class SpatialGrid
    {
    public:
        /**
         * @param tiles       Map size in tiles.
         * @param cellShift   log2 of the cell edge in tiles (3 == 8x8-tile cells).
         * @param chunkCount  Number of parallel chunks the build will be split into.
         */
        void Configure(int tiles, int cellShift, int chunkCount);

        /** @brief Sorts @p count units into cells. Positions are read, nothing is written back. */
        void Build(JobSystem& jobs, std::int32_t count,
                   const Fixed* posX, const Fixed* posY, const std::uint8_t* team);

        [[nodiscard]] int CellsPerSide() const { return cellsPerSide_; }
        [[nodiscard]] int CellShift() const { return cellShift_; }
        [[nodiscard]] int CellSizeTiles() const { return 1 << cellShift_; }
        [[nodiscard]] std::int32_t CellCount() const { return cellCount_; }

        [[nodiscard]] int CellXOf(Fixed worldX) const
        {
            const std::int32_t tile = worldX.ToInt();
            return Clamp(tile >> cellShift_);
        }
        [[nodiscard]] int CellYOf(Fixed worldY) const
        {
            const std::int32_t tile = worldY.ToInt();
            return Clamp(tile >> cellShift_);
        }
        [[nodiscard]] std::int32_t CellIndex(int cx, int cy) const
        {
            return static_cast<std::int32_t>(cy) * cellsPerSide_ + cx;
        }

        /** @brief Index of the first unit of cell @p c inside SortedUnits(). */
        [[nodiscard]] std::int32_t CellBegin(std::int32_t c) const { return cellStart_[c]; }
        /** @brief One past the last unit of cell @p c inside SortedUnits(). */
        [[nodiscard]] std::int32_t CellEnd(std::int32_t c) const { return cellStart_[c + 1]; }
        [[nodiscard]] std::int32_t CellPopulation(std::int32_t c) const
        {
            return cellStart_[c + 1] - cellStart_[c];
        }

        /** @brief Unit indices grouped by cell, ascending within each cell. */
        [[nodiscard]] const std::vector<std::int32_t>& SortedUnits() const { return sorted_; }
        /**
         * @brief The positions of `SortedUnits()`, in the same order.
         *
         * A copy of data that already exists, kept because of what it costs *not* to have it.
         * The movement pass reads a handful of neighbours per unit; with only the index list it
         * would follow each one into the position arrays at a scattered offset, which at a
         * million units is several million cache misses a tick and was measured at roughly sixty
         * milliseconds. Writing the positions out during the scatter -- which is already
         * touching those cache lines -- makes every neighbour read sequential within its cell.
         */
        [[nodiscard]] const GridPoint* SortedPositions() const { return sortedPos_.data(); }
        /** @brief Where unit @p i landed inside the sorted arrays. Lets the movement pass read a
         *  contiguous run of its own cell-mates instead of striding through the cell. */
        [[nodiscard]] const std::int32_t* SortedSlotOfUnit() const { return sortedSlot_.data(); }
        /** @brief `CellBegin`/`CellEnd` as a raw array, for a hot loop that wants no call. */
        [[nodiscard]] const std::int32_t* CellStarts() const { return cellStart_.data(); }
        /** @brief The cell each unit landed in, indexed by unit. */
        [[nodiscard]] const std::vector<std::int32_t>& CellOfUnit() const { return cellOfUnit_; }
        /**
         * @brief Units of each team in each cell, `cell * kMaxTeams + team`.
         *
         * Fog of war rasterises visibility from this instead of walking a million units, and the
         * combat pass reads an enemy count out of it instead of testing pairs.
         */
        [[nodiscard]] const std::vector<std::uint16_t>& CellTeamCount() const { return cellTeamCount_; }
        [[nodiscard]] std::uint16_t TeamInCell(std::int32_t cell, int team) const
        {
            return cellTeamCount_[static_cast<std::size_t>(cell) * kMaxTeams + team];
        }

    private:
        [[nodiscard]] int Clamp(std::int32_t v) const
        {
            return v < 0 ? 0 : (v >= cellsPerSide_ ? cellsPerSide_ - 1 : static_cast<int>(v));
        }

        int cellShift_ = 3;
        int cellsPerSide_ = 1;
        int chunkCount_ = 1;
        std::int32_t cellCount_ = 1;

        std::vector<std::int32_t> cellStart_;      ///< cellCount_ + 1 entries
        std::vector<std::int32_t> chunkOffsets_;   ///< chunkCount_ * cellCount_
        std::vector<std::int32_t> sorted_;
        std::vector<std::int32_t> sortedSlot_;
        std::vector<GridPoint> sortedPos_;
        std::vector<std::int32_t> cellOfUnit_;
        std::vector<std::uint16_t> cellTeamCount_;
    };
}
