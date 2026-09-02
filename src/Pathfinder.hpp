// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

#include "JobSystem.hpp"
#include "Terrain.hpp"

namespace CnaRts
{
    /** @brief Cost of one orthogonal step in the integration fields, in "decimetres". */
    constexpr std::uint16_t kStepOrthogonal = 10;
    /** @brief Cost of one diagonal step: 14/10 approximates sqrt(2) to within 1%. */
    constexpr std::uint16_t kStepDiagonal = 14;
    /** @brief Integration cost meaning "no path from here". */
    constexpr std::uint16_t kUnreachable = 0xFFFF;

    /**
     * @brief A maximal run of mutually passable tiles along the border between two sectors.
     *
     * A portal is a *span*, not a point, and both facts matter. The abstract A* needs one
     * representative tile so the graph has finite nodes; the flow field that actually moves the
     * units seeds from the whole span, because seeding from the representative alone funnels a
     * thousand units through a single tile and turns every doorway into a traffic jam.
     */
    struct Portal
    {
        int sector[2] = {-1, -1};        ///< the two sectors this portal joins
        std::int32_t repTile[2] = {0, 0};///< representative tile on each side, for the abstract graph
        std::int32_t spanStart[2] = {0, 0};///< first tile of the run on each side
        int localIndex[2] = {0, 0};       ///< position of this portal inside each sector's portal list
        int spanLength = 0;              ///< tiles in the run
        bool vertical = false;           ///< true when the border runs top-to-bottom (sectors side by side)
    };

    /**
     * @brief The static abstract graph: sector portals plus every within-sector portal distance.
     *
     * Built once per map, in parallel across sectors, and then never touched again -- which is
     * the entire economic argument for hierarchical pathfinding here. A path query at a million
     * units cannot afford to look at four million tiles; it looks at a few dozen portals whose
     * mutual distances were paid for at load time, and only the sectors the answer actually
     * crosses ever get a tile-level field built for them.
     */
    class SectorGraph
    {
    public:
        void Build(const Terrain& terrain, JobSystem& jobs);

        [[nodiscard]] const std::vector<Portal>& Portals() const { return portals_; }
        /** @brief Portal ids touching sector @p sector. */
        [[nodiscard]] const std::int32_t* SectorPortals(int sector, int& count) const
        {
            const std::int32_t from = sectorPortalStart_[static_cast<std::size_t>(sector)];
            const std::int32_t to   = sectorPortalStart_[static_cast<std::size_t>(sector) + 1];
            count = static_cast<int>(to - from);
            return sectorPortalList_.data() + from;
        }

        /**
         * @brief Within-sector distance between the @p a-th and @p b-th portal of @p sector.
         * @return kUnreachable when no within-sector route exists.
         */
        [[nodiscard]] std::uint16_t PortalDistance(int sector, int a, int b) const
        {
            const std::int32_t base = sectorMatrixStart_[static_cast<std::size_t>(sector)];
            int count = 0;
            (void) SectorPortals(sector, count);
            return portalMatrix_[static_cast<std::size_t>(base) + static_cast<std::size_t>(a) * count + b];
        }

        [[nodiscard]] std::size_t PortalCount() const { return portals_.size(); }

    private:
        void CollectPortals(const Terrain& terrain);
        void BuildMatrices(const Terrain& terrain, JobSystem& jobs);

        std::vector<Portal> portals_;
        std::vector<std::int32_t> sectorPortalStart_;  ///< sectorCount + 1
        std::vector<std::int32_t> sectorPortalList_;
        std::vector<std::int32_t> sectorMatrixStart_;  ///< sectorCount, offset into portalMatrix_
        std::vector<std::uint16_t> portalMatrix_;
    };

    /**
     * @brief One step of an abstract route: "cross this sector and leave through that portal".
     *
     * The last step carries `exitPortal == -1`, meaning the goal tile itself is the target.
     */
    struct CorridorStep
    {
        int sector = -1;
        int exitPortal = -1;
    };

    /** @brief The result of one abstract query: the ordered sectors a group will cross. */
    struct Corridor
    {
        std::vector<CorridorStep> steps;
        std::int32_t goalTile = 0;
        [[nodiscard]] bool Empty() const { return steps.empty(); }
    };

    /**
     * @brief Scratch buffers an abstract query needs, kept alive across queries.
     *
     * A path query allocating three vectors sized by the portal count would be the single
     * largest allocation in a frame that otherwise allocates nothing at all.
     */
    struct PathfinderScratch
    {
        std::vector<std::uint32_t> gScore;
        std::vector<std::int32_t>  cameFrom;
        std::vector<std::uint8_t>  closed;
        std::vector<std::uint16_t> field;         ///< reusable integration field, sector sized
        std::vector<std::int32_t>  seeds;
        std::vector<std::uint64_t> heap;          ///< binary heap of (fScore << 32) | node
    };

    /**
     * @brief Integrates costs over an axis-aligned tile region, seeded from @p seeds.
     *
     * Dial's algorithm rather than a binary heap: every edge costs either 10 or 14, so a bucket
     * ring of 15 entries is a *constant-time* priority queue where a heap would pay a log. The
     * region is small (one sector), it is integrated thousands of times while a map is loading,
     * and that constant is the difference between a snappy start-up and a visible pause.
     *
     * @param terrain The map; tiles outside the region are never entered.
     * @param x0,y0,w,h The region, in tile coordinates.
     * @param seeds Region-local tile indices (`ly * w + lx`) to start from, all at cost 0.
     * @param out Region-local costs, resized to `w * h`; unreachable tiles get kUnreachable.
     */
    void IntegrateRegion(const Terrain& terrain, int x0, int y0, int w, int h,
                         const std::vector<std::int32_t>& seeds,
                         std::vector<std::uint16_t>& out);

    /**
     * @brief Abstract A* from @p startTile to @p goalTile over the portal graph.
     *
     * @return false when the two tiles are in different connected components -- which Terrain's
     *         generator makes impossible, so a false return is a defect rather than a map.
     */
    bool FindCorridor(const Terrain& terrain, const SectorGraph& graph,
                      std::int32_t startTile, std::int32_t goalTile,
                      PathfinderScratch& scratch, Corridor& out);
}
