// SPDX-License-Identifier: MIT
#include "Simulation.hpp"

#include <algorithm>
#include <array>

#include "System/Diagnostics/Stopwatch.hpp"

namespace CnaRts
{
    namespace
    {
        /** @brief Where the fallen are parked. Never a playable team, so nobody targets them. */
        constexpr int kFallenTeam = kMaxTeams - 1;

        /** @brief Per-archetype top speed, in world units per tick. */
        const Fixed kSpeedOfKind[3] = {
            Fixed::FromRaw(6000),   // Infantry: ~2.7 tiles/second at 30 Hz
            Fixed::FromRaw(9200),   // Cavalry
            Fixed::FromRaw(3600),   // Siege
        };
        /** @brief How much of the gap between current and desired velocity is closed per tick. */
        constexpr Fixed kAcceleration = Fixed::FromRaw(20000);   // ~0.305
        /** @brief Smallest slot spacing inside a formation, in world units. Larger groups widen
         *  it themselves through FormationSpacingFor(). */
        constexpr Fixed kMinFormationSpacing = Fixed::FromRaw(45000);   // ~0.69 tiles
        /** @brief How hard neighbours push each other apart, relative to top speed. */
        constexpr Fixed kSeparationStrength = Fixed::FromRaw(18000);
        /**
         * @brief Ceiling on the total crowd push, relative to the unit-length goal direction.
         *
         * Without this an army does not march. Separation is summed over several neighbours and
         * a dense spawn puts a hundred and sixty units in one cell, so the sum reaches three
         * times the length of the direction vector it is added to -- and a unit whose steering
         * is three parts "get away from these people" and one part "walk east" mills about in a
         * churning blob instead of moving. Capped below one, the goal always wins the argument
         * and the crowd behaviour becomes what it should be: a correction, not a destination.
         */
        constexpr Fixed kMaxCrowdPush = Fixed::FromRaw(45000);
        /** @brief How hard a crowded cell pushes toward an emptier one. */
        constexpr Fixed kDensityStrength = Fixed::FromRaw(9000);

        /**
         * @brief Separation falloff, bucketed by squared distance.
         *
         * The inner loop of the movement pass runs `unitCount * neighbourSamples` times a tick --
         * six million times at the top scale -- so it may not contain a square root. It does not
         * need one: the *direction* of a push is quantised to eight compass points by sign tests,
         * and its *magnitude* comes out of this table, indexed by the squared distance shifted
         * down. Nothing in the loop divides, and the visual difference at a scale where a unit is
         * two pixels across is precisely none.
         */
        constexpr int kSeparationBuckets = 64;
        constexpr int kSeparationShift = 26;   // (dx^2 + dy^2) >> 26 == 64 at one world unit

        /** @brief `1 / (population + 8)`, tabulated: the crowd-gradient normaliser, without the
         *  integer division it would otherwise cost every unit every tick. */
        constexpr int kInverseDensityEntries = 1024;
        std::array<Fixed, kInverseDensityEntries> BuildInverseDensity()
        {
            std::array<Fixed, kInverseDensityEntries> table{};
            for (int i = 0; i < kInverseDensityEntries; ++i)
                table[static_cast<std::size_t>(i)] = Fixed::FromRatio(1, i + 8);
            return table;
        }
        const std::array<Fixed, kInverseDensityEntries> kInverseDensity = BuildInverseDensity();

        std::array<Fixed, kSeparationBuckets> BuildSeparationFalloff()
        {
            std::array<Fixed, kSeparationBuckets> table{};
            for (int i = 0; i < kSeparationBuckets; ++i)
            {
                // 1 - sqrt(i / 64): full strength when touching, zero at one world unit apart.
                const auto root = static_cast<std::int32_t>(
                    IntSqrt64(static_cast<std::uint64_t>(i) * Fixed::kOne * Fixed::kOne /
                              kSeparationBuckets));
                table[static_cast<std::size_t>(i)] = Fixed::FromRaw(Fixed::kOne - root);
            }
            return table;
        }
        const std::array<Fixed, kSeparationBuckets> kSeparationFalloff = BuildSeparationFalloff();

        /** @brief Quantises a vector to one of the eight compass directions, with no arithmetic
         *  beyond comparisons. Returns an index into kDirection8. */
        inline int QuantiseDirection(Fixed dx, Fixed dy)
        {
            const std::int32_t ax = dx.raw < 0 ? -dx.raw : dx.raw;
            const std::int32_t ay = dy.raw < 0 ? -dy.raw : dy.raw;
            // tan(22.5 degrees) is 0.4142; 2/5 is close enough to place the octant boundaries.
            if (static_cast<std::int64_t>(ay) * 5 < static_cast<std::int64_t>(ax) * 2)
                return dx.raw >= 0 ? 0 : 1;
            if (static_cast<std::int64_t>(ax) * 5 < static_cast<std::int64_t>(ay) * 2)
                return dy.raw >= 0 ? 2 : 3;
            if (dx.raw >= 0) return dy.raw >= 0 ? 4 : 5;
            return dy.raw >= 0 ? 6 : 7;
        }

        /**
         * @brief Scales @p v to @p length using an octagonal length approximation.
         *
         * `max + 0.406 * min` is within 4% of the true magnitude at every angle, which for a
         * steering direction is indistinguishable, and it replaces a square root with two
         * comparisons and a shift. The one division that remains is per unit, not per neighbour.
         */
        inline Vec2f ScaleToLength(Vec2f v, Fixed length)
        {
            const std::int32_t ax = v.x.raw < 0 ? -v.x.raw : v.x.raw;
            const std::int32_t ay = v.y.raw < 0 ? -v.y.raw : v.y.raw;
            const std::int32_t hi = ax > ay ? ax : ay;
            const std::int32_t lo = ax > ay ? ay : ax;
            const std::int32_t approx = hi + ((lo * 13) >> 5);
            if (approx <= 0) return {};
            const Fixed scale = length / Fixed::FromRaw(approx);
            return {v.x * scale, v.y * scale};
        }

        inline std::uint64_t MixHash(std::uint64_t z)
        {
            z += 0x9E3779B97F4A7C15ull;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }

        inline double ElapsedMs(const System::Diagnostics::Stopwatch& watch)
        {
            return static_cast<double>(watch.getElapsedTicksProperty()) / 10000.0;
        }
    }

    Simulation::Simulation() = default;
    Simulation::~Simulation() = default;

    std::int32_t Simulation::DefaultMapTiles(std::int32_t unitCount, int sectorSize)
    {
        // Density, not extent, is what the benchmark holds constant. If the map stayed the same
        // size while the army grew a hundredfold, the 10k row would be measuring an empty map and
        // the 1M row a solid wall of units, and the two numbers would not be comparable.
        //
        // The constant is 1.25 tiles of map edge per square root of the army, which works out to
        // about one unit per one and a half tiles. Twice that spacing was the first choice and
        // looked wrong on screen -- an army thinned across four tiles per soldier reads as a
        // skirmish line rather than a horde -- and it cost more than it looked: the spatial grid
        // was four times as many cells for the same number of units, so the per-cell count table
        // the sort walks twice a tick grew from one and a half megabytes to sixteen.
        const auto side = static_cast<std::int32_t>(
            (5 * IntSqrt64(static_cast<std::uint64_t>(std::max(unitCount, 1)))) / 4);
        const std::int32_t rounded = ((side + sectorSize - 1) / sectorSize) * sectorSize;
        return std::clamp(rounded, static_cast<std::int32_t>(sectorSize * 4),
                          static_cast<std::int32_t>(4096));
    }

    void Simulation::Initialize(const SimConfig& config)
    {
        config_ = config;
        config_.unitCount = std::max<std::int32_t>(1, config_.unitCount);
        config_.teams = std::clamp(config_.teams, 2, kMaxTeams - 1);
        config_.groupsPerTeam = std::clamp(config_.groupsPerTeam, 1, 32);
        config_.sectorSize = std::clamp(config_.sectorSize, 8, 64);
        if (config_.mapTiles <= 0)
            config_.mapTiles = DefaultMapTiles(config_.unitCount, config_.sectorSize);

        jobs_ = std::make_unique<JobSystem>(config_.workers);
        const int workers = jobs_->WorkerCount();

        terrain_.Generate(config_.mapTiles, config_.sectorSize, config_.seed, config_.blockedPercent);
        config_.mapTiles = terrain_.Size();
        graph_.Build(terrain_, *jobs_);
        grid_.Configure(terrain_.Size(), config_.cellShift, workers);
        fog_.Configure(grid_.CellsPerSide(), kMaxTeams);

        chunkHash_.assign(static_cast<std::size_t>(workers), 0);
        chunkSumX_.assign(static_cast<std::size_t>(workers) * kMaxTeams, 0);
        chunkSumY_.assign(static_cast<std::size_t>(workers) * kMaxTeams, 0);
        chunkAlive_.assign(static_cast<std::size_t>(workers) * kMaxTeams, 0);
        teamAlive_.assign(kMaxTeams, 0);
        teamCentroid_.assign(kMaxTeams, Vec2f{});

        orderRng_.Seed(config_.seed ^ 0x5DEECE66Dull);
        tick_ = 0;
        checksum_ = 0;
        pathQueries_ = 0;
        deaths_ = 0;
        pendingCommands_.clear();
        commandLog_.clear();
        repathQueue_.clear();

        BuildArmies();

        grid_.Build(*jobs_, config_.unitCount, posX_[current_].data(), posY_[current_].data(),
                    team_.data());
        UpdateCentroids();
        fog_.Reset();
        fog_.Update(*jobs_, grid_, config_.visionRadiusCells);
    }

    void Simulation::BuildArmies()
    {
        const std::int32_t count = config_.unitCount;
        const int tiles = terrain_.Size();
        const int groupCount = config_.teams * config_.groupsPerTeam;

        posX_[0].assign(static_cast<std::size_t>(count), kFixedZero);
        posY_[0].assign(static_cast<std::size_t>(count), kFixedZero);
        posX_[1].assign(static_cast<std::size_t>(count), kFixedZero);
        posY_[1].assign(static_cast<std::size_t>(count), kFixedZero);
        velX_.assign(static_cast<std::size_t>(count), kFixedZero);
        velY_.assign(static_cast<std::size_t>(count), kFixedZero);
        team_.assign(static_cast<std::size_t>(count), 0);
        kind_.assign(static_cast<std::size_t>(count), 0);
        health_.assign(static_cast<std::size_t>(count), 255);
        groupOf_.assign(static_cast<std::size_t>(count), 0);
        current_ = 0;

        // Armies are drawn up facing each other across the map, each filling its own band, with
        // a narrow no-man's-land down the middle.
        //
        // Opposite corners were the obvious arrangement and were wrong for this benchmark. The
        // map grows with the army to hold density constant, so at a million units it is two
        // thousand tiles across and a corner-to-corner march at three tiles a second takes ten
        // minutes -- the armies never meet, nothing ever fights, and the interesting case is
        // never measured. Facing bands put them in contact within seconds at every scale, and
        // still leave the whole map to manoeuvre across afterwards.
        spawnTiles_.assign(static_cast<std::size_t>(kMaxTeams), {});
        const int margin = std::max(2, tiles / 40);
        const int bandWidth = tiles / config_.teams;
        for (int t = 0; t < config_.teams; ++t)
        {
            auto& pool = spawnTiles_[static_cast<std::size_t>(t)];
            const int x0 = t * bandWidth + margin;
            const int x1 = (t + 1) * bandWidth - margin;
            // Sampled on a stride rather than tile by tile: at four million tiles the exhaustive
            // list is fifty megabytes of spawn pool nobody needs that finely.
            const int stride = std::max(1, tiles / 512);
            for (int y = margin; y < tiles - margin; y += stride)
                for (int x = x0; x < x1; x += stride)
                    if (terrain_.Passable(x, y)) pool.push_back(terrain_.Index(x, y));
            if (pool.empty()) pool = terrain_.PassableTiles();
        }

        groups_.clear();
        groups_.resize(static_cast<std::size_t>(groupCount));
        const std::int32_t perGroup = count / groupCount;
        const std::int32_t remainder = count % groupCount;

        Rng rng(config_.seed ^ 0x1234ABCDull);
        std::int32_t cursor = 0;
        for (int g = 0; g < groupCount; ++g)
        {
            Group& group = groups_[static_cast<std::size_t>(g)];
            group.team = static_cast<std::uint8_t>(g / config_.groupsPerTeam);
            group.firstUnit = cursor;
            group.unitCount = perGroup + (g < remainder ? 1 : 0);
            cursor += group.unitCount;
            group.formation = static_cast<FormationKind>(
                1 + (g % (static_cast<int>(FormationKind::Count) - 1)));
            group.formationSpacing =
                FormationSpacingFor(group.unitCount, kMinFormationSpacing);
            group.formationRadius =
                FormationRadius(group.formation, group.unitCount, group.formationSpacing);
            group.nav.Configure(terrain_);

            const auto& pool = spawnTiles_[static_cast<std::size_t>(group.team)];
            for (std::int32_t i = group.firstUnit; i < group.firstUnit + group.unitCount; ++i)
            {
                const std::int32_t tile = pool[rng.NextBelow(static_cast<std::uint32_t>(pool.size()))];
                posX_[0][static_cast<std::size_t>(i)] =
                    Fixed::FromRaw((tile % tiles) * Fixed::kOne +
                                   static_cast<std::int32_t>(rng.NextBelow(Fixed::kOne)));
                posY_[0][static_cast<std::size_t>(i)] =
                    Fixed::FromRaw((tile / tiles) * Fixed::kOne +
                                   static_cast<std::int32_t>(rng.NextBelow(Fixed::kOne)));
                team_[static_cast<std::size_t>(i)] = group.team;
                groupOf_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(g);
                // Cavalry and siege are the minority, as they would be in a real army; the mix is
                // what makes a marching column stretch out instead of moving as a rigid block.
                const std::uint32_t roll = rng.NextBelow(100);
                kind_[static_cast<std::size_t>(i)] =
                    roll < 65 ? 0 : (roll < 90 ? 1 : 2);
            }

            group.goalTile = pool[rng.NextBelow(static_cast<std::uint32_t>(pool.size()))];
            group.goalPos = {Fixed::FromRaw((group.goalTile % tiles) * Fixed::kOne + Fixed::kHalf),
                             Fixed::FromRaw((group.goalTile / tiles) * Fixed::kOne + Fixed::kHalf)};
            group.needsRepath = true;
            repathQueue_.push_back(g);
        }
    }

    void Simulation::LoadReplay(std::vector<Command> commands)
    {
        std::stable_sort(commands.begin(), commands.end(),
                         [](const Command& a, const Command& b) {
                             return a.tick != b.tick ? a.tick < b.tick : a.group < b.group;
                         });
        replayCoversUntil_ = commands.empty() ? 0 : commands.back().tick;
        replay_ = std::move(commands);
        replayCursor_ = 0;
    }

    void Simulation::QueueCommand(Command command)
    {
        if (command.tick <= tick_) command.tick = tick_ + 1;
        pendingCommands_.push_back(command);
    }

    std::int32_t Simulation::NearestPassableTile(Vec2f world) const
    {
        const int tiles = terrain_.Size();
        const int tx = std::clamp(world.x.ToInt(), 0, tiles - 1);
        const int ty = std::clamp(world.y.ToInt(), 0, tiles - 1);
        if (terrain_.Passable(tx, ty)) return terrain_.Index(tx, ty);
        // A widening ring search. The map has exactly one connected region, so this terminates
        // long before it runs out of rings.
        for (int r = 1; r < tiles; ++r)
        {
            for (int d = -r; d <= r; ++d)
            {
                const int candidates[4][2] = {
                    {tx + d, ty - r}, {tx + d, ty + r}, {tx - r, ty + d}, {tx + r, ty + d}};
                for (const auto& c : candidates)
                    if (terrain_.Passable(c[0], c[1])) return terrain_.Index(c[0], c[1]);
            }
        }
        return terrain_.PassableTiles().empty() ? 0 : terrain_.PassableTiles().front();
    }

    void Simulation::RetargetGroup(Group& group, std::int32_t goalTile, FormationKind formation)
    {
        const int tiles = terrain_.Size();
        group.goalTile = goalTile;
        group.goalPos = {Fixed::FromRaw((goalTile % tiles) * Fixed::kOne + Fixed::kHalf),
                         Fixed::FromRaw((goalTile / tiles) * Fixed::kOne + Fixed::kHalf)};
        group.formation = formation;
        group.formationSpacing = FormationSpacingFor(group.unitCount, kMinFormationSpacing);
        group.formationRadius =
            FormationRadius(formation, group.unitCount, group.formationSpacing);
        group.needsRepath = true;
        group.hasPath = false;
    }

    void Simulation::ApplyCommands()
    {
        if (replayCursor_ < replay_.size())
        {
            while (replayCursor_ < replay_.size() && replay_[replayCursor_].tick <= tick_)
                pendingCommands_.push_back(replay_[replayCursor_++]);
        }
        if (pendingCommands_.empty()) return;

        // Sorted by (tick, group) before application: two commands landing on the same tick must
        // resolve in an order that does not depend on which one was queued first by a mouse.
        std::stable_sort(pendingCommands_.begin(), pendingCommands_.end(),
                         [](const Command& a, const Command& b) {
                             return a.tick != b.tick ? a.tick < b.tick : a.group < b.group;
                         });

        std::size_t applied = 0;
        while (applied < pendingCommands_.size() && pendingCommands_[applied].tick <= tick_)
        {
            const Command& command = pendingCommands_[applied];
            if (command.group < groups_.size())
            {
                Command stamped = command;
                stamped.tick = tick_;
                commandLog_.push_back(stamped);
                Group& group = groups_[command.group];
                RetargetGroup(group, command.goalTile,
                              static_cast<FormationKind>(
                                  command.formation % static_cast<int>(FormationKind::Count)));
                repathQueue_.push_back(static_cast<std::int32_t>(command.group));
            }
            ++applied;
        }
        pendingCommands_.erase(pendingCommands_.begin(),
                               pendingCommands_.begin() + static_cast<std::ptrdiff_t>(applied));
    }

    void Simulation::IssueAutoOrders()
    {
        if (config_.orderIntervalTicks <= 0) return;
        if (tick_ <= replayCoversUntil_ && !replay_.empty()) return;   // the log is in charge

        const int groupCount = static_cast<int>(groups_.size());
        const std::uint32_t interval = static_cast<std::uint32_t>(config_.orderIntervalTicks);
        const std::uint32_t stride = std::max<std::uint32_t>(1, interval / groupCount);

        for (int g = 0; g < groupCount; ++g)
        {
            // Groups are staggered across the interval so the path queries they trigger arrive
            // one at a time instead of all landing on the same tick.
            if (tick_ % interval != (static_cast<std::uint32_t>(g) * stride) % interval) continue;

            Group& group = groups_[static_cast<std::size_t>(g)];
            const int enemyTeam = (group.team + 1) % config_.teams;
            std::int32_t goalTile;
            // Mostly "march on the enemy". A group that wanders off to a random corner every
            // third order never reaches a battle, and a benchmark whose armies never meet is not
            // measuring the case it exists to measure.
            if (orderRng_.NextBelow(100) < 88 && teamAlive_[static_cast<std::size_t>(enemyTeam)] > 0)
            {
                // March on the enemy's centre of mass, offset a little so the four groups of a
                // team converge on a front rather than on a point.
                const Vec2f centre = teamCentroid_[static_cast<std::size_t>(enemyTeam)];
                const int spread = std::max(6, terrain_.Size() / 24);
                const Vec2f aim{centre.x + Fixed::FromInt(orderRng_.NextRange(-spread, spread)),
                                centre.y + Fixed::FromInt(orderRng_.NextRange(-spread, spread))};
                goalTile = NearestPassableTile(aim);
            }
            else
            {
                const auto& tiles = terrain_.PassableTiles();
                goalTile = tiles[orderRng_.NextBelow(static_cast<std::uint32_t>(tiles.size()))];
            }

            Command command;
            command.tick = tick_;
            command.group = static_cast<std::uint16_t>(g);
            command.goalTile = goalTile;
            command.formation = static_cast<std::uint8_t>(
                1 + orderRng_.NextBelow(static_cast<std::uint32_t>(FormationKind::Count) - 1));
            pendingCommands_.push_back(command);
        }
    }

    void Simulation::PlanGroup(Group& group)
    {
        // The corridor is found from the group's own centre of mass, not from an arbitrary
        // member: a plan anchored on unit zero sends a hundred thousand units around whichever
        // side of a lake that one unit happened to be standing on.
        Vec2f centre{};
        const std::int32_t sampleStep = std::max<std::int32_t>(1, group.unitCount / 64);
        std::int64_t sumX = 0, sumY = 0;
        std::int32_t samples = 0;
        for (std::int32_t i = group.firstUnit; i < group.firstUnit + group.unitCount; i += sampleStep)
        {
            sumX += posX_[current_][static_cast<std::size_t>(i)].raw;
            sumY += posY_[current_][static_cast<std::size_t>(i)].raw;
            ++samples;
        }
        if (samples > 0)
            centre = {Fixed::FromRaw(static_cast<std::int32_t>(sumX / samples)),
                      Fixed::FromRaw(static_cast<std::int32_t>(sumY / samples))};

        const std::int32_t startTile = NearestPassableTile(centre);
        const std::int32_t goalTile = terrain_.PassableIndex(group.goalTile)
                                          ? group.goalTile
                                          : NearestPassableTile(group.goalPos);
        group.goalTile = goalTile;

        ++pathQueries_;
        if (FindCorridor(terrain_, graph_, startTile, goalTile, scratch_, group.corridor))
        {
            group.nav.Plan(terrain_, graph_, group.corridor, scratch_);
            group.hasPath = true;
        }
        else
        {
            group.nav.Clear();
            group.hasPath = false;
        }
        group.needsRepath = false;

        // The formation faces the way the group is travelling, so a line arrives broadside-on.
        const Vec2f heading = group.goalPos - centre;
        const Vec2f facing = ScaleToLength(heading, kFixedOne);
        if (facing.x.raw != 0 || facing.y.raw != 0) group.facing = facing;
    }

    void Simulation::ServicePathQueries()
    {
        int budget = std::max(1, config_.maxPathsPerTick);
        while (budget > 0 && !repathQueue_.empty())
        {
            const std::int32_t g = repathQueue_.front();
            repathQueue_.erase(repathQueue_.begin());
            if (g < 0 || g >= static_cast<std::int32_t>(groups_.size())) continue;
            Group& group = groups_[static_cast<std::size_t>(g)];
            if (!group.needsRepath) continue;
            PlanGroup(group);
            --budget;
        }
    }

    void Simulation::MoveUnits()
    {
        const std::int32_t count = config_.unitCount;
        const int tiles = terrain_.Size();
        const int sectorShift = terrain_.SectorShift();
        const int sectorSize = terrain_.SectorSize();
        const int sectorsPerSide = terrain_.SectorsPerSide();
        const int cellShift = grid_.CellShift();
        const int cellsPerSide = grid_.CellsPerSide();
        const int neighbourSamples = std::max(0, config_.neighbourSamples);

        const Fixed* readX = posX_[current_].data();
        const Fixed* readY = posY_[current_].data();
        Fixed* writeX = posX_[1 - current_].data();
        Fixed* writeY = posY_[1 - current_].data();
        const GridPoint* sortedPos = grid_.SortedPositions();
        const std::int32_t* sortedSlot = grid_.SortedSlotOfUnit();
        const std::int32_t* cellStarts = grid_.CellStarts();
        const std::int32_t* cellOf = grid_.CellOfUnit().data();

        const Fixed lowBound = Fixed::FromRaw(Fixed::kHalf);
        const Fixed highBound = Fixed::FromRaw((tiles - 1) * Fixed::kOne + Fixed::kHalf);

        jobs_->ParallelChunks(count, [&](int, std::int32_t begin, std::int32_t end) {
            for (std::int32_t i = begin; i < end; ++i)
            {
                const auto ui = static_cast<std::size_t>(i);
                const Vec2f pos{readX[i], readY[i]};

                if (health_[ui] == 0)
                {
                    // The fallen keep their place on the field. Skipping them here is what makes
                    // --no-respawn cheaper than the live case rather than merely different.
                    writeX[i] = pos.x;
                    writeY[i] = pos.y;
                    continue;
                }

                const int tx = std::clamp(pos.x.ToInt(), 0, tiles - 1);
                const int ty = std::clamp(pos.y.ToInt(), 0, tiles - 1);
                const Group& group = groups_[groupOf_[ui]];

                // --- Where does this unit want to be? ---
                Vec2f desired;
                const Vec2f toGoal = group.goalPos - pos;
                const std::int64_t goalDistanceSquared = LengthSquaredRaw(toGoal);
                const std::int64_t formationRange =
                    static_cast<std::int64_t>(group.formationRadius.raw) * group.formationRadius.raw;

                if (goalDistanceSquared < formationRange)
                {
                    // Inside the formation envelope the unit steers at its own slot, which is
                    // what turns an arriving mob into a line, a wedge or a box.
                    const Vec2f slot = FormationSlotOffset(
                        group.formation, i - group.firstUnit, group.unitCount,
                        group.formationSpacing);
                    const Vec2f target = group.goalPos + RotateToFacing(slot, group.facing);
                    desired = ScaleToLength(target - pos, kFixedOne);
                }
                else
                {
                    const int sector = ((ty >> sectorShift) * sectorsPerSide) + (tx >> sectorShift);
                    if (group.hasPath && group.nav.HasFieldFor(sector))
                    {
                        const std::int32_t local =
                            ((ty & (sectorSize - 1)) << sectorShift) | (tx & (sectorSize - 1));
                        const std::uint8_t flow = group.nav.FlowAt(sector, local);
                        desired = flow == kFlowArrived ? ScaleToLength(toGoal, kFixedOne)
                                                       : kDirection8[flow];
                    }
                    else
                    {
                        const std::int32_t guide =
                            group.hasPath ? group.nav.GuideTileOf(sector) : -1;
                        if (guide >= 0)
                        {
                            const Vec2f target{
                                Fixed::FromRaw((guide % tiles) * Fixed::kOne + Fixed::kHalf),
                                Fixed::FromRaw((guide / tiles) * Fixed::kOne + Fixed::kHalf)};
                            desired = ScaleToLength(target - pos, kFixedOne);
                        }
                        else
                        {
                            desired = ScaleToLength(toGoal, kFixedOne);
                        }
                    }
                }

                // --- Crowd pressure, straight off the grid the sort just built. ---
                const int cx = tx >> cellShift;
                const int cy = ty >> cellShift;
                const std::int32_t cell = cellOf[i];
                const auto population = [&](int nx, int ny) -> std::int32_t {
                    if (static_cast<unsigned>(nx) >= static_cast<unsigned>(cellsPerSide) ||
                        static_cast<unsigned>(ny) >= static_cast<unsigned>(cellsPerSide))
                        return 0;
                    const std::int32_t at = static_cast<std::int32_t>(ny) * cellsPerSide + nx;
                    return cellStarts[at + 1] - cellStarts[at];
                };
                const std::int32_t here = cellStarts[cell + 1] - cellStarts[cell];
                // One reciprocal, two multiplies: the gradient is normalised by the local
                // crowd so a dense cell pushes no harder than a sparse one, without the two
                // divisions the obvious spelling costs.
                const Fixed inverseDensity =
                    kInverseDensity[static_cast<std::size_t>(
                        std::clamp(here, 0, kInverseDensityEntries - 1))];
                const auto gradientX = static_cast<std::int32_t>(
                    std::clamp(population(cx - 1, cy) - population(cx + 1, cy), -64, 64));
                const auto gradientY = static_cast<std::int32_t>(
                    std::clamp(population(cx, cy - 1) - population(cx, cy + 1), -64, 64));
                Vec2f push{inverseDensity * gradientX * kDensityStrength,
                           inverseDensity * gradientY * kDensityStrength};

                // --- A handful of real neighbours, read as one contiguous run. ---
                //
                // The sample is the unit's own neighbourhood *inside its cell's slice of the
                // sorted array*, centred on where the sort put this unit. Striding across the
                // whole cell instead would sample it more evenly and would cost a modulo per
                // unit and a cache miss per neighbour; this reads one or two cache lines and no
                // divisions. What it gives up is that the partners are cell-mates with nearby
                // unit indices rather than an even spread of the cell, which at fifteen units
                // per cell and six samples is a distinction without a visible difference.
                if (neighbourSamples > 0)
                {
                    const std::int32_t from = cellStarts[cell];
                    const std::int32_t to = cellStarts[cell + 1];
                    if (to - from > 1)
                    {
                        const std::int32_t slot = sortedSlot[i];
                        std::int32_t hi = std::min(to, slot + (neighbourSamples + 1) / 2 + 1);
                        std::int32_t lo = std::max(from, hi - neighbourSamples - 1);
                        hi = std::min(to, lo + neighbourSamples + 1);
                        for (std::int32_t k = lo; k < hi; ++k)
                        {
                            if (k == slot) continue;
                            const Fixed dx = pos.x - sortedPos[k].x;
                            const Fixed dy = pos.y - sortedPos[k].y;
                            const std::int64_t distanceSquared =
                                static_cast<std::int64_t>(dx.raw) * dx.raw +
                                static_cast<std::int64_t>(dy.raw) * dy.raw;
                            const auto bucket =
                                static_cast<std::int32_t>(distanceSquared >> kSeparationShift);
                            if (bucket >= kSeparationBuckets || distanceSquared == 0) continue;
                            const Fixed weight =
                                kSeparationFalloff[static_cast<std::size_t>(bucket)] *
                                kSeparationStrength;
                            const Vec2f away = kDirection8[QuantiseDirection(dx, dy)];
                            push += Vec2f{away.x * weight, away.y * weight};
                        }
                    }
                }

                // --- Integrate. ---
                {
                    const std::int32_t ax = push.x.raw < 0 ? -push.x.raw : push.x.raw;
                    const std::int32_t ay = push.y.raw < 0 ? -push.y.raw : push.y.raw;
                    const std::int32_t hi2 = ax > ay ? ax : ay;
                    const std::int32_t lo2 = ax > ay ? ay : ax;
                    if (hi2 + ((lo2 * 13) >> 5) > kMaxCrowdPush.raw)
                        push = ScaleToLength(push, kMaxCrowdPush);
                }
                const Fixed speed = kSpeedOfKind[kind_[ui] % 3];
                const Vec2f steer = ScaleToLength(
                    Vec2f{desired.x + push.x, desired.y + push.y}, speed);
                Fixed vx = velX_[ui] + (steer.x - velX_[ui]) * kAcceleration;
                Fixed vy = velY_[ui] + (steer.y - velY_[ui]) * kAcceleration;

                Fixed nx = pos.x + vx;
                Fixed ny = pos.y + vy;
                // Axis-separated collision: a unit that walks into a cliff slides along it
                // instead of stopping dead, which is the difference between an army flowing
                // around an obstacle and an army piling up against it.
                const int ntx = nx.ToInt();
                if (ntx != tx && !terrain_.Passable(ntx, ty))
                {
                    nx = pos.x;
                    vx = Fixed::FromRaw(-vx.raw / 4);
                }
                const int nty = ny.ToInt();
                if (nty != ty && !terrain_.Passable(tx, nty))
                {
                    ny = pos.y;
                    vy = Fixed::FromRaw(-vy.raw / 4);
                }

                writeX[i] = FixedClamp(nx, lowBound, highBound);
                writeY[i] = FixedClamp(ny, lowBound, highBound);
                velX_[ui] = vx;
                velY_[ui] = vy;
            }
        });

        current_ = 1 - current_;
    }

    void Simulation::ResolveCombat()
    {
        const std::int32_t count = config_.unitCount;
        const int tiles = terrain_.Size();
        const std::int32_t* cellOf = grid_.CellOfUnit().data();
        const int workers = jobs_->WorkerCount();
        std::vector<std::int64_t> chunkDeaths(static_cast<std::size_t>(workers), 0);

        Fixed* writeX = posX_[current_].data();
        Fixed* writeY = posY_[current_].data();
        const std::uint64_t tickSalt = MixHash(config_.seed ^ (static_cast<std::uint64_t>(tick_) << 32));

        jobs_->ParallelChunks(count, [&](int chunk, std::int32_t begin, std::int32_t end) {
            std::int64_t deaths = 0;
            for (std::int32_t i = begin; i < end; ++i)
            {
                const std::size_t ui = static_cast<std::size_t>(i);
                const int myTeam = team_[ui];
                if (myTeam == kFallenTeam) continue;

                std::int32_t enemies = 0;
                for (int t = 0; t < kMaxTeams; ++t)
                    if (t != myTeam && t != kFallenTeam) enemies += grid_.TeamInCell(cellOf[i], t);
                if (enemies == 0)
                {
                    // Out of contact, a unit recovers slowly. Without this every battle is
                    // terminal and the map empties out well before a benchmark run ends.
                    if (health_[ui] > 0 && health_[ui] < 255) ++health_[ui];
                    continue;
                }

                const std::int32_t damage = std::max<std::int32_t>(1, (enemies * 3) >> 4);
                if (health_[ui] > damage)
                {
                    health_[ui] = static_cast<std::uint8_t>(health_[ui] - damage);
                    continue;
                }

                ++deaths;
                if (!config_.respawn)
                {
                    health_[ui] = 0;
                    team_[ui] = static_cast<std::uint8_t>(kFallenTeam);
                    velX_[ui] = kFixedZero;
                    velY_[ui] = kFixedZero;
                    continue;
                }

                // Reinforcements walk in from the team's own corner. Constant entity count is
                // deliberate: a benchmark whose workload shrinks as it runs measures its own
                // attrition rate, not its throughput.
                const auto& pool = spawnTiles_[static_cast<std::size_t>(myTeam)];
                const std::uint64_t roll = MixHash(tickSalt ^ static_cast<std::uint64_t>(i));
                const std::int32_t tile =
                    pool[static_cast<std::size_t>(roll % pool.size())];
                writeX[i] = Fixed::FromRaw((tile % tiles) * Fixed::kOne +
                                           static_cast<std::int32_t>((roll >> 20) & 0xFFFF));
                writeY[i] = Fixed::FromRaw((tile / tiles) * Fixed::kOne +
                                           static_cast<std::int32_t>((roll >> 40) & 0xFFFF));
                velX_[ui] = kFixedZero;
                velY_[ui] = kFixedZero;
                health_[ui] = 255;
            }
            chunkDeaths[static_cast<std::size_t>(chunk)] = deaths;
        });

        for (const std::int64_t deaths : chunkDeaths) deaths_ += deaths;
    }

    void Simulation::UpdateCentroids()
    {
        const std::int32_t count = config_.unitCount;
        const int workers = jobs_->WorkerCount();
        std::fill(chunkSumX_.begin(), chunkSumX_.end(), 0);
        std::fill(chunkSumY_.begin(), chunkSumY_.end(), 0);
        std::fill(chunkAlive_.begin(), chunkAlive_.end(), 0);

        const Fixed* readX = posX_[current_].data();
        const Fixed* readY = posY_[current_].data();

        jobs_->ParallelChunks(count, [&](int chunk, std::int32_t begin, std::int32_t end) {
            std::int64_t sumX[kMaxTeams] = {};
            std::int64_t sumY[kMaxTeams] = {};
            std::int32_t alive[kMaxTeams] = {};
            for (std::int32_t i = begin; i < end; ++i)
            {
                const int t = team_[static_cast<std::size_t>(i)] & (kMaxTeams - 1);
                sumX[t] += readX[i].raw;
                sumY[t] += readY[i].raw;
                ++alive[t];
            }
            for (int t = 0; t < kMaxTeams; ++t)
            {
                const std::size_t at = static_cast<std::size_t>(chunk) * kMaxTeams + t;
                chunkSumX_[at] = sumX[t];
                chunkSumY_[at] = sumY[t];
                chunkAlive_[at] = alive[t];
            }
        });

        for (int t = 0; t < kMaxTeams; ++t)
        {
            std::int64_t sumX = 0, sumY = 0;
            std::int32_t alive = 0;
            for (int chunk = 0; chunk < workers; ++chunk)
            {
                const std::size_t at = static_cast<std::size_t>(chunk) * kMaxTeams + t;
                sumX += chunkSumX_[at];
                sumY += chunkSumY_[at];
                alive += chunkAlive_[at];
            }
            teamAlive_[static_cast<std::size_t>(t)] = alive;
            teamCentroid_[static_cast<std::size_t>(t)] =
                alive > 0 ? Vec2f{Fixed::FromRaw(static_cast<std::int32_t>(sumX / alive)),
                                  Fixed::FromRaw(static_cast<std::int32_t>(sumY / alive))}
                          : Vec2f{};
        }
    }

    void Simulation::UpdateChecksum()
    {
        const std::int32_t count = config_.unitCount;
        const int workers = jobs_->WorkerCount();
        const Fixed* readX = posX_[current_].data();
        const Fixed* readY = posY_[current_].data();

        jobs_->ParallelChunks(count, [&](int chunk, std::int32_t begin, std::int32_t end) {
            std::uint64_t sum = 0;
            for (std::int32_t i = begin; i < end; ++i)
            {
                const std::size_t ui = static_cast<std::size_t>(i);
                const std::uint64_t position =
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(readX[i].raw)) << 32) |
                    static_cast<std::uint32_t>(readY[i].raw);
                const std::uint64_t status =
                    static_cast<std::uint64_t>(health_[ui]) |
                    (static_cast<std::uint64_t>(team_[ui]) << 8) |
                    (static_cast<std::uint64_t>(kind_[ui]) << 16);
                sum += MixHash(position ^ (static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull));
                sum += MixHash(status ^ (static_cast<std::uint64_t>(i) * 0x2545F4914F6CDD1Dull));
            }
            chunkHash_[static_cast<std::size_t>(chunk)] = sum;
        });

        // Summed, not folded in sequence. A hash built by chaining the per-chunk results would
        // depend on how many chunks there were, so the same run on an eight-core and a
        // sixteen-core machine would report two different numbers and the determinism check
        // would fail for a reason that has nothing to do with the simulation. Addition is
        // associative and commutative, and each term already carries its own unit index, so the
        // total is a function of the state alone -- which is exactly what it claims to be.
        std::uint64_t total = 0;
        for (int chunk = 0; chunk < workers; ++chunk)
            total += chunkHash_[static_cast<std::size_t>(chunk)];
        checksum_ = MixHash(total ^ (static_cast<std::uint64_t>(tick_) << 1));
    }

    void Simulation::Tick()
    {
        System::Diagnostics::Stopwatch whole = System::Diagnostics::Stopwatch::StartNew();
        System::Diagnostics::Stopwatch phase;

        phase.Restart();
        IssueAutoOrders();
        ApplyCommands();
        ServicePathQueries();
        phase.Stop();
        stats_.pathMs = ElapsedMs(phase);

        phase.Restart();
        MoveUnits();
        phase.Stop();
        stats_.moveMs = ElapsedMs(phase);

        phase.Restart();
        grid_.Build(*jobs_, config_.unitCount, posX_[current_].data(), posY_[current_].data(),
                    team_.data());
        phase.Stop();
        stats_.gridMs = ElapsedMs(phase);

        phase.Restart();
        if (config_.combat) ResolveCombat();
        UpdateCentroids();
        phase.Stop();
        stats_.combatMs = ElapsedMs(phase);

        phase.Restart();
        if (config_.fogInterval > 0 &&
            tick_ % static_cast<std::uint32_t>(config_.fogInterval) == 0)
            fog_.Update(*jobs_, grid_, config_.visionRadiusCells);
        phase.Stop();
        stats_.fogMs = ElapsedMs(phase);

        phase.Restart();
        if (config_.checksumInterval > 0 &&
            tick_ % static_cast<std::uint32_t>(config_.checksumInterval) == 0)
            UpdateChecksum();
        phase.Stop();
        stats_.checksumMs = ElapsedMs(phase);

        // Corridors are refreshed as an army advances along them, staggered so at most one
        // group asks per tick. A plan made a hundred ticks ago was made from a centre of mass
        // that has since walked a third of the way to the goal, and the sectors behind it are
        // carrying flow fields nobody is standing in any more.
        constexpr std::uint32_t kCorridorRefreshTicks = 120;
        for (std::size_t g = 0; g < groups_.size(); ++g)
        {
            Group& group = groups_[g];
            if (group.needsRepath) continue;
            if (group.hasPath && group.corridor.steps.size() <= 1) continue;
            if ((tick_ + static_cast<std::uint32_t>(g) * 7) % kCorridorRefreshTicks != 0) continue;
            group.needsRepath = true;
            repathQueue_.push_back(static_cast<std::int32_t>(g));
        }

        ++tick_;
        whole.Stop();
        stats_.totalMs = ElapsedMs(whole);
    }
}
