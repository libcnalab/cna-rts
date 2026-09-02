// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

#include "JobSystem.hpp"
#include "SpatialGrid.hpp"

namespace CnaRts
{
    /**
     * @brief Per-team visibility over a coarse grid, rasterised from cell occupancy.
     *
     * The scaling trick that makes fog of war possible at this size: **vision is not stamped per
     * unit.** A million circular stamps would be a hundred million writes a tick, and would spend
     * nearly all of them re-lighting a tile that a thousand neighbouring units had already lit.
     *
     * What is stamped instead is *occupancy*: the spatial grid already knows, from its own sort,
     * which cells hold units of which team, so fog of war starts from a grid of ones and zeroes
     * whose size is the cell count -- independent of the unit count. Vision radius is then a
     * chamfer distance transform over that grid, two raster sweeps with a 5/7 mask that
     * approximates Euclidean distance to within 2%, and a cell is visible when its distance to
     * the nearest occupied cell is inside the radius. The cost is `O(cells)` with a constant of
     * about eight, whatever the radius and however many units produced the occupancy.
     */
    class FogOfWar
    {
    public:
        /**
         * @param cellsPerSide Resolution of the fog grid -- the spatial grid's own resolution, so
         *                     occupancy transfers with no resampling.
         * @param teams        Teams to track.
         */
        void Configure(int cellsPerSide, int teams);

        /** @brief Recomputes visibility and folds it into the explored map. */
        void Update(JobSystem& jobs, const SpatialGrid& grid, int visionRadiusCells);

        /** @brief Discards everything: nothing visible, nothing explored. */
        void Reset();

        [[nodiscard]] int CellsPerSide() const { return cellsPerSide_; }

        /**
         * @brief The texture the renderer samples: one byte per cell for @p team.
         *
         * 255 fully visible, 96 explored but not currently seen, 0 never seen.
         */
        [[nodiscard]] const std::vector<std::uint8_t>& Shade(int team) const
        {
            return shade_[static_cast<std::size_t>(team)];
        }

        /** @brief How many cells @p team can currently see, for the HUD. */
        [[nodiscard]] std::int32_t VisibleCells(int team) const
        {
            return visibleCells_[static_cast<std::size_t>(team)];
        }
        /** @brief How many cells @p team has ever seen. */
        [[nodiscard]] std::int32_t ExploredCells(int team) const
        {
            return exploredCells_[static_cast<std::size_t>(team)];
        }

    private:
        /** @brief Chamfer weight of an orthogonal step; 7 for a diagonal one (7/5 ~ sqrt 2). */
        static constexpr std::uint16_t kChamferOrthogonal = 5;
        static constexpr std::uint16_t kChamferDiagonal   = 7;

        void UpdateTeam(int team, const SpatialGrid& grid, int radius);

        int cellsPerSide_ = 0;
        int teams_ = 0;
        std::vector<std::vector<std::uint16_t>> distance_;   ///< per team, chamfer scratch
        std::vector<std::vector<std::uint8_t>> explored_;
        std::vector<std::vector<std::uint8_t>> shade_;
        std::vector<std::int32_t> visibleCells_;
        std::vector<std::int32_t> exploredCells_;
    };
}
