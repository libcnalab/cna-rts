// SPDX-License-Identifier: MIT
#include <exception>
#include <iostream>
#include <string>

#include "CNA/Platform/Entrypoint.hpp"

#include "Benchmark.hpp"
#include "CliOptions.hpp"
#include "RtsGame.hpp"

int main(int argc, char** argv)
{
    using namespace CnaRts;

    const CliOptions options = CliOptions::Parse(argc, argv);
    const char* executable = argc > 0 ? argv[0] : "cna-rts";

    if (options.showHelp)
    {
        PrintHelp(executable);
        return 0;
    }
    if (options.parseError)
    {
        std::cerr << "cna-rts: " << options.errorMessage << "\n\n";
        PrintHelp(executable);
        return 2;
    }

    try
    {
        if (options.mode == RunMode::SimOnly) return RunSimOnlyBenchmark(options);
        if (options.mode == RunMode::Verify) return RunVerify(options);

        RtsGame game(options);
        game.Run();

        if (!options.recordPath.empty())
        {
            ReplayLog log;
            log.commands = game.Sim().CommandLog();
            log.finalTick = game.Sim().CurrentTick();
            log.finalChecksum = game.Sim().Checksum();
            std::string error;
            if (WriteCommandLog(options.recordPath, game.Sim().Config(), log, error))
            {
                std::cout << "cna-rts: recorded " << log.commands.size() << " orders to "
                          << options.recordPath << "\n"
                          << "cna-rts: final hash " << std::hex << log.finalChecksum << std::dec
                          << " at tick " << log.finalTick << "\n";
            }
            else
            {
                std::cerr << "cna-rts: " << error << "\n";
                return 1;
            }
        }

        if (options.mode == RunMode::Benchmark)
        {
            const std::string caption =
                "Measured on screen at " + std::to_string(options.windowWidth) + "x" +
                std::to_string(options.windowHeight) + ", " +
                DescribeHost(options.sim, game.RendererName()) + ", " +
                std::to_string(options.benchFrames) + " frames per scale after " +
                std::to_string(options.benchWarmupFrames) + " warm-up frames.";
            const std::string markdown = RenderMarkdown(game.BenchmarkRows(), caption);
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
        }

        if (!game.Diagnostic().empty())
        {
            std::cerr << "cna-rts: " << game.Diagnostic() << "\n";
            return 1;
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "cna-rts: uncaught exception: " << error.what() << "\n";
        return 1;
    }
    catch (...)
    {
        std::cerr << "cna-rts: terminated by an unknown exception\n";
        return 1;
    }

    return 0;
}
