// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

#include "Fixed.hpp"
#include "Pathfinder.hpp"

namespace CnaRts
{
    /** @brief Flow byte meaning "this tile is the target"; every other value indexes kDirection8. */
    constexpr std::uint8_t kFlowArrived = 8;

    /** @brief The eight flow directions as unit vectors, diagonals pre-normalised. */
    extern const Vec2f kDirection8[9];

    /**
     * @brief One group's navigation state: a flow field per corridor sector, and a way home.
     *
     * The load-bearing idea of the whole movement system. A path query answers for a *group*,
     * not for a unit: the abstract corridor is computed once, a tile-level flow field is built
     * for each sector it crosses, and then a hundred thousand units in that group each spend one
     * array lookup per tick to know which way to walk. Per-unit A* at this scale is not slow, it
     * is arithmetically impossible -- a million units repathing once a second at even ten
     * microseconds each is ten seconds of work per second.
     *
     * Units that wander off the corridor are not abandoned. `GuideTileOf` gives every sector on
     * the map a target that leads back toward the corridor, so a unit shoved out of the planned
     * route by the crowd walks back into it instead of pressing against a cliff forever.
     */
    class GroupNavigation
    {
    public:
        void Configure(const Terrain& terrain);

        /**
         * @brief Builds the flow fields for @p corridor and the off-corridor guidance around it.
         *
         * @param scratch Reused integration buffers; nothing is allocated per call after the
         *                first plan of a given size.
         */
        void Plan(const Terrain& terrain, const SectorGraph& graph,
                  const Corridor& corridor, PathfinderScratch& scratch);

        /** @brief Forgets the plan; every sector falls back to steering straight at the goal. */
        void Clear();

        [[nodiscard]] bool HasPlan() const { return hasPlan_; }
        [[nodiscard]] std::int32_t GoalTile() const { return goalTile_; }

        /**
         * @brief The flow direction at a tile, or `kFlowArrived` when there is no field here.
         *
         * @param sector    The sector the tile is in.
         * @param localTile `(ty - originY) * sectorSize + (tx - originX)`.
         */
        [[nodiscard]] std::uint8_t FlowAt(int sector, std::int32_t localTile) const
        {
            const std::int32_t slot = flowSlotOfSector_[static_cast<std::size_t>(sector)];
            if (slot < 0) return kFlowArrived;
            return flowData_[static_cast<std::size_t>(slot) * sectorArea_ +
                             static_cast<std::size_t>(localTile)];
        }

        [[nodiscard]] bool HasFieldFor(int sector) const
        {
            return flowSlotOfSector_[static_cast<std::size_t>(sector)] >= 0;
        }

        /** @brief Where a unit in @p sector should head when that sector has no flow field. */
        [[nodiscard]] std::int32_t GuideTileOf(int sector) const
        {
            return guideTile_[static_cast<std::size_t>(sector)];
        }

    private:
        void BuildGuidance(const Terrain& terrain, const SectorGraph& graph,
                           const Corridor& corridor);

        int sectorSize_ = 0;
        int sectorArea_ = 0;
        int sectorCount_ = 0;
        bool hasPlan_ = false;
        std::int32_t goalTile_ = 0;

        std::vector<std::int32_t> flowSlotOfSector_;
        std::vector<std::uint8_t> flowData_;
        std::vector<std::int32_t> guideTile_;
        std::vector<std::int32_t> bfsQueue_;
    };
}
