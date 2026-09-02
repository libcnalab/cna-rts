// SPDX-License-Identifier: MIT
#include "Pathfinder.hpp"

#include <algorithm>
#include <array>

namespace CnaRts
{
    namespace
    {
        /** @brief The eight neighbour offsets, orthogonals first so the cheap ones are tried first. */
        constexpr int kNeighbourDx[8] = { 1, -1,  0,  0,  1,  1, -1, -1};
        constexpr int kNeighbourDy[8] = { 0,  0,  1, -1,  1, -1,  1, -1};
        constexpr std::uint16_t kNeighbourCost[8] = {
            kStepOrthogonal, kStepOrthogonal, kStepOrthogonal, kStepOrthogonal,
            kStepDiagonal,   kStepDiagonal,   kStepDiagonal,   kStepDiagonal};

        /** @brief Octile distance in the same units the integration fields use. */
        std::uint32_t OctileDistance(int ax, int ay, int bx, int by)
        {
            const int dx = ax > bx ? ax - bx : bx - ax;
            const int dy = ay > by ? ay - by : by - ay;
            const int lo = dx < dy ? dx : dy;
            const int hi = dx < dy ? dy : dx;
            return static_cast<std::uint32_t>(lo) * kStepDiagonal +
                   static_cast<std::uint32_t>(hi - lo) * kStepOrthogonal;
        }

        /** @brief Walks a portal's span on side @p side, calling @p visit with each global tile. */
        template <typename Fn>
        void ForEachSpanTile(const Portal& portal, int side, int tiles, Fn&& visit)
        {
            const std::int32_t stride = portal.vertical ? tiles : 1;
            for (int i = 0; i < portal.spanLength; ++i)
                visit(portal.spanStart[side] + stride * i);
        }
    }

    void IntegrateRegion(const Terrain& terrain, int x0, int y0, int w, int h,
                         const std::vector<std::int32_t>& seeds,
                         std::vector<std::uint16_t>& out)
    {
        out.assign(static_cast<std::size_t>(w) * h, kUnreachable);

        // Dial's bucket ring. Its length must exceed the largest edge cost so a bucket is never
        // reused while entries are still pending in it; 16 is the next power of two above 14,
        // which turns the modulo into a mask.
        constexpr int kBucketCount = 16;
        static thread_local std::array<std::vector<std::int32_t>, kBucketCount> buckets;
        for (auto& bucket : buckets) bucket.clear();

        std::size_t pending = 0;
        for (const std::int32_t seed : seeds)
        {
            if (seed < 0 || seed >= w * h) continue;
            if (out[static_cast<std::size_t>(seed)] == 0) continue;
            out[static_cast<std::size_t>(seed)] = 0;
            buckets[0].push_back(seed);
            ++pending;
        }
        if (pending == 0) return;

        std::uint32_t cost = 0;
        std::size_t scanned = 0;
        // The ring is scanned forward from the current cost; a full lap with nothing pending
        // means every reachable tile has been settled.
        while (pending > 0)
        {
            std::vector<std::int32_t>& bucket = buckets[cost & (kBucketCount - 1)];
            if (bucket.empty())
            {
                ++cost;
                if (++scanned > kBucketCount)
                {
                    // Nothing within a full ring lap: the remaining pending entries are stale
                    // duplicates that were superseded before they were popped.
                    break;
                }
                continue;
            }
            scanned = 0;

            const std::int32_t local = bucket.back();
            bucket.pop_back();
            --pending;
            const std::uint16_t here = out[static_cast<std::size_t>(local)];
            if (here != static_cast<std::uint16_t>(cost)) continue;   // superseded

            const int lx = local % w;
            const int ly = local / w;
            const int gx = x0 + lx;
            const int gy = y0 + ly;

            for (int d = 0; d < 8; ++d)
            {
                const int nlx = lx + kNeighbourDx[d];
                const int nly = ly + kNeighbourDy[d];
                if (nlx < 0 || nly < 0 || nlx >= w || nly >= h) continue;
                const int ngx = gx + kNeighbourDx[d];
                const int ngy = gy + kNeighbourDy[d];
                if (!terrain.Passable(ngx, ngy)) continue;
                if (d >= 4)
                {
                    // No corner cutting: a diagonal step needs both of its orthogonal
                    // components clear, or units slide through the corner of a rock.
                    if (!terrain.Passable(gx + kNeighbourDx[d], gy)) continue;
                    if (!terrain.Passable(gx, gy + kNeighbourDy[d])) continue;
                }
                const std::uint32_t candidate = here + kNeighbourCost[d];
                if (candidate >= kUnreachable) continue;
                const std::int32_t nlocal = nly * w + nlx;
                if (candidate >= out[static_cast<std::size_t>(nlocal)]) continue;
                out[static_cast<std::size_t>(nlocal)] = static_cast<std::uint16_t>(candidate);
                buckets[candidate & (kBucketCount - 1)].push_back(nlocal);
                ++pending;
            }
        }
    }

    void SectorGraph::Build(const Terrain& terrain, JobSystem& jobs)
    {
        CollectPortals(terrain);
        BuildMatrices(terrain, jobs);
    }

    void SectorGraph::CollectPortals(const Terrain& terrain)
    {
        portals_.clear();
        const int sectorSize = terrain.SectorSize();
        const int perSide = terrain.SectorsPerSide();

        const auto addRun = [&](int sectorA, int sectorB,
                                std::int32_t startA, std::int32_t startB,
                                int length, bool vertical) {
            Portal portal;
            portal.sector[0] = sectorA;
            portal.sector[1] = sectorB;
            portal.spanStart[0] = startA;
            portal.spanStart[1] = startB;
            portal.spanLength = length;
            portal.vertical = vertical;
            const std::int32_t stride = vertical ? terrain.Size() : 1;
            portal.repTile[0] = startA + stride * (length / 2);
            portal.repTile[1] = startB + stride * (length / 2);
            portals_.push_back(portal);
        };

        // Vertical borders: sector (sx, sy) against (sx + 1, sy). The shared edge runs along y,
        // so the portal span is vertical.
        for (int sy = 0; sy < perSide; ++sy)
        {
            for (int sx = 0; sx + 1 < perSide; ++sx)
            {
                const int sectorA = sy * perSide + sx;
                const int sectorB = sectorA + 1;
                const int xa = sx * sectorSize + sectorSize - 1;
                const int xb = xa + 1;
                const int y0 = sy * sectorSize;
                int runStart = -1;
                for (int y = y0; y <= y0 + sectorSize; ++y)
                {
                    const bool open = y < y0 + sectorSize &&
                                      terrain.Passable(xa, y) && terrain.Passable(xb, y);
                    if (open && runStart < 0) runStart = y;
                    if (!open && runStart >= 0)
                    {
                        addRun(sectorA, sectorB,
                               terrain.Index(xa, runStart), terrain.Index(xb, runStart),
                               y - runStart, true);
                        runStart = -1;
                    }
                }
            }
        }

        // Horizontal borders: sector (sx, sy) against (sx, sy + 1); the span runs along x.
        for (int sy = 0; sy + 1 < perSide; ++sy)
        {
            for (int sx = 0; sx < perSide; ++sx)
            {
                const int sectorA = sy * perSide + sx;
                const int sectorB = sectorA + perSide;
                const int ya = sy * sectorSize + sectorSize - 1;
                const int yb = ya + 1;
                const int x0 = sx * sectorSize;
                int runStart = -1;
                for (int x = x0; x <= x0 + sectorSize; ++x)
                {
                    const bool open = x < x0 + sectorSize &&
                                      terrain.Passable(x, ya) && terrain.Passable(x, yb);
                    if (open && runStart < 0) runStart = x;
                    if (!open && runStart >= 0)
                    {
                        addRun(sectorA, sectorB,
                               terrain.Index(runStart, ya), terrain.Index(runStart, yb),
                               x - runStart, false);
                        runStart = -1;
                    }
                }
            }
        }

        // Index the portals by sector, counting-sort style so the lists stay contiguous.
        const int sectorCount = terrain.SectorCount();
        sectorPortalStart_.assign(static_cast<std::size_t>(sectorCount) + 1, 0);
        for (const Portal& portal : portals_)
            for (int side = 0; side < 2; ++side)
                ++sectorPortalStart_[static_cast<std::size_t>(portal.sector[side]) + 1];
        for (int s = 0; s < sectorCount; ++s)
            sectorPortalStart_[static_cast<std::size_t>(s) + 1] +=
                sectorPortalStart_[static_cast<std::size_t>(s)];

        sectorPortalList_.assign(static_cast<std::size_t>(portals_.size()) * 2, 0);
        std::vector<std::int32_t> cursor(sectorPortalStart_.begin(), sectorPortalStart_.end() - 1);
        for (std::size_t p = 0; p < portals_.size(); ++p)
        {
            for (int side = 0; side < 2; ++side)
            {
                const int sector = portals_[p].sector[side];
                const std::int32_t at = cursor[static_cast<std::size_t>(sector)]++;
                sectorPortalList_[static_cast<std::size_t>(at)] = static_cast<std::int32_t>(p);
                portals_[p].localIndex[side] =
                    static_cast<int>(at - sectorPortalStart_[static_cast<std::size_t>(sector)]);
            }
        }
    }

    void SectorGraph::BuildMatrices(const Terrain& terrain, JobSystem& jobs)
    {
        const int sectorCount = terrain.SectorCount();
        const int sectorSize = terrain.SectorSize();

        sectorMatrixStart_.assign(static_cast<std::size_t>(sectorCount), 0);
        std::size_t total = 0;
        for (int s = 0; s < sectorCount; ++s)
        {
            int count = 0;
            (void) SectorPortals(s, count);
            sectorMatrixStart_[static_cast<std::size_t>(s)] = static_cast<std::int32_t>(total);
            total += static_cast<std::size_t>(count) * count;
        }
        portalMatrix_.assign(total, kUnreachable);

        // One integration per portal per sector, all sectors independent -- the single most
        // parallel phase in the whole program, and the reason map load is not a visible pause.
        jobs.ParallelChunks(sectorCount, [&](int, std::int32_t begin, std::int32_t end) {
            std::vector<std::uint16_t> field;
            std::vector<std::int32_t> seeds;
            for (std::int32_t s = begin; s < end; ++s)
            {
                int count = 0;
                const std::int32_t* list = SectorPortals(static_cast<int>(s), count);
                if (count == 0) continue;
                const int ox = terrain.SectorOriginX(static_cast<int>(s));
                const int oy = terrain.SectorOriginY(static_cast<int>(s));
                const std::int32_t base = sectorMatrixStart_[static_cast<std::size_t>(s)];

                for (int i = 0; i < count; ++i)
                {
                    const Portal& from = portals_[static_cast<std::size_t>(list[i])];
                    const int fromSide = from.sector[0] == s ? 0 : 1;

                    seeds.clear();
                    ForEachSpanTile(from, fromSide, terrain.Size(), [&](std::int32_t tile) {
                        seeds.push_back((tile / terrain.Size() - oy) * sectorSize +
                                        (tile % terrain.Size() - ox));
                    });
                    IntegrateRegion(terrain, ox, oy, sectorSize, sectorSize, seeds, field);

                    for (int j = 0; j < count; ++j)
                    {
                        if (i == j)
                        {
                            portalMatrix_[static_cast<std::size_t>(base) +
                                          static_cast<std::size_t>(i) * count + j] = 0;
                            continue;
                        }
                        const Portal& to = portals_[static_cast<std::size_t>(list[j])];
                        const int toSide = to.sector[0] == s ? 0 : 1;
                        std::uint16_t best = kUnreachable;
                        ForEachSpanTile(to, toSide, terrain.Size(), [&](std::int32_t tile) {
                            const std::int32_t local = (tile / terrain.Size() - oy) * sectorSize +
                                                       (tile % terrain.Size() - ox);
                            best = std::min(best, field[static_cast<std::size_t>(local)]);
                        });
                        portalMatrix_[static_cast<std::size_t>(base) +
                                      static_cast<std::size_t>(i) * count + j] = best;
                    }
                }
            }
        });
    }

    bool FindCorridor(const Terrain& terrain, const SectorGraph& graph,
                      std::int32_t startTile, std::int32_t goalTile,
                      PathfinderScratch& scratch, Corridor& out)
    {
        out.steps.clear();
        out.goalTile = goalTile;

        const int tiles = terrain.Size();
        const int sectorSize = terrain.SectorSize();
        const int startSector = terrain.SectorIndex(startTile % tiles, startTile / tiles);
        const int goalSector  = terrain.SectorIndex(goalTile % tiles, goalTile / tiles);

        if (startSector == goalSector)
        {
            out.steps.push_back({goalSector, -1});
            return true;
        }

        const auto localOf = [&](std::int32_t tile, int sector) {
            return (tile / tiles - terrain.SectorOriginY(sector)) * sectorSize +
                   (tile % tiles - terrain.SectorOriginX(sector));
        };

        // Distances from the start tile to the start sector's portals, and from the goal
        // sector's portals to the goal tile. Two tile-level integrations per query, both
        // confined to one sector: this is the "hierarchical" bargain made concrete.
        int startPortalCount = 0;
        const std::int32_t* startPortals = graph.SectorPortals(startSector, startPortalCount);
        scratch.seeds.assign(1, localOf(startTile, startSector));
        IntegrateRegion(terrain, terrain.SectorOriginX(startSector), terrain.SectorOriginY(startSector),
                        sectorSize, sectorSize, scratch.seeds, scratch.field);
        std::vector<std::uint16_t> startCosts(static_cast<std::size_t>(startPortalCount), kUnreachable);
        for (int i = 0; i < startPortalCount; ++i)
        {
            const Portal& portal = graph.Portals()[static_cast<std::size_t>(startPortals[i])];
            const int side = portal.sector[0] == startSector ? 0 : 1;
            std::uint16_t best = kUnreachable;
            ForEachSpanTile(portal, side, tiles, [&](std::int32_t tile) {
                best = std::min(best, scratch.field[static_cast<std::size_t>(localOf(tile, startSector))]);
            });
            startCosts[static_cast<std::size_t>(i)] = best;
        }

        int goalPortalCount = 0;
        const std::int32_t* goalPortals = graph.SectorPortals(goalSector, goalPortalCount);
        scratch.seeds.assign(1, localOf(goalTile, goalSector));
        IntegrateRegion(terrain, terrain.SectorOriginX(goalSector), terrain.SectorOriginY(goalSector),
                        sectorSize, sectorSize, scratch.seeds, scratch.field);
        std::vector<std::uint16_t> goalCosts(static_cast<std::size_t>(goalPortalCount), kUnreachable);
        for (int i = 0; i < goalPortalCount; ++i)
        {
            const Portal& portal = graph.Portals()[static_cast<std::size_t>(goalPortals[i])];
            const int side = portal.sector[0] == goalSector ? 0 : 1;
            std::uint16_t best = kUnreachable;
            ForEachSpanTile(portal, side, tiles, [&](std::int32_t tile) {
                best = std::min(best, scratch.field[static_cast<std::size_t>(localOf(tile, goalSector))]);
            });
            goalCosts[static_cast<std::size_t>(i)] = best;
        }

        const std::size_t portalCount = graph.PortalCount();
        if (portalCount == 0) return false;
        constexpr std::uint32_t kInfinity = 0xFFFFFFFFu;
        scratch.gScore.assign(portalCount, kInfinity);
        scratch.cameFrom.assign(portalCount, -1);
        scratch.closed.assign(portalCount, 0);
        scratch.heap.clear();

        const int goalX = goalTile % tiles;
        const int goalY = goalTile / tiles;
        const auto heuristic = [&](std::int32_t portalId) {
            const Portal& portal = graph.Portals()[static_cast<std::size_t>(portalId)];
            const std::int32_t tile = portal.repTile[0];
            return OctileDistance(tile % tiles, tile / tiles, goalX, goalY);
        };
        const auto push = [&](std::int32_t portalId, std::uint32_t f) {
            scratch.heap.push_back((static_cast<std::uint64_t>(f) << 32) |
                                   static_cast<std::uint32_t>(portalId));
            std::push_heap(scratch.heap.begin(), scratch.heap.end(), std::greater<>());
        };

        for (int i = 0; i < startPortalCount; ++i)
        {
            if (startCosts[static_cast<std::size_t>(i)] >= kUnreachable) continue;
            const std::int32_t portalId = startPortals[i];
            scratch.gScore[static_cast<std::size_t>(portalId)] = startCosts[static_cast<std::size_t>(i)];
            push(portalId, startCosts[static_cast<std::size_t>(i)] + heuristic(portalId));
        }

        std::int32_t bestFinalPortal = -1;
        std::uint32_t bestTotal = kInfinity;

        while (!scratch.heap.empty())
        {
            std::pop_heap(scratch.heap.begin(), scratch.heap.end(), std::greater<>());
            const std::uint64_t packed = scratch.heap.back();
            scratch.heap.pop_back();
            const auto portalId = static_cast<std::int32_t>(packed & 0xFFFFFFFFu);
            const auto f = static_cast<std::uint32_t>(packed >> 32);
            if (f >= bestTotal) break;                       // nothing left can beat the answer
            if (scratch.closed[static_cast<std::size_t>(portalId)]) continue;
            scratch.closed[static_cast<std::size_t>(portalId)] = 1;

            const std::uint32_t g = scratch.gScore[static_cast<std::size_t>(portalId)];
            const Portal& portal = graph.Portals()[static_cast<std::size_t>(portalId)];

            // Reaching the goal sector is not the end of the search: a different portal into the
            // same sector may still be cheaper once the within-sector leg is added.
            for (int side = 0; side < 2; ++side)
            {
                if (portal.sector[side] != goalSector) continue;
                const std::uint16_t leg = goalCosts[static_cast<std::size_t>(portal.localIndex[side])];
                if (leg >= kUnreachable) continue;
                if (g + leg < bestTotal)
                {
                    bestTotal = g + leg;
                    bestFinalPortal = portalId;
                }
            }

            for (int side = 0; side < 2; ++side)
            {
                const int sector = portal.sector[side];
                int count = 0;
                const std::int32_t* list = graph.SectorPortals(sector, count);
                const int fromLocal = portal.localIndex[side];
                for (int j = 0; j < count; ++j)
                {
                    const std::int32_t nextId = list[j];
                    if (nextId == portalId) continue;
                    if (scratch.closed[static_cast<std::size_t>(nextId)]) continue;
                    const std::uint16_t step = graph.PortalDistance(sector, fromLocal, j);
                    if (step >= kUnreachable) continue;
                    const std::uint32_t candidate = g + step;
                    if (candidate >= scratch.gScore[static_cast<std::size_t>(nextId)]) continue;
                    scratch.gScore[static_cast<std::size_t>(nextId)] = candidate;
                    scratch.cameFrom[static_cast<std::size_t>(nextId)] = portalId;
                    push(nextId, candidate + heuristic(nextId));
                }
            }
        }

        if (bestFinalPortal < 0) return false;

        // Walk the parent chain back to a start-sector portal, then replay it forwards.
        std::vector<std::int32_t> chain;
        for (std::int32_t at = bestFinalPortal; at >= 0;
             at = scratch.cameFrom[static_cast<std::size_t>(at)])
        {
            chain.push_back(at);
            if (chain.size() > portalCount) return false;   // cycle: a defect, not a map
        }
        std::reverse(chain.begin(), chain.end());

        int sector = startSector;
        for (const std::int32_t portalId : chain)
        {
            const Portal& portal = graph.Portals()[static_cast<std::size_t>(portalId)];
            out.steps.push_back({sector, portalId});
            sector = portal.sector[0] == sector ? portal.sector[1] : portal.sector[0];
        }
        out.steps.push_back({goalSector, -1});
        return true;
    }
}
