// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include "CliOptions.hpp"
#include "Simulation.hpp"

namespace CnaRts
{
    /** @brief One row of the README table: everything measured at one unit count. */
    struct BenchRow
    {
        std::int32_t units = 0;
        int mapTiles = 0;
        bool windowed = false;

        double frameMs = 0.0;
        double p99Ms = 0.0;
        double fps = 0.0;

        double simMs = 0.0;        ///< whole tick
        double pathMs = 0.0;
        double gridMs = 0.0;
        double moveMs = 0.0;
        double combatMs = 0.0;
        double fogMs = 0.0;
        double checksumMs = 0.0;

        double buildMs = 0.0;      ///< instance-stream fill
        double uploadMs = 0.0;
        double drawMs = 0.0;
        int drawCalls = 0;
        double streamMiB = 0.0;

        double ticksPerSecond = 0.0;
        std::uint64_t checksum = 0;
    };

    /** @brief Describes the machine the numbers came from, for the table's caption. */
    std::string DescribeHost(const SimConfig& config, const std::string& rendererName);

    /** @brief Renders the rows as a GitHub-flavoured markdown section. */
    std::string RenderMarkdown(const std::vector<BenchRow>& rows, const std::string& caption);

    /** @brief Writes @p text to @p path, appending a trailing newline. */
    bool WriteTextFile(const std::string& path, const std::string& text, std::string& error);

    /** @brief `--sim-only`: measures the simulation with no window and prints the table. */
    int RunSimOnlyBenchmark(const CliOptions& options);

    /** @brief `--verify`: runs the same seed twice, with different worker counts, and compares. */
    int RunVerify(const CliOptions& options);

    /**
     * @brief A recorded run: its configuration, its orders, and the hash it ended on.
     *
     * The final tick and hash are what turn `--replay` from "run these orders again" into a
     * check: a replay that reaches the same tick with a different hash has found a real
     * divergence, and one that reaches it with the same hash has proved the run reproduces.
     */
    struct ReplayLog
    {
        std::vector<Command> commands;
        std::uint32_t finalTick = 0;
        std::uint64_t finalChecksum = 0;
    };

    /** @brief Order-log persistence for `--record` / `--replay`. */
    bool WriteCommandLog(const std::string& path, const SimConfig& config,
                         const ReplayLog& log, std::string& error);
    bool ReadCommandLog(const std::string& path, SimConfig& config,
                        ReplayLog& log, std::string& error);
}
