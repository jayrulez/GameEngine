module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:random;

import :base;

export namespace draconic::foundation
{
    // =======================================================================
    // Random - PCG32. Deterministic and seedable (good for replays/tests).
    // =======================================================================
    class Random
    {
    public:
        explicit Random(u64 seed = 0x853c49e6748fea9bull,
                        u64 sequence = 0xda3e39cb94b95bdbull) noexcept
        {
            m_state = 0;
            m_inc = (sequence << 1u) | 1u;
            NextU32();
            m_state += seed;
            NextU32();
        }

        u32 NextU32() noexcept
        {
            const u64 old = m_state;
            m_state = old * 6364136223846793005ull + m_inc;
            const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
            const u32 rot = static_cast<u32>(old >> 59u);
            return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
        }

        [[nodiscard]] u64 NextU64() noexcept
        {
            const u64 hi = NextU32();
            const u64 lo = NextU32();
            return (hi << 32u) | lo;
        }

        // Uniform float in [0, 1).
        [[nodiscard]] f32 NextFloat() noexcept
        {
            return static_cast<f32>(NextU32() >> 8u) * (1.0f / 16777216.0f);
        }

        [[nodiscard]] f32 NextFloat(f32 min, f32 max) noexcept
        {
            return min + NextFloat() * (max - min);
        }

        // Uniform integer in [min, max] inclusive.
        [[nodiscard]] i32 NextInt(i32 min, i32 max) noexcept
        {
            const u32 range = static_cast<u32>(max - min) + 1u;
            return min + static_cast<i32>(NextU32() % range);
        }

        [[nodiscard]] bool NextBool() noexcept { return (NextU32() & 1u) != 0u; }

    private:
        u64 m_state;
        u64 m_inc;
    };
}
