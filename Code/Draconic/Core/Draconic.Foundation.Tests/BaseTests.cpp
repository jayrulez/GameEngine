#include <doctest/doctest.h>

#include <cstring>

#include "Draconic.Foundation/Debug/Assert.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;

using namespace draconic::foundation;

TEST_CASE("base: fundamental type widths")
{
    CHECK(sizeof(i8) == 1);
    CHECK(sizeof(i16) == 2);
    CHECK(sizeof(i32) == 4);
    CHECK(sizeof(i64) == 8);
    CHECK(sizeof(u8) == 1);
    CHECK(sizeof(u16) == 2);
    CHECK(sizeof(u32) == 4);
    CHECK(sizeof(u64) == 8);
    CHECK(sizeof(f32) == 4);
    CHECK(sizeof(f64) == 8);
    CHECK(sizeof(widechar) == 2); // UTF-16 wide unit
    CHECK(sizeof(utf8char) == 1);
}

TEST_CASE("base: Min / Max / Clamp")
{
    CHECK(Min(3, 5) == 3);
    CHECK(Max(3, 5) == 5);
    CHECK(Clamp(10, 0, 5) == 5);
    CHECK(Clamp(-2, 0, 5) == 0);
    CHECK(Clamp(3, 0, 5) == 3);

    // constexpr-usable
    static_assert(Min(1, 2) == 1);
    static_assert(Clamp(7, 0, 4) == 4);
}

TEST_CASE("base: ArrayCount")
{
    int values[4]{};
    CHECK(ArrayCount(values) == 4u);
    static_assert(ArrayCount(values) == 4u);
}

TEST_CASE("base: Swap")
{
    int a = 1;
    int b = 2;
    Swap(a, b);
    CHECK(a == 2);
    CHECK(b == 1);
}

TEST_CASE("base: Status")
{
    Status ok;
    CHECK(ok.IsOk());
    CHECK(static_cast<bool>(ok));
    CHECK(ok.Code() == ErrorCode::Ok);

    Status bad = ErrorCode::NotFound;
    CHECK_FALSE(bad.IsOk());
    CHECK_FALSE(static_cast<bool>(bad));
    CHECK(bad.Code() == ErrorCode::NotFound);

    CHECK(ok == Status{});
    CHECK(bad == Status{ErrorCode::NotFound});
}

TEST_CASE("base: Result value case")
{
    Result<int> r = 42;
    REQUIRE(r.HasValue());
    CHECK(static_cast<bool>(r));
    CHECK(r.Value() == 42);
    CHECK(r.ValueOr(-1) == 42);
}

TEST_CASE("base: Result error case")
{
    Result<int> r = Err(ErrorCode::OutOfRange);
    CHECK_FALSE(r.HasValue());
    CHECK_FALSE(static_cast<bool>(r));
    CHECK(r.Error() == ErrorCode::OutOfRange);
    CHECK(r.ValueOr(-1) == -1);
}

TEST_CASE("base: Result manages a non-trivial payload")
{
    struct Counter
    {
        static int& Live()
        {
            static int n = 0;
            return n;
        }
        Counter() { ++Live(); }
        Counter(const Counter&) { ++Live(); }
        Counter(Counter&&) { ++Live(); }
        ~Counter() { --Live(); }
    };

    CHECK(Counter::Live() == 0);
    {
        Result<Counter> r{Counter{}};
        CHECK(r.HasValue());
        CHECK(Counter::Live() == 1);

        Result<Counter> e = Err(ErrorCode::Internal);
        CHECK_FALSE(e.HasValue());
        CHECK(Counter::Live() == 1); // error case constructs no Counter
    }
    CHECK(Counter::Live() == 0); // all destroyed
}

TEST_CASE("base: Optional")
{
    Optional<int> empty;
    CHECK_FALSE(empty.HasValue());
    CHECK_FALSE(static_cast<bool>(empty));
    CHECK(empty.ValueOr(-1) == -1);

    Optional<int> some = 7;
    REQUIRE(some.HasValue());
    CHECK(*some == 7);
    CHECK(some.ValueOr(-1) == 7);

    some = NullOpt;
    CHECK_FALSE(some.HasValue());

    some.Emplace(99);
    CHECK(some.Value() == 99);
    some.Reset();
    CHECK_FALSE(some.HasValue());
}

TEST_CASE("base: Optional equality")
{
    Optional<int> a, b;
    CHECK(a == b); // both empty
    a = 5;
    CHECK(a != b); // engaged vs empty
    b = 5;
    CHECK(a == b); // equal values
    b = 6;
    CHECK(a != b); // differing values
    b.Reset();
    CHECK(a != b);
}

TEST_CASE("base: Optional manages non-trivial payload lifetimes")
{
    struct Tracked
    {
        static int& Live()
        {
            static int n = 0;
            return n;
        }
        int v;
        explicit Tracked(int x) : v(x) { ++Live(); }
        Tracked(const Tracked& o) : v(o.v) { ++Live(); }
        Tracked(Tracked&& o) noexcept : v(o.v) { ++Live(); }
        ~Tracked() { --Live(); }
    };

    Tracked::Live() = 0;
    {
        Optional<Tracked> opt{Tracked{5}};
        CHECK(Tracked::Live() == 1);
        Optional<Tracked> copy = opt;
        CHECK(Tracked::Live() == 2);
        CHECK(copy->v == 5);
        opt.Reset();
        CHECK(Tracked::Live() == 1);
    }
    CHECK(Tracked::Live() == 0);
}

// --- Base: byte order ------------------------------------------------------

TEST_CASE("base: byte order helpers")
{
    CHECK(kIsLittleEndian);
    CHECK(ByteSwap<u16>(0x1234) == 0x3412);
    CHECK(ByteSwap<u32>(0x11223344u) == 0x44332211u);

    // Native<->little is a no-op here; native<->big swaps and round-trips.
    CHECK(NativeToLittle<u32>(0x01020304u) == 0x01020304u);
    CHECK(BigToNative(NativeToBig<u32>(0xDEADBEEFu)) == 0xDEADBEEFu);
}
