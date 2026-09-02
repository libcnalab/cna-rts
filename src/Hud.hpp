// SPDX-License-Identifier: MIT
#pragma once

#include <deque>
#include <string>

#include "Microsoft/Xna/Framework/Graphics/SpriteBatch.hpp"

#include "Camera.hpp"
#include "Simulation.hpp"
#include "TerrainRenderer.hpp"
#include "TextRenderer.hpp"
#include "UnitRenderer.hpp"

namespace CnaRts
{
    /** @brief Rolling frame-time record, so the HUD can show a number that is not noise. */
    class FrameTimeline
    {
    public:
        void Push(double frameMs);
        void Reset();

        [[nodiscard]] double AverageMs() const;
        [[nodiscard]] double PercentileMs(double percentile) const;
        [[nodiscard]] double Fps() const;
        [[nodiscard]] std::size_t Count() const { return samples_.size(); }
        [[nodiscard]] const std::deque<double>& Samples() const { return samples_; }

        static constexpr std::size_t kWindow = 180;

    private:
        std::deque<double> samples_;
        double sum_ = 0.0;
    };

    /** @brief Everything the overlay reports about one frame. */
    struct HudFrame
    {
        double frameMs = 0.0;
        double simMs = 0.0;
        double renderMs = 0.0;
        double ticksPerSecond = 0.0;
        int viewerTeam = 0;
        int selectedGroup = -1;
        bool fogEnabled = true;
        bool paused = false;
        bool showHelp = false;
    };

    /** @brief The on-screen instrumentation: counters, phase timings, minimap and help. */
    class Hud
    {
    public:
        void Draw(Microsoft::Xna::Framework::Graphics::SpriteBatch& batch,
                  const TextRenderer& text, const TerrainRenderer& terrain,
                  const Simulation& simulation, const UnitRenderStats& unitStats,
                  const FrameTimeline& timeline, const Camera& camera, const HudFrame& frame);

    private:
        void DrawPanel(Microsoft::Xna::Framework::Graphics::SpriteBatch& batch,
                       const TextRenderer& text, int x, int y, int width, int height) const;
        void DrawGraph(Microsoft::Xna::Framework::Graphics::SpriteBatch& batch,
                       const TextRenderer& text, const FrameTimeline& timeline,
                       int x, int y, int width, int height) const;
    };

    /** @brief Formats a count as 1234, 12.3k or 1.20M -- the HUD has no room for a million digits. */
    std::string FormatCount(std::int64_t value);
    /** @brief Formats a millisecond figure to two decimals. */
    std::string FormatMs(double value);
}
