// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace CnaRts
{
    /**
     * @brief A splitmix64-seeded xoshiro256++ generator: the simulation's only source of chance.
     *
     * `std::mt19937` would be reproducible too, but `std::uniform_int_distribution` is not --
     * the standard fixes the engine's bit stream and leaves the distribution's mapping to the
     * implementation, so the same engine and the same seed produce different draws on libstdc++
     * and libc++. Everything here is spelled out arithmetically for that reason.
     */
    class Rng
    {
    public:
        explicit Rng(std::uint64_t seed = 0x9E3779B97F4A7C15ull) { Seed(seed); }

        void Seed(std::uint64_t seed)
        {
            // splitmix64: turns one seed word into four well-mixed state words, so seeds 1 and 2
            // do not produce correlated streams.
            for (std::uint64_t& word : state_)
            {
                seed += 0x9E3779B97F4A7C15ull;
                std::uint64_t z = seed;
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                word = z ^ (z >> 31);
            }
        }

        std::uint64_t NextUInt64()
        {
            const std::uint64_t result = Rotl(state_[0] + state_[3], 23) + state_[0];
            const std::uint64_t t = state_[1] << 17;
            state_[2] ^= state_[0];
            state_[3] ^= state_[1];
            state_[1] ^= state_[2];
            state_[0] ^= state_[3];
            state_[2] ^= t;
            state_[3] = Rotl(state_[3], 45);
            return result;
        }

        std::uint32_t NextUInt32() { return static_cast<std::uint32_t>(NextUInt64() >> 32); }

        /** @brief Uniform in [0, @p bound), by the multiply-shift method (no modulo bias worth
         *  worrying about at these bounds, and no implementation-defined distribution object). */
        std::uint32_t NextBelow(std::uint32_t bound)
        {
            if (bound == 0) return 0;
            return static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(NextUInt32()) * bound) >> 32);
        }

        /** @brief Uniform in [@p lo, @p hi]. */
        std::int32_t NextRange(std::int32_t lo, std::int32_t hi)
        {
            if (hi <= lo) return lo;
            return lo + static_cast<std::int32_t>(NextBelow(static_cast<std::uint32_t>(hi - lo + 1)));
        }

    private:
        static constexpr std::uint64_t Rotl(std::uint64_t x, int k)
        {
            return (x << k) | (x >> (64 - k));
        }

        std::uint64_t state_[4]{};
    };

    /**
     * @brief FNV-1a over the raw simulation state: the number that proves determinism.
     *
     * Accumulated rather than returned whole so a caller can fold in several arrays -- and
     * folded in a fixed order, because a hash whose result depends on which thread finished
     * first would report exactly the property it is meant to detect.
     */
    class Checksum
    {
    public:
        void Feed(std::uint32_t word)
        {
            hash_ ^= word;
            hash_ *= 0x100000001B3ull;
        }
        void Feed(std::int32_t word) { Feed(static_cast<std::uint32_t>(word)); }

        [[nodiscard]] std::uint64_t Value() const { return hash_; }
        void Reset() { hash_ = 0xCBF29CE484222325ull; }

        /** @brief Combines an independently computed partial hash, order-sensitively. */
        void Combine(std::uint64_t partial)
        {
            Feed(static_cast<std::uint32_t>(partial));
            Feed(static_cast<std::uint32_t>(partial >> 32));
        }

    private:
        std::uint64_t hash_ = 0xCBF29CE484222325ull;
    };
}
