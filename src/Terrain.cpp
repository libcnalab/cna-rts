// SPDX-License-Identifier: MIT
#include "Terrain.hpp"

#include <algorithm>
#include <vector>

namespace CnaRts
{
    namespace
    {
        /** @brief Bilinearly interpolated value noise on a coarse lattice, in fixed 16.16-ish
         *  integer arithmetic so the map is identical everywhere the seed is. */
        class ValueNoise
        {
        public:
            ValueNoise(int lattice, std::uint64_t seed) : lattice_(lattice)
            {
                Rng rng(seed);
                values_.resize(static_cast<std::size_t>((lattice_ + 1) * (lattice_ + 1)));
                for (std::int32_t& v : values_) v = static_cast<std::int32_t>(rng.NextBelow(1 << 16));
            }

            /** @param x,y  Tile coordinate. @param tiles Map size. @return 0..65535. */
            [[nodiscard]] std::int32_t Sample(int x, int y, int tiles) const
            {
                const std::int64_t scaled = static_cast<std::int64_t>(lattice_) * 65536;
                const std::int64_t fx = (static_cast<std::int64_t>(x) * scaled) / tiles;
                const std::int64_t fy = (static_cast<std::int64_t>(y) * scaled) / tiles;
                const int gx = static_cast<int>(fx >> 16);
                const int gy = static_cast<int>(fy >> 16);
                const std::int64_t tx = fx & 0xFFFF;
                const std::int64_t ty = fy & 0xFFFF;
                // Smoothstep on the fraction, so the lattice does not show as a diamond grid.
                const std::int64_t sx = (tx * tx * (3 * 65536 - 2 * tx)) >> 32;
                const std::int64_t sy = (ty * ty * (3 * 65536 - 2 * ty)) >> 32;

                const std::int64_t v00 = At(gx,     gy);
                const std::int64_t v10 = At(gx + 1, gy);
                const std::int64_t v01 = At(gx,     gy + 1);
                const std::int64_t v11 = At(gx + 1, gy + 1);
                const std::int64_t top    = v00 + (((v10 - v00) * sx) >> 16);
                const std::int64_t bottom = v01 + (((v11 - v01) * sx) >> 16);
                return static_cast<std::int32_t>(top + (((bottom - top) * sy) >> 16));
            }

        private:
            [[nodiscard]] std::int64_t At(int gx, int gy) const
            {
                gx = std::clamp(gx, 0, lattice_);
                gy = std::clamp(gy, 0, lattice_);
                return values_[static_cast<std::size_t>(gy) * (lattice_ + 1) + gx];
            }

            int lattice_;
            std::vector<std::int32_t> values_;
        };

        constexpr std::uint32_t Rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b)
        {
            return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
                   (static_cast<std::uint32_t>(b) << 16) | 0xFF000000u;
        }
    }

    void Terrain::Generate(int tiles, int sectorSize, std::uint64_t seed, int blockedPercent)
    {
        // Rounded down to a power of two so SectorIndex() is a shift rather than a division.
        sectorShift_ = 3;
        while ((1 << (sectorShift_ + 1)) <= std::clamp(sectorSize, 8, 64)) ++sectorShift_;
        sectorSize_ = 1 << sectorShift_;
        tiles_ = std::max(sectorSize_, (tiles / sectorSize_) * sectorSize_);
        sectorsPerSide_ = tiles_ >> sectorShift_;
        blockedPercent = std::clamp(blockedPercent, 0, 60);

        sectorOrigin_.resize(static_cast<std::size_t>(SectorCount()) * 2);
        for (int s = 0; s < SectorCount(); ++s)
        {
            sectorOrigin_[static_cast<std::size_t>(s) * 2]     = (s % sectorsPerSide_) << sectorShift_;
            sectorOrigin_[static_cast<std::size_t>(s) * 2 + 1] = (s / sectorsPerSide_) << sectorShift_;
        }

        const std::size_t tileCount = static_cast<std::size_t>(tiles_) * tiles_;
        passable_.assign(tileCount, 1);
        kind_.assign(tileCount, TileKind::Grass);

        // Two octaves: a coarse one that shapes continents and a fine one that roughens the
        // coast. Anything more elaborate would be prettier and no more interesting to path
        // through, which is what the map is for.
        const ValueNoise coarse(8, seed ^ 0xA24BAED4963EE407ull);
        const ValueNoise fine(std::max(16, tiles_ / 24), seed ^ 0x9E3779B97F4A7C15ull);

        std::vector<std::int32_t> height(tileCount);
        for (int y = 0; y < tiles_; ++y)
        {
            for (int x = 0; x < tiles_; ++x)
            {
                const std::int32_t h =
                    (coarse.Sample(x, y, tiles_) * 3 + fine.Sample(x, y, tiles_)) / 4;
                height[static_cast<std::size_t>(Index(x, y))] = h;
            }
        }

        // The thresholds are chosen from the *actual* height distribution rather than from fixed
        // constants: value noise is not uniformly distributed, so a fixed cut gives wildly
        // different blocked fractions at different seeds, and a benchmark whose obstacle density
        // wanders with the seed is not measuring one thing.
        std::vector<std::int32_t> sorted = height;
        std::sort(sorted.begin(), sorted.end());
        const auto quantile = [&](int permille) {
            const std::size_t at = std::min(sorted.size() - 1,
                                            static_cast<std::size_t>(sorted.size()) * permille / 1000);
            return sorted[at];
        };
        // Water takes two thirds of the blocked budget from the low end, rock the rest from the high.
        const int waterPermille = blockedPercent * 10 * 2 / 3;
        const int rockPermille  = blockedPercent * 10 - waterPermille;
        const std::int32_t waterLevel = quantile(waterPermille);
        const std::int32_t sandLevel  = quantile(waterPermille + 40);
        const std::int32_t dirtLevel  = quantile(1000 - rockPermille - 120);
        const std::int32_t rockLevel  = quantile(1000 - rockPermille);

        for (std::size_t i = 0; i < tileCount; ++i)
        {
            const std::int32_t h = height[i];
            if (blockedPercent > 0 && h <= waterLevel)      { kind_[i] = TileKind::Water; passable_[i] = 0; }
            else if (blockedPercent > 0 && h >= rockLevel)  { kind_[i] = TileKind::Rock;  passable_[i] = 0; }
            else if (h <= sandLevel)                        { kind_[i] = TileKind::Sand; }
            else if (h >= dirtLevel)                        { kind_[i] = TileKind::Dirt; }
            else                                            { kind_[i] = TileKind::Grass; }
        }

        FloodFillLargestRegion();

        passableTiles_.clear();
        passableTiles_.reserve(tileCount);
        for (std::size_t i = 0; i < tileCount; ++i)
            if (passable_[i]) passableTiles_.push_back(static_cast<std::int32_t>(i));

        // The byte array was the convenient shape to generate into; the bitset is the shape the
        // movement pass wants to read from. Pack once, then let the bytes go.
        passableBits_.assign((tileCount + 63) / 64, 0);
        for (std::size_t i = 0; i < tileCount; ++i)
            if (passable_[i]) passableBits_[i >> 6] |= std::uint64_t{1} << (i & 63);

        BuildColorMap();
        std::vector<std::uint8_t>().swap(passable_);
    }

    void Terrain::FloodFillLargestRegion()
    {
        // A map with several disconnected pockets is a map where a perfectly correct path query
        // has no answer, and "the pathfinder returned nothing" would then be indistinguishable
        // from a bug. Everything outside the largest connected region becomes rock, so every
        // passable tile can reach every other one and a failed query is always a real defect.
        const std::size_t tileCount = passable_.size();
        std::vector<std::int32_t> region(tileCount, -1);
        std::vector<std::int32_t> stack;
        std::int32_t bestRegion = -1;
        std::size_t bestSize = 0;
        std::int32_t regionId = 0;

        for (int y = 0; y < tiles_; ++y)
        {
            for (int x = 0; x < tiles_; ++x)
            {
                const std::int32_t start = Index(x, y);
                if (!passable_[static_cast<std::size_t>(start)] ||
                    region[static_cast<std::size_t>(start)] >= 0)
                    continue;

                std::size_t size = 0;
                stack.clear();
                stack.push_back(start);
                region[static_cast<std::size_t>(start)] = regionId;
                while (!stack.empty())
                {
                    const std::int32_t at = stack.back();
                    stack.pop_back();
                    ++size;
                    const int ax = at % tiles_;
                    const int ay = at / tiles_;
                    const int dx[4] = {1, -1, 0, 0};
                    const int dy[4] = {0, 0, 1, -1};
                    for (int d = 0; d < 4; ++d)
                    {
                        const int nx = ax + dx[d];
                        const int ny = ay + dy[d];
                        if (!InBounds(nx, ny)) continue;
                        const std::int32_t ni = Index(nx, ny);
                        if (!passable_[static_cast<std::size_t>(ni)]) continue;
                        if (region[static_cast<std::size_t>(ni)] >= 0) continue;
                        region[static_cast<std::size_t>(ni)] = regionId;
                        stack.push_back(ni);
                    }
                }
                if (size > bestSize) { bestSize = size; bestRegion = regionId; }
                ++regionId;
            }
        }

        for (std::size_t i = 0; i < tileCount; ++i)
        {
            if (passable_[i] && region[i] != bestRegion)
            {
                passable_[i] = 0;
                kind_[i] = TileKind::Rock;
            }
        }
    }

    void Terrain::BuildColorMap()
    {
        static const std::uint32_t kPalette[5] = {
            Rgba(28,  58, 102),   // Water
            Rgba(178, 162, 108),  // Sand
            Rgba(58,  92,  46),   // Grass
            Rgba(88,  74,  50),   // Dirt
            Rgba(78,  78,  84),   // Rock
        };

        colorMap_.resize(kind_.size());
        for (std::size_t i = 0; i < kind_.size(); ++i)
        {
            const std::uint32_t base = kPalette[static_cast<int>(kind_[i])];
            // A cheap deterministic dither keeps a 2048-tile map from reading as flat colour
            // fields when it is stretched over the screen. The index is narrowed to 32 bits
            // *before* the multiply: at 64 bits the product does not wrap, so the shift returns
            // thousands instead of the intended 0..7 and every tile past the first saturates to
            // white -- which is exactly what the first screenshot showed.
            const std::uint32_t jitter =
                (static_cast<std::uint32_t>(i) * 2654435761u) >> 29;
            const auto shade = [&](std::uint32_t channel, int shift) {
                const std::int32_t v = static_cast<std::int32_t>((channel >> shift) & 0xFF) +
                                       static_cast<std::int32_t>(jitter) - 4;
                return static_cast<std::uint32_t>(std::clamp(v, 0, 255)) << shift;
            };
            colorMap_[i] = shade(base, 0) | shade(base, 8) | shade(base, 16) | 0xFF000000u;
        }
    }
}
