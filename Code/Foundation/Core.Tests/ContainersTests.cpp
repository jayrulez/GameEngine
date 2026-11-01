// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include <cstring>

#include "Core/Debug/Assert.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;

using namespace foundation::core;

// --- Containers: Span ------------------------------------------------------

TEST_CASE("containers: Span views contiguous memory")
{
    int values[5] = {10, 20, 30, 40, 50};
    Span<int> s = values;

    CHECK(s.Size() == 5u);
    CHECK_FALSE(s.IsEmpty());
    CHECK(s[0] == 10);
    CHECK(s.Front() == 10);
    CHECK(s.Back() == 50);

    Span<int> mid = s.SubSpan(1, 3);
    CHECK(mid.Size() == 3u);
    CHECK(mid[0] == 20);

    int sum = 0;
    for (int v : s)
    {
        sum += v;
    }
    CHECK(sum == 150);
}

// --- Containers: Array -----------------------------------------------------

TEST_CASE("containers: Array push/access/grow")
{
    Array<int> a;
    CHECK(a.IsEmpty());

    for (int i = 0; i < 100; ++i)
    {
        a.PushBack(i);
    }
    CHECK(a.Size() == 100u);
    CHECK(a.Capacity() >= 100u);
    CHECK(a.Front() == 0);
    CHECK(a.Back() == 99);
    CHECK(a[50] == 50);

    int sum = 0;
    for (int v : a)
    {
        sum += v;
    }
    CHECK(sum == 4950);
}

TEST_CASE("containers: Array emplace, pop, remove")
{
    Array<int> a;
    a.EmplaceBack(1);
    a.EmplaceBack(2);
    a.EmplaceBack(3);
    a.EmplaceBack(4);

    a.RemoveAt(1); // -> {1, 3, 4}
    CHECK(a.Size() == 3u);
    CHECK(a[0] == 1);
    CHECK(a[1] == 3);
    CHECK(a[2] == 4);

    a.RemoveAtSwap(0); // -> {4, 3}
    CHECK(a.Size() == 2u);
    CHECK(a[0] == 4);
    CHECK(a[1] == 3);

    a.PopBack(); // -> {4}
    CHECK(a.Size() == 1u);
    CHECK(a.Back() == 4);
}

TEST_CASE("containers: Array insert")
{
    Array<int> a;
    a.PushBack(1);
    a.PushBack(4);

    a.Insert(1, 2); // middle -> {1, 2, 4}
    CHECK(a.Size() == 3u);
    CHECK(a[0] == 1);
    CHECK(a[1] == 2);
    CHECK(a[2] == 4);

    a.Insert(0, 0); // front  -> {0, 1, 2, 4}
    CHECK(a[0] == 0);
    CHECK(a[1] == 1);

    a.Insert(a.Size(), 5); // end (append) -> {0, 1, 2, 4, 5}
    CHECK(a.Size() == 5u);
    CHECK(a.Back() == 5);
    CHECK(a[3] == 4);
}

// Past one insertion run the sort merges: check order and stability at sizes around the run and
// merge-width boundaries, with many equal keys.
TEST_CASE("containers: Array sort orders and keeps equal keys in order at merge sizes")
{
    const usize sizes[] = {31, 32, 33, 64, 65, 100, 1000, 4097};
    for (usize n : sizes)
    {
        Array<u32> values; // key * 10000 + its original position
        u32 seed = 12345u;
        for (usize i = 0; i < n; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            values.PushBack((seed >> 16) % 17u * 10000u + static_cast<u32>(i));
        }
        values.Sort([](u32 a, u32 b) { return a / 10000u < b / 10000u; }); // by key only
        bool sortedAndStable = true;
        for (usize i = 1; i < n; ++i)
        {
            const u32 prevKey = values[i - 1] / 10000u;
            const u32 key = values[i] / 10000u;
            if (prevKey > key || (prevKey == key && values[i - 1] % 10000u > values[i] % 10000u))
                sortedAndStable = false;
        }
        CHECK(values.Size() == n);
        CHECK(sortedAndStable);
    }
}

namespace
{
    // An element whose operator& does not answer its address, as COM's ComPtr returns a reference
    // wrapper: the containers must never take an element's address with &.
    struct Wrapped
    {
        i32 value = 0;
        struct Ref
        {
        };
        Ref operator&() const { return Ref{}; }
    };
}

TEST_CASE("containers: an element that overloads operator& is stored, grown, moved, found and removed")
{
    Array<Wrapped> items;
    for (i32 i = 0; i < 40; ++i) // past the first capacities, so it grows
        items.PushBack(Wrapped{i});
    items.EmplaceBack(Wrapped{40});
    items.Insert(0, Wrapped{-1});
    REQUIRE(items.Size() == 42u);
    CHECK(items[0].value == -1);
    CHECK(items[41].value == 40);
    items.RemoveAt(0);
    items.PopBack();
    items.Sort([](const Wrapped& a, const Wrapped& b) { return a.value > b.value; });
    CHECK(items[0].value == 39);
    Array<Wrapped> copy = items;
    CHECK(copy.Size() == 40u);
    items.Resize(3);
    CHECK(items.Size() == 3u);
    items.Clear();
    CHECK(items.IsEmpty());

    HashMap<i32, Wrapped> map;
    map.InsertOrAssign(7, Wrapped{70});
    const Wrapped* found = map.Find(7);
    REQUIRE(found != nullptr);
    CHECK(found->value == 70);
    CHECK(map.Find(8) == nullptr);

    RingBuffer<Wrapped> ring(4);
    (void)ring.PushBack(Wrapped{1});
    (void)ring.PushBack(Wrapped{2});
    CHECK(ring.Front().value == 1);
    Wrapped out;
    CHECK(ring.PopFront(out));
    CHECK(out.value == 1);
    CHECK(ring.Front().value == 2);
}

TEST_CASE("containers: Array sort through a kept buffer allocates only while the buffer grows")
{
    TrackingAllocator tracking(DefaultAllocator());
    Array<i32> values(tracking);
    Array<i32> merged(tracking);
    const auto fill = [&values](u32 seed)
    {
        values.Clear();
        for (i32 i = 0; i < 500; ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            values.PushBack(static_cast<i32>(seed >> 20));
        }
    };
    const auto ordered = [&values]()
    {
        for (usize i = 1; i < values.Size(); ++i)
            if (values[i] < values[i - 1])
                return false;
        return true;
    };
    fill(7u);
    values.Sort([](i32 a, i32 b) { return a < b; }, merged);
    CHECK(ordered());
    const usize capacity = merged.Capacity();
    CHECK(capacity >= values.Size());
    // A second sort of the same size reuses the buffer: nothing is allocated.
    fill(99u);
    const u64 before = tracking.TotalAllocations();
    values.Sort([](i32 a, i32 b) { return a < b; }, merged);
    CHECK(ordered());
    CHECK(tracking.TotalAllocations() == before);
    CHECK(merged.Capacity() == capacity);
}

TEST_CASE("containers: Array sort")
{
    Array<int> a;
    a.PushBack(3);
    a.PushBack(1);
    a.PushBack(4);
    a.PushBack(1);
    a.PushBack(5);
    a.PushBack(9);
    a.PushBack(2);
    a.Sort([](int x, int y) { return x < y; }); // ascending
    CHECK(a[0] == 1);
    CHECK(a[1] == 1);
    CHECK(a[2] == 2);
    CHECK(a[3] == 3);
    CHECK(a[6] == 9);

    a.Sort([](int x, int y) { return x > y; }); // descending
    CHECK(a[0] == 9);
    CHECK(a.Back() == 1);

    // Stability: equal keys keep insertion order (pair by first, stable on second).
    Array<int> pairs; // encode (key*10 + tag)
    pairs.PushBack(11);
    pairs.PushBack(12);
    pairs.PushBack(21);
    pairs.PushBack(13);
    pairs.PushBack(22);
    pairs.Sort([](int x, int y) { return (x / 10) < (y / 10); });
    CHECK(pairs[0] == 11);
    CHECK(pairs[1] == 12);
    CHECK(pairs[2] == 13);
    CHECK(pairs[3] == 21);
    CHECK(pairs[4] == 22);
}

TEST_CASE("containers: Array resize and clear")
{
    Array<int> a;
    a.Resize(4); // default-constructed ints (0)
    CHECK(a.Size() == 4u);
    CHECK(a[0] == 0);
    CHECK(a[3] == 0);

    a[2] = 99;
    a.Resize(2); // truncate
    CHECK(a.Size() == 2u);

    const usize capBefore = a.Capacity();
    a.Clear();
    CHECK(a.IsEmpty());
    CHECK(a.Capacity() == capBefore); // clear keeps capacity

    // A first Resize from empty is exact; a growing Resize reserves geometrically, so a buffer
    // grown step by step (a mip chain built level by level) does not reallocate at every step,
    // and the relocation keeps every trivially copyable element.
    Array<u8> bytes;
    bytes.Resize(1000);
    CHECK(bytes.Capacity() == 1000u);
    for (usize i = 0; i < 1000; ++i)
    {
        bytes[i] = static_cast<u8>(i);
    }
    bytes.Resize(1001);
    CHECK(bytes.Capacity() >= 1500u); // 1.5x, not 1001
    const usize grownCapacity = bytes.Capacity();
    bytes.Resize(1400);
    CHECK(bytes.Capacity() == grownCapacity); // within the reserve: no reallocation
    CHECK(bytes[999] == static_cast<u8>(999)); // relocated intact...
    CHECK(bytes[1000] == 0);                   // ...and the new tail value-initialized
    CHECK(bytes[1399] == 0);
}

TEST_CASE("containers: Array manages non-trivial element lifetimes")
{
    struct Item
    {
        static int& Live()
        {
            static int n = 0;
            return n;
        }
        int value;
        explicit Item(int v) : value(v) { ++Live(); }
        Item(const Item& o) : value(o.value) { ++Live(); }
        Item(Item&& o) noexcept : value(o.value) { ++Live(); }
        Item& operator=(const Item&) = default;
        Item& operator=(Item&&) = default;
        ~Item() { --Live(); }
    };

    Item::Live() = 0;
    {
        Array<Item> a;
        for (int i = 0; i < 20; ++i)
        {
            a.EmplaceBack(i);
        } // forces reallocations
        CHECK(Item::Live() == 20);

        Array<Item> copy = a; // deep copy
        CHECK(Item::Live() == 40);

        Array<Item> moved = Move(a); // steals buffer, no new Items
        CHECK(Item::Live() == 40);
        CHECK(moved.Size() == 20u);
    }
    CHECK(Item::Live() == 0); // everything destroyed
}

TEST_CASE("containers: Array honours a custom allocator")
{
    alignas(64) byte buffer[4096];
    LinearAllocator arena(buffer, sizeof(buffer));

    Array<int> a(arena);
    for (int i = 0; i < 10; ++i)
    {
        a.PushBack(i);
    }
    CHECK(a.Size() == 10u);
    CHECK(arena.Used() > 0u);
}

// --- Hash ------------------------------------------------------------------

TEST_CASE("hash: integers and strings hash deterministically")
{
    Hash<int> hi;
    CHECK(hi(42) == hi(42));
    CHECK(hi(42) != hi(43));

    Hash<StringView> hs;
    CHECK(hs(u8"hello") == hs(u8"hello"));
    CHECK(hs(u8"hello") != hs(u8"world"));

    CHECK(HashBytes("abc", 3) == HashBytes("abc", 3));
}

// The published FNV-1a 64 test vectors: the basis is the standard one, and HashBytes, HashText and
// the type ids built on them are the same function.
TEST_CASE("hash: HashBytes and HashText are the published FNV-1a 64")
{
    CHECK(kFnv1a64OffsetBasis == 14695981039346656037ull);
    CHECK(HashBytes("", 0) == 0xcbf29ce484222325ull);
    CHECK(HashBytes("a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(HashBytes("foobar", 6) == 0x85944171f73967e8ull);
    CHECK(HashText(u8"foobar") == 0x85944171f73967e8ull);
    // A type id chains namespace, "::" and name through the same function.
    u64 chained = HashBytes("rtti::test", 10);
    chained = HashBytes("::", 2, chained);
    chained = HashBytes("Probe", 5, chained);
    CHECK(ComputeTypeId("rtti::test", "Probe") == chained);
    CHECK(ComputeTypeId("rtti::test", "Probe") == HashBytes("rtti::test::Probe", 17));
}

// --- Containers: HashMap ---------------------------------------------------

TEST_CASE("hashmap: insert, find, contains, overwrite")
{
    HashMap<int, int> m;
    CHECK(m.IsEmpty());

    m.InsertOrAssign(1, 100);
    m.InsertOrAssign(2, 200);
    CHECK(m.Size() == 2u);
    CHECK(m.Contains(1));
    CHECK_FALSE(m.Contains(99));

    REQUIRE(m.Find(2) != nullptr);
    CHECK(*m.Find(2) == 200);
    CHECK(m.Find(99) == nullptr);

    m.InsertOrAssign(1, 111); // overwrite
    CHECK(m.Size() == 2u);
    CHECK(*m.Find(1) == 111);
}

TEST_CASE("hashmap: remove and tombstone reuse")
{
    HashMap<int, int> m;
    for (int i = 0; i < 50; ++i)
    {
        m.InsertOrAssign(i, i * 10);
    }
    CHECK(m.Size() == 50u);

    CHECK(m.Remove(25));
    CHECK_FALSE(m.Remove(25)); // already gone
    CHECK(m.Size() == 49u);
    CHECK_FALSE(m.Contains(25));

    m.InsertOrAssign(25, 999); // reuse
    CHECK(m.Contains(25));
    CHECK(*m.Find(25) == 999);

    // All other keys still findable after rehashes.
    for (int i = 0; i < 50; ++i)
    {
        REQUIRE(m.Find(i) != nullptr);
        CHECK(*m.Find(i) == (i == 25 ? 999 : i * 10));
    }
}

TEST_CASE("hashmap: grows and iterates")
{
    HashMap<int, int> m;
    int expectedSum = 0;
    for (int i = 0; i < 500; ++i)
    {
        m.InsertOrAssign(i, i);
        expectedSum += i;
    }
    CHECK(m.Size() == 500u);

    int sum = 0;
    int count = 0;
    for (auto& entry : m)
    {
        sum += entry.value;
        ++count;
    }
    CHECK(count == 500);
    CHECK(sum == expectedSum);
}

TEST_CASE("hashmap: string keys")
{
    HashMap<String, int> ages;
    ages.InsertOrAssign(String(u8"alice"), 30);
    ages.InsertOrAssign(String(u8"bob"), 25);

    REQUIRE(ages.Find(String(u8"alice")) != nullptr);
    CHECK(*ages.Find(String(u8"alice")) == 30);
    CHECK(*ages.Find(String(u8"bob")) == 25);
    CHECK(ages.Find(String(u8"carol")) == nullptr);
}

TEST_CASE("hashmap: manages non-trivial value lifetimes")
{
    struct Val
    {
        static int& Live()
        {
            static int n = 0;
            return n;
        }
        int v;
        explicit Val(int x = 0) : v(x) { ++Live(); }
        Val(const Val& o) : v(o.v) { ++Live(); }
        Val(Val&& o) noexcept : v(o.v) { ++Live(); }
        Val& operator=(const Val&) = default;
        Val& operator=(Val&&) = default;
        ~Val() { --Live(); }
    };

    Val::Live() = 0;
    {
        HashMap<int, Val> m;
        for (int i = 0; i < 30; ++i)
        {
            m.InsertOrAssign(i, Val{i});
        } // forces rehashes
        CHECK(m.Size() == 30u);
        m.Remove(5);
        CHECK(m.Size() == 29u);
    }
    CHECK(Val::Live() == 0); // all destroyed across rehash/remove/clear
}

// --- Containers: HashSet ---------------------------------------------------

TEST_CASE("hashset: insert, contains, remove, dedupe")
{
    HashSet<int> set;
    CHECK(set.IsEmpty());

    CHECK(set.Insert(5));       // newly inserted
    CHECK_FALSE(set.Insert(5)); // already present
    CHECK(set.Insert(7));
    CHECK(set.Size() == 2u);

    CHECK(set.Contains(5));
    CHECK_FALSE(set.Contains(99));

    CHECK(set.Remove(5));
    CHECK_FALSE(set.Remove(5));
    CHECK_FALSE(set.Contains(5));
    CHECK(set.Size() == 1u);
}

TEST_CASE("hashset: grows and iterates keys")
{
    HashSet<int> set;
    int expectedSum = 0;
    for (int i = 0; i < 200; ++i)
    {
        set.Insert(i);
        expectedSum += i;
    }
    CHECK(set.Size() == 200u);

    int sum = 0;
    int count = 0;
    for (int key : set)
    {
        sum += key;
        ++count;
    }
    CHECK(count == 200);
    CHECK(sum == expectedSum);
}

TEST_CASE("hashset: string keys")
{
    HashSet<String> set;
    CHECK(set.Insert(String(u8"alpha")));
    CHECK(set.Insert(String(u8"beta")));
    CHECK_FALSE(set.Insert(String(u8"alpha")));

    CHECK(set.Contains(String(u8"beta")));
    CHECK_FALSE(set.Contains(String(u8"gamma")));
    CHECK(set.Size() == 2u);
}

// --- Containers: RingBuffer ------------------------------------------------

TEST_CASE("ringbuffer: FIFO push/pop, full and empty")
{
    RingBuffer<int> ring(3);
    CHECK(ring.IsEmpty());
    CHECK(ring.Capacity() == 3u);

    CHECK(ring.PushBack(1));
    CHECK(ring.PushBack(2));
    CHECK(ring.PushBack(3));
    CHECK(ring.IsFull());
    CHECK_FALSE(ring.PushBack(4)); // rejected when full

    CHECK(ring.Front() == 1);
    CHECK(ring.Back() == 3);

    int out = 0;
    CHECK(ring.PopFront(out));
    CHECK(out == 1);
    CHECK(ring.PopFront(out));
    CHECK(out == 2);
    CHECK(ring.Size() == 1u);

    out = -1;
    CHECK(ring.PopFront(out));
    CHECK(out == 3);
    CHECK(ring.IsEmpty());
    CHECK_FALSE(ring.PopFront(out)); // empty
}

TEST_CASE("ringbuffer: wraps around with interleaved push/pop")
{
    RingBuffer<int> ring(3);
    int out = 0;
    // Cycle well past capacity to exercise index wrap-around.
    for (int i = 0; i < 20; ++i)
    {
        CHECK(ring.PushBack(i));
        CHECK(ring.PopFront(out));
        CHECK(out == i);
    }
    CHECK(ring.IsEmpty());
}

TEST_CASE("ringbuffer: manages non-trivial element lifetimes")
{
    struct Item
    {
        static int& Live()
        {
            static int n = 0;
            return n;
        }
        int v;
        explicit Item(int x = 0) : v(x) { ++Live(); }
        Item(const Item& o) : v(o.v) { ++Live(); }
        Item(Item&& o) noexcept : v(o.v) { ++Live(); }
        Item& operator=(const Item&) = default;
        Item& operator=(Item&&) = default;
        ~Item() { --Live(); }
    };

    Item::Live() = 0;
    {
        RingBuffer<Item> ring(4);
        ring.PushBack(Item{1});
        ring.PushBack(Item{2});
        ring.PushBack(Item{3});
        CHECK(Item::Live() == 3);

        Item out{0};
        ring.PopFront(out);
        CHECK(out.v == 1);
    }
    CHECK(Item::Live() == 0); // all destroyed
}

// --- Containers: IntrusiveList ---------------------------------------------

namespace
{
    struct ListItem : IntrusiveListNode
    {
        int value;
        explicit ListItem(int v) : value(v) {}
    };
}

TEST_CASE("intrusivelist: push/iterate/remove without owning")
{
    ListItem a{1};
    ListItem b{2};
    ListItem c{3};

    IntrusiveList<ListItem> list;
    CHECK(list.IsEmpty());

    list.PushBack(a);
    list.PushBack(b);
    list.PushBack(c);
    CHECK(list.Size() == 3u);
    CHECK(list.Front()->value == 1);
    CHECK(list.Back()->value == 3);

    int sum = 0;
    for (ListItem& item : list)
    {
        sum += item.value;
    }
    CHECK(sum == 6);

    // Remove the middle element (O(1), given the node).
    list.Remove(b);
    CHECK(list.Size() == 2u);
    int order[2] = {0, 0};
    int i = 0;
    for (ListItem& item : list)
    {
        order[i++] = item.value;
    }
    CHECK(order[0] == 1);
    CHECK(order[1] == 3);

    // PushFront orders before existing.
    ListItem head{0};
    list.PushFront(head);
    CHECK(list.Front()->value == 0);

    list.Clear();
    CHECK(list.IsEmpty());
}
