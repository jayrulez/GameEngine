// Ported from Sedulous.UI.Tests/src/AnimationTests.bf (faithful). Beef owned setter delegates ->
// Function<void(T)>; `scope`/`new` -> stack value or UniquePtr (manager takes ownership); Vector2 ->
// Float2 (Vector2Animation -> Float2Animation); Color byte ctors -> float (/255); ctx.Animations ->
// ctx.Animations().
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

// === FloatAnimation ===

TEST_CASE("animation: Float_AtStart_ReturnsFrom")
{
    f32 result = -1;
    FloatAnimation anim(0, 100, 1.0f, [&result](f32 v) { result = v; });
    anim.Start();
    anim.Update(0);
    CHECK(result == 0);
}

TEST_CASE("animation: Float_AtEnd_ReturnsTo")
{
    f32 result = -1;
    FloatAnimation anim(0, 100, 1.0f, [&result](f32 v) { result = v; });
    anim.Start();
    anim.Update(1.0f);
    CHECK(result == 100);
}

TEST_CASE("animation: Float_AtMid_ReturnsInterpolated")
{
    f32 result = -1;
    FloatAnimation anim(0, 100, 1.0f, [&result](f32 v) { result = v; });
    anim.Start();
    anim.Update(0.5f);
    CHECK(result == doctest::Approx(50).epsilon(0.0001));
}

TEST_CASE("animation: Float_WithEasing_AppliesEasing")
{
    f32 result = -1;
    FloatAnimation anim(0, 100, 1.0f, [&result](f32 v) { result = v; }, Easing::EaseInCubic);
    anim.Start();
    anim.Update(0.5f);
    CHECK(result < 50);
    CHECK(result > 0);
}

TEST_CASE("animation: Float_Completes_ReturnsTrue")
{
    FloatAnimation anim(0, 1, 0.5f, [](f32) {});
    anim.Start();
    CHECK(!anim.Update(0.3f));
    CHECK(anim.Update(0.3f));
    CHECK(anim.IsComplete());
}

// === ColorAnimation ===

TEST_CASE("animation: Color_Interpolates")
{
    Color result = Color::Black;
    ColorAnimation anim(Color{0, 0, 0, 1}, Color{1, 1, 1, 1}, 1.0f,
                        [&result](Color v) { result = v; });
    anim.Start();
    anim.Update(0.5f);
    CHECK(result.r > 0.3f);
    CHECK(result.r < 0.8f);
}

// === Float2Animation ===

TEST_CASE("animation: Float2_Interpolates")
{
    Float2 result = Float2{0, 0};
    Float2Animation anim(Float2{0, 0}, Float2{100, 200}, 1.0f, [&result](Float2 v) { result = v; });
    anim.Start();
    anim.Update(0.5f);
    CHECK(result.x == doctest::Approx(50).epsilon(0.0001));
    CHECK(result.y == doctest::Approx(100).epsilon(0.0001));
}

// === Delay ===

TEST_CASE("animation: Delay_WaitsBeforePlaying")
{
    f32 result = -1;
    FloatAnimation anim(0, 100, 1.0f, [&result](f32 v) { result = v; });
    anim.SetDelay(0.5f);
    anim.Start();
    anim.Update(0.3f); // still in delay
    CHECK(result == -1);
    anim.Update(0.3f); // 0.6 total, 0.1 active
    CHECK(result >= 0);
}

// === AutoReverse ===

TEST_CASE("animation: AutoReverse_PlaysBackward")
{
    f32 result = -1;
    FloatAnimation anim(0, 100, 1.0f, [&result](f32 v) { result = v; });
    anim.SetAutoReverse(true);
    anim.SetRepeatCount(1);
    anim.Start();

    anim.Update(1.0f); // completes cycle 0
    anim.Update(0.5f); // midpoint of reverse
    CHECK(result < 100);
}

// === RepeatCount ===

TEST_CASE("animation: RepeatCount_PlaysMultipleTimes")
{
    FloatAnimation anim(0, 1, 0.1f, [](f32) {});
    anim.SetRepeatCount(2);
    anim.Start();

    anim.Update(0.1f);
    CHECK(!anim.IsComplete());
    anim.Update(0.1f);
    CHECK(!anim.IsComplete());
    anim.Update(0.1f);
    CHECK(anim.IsComplete());
}

TEST_CASE("animation: RepeatInfinite_NeverCompletes")
{
    FloatAnimation anim(0, 1, 0.1f, [](f32) {});
    anim.SetRepeatCount(-1);
    anim.Start();

    for (i32 i = 0; i < 100; ++i)
    {
        anim.Update(0.1f);
    }

    CHECK(!anim.IsComplete());
    CHECK(anim.IsRunning());
}

// === OnComplete event ===

TEST_CASE("animation: OnComplete_FiresWhenDone")
{
    bool fired = false;
    FloatAnimation anim(0, 1, 0.5f, [](f32) {});
    anim.OnComplete.Add(Event<void(Animation*)>::Handler{[&fired](Animation*) { fired = true; }});
    anim.Start();
    anim.Update(1.0f);
    CHECK(fired);
}

// === Zero duration ===

TEST_CASE("animation: ZeroDuration_SnapsToEnd")
{
    f32 result = -1;
    FloatAnimation anim(0, 100, 0, [&result](f32 v) { result = v; });
    anim.Start();
    CHECK(anim.Update(0));
    CHECK(result == 100);
    CHECK(anim.IsComplete());
}

// === Reset ===

TEST_CASE("animation: Reset_AllowsReplay")
{
    f32 result = -1;
    FloatAnimation anim(0, 100, 0.5f, [&result](f32 v) { result = v; });
    anim.Start();
    anim.Update(1.0f);
    CHECK(anim.IsComplete());

    anim.Reset();
    CHECK(!anim.IsComplete());
    CHECK(!anim.IsRunning());

    anim.Start();
    anim.Update(0.25f);
    CHECK(result == doctest::Approx(50).epsilon(0.0001));
}

// === Storyboard ===

TEST_CASE("animation: Storyboard_Sequential_RunsInOrder")
{
    i32 order = 0;
    i32 first = -1, second = -1;

    Storyboard sb(Storyboard::Mode::Sequential);
    sb.Add(foundation::MakeUnique<FloatAnimation>(foundation::DefaultAllocator(), 0.0f, 1.0f, 0.1f,
                                            Function<void(f32)>{[&first, &order](f32)
                                                                {
                                                                    if (first < 0)
                                                                    {
                                                                        first = order++;
                                                                    }
                                                                }}));
    sb.Add(foundation::MakeUnique<FloatAnimation>(foundation::DefaultAllocator(), 0.0f, 1.0f, 0.1f,
                                            Function<void(f32)>{[&second, &order](f32)
                                                                {
                                                                    if (second < 0)
                                                                    {
                                                                        second = order++;
                                                                    }
                                                                }}));
    sb.Start();

    sb.Update(0.05f);
    CHECK(first == 0);
    CHECK(second == -1);

    sb.Update(0.1f);
    sb.Update(0.05f);
    CHECK(second >= 0);
}

TEST_CASE("animation: Storyboard_Parallel_RunsSimultaneously")
{
    bool aRan = false, bRan = false;

    Storyboard sb(Storyboard::Mode::Parallel);
    sb.Add(foundation::MakeUnique<FloatAnimation>(foundation::DefaultAllocator(), 0.0f, 1.0f, 0.2f,
                                            Function<void(f32)>{[&aRan](f32) { aRan = true; }}));
    sb.Add(foundation::MakeUnique<FloatAnimation>(foundation::DefaultAllocator(), 0.0f, 1.0f, 0.1f,
                                            Function<void(f32)>{[&bRan](f32) { bRan = true; }}));
    sb.Start();

    sb.Update(0.05f);
    CHECK(aRan);
    CHECK(bRan);
}

// === AnimationManager ===

TEST_CASE("animation: Manager_DeletesOnComplete")
{
    AnimationManager mgr;
    mgr.Add(foundation::MakeUnique<FloatAnimation>(foundation::DefaultAllocator(), 0.0f, 1.0f, 0.1f,
                                             Function<void(f32)>{[](f32) {}}));
    CHECK(mgr.ActiveCount() == 1);

    mgr.Update(0.2f);
    CHECK(mgr.ActiveCount() == 0);
}

TEST_CASE("animation: Manager_CancelAll")
{
    AnimationManager mgr;
    mgr.Add(foundation::MakeUnique<FloatAnimation>(foundation::DefaultAllocator(), 0.0f, 1.0f, 1.0f,
                                             Function<void(f32)>{[](f32) {}}));
    mgr.Add(foundation::MakeUnique<FloatAnimation>(foundation::DefaultAllocator(), 0.0f, 1.0f, 1.0f,
                                             Function<void(f32)>{[](f32) {}}));
    CHECK(mgr.ActiveCount() == 2);

    mgr.CancelAll();
    CHECK(mgr.ActiveCount() == 0);
}

TEST_CASE("animation: Manager_CancelForView")
{
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator(), 50.0f, 30.0f);
    root->AddView(view.Get());

    auto anim = foundation::MakeUnique<FloatAnimation>(foundation::DefaultAllocator(), 0.0f, 1.0f, 1.0f,
                                                 Function<void(f32)>{[](f32) {}});
    anim->SetTarget(view.Get());
    ctx.Animations()->Add(Move(anim));
    CHECK(ctx.Animations()->ActiveCount() == 1);

    ctx.Animations()->CancelForView(view.Get());
    CHECK(ctx.Animations()->ActiveCount() == 0);
}

TEST_CASE("animation: Manager_AutoCancelOnViewDelete")
{
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator(), 50.0f, 30.0f);
    root->AddView(view.Get());

    auto anim = foundation::MakeUnique<FloatAnimation>(foundation::DefaultAllocator(), 0.0f, 1.0f, 1.0f,
                                                 Function<void(f32)>{[](f32) {}});
    anim->SetTarget(view.Get());
    ctx.Animations()->Add(Move(anim));

    root->RemoveView(view.Get(), true);
    CHECK(ctx.Animations()->ActiveCount() == 0);
}
