// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Fixed.hpp"
#include "FlowField.hpp"
#include "FogOfWar.hpp"
#include "Formation.hpp"
#include "JobSystem.hpp"
#include "Pathfinder.hpp"
#include "Rng.hpp"
#include "SpatialGrid.hpp"
#include "Terrain.hpp"

namespace CnaRts
{
    /** @brief Simulation ticks per second. Fixed, because a variable step is not deterministic. */
    constexpr int kTicksPerSecond = 30;

    /** @brief Unit archetypes. Purely a speed/size/colour distinction -- there is no economy here. */
    enum class UnitKind : std::uint8_t
    {
        Infantry = 0,
        Cavalry  = 1,
        Siege    = 2,
        Count    = 3,
    };

    /** @brief Everything that decides what a run *is*. Two runs with equal configs are equal runs. */
    struct SimConfig
    {
        std::int32_t unitCount = 100000;
        int mapTiles = 0;            ///< 0 asks for a size that keeps the unit density constant
        int sectorSize = 32;
        int cellShift = 3;           ///< log2 of the spatial-grid cell edge in tiles
        int blockedPercent = 22;
        int teams = 2;               ///< playable teams; the last slot is reserved for the fallen
        int groupsPerTeam = 4;
        std::uint64_t seed = 1;
        bool combat = true;
        bool respawn = true;
        int neighbourSamples = 6;    ///< pairwise separation partners examined per unit per tick
        int visionRadiusCells = 6;
        int fogInterval = 4;         ///< ticks between fog-of-war rebuilds
        int orderIntervalTicks = 600;
        int maxPathsPerTick = 1;
        int checksumInterval = 1;    ///< 0 disables the state hash
        int workers = 0;             ///< 0 asks the hardware
    };

    /** @brief Where one tick's time went, in milliseconds. */
    struct SimStats
    {
        double pathMs = 0.0;
        double gridMs = 0.0;
        double moveMs = 0.0;
        double combatMs = 0.0;
        double fogMs = 0.0;
        double checksumMs = 0.0;
        double totalMs = 0.0;
    };

    /**
     * @brief A group order, stamped with the tick it takes effect on.
     *
     * Orders are the *only* input the simulation accepts, and they carry their tick with them.
     * That is what makes a replay a replay rather than an approximation: the same seed and the
     * same command list reproduce the run exactly, whether the commands came from a player's
     * mouse, from the built-in AI, or from a file recorded last week.
     */
    struct Command
    {
        std::uint32_t tick = 0;
        std::uint16_t group = 0;
        std::int32_t goalTile = 0;
        std::uint8_t formation = 0;
    };

    /** @brief A commanded body of units. Everything expensive in navigation is per group. */
    struct Group
    {
        std::uint8_t team = 0;
        std::int32_t firstUnit = 0;
        std::int32_t unitCount = 0;
        std::int32_t goalTile = 0;
        Vec2f goalPos;
        Vec2f facing{kFixedZero, kFixedOne};
        FormationKind formation = FormationKind::Box;
        Fixed formationSpacing = kFixedZero;
        Fixed formationRadius = kFixedZero;
        bool needsRepath = false;
        bool hasPath = false;
        GroupNavigation nav;
        Corridor corridor;
    };

    /**
     * @brief The deterministic RTS simulation: entities, movement, navigation, combat, fog.
     *
     * Entities live in parallel arrays rather than in objects. At a million units the difference
     * is not stylistic: the movement pass touches position and velocity and nothing else, and a
     * struct-of-entities layout would drag health, team, kind and padding through the cache with
     * them, roughly tripling the bytes the hot loop reads.
     *
     * Positions are double-buffered. A unit reads its neighbours' positions to steer away from
     * them, so writing a new position into the array another thread is still reading would make
     * the result depend on thread timing -- the exact failure a deterministic simulation exists
     * to prevent. Everything else a unit writes is its own, and is written in place.
     */
    class Simulation
    {
    public:
        Simulation();
        ~Simulation();

        Simulation(const Simulation&) = delete;
        Simulation& operator=(const Simulation&) = delete;

        /** @brief Builds the map, the abstract graph and the starting armies. */
        void Initialize(const SimConfig& config);

        /** @brief Advances exactly one fixed step. */
        void Tick();

        /** @brief Queues an order. A tick in the past is clamped to the next tick. */
        void QueueCommand(Command command);

        [[nodiscard]] const SimConfig& Config() const { return config_; }
        [[nodiscard]] const Terrain& GetTerrain() const { return terrain_; }
        [[nodiscard]] const SectorGraph& Graph() const { return graph_; }
        [[nodiscard]] const SpatialGrid& Grid() const { return grid_; }
        [[nodiscard]] const FogOfWar& Fog() const { return fog_; }
        [[nodiscard]] const std::vector<Group>& Groups() const { return groups_; }
        [[nodiscard]] JobSystem& Jobs() { return *jobs_; }

        [[nodiscard]] std::int32_t UnitCount() const { return config_.unitCount; }
        [[nodiscard]] const Fixed* PosX() const { return posX_[current_].data(); }
        [[nodiscard]] const Fixed* PosY() const { return posY_[current_].data(); }
        [[nodiscard]] const Fixed* VelX() const { return velX_.data(); }
        [[nodiscard]] const Fixed* VelY() const { return velY_.data(); }
        [[nodiscard]] const std::uint8_t* TeamOf() const { return team_.data(); }
        [[nodiscard]] const std::uint8_t* KindOf() const { return kind_.data(); }
        [[nodiscard]] const std::uint8_t* HealthOf() const { return health_.data(); }
        [[nodiscard]] const std::uint8_t* GroupOf() const { return groupOf_.data(); }

        [[nodiscard]] std::uint32_t CurrentTick() const { return tick_; }
        [[nodiscard]] std::uint64_t Checksum() const { return checksum_; }
        [[nodiscard]] const SimStats& Stats() const { return stats_; }
        [[nodiscard]] std::int64_t PathQueryCount() const { return pathQueries_; }
        [[nodiscard]] std::int64_t DeathCount() const { return deaths_; }
        [[nodiscard]] std::int32_t AliveOfTeam(int team) const
        {
            return teamAlive_[static_cast<std::size_t>(team)];
        }
        [[nodiscard]] Vec2f TeamCentroid(int team) const
        {
            return teamCentroid_[static_cast<std::size_t>(team)];
        }

        /** @brief Every order this run has executed, in tick order: the replay file's contents. */
        [[nodiscard]] const std::vector<Command>& CommandLog() const { return commandLog_; }
        /** @brief Feeds a recorded log in; auto-orders are suppressed for any tick it covers. */
        void LoadReplay(std::vector<Command> commands);

        /** @brief Tile index nearest to a world position, clamped into the map and to passable. */
        [[nodiscard]] std::int32_t NearestPassableTile(Vec2f world) const;

        static std::int32_t DefaultMapTiles(std::int32_t unitCount, int sectorSize);

    private:
        void BuildArmies();
        void ApplyCommands();
        void IssueAutoOrders();
        void ServicePathQueries();
        void MoveUnits();
        void ResolveCombat();
        void UpdateCentroids();
        void UpdateChecksum();
        void PlanGroup(Group& group);
        void RetargetGroup(Group& group, std::int32_t goalTile, FormationKind formation);

        SimConfig config_;
        std::unique_ptr<JobSystem> jobs_;
        Terrain terrain_;
        SectorGraph graph_;
        SpatialGrid grid_;
        FogOfWar fog_;

        int current_ = 0;
        std::vector<Fixed> posX_[2];
        std::vector<Fixed> posY_[2];
        std::vector<Fixed> velX_, velY_;
        std::vector<std::uint8_t> team_, kind_, health_, groupOf_;

        std::vector<Group> groups_;
        PathfinderScratch scratch_;               ///< path queries run on one thread, in order
        std::vector<std::int32_t> repathQueue_;
        std::vector<std::vector<std::int32_t>> spawnTiles_;  ///< per team

        std::vector<Command> pendingCommands_;
        std::vector<Command> commandLog_;
        std::vector<Command> replay_;
        std::size_t replayCursor_ = 0;
        std::uint32_t replayCoversUntil_ = 0;

        std::vector<std::uint64_t> chunkHash_;
        std::vector<std::int64_t> chunkSumX_, chunkSumY_;
        std::vector<std::int32_t> chunkAlive_;
        std::vector<std::int32_t> teamAlive_;
        std::vector<Vec2f> teamCentroid_;

        Rng orderRng_;
        std::uint32_t tick_ = 0;
        std::uint64_t checksum_ = 0;
        std::int64_t pathQueries_ = 0;
        std::int64_t deaths_ = 0;
        SimStats stats_;
    };
}
