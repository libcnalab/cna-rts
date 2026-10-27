// SPDX-License-Identifier: MIT
#include "RtsGame.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

#include "Microsoft/Xna/Framework/Color.hpp"
#include "Palette.hpp"
#include "Microsoft/Xna/Framework/GameWindow.hpp"
#include "Microsoft/Xna/Framework/Graphics/BlendState.hpp"
#include "Microsoft/Xna/Framework/Graphics/DepthStencilState.hpp"
#include "Microsoft/Xna/Framework/Graphics/GraphicsDevice.hpp"
#include "Microsoft/Xna/Framework/Graphics/GraphicsProfile.hpp"
#include "Microsoft/Xna/Framework/Graphics/RasterizerState.hpp"
#include "Microsoft/Xna/Framework/Graphics/RenderTarget2D.hpp"
#include "Microsoft/Xna/Framework/Graphics/SamplerState.hpp"
#include "Microsoft/Xna/Framework/Graphics/SpriteSortMode.hpp"
#include "Microsoft/Xna/Framework/Input/ButtonState.hpp"
#include "Microsoft/Xna/Framework/Input/Keyboard.hpp"
#include "Microsoft/Xna/Framework/Input/Keys.hpp"
#include "Microsoft/Xna/Framework/Input/Mouse.hpp"
#include "System/Diagnostics/Stopwatch.hpp"
#include "System/TimeSpan.hpp"

using namespace Microsoft::Xna::Framework;
using namespace Microsoft::Xna::Framework::Graphics;
using namespace Microsoft::Xna::Framework::Input;

namespace CnaRts
{
    namespace
    {
        constexpr double kSecondsPerTick = 1.0 / static_cast<double>(kTicksPerSecond);
        /** @brief Ticks a single frame is allowed to run before it gives up and lets time slip.
         *  Without it, a frame that fell behind spirals: catching up costs more than the budget
         *  it was trying to reclaim. */
        constexpr int kMaxTicksPerFrame = 4;

        inline double ElapsedMs(const System::Diagnostics::Stopwatch& watch)
        {
            return static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0;
        }
    }

    RtsGame::RtsGame(const CliOptions& options)
        : options_(options), graphics_(this)
    {
        // Reach caps a draw at 65,535 instances; the benchmark submits up to one million.
        graphics_.setGraphicsProfileProperty(GraphicsProfile::HiDef);
        graphics_.setPreferredBackBufferWidthProperty(options_.windowWidth);
        graphics_.setPreferredBackBufferHeightProperty(options_.windowHeight);
        graphics_.setSynchronizeWithVerticalRetraceProperty(options_.vsync);
        if (options_.fullScreen) graphics_.setIsFullScreenProperty(true);

        // A benchmark must not be paced by the harness: fixed-step drawing would report the
        // target rate rather than the achieved one, and vsync would report the monitor.
        setIsFixedTimeStepProperty(false);
        setIsMouseVisibleProperty(true);

        fogEnabled_ = options_.fogEnabled;
        viewerTeam_ = options_.viewerTeam;
        // A capture or a benchmark that stares at empty terrain measures and shows nothing, so
        // both follow the armies by default; an interactive session leaves the camera alone
        // unless the player asks for it with C.
        followAction_ = options_.follow ||
                        options_.mode == RunMode::Benchmark ||
                        !options_.screenshotPath.empty();
        camera_.viewportWidth = options_.windowWidth;
        camera_.viewportHeight = options_.windowHeight;
    }

    RtsGame::~RtsGame() = default;

    void RtsGame::Initialize()
    {
        Game::Initialize();
        getWindowProperty().setTitleProperty("cna-rts -- One Million Units");
    }

    void RtsGame::StartScale(std::int32_t units)
    {
        SimConfig config = options_.sim;
        config.unitCount = units;
        simulation_.Initialize(config);
        if (!replay_.commands.empty()) simulation_.LoadReplay(replay_.commands);
        replayChecked_ = false;

        selectedGroup_ = -1;
        simAccumulator_ = 0.0;
        lastFogRefreshTick_ = 0xFFFFFFFFu;
        timeline_.Reset();

        GraphicsDevice& device = *graphics_.getGraphicsDeviceProperty();
        terrainRenderer_.Load(device, simulation_);
        if (!unitRenderer_.Load(device, units))
        {
            diagnostic_ = unitRenderer_.Diagnostic();
            std::cerr << "cna-rts: instanced unit rendering unavailable -- " << diagnostic_ << "\n";
        }
        FitCameraToMap();
    }

    void RtsGame::FitCameraToMap()
    {
        const auto extent = static_cast<float>(simulation_.Config().mapTiles);
        camera_.centerX = extent * 0.5f;
        camera_.centerY = extent * 0.5f;
        camera_.pixelsPerUnit =
            options_.zoom > 0.0f
                ? options_.zoom
                : std::min(static_cast<float>(camera_.viewportWidth),
                           static_cast<float>(camera_.viewportHeight)) / extent;
        camera_.ClampTo(simulation_.Config().mapTiles);
    }

    void RtsGame::LoadContent()
    {
        GraphicsDevice& device = *graphics_.getGraphicsDeviceProperty();
        batch_ = std::make_unique<SpriteBatch>(device);
        text_.Load(device);
        rendererName_ = std::string(device.GetGraphicsRendererName());

        camera_.viewportWidth = device.getViewportProperty().getWidthProperty();
        camera_.viewportHeight = device.getViewportProperty().getHeightProperty();

        if (!options_.replayPath.empty())
        {
            SimConfig recorded = options_.sim;
            std::string error;
            if (ReadCommandLog(options_.replayPath, recorded, replay_, error))
            {
                options_.sim = recorded;
                std::cout << "cna-rts: replaying " << replay_.commands.size() << " orders from "
                          << options_.replayPath << " (recorded through tick " << replay_.finalTick
                          << ")\n";
            }
            else
            {
                std::cerr << "cna-rts: " << error << "\n";
                replay_ = ReplayLog{};
            }
        }

        const std::int32_t first = options_.mode == RunMode::Benchmark && !options_.benchScales.empty()
                                       ? options_.benchScales.front()
                                       : options_.sim.unitCount;
        StartScale(first);
    }

    void RtsGame::UnloadContent()
    {
        unitRenderer_.Unload();
        terrainRenderer_.Unload();
        text_.Unload();
        batch_.reset();
    }

    void RtsGame::IssueOrder(int group, float worldX, float worldY)
    {
        if (group < 0 || group >= static_cast<int>(simulation_.Groups().size())) return;
        const Vec2f target{Fixed::FromDouble(worldX), Fixed::FromDouble(worldY)};
        Command command;
        command.tick = simulation_.CurrentTick() + 1;
        command.group = static_cast<std::uint16_t>(group);
        command.goalTile = simulation_.NearestPassableTile(target);
        command.formation = static_cast<std::uint8_t>(
            simulation_.Groups()[static_cast<std::size_t>(group)].formation);
        simulation_.QueueCommand(command);
    }

    void RtsGame::HandleInput(double elapsedSeconds)
    {
        const KeyboardState keys = Keyboard::GetState();
        const MouseState mouse = Mouse::GetState();
        const auto pressed = [&](Keys key) {
            return keys.IsKeyDown(key) && (!hasPreviousInput_ || !previousKeys_.IsKeyDown(key));
        };

        if (keys.IsKeyDown(Keys::Escape)) Exit();

        // Panning is expressed in tiles per second so it feels the same at every zoom.
        const auto pan = static_cast<float>(elapsedSeconds) * 900.0f / camera_.pixelsPerUnit;
        if (keys.IsKeyDown(Keys::A) || keys.IsKeyDown(Keys::Left))  camera_.centerX -= pan;
        if (keys.IsKeyDown(Keys::D) || keys.IsKeyDown(Keys::Right)) camera_.centerX += pan;
        if (keys.IsKeyDown(Keys::W) || keys.IsKeyDown(Keys::Up))    camera_.centerY -= pan;
        if (keys.IsKeyDown(Keys::S) || keys.IsKeyDown(Keys::Down))  camera_.centerY += pan;

        if (pressed(Keys::Space)) paused_ = !paused_;
        if (pressed(Keys::OemPeriod)) stepOnce_ = true;
        if (pressed(Keys::F1)) showHelp_ = !showHelp_;
        if (pressed(Keys::G)) fogEnabled_ = !fogEnabled_;
        if (pressed(Keys::R)) timeline_.Reset();
        if (pressed(Keys::C)) followAction_ = !followAction_;
        if (pressed(Keys::V))
        {
            viewerTeam_ = viewerTeam_ + 1;
            if (viewerTeam_ >= options_.sim.teams) viewerTeam_ = -1;
            lastFogRefreshTick_ = 0xFFFFFFFFu;
        }
        if (pressed(Keys::OemMinus)) selectedGroup_ = -1;
        for (int i = 0; i < 8; ++i)
        {
            if (pressed(static_cast<Keys>(static_cast<int>(Keys::D0) + i)))
            {
                selectedGroup_ = i < static_cast<int>(simulation_.Groups().size()) ? i : -1;
            }
        }
        if (pressed(Keys::F) && selectedGroup_ >= 0)
        {
            const Group& group = simulation_.Groups()[static_cast<std::size_t>(selectedGroup_)];
            Command command;
            command.tick = simulation_.CurrentTick() + 1;
            command.group = static_cast<std::uint16_t>(selectedGroup_);
            command.goalTile = group.goalTile;
            command.formation = static_cast<std::uint8_t>(
                (static_cast<int>(group.formation) + 1) % static_cast<int>(FormationKind::Count));
            simulation_.QueueCommand(command);
        }

        // Zoom about the cursor, so the thing under the pointer stays under the pointer.
        if (hasPreviousInput_)
        {
            const int wheelDelta =
                mouse.getScrollWheelValueProperty() - previousMouse_.getScrollWheelValueProperty();
            if (wheelDelta != 0)
            {
                const float anchorX = camera_.WorldX(static_cast<float>(mouse.getXProperty()));
                const float anchorY = camera_.WorldY(static_cast<float>(mouse.getYProperty()));
                const float factor = std::pow(1.0015f, static_cast<float>(wheelDelta));
                const auto extent = static_cast<float>(simulation_.Config().mapTiles);
                const float minimum =
                    std::min(static_cast<float>(camera_.viewportWidth),
                             static_cast<float>(camera_.viewportHeight)) / extent * 0.9f;
                camera_.pixelsPerUnit =
                    std::clamp(camera_.pixelsPerUnit * factor, minimum, 96.0f);
                camera_.centerX += anchorX - camera_.WorldX(static_cast<float>(mouse.getXProperty()));
                camera_.centerY += anchorY - camera_.WorldY(static_cast<float>(mouse.getYProperty()));
            }

            if (mouse.getMiddleButtonProperty() == ButtonState::Pressed &&
                previousMouse_.getMiddleButtonProperty() == ButtonState::Pressed)
            {
                camera_.centerX -= static_cast<float>(mouse.getXProperty() -
                                                      previousMouse_.getXProperty()) /
                                   camera_.pixelsPerUnit;
                camera_.centerY -= static_cast<float>(mouse.getYProperty() -
                                                      previousMouse_.getYProperty()) /
                                   camera_.pixelsPerUnit;
            }

            const bool leftClick = mouse.getLeftButtonProperty() == ButtonState::Pressed &&
                                   previousMouse_.getLeftButtonProperty() == ButtonState::Released;
            const bool rightClick = mouse.getRightButtonProperty() == ButtonState::Pressed &&
                                    previousMouse_.getRightButtonProperty() == ButtonState::Released;
            if (leftClick || rightClick)
            {
                const float worldX = camera_.WorldX(static_cast<float>(mouse.getXProperty()));
                const float worldY = camera_.WorldY(static_cast<float>(mouse.getYProperty()));
                if (rightClick)
                {
                    const int team = viewerTeam_ < 0 ? 0 : viewerTeam_;
                    for (std::size_t g = 0; g < simulation_.Groups().size(); ++g)
                        if (simulation_.Groups()[g].team == team)
                            IssueOrder(static_cast<int>(g), worldX, worldY);
                }
                else if (selectedGroup_ >= 0)
                {
                    IssueOrder(selectedGroup_, worldX, worldY);
                }
            }
        }

        camera_.ClampTo(simulation_.Config().mapTiles);
        previousKeys_ = keys;
        previousMouse_ = mouse;
        hasPreviousInput_ = true;
    }

    void RtsGame::AdvanceSimulation(double elapsedSeconds)
    {
        ticksThisFrame_ = 0;
        simMsThisFrame_ = 0.0;

        if (paused_ && !stepOnce_) { simAccumulator_ = 0.0; return; }

        System::Diagnostics::Stopwatch watch = System::Diagnostics::Stopwatch::StartNew();
        if (stepOnce_)
        {
            simulation_.Tick();
            ticksThisFrame_ = 1;
            stepOnce_ = false;
        }
        else
        {
            simAccumulator_ += elapsedSeconds;
            while (simAccumulator_ >= kSecondsPerTick && ticksThisFrame_ < kMaxTicksPerFrame)
            {
                simulation_.Tick();
                simAccumulator_ -= kSecondsPerTick;
                ++ticksThisFrame_;
            }
            // Whatever could not be simulated in the budget is dropped rather than owed. The
            // alternative is a death spiral: at a million units a frame that falls two ticks
            // behind cannot catch up, and trying makes the next frame later still.
            if (simAccumulator_ > kSecondsPerTick * kMaxTicksPerFrame)
                simAccumulator_ = 0.0;
        }
        watch.Stop();
        simMsThisFrame_ = ElapsedMs(watch);
    }

    void RtsGame::CheckReplay()
    {
        // A replay that only re-issues the orders proves nothing on its own. The recorded log
        // carries the tick the original run stopped on and the hash it stopped with, so this is
        // the moment the claim is actually tested.
        if (replayChecked_ || replay_.finalTick == 0) return;
        if (simulation_.CurrentTick() < replay_.finalTick) return;
        replayChecked_ = true;
        replayMatched_ = simulation_.Checksum() == replay_.finalChecksum;
        std::cout << "cna-rts: replay reached tick " << simulation_.CurrentTick() << " with hash "
                  << std::hex << simulation_.Checksum() << ", recorded " << replay_.finalChecksum
                  << std::dec << " -- " << (replayMatched_ ? "MATCH" : "*** MISMATCH ***") << "\n";
        if (!replayMatched_)
            diagnostic_ = "replay diverged from the recorded run";
        if (options_.screenshotPath.empty()) Exit();
    }

    void RtsGame::FollowAction()
    {
        // Where the fighting is, not where the map's middle is. The midpoint between two armies'
        // centres of mass sounds right and is usually wrong: until they meet it points at empty
        // terrain between them, which is what the first captures from this camera showed.
        //
        // The spatial grid already knows how many units of each team occupy every cell, so the
        // contested cell -- the one whose *second* largest team count is highest -- is one linear
        // scan away, and it is by construction a cell with a battle in it. With no contact
        // anywhere, the fullest cell is the next best thing and at least contains an army.
        if (--followCountdown_ > 0)
        {
            camera_.centerX += (followTargetX_ - camera_.centerX) * 0.04f;
            camera_.centerY += (followTargetY_ - camera_.centerY) * 0.04f;
            camera_.ClampTo(simulation_.Config().mapTiles);
            return;
        }
        followCountdown_ = 20;

        const SpatialGrid& grid = simulation_.Grid();
        const std::int32_t cellCount = grid.CellCount();
        std::int32_t bestContested = -1;
        std::int32_t bestContestedScore = 0;
        std::int32_t bestCrowded = -1;
        std::int32_t bestCrowdedScore = 0;
        for (std::int32_t c = 0; c < cellCount; ++c)
        {
            std::int32_t largest = 0, second = 0, total = 0;
            for (int team = 0; team < options_.sim.teams; ++team)
            {
                const std::int32_t population = grid.TeamInCell(c, team);
                total += population;
                if (population > largest) { second = largest; largest = population; }
                else if (population > second) { second = population; }
            }
            if (second > bestContestedScore) { bestContestedScore = second; bestContested = c; }
            // The fallback prefers the *viewing* team's own crowd. Picking the fullest cell of
            // any team sends the camera to the enemy's rear, which under fog of war is a screen
            // of unbroken black -- a correct render of nothing worth looking at.
            const std::int32_t crowd =
                (fogEnabled_ && viewerTeam_ >= 0) ? grid.TeamInCell(c, viewerTeam_) : total;
            if (crowd > bestCrowdedScore) { bestCrowdedScore = crowd; bestCrowded = c; }
        }

        const std::int32_t chosen = bestContested >= 0 ? bestContested : bestCrowded;
        if (chosen < 0) return;
        const int cellSize = grid.CellSizeTiles();
        followTargetX_ = static_cast<float>((chosen % grid.CellsPerSide()) * cellSize + cellSize / 2);
        followTargetY_ = static_cast<float>((chosen / grid.CellsPerSide()) * cellSize + cellSize / 2);

        camera_.centerX += (followTargetX_ - camera_.centerX) * 0.04f;
        camera_.centerY += (followTargetY_ - camera_.centerY) * 0.04f;
        camera_.ClampTo(simulation_.Config().mapTiles);
    }

    void RtsGame::Update(GameTime& gameTime)
    {
        const double elapsed = gameTime.getElapsedGameTimeProperty().getTotalSecondsProperty();

        // Input is read only while the window has focus, and not at all while a capture is
        // pending. Without both guards an unattended run picks up whatever the desktop is doing:
        // the first captures taken here came back with the help overlay open and the zoom moved,
        // from keys and a wheel that belonged to another window. A `--screenshot` run exists to
        // produce one reproducible file, so its framing is whatever `--zoom` and `--follow` say
        // and nothing else.
        const bool capturePending = !options_.screenshotPath.empty() && !captureDone_;
        if (options_.mode == RunMode::Interactive && getIsActiveProperty() && !capturePending)
            HandleInput(elapsed);
        else
            hasPreviousInput_ = false;
        AdvanceSimulation(options_.mode == RunMode::Benchmark ? kSecondsPerTick : elapsed);

        if (followAction_) FollowAction();
        CheckReplay();

        // Bounded by ticks, not by frames, so a --record run and its --replay stop at the same
        // point in the simulation however differently the two frame rates turned out.
        if (options_.maxTicks > 0 &&
            simulation_.CurrentTick() >= static_cast<std::uint32_t>(options_.maxTicks))
            Exit();

        Game::Update(gameTime);
    }

    void RtsGame::UpdateBenchmark(double frameMs)
    {
        if (options_.mode != RunMode::Benchmark) return;
        ++benchFrameCount_;
        if (benchFrameCount_ <= options_.benchWarmupFrames) return;

        const SimStats& sim = simulation_.Stats();
        const UnitRenderStats& units = unitRenderer_.Stats();
        benchAccumulator_.frameMs += frameMs;
        benchAccumulator_.simMs += sim.totalMs;
        benchAccumulator_.pathMs += sim.pathMs;
        benchAccumulator_.gridMs += sim.gridMs;
        benchAccumulator_.moveMs += sim.moveMs;
        benchAccumulator_.combatMs += sim.combatMs;
        benchAccumulator_.fogMs += sim.fogMs;
        benchAccumulator_.checksumMs += sim.checksumMs;
        benchAccumulator_.buildMs += units.buildMs;
        benchAccumulator_.uploadMs += units.uploadMs;
        benchAccumulator_.drawMs += units.drawMs;
        benchFrameTimes_.push_back(frameMs);

        const int measured = benchFrameCount_ - options_.benchWarmupFrames;
        if (measured < options_.benchFrames) return;

        BenchRow row = benchAccumulator_;
        const auto samples = static_cast<double>(measured);
        row.units = simulation_.Config().unitCount;
        row.mapTiles = simulation_.Config().mapTiles;
        row.windowed = true;
        row.frameMs /= samples;
        row.simMs /= samples;
        row.pathMs /= samples;
        row.gridMs /= samples;
        row.moveMs /= samples;
        row.combatMs /= samples;
        row.fogMs /= samples;
        row.checksumMs /= samples;
        row.buildMs /= samples;
        row.uploadMs /= samples;
        row.drawMs /= samples;
        row.fps = row.frameMs > 0.0 ? 1000.0 / row.frameMs : 0.0;
        std::sort(benchFrameTimes_.begin(), benchFrameTimes_.end());
        row.p99Ms = benchFrameTimes_.empty()
            ? 0.0
            : benchFrameTimes_[static_cast<std::size_t>(
                  0.99 * static_cast<double>(benchFrameTimes_.size() - 1))];
        row.drawCalls = units.drawCalls;
        row.streamMiB = static_cast<double>(units.instances) * 8.0 / 1048576.0;
        row.checksum = simulation_.Checksum();
        row.ticksPerSecond = row.simMs > 0.0 ? 1000.0 / row.simMs : 0.0;
        benchRows_.push_back(row);

        std::cout << "cna-rts: " << row.units << " units -> " << row.fps << " fps ("
                  << row.frameMs << " ms/frame, sim " << row.simMs << " ms/tick)\n";

        benchAccumulator_ = BenchRow{};
        benchFrameTimes_.clear();
        benchFrameCount_ = 0;
        ++benchIndex_;
        if (benchIndex_ >= options_.benchScales.size()) { Exit(); return; }
        StartScale(options_.benchScales[benchIndex_]);
    }

    void RtsGame::DrawScene(GraphicsDevice& device)
    {
        device.Clear(RgbaColor(8, 10, 14, 255));

        // Ground first, through SpriteBatch: one textured quad, point-sampled once a tile is
        // several pixels across and filtered below that.
        const SamplerState& groundSampler =
            camera_.pixelsPerUnit >= 3.0f ? SamplerState::PointClamp : SamplerState::LinearClamp;
        batch_->Begin(SpriteSortMode::Deferred, BlendState::Opaque, &groundSampler,
                      &DepthStencilState::None, &RasterizerState::CullNone);
        terrainRenderer_.DrawTerrain(*batch_, camera_, simulation_.Config().mapTiles);
        batch_->End();

        // Then the army: one draw call, however many units there are.
        if (unitRenderer_.Ready())
        {
            unitRenderer_.Build(simulation_.Jobs(), simulation_,
                                fogEnabled_ ? viewerTeam_ : -1, selectedGroup_);
            unitRenderer_.Draw(device, camera_, simulation_, terrainRenderer_.FogTexture(),
                               fogEnabled_ && viewerTeam_ >= 0);
        }

        // Then the dark over what nobody can see, and the overlay over everything.
        batch_->Begin(SpriteSortMode::Deferred, BlendState::AlphaBlend, &SamplerState::LinearClamp,
                      &DepthStencilState::None, &RasterizerState::CullNone);
        if (fogEnabled_ && viewerTeam_ >= 0)
            terrainRenderer_.DrawFog(*batch_, camera_, simulation_.Config().mapTiles);
        batch_->End();

        batch_->Begin(SpriteSortMode::Deferred, BlendState::AlphaBlend, &SamplerState::PointClamp,
                      &DepthStencilState::None, &RasterizerState::CullNone);
        HudFrame frame;
        frame.frameMs = timeline_.AverageMs();
        frame.simMs = simMsThisFrame_;
        frame.renderMs = renderMsThisFrame_;
        frame.ticksPerSecond = tickRate_;
        frame.viewerTeam = viewerTeam_;
        frame.selectedGroup = selectedGroup_;
        frame.fogEnabled = fogEnabled_;
        frame.paused = paused_;
        frame.showHelp = showHelp_;
        hud_.Draw(*batch_, text_, terrainRenderer_, simulation_, unitRenderer_.Stats(), timeline_,
                  camera_, frame);
        batch_->End();
    }

    void RtsGame::CaptureScreenshot(GraphicsDevice& device)
    {
        // Rendered again into an off-screen target rather than read back from the back buffer:
        // XNA has no back-buffer read, and on a double-buffered surface what a read would return
        // is whichever buffer the driver felt like handing over.
        const int width = camera_.viewportWidth;
        const int height = camera_.viewportHeight;
        if (!capture_ || capture_->getWidthProperty() != width ||
            capture_->getHeightProperty() != height)
        {
            capture_ = std::make_unique<RenderTarget2D>(device, width, height);
        }

        device.SetRenderTarget(capture_.get());
        DrawScene(device);
        device.SetRenderTarget(nullptr);

        try
        {
            capture_->SaveAsPng(options_.screenshotPath);
            std::cout << "cna-rts: wrote " << options_.screenshotPath << " (" << width << "x"
                      << height << ", frame " << frameIndex_ << ", tick "
                      << simulation_.CurrentTick() << ")\n";
        }
        catch (const std::exception& error)
        {
            std::cerr << "cna-rts: could not write " << options_.screenshotPath << ": "
                      << error.what() << "\n";
        }
        captureDone_ = true;
        if (options_.screenshotExit) Exit();
    }

    void RtsGame::Draw(const GameTime& gameTime)
    {
        GraphicsDevice& device = *graphics_.getGraphicsDeviceProperty();
        camera_.viewportWidth = device.getViewportProperty().getWidthProperty();
        camera_.viewportHeight = device.getViewportProperty().getHeightProperty();

        // The fog and minimap textures only change when the simulation's fog does, so they are
        // rebuilt on its schedule and not on the renderer's.
        if (simulation_.CurrentTick() != lastFogRefreshTick_ &&
            simulation_.CurrentTick() % static_cast<std::uint32_t>(
                std::max(1, simulation_.Config().fogInterval)) == 0)
        {
            terrainRenderer_.Refresh(simulation_, viewerTeam_, fogEnabled_);
            lastFogRefreshTick_ = simulation_.CurrentTick();
        }

        System::Diagnostics::Stopwatch renderWatch = System::Diagnostics::Stopwatch::StartNew();
        DrawScene(device);
        renderWatch.Stop();
        renderMsThisFrame_ = ElapsedMs(renderWatch);

        ++frameIndex_;
        if (!captureDone_ && !options_.screenshotPath.empty() &&
            frameIndex_ >= options_.screenshotFrame)
        {
            CaptureScreenshot(device);
        }

        Game::Draw(gameTime);

        const std::int64_t now = System::Diagnostics::Stopwatch::GetTimestamp();
        if (lastFrameTimestamp_ != 0)
        {
            const double frameMs = static_cast<double>(
                System::Diagnostics::Stopwatch::GetElapsedTime(lastFrameTimestamp_, now)
                    .getTicksProperty()) / 10000.0;
            timeline_.Push(frameMs);
            tickWindowTicks_ += ticksThisFrame_;
            tickWindowMs_ += frameMs;
            if (tickWindowMs_ >= 500.0)
            {
                tickRate_ = tickWindowTicks_ * 1000.0 / tickWindowMs_;
                tickWindowTicks_ = 0;
                tickWindowMs_ = 0.0;
            }
            UpdateBenchmark(frameMs);
        }
        lastFrameTimestamp_ = now;
    }
}
