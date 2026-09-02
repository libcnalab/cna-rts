// SPDX-License-Identifier: MIT
#include "CliOptions.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace CnaRts
{
    namespace
    {
        /** @brief Splits `--flag=value` into its two halves; a bare flag yields an empty value. */
        void SplitAssignment(const std::string& argument, std::string& flag, std::string& value,
                             bool& hasValue)
        {
            const std::size_t equals = argument.find('=');
            if (equals == std::string::npos)
            {
                flag = argument;
                value.clear();
                hasValue = false;
                return;
            }
            flag = argument.substr(0, equals);
            value = argument.substr(equals + 1);
            hasValue = true;
        }

        std::vector<std::string> Split(const std::string& text, char separator)
        {
            std::vector<std::string> parts;
            std::stringstream stream(text);
            std::string item;
            while (std::getline(stream, item, separator))
                if (!item.empty()) parts.push_back(item);
            return parts;
        }
    }

    bool ParseCount(const std::string& text, std::int32_t& out)
    {
        if (text.empty()) return false;
        char* endptr = nullptr;
        const double value = std::strtod(text.c_str(), &endptr);
        if (endptr == text.c_str()) return false;
        double scaled = value;
        while (*endptr != '\0')
        {
            switch (std::tolower(static_cast<unsigned char>(*endptr)))
            {
                case 'k': scaled *= 1000.0; break;
                case 'm': scaled *= 1000000.0; break;
                case '_': case ' ': case ',': break;
                default: return false;
            }
            ++endptr;
        }
        if (scaled < 1.0 || scaled > 2147483000.0) return false;
        out = static_cast<std::int32_t>(scaled);
        return true;
    }

    CliOptions CliOptions::Parse(int argc, char** argv)
    {
        CliOptions options;
        const auto fail = [&](const std::string& message) {
            options.parseError = true;
            if (options.errorMessage.empty()) options.errorMessage = message;
        };

        for (int i = 1; i < argc; ++i)
        {
            std::string flag, value;
            bool hasValue = false;
            SplitAssignment(argv[i], flag, value, hasValue);

            // `--flag value` is accepted as well as `--flag=value`, because both spellings turn up
            // in shell history and refusing one of them is a papercut with no upside.
            const auto take = [&](const char* name) -> bool {
                if (flag != name) return false;
                if (!hasValue)
                {
                    if (i + 1 >= argc) { fail(std::string(name) + " needs a value"); return false; }
                    value = argv[++i];
                }
                return true;
            };
            const auto takeInt = [&](const char* name, int& target, int lo, int hi) -> bool {
                if (!take(name)) return false;
                try
                {
                    const int parsed = std::stoi(value);
                    if (parsed < lo || parsed > hi)
                        fail(std::string(name) + " must be between " + std::to_string(lo) + " and " +
                             std::to_string(hi));
                    else
                        target = parsed;
                }
                catch (const std::exception&) { fail(std::string(name) + " wants a number"); }
                return true;
            };

            if (flag == "-h" || flag == "--help") { options.showHelp = true; continue; }

            if (take("--units"))
            {
                if (!ParseCount(value, options.sim.unitCount))
                    fail("--units wants a count like 250000, 250k or 1M");
                continue;
            }
            if (take("--seed"))
            {
                try { options.sim.seed = std::stoull(value); }
                catch (const std::exception&) { fail("--seed wants a number"); }
                continue;
            }
            if (takeInt("--map", options.sim.mapTiles, 64, 4096)) continue;
            if (takeInt("--sector", options.sim.sectorSize, 8, 64)) continue;
            if (takeInt("--cell-shift", options.sim.cellShift, 1, 8)) continue;
            if (takeInt("--blocked", options.sim.blockedPercent, 0, 60)) continue;
            if (takeInt("--teams", options.sim.teams, 2, kMaxTeams - 1)) continue;
            if (takeInt("--groups", options.sim.groupsPerTeam, 1, 32)) continue;
            if (takeInt("--neighbours", options.sim.neighbourSamples, 0, 32)) continue;
            if (takeInt("--neighbors", options.sim.neighbourSamples, 0, 32)) continue;
            if (takeInt("--vision", options.sim.visionRadiusCells, 1, 64)) continue;
            if (takeInt("--fog-interval", options.sim.fogInterval, 1, 240)) continue;
            if (takeInt("--order-interval", options.sim.orderIntervalTicks, 0, 100000)) continue;
            if (takeInt("--paths-per-tick", options.sim.maxPathsPerTick, 1, 64)) continue;
            if (takeInt("--workers", options.sim.workers, 0, 256)) continue;
            if (takeInt("--width", options.windowWidth, 320, 16384)) continue;
            if (takeInt("--height", options.windowHeight, 240, 16384)) continue;
            if (takeInt("--viewer", options.viewerTeam, -1, kMaxTeams - 2)) continue;
            if (takeInt("--bench-frames", options.benchFrames, 10, 1000000)) continue;
            if (takeInt("--bench-warmup", options.benchWarmupFrames, 0, 1000000)) continue;
            if (takeInt("--verify-ticks", options.verifyTicks, 1, 1000000)) continue;
            if (takeInt("--max-ticks", options.maxTicks, 1, 100000000)) continue;
            if (takeInt("--screenshot-frame", options.screenshotFrame, 1, 1000000)) continue;

            if (take("--checksum"))
            {
                if (value == "off") options.sim.checksumInterval = 0;
                else
                {
                    try { options.sim.checksumInterval = std::stoi(value); }
                    catch (const std::exception&) { fail("--checksum wants a tick count or 'off'"); }
                }
                continue;
            }
            if (take("--zoom"))
            {
                try { options.zoom = std::stof(value); }
                catch (const std::exception&) { fail("--zoom wants a pixels-per-tile number"); }
                continue;
            }
            if (take("--bench-scales"))
            {
                options.benchScales.clear();
                for (const std::string& part : Split(value, ','))
                {
                    std::int32_t parsed = 0;
                    if (ParseCount(part, parsed)) options.benchScales.push_back(parsed);
                    else fail("--bench-scales wants a comma-separated list like 10k,100k,1M");
                }
                if (options.benchScales.empty()) fail("--bench-scales listed no usable scale");
                continue;
            }
            if (take("--bench-out")) { options.benchOutput = value; continue; }
            if (take("--screenshot")) { options.screenshotPath = value; continue; }
            if (take("--record")) { options.recordPath = value; continue; }
            if (take("--replay")) { options.replayPath = value; continue; }

            if (flag == "--bench") { options.mode = RunMode::Benchmark; continue; }
            if (flag == "--sim-only") { options.mode = RunMode::SimOnly; continue; }
            if (flag == "--verify") { options.mode = RunMode::Verify; continue; }
            if (flag == "--fullscreen") { options.fullScreen = true; continue; }
            if (flag == "--vsync") { options.vsync = true; continue; }
            if (flag == "--no-vsync") { options.vsync = false; continue; }
            if (flag == "--no-fog") { options.fogEnabled = false; continue; }
            if (flag == "--follow") { options.follow = true; continue; }
            if (flag == "--no-combat") { options.sim.combat = false; continue; }
            if (flag == "--no-respawn") { options.sim.respawn = false; continue; }
            if (flag == "--screenshot-exit") { options.screenshotExit = true; continue; }

            fail("unknown option '" + flag + "'");
        }

        // A capture asked for on its own is a capture the caller wants to collect and be done
        // with; one asked for during a benchmark sweep is a picture *of* the sweep and must not
        // cut it short.
        if (!options.screenshotPath.empty() && options.mode == RunMode::Interactive)
            options.screenshotExit = true;

        return options;
    }

    void PrintHelp(const char* executable)
    {
        std::cout <<
            "cna-rts -- One Million Units: an RTS scaling benchmark on the CNA runtime\n"
            "\n"
            "USAGE\n"
            "  " << executable << " [options]\n"
            "\n"
            "MODES\n"
            "  (default)             open a window and run interactively\n"
            "  --bench               sweep --bench-scales on screen, print a markdown table, exit\n"
            "  --sim-only            no window: measure the simulation alone across the scales\n"
            "  --verify              no window: prove the run is deterministic, then exit\n"
            "\n"
            "SCALE AND WORLD\n"
            "  --units N             units to simulate; 250000, 250k and 1M all work (100k)\n"
            "  --map N               map edge in tiles; the default keeps unit density constant\n"
            "  --sector N            sector edge in tiles, the coarse path graph's grain (32)\n"
            "  --cell-shift N        log2 of the spatial-grid cell edge in tiles (3)\n"
            "  --blocked N           percent of the map made impassable (22)\n"
            "  --teams N             playable teams, 2 or 3 (2)\n"
            "  --groups N            commanded groups per team (4)\n"
            "  --seed N              run seed; the same seed is the same run (1)\n"
            "\n"
            "SIMULATION\n"
            "  --neighbours N        separation partners examined per unit per tick (6)\n"
            "  --vision N            vision radius in grid cells (6)\n"
            "  --fog-interval N      ticks between fog-of-war rebuilds (4)\n"
            "  --order-interval N    ticks between a group's automatic orders (600)\n"
            "  --paths-per-tick N    abstract path queries serviced per tick (1)\n"
            "  --checksum N|off      ticks between state hashes; 'off' disables them (1)\n"
            "  --no-combat           units pass through each other without fighting\n"
            "  --no-respawn          the fallen stay fallen instead of rejoining their army\n"
            "  --workers N           worker threads; 0 asks the hardware (0)\n"
            "\n"
            "PRESENTATION\n"
            "  --width N --height N  window size (1600x900)\n"
            "  --fullscreen          borderless full screen\n"
            "  --vsync               wait for vertical retrace (off: a benchmark measures work)\n"
            "  --zoom N              pixels per tile; the default fits the whole map\n"
            "  --viewer N            team whose fog of war is shown; -1 reveals everything (0)\n"
            "  --no-fog              disable fog of war entirely\n"
            "  --follow              keep the camera on the front line (default under --bench)\n"
            "\n"
            "BENCHMARK\n"
            "  --bench-scales LIST   comma-separated unit counts (10k,100k,1M)\n"
            "  --bench-frames N      measured frames per scale (600)\n"
            "  --bench-warmup N      discarded frames per scale (90)\n"
            "  --bench-out FILE      also write the markdown table here\n"
            "  --screenshot FILE     write a PNG of frame --screenshot-frame and (alone) exit\n"
            "  --screenshot-frame N  which frame to capture (240)\n"
            "  --screenshot-exit     quit as soon as the capture is written\n"
            "  --verify-ticks N      ticks each --verify run advances (400)\n"
            "  --max-ticks N         quit once the simulation reaches tick N -- the tick count,\n"
            "                        not the frame count, so a record and its replay stop in the\n"
            "                        same place whatever the frame rate\n"
            "\n"
            "REPLAY\n"
            "  --record FILE         write every order issued this run\n"
            "  --replay FILE         replay a recorded order log and check the hashes agree\n"
            "\n"
            "  -h, --help            this text\n";
    }
}
