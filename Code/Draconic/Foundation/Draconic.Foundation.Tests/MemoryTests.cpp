#include <doctest/doctest.h>

#include <cstring>

#include "Draconic.Foundation/Debug/Assert.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;

using namespace draconic::foundation;

// --- Memory ----------------------------------------------------------------

TEST_CASE("memory: alignment helpers")
{
    CHECK(IsPowerOfTwo(1u));
    CHECK(IsPowerOfTwo(64u));
    CHECK_FALSE(IsPowerOfTwo(0u));
    CHECK_FALSE(IsPowerOfTwo(48u));

    CHECK(AlignUp(0u, 16u) == 0u);
    CHECK(AlignUp(1u, 16u) == 16u);
    CHECK(AlignUp(16u, 16u) == 16u);
    CHECK(AlignUp(17u, 16u) == 32u);
    CHECK(AlignDown(31u, 16u) == 16u);

    static_assert(AlignUp(17u, 16u) == 32u);
}

TEST_CASE("memory: SystemAllocator gives aligned, usable storage")
{
    IAllocator& alloc = DefaultAllocator();

    void* p = alloc.Allocate(128, 64);
    REQUIRE(p != nullptr);
    CHECK(IsAligned(p, 64));

    MemSet(p, 0xAB, 128);
    CHECK(static_cast<u8*>(p)[0] == 0xABu);
    CHECK(static_cast<u8*>(p)[127] == 0xABu);

    alloc.Free(p);
    alloc.Free(nullptr); // no-op
}

TEST_CASE("memory: New / Delete construct and destroy")
{
    struct Tracked
    {
        static int& Live()
        {
            static int n = 0;
            return n;
        }
        int value;
        explicit Tracked(int v) : value(v) { ++Live(); }
        ~Tracked() { --Live(); }
    };

    IAllocator& alloc = DefaultAllocator();
    CHECK(Tracked::Live() == 0);

    Tracked* t = alloc.New<Tracked>(7);
    REQUIRE(t != nullptr);
    CHECK(t->value == 7);
    CHECK(Tracked::Live() == 1);

    alloc.Delete(t);
    CHECK(Tracked::Live() == 0);
}

TEST_CASE("memory: LinearAllocator bumps, aligns, exhausts, resets")
{
    alignas(64) byte buffer[256];
    LinearAllocator arena(buffer, sizeof(buffer));

    CHECK(arena.Capacity() == 256u);
    CHECK(arena.Used() == 0u);

    void* a = arena.Allocate(10, 16);
    REQUIRE(a != nullptr);
    CHECK(IsAligned(a, 16));

    void* b = arena.Allocate(10, 16);
    REQUIRE(b != nullptr);
    CHECK(IsAligned(b, 16));
    CHECK(b != a);
    CHECK(arena.Used() >= 20u);

    // Free is a no-op; the arena keeps growing.
    arena.Free(a);

    // Exhaust it.
    void* big = arena.Allocate(1024, 16);
    CHECK(big == nullptr);

    arena.Reset();
    CHECK(arena.Used() == 0u);
    void* c = arena.Allocate(10, 16);
    CHECK(c == a); // first allocation lands at the start again
}

TEST_CASE("memory: PoolAllocator hands out and recycles fixed blocks")
{
    alignas(16) byte buffer[256];
    PoolAllocator pool(buffer, sizeof(buffer), 32, 16);

    const usize capacity = pool.Capacity();
    CHECK(capacity >= 1u);
    CHECK(pool.FreeCount() == capacity);
    CHECK(pool.BlockSize() >= 32u);

    // Drain the pool.
    Array<void*> blocks;
    for (usize i = 0; i < capacity; ++i)
    {
        void* b = pool.Allocate(32, 16);
        REQUIRE(b != nullptr);
        CHECK(IsAligned(b, 16));
        blocks.PushBack(b);
    }
    CHECK(pool.FreeCount() == 0u);
    CHECK(pool.Allocate(32, 16) == nullptr); // exhausted

    // Free one and reallocate -> recycles the block.
    void* recycled = blocks[0];
    pool.Free(recycled);
    CHECK(pool.FreeCount() == 1u);
    CHECK(pool.Allocate(32, 16) == recycled);
}

TEST_CASE("memory: StackAllocator markers reclaim in LIFO order")
{
    alignas(16) byte buffer[256];
    StackAllocator stack(buffer, sizeof(buffer));

    void* a = stack.Allocate(16, 16);
    REQUIRE(a != nullptr);

    const StackAllocator::Marker marker = stack.GetMarker();
    void* b = stack.Allocate(32, 16);
    void* c = stack.Allocate(32, 16);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);
    CHECK(stack.Used() >= 80u);

    // Roll back to the marker; the next allocation reuses b's address.
    stack.FreeToMarker(marker);
    void* b2 = stack.Allocate(32, 16);
    CHECK(b2 == b);

    stack.Reset();
    CHECK(stack.Used() == 0u);
    CHECK(stack.Allocate(16, 16) == a);
}

// --- Smart pointers --------------------------------------------------------

namespace
{
    struct Widget : RefCounted
    {
        static int& Live()
        {
            static int n = 0;
            return n;
        }
        int value;
        explicit Widget(int v) : value(v) { ++Live(); }
        ~Widget() override { --Live(); }
    };

    struct DerivedWidget : Widget
    {
        explicit DerivedWidget(int v) : Widget(v) {}
    };
}

TEST_CASE("memory: UniquePtr owns and releases")
{
    Widget::Live() = 0;
    {
        UniquePtr<Widget> u = MakeUnique<Widget>(DefaultAllocator(), 5);
        REQUIRE(static_cast<bool>(u));
        CHECK(u->value == 5);
        CHECK(Widget::Live() == 1);

        UniquePtr<Widget> moved = Move(u);
        CHECK_FALSE(static_cast<bool>(u));
        CHECK(moved->value == 5);
        CHECK(Widget::Live() == 1);
    }
    CHECK(Widget::Live() == 0);
}

TEST_CASE("memory: RefPtr shares ownership via the intrusive count")
{
    Widget::Live() = 0;
    {
        RefPtr<Widget> a = MakeRef<Widget>(DefaultAllocator(), 9);
        REQUIRE(static_cast<bool>(a));
        CHECK(a->value == 9);
        CHECK(a->RefCount() == 1u);
        CHECK(Widget::Live() == 1);

        {
            RefPtr<Widget> b = a; // copy -> +1
            CHECK(a->RefCount() == 2u);
            CHECK(b.Get() == a.Get());
            CHECK(a == b);
        }
        // b dropped -> back to 1
        CHECK(a->RefCount() == 1u);
        CHECK(Widget::Live() == 1);
    }
    CHECK(Widget::Live() == 0); // destroyed at strong -> 0
}

TEST_CASE("memory: RefPtr upcasts from a derived type")
{
    Widget::Live() = 0;
    {
        RefPtr<DerivedWidget> d = MakeRef<DerivedWidget>(DefaultAllocator(), 3);
        RefPtr<Widget> base = d; // upcast, shares the count
        CHECK(base->value == 3);
        CHECK(d->RefCount() == 2u);
    }
    CHECK(Widget::Live() == 0);
}

TEST_CASE("memory: WeakRefPtr locks while alive and expires after")
{
    Widget::Live() = 0;

    WeakRefPtr<Widget> weak;
    CHECK(weak.Expired());

    {
        RefPtr<Widget> strong = MakeRef<Widget>(DefaultAllocator(), 11);
        weak = WeakRefPtr<Widget>(strong);

        CHECK_FALSE(weak.Expired());
        CHECK(Widget::Live() == 1);

        RefPtr<Widget> locked = weak.Lock();
        REQUIRE(static_cast<bool>(locked));
        CHECK(locked->value == 11);
        CHECK(strong->RefCount() == 2u); // strong + locked
    }

    // strong gone -> object destroyed, but the weak ref keeps the control block.
    CHECK(Widget::Live() == 0);
    CHECK(weak.Expired());
    CHECK_FALSE(static_cast<bool>(weak.Lock()));
}

TEST_CASE("memory: outstanding WeakRefPtr does not keep the object alive")
{
    Widget::Live() = 0;

    WeakRefPtr<Widget> weak;
    {
        RefPtr<Widget> strong = MakeRef<Widget>(DefaultAllocator(), 1);
        weak = WeakRefPtr<Widget>(strong);
        CHECK(Widget::Live() == 1);
    }
    // Object destroyed at strong -> 0 even though a weak ref remains.
    CHECK(Widget::Live() == 0);
    CHECK(weak.Expired());
    // weak destructor here frees the retained control block (clean under ASan).
}

TEST_CASE("memory: TrackingAllocator counts live allocations and detects leaks")
{
    TrackingAllocator tracker(DefaultAllocator());
    CHECK(tracker.LiveAllocations() == 0u);

    void* a = tracker.Allocate(64, 16);
    void* b = tracker.Allocate(32, 16);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(tracker.LiveAllocations() == 2u);
    CHECK(tracker.TotalAllocations() == 2u);
    CHECK(tracker.HasLeaks());

    tracker.Free(a);
    CHECK(tracker.LiveAllocations() == 1u);
    tracker.Free(b);
    CHECK(tracker.LiveAllocations() == 0u);
    CHECK(tracker.TotalFrees() == 2u);
    CHECK_FALSE(tracker.HasLeaks());

    // Works as a drop-in IAllocator for New/Delete.
    struct Probe
    {
        int v = 3;
    };
    Probe* p = tracker.New<Probe>();
    CHECK(tracker.LiveAllocations() == 1u);
    CHECK(p->v == 3);
    tracker.Delete(p);
    CHECK(tracker.LiveAllocations() == 0u);
}

TEST_CASE("memory: FrameAllocator double-buffers across frames")
{
    alignas(16) byte buffer[256];
    FrameAllocator frame(buffer, sizeof(buffer)); // two 128-byte halves

    void* a = frame.Allocate(16, 16);
    REQUIRE(a != nullptr);
    MemSet(a, 0x11, 16);

    // Next frame uses the other half; the previous frame's data stays valid.
    frame.NextFrame();
    void* b = frame.Allocate(16, 16);
    REQUIRE(b != nullptr);
    CHECK(b != a);
    CHECK(static_cast<u8*>(a)[0] == 0x11u); // last frame's allocation still readable

    // Frame after that swaps back and resets the first half.
    frame.NextFrame();
    void* c = frame.Allocate(16, 16);
    CHECK(c == a); // reuses the first half's start

    CHECK(frame.Used() >= 16u);
}

TEST_CASE("memory: TrackingAllocator tracks bytes and peak")
{
    TrackingAllocator tracker(DefaultAllocator());
    CHECK(tracker.LiveBytes() == 0u);

    void* a = tracker.Allocate(100, 16);
    void* b = tracker.Allocate(50, 16);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(IsAligned(a, 16));
    CHECK(tracker.LiveBytes() == 150u);
    CHECK(tracker.TotalBytesAllocated() == 150u);
    CHECK(tracker.PeakBytes() == 150u);

    tracker.Free(a);
    CHECK(tracker.LiveBytes() == 50u);
    CHECK(tracker.PeakBytes() == 150u); // peak retained after free

    void* c = tracker.Allocate(200, 16);
    CHECK(tracker.LiveBytes() == 250u);
    CHECK(tracker.PeakBytes() == 250u);
    CHECK(tracker.TotalBytesAllocated() == 350u);

    tracker.Free(b);
    tracker.Free(c);
    CHECK(tracker.LiveBytes() == 0u);
    CHECK_FALSE(tracker.HasLeaks());
}

TEST_CASE("memory: TaggedAllocator records per-tag usage via the registry")
{
    // Core knows no subsystems; the caller registers tags by name.
    const MemoryTag graphics = RegisterMemoryTag("TestGraphics");
    const MemoryTag audio = RegisterMemoryTag("TestAudio");
    CHECK(graphics.value != audio.value);
    // Idempotent: same name -> same tag.
    CHECK(RegisterMemoryTag("TestGraphics").value == graphics.value);
    CHECK(std::strcmp(MemoryTagName(graphics), "TestGraphics") == 0);

    const u64 beforeBytes = MemoryTagBytes(graphics);
    const u64 beforeCount = MemoryTagAllocations(graphics);

    TaggedAllocator gfx(DefaultAllocator(), graphics);
    void* a = gfx.Allocate(128, 16);
    void* b = gfx.Allocate(64, 16);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    CHECK(MemoryTagBytes(graphics) == beforeBytes + 192);
    CHECK(MemoryTagAllocations(graphics) == beforeCount + 2);
    CHECK(MemoryTagBytes(audio) == 0u); // other tags unaffected

    gfx.Free(a);
    gfx.Free(b);
    CHECK(MemoryTagBytes(graphics) == beforeBytes);
    CHECK(MemoryTagAllocations(graphics) == beforeCount);
}
