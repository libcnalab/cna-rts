// SPDX-License-Identifier: MIT
#include "TerrainRenderer.hpp"

#include <algorithm>
#include <cmath>

#include "Microsoft/Xna/Framework/Color.hpp"
#include "Palette.hpp"
#include "Microsoft/Xna/Framework/Graphics/SurfaceFormat.hpp"
#include "System/Diagnostics/Stopwatch.hpp"

using namespace Microsoft::Xna::Framework;
using namespace Microsoft::Xna::Framework::Graphics;

namespace CnaRts
{
    namespace
    {
        /** @brief Largest terrain texture edge. Above this the map is point-sampled down, which
         *  costs nothing visually: the zoom that would resolve individual tiles on a 4096-tile
         *  map shows a few hundred of them at a time. */
        constexpr int kMaxTerrainTexture = 2048;

        /** @brief Fog alpha for the three visibility states. */
        constexpr std::uint8_t kAlphaVisible = 0;
        constexpr std::uint8_t kAlphaExplored = 118;
        constexpr std::uint8_t kAlphaUnknown = 250;

        std::uint32_t Blend(std::uint32_t base, std::uint32_t tint, int weight)
        {
            const auto channel = [&](int shift) {
                const int a = static_cast<int>((base >> shift) & 0xFF);
                const int b = static_cast<int>((tint >> shift) & 0xFF);
                return static_cast<std::uint32_t>(a + ((b - a) * weight) / 255) << shift;
            };
            return channel(0) | channel(8) | channel(16) | 0xFF000000u;
        }

        inline double ElapsedMs(const System::Diagnostics::Stopwatch& watch)
        {
            return static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0;
        }
    }

    TerrainRenderer::TerrainRenderer() = default;
    TerrainRenderer::~TerrainRenderer() = default;

    void TerrainRenderer::Load(GraphicsDevice& device, const Simulation& simulation)
    {
        Unload();
        const Terrain& terrain = simulation.GetTerrain();
        const int tiles = terrain.Size();
        const int step = std::max(1, (tiles + kMaxTerrainTexture - 1) / kMaxTerrainTexture);
        terrainSize_ = tiles / step;
        cellsPerSide_ = simulation.Grid().CellsPerSide();

        terrain_ = std::make_unique<Texture2D>(device, terrainSize_, terrainSize_,
                                               false, SurfaceFormat::Color);

        // Uploaded a band at a time through one reusable buffer. A single upload would need a
        // four-million-element Color array, and Color is a polymorphic C++ object here rather
        // than the blittable value type XNA has, so that array would be tens of megabytes of
        // vtable pointers on the way to sixteen megabytes of pixels.
        const std::vector<std::uint32_t>& colors = terrain.ColorMap();
        constexpr int kBandRows = 64;
        std::vector<Color> band(static_cast<std::size_t>(terrainSize_) * kBandRows);
        for (int y0 = 0; y0 < terrainSize_; y0 += kBandRows)
        {
            const int rows = std::min(kBandRows, terrainSize_ - y0);
            for (int y = 0; y < rows; ++y)
            {
                for (int x = 0; x < terrainSize_; ++x)
                {
                    const std::size_t source =
                        static_cast<std::size_t>((y0 + y) * step) * tiles + static_cast<std::size_t>(x) * step;
                    band[static_cast<std::size_t>(y) * terrainSize_ + x] = PackedColor(colors[source]);
                }
            }
            const Rectangle region(0, y0, terrainSize_, rows);
            terrain_->SetData(0, &region, band.data(), 0, terrainSize_ * rows);
        }

        fog_ = std::make_unique<Texture2D>(device, cellsPerSide_, cellsPerSide_,
                                           false, SurfaceFormat::Color);
        minimap_ = std::make_unique<Texture2D>(device, cellsPerSide_, cellsPerSide_,
                                               false, SurfaceFormat::Color);
        scratch_.assign(static_cast<std::size_t>(cellsPerSide_) * cellsPerSide_, PackedColor(0u));

        // The minimap's static layer: the average terrain colour of each spatial-grid cell, so
        // the per-frame work is only the armies and the fog on top of it.
        minimapBase_.assign(static_cast<std::size_t>(cellsPerSide_) * cellsPerSide_, 0);
        const int cellSize = simulation.Grid().CellSizeTiles();
        for (int cy = 0; cy < cellsPerSide_; ++cy)
        {
            for (int cx = 0; cx < cellsPerSide_; ++cx)
            {
                std::uint32_t r = 0, g = 0, b = 0, samples = 0;
                for (int y = cy * cellSize; y < (cy + 1) * cellSize; y += 2)
                {
                    for (int x = cx * cellSize; x < (cx + 1) * cellSize; x += 2)
                    {
                        if (!terrain.InBounds(x, y)) continue;
                        const std::uint32_t c =
                            colors[static_cast<std::size_t>(terrain.Index(x, y))];
                        r += c & 0xFF;
                        g += (c >> 8) & 0xFF;
                        b += (c >> 16) & 0xFF;
                        ++samples;
                    }
                }
                if (samples == 0) samples = 1;
                minimapBase_[static_cast<std::size_t>(cy) * cellsPerSide_ + cx] =
                    (r / samples) | ((g / samples) << 8) | ((b / samples) << 16) | 0xFF000000u;
            }
        }

        Refresh(simulation, 0, true);
    }

    void TerrainRenderer::Unload()
    {
        terrain_.reset();
        fog_.reset();
        minimap_.reset();
        minimapBase_.clear();
        scratch_.clear();
    }

    void TerrainRenderer::Refresh(const Simulation& simulation, int viewerTeam, bool fogEnabled)
    {
        if (!fog_ || !minimap_) return;
        System::Diagnostics::Stopwatch watch = System::Diagnostics::Stopwatch::StartNew();

        const FogOfWar& fog = simulation.Fog();
        const SpatialGrid& grid = simulation.Grid();
        const int cells = cellsPerSide_ * cellsPerSide_;
        const bool reveal = !fogEnabled || viewerTeam < 0;
        const std::vector<std::uint8_t>& shade =
            fog.Shade(reveal ? 0 : viewerTeam);

        for (int c = 0; c < cells; ++c)
        {
            std::uint8_t alpha = kAlphaVisible;
            if (!reveal)
            {
                const std::uint8_t value = shade[static_cast<std::size_t>(c)];
                alpha = value >= 200 ? kAlphaVisible
                                     : (value > 0 ? kAlphaExplored : kAlphaUnknown);
            }
            scratch_[static_cast<std::size_t>(c)] =
                PackedColor(static_cast<std::uint32_t>(alpha) << 24);
        }
        fog_->SetData(scratch_.data(), cells);

        // Minimap: terrain, then whichever team owns each cell, then the fog on top. Team
        // presence comes from the grid's own per-cell tally, so the minimap costs nothing per
        // unit -- it is the same trick fog of war uses, for the same reason.
        static const std::uint32_t kTeamTint[kMaxTeams] = {
            0xFFFF9C59u,   // blue team, in ABGR
            0xFF4C55FFu,   // red team
            0xFF59E559u,   // green team
            0xFF6E6E6Eu,   // the fallen
        };
        for (int c = 0; c < cells; ++c)
        {
            std::uint32_t pixel = minimapBase_[static_cast<std::size_t>(c)];
            int best = -1;
            std::uint16_t bestCount = 0;
            for (int t = 0; t < kMaxTeams; ++t)
            {
                const std::uint16_t population = grid.TeamInCell(c, t);
                if (population > bestCount) { bestCount = population; best = t; }
            }
            if (best >= 0)
            {
                const int weight = std::min<int>(255, 90 + bestCount * 12);
                pixel = Blend(pixel, kTeamTint[best], weight);
            }
            if (!reveal)
            {
                const std::uint8_t value = shade[static_cast<std::size_t>(c)];
                if (value == 0) pixel = 0xFF000000u;
                else if (value < 200) pixel = Blend(pixel, 0xFF000000u, 130);
            }
            scratch_[static_cast<std::size_t>(c)] = PackedColor(pixel);
        }
        minimap_->SetData(scratch_.data(), cells);

        watch.Stop();
        refreshMs_ = ElapsedMs(watch);
    }

    void TerrainRenderer::DrawTerrain(SpriteBatch& batch, const Camera& camera, int mapTiles) const
    {
        if (!terrain_) return;
        const auto extent = static_cast<float>(mapTiles);
        const Rectangle dest(
            static_cast<int>(std::floor(camera.ScreenX(0.0f))),
            static_cast<int>(std::floor(camera.ScreenY(0.0f))),
            static_cast<int>(std::ceil(extent * camera.pixelsPerUnit)),
            static_cast<int>(std::ceil(extent * camera.pixelsPerUnit)));
        batch.Draw(*terrain_, dest, terrain_->getBoundsProperty(), PackedColor(0xFFFFFFFFu));
    }

    void TerrainRenderer::DrawFog(SpriteBatch& batch, const Camera& camera, int mapTiles) const
    {
        if (!fog_) return;
        const auto extent = static_cast<float>(mapTiles);
        const Rectangle dest(
            static_cast<int>(std::floor(camera.ScreenX(0.0f))),
            static_cast<int>(std::floor(camera.ScreenY(0.0f))),
            static_cast<int>(std::ceil(extent * camera.pixelsPerUnit)),
            static_cast<int>(std::ceil(extent * camera.pixelsPerUnit)));
        batch.Draw(*fog_, dest, fog_->getBoundsProperty(), PackedColor(0xFFFFFFFFu));
    }

    void TerrainRenderer::DrawMinimap(SpriteBatch& batch, const Rectangle& bounds,
                                      const Camera& camera, int mapTiles,
                                      const Texture2D& pixel) const
    {
        if (!minimap_) return;
        const Rectangle outer(bounds.X - 2, bounds.Y - 2,
                              bounds.Width + 4, bounds.Height + 4);
        batch.Draw(pixel, outer, PackedColor(0xC0101010u));
        batch.Draw(*minimap_, bounds, minimap_->getBoundsProperty(), PackedColor(0xFFFFFFFFu));

        // The view rectangle, so the minimap says where the camera is as well as what is on the map.
        const auto extent = static_cast<float>(mapTiles);
        const float scale = static_cast<float>(bounds.Width) / extent;
        const int viewX = bounds.X +
                          static_cast<int>(std::clamp(camera.Left(), 0.0f, extent) * scale);
        const int viewY = bounds.Y +
                          static_cast<int>(std::clamp(camera.Top(), 0.0f, extent) * scale);
        const int viewW = std::max(2, static_cast<int>(
            std::min(camera.Right(), extent) * scale) - (viewX - bounds.X));
        const int viewH = std::max(2, static_cast<int>(
            std::min(camera.Bottom(), extent) * scale) - (viewY - bounds.Y));
        const Color outline = PackedColor(0xFFFFFFFFu);
        batch.Draw(pixel, Rectangle(viewX, viewY, viewW, 1), outline);
        batch.Draw(pixel, Rectangle(viewX, viewY + viewH - 1, viewW, 1), outline);
        batch.Draw(pixel, Rectangle(viewX, viewY, 1, viewH), outline);
        batch.Draw(pixel, Rectangle(viewX + viewW - 1, viewY, 1, viewH), outline);
    }
}
