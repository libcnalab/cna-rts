// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <limits>

namespace CnaRts
{
    /**
     * @brief Q16.16 signed fixed-point scalar: the simulation's only numeric type.
     *
     * The whole point of this project's "deterministic simulation" claim rests here. Floating
     * point would be *reproducible* on one binary and one machine, but the moment a compiler
     * contracts a multiply-add, vectorises a reduction into a different summation order, or a
     * different libm rounds `sqrt` one ulp away, two runs of the same seed diverge -- and in an
     * RTS a one-ulp divergence at tick 40 is a different battle by tick 4000. Integers have no
     * such freedom: `a * b >> 16` is the same value on every conforming implementation.
     *
     * The representable range is +-32767.99998, which bounds the map at 32k world units -- two
     * orders of magnitude above the 2048-unit map the largest benchmark scale uses.
     */
    struct Fixed
    {
        std::int32_t raw = 0;

        static constexpr int          kFractionBits = 16;
        static constexpr std::int32_t kOne          = 1 << kFractionBits;
        static constexpr std::int32_t kHalf         = 1 << (kFractionBits - 1);

        constexpr Fixed() = default;

        /** @brief Wraps an already-scaled integer. Named, because the implicit reading is wrong. */
        static constexpr Fixed FromRaw(std::int32_t value) { Fixed f; f.raw = value; return f; }
        /** @brief Exact conversion of a whole number. */
        static constexpr Fixed FromInt(std::int32_t value) { return FromRaw(value * kOne); }
        /** @brief @p numerator / @p denominator, rounded toward zero. */
        static constexpr Fixed FromRatio(std::int32_t numerator, std::int32_t denominator)
        {
            return FromRaw(static_cast<std::int32_t>(
                (static_cast<std::int64_t>(numerator) * kOne) / denominator));
        }
        /**
         * @brief Converts a double at *setup* time.
         *
         * Deliberately not `constexpr`-only and deliberately not used inside a tick: a literal
         * written in the source is the same double on every machine, so converting it once
         * during configuration is deterministic. Converting a *computed* double would not be.
         */
        static Fixed FromDouble(double value)
        {
            return FromRaw(static_cast<std::int32_t>(value * static_cast<double>(kOne)));
        }

        [[nodiscard]] constexpr std::int32_t ToInt() const { return raw >> kFractionBits; }
        [[nodiscard]] constexpr std::int32_t ToIntRounded() const { return (raw + kHalf) >> kFractionBits; }
        [[nodiscard]] constexpr float ToFloat() const
        {
            return static_cast<float>(raw) / static_cast<float>(kOne);
        }

        constexpr Fixed operator-() const { return FromRaw(-raw); }

        constexpr Fixed& operator+=(Fixed rhs) { raw += rhs.raw; return *this; }
        constexpr Fixed& operator-=(Fixed rhs) { raw -= rhs.raw; return *this; }
        constexpr Fixed& operator*=(Fixed rhs) { *this = *this * rhs; return *this; }
        constexpr Fixed& operator/=(Fixed rhs) { *this = *this / rhs; return *this; }

        friend constexpr Fixed operator+(Fixed a, Fixed b) { return FromRaw(a.raw + b.raw); }
        friend constexpr Fixed operator-(Fixed a, Fixed b) { return FromRaw(a.raw - b.raw); }

        friend constexpr Fixed operator*(Fixed a, Fixed b)
        {
            // The 64-bit intermediate is not an optimisation to be talked out of: two Q16.16
            // values multiply to Q32.32, which does not fit the 32-bit representation until it
            // has been shifted back down.
            return FromRaw(static_cast<std::int32_t>(
                (static_cast<std::int64_t>(a.raw) * static_cast<std::int64_t>(b.raw)) >> kFractionBits));
        }
        friend constexpr Fixed operator*(Fixed a, std::int32_t b) { return FromRaw(a.raw * b); }
        friend constexpr Fixed operator*(std::int32_t a, Fixed b) { return FromRaw(a * b.raw); }

        friend constexpr Fixed operator/(Fixed a, Fixed b)
        {
            if (b.raw == 0) return FromRaw(a.raw >= 0 ? std::numeric_limits<std::int32_t>::max()
                                                      : std::numeric_limits<std::int32_t>::min());
            return FromRaw(static_cast<std::int32_t>(
                (static_cast<std::int64_t>(a.raw) << kFractionBits) / static_cast<std::int64_t>(b.raw)));
        }
        friend constexpr Fixed operator/(Fixed a, std::int32_t b)
        {
            return b == 0 ? a : FromRaw(a.raw / b);
        }

        friend constexpr bool operator==(Fixed a, Fixed b) { return a.raw == b.raw; }
        friend constexpr bool operator!=(Fixed a, Fixed b) { return a.raw != b.raw; }
        friend constexpr bool operator< (Fixed a, Fixed b) { return a.raw <  b.raw; }
        friend constexpr bool operator<=(Fixed a, Fixed b) { return a.raw <= b.raw; }
        friend constexpr bool operator> (Fixed a, Fixed b) { return a.raw >  b.raw; }
        friend constexpr bool operator>=(Fixed a, Fixed b) { return a.raw >= b.raw; }
    };

    constexpr Fixed kFixedZero = Fixed::FromRaw(0);
    constexpr Fixed kFixedOne  = Fixed::FromRaw(Fixed::kOne);

    constexpr Fixed FixedAbs(Fixed v) { return v.raw < 0 ? Fixed::FromRaw(-v.raw) : v; }
    constexpr Fixed FixedMin(Fixed a, Fixed b) { return a.raw < b.raw ? a : b; }
    constexpr Fixed FixedMax(Fixed a, Fixed b) { return a.raw > b.raw ? a : b; }
    constexpr Fixed FixedClamp(Fixed v, Fixed lo, Fixed hi)
    {
        return v.raw < lo.raw ? lo : (v.raw > hi.raw ? hi : v);
    }

    /**
     * @brief Integer square root of a 64-bit value, exact and identical everywhere.
     *
     * Newton's method on integers, seeded from the bit width. `std::sqrt` on a double would give
     * the same answer on nearly every machine and a different one on the few where it does not,
     * which is precisely the class of bug a deterministic simulation exists to rule out.
     */
    constexpr std::uint32_t IntSqrt64(std::uint64_t value)
    {
        if (value == 0) return 0;
        std::uint64_t guess = value;
        std::uint64_t shift = 0;
        while ((guess >> shift) > 1) shift += 2;
        guess = std::uint64_t{1} << ((shift >> 1) + 1);
        while (true)
        {
            const std::uint64_t next = (guess + value / guess) >> 1;
            if (next >= guess) break;
            guess = next;
        }
        return static_cast<std::uint32_t>(guess);
    }

    /** @brief `sqrt` in Q16.16, via the exact integer root of the value scaled by 2^16. */
    inline Fixed FixedSqrt(Fixed value)
    {
        if (value.raw <= 0) return kFixedZero;
        return Fixed::FromRaw(static_cast<std::int32_t>(
            IntSqrt64(static_cast<std::uint64_t>(value.raw) << Fixed::kFractionBits)));
    }

    /** @brief A 2D vector in the simulation's fixed-point space. */
    struct Vec2f
    {
        Fixed x{}, y{};

        constexpr Vec2f() = default;
        constexpr Vec2f(Fixed px, Fixed py) : x(px), y(py) {}

        friend constexpr Vec2f operator+(Vec2f a, Vec2f b) { return {a.x + b.x, a.y + b.y}; }
        friend constexpr Vec2f operator-(Vec2f a, Vec2f b) { return {a.x - b.x, a.y - b.y}; }
        friend constexpr Vec2f operator*(Vec2f a, Fixed s) { return {a.x * s, a.y * s}; }
        constexpr Vec2f& operator+=(Vec2f b) { x += b.x; y += b.y; return *this; }
        constexpr Vec2f& operator-=(Vec2f b) { x -= b.x; y -= b.y; return *this; }
    };

    /** @brief Squared length, widened to 64 bits so a long vector cannot overflow the square. */
    inline std::int64_t LengthSquaredRaw(Vec2f v)
    {
        const std::int64_t x = v.x.raw;
        const std::int64_t y = v.y.raw;
        return x * x + y * y;
    }

    /** @brief Length in Q16.16, exact to the last representable bit. */
    inline Fixed Length(Vec2f v)
    {
        const std::int64_t squared = LengthSquaredRaw(v);
        if (squared == 0) return kFixedZero;
        // squared is Q32.32; its integer root is Q16.16 directly.
        return Fixed::FromRaw(static_cast<std::int32_t>(IntSqrt64(static_cast<std::uint64_t>(squared))));
    }

    /**
     * @brief Unit-length @p v, or (0,0) when @p v is shorter than one raw unit.
     *
     * Returning zero rather than an arbitrary axis matters: a unit that has arrived should stop,
     * not drift north forever because normalising (0,0) had to pick something.
     */
    inline Vec2f Normalized(Vec2f v)
    {
        const Fixed len = Length(v);
        if (len.raw <= 0) return {};
        return {v.x / len, v.y / len};
    }
}
