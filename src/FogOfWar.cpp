// SPDX-License-Identifier: MIT
#include "FogOfWar.hpp"

#include <algorithm>

namespace CnaRts
{
    void FogOfWar::Configure(int cellsPerSide, int teams)
    {
        cellsPerSide_ = std::max(1, cellsPerSide);
        teams_ = std::clamp(teams, 1, kMaxTeams);
        const std::size_t cells = static_cast<std::size_t>(cellsPerSide_) * cellsPerSide_;

        distance_.assign(static_cast<std::size_t>(teams_), std::vector<std::uint16_t>(cells, 0));
        explored_.assign(static_cast<std::size_t>(teams_), std::vector<std::uint8_t>(cells, 0));
        shade_.assign(static_cast<std::size_t>(teams_), std::vector<std::uint8_t>(cells, 0));
        visibleCells_.assign(static_cast<std::size_t>(teams_), 0);
        exploredCells_.assign(static_cast<std::size_t>(teams_), 0);
    }

    void FogOfWar::Reset()
    {
        for (auto& map : explored_) std::fill(map.begin(), map.end(), 0);
        for (auto& map : shade_)    std::fill(map.begin(), map.end(), 0);
        std::fill(visibleCells_.begin(), visibleCells_.end(), 0);
        std::fill(exploredCells_.begin(), exploredCells_.end(), 0);
    }

    void FogOfWar::Update(JobSystem& jobs, const SpatialGrid& grid, int visionRadiusCells)
    {
        if (cellsPerSide_ != grid.CellsPerSide()) Configure(grid.CellsPerSide(), teams_);
        const int radius = std::max(1, visionRadiusCells);

        // One team per chunk. The teams share nothing, so this is the whole synchronisation
        // story; the sweeps inside a team are raster-ordered and stay serial by nature.
        jobs.ParallelChunks(teams_, [&](int, std::int32_t begin, std::int32_t end) {
            for (std::int32_t t = begin; t < end; ++t)
                UpdateTeam(static_cast<int>(t), grid, radius);
        });
    }

    void FogOfWar::UpdateTeam(int team, const SpatialGrid& grid, int radius)
    {
        const int side = cellsPerSide_;
        std::vector<std::uint16_t>& distance = distance_[static_cast<std::size_t>(team)];
        std::vector<std::uint8_t>& explored = explored_[static_cast<std::size_t>(team)];
        std::vector<std::uint8_t>& shade = shade_[static_cast<std::size_t>(team)];

        constexpr std::uint16_t kFar = 0x7FFF;
        for (std::int32_t c = 0; c < static_cast<std::int32_t>(distance.size()); ++c)
            distance[static_cast<std::size_t>(c)] = grid.TeamInCell(c, team) > 0 ? 0 : kFar;

        // Forward sweep: each cell takes the best of the four already-settled neighbours above
        // and to the left. The backward sweep then does the other four, and two sweeps are
        // enough for the 3x3 chamfer mask to be exact for that mask.
        for (int y = 0; y < side; ++y)
        {
            for (int x = 0; x < side; ++x)
            {
                const std::size_t at = static_cast<std::size_t>(y) * side + x;
                std::uint16_t best = distance[at];
                if (best == 0) continue;
                const auto consider = [&](int nx, int ny, std::uint16_t weight) {
                    if (nx < 0 || ny < 0 || nx >= side || ny >= side) return;
                    const std::uint16_t candidate =
                        distance[static_cast<std::size_t>(ny) * side + nx] + weight;
                    if (candidate < best) best = candidate;
                };
                consider(x - 1, y,     kChamferOrthogonal);
                consider(x,     y - 1, kChamferOrthogonal);
                consider(x - 1, y - 1, kChamferDiagonal);
                consider(x + 1, y - 1, kChamferDiagonal);
                distance[at] = best;
            }
        }

        const auto visibleLimit = static_cast<std::uint16_t>(radius * kChamferOrthogonal);
        std::int32_t visibleCount = 0;
        std::int32_t exploredCount = 0;
        for (int y = side - 1; y >= 0; --y)
        {
            for (int x = side - 1; x >= 0; --x)
            {
                const std::size_t at = static_cast<std::size_t>(y) * side + x;
                std::uint16_t best = distance[at];
                if (best != 0)
                {
                    const auto consider = [&](int nx, int ny, std::uint16_t weight) {
                        if (nx < 0 || ny < 0 || nx >= side || ny >= side) return;
                        const std::uint16_t candidate =
                            distance[static_cast<std::size_t>(ny) * side + nx] + weight;
                        if (candidate < best) best = candidate;
                    };
                    consider(x + 1, y,     kChamferOrthogonal);
                    consider(x,     y + 1, kChamferOrthogonal);
                    consider(x + 1, y + 1, kChamferDiagonal);
                    consider(x - 1, y + 1, kChamferDiagonal);
                    distance[at] = best;
                }

                const bool visible = best <= visibleLimit;
                if (visible)
                {
                    ++visibleCount;
                    explored[at] = 1;
                    shade[at] = 255;
                }
                else
                {
                    shade[at] = explored[at] ? 96 : 0;
                }
                if (explored[at]) ++exploredCount;
            }
        }

        visibleCells_[static_cast<std::size_t>(team)] = visibleCount;
        exploredCells_[static_cast<std::size_t>(team)] = exploredCount;
    }
}
