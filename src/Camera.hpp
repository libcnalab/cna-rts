// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>

#include "Microsoft/Xna/Framework/Matrix.hpp"

namespace CnaRts
{
    /**
     * @brief A top-down orthographic camera over the tile map.
     *
     * World units are tiles, screen units are pixels, and `pixelsPerUnit` is the only thing
     * between them. The projection is built with the top edge above the bottom edge so that
     * world +y points down the screen, which is XNA's own screen convention and keeps the
     * SpriteBatch overlays and the instanced units agreeing about which way is north.
     */
    struct Camera
    {
        float centerX = 0.0f;
        float centerY = 0.0f;
        float pixelsPerUnit = 8.0f;
        int viewportWidth = 1280;
        int viewportHeight = 720;

        [[nodiscard]] float HalfWorldWidth() const
        {
            return static_cast<float>(viewportWidth) * 0.5f / pixelsPerUnit;
        }
        [[nodiscard]] float HalfWorldHeight() const
        {
            return static_cast<float>(viewportHeight) * 0.5f / pixelsPerUnit;
        }
        [[nodiscard]] float Left() const   { return centerX - HalfWorldWidth(); }
        [[nodiscard]] float Right() const  { return centerX + HalfWorldWidth(); }
        [[nodiscard]] float Top() const    { return centerY - HalfWorldHeight(); }
        [[nodiscard]] float Bottom() const { return centerY + HalfWorldHeight(); }

        [[nodiscard]] float ScreenX(float worldX) const
        {
            return (worldX - Left()) * pixelsPerUnit;
        }
        [[nodiscard]] float ScreenY(float worldY) const
        {
            return (worldY - Top()) * pixelsPerUnit;
        }
        [[nodiscard]] float WorldX(float screenX) const
        {
            return Left() + screenX / pixelsPerUnit;
        }
        [[nodiscard]] float WorldY(float screenY) const
        {
            return Top() + screenY / pixelsPerUnit;
        }

        /** @brief World-space to clip-space, y down. */
        [[nodiscard]] Microsoft::Xna::Framework::Matrix Projection() const
        {
            return Microsoft::Xna::Framework::Matrix::CreateOrthographicOffCenter(
                Left(), Right(), Bottom(), Top(), 0.0f, 1.0f);
        }

        /** @brief Keeps the view inside a square map of @p mapTiles tiles. */
        void ClampTo(int mapTiles)
        {
            const auto extent = static_cast<float>(mapTiles);
            // A map smaller than the window is centred rather than clamped to a corner.
            centerX = HalfWorldWidth() * 2.0f >= extent
                          ? extent * 0.5f
                          : std::clamp(centerX, HalfWorldWidth(), extent - HalfWorldWidth());
            centerY = HalfWorldHeight() * 2.0f >= extent
                          ? extent * 0.5f
                          : std::clamp(centerY, HalfWorldHeight(), extent - HalfWorldHeight());
        }
    };
}
