// Unit tests for the :time partition (Duration / TimePoint / Clock / Stopwatch).
// Wall-clock assertions are lower-bound + generous upper-bound only, so a stalled
// or throttled machine can't make them flaky.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;

using namespace draconic::foundation;

TEST_CASE("time: Duration unit conversions round-trip")
{
    const Duration s = Duration::FromSeconds(2.5);
    CHECK(s.AsSeconds() == doctest::Approx(2.5));
    CHECK(s.AsMilliseconds() == doctest::Approx(2500.0));
    CHECK(s.AsMicroseconds() == doctest::Approx(2'500'000.0));
    CHECK(s.AsNanoseconds() == 2'500'000'000ll);
    CHECK(s.AsSecondsF() == doctest::Approx(2.5f));

    CHECK(Duration::FromMilliseconds(1000.0).AsSeconds() == doctest::Approx(1.0));
    CHECK(Duration::FromMicroseconds(1'000.0).AsMilliseconds() == doctest::Approx(1.0));
    CHECK(Duration::FromNanoseconds(1'000'000).AsMilliseconds() == doctest::Approx(1.0));
}

TEST_CASE("time: Duration zero / negative")
{
    CHECK(Duration{}.IsZero());
    CHECK(Duration::Zero().IsZero());
    CHECK(Duration::Zero().AsNanoseconds() == 0);

    const Duration neg = -Duration::FromSeconds(1.0);
    CHECK(neg.AsSeconds() == doctest::Approx(-1.0));
    CHECK_FALSE(neg.IsZero());
}

TEST_CASE("time: Duration arithmetic")
{
    const Duration a = Duration::FromMilliseconds(100.0);
    const Duration b = Duration::FromMilliseconds(25.0);

    CHECK((a + b).AsMilliseconds() == doctest::Approx(125.0));
    CHECK((a - b).AsMilliseconds() == doctest::Approx(75.0));
    CHECK((a * 2.0).AsMilliseconds() == doctest::Approx(200.0));
    CHECK((a / 4.0).AsMilliseconds() == doctest::Approx(25.0));
    CHECK((a / b) == doctest::Approx(4.0)); // ratio of two durations

    Duration acc = Duration::Zero();
    acc += a;
    acc += b;
    CHECK(acc.AsMilliseconds() == doctest::Approx(125.0));
    acc -= b;
    CHECK(acc.AsMilliseconds() == doctest::Approx(100.0));
}

TEST_CASE("time: Duration comparisons")
{
    const Duration a = Duration::FromMilliseconds(10.0);
    const Duration b = Duration::FromMilliseconds(20.0);
    CHECK(a < b);
    CHECK(b > a);
    CHECK(a <= a);
    CHECK(a >= a);
    CHECK(a == Duration::FromMilliseconds(10.0));
    CHECK(a != b);
}

TEST_CASE("time: Clock advances monotonically")
{
    CHECK(Clock::Frequency() > 0u);

    const TimePoint t0 = Clock::Now();
    SleepMilliseconds(3);
    const TimePoint t1 = Clock::Now();

    CHECK(t1 >= t0);
    const Duration elapsed = t1 - t0;
    // At least ~1ms actually elapsed; well under a second on any sane machine.
    CHECK(elapsed.AsMilliseconds() >= 1.0);
    CHECK(elapsed.AsSeconds() < 5.0);
}

TEST_CASE("time: TimePoint difference sign + Duration shift")
{
    const TimePoint t0 = Clock::Now();
    SleepMilliseconds(2);
    const TimePoint t1 = Clock::Now();

    CHECK((t1 - t0).AsNanoseconds() > 0);
    CHECK((t0 - t1).AsNanoseconds() < 0); // reversed subtraction is negative

    // Shifting a point forward then measuring the gap recovers ~the duration.
    const Duration d = Duration::FromMilliseconds(50.0);
    const Duration recovered = (t0 + d) - t0;
    CHECK(recovered.AsMilliseconds() == doctest::Approx(50.0).epsilon(0.01));
    CHECK(((t0 - d) - t0).AsMilliseconds() == doctest::Approx(-50.0).epsilon(0.01));
}

TEST_CASE("time: Stopwatch measures elapsed time")
{
    Stopwatch sw = Stopwatch::StartNew();
    CHECK(sw.IsRunning());
    SleepMilliseconds(3);
    const Duration e1 = sw.Elapsed();
    CHECK(e1.AsMilliseconds() >= 1.0);

    // A running stopwatch keeps accruing.
    SleepMilliseconds(3);
    CHECK(sw.Elapsed() >= e1);
}

TEST_CASE("time: Stopwatch Stop freezes, Start resumes (additive)")
{
    Stopwatch sw = Stopwatch::StartNew();
    SleepMilliseconds(3);
    sw.Stop();
    CHECK_FALSE(sw.IsRunning());

    const Duration frozen = sw.Elapsed();
    SleepMilliseconds(3);
    // While stopped, Elapsed does not change.
    CHECK(sw.Elapsed().AsNanoseconds() == frozen.AsNanoseconds());

    // Resuming accumulates on top of the frozen value.
    sw.Start();
    SleepMilliseconds(3);
    CHECK(sw.Elapsed() > frozen);
}

TEST_CASE("time: Stopwatch Reset and Restart")
{
    Stopwatch sw = Stopwatch::StartNew();
    SleepMilliseconds(2);

    sw.Reset();
    CHECK_FALSE(sw.IsRunning());
    CHECK(sw.Elapsed().IsZero());

    sw.Restart();
    CHECK(sw.IsRunning());
    SleepMilliseconds(2);
    CHECK(sw.Elapsed().AsMilliseconds() >= 1.0);

    // Restart on a running watch throws away prior accumulation.
    const Duration before = sw.Elapsed();
    sw.Restart();
    CHECK(sw.Elapsed() < before);
}

TEST_CASE("time: default-constructed Stopwatch is stopped at zero")
{
    Stopwatch sw;
    CHECK_FALSE(sw.IsRunning());
    CHECK(sw.Elapsed().IsZero());
    SleepMilliseconds(2);
    CHECK(sw.Elapsed().IsZero()); // never started -> still zero
}
