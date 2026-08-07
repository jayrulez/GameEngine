// Draconic Foundation - :time partition
//
// Unit-safe time types built on the platform tick counter (:system). Callers work
// with Duration / TimePoint / Stopwatch instead of juggling raw ticks + frequency:
//
//   const TimePoint start = Clock::Now();
//   ... work ...
//   const Duration dt = Clock::Now() - start;   // f64 dt.AsMilliseconds(), etc.
//
//   Stopwatch sw = Stopwatch::StartNew();
//   ... work ...
//   log(sw.Elapsed().AsMilliseconds());
//
// Duration stores signed integer nanoseconds (exact, chrono-like). TimePoint is an
// opaque monotonic tick count - differences yield a Duration; it is NOT a wall-clock
// date. All conversions are overflow-safe for realistic timer frequencies (<= ~1e9 Hz).

module;
#include "Draconic.Foundation/Prelude.h"
#include <compare>

export module draconic.foundation:time;

import :base;
import :system; // GetTicks, GetTickFrequency

namespace draconic::foundation::detail
{
    // Overflow-safe tick<->nanosecond conversion. Splitting into whole + remainder
    // keeps the intermediate products within u64 for any frequency up to ~1e9 Hz
    // (the platform counters we target: QPC ~1e7, Linux CLOCK_MONOTONIC = 1e9).
    [[nodiscard]] inline i64 TicksToNanos(u64 ticks, u64 freq) noexcept
    {
        if (freq == 0u)
        {
            return 0;
        }
        const u64 whole = ticks / freq;
        const u64 rem = ticks % freq;
        return static_cast<i64>(whole) * 1'000'000'000ll +
               static_cast<i64>((rem * 1'000'000'000ull) / freq);
    }

    [[nodiscard]] inline u64 NanosToTicks(i64 nanos, u64 freq) noexcept
    {
        if (nanos <= 0)
        {
            return 0u;
        }
        const u64 n = static_cast<u64>(nanos);
        const u64 whole = n / 1'000'000'000ull;
        const u64 rem = n % 1'000'000'000ull;
        return whole * freq + (rem * freq) / 1'000'000'000ull;
    }
}

export namespace draconic::foundation
{
    // A signed span of time, stored as integer nanoseconds. Construct via From*;
    // read via As*. Arithmetic and comparison behave like a plain number.
    class Duration
    {
    public:
        constexpr Duration() noexcept = default;

        [[nodiscard]] static constexpr Duration FromNanoseconds(i64 ns) noexcept
        {
            return Duration(ns);
        }
        [[nodiscard]] static constexpr Duration FromMicroseconds(f64 us) noexcept
        {
            return Duration(static_cast<i64>(us * 1'000.0));
        }
        [[nodiscard]] static constexpr Duration FromMilliseconds(f64 ms) noexcept
        {
            return Duration(static_cast<i64>(ms * 1'000'000.0));
        }
        [[nodiscard]] static constexpr Duration FromSeconds(f64 s) noexcept
        {
            return Duration(static_cast<i64>(s * 1'000'000'000.0));
        }
        [[nodiscard]] static constexpr Duration Zero() noexcept { return Duration(0); }

        [[nodiscard]] constexpr i64 AsNanoseconds() const noexcept { return m_nanos; }
        [[nodiscard]] constexpr f64 AsMicroseconds() const noexcept
        {
            return static_cast<f64>(m_nanos) / 1'000.0;
        }
        [[nodiscard]] constexpr f64 AsMilliseconds() const noexcept
        {
            return static_cast<f64>(m_nanos) / 1'000'000.0;
        }
        [[nodiscard]] constexpr f64 AsSeconds() const noexcept
        {
            return static_cast<f64>(m_nanos) / 1'000'000'000.0;
        }
        // Convenience for the common game-loop f32 delta-time path.
        [[nodiscard]] constexpr f32 AsSecondsF() const noexcept
        {
            return static_cast<f32>(AsSeconds());
        }

        [[nodiscard]] constexpr bool IsZero() const noexcept { return m_nanos == 0; }

        constexpr Duration operator+(Duration o) const noexcept
        {
            return Duration(m_nanos + o.m_nanos);
        }
        constexpr Duration operator-(Duration o) const noexcept
        {
            return Duration(m_nanos - o.m_nanos);
        }
        constexpr Duration operator-() const noexcept { return Duration(-m_nanos); }
        constexpr Duration operator*(f64 scale) const noexcept
        {
            return Duration(static_cast<i64>(static_cast<f64>(m_nanos) * scale));
        }
        constexpr Duration operator/(f64 divisor) const noexcept
        {
            return Duration(static_cast<i64>(static_cast<f64>(m_nanos) / divisor));
        }
        // Ratio of two spans (e.g. how many frames fit in a second).
        constexpr f64 operator/(Duration o) const noexcept
        {
            return static_cast<f64>(m_nanos) / static_cast<f64>(o.m_nanos);
        }

        constexpr Duration& operator+=(Duration o) noexcept
        {
            m_nanos += o.m_nanos;
            return *this;
        }
        constexpr Duration& operator-=(Duration o) noexcept
        {
            m_nanos -= o.m_nanos;
            return *this;
        }

        constexpr bool operator==(Duration o) const noexcept { return m_nanos == o.m_nanos; }
        constexpr std::strong_ordering operator<=>(Duration o) const noexcept
        {
            return m_nanos <=> o.m_nanos;
        }

    private:
        explicit constexpr Duration(i64 ns) noexcept : m_nanos(ns) {}
        i64 m_nanos = 0;
    };

    // An opaque point on the monotonic high-resolution clock. Subtract two points
    // to get a Duration; add/subtract a Duration to shift. Not a wall-clock date.
    class TimePoint
    {
    public:
        constexpr TimePoint() noexcept = default;

        [[nodiscard]] static constexpr TimePoint FromTicks(u64 ticks) noexcept
        {
            return TimePoint(ticks);
        }
        [[nodiscard]] constexpr u64 Ticks() const noexcept { return m_ticks; }

        // Elapsed span from `earlier` to `*this` (negative if `earlier` is later).
        [[nodiscard]] Duration operator-(TimePoint earlier) const noexcept
        {
            const u64 freq = GetTickFrequency();
            const bool negative = m_ticks < earlier.m_ticks;
            const u64 delta = negative ? (earlier.m_ticks - m_ticks) : (m_ticks - earlier.m_ticks);
            const i64 nanos = detail::TicksToNanos(delta, freq);
            return Duration::FromNanoseconds(negative ? -nanos : nanos);
        }

        [[nodiscard]] TimePoint operator+(Duration d) const noexcept
        {
            const u64 freq = GetTickFrequency();
            const i64 ns = d.AsNanoseconds();
            return ns >= 0 ? TimePoint(m_ticks + detail::NanosToTicks(ns, freq))
                           : TimePoint(m_ticks - detail::NanosToTicks(-ns, freq));
        }
        [[nodiscard]] TimePoint operator-(Duration d) const noexcept { return *this + (-d); }

        constexpr bool operator==(TimePoint o) const noexcept { return m_ticks == o.m_ticks; }
        constexpr std::strong_ordering operator<=>(TimePoint o) const noexcept
        {
            return m_ticks <=> o.m_ticks;
        }

    private:
        explicit constexpr TimePoint(u64 ticks) noexcept : m_ticks(ticks) {}
        u64 m_ticks = 0;
    };

    // The engine's monotonic high-resolution clock (built on the platform tick counter).
    namespace Clock
    {
        [[nodiscard]] inline TimePoint Now() noexcept { return TimePoint::FromTicks(GetTicks()); }
        [[nodiscard]] inline u64 Frequency() noexcept { return GetTickFrequency(); }
    }

    // Accumulating elapsed-time timer. Start/Stop are additive (elapsed accrues across
    // pause/resume); Reset zeroes and stops; Restart zeroes and starts. Default-constructed
    // stopped at zero; use StartNew() to begin immediately.
    class Stopwatch
    {
    public:
        Stopwatch() noexcept = default;

        [[nodiscard]] static Stopwatch StartNew() noexcept
        {
            Stopwatch sw;
            sw.Start();
            return sw;
        }

        void Start() noexcept
        {
            if (!m_running)
            {
                m_running = true;
                m_startedAt = Clock::Now();
            }
        }
        void Stop() noexcept
        {
            if (m_running)
            {
                m_accumulated += (Clock::Now() - m_startedAt);
                m_running = false;
            }
        }
        void Reset() noexcept
        {
            m_running = false;
            m_accumulated = Duration::Zero();
        }
        void Restart() noexcept
        {
            m_accumulated = Duration::Zero();
            m_running = true;
            m_startedAt = Clock::Now();
        }

        [[nodiscard]] bool IsRunning() const noexcept { return m_running; }
        [[nodiscard]] Duration Elapsed() const noexcept
        {
            return m_running ? m_accumulated + (Clock::Now() - m_startedAt) : m_accumulated;
        }

    private:
        TimePoint m_startedAt{};
        Duration m_accumulated{};
        bool m_running = false;
    };
}
