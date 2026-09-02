// SPDX-License-Identifier: MIT
#include "Formation.hpp"

#include <algorithm>

namespace CnaRts
{
    namespace
    {
        /** @brief Ceiling of the integer square root: the side of the smallest square holding n. */
        std::int32_t SquareSide(std::int32_t n)
        {
            if (n <= 1) return 1;
            auto side = static_cast<std::int32_t>(IntSqrt64(static_cast<std::uint64_t>(n)));
            while (side * side < n) ++side;
            return side;
        }

        /**
         * @brief A quarter turn of cosine in 32 steps, as exact Q16.16 constants.
         *
         * With the quadrant symmetry below this gives 128 directions around a circle, every one
         * of them an exact unit vector.
         *
         * Interpolating between eight cardinal directions was the first attempt and drew an
         * octagon: a linear blend of two unit vectors is 7.6% short at the midpoint, so every
         * ring came out visibly faceted. Spelling the table out also keeps the values identical
         * on every platform, which calling `std::cos` at start-up would not -- one ulp of
         * difference there is a different simulation.
         */
        constexpr std::int32_t kCosQuarter[33] = {
            65536, 65457, 65220, 64827, 64277, 63572,
            62714, 61705, 60547, 59244, 57798, 56212,
            54491, 52639, 50660, 48559, 46341, 44011,
            41576, 39040, 36410, 33692, 30893, 28020,
            25080, 22078, 19024, 15924, 12785,  9616,
             6424,  3216,     0,
        };

        /** @brief Unit vector at `angleIndex / 128` of a turn. */
        Vec2f DirectionAt(std::int32_t angleIndex)
        {
            const std::int32_t quadrant = (angleIndex >> 5) & 3;
            const std::int32_t step = angleIndex & 31;
            std::int32_t cosRaw = 0;
            std::int32_t sinRaw = 0;
            switch (quadrant)
            {
                case 0:  cosRaw =  kCosQuarter[step];      sinRaw =  kCosQuarter[32 - step]; break;
                case 1:  cosRaw = -kCosQuarter[32 - step]; sinRaw =  kCosQuarter[step];      break;
                case 2:  cosRaw = -kCosQuarter[step];      sinRaw = -kCosQuarter[32 - step]; break;
                default: cosRaw =  kCosQuarter[32 - step]; sinRaw = -kCosQuarter[step];      break;
            }
            return {Fixed::FromRaw(cosRaw), Fixed::FromRaw(sinRaw)};
        }
    }

    const char* FormationName(FormationKind kind)
    {
        switch (kind)
        {
            case FormationKind::Blob:   return "blob";
            case FormationKind::Line:   return "line";
            case FormationKind::Column: return "column";
            case FormationKind::Wedge:  return "wedge";
            case FormationKind::Box:    return "box";
            case FormationKind::Circle: return "circle";
            default:                    return "?";
        }
    }

    Vec2f FormationSlotOffset(FormationKind kind, std::int32_t slot, std::int32_t total,
                              Fixed spacing)
    {
        if (total <= 0) return {};
        total = std::min(total, kFormationSlots);
        slot = slot % total;

        switch (kind)
        {
            case FormationKind::Blob:
                return {};

            case FormationKind::Line:
            {
                // Four times as wide as it is deep: a rank, not a square.
                const std::int32_t perRank = std::max<std::int32_t>(1, SquareSide(total) * 4);
                const std::int32_t rank = slot / perRank;
                const std::int32_t file = slot % perRank;
                const std::int32_t width = std::min(total, perRank);
                return {spacing * (file - width / 2), spacing * -rank};
            }

            case FormationKind::Column:
            {
                // The mirror image of the line: deep and narrow.
                const std::int32_t perRank = std::max<std::int32_t>(1, SquareSide(total) / 3);
                const std::int32_t rank = slot / perRank;
                const std::int32_t file = slot % perRank;
                return {spacing * (file - perRank / 2), spacing * -rank};
            }

            case FormationKind::Wedge:
            {
                // Rank r holds 2r + 1 slots, so the first r ranks hold exactly r*r of them and
                // the rank is the integer square root -- no search.
                //
                // The obvious loop over ranks costs O(sqrt(slot)), up to forty-five iterations
                // here, and this runs once per unit per tick for every unit inside its
                // formation's envelope. At a million units that loop alone measured about ten
                // milliseconds a tick.
                const auto rank = static_cast<std::int32_t>(
                    IntSqrt64(static_cast<std::uint64_t>(slot)));
                const std::int32_t file = slot - rank * rank - rank;
                return {spacing * file, spacing * -rank};
            }

            case FormationKind::Box:
            {
                const std::int32_t side = SquareSide(total);
                const std::int32_t row = slot / side;
                const std::int32_t col = slot % side;
                return {spacing * (col - side / 2), spacing * (row - side / 2)};
            }

            case FormationKind::Circle:
            {
                // Concentric rings. Ring r holds 6r slots (ring 0 holds one), so the first r
                // rings hold 1 + 3r(r-1); inverting that quadratic gives the ring directly, for
                // the same reason the wedge above does not search for its rank. The two
                // correction steps absorb the integer root's rounding and run at most once each.
                const auto root = static_cast<std::int32_t>(
                    IntSqrt64(static_cast<std::uint64_t>(9 + 12 * std::max(slot - 1, 0))));
                std::int32_t ring = slot == 0 ? 0 : (3 + root) / 6;
                while (ring > 0 && 1 + 3 * ring * (ring - 1) > slot) --ring;
                while (1 + 3 * (ring + 1) * ring <= slot) ++ring;

                const std::int32_t consumed = ring == 0 ? 0 : 1 + 3 * ring * (ring - 1);
                const std::int32_t capacity = ring == 0 ? 1 : 6 * ring;
                const std::int32_t within = slot - consumed;
                const Vec2f direction = DirectionAt((within * 128) / std::max(capacity, 1));
                const Fixed radius = spacing * ring;
                return {direction.x * radius, direction.y * radius};
            }

            default:
                return {};
        }
    }

    Fixed FormationSpacingFor(std::int32_t total, Fixed minimum)
    {
        if (total <= 0) return minimum;
        // The ground the group needs, at the same density the map is generated for.
        const auto footprint = static_cast<std::int32_t>(
            IntSqrt64(static_cast<std::uint64_t>(total) * 3u / 2u));
        const std::int32_t slotsPerSide = SquareSide(std::min(total, kFormationSlots));
        const Fixed spacing = Fixed::FromRatio(std::max(footprint, 1), std::max(slotsPerSide, 1));
        return spacing.raw > minimum.raw ? spacing : minimum;
    }

    Fixed FormationRadius(FormationKind kind, std::int32_t total, Fixed spacing)
    {
        if (total <= 0) return kFixedZero;
        total = std::min(total, kFormationSlots);
        switch (kind)
        {
            case FormationKind::Blob:
                return spacing * std::max<std::int32_t>(2, SquareSide(total) / 2);
            case FormationKind::Line:
                return spacing * std::max<std::int32_t>(2, SquareSide(total) * 2);
            case FormationKind::Circle:
                return spacing * std::max<std::int32_t>(2, SquareSide(total) / 2 + 1);
            default:
                return spacing * std::max<std::int32_t>(2, SquareSide(total));
        }
    }
}
