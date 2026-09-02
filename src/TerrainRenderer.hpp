// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Microsoft/Xna/Framework/Rectangle.hpp"
#include "Microsoft/Xna/Framework/Graphics/GraphicsDevice.hpp"
#include "Microsoft/Xna/Framework/Graphics/SpriteBatch.hpp"
#include "Microsoft/Xna/Framework/Graphics/Texture2D.hpp"

#include "Camera.hpp"
#include "Simulation.hpp"

namespace CnaRts
{
    /**
     * @brief The ground, the fog over it, and the minimap that summarises both.
     *
     * All three are textures drawn through `SpriteBatch`, because all three are one quad each --
     * the instanced path exists for the million things, not for the three. The terrain texture is
     * built once; the fog and minimap textures are rebuilt on the simulation's fog interval, not
     * every frame, because neither changes faster than that.
     */
    class TerrainRenderer
    {
    public:
        TerrainRenderer();
        ~TerrainRenderer();

        TerrainRenderer(const TerrainRenderer&) = delete;
        TerrainRenderer& operator=(const TerrainRenderer&) = delete;

        void Load(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device,
                  const Simulation& simulation);
        void Unload();

        /** @brief Rebuilds the fog and minimap textures for @p viewerTeam. */
        void Refresh(const Simulation& simulation, int viewerTeam, bool fogEnabled);

        void DrawTerrain(Microsoft::Xna::Framework::Graphics::SpriteBatch& batch,
                         const Camera& camera, int mapTiles) const;
        void DrawFog(Microsoft::Xna::Framework::Graphics::SpriteBatch& batch,
                     const Camera& camera, int mapTiles) const;
        /** @brief Draws the minimap into @p bounds and outlines the camera's view rectangle. */
        void DrawMinimap(Microsoft::Xna::Framework::Graphics::SpriteBatch& batch,
                         const Microsoft::Xna::Framework::Rectangle& bounds, const Camera& camera,
                         int mapTiles,
                         const Microsoft::Xna::Framework::Graphics::Texture2D& pixel) const;

        /** @brief The fog texture, which the unit shader samples to hide what is not seen. */
        [[nodiscard]] const Microsoft::Xna::Framework::Graphics::Texture2D* FogTexture() const
        {
            return fog_.get();
        }

        [[nodiscard]] double LastRefreshMs() const { return refreshMs_; }

    private:
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::Texture2D> terrain_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::Texture2D> fog_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::Texture2D> minimap_;

        int terrainSize_ = 0;
        int cellsPerSide_ = 0;
        std::vector<std::uint32_t> minimapBase_;   ///< terrain colour per cell, built once
        std::vector<Microsoft::Xna::Framework::Color> scratch_;
        double refreshMs_ = 0.0;
    };
}
