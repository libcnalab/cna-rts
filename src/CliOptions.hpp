// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Simulation.hpp"

namespace CnaRts
{
    /** @brief What the process was asked to do. */
    enum class RunMode
    {
        Interactive,   ///< open a window and play
        Benchmark,     ///< open a window, sweep the scales, print a table, exit
        SimOnly,       ///< no window: measure the simulation alone
        Verify,        ///< no window: prove the run is deterministic, then exit
    };

    /** @brief Every command-line switch, already validated. */
    struct CliOptions
    {
        SimConfig sim;
        RunMode mode = RunMode::Interactive;

        int windowWidth = 1600;
        int windowHeight = 900;
        bool fullScreen = false;
        bool vsync = false;
        bool fogEnabled = true;
        float zoom = 0.0f;             ///< 0 means "fit the whole map"
        int viewerTeam = 0;
        bool follow = false;           ///< keep the camera on the front line

        std::vector<std::int32_t> benchScales{10000, 100000, 1000000};
        int benchFrames = 600;
        int benchWarmupFrames = 90;
        int verifyTicks = 400;
        int maxTicks = 0;              ///< 0 runs until the window is closed
        std::string benchOutput;       ///< markdown written here as well as to stdout
        std::string recordPath;
        std::string replayPath;
        std::string screenshotPath;    ///< PNG written once the run reaches screenshotFrame
        int screenshotFrame = 240;
        bool screenshotExit = false;   ///< quit as soon as the capture is on disk

        bool showHelp = false;
        bool parseError = false;
        std::string errorMessage;

        static CliOptions Parse(int argc, char** argv);
    };

    void PrintHelp(const char* executable);

    /** @brief Parses 12345, 12k, 1.5M and so on. Returns false on anything else. */
    bool ParseCount(const std::string& text, std::int32_t& out);
}
