// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "Fixed.hpp"

namespace CnaRts
{
    /** @brief The shape a group takes once it stops travelling and starts arriving. */
    enum class FormationKind : std::uint8_t
    {
        Blob   = 0,   ///< no slots at all: everyone crowds the goal
        Line   = 1,   ///< a wide rank facing the goal
        Column = 2,   ///< a deep marching column
        Wedge  = 3,   ///< a triangle, point forward
        Box    = 4,   ///< a filled rectangle
        Circle = 5,   ///< a ring, for holding ground
        Count  = 6,
    };

    const char* FormationName(FormationKind kind);

    /**
     * @brief Distinct slots any formation offers, however large the group is.
     *
     * Without a cap, shape and scale collide. A hundred and twenty-five thousand units in a
     * single rank is a line four hundred tiles wide -- wider than the map -- and a group whose
     * formation envelope is that big is *always* inside it, so it never marches, it only spreads.
     * Capping the slots means units past the cap share one, and at these densities that is
     * exactly right: sixty units assigned to the same slot pile up around it and separation
     * spreads them into a blob, so the formation reads as the shape of the whole army rather
     * than as one soldier per position.
     */
    constexpr std::int32_t kFormationSlots = 2048;

    /**
     * @brief The offset of one slot in formation-local space: +x is right, +y is forward.
     *
     * Slots are a pure function of `(kind, slot, total, spacing)` -- no state, no assignment
     * pass, no allocation. That is what makes a formation affordable at a hundred thousand units
     * per group: the slot a unit occupies is derived from its own index every tick, so there is
     * nothing to store and nothing to keep in sync when the group's membership changes.
     *
     * @param slot    The unit's index within its group.
     * @param total   Group size; used to size the shape.
     * @param spacing Distance between adjacent slots, in world units.
     */
    Vec2f FormationSlotOffset(FormationKind kind, std::int32_t slot, std::int32_t total,
                              Fixed spacing);

    /**
     * @brief Rotates a formation-local offset into world space.
     *
     * @param facing Unit-length direction the formation faces, i.e. its local +y axis.
     */
    inline Vec2f RotateToFacing(Vec2f local, Vec2f facing)
    {
        // Right = (facing.y, -facing.x), so world = local.x * right + local.y * facing.
        return {local.x * facing.y + local.y * facing.x,
                local.x * -facing.x + local.y * facing.y};
    }

    /** @brief Rough radius a group of @p total units in @p kind occupies, for the arrival test. */
    Fixed FormationRadius(FormationKind kind, std::int32_t total, Fixed spacing);

    /**
     * @brief Slot spacing that gives @p total units the ground they actually need.
     *
     * Slots are capped, so spacing has to carry the scale instead. A fixed spacing of two thirds
     * of a tile is right for a thousand soldiers and absurd for a hundred and twenty-five
     * thousand: the same two thousand slots then describe a patch eight tiles deep, into which
     * the group crams a hundred and thirty units per tile, and an army that should look like a
     * horde spread across a province renders as a thin bright line.
     *
     * This sizes the formation so its footprint matches the group's own natural footprint at the
     * map's density -- about one and a half tiles per unit -- and never goes below @p minimum.
     */
    Fixed FormationSpacingFor(std::int32_t total, Fixed minimum);
}
