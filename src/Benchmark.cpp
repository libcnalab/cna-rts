// SPDX-License-Identifier: MIT
#include "Benchmark.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "System/Diagnostics/Stopwatch.hpp"

namespace CnaRts
{
    namespace
    {
        std::string Fixed2(double value)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%.2f", value);
            return buffer;
        }
        std::string Fixed1(double value)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%.1f", value);
            return buffer;
        }
        std::string Units(std::int32_t value)
        {
            char buffer[32];
            if (value >= 1000000 && value % 1000000 == 0)
                std::snprintf(buffer, sizeof buffer, "%dM", value / 1000000);
            else if (value >= 1000 && value % 1000 == 0)
                std::snprintf(buffer, sizeof buffer, "%dk", value / 1000);
            else
                std::snprintf(buffer, sizeof buffer, "%d", value);
            return buffer;
        }

        inline double ElapsedMs(const System::Diagnostics::Stopwatch& watch)
        {
            return static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0;
        }
    }

    std::string DescribeHost(const SimConfig& config, const std::string& rendererName)
    {
        std::ostringstream out;
        const int hardware = static_cast<int>(std::thread::hardware_concurrency());
        out << "renderer " << rendererName << ", "
            << (config.workers > 0 ? config.workers : hardware) << " worker threads, seed "
            << config.seed << ", " << config.blockedPercent << "% blocked terrain, "
            << config.teams << " teams x " << config.groupsPerTeam << " groups";
        if (!config.combat) out << ", combat off";
        if (config.checksumInterval == 0) out << ", checksum off";
        return out.str();
    }

    std::string RenderMarkdown(const std::vector<BenchRow>& rows, const std::string& caption)
    {
        std::ostringstream out;
        const bool windowed = std::any_of(rows.begin(), rows.end(),
                                          [](const BenchRow& row) { return row.windowed; });

        if (windowed)
        {
            out << "| Entities | Map | FPS | Frame ms | p99 ms | Sim ms/tick | Instance build ms |"
                   " Upload ms | Draw calls | Stream MiB |\n";
            out << "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";
            for (const BenchRow& row : rows)
            {
                out << "| " << Units(row.units)
                    << " | " << row.mapTiles << "x" << row.mapTiles
                    << " | **" << Fixed1(row.fps) << "**"
                    << " | " << Fixed2(row.frameMs)
                    << " | " << Fixed2(row.p99Ms)
                    << " | " << Fixed2(row.simMs)
                    << " | " << Fixed2(row.buildMs)
                    << " | " << Fixed2(row.uploadMs)
                    << " | " << row.drawCalls
                    << " | " << Fixed2(row.streamMiB)
                    << " |\n";
            }
        }
        else
        {
            out << "| Entities | Map | Ticks/s | Tick ms | Path ms | Grid ms | Move ms |"
                   " Fight ms | Fog ms | Hash ms |\n";
            out << "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";
            for (const BenchRow& row : rows)
            {
                out << "| " << Units(row.units)
                    << " | " << row.mapTiles << "x" << row.mapTiles
                    << " | **" << Fixed1(row.ticksPerSecond) << "**"
                    << " | " << Fixed2(row.simMs)
                    << " | " << Fixed2(row.pathMs)
                    << " | " << Fixed2(row.gridMs)
                    << " | " << Fixed2(row.moveMs)
                    << " | " << Fixed2(row.combatMs)
                    << " | " << Fixed2(row.fogMs)
                    << " | " << Fixed2(row.checksumMs)
                    << " |\n";
            }
        }

        if (!caption.empty()) out << "\n" << caption << "\n";
        return out.str();
    }

    bool WriteTextFile(const std::string& path, const std::string& text, std::string& error)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            error = "could not open '" + path + "' for writing";
            return false;
        }
        file << text;
        if (!text.empty() && text.back() != '\n') file << '\n';
        return static_cast<bool>(file);
    }

    int RunSimOnlyBenchmark(const CliOptions& options)
    {
        std::vector<BenchRow> rows;
        // The measured tick budget is expressed in ticks rather than frames, because with no
        // window there is no frame: `--bench-frames` is read as "ticks to measure" so one flag
        // means one thing in both benchmark modes.
        const int measured = options.benchFrames;
        const int warmup = options.benchWarmupFrames;

        for (const std::int32_t units : options.benchScales)
        {
            SimConfig config = options.sim;
            config.unitCount = units;

            std::cout << "cna-rts: simulating " << units << " units ..." << std::flush;
            Simulation simulation;
            System::Diagnostics::Stopwatch setup = System::Diagnostics::Stopwatch::StartNew();
            simulation.Initialize(config);
            setup.Stop();

            for (int i = 0; i < warmup; ++i) simulation.Tick();

            BenchRow row;
            row.units = units;
            row.mapTiles = simulation.Config().mapTiles;
            row.windowed = false;

            System::Diagnostics::Stopwatch watch = System::Diagnostics::Stopwatch::StartNew();
            for (int i = 0; i < measured; ++i)
            {
                simulation.Tick();
                const SimStats& stats = simulation.Stats();
                row.pathMs += stats.pathMs;
                row.gridMs += stats.gridMs;
                row.moveMs += stats.moveMs;
                row.combatMs += stats.combatMs;
                row.fogMs += stats.fogMs;
                row.checksumMs += stats.checksumMs;
            }
            watch.Stop();

            const double wall = ElapsedMs(watch);
            const auto ticks = static_cast<double>(std::max(1, measured));
            row.simMs = wall / ticks;
            row.pathMs /= ticks;
            row.gridMs /= ticks;
            row.moveMs /= ticks;
            row.combatMs /= ticks;
            row.fogMs /= ticks;
            row.checksumMs /= ticks;
            row.ticksPerSecond = row.simMs > 0.0 ? 1000.0 / row.simMs : 0.0;
            row.checksum = simulation.Checksum();
            rows.push_back(row);

            std::cout << " map " << row.mapTiles << ", setup " << Fixed1(ElapsedMs(setup))
                      << " ms, " << Fixed2(row.simMs) << " ms/tick, hash "
                      << std::hex << row.checksum << std::dec << "\n";
        }

        const std::string caption =
            "Simulation only, no rendering: " + DescribeHost(options.sim, "none (headless)") + ".";
        const std::string markdown = RenderMarkdown(rows, caption);
        std::cout << "\n" << markdown;

        if (!options.benchOutput.empty())
        {
            std::string error;
            if (!WriteTextFile(options.benchOutput, markdown, error))
            {
                std::cerr << "cna-rts: " << error << "\n";
                return 1;
            }
            std::cout << "cna-rts: wrote " << options.benchOutput << "\n";
        }
        return 0;
    }

    int RunVerify(const CliOptions& options)
    {
        // Determinism is claimed, so it is checked -- and checked in the way that actually
        // catches the bug it is about. Running the same binary twice with the same worker count
        // proves almost nothing; the failure mode worth catching is a result that depends on how
        // the work was divided, so the second run deliberately uses a different worker count.
        const int ticks = options.verifyTicks;
        int failures = 0;

        for (const std::int32_t units : options.benchScales)
        {
            SimConfig first = options.sim;
            first.unitCount = units;
            first.workers = options.sim.workers > 0 ? options.sim.workers : 0;

            SimConfig second = first;
            const int hardware = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
            second.workers = first.workers == 1 ? std::max(2, hardware)
                                                : std::max(1, hardware / 3);

            Simulation a, b;
            a.Initialize(first);
            b.Initialize(second);
            for (int i = 0; i < ticks; ++i) { a.Tick(); b.Tick(); }

            const bool match = a.Checksum() == b.Checksum();
            if (!match) ++failures;
            std::cout << "cna-rts: " << units << " units, " << ticks << " ticks -- "
                      << (first.workers == 0 ? hardware : first.workers) << " workers gives "
                      << std::hex << a.Checksum() << ", " << std::dec << second.workers
                      << " workers gives " << std::hex << b.Checksum() << std::dec << "  "
                      << (match ? "MATCH" : "*** MISMATCH ***") << "\n";
        }

        if (failures == 0)
            std::cout << "cna-rts: deterministic across every scale and worker count tested.\n";
        else
            std::cerr << "cna-rts: " << failures << " scale(s) diverged.\n";
        return failures == 0 ? 0 : 1;
    }

    bool WriteCommandLog(const std::string& path, const SimConfig& config,
                         const ReplayLog& log, std::string& error)
    {
        const std::vector<Command>& commands = log.commands;
        std::ostringstream out;
        out << "cna-rts-replay 2\n"
            << "seed " << config.seed << "\n"
            << "units " << config.unitCount << "\n"
            << "map " << config.mapTiles << "\n"
            << "sector " << config.sectorSize << "\n"
            << "blocked " << config.blockedPercent << "\n"
            << "teams " << config.teams << "\n"
            << "groups " << config.groupsPerTeam << "\n"
            << "neighbours " << config.neighbourSamples << "\n"
            << "combat " << (config.combat ? 1 : 0) << "\n"
            << "respawn " << (config.respawn ? 1 : 0) << "\n"
            << "finaltick " << log.finalTick << "\n"
            << "finalhash " << log.finalChecksum << "\n"
            << "commands " << commands.size() << "\n";
        for (const Command& command : commands)
            out << command.tick << ' ' << command.group << ' ' << command.goalTile << ' '
                << static_cast<int>(command.formation) << '\n';
        return WriteTextFile(path, out.str(), error);
    }

    bool ReadCommandLog(const std::string& path, SimConfig& config,
                        ReplayLog& log, std::string& error)
    {
        std::ifstream file(path);
        if (!file)
        {
            error = "could not open '" + path + "'";
            return false;
        }

        std::string token;
        file >> token;
        if (token != "cna-rts-replay")
        {
            error = "'" + path + "' is not a cna-rts replay log";
            return false;
        }
        int version = 0;
        file >> version;
        if (version != 2)
        {
            error = "replay log version " + std::to_string(version) + " is not supported";
            return false;
        }

        // The configuration travels with the log, because a replay against a different map or a
        // different army is not a replay at all -- it would diverge on the first tick and the
        // mismatch would look like a determinism bug rather than like the mistake it is.
        std::size_t expected = 0;
        while (file >> token)
        {
            if (token == "seed") file >> config.seed;
            else if (token == "units") file >> config.unitCount;
            else if (token == "map") file >> config.mapTiles;
            else if (token == "sector") file >> config.sectorSize;
            else if (token == "blocked") file >> config.blockedPercent;
            else if (token == "teams") file >> config.teams;
            else if (token == "groups") file >> config.groupsPerTeam;
            else if (token == "neighbours") file >> config.neighbourSamples;
            else if (token == "combat") { int on = 1; file >> on; config.combat = on != 0; }
            else if (token == "respawn") { int on = 1; file >> on; config.respawn = on != 0; }
            else if (token == "finaltick") file >> log.finalTick;
            else if (token == "finalhash") file >> log.finalChecksum;
            else if (token == "commands") { file >> expected; break; }
            else { error = "unexpected token '" + token + "' in " + path; return false; }
        }

        std::vector<Command>& commands = log.commands;
        commands.clear();
        commands.reserve(expected);
        Command command;
        int formation = 0;
        std::uint32_t tick = 0;
        int group = 0;
        std::int32_t goal = 0;
        while (file >> tick >> group >> goal >> formation)
        {
            command.tick = tick;
            command.group = static_cast<std::uint16_t>(group);
            command.goalTile = goal;
            command.formation = static_cast<std::uint8_t>(formation);
            commands.push_back(command);
        }
        if (expected != 0 && commands.size() != expected)
        {
            error = "replay log says " + std::to_string(expected) + " commands but holds " +
                    std::to_string(commands.size());
            return false;
        }
        return true;
    }
}
