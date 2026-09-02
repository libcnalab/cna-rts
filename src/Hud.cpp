// SPDX-License-Identifier: MIT
#include "Hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <vector>

#include "Microsoft/Xna/Framework/Color.hpp"
#include "Palette.hpp"
#include "Microsoft/Xna/Framework/Rectangle.hpp"

using namespace Microsoft::Xna::Framework;
using namespace Microsoft::Xna::Framework::Graphics;

namespace CnaRts
{
    void FrameTimeline::Push(double frameMs)
    {
        samples_.push_back(frameMs);
        sum_ += frameMs;
        while (samples_.size() > kWindow)
        {
            sum_ -= samples_.front();
            samples_.pop_front();
        }
    }

    void FrameTimeline::Reset()
    {
        samples_.clear();
        sum_ = 0.0;
    }

    double FrameTimeline::AverageMs() const
    {
        return samples_.empty() ? 0.0 : sum_ / static_cast<double>(samples_.size());
    }

    double FrameTimeline::PercentileMs(double percentile) const
    {
        if (samples_.empty()) return 0.0;
        std::vector<double> sorted(samples_.begin(), samples_.end());
        std::sort(sorted.begin(), sorted.end());
        const auto at = static_cast<std::size_t>(
            std::clamp(percentile, 0.0, 1.0) * static_cast<double>(sorted.size() - 1));
        return sorted[at];
    }

    double FrameTimeline::Fps() const
    {
        const double average = AverageMs();
        return average > 0.0 ? 1000.0 / average : 0.0;
    }

    std::string FormatCount(std::int64_t value)
    {
        char buffer[32];
        if (value < 10000)
            std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(value));
        else if (value < 1000000)
            std::snprintf(buffer, sizeof buffer, "%.1fK", static_cast<double>(value) / 1000.0);
        else
            std::snprintf(buffer, sizeof buffer, "%.2fM", static_cast<double>(value) / 1000000.0);
        return buffer;
    }

    std::string FormatMs(double value)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%.2f", value);
        return buffer;
    }

    namespace
    {
        std::string Format(const char* format, ...)
        {
            char buffer[256];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof buffer, format, args);
            va_end(args);
            return buffer;
        }
    }

    void Hud::DrawPanel(SpriteBatch& batch, const TextRenderer& text, int x, int y, int width,
                        int height) const
    {
        if (const Texture2D* pixel = text.WhitePixel())
            batch.Draw(*pixel, Rectangle(x, y, width, height), PackedColor(0xB4141414u));
    }

    void Hud::DrawGraph(SpriteBatch& batch, const TextRenderer& text, const FrameTimeline& timeline,
                        int x, int y, int width, int height) const
    {
        const Texture2D* pixel = text.WhitePixel();
        if (pixel == nullptr || timeline.Count() == 0) return;

        // Scaled to the worst frame in the window rather than to a fixed ceiling: a benchmark
        // that spends its life at 5 ms and a benchmark that spends it at 90 ms should both show
        // their *shape*, which a fixed axis flattens in one case and clips in the other.
        double worst = 1.0;
        for (const double sample : timeline.Samples()) worst = std::max(worst, sample);

        const auto count = static_cast<int>(timeline.Samples().size());
        const int columns = std::min(count, width);
        const int first = count - columns;
        for (int i = 0; i < columns; ++i)
        {
            const double sample = timeline.Samples()[static_cast<std::size_t>(first + i)];
            const int bar = std::max(1, static_cast<int>(sample / worst * height));
            // Green under 16.7 ms, amber under 33.3, red beyond: the 60 and 30 Hz budgets.
            const std::uint32_t colour = sample <= 16.7 ? 0xFF60D060u
                                       : (sample <= 33.4 ? 0xFF40C0F0u : 0xFF4040F0u);
            batch.Draw(*pixel, Rectangle(x + i, y + height - bar, 1, bar), PackedColor(colour));
        }

        // The 60 Hz line, where it falls on this scale.
        const int line = y + height - static_cast<int>(16.7 / worst * height);
        if (line > y && line < y + height)
            batch.Draw(*pixel, Rectangle(x, line, columns, 1), PackedColor(0x60FFFFFFu));
    }

    void Hud::Draw(SpriteBatch& batch, const TextRenderer& text, const TerrainRenderer& terrain,
                   const Simulation& simulation, const UnitRenderStats& unitStats,
                   const FrameTimeline& timeline, const Camera& camera, const HudFrame& frame)
    {
        const SimConfig& config = simulation.Config();
        const SimStats& sim = simulation.Stats();
        const int scale = 2;
        const int line = TextRenderer::LineHeight(scale);
        const int panelWidth = 430;
        const int panelHeight = 30 + line * 15;

        DrawPanel(batch, text, 8, 8, panelWidth, panelHeight);

        int y = 16;
        const auto row = [&](const std::string& label, std::uint32_t colour = 0xFFE6E6E6u) {
            text.DrawShadowed(batch, label, 18, y, scale, PackedColor(colour));
            y += line;
        };

        row(Format("CNA-RTS  %s UNITS  %dx%d TILES", FormatCount(config.unitCount).c_str(),
                   config.mapTiles, config.mapTiles),
            0xFF7FE0FFu);
        row(Format("FPS %5.1f   FRAME %s MS   P99 %s",
                   timeline.Fps(), FormatMs(timeline.AverageMs()).c_str(),
                   FormatMs(timeline.PercentileMs(0.99)).c_str()),
            0xFF80FF80u);
        row(Format("SIM %s/TICK  RENDER %s  %.1f TICKS/S",
                   FormatMs(sim.totalMs).c_str(), FormatMs(frame.renderMs).c_str(),
                   frame.ticksPerSecond));
        row("");
        row("SIMULATION (MS/TICK)", 0xFFB0B0FFu);
        row(Format("  PATH %s  GRID %s  MOVE %s", FormatMs(sim.pathMs).c_str(),
                   FormatMs(sim.gridMs).c_str(), FormatMs(sim.moveMs).c_str()));
        row(Format("  FIGHT %s  FOG %s  HASH %s", FormatMs(sim.combatMs).c_str(),
                   FormatMs(sim.fogMs).c_str(), FormatMs(sim.checksumMs).c_str()));
        row("");
        row("RENDER", 0xFFB0B0FFu);
        row(Format("  BUILD %s  UPLOAD %s  DRAW CALLS %d", FormatMs(unitStats.buildMs).c_str(),
                   FormatMs(unitStats.uploadMs).c_str(), unitStats.drawCalls));
        row(Format("  INSTANCES %s  STREAM %s MB  FOG %s",
                   FormatCount(unitStats.instances).c_str(),
                   FormatMs(static_cast<double>(unitStats.instances) * 8.0 / 1048576.0).c_str(),
                   FormatMs(terrain.LastRefreshMs()).c_str()));
        row("");
        row("STATE", 0xFFB0B0FFu);
        row(Format("  TICK %u   HASH %016llX", simulation.CurrentTick(),
                   static_cast<unsigned long long>(simulation.Checksum())));
        row(Format("  BLUE %s  RED %s  FALLEN %s  PATHS %s",
                   FormatCount(simulation.AliveOfTeam(0)).c_str(),
                   FormatCount(simulation.AliveOfTeam(1)).c_str(),
                   FormatCount(simulation.DeathCount()).c_str(),
                   FormatCount(simulation.PathQueryCount()).c_str()));

        DrawGraph(batch, text, timeline, 18, y + 4, panelWidth - 20, 46);

        // Minimap, bottom right.
        const int miniSize = 220;
        const Rectangle miniBounds(camera.viewportWidth - miniSize - 14,
                                   camera.viewportHeight - miniSize - 14, miniSize, miniSize);
        if (const Texture2D* pixel = text.WhitePixel())
            terrain.DrawMinimap(batch, miniBounds, camera, config.mapTiles, *pixel);

        // Status line, bottom left.
        const int statusY = camera.viewportHeight - line - 10;
        DrawPanel(batch, text, 8, statusY - 6, 640, line + 12);
        const std::string status =
            Format("%s  ZOOM %.2f PX/TILE  FOG %s  VIEW %s  GROUP %s  [F1] HELP",
                   frame.paused ? "PAUSED" : "RUNNING", camera.pixelsPerUnit,
                   frame.fogEnabled ? "ON" : "OFF",
                   frame.viewerTeam == 0 ? "BLUE" : (frame.viewerTeam == 1 ? "RED" : "ALL"),
                   frame.selectedGroup < 0 ? "-" : std::to_string(frame.selectedGroup).c_str());
        text.DrawShadowed(batch, status, 18, statusY, scale, PackedColor(0xFFE6E6E6u));

        if (!frame.showHelp) return;

        static const char* const kHelp[] = {
            "CNA-RTS  ONE MILLION UNITS",
            "",
            "WASD / ARROWS   PAN THE CAMERA",
            "MOUSE WHEEL     ZOOM",
            "MIDDLE DRAG     PAN",
            "LEFT CLICK      ORDER THE SELECTED GROUP TO MARCH THERE",
            "RIGHT CLICK     ORDER EVERY GROUP OF THE VIEWING TEAM THERE",
            "0-7             SELECT A GROUP   [-] SELECT NONE",
            "F               CYCLE THE FORMATION OF THE SELECTED GROUP",
            "V               SWITCH THE VIEWING TEAM (BLUE / RED / ALL)",
            "G               TOGGLE FOG OF WAR",
            "SPACE           PAUSE   [.] SINGLE STEP",
            "R               RESET THE FRAME-TIME WINDOW",
            "F1              THIS HELP        ESC   QUIT",
            "",
            "EVERY ORDER IS TICK-STAMPED AND LOGGED, SO --RECORD AND",
            "--REPLAY REPRODUCE A RUN EXACTLY, HASH FOR HASH.",
        };
        const int helpLines = static_cast<int>(sizeof(kHelp) / sizeof(kHelp[0]));
        const int helpWidth = 640;
        const int helpHeight = helpLines * line + 28;
        const int helpX = (camera.viewportWidth - helpWidth) / 2;
        const int helpY = (camera.viewportHeight - helpHeight) / 2;
        DrawPanel(batch, text, helpX, helpY, helpWidth, helpHeight);
        for (int i = 0; i < helpLines; ++i)
        {
            text.DrawShadowed(batch, kHelp[i], helpX + 16, helpY + 14 + i * line, scale,
                              PackedColor(i == 0 ? 0xFF7FE0FFu : 0xFFE6E6E6u));
        }
    }
}
