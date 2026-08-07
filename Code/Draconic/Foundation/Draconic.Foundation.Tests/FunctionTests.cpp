#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;

using namespace draconic::foundation;

namespace
{
    int FreeAdd(int a, int b) { return a + b; }

    // A large-capture callable that must spill to the allocator (exceeds SBO).
    struct Big
    {
        u64 pad[16] = {};
        int base = 100;
        int operator()(int x) const { return base + x + static_cast<int>(pad[0]); }
    };
}

TEST_CASE("function: empty state")
{
    Function<int(int)> f;
    CHECK_FALSE(static_cast<bool>(f));
    f = nullptr;
    CHECK_FALSE(static_cast<bool>(f));
}

TEST_CASE("function: holds a function pointer")
{
    Function<int(int, int)> f = &FreeAdd;
    REQUIRE(static_cast<bool>(f));
    CHECK(f(2, 3) == 5);
}

TEST_CASE("function: holds a captureless and a capturing lambda (SBO)")
{
    Function<int(int)> square = [](int x) { return x * x; };
    CHECK(square(5) == 25);

    int captured = 10;
    Function<int(int)> add = [captured](int x) { return x + captured; };
    CHECK(add(7) == 17);
}

TEST_CASE("function: large captures fall back to the allocator")
{
    Function<int(int)> f = Big{};
    REQUIRE(static_cast<bool>(f));
    CHECK(f(5) == 105);
}

TEST_CASE("function: move transfers ownership (inline and heap)")
{
    // Inline (SBO) target.
    Function<int(int)> a = [](int x) { return x + 1; };
    Function<int(int)> b = Move(a);
    CHECK_FALSE(static_cast<bool>(a));
    REQUIRE(static_cast<bool>(b));
    CHECK(b(1) == 2);

    // Heap target.
    Function<int(int)> c = Big{};
    Function<int(int)> d = Move(c);
    CHECK_FALSE(static_cast<bool>(c));
    REQUIRE(static_cast<bool>(d));
    CHECK(d(5) == 105);

    // Move-assign over an existing target.
    d = Move(b);
    CHECK(d(1) == 2);
}

TEST_CASE("function: void return and reset")
{
    int sink = 0;
    Function<void(int)> f = [&sink](int x) { sink += x; };
    f(3);
    f(4);
    CHECK(sink == 7);
    f.Reset();
    CHECK_FALSE(static_cast<bool>(f));
}

TEST_CASE("function: moves a move-only captured value")
{
    UniquePtr<int> owned = MakeUnique<int>(DefaultAllocator(), 42);
    Function<int()> f = [captured = Move(owned)]() { return *captured; };
    CHECK(f() == 42);
}
