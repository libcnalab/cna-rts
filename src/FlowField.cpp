// SPDX-License-Identifier: MIT
#include "FlowField.hpp"

#include <algorithm>

namespace CnaRts
{
    namespace
    {
        constexpr int kFlowDx[8] = { 1, -1,  0,  0,  1,  1, -1, -1};
        constexpr int kFlowDy[8] = { 0,  0,  1, -1,  1, -1,  1, -1};
    }

    // 0.70710678 in Q16.16 is 46341; the diagonals are pre-normalised so the movement pass never
    // divides by a length it already knows.
    const Vec2f kDirection8[9] = {
        {Fixed::FromRaw( 65536), Fixed::FromRaw(     0)},
        {Fixed::FromRaw(-65536), Fixed::FromRaw(     0)},
        {Fixed::FromRaw(     0), Fixed::FromRaw( 65536)},
        {Fixed::FromRaw(     0), Fixed::FromRaw(-65536)},
        {Fixed::FromRaw( 46341), Fixed::FromRaw( 46341)},
        {Fixed::FromRaw( 46341), Fixed::FromRaw(-46341)},
        {Fixed::FromRaw(-46341), Fixed::FromRaw( 46341)},
        {Fixed::FromRaw(-46341), Fixed::FromRaw(-46341)},
        {Fixed::FromRaw(     0), Fixed::FromRaw(     0)},   // kFlowArrived
    };

    void GroupNavigation::Configure(const Terrain& terrain)
    {
        sectorSize_ = terrain.SectorSize();
        sectorArea_ = sectorSize_ * sectorSize_;
        sectorCount_ = terrain.SectorCount();
        flowSlotOfSector_.assign(static_cast<std::size_t>(sectorCount_), -1);
        guideTile_.assign(static_cast<std::size_t>(sectorCount_), -1);
        flowData_.clear();
        hasPlan_ = false;
    }

    void GroupNavigation::Clear()
    {
        std::fill(flowSlotOfSector_.begin(), flowSlotOfSector_.end(), -1);
        std::fill(guideTile_.begin(), guideTile_.end(), -1);
        hasPlan_ = false;
    }

    void GroupNavigation::Plan(const Terrain& terrain, const SectorGraph& graph,
                               const Corridor& corridor, PathfinderScratch& scratch)
    {
        std::fill(flowSlotOfSector_.begin(), flowSlotOfSector_.end(), -1);
        flowData_.clear();
        goalTile_ = corridor.goalTile;
        hasPlan_ = !corridor.Empty();
        if (!hasPlan_) return;

        const int tiles = terrain.Size();
        flowData_.resize(corridor.steps.size() * static_cast<std::size_t>(sectorArea_), kFlowArrived);

        std::int32_t slot = 0;
        for (const CorridorStep& step : corridor.steps)
        {
            const int ox = terrain.SectorOriginX(step.sector);
            const int oy = terrain.SectorOriginY(step.sector);

            scratch.seeds.clear();
            if (step.exitPortal >= 0)
            {
                // The whole span, not the representative tile. A field seeded from one tile makes
                // every unit in the sector aim at the same tile, and a doorway sixteen tiles wide
                // is then used one tile wide.
                const Portal& portal = graph.Portals()[static_cast<std::size_t>(step.exitPortal)];
                const int side = portal.sector[0] == step.sector ? 0 : 1;
                const std::int32_t stride = portal.vertical ? tiles : 1;
                for (int i = 0; i < portal.spanLength; ++i)
                {
                    const std::int32_t tile = portal.spanStart[side] + stride * i;
                    scratch.seeds.push_back((tile / tiles - oy) * sectorSize_ + (tile % tiles - ox));
                }
            }
            else
            {
                scratch.seeds.push_back((corridor.goalTile / tiles - oy) * sectorSize_ +
                                        (corridor.goalTile % tiles - ox));
            }

            IntegrateRegion(terrain, ox, oy, sectorSize_, sectorSize_, scratch.seeds, scratch.field);

            std::uint8_t* flow = flowData_.data() + static_cast<std::size_t>(slot) * sectorArea_;
            for (int ly = 0; ly < sectorSize_; ++ly)
            {
                for (int lx = 0; lx < sectorSize_; ++lx)
                {
                    const std::int32_t local = ly * sectorSize_ + lx;
                    const std::uint16_t here = scratch.field[static_cast<std::size_t>(local)];
                    if (here == 0 || here == kUnreachable) { flow[local] = kFlowArrived; continue; }

                    std::uint16_t best = here;
                    std::uint8_t bestDir = kFlowArrived;
                    for (int d = 0; d < 8; ++d)
                    {
                        const int nlx = lx + kFlowDx[d];
                        const int nly = ly + kFlowDy[d];
                        if (nlx < 0 || nly < 0 || nlx >= sectorSize_ || nly >= sectorSize_) continue;
                        if (d >= 4)
                        {
                            if (!terrain.Passable(ox + lx + kFlowDx[d], oy + ly)) continue;
                            if (!terrain.Passable(ox + lx, oy + ly + kFlowDy[d])) continue;
                        }
                        const std::uint16_t neighbour =
                            scratch.field[static_cast<std::size_t>(nly * sectorSize_ + nlx)];
                        if (neighbour < best) { best = neighbour; bestDir = static_cast<std::uint8_t>(d); }
                    }
                    flow[local] = bestDir;
                }
            }

            flowSlotOfSector_[static_cast<std::size_t>(step.sector)] = slot;
            ++slot;
        }

        BuildGuidance(terrain, graph, corridor);
    }

    void GroupNavigation::BuildGuidance(const Terrain& terrain, const SectorGraph& graph,
                                        const Corridor& corridor)
    {
        std::fill(guideTile_.begin(), guideTile_.end(), -1);
        bfsQueue_.clear();
        bfsQueue_.reserve(static_cast<std::size_t>(sectorCount_));

        // Corridor sectors are their own guidance: a unit standing in one already has a field.
        for (const CorridorStep& step : corridor.steps)
        {
            if (guideTile_[static_cast<std::size_t>(step.sector)] >= 0) continue;
            guideTile_[static_cast<std::size_t>(step.sector)] = corridor.goalTile;
            bfsQueue_.push_back(step.sector);
        }

        // Breadth-first over the sector adjacency the portals define, so every sector on the map
        // learns which doorway leads back toward the plan. Sector-level, not tile-level: four
        // thousand nodes, once per order.
        for (std::size_t head = 0; head < bfsQueue_.size(); ++head)
        {
            const int sector = bfsQueue_[head];
            int count = 0;
            const std::int32_t* list = graph.SectorPortals(sector, count);
            for (int i = 0; i < count; ++i)
            {
                const Portal& portal = graph.Portals()[static_cast<std::size_t>(list[i])];
                const int side = portal.sector[0] == sector ? 0 : 1;
                const int other = portal.sector[1 - side];
                if (guideTile_[static_cast<std::size_t>(other)] >= 0) continue;
                // The doorway itself, on the far sector's side: a unit there walks to the portal
                // and crosses, rather than at the middle of a neighbouring sector's rock face.
                guideTile_[static_cast<std::size_t>(other)] = portal.repTile[1 - side];
                bfsQueue_.push_back(other);
            }
        }

        (void) terrain;
    }
}
