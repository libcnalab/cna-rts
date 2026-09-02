// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Microsoft/Xna/Framework/Game.hpp"
#include "Microsoft/Xna/Framework/GameTime.hpp"
#include "Microsoft/Xna/Framework/GraphicsDeviceManager.hpp"
#include "Microsoft/Xna/Framework/Graphics/RenderTarget2D.hpp"
#include "Microsoft/Xna/Framework/Graphics/SpriteBatch.hpp"
#include "Microsoft/Xna/Framework/Input/KeyboardState.hpp"
#include "Microsoft/Xna/Framework/Input/MouseState.hpp"

#include "Benchmark.hpp"
#include "Camera.hpp"
#include "CliOptions.hpp"
#include "Hud.hpp"
#include "Simulation.hpp"
#include "TerrainRenderer.hpp"
#include "TextRenderer.hpp"
#include "UnitRenderer.hpp"

namespace CnaRts
{
    /**
     * @brief The XNA game: a fixed-step simulation under a free-running renderer.
     *
     * The two clocks are deliberately separate. The simulation advances in whole 30 Hz ticks and
     * never in fractions of one, because a variable step is not reproducible and reproducibility
     * is the point; the renderer draws whatever the last completed tick left behind, as often as
     * the machine can. A frame therefore contains zero, one or several ticks, and the HUD says
     * which -- at a million units the honest answer is often "one tick every other frame".
     */
    class RtsGame : public Microsoft::Xna::Framework::Game
    {
    public:
        explicit RtsGame(const CliOptions& options);
        ~RtsGame() override;

        RtsGame(const RtsGame&) = delete;
        RtsGame& operator=(const RtsGame&) = delete;

        /** @brief Benchmark rows collected so far; the caller prints them after Run() returns. */
        [[nodiscard]] const std::vector<BenchRow>& BenchmarkRows() const { return benchRows_; }
        /** @brief Non-empty when something went wrong that Run() could not report itself. */
        [[nodiscard]] const std::string& Diagnostic() const { return diagnostic_; }
        [[nodiscard]] const Simulation& Sim() const { return simulation_; }
        [[nodiscard]] bool ReplayMatched() const { return replayMatched_; }
        /** @brief The renderer CNA actually initialised, for the benchmark caption. */
        [[nodiscard]] const std::string& RendererName() const { return rendererName_; }

    protected:
        void Initialize() override;
        void LoadContent() override;
        void UnloadContent() override;
        void Update(Microsoft::Xna::Framework::GameTime& gameTime) override;
        void Draw(const Microsoft::Xna::Framework::GameTime& gameTime) override;

    private:
        void StartScale(std::int32_t units);
        void HandleInput(double elapsedSeconds);
        void AdvanceSimulation(double elapsedSeconds);
        void UpdateBenchmark(double frameMs);
        void FitCameraToMap();
        /** @brief Eases the camera toward the fighting. */
        void FollowAction();
        /** @brief Compares the replay's hash with the recorded one once it reaches that tick. */
        void CheckReplay();
        void IssueOrder(int group, float worldX, float worldY);
        /** @brief Draws the whole scene. Called once per frame, twice on a capture frame. */
        void DrawScene(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device);
        void CaptureScreenshot(Microsoft::Xna::Framework::Graphics::GraphicsDevice& device);

        CliOptions options_;
        Microsoft::Xna::Framework::GraphicsDeviceManager graphics_;
        std::unique_ptr<Microsoft::Xna::Framework::Graphics::SpriteBatch> batch_;

        Simulation simulation_;
        TerrainRenderer terrainRenderer_;
        UnitRenderer unitRenderer_;
        TextRenderer text_;
        Hud hud_;
        Camera camera_;
        FrameTimeline timeline_;

        Microsoft::Xna::Framework::Input::KeyboardState previousKeys_;
        Microsoft::Xna::Framework::Input::MouseState previousMouse_;
        bool hasPreviousInput_ = false;

        bool paused_ = false;
        bool stepOnce_ = false;
        bool showHelp_ = false;
        bool fogEnabled_ = true;
        int viewerTeam_ = 0;
        bool followAction_ = false;
        int followCountdown_ = 0;
        float followTargetX_ = 0.0f;
        float followTargetY_ = 0.0f;
        int selectedGroup_ = -1;
        double simAccumulator_ = 0.0;
        int ticksThisFrame_ = 0;
        /// Achieved tick rate, measured over a window rather than per frame. The instantaneous
        /// count is zero on most frames -- the simulation runs at 30 Hz under a renderer that may
        /// be at 1000 -- so a per-frame figure would make a healthy run look stalled.
        double tickRate_ = 0.0;
        int tickWindowTicks_ = 0;
        double tickWindowMs_ = 0.0;
        double simMsThisFrame_ = 0.0;
        double renderMsThisFrame_ = 0.0;
        /// Timestamp of the previous frame's end. Frame time is measured end-to-end rather than
        /// around Draw(), because the simulation runs in Update() and a frame that leaves it out
        /// is not the frame the user waited for.
        std::int64_t lastFrameTimestamp_ = 0;
        std::uint32_t lastFogRefreshTick_ = 0xFFFFFFFFu;

        // Benchmark sweep state.
        std::size_t benchIndex_ = 0;
        int benchFrameCount_ = 0;
        BenchRow benchAccumulator_;
        /// Every measured frame time at the current scale. The HUD's rolling window is 180
        /// frames, so taking the table's p99 from it would report the tail of the last three
        /// seconds rather than of the run the row claims to summarise.
        std::vector<double> benchFrameTimes_;
        std::vector<BenchRow> benchRows_;

        std::unique_ptr<Microsoft::Xna::Framework::Graphics::RenderTarget2D> capture_;
        std::int64_t frameIndex_ = 0;
        bool captureDone_ = false;

        ReplayLog replay_;
        bool replayChecked_ = false;
        bool replayMatched_ = true;
        std::string diagnostic_;
        std::string rendererName_ = "unknown";
    };
}
