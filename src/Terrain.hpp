// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

#include "Rng.hpp"

namespace CnaRts
{
    /** @brief What a tile is made of. Drives both passability and the terrain colour ramp. */
    enum class TileKind : std::uint8_t
    {
        Water = 0,   ///< impassable
        Sand  = 1,
        Grass = 2,
        Dirt  = 3,
        Rock  = 4,   ///< impassable
    };

    /**
     * @brief The static map: a square tile grid, its passability, and its sector decomposition.
     *
     * One tile is one world unit, so a tile coordinate is the integer part of a world
     * coordinate and no scale factor ever has to be remembered. The grid is generated from the
     * run's seed and never changes afterwards, which is what lets every pathfinding structure
     * built on top of it be cached for the whole run.
     */
    class Terrain
    {
    public:
        /**
         * @brief Generates a map.
         *
         * @param tiles      Width and height in tiles; forced to a multiple of @p sectorSize.
         * @param sectorSize Tiles per sector edge, the granularity of the coarse path graph.
         * @param seed       Run seed. The same seed always produces the same map.
         * @param blockedPercent Roughly how much of the map to make impassable, 0-60.
         */
        void Generate(int tiles, int sectorSize, std::uint64_t seed, int blockedPercent);

        [[nodiscard]] int Size() const { return tiles_; }
        [[nodiscard]] int SectorSize() const { return sectorSize_; }
        /** @brief log2 of SectorSize(); the sector size is forced to a power of two so the
         *  movement pass can divide by it with a shift. At a million lookups a tick, two integer
         *  divisions per unit is not a rounding error. */
        [[nodiscard]] int SectorShift() const { return sectorShift_; }
        [[nodiscard]] int SectorsPerSide() const { return sectorsPerSide_; }
        [[nodiscard]] int SectorCount() const { return sectorsPerSide_ * sectorsPerSide_; }

        [[nodiscard]] bool InBounds(int tx, int ty) const
        {
            return static_cast<unsigned>(tx) < static_cast<unsigned>(tiles_) &&
                   static_cast<unsigned>(ty) < static_cast<unsigned>(tiles_);
        }
        /**
         * @brief Whether a tile can be walked on.
         *
         * Passability is a bitset, not a byte array, and that is a measured decision rather than
         * a tidy one. The movement pass asks this two or three times per unit, at tile
         * coordinates scattered across the map, so the array is read randomly a few million times
         * a tick: at one byte per tile a 2048-tile map is 4 MB and every read is a cache miss,
         * while at one bit it is 512 KB and most reads are served by L2.
         */
        [[nodiscard]] bool Passable(int tx, int ty) const
        {
            if (!InBounds(tx, ty)) return false;
            return PassableIndex(Index(tx, ty));
        }
        [[nodiscard]] bool PassableIndex(std::int32_t index) const
        {
            return ((passableBits_[static_cast<std::size_t>(index) >> 6] >>
                     (static_cast<unsigned>(index) & 63u)) & 1u) != 0u;
        }
        [[nodiscard]] TileKind Kind(int tx, int ty) const { return kind_[Index(tx, ty)]; }

        [[nodiscard]] std::int32_t Index(int tx, int ty) const
        {
            return static_cast<std::int32_t>(ty) * tiles_ + tx;
        }
        [[nodiscard]] int SectorIndex(int tx, int ty) const
        {
            return ((ty >> sectorShift_) * sectorsPerSide_) + (tx >> sectorShift_);
        }
        [[nodiscard]] int SectorOriginX(int sector) const
        {
            return sectorOrigin_[static_cast<std::size_t>(sector) * 2];
        }
        [[nodiscard]] int SectorOriginY(int sector) const
        {
            return sectorOrigin_[static_cast<std::size_t>(sector) * 2 + 1];
        }

        /** @brief Every passable tile index, in row-major order: the spawn pool. */
        [[nodiscard]] const std::vector<std::int32_t>& PassableTiles() const { return passableTiles_; }

        /** @brief RGBA8 colour per tile, row-major, for the terrain texture. Built once. */
        [[nodiscard]] const std::vector<std::uint32_t>& ColorMap() const { return colorMap_; }

    private:
        void FloodFillLargestRegion();
        void BuildColorMap();

        int tiles_ = 0;
        int sectorSize_ = 32;
        int sectorShift_ = 5;
        int sectorsPerSide_ = 0;
        std::vector<std::uint64_t> passableBits_;
        std::vector<std::int32_t> sectorOrigin_;   ///< two entries per sector: origin x, origin y
        std::vector<std::uint8_t> passable_;       ///< generation-time scratch, freed afterwards
        std::vector<TileKind>     kind_;
        std::vector<std::int32_t> passableTiles_;
        std::vector<std::uint32_t> colorMap_;
    };
}
