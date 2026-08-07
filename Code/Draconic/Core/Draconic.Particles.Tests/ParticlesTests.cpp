// draconic.particles - CPU runtime coverage: value types (ranges/curves/emission shapes), the
// SoA stream container (lazy alloc / typed access / swap-remove / compaction), the modules
// (initializers + behaviors), and the effect/system Update loop (spawn, integrate, age, die),
// plus sub-emitters, LOD, and determinism. No GPU/renderer.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.particles;

using namespace draconic::foundation;
namespace particles = draconic::particles;

// ---- value types -------------------------------------------------------------------------------

TEST_CASE("RangeFloat/RangeColor: diagonal lerp between min and max")
{
    const particles::RangeFloat r{2.0f, 6.0f};
    CHECK(r.Evaluate(0.0f) == doctest::Approx(2.0f));
    CHECK(r.Evaluate(1.0f) == doctest::Approx(6.0f));
    CHECK(r.Evaluate(0.5f) == doctest::Approx(4.0f));
    CHECK(particles::RangeFloat::Constant(3.0f).IsConstant());

    const particles::RangeColor c{Float4{0, 0, 0, 0}, Float4{1, 2, 3, 4}};
    const Float4 mid = c.Evaluate(0.5f);
    CHECK(mid.x == doctest::Approx(0.5f));
    CHECK(mid.w == doctest::Approx(2.0f));
}

TEST_CASE("ParticleCurveFloat: endpoints, Constant, Linear, FadeOut")
{
    CHECK_FALSE(particles::ParticleCurveFloat{}.IsActive());
    CHECK(particles::ParticleCurveFloat::Constant(5.0f).Evaluate(0.37f) == doctest::Approx(5.0f));

    const particles::ParticleCurveFloat lin = particles::ParticleCurveFloat::Linear(0.0f, 10.0f);
    CHECK(lin.Evaluate(0.0f) == doctest::Approx(0.0f));
    CHECK(lin.Evaluate(1.0f) == doctest::Approx(10.0f));
    CHECK(lin.Evaluate(-1.0f) == doctest::Approx(0.0f)); // clamps to first key
    CHECK(lin.Evaluate(2.0f) == doctest::Approx(10.0f)); // clamps to last key
    CHECK(lin.Evaluate(0.5f) > 0.0f);
    CHECK(lin.Evaluate(0.5f) < 10.0f);

    const particles::ParticleCurveFloat fade = particles::ParticleCurveFloat::FadeOut(1.0f, 0.75f);
    CHECK(fade.Evaluate(0.0f) == doctest::Approx(1.0f));
    CHECK(fade.Evaluate(1.0f) == doctest::Approx(0.0f));
    CHECK(fade.Evaluate(0.5f) == doctest::Approx(1.0f)); // constant until fadeStart
}

TEST_CASE("EmissionShape: Sphere samples inside radius, Point is origin")
{
    Random rng(1234);
    Float3 pos, dir;

    particles::EmissionShape::Point().Sample(rng, pos, dir);
    CHECK(LengthSquared(pos) == doctest::Approx(0.0f));

    const particles::EmissionShape sphere = particles::EmissionShape::Sphere(2.0f);
    for (int i = 0; i < 200; ++i)
    {
        sphere.Sample(rng, pos, dir);
        CHECK(Length(pos) <= doctest::Approx(2.0f).epsilon(0.01));
        CHECK(Length(dir) == doctest::Approx(1.0f).epsilon(0.01));
    }
}

// ---- stream container --------------------------------------------------------------------------

TEST_CASE("ParticleStreamContainer: core streams present, others lazy, typed access")
{
    particles::ParticleStreamContainer streams(64);
    CHECK(streams.Positions() != nullptr);
    CHECK(streams.Ages() != nullptr);
    CHECK(streams.Lifetimes() != nullptr);
    CHECK(streams.Velocities() == nullptr); // not allocated yet

    streams.EnsureStream(particles::ParticleStreamId::Velocity,
                         particles::StreamElementType::Float3);
    CHECK(streams.Velocities() != nullptr);

    // Idempotent: a second EnsureStream keeps the same stream object.
    particles::ParticleStream* before = streams.GetStream(particles::ParticleStreamId::Velocity);
    streams.EnsureStream(particles::ParticleStreamId::Velocity,
                         particles::StreamElementType::Float3);
    CHECK(streams.GetStream(particles::ParticleStreamId::Velocity) == before);

    // Wrong element type -> null (checked cast).
    CHECK(streams.GetCPUStream<f32>(particles::ParticleStreamId::Velocity) == nullptr);
}

TEST_CASE("ParticleStreamContainer: swap-remove keeps arrays dense, CompactDead drops aged")
{
    particles::ParticleStreamContainer streams(16);
    particles::CPUStream<Float3>* pos = streams.Positions();
    particles::CPUStream<f32>* ages = streams.Ages();
    particles::CPUStream<f32>* lifetimes = streams.Lifetimes();

    for (i32 i = 0; i < 5; ++i)
    {
        (*pos)[i] = Float3{static_cast<f32>(i), 0, 0};
        (*ages)[i] = 0.0f;
        (*lifetimes)[i] = 1.0f;
    }
    streams.aliveCount = 5;

    // Kill index 1: last (index 4) swaps into slot 1.
    streams.SwapRemove(1);
    CHECK(streams.aliveCount == 4);
    CHECK((*pos)[1].x == doctest::Approx(4.0f));

    // Age two out and compact.
    (*ages)[0] = 2.0f;
    (*ages)[2] = 2.0f;
    const i32 removed = streams.CompactDead();
    CHECK(removed == 2);
    CHECK(streams.aliveCount == 2);

    CHECK(streams.GetLifeRatio(0) == doctest::Approx(0.0f));
}

// ---- modules -----------------------------------------------------------------------------------

TEST_CASE("GravityBehavior accelerates velocity down; AlphaOverLifetime fades alpha")
{
    particles::ParticleStreamContainer streams(8);
    streams.EnsureStream(particles::ParticleStreamId::Velocity,
                         particles::StreamElementType::Float3);
    streams.EnsureStream(particles::ParticleStreamId::Color, particles::StreamElementType::Float4);
    streams.aliveCount = 1;
    (*streams.Velocities())[0] = Float3{0, 0, 0};
    (*streams.Colors())[0] = Float4{1, 1, 1, 1};
    (*streams.Ages())[0] = 0.5f;
    (*streams.Lifetimes())[0] = 1.0f;

    Random rng(1);
    particles::ParticleUpdateContext ctx{0.0f, 0.5f, Float3::Zero, &rng};

    particles::GravityBehavior gravity;
    gravity.Update(streams, ctx);
    CHECK((*streams.Velocities())[0].y < 0.0f); // pulled downward

    particles::AlphaOverLifetimeBehavior alpha;
    alpha.curve = particles::ParticleCurveFloat::Linear(1.0f, 0.0f);
    alpha.Update(streams, ctx);
    CHECK((*streams.Colors())[0].w < 1.0f); // alpha reduced at t=0.5
}

// ---- effect / system Update loop ---------------------------------------------------------------

namespace
{
    // Builds a simple upward fountain: continuous emission, 2s life, gravity.
    void BuildFountain(particles::ParticleSystem& sys, f32 rate = 100.0f)
    {
        sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
            particles::RangeFloat(2.0f, 2.0f);
        sys.AddInitializer<particles::VelocityInitializer>().baseVelocity = Float3{0, 5, 0};
        sys.AddInitializer<particles::SizeInitializer>();
        sys.AddInitializer<particles::ColorInitializer>();
        sys.AddBehavior<particles::GravityBehavior>();
        sys.emitter.mode = particles::EmissionMode::Continuous;
        sys.emitter.spawnRate = rate;
    }
}

TEST_CASE("ParticleSystem: continuous emission spawns, integrates, and ages out")
{
    particles::ParticleSystem sys(10000);
    BuildFountain(sys, 100.0f);

    // 0.1s at 100/s -> ~10 particles.
    sys.Update(0.1f);
    CHECK(sys.AliveCount() >= 9);
    CHECK(sys.AliveCount() <= 11);

    // Velocity carried the particles upward (position integrated).
    const particles::CPUStream<Float3>* pos = sys.Streams().Positions();
    CHECK((*pos)[0].y > 0.0f);

    // Run well past the 2s lifetime with emission off -> everything dies.
    sys.emitter.isEmitting = false;
    for (int i = 0; i < 300; ++i)
    {
        sys.Update(0.016f);
    }
    CHECK(sys.AliveCount() == 0);
}

TEST_CASE("ParticleSystem: never exceeds MaxParticles")
{
    particles::ParticleSystem sys(50);
    BuildFountain(sys, 100000.0f); // absurd rate
    for (int i = 0; i < 10; ++i)
    {
        sys.Update(0.1f);
    }
    CHECK(sys.AliveCount() <= 50);
}

TEST_CASE("ParticleSystem/Effect: authoring add/remove modules + systems")
{
    particles::ParticleSystem sys(100);
    BuildFountain(sys); // 4 initializers, 1 behavior
    CHECK(sys.InitializerCount() == 4);
    CHECK(sys.BehaviorCount() == 1);

    sys.RemoveInitializer(0);
    CHECK(sys.InitializerCount() == 3);
    sys.RemoveBehavior(0);
    CHECK(sys.BehaviorCount() == 0);
    sys.RemoveInitializer(99); // out of range: no-op
    CHECK(sys.InitializerCount() == 3);

    particles::ParticleEffect fx;
    fx.AddSystem(100);
    fx.AddSystem(100);
    CHECK(fx.SystemCount() == 2);
    fx.RemoveSystem(0);
    CHECK(fx.SystemCount() == 1);
    fx.Clear();
    CHECK(fx.SystemCount() == 0);
}

TEST_CASE("ParticleSystem: module reorder preserves identity, respects bounds")
{
    particles::ParticleSystem sys(100);
    // Distinct behaviors so we can identify order by runtime type.
    particles::ParticleBehavior* g = &sys.AddBehavior<particles::GravityBehavior>();
    particles::ParticleBehavior* d = &sys.AddBehavior<particles::DragBehavior>();
    particles::ParticleBehavior* w = &sys.AddBehavior<particles::WindBehavior>();
    CHECK(sys.GetBehavior(0) == g);
    CHECK(sys.GetBehavior(2) == w);

    sys.MoveBehavior(2, 0); // wind to the front
    CHECK(sys.GetBehavior(0) == w);
    CHECK(sys.GetBehavior(1) == g);
    CHECK(sys.GetBehavior(2) == d);

    sys.MoveBehavior(0, 2); // wind to the back
    CHECK(sys.GetBehavior(2) == w);
    CHECK(sys.GetBehavior(0) == g);

    sys.MoveBehavior(0, 5); // out of range: no-op
    CHECK(sys.GetBehavior(0) == g);
    CHECK(sys.BehaviorCount() == 3);
}

TEST_CASE("ParticleSystem: SetMaxParticles resizes the budget and re-declares streams")
{
    particles::ParticleSystem sys(50);
    BuildFountain(sys, 500.0f); // heavy spawn rate to exceed the small budget
    CHECK(sys.MaxParticles() == 50);
    for (i32 i = 0; i < 30; ++i)
    {
        sys.Step(1.0f / 60.0f);
    }
    CHECK(sys.AliveCount() <= 50);
    const i32 cappedAlive = sys.AliveCount();
    CHECK(cappedAlive == 50); // saturated at the old budget

    sys.SetMaxParticles(2000);
    CHECK(sys.MaxParticles() == 2000);
    CHECK(sys.AliveCount() == 0); // resize restarts the alive set
    // Streams still function after the reallocation: the system spawns past the old cap.
    for (i32 i = 0; i < 60; ++i)
    {
        sys.Step(1.0f / 60.0f);
    }
    CHECK(sys.AliveCount() > 50);

    sys.SetMaxParticles(2000); // same value: no-op, keeps running
    CHECK(sys.MaxParticles() == 2000);
}

TEST_CASE("ParticleEmitter: single burst when interval <= 0")
{
    particles::ParticleSystem sys(1000);
    sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
        particles::RangeFloat(5.0f, 5.0f);
    sys.emitter.mode = particles::EmissionMode::Burst;
    sys.emitter.burstCount = 20;
    sys.emitter.burstInterval = 0.0f; // single burst

    sys.Update(0.016f);
    CHECK(sys.AliveCount() == 20);
    sys.Update(0.016f);
    CHECK(sys.AliveCount() == 20); // no further bursts
}

TEST_CASE("ParticleSystem: same seed + same input is deterministic")
{
    particles::ParticleSystem a(1000, /*seed*/ 42);
    particles::ParticleSystem b(1000, /*seed*/ 42);
    BuildFountain(a, 200.0f);
    BuildFountain(b, 200.0f);
    for (int i = 0; i < 20; ++i)
    {
        a.Update(0.02f);
        b.Update(0.02f);
    }

    REQUIRE(a.AliveCount() == b.AliveCount());
    REQUIRE(a.AliveCount() > 0);
    const particles::CPUStream<Float3>* pa = a.Streams().Positions();
    const particles::CPUStream<Float3>* pb = b.Streams().Positions();
    for (i32 i = 0; i < a.AliveCount(); ++i)
    {
        CHECK((*pa)[i].x == doctest::Approx((*pb)[i].x));
        CHECK((*pa)[i].y == doctest::Approx((*pb)[i].y));
    }
}

TEST_CASE("LOD: beyond cull distance the system stops spawning")
{
    particles::ParticleSystem sys(1000);
    BuildFountain(sys, 100.0f);
    sys.lodStartDistance = 10.0f;
    sys.lodCullDistance = 20.0f;
    sys.position = Float3{0, 0, 0};

    sys.Update(0.1f, /*cameraPos*/ Float3{100, 0, 0}); // far past cull
    CHECK(sys.LODRateMultiplier() == doctest::Approx(0.0f));
    CHECK(sys.AliveCount() == 0);
}

// ---- sub-emitters ------------------------------------------------------------------------------

// ---- trails -----------------------------------------------------------------------------------

namespace
{
    // A moving Trail-mode system with N burst particles, recording every frame.
    void BuildTrailSystem(particles::ParticleSystem& sys, i32 burst, i32 maxPoints)
    {
        sys.renderMode = particles::ParticleRenderMode::Trail;
        sys.trail.enabled = true;
        sys.trail.maxPoints = maxPoints;
        sys.trail.recordInterval = 0.0f; // record every frame
        sys.trail.minVertexDistance = 0.0f;
        sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
            particles::RangeFloat(100.0f, 100.0f);
        sys.AddInitializer<particles::VelocityInitializer>().baseVelocity =
            Float3{5.0f, 0.0f, 0.0f};
        sys.emitter.mode = particles::EmissionMode::Burst;
        sys.emitter.burstCount = burst;
        sys.emitter.burstInterval = 0.0f;
    }
}

TEST_CASE("Trails: ring buffer fills, caps at maxPoints, and records the current position")
{
    particles::ParticleSystem sys(100);
    BuildTrailSystem(sys, /*burst*/ 3, /*maxPoints*/ 4);
    CHECK_FALSE(sys.trail.IsActive() == false); // enabled + maxPoints>=2

    sys.Update(0.1f);
    REQUIRE(sys.AliveCount() == 3);
    CHECK(sys.TrailMaxPoints() == 4);
    CHECK(sys.TrailStates()[0].count == 1);

    for (int i = 0; i < 10; ++i)
    {
        sys.Update(0.1f);
    }
    const Span<const particles::ParticleTrailState> states = sys.TrailStates();
    REQUIRE(states.Size() == 3);
    for (usize i = 0; i < states.Size(); ++i)
    {
        CHECK(states[i].count == 4);
    } // capped at maxPoints

    // The newest point (at head) is the particle's current position.
    const Span<const particles::TrailPoint> points = sys.TrailPoints();
    const particles::CPUStream<Float3>* pos = sys.Streams().Positions();
    const i32 head = states[0].head;
    CHECK(points[0 * 4 + head].position.x == doctest::Approx((*pos)[0].x));
}

TEST_CASE("Trails: recordInterval gates how often points are added")
{
    particles::ParticleSystem slow(100);
    BuildTrailSystem(slow, 1, 16);
    slow.trail.recordInterval = 0.5f; // one point per 0.5s
    slow.trail.minVertexDistance = 1e9f;

    for (int i = 0; i < 10; ++i)
    {
        slow.Update(0.1f);
    } // 1.0s total
    REQUIRE(slow.AliveCount() == 1);
    // ~1 initial + ~2 interval points (at 0.5s, 1.0s) -> a handful, not 10.
    CHECK(slow.TrailStates()[0].count <= 4);
    CHECK(slow.TrailStates()[0].count >= 2);
}

TEST_CASE("Trails: compaction keeps trail state aligned; dead particles drop cleanly")
{
    particles::ParticleSystem sys(100);
    sys.renderMode = particles::ParticleRenderMode::Trail;
    sys.trail.enabled = true;
    sys.trail.maxPoints = 8;
    sys.trail.recordInterval = 0.0f;
    sys.trail.minVertexDistance = 0.0f;
    sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
        particles::RangeFloat(0.25f, 0.25f); // short
    sys.AddInitializer<particles::VelocityInitializer>().baseVelocity = Float3{2.0f, 0.0f, 0.0f};
    sys.emitter.mode = particles::EmissionMode::Continuous;
    sys.emitter.spawnRate = 200.0f;

    for (int i = 0; i < 30; ++i)
    {
        sys.Update(0.02f);
    }
    // Steady state: alive particles all have trail states within [0, count<=maxPoints].
    const Span<const particles::ParticleTrailState> states = sys.TrailStates();
    CHECK(static_cast<i32>(states.Size()) == sys.AliveCount());
    for (usize i = 0; i < states.Size(); ++i)
    {
        CHECK(states[i].count <= 8);
        CHECK(states[i].count >= 0);
    }

    sys.emitter.isEmitting = false;
    for (int i = 0; i < 30; ++i)
    {
        sys.Update(0.02f);
    }
    CHECK(sys.AliveCount() == 0); // everything ages out, no crash
}

TEST_CASE("Sub-emitter: parent death spawns into the child system")
{
    particles::ParticleEffect fx(u8"fireworks");

    // System 0: short-lived rockets (die quickly -> emit OnDeath events).
    particles::ParticleSystem& rockets = fx.AddSystem(100);
    rockets.AddInitializer<particles::LifetimeInitializer>().lifetime =
        particles::RangeFloat(0.05f, 0.05f);
    rockets.emitter.mode = particles::EmissionMode::Burst;
    rockets.emitter.burstCount = 4;
    rockets.emitter.burstInterval = 0.0f;

    // System 1: sparks, spawned by rocket deaths (no self-emission).
    particles::ParticleSystem& sparks = fx.AddSystem(1000);
    sparks.AddInitializer<particles::LifetimeInitializer>().lifetime =
        particles::RangeFloat(1.0f, 1.0f);
    sparks.emitter.isEmitting = false;

    particles::SubEmitterLink link = particles::SubEmitterLink::Default();
    link.trigger = particles::ParticleEventType::OnDeath;
    link.childSystemIndex = 1;
    link.spawnCount = 10;
    link.probability = 1.0f;
    fx.AddSubEmitterLink(link);

    particles::ParticleEffectInstance inst(fx);
    inst.Update(0.016f); // rockets burst (4), still alive
    CHECK(rockets.AliveCount() == 4);
    CHECK(sparks.AliveCount() == 0);

    inst.Update(0.1f); // rockets age out -> death events -> 4 * 10 sparks
    CHECK(rockets.AliveCount() == 0);
    CHECK(sparks.AliveCount() == 40);
}

// ---- new parity+ features ----------------------------------------------------------------------

TEST_CASE("EmissionShape: Circle is a flat XZ disc, Edge is a line on X")
{
    Random rng;
    for (int i = 0; i < 64; ++i)
    {
        Float3 pos, dir;
        particles::EmissionShape::Circle(2.0f).Sample(rng, pos, dir);
        CHECK(pos.y == doctest::Approx(0.0f));
        CHECK(Length(Float3{pos.x, 0.0f, pos.z}) <= doctest::Approx(2.0f).epsilon(0.01));

        particles::EmissionShape::Edge(3.0f).Sample(rng, pos, dir);
        CHECK(pos.y == doctest::Approx(0.0f));
        CHECK(pos.z == doctest::Approx(0.0f));
        CHECK(pos.x >= -3.01f);
        CHECK(pos.x <= 3.01f);
    }
}

TEST_CASE("EmissionShape: Arc restricts the azimuth to the first quadrant")
{
    Random rng;
    particles::EmissionShape s = particles::EmissionShape::Circle(1.0f, /*shell*/ true);
    s.arc = 0.25f; // quarter turn -> phi in [0, pi/2] -> x>=0, z>=0
    for (int i = 0; i < 64; ++i)
    {
        Float3 pos, dir;
        s.Sample(rng, pos, dir);
        CHECK(pos.x >= -0.001f);
        CHECK(pos.z >= -0.001f);
    }
}

TEST_CASE("FlipbookSettings: FrameUV walks a grid over lifetime")
{
    particles::FlipbookSettings fb;
    fb.enabled = true;
    fb.columns = 4;
    fb.rows = 4;
    fb.overLifetime = true;
    CHECK(fb.IsActive());
    CHECK(fb.FrameCount() == 16);

    const Float4 f0 = fb.FrameUV(0.0f, 0.0f); // frame 0 -> col0,row0
    CHECK(f0.x == doctest::Approx(0.0f));
    CHECK(f0.y == doctest::Approx(0.0f));
    CHECK(f0.z == doctest::Approx(0.25f));
    CHECK(f0.w == doctest::Approx(0.25f));

    const Float4 f8 = fb.FrameUV(0.5f, 0.0f); // frame 8 -> col0,row2
    CHECK(f8.x == doctest::Approx(0.0f));
    CHECK(f8.y == doctest::Approx(0.5f));

    const Float4 fL = fb.FrameUV(0.999f, 0.0f); // frame 15 -> col3,row3
    CHECK(fL.x == doctest::Approx(0.75f));
    CHECK(fL.y == doctest::Approx(0.75f));
}

TEST_CASE("Local space: particles spawn emitter-relative (near origin), not at the world position")
{
    particles::ParticleEffect fx(u8"local");
    particles::ParticleSystem& sys = fx.AddSystem(100);
    sys.simulationSpace = particles::ParticleSpace::Local;
    sys.AddInitializer<particles::PositionInitializer>(); // Point shape -> local origin
    sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
        particles::RangeFloat(5.0f, 5.0f);
    sys.emitter.mode = particles::EmissionMode::Burst;
    sys.emitter.burstCount = 8;

    particles::ParticleEffectInstance inst(fx);
    inst.position = Float3{100.0f, 0.0f, 0.0f}; // far from origin
    inst.Update(0.016f);
    REQUIRE(sys.AliveCount() == 8);
    // Stored positions are local (~origin); the emitter transform is applied only at extract.
    CHECK(Length((*sys.Streams().Positions())[0]) < 1.0f);
}

TEST_CASE("Emitter duration: one-shot stops, looping re-arms")
{
    particles::ParticleEmitter oneShot;
    oneShot.mode = particles::EmissionMode::Continuous;
    oneShot.spawnRate = 100.0f;
    oneShot.duration = 0.1f;
    oneShot.looping = false;
    CHECK(oneShot.CalculateSpawnCount(0.05f) == 5); // inside the window
    CHECK(oneShot.CalculateSpawnCount(0.10f) == 0); // past it -> stop

    particles::ParticleEmitter loop;
    loop.mode = particles::EmissionMode::Continuous;
    loop.spawnRate = 100.0f;
    loop.duration = 0.1f;
    loop.looping = true;
    CHECK(loop.CalculateSpawnCount(0.05f) == 5);
    CHECK(loop.CalculateSpawnCount(0.10f) > 0); // wraps and keeps emitting
}

TEST_CASE("Prewarm: the effect is already populated on its first Update")
{
    particles::ParticleEffect fx(u8"prewarm");
    particles::ParticleSystem& sys = fx.AddSystem(500);
    sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
        particles::RangeFloat(10.0f, 10.0f);
    sys.emitter.spawnRate = 100.0f;
    sys.prewarmTime = 1.0f; // ~100 particles simulated before the first visible frame

    particles::ParticleEffectInstance inst(fx);
    inst.Update(0.016f);
    CHECK(sys.AliveCount() > 50);
}

TEST_CASE("CollisionBehavior: a particle bounces off the ground plane")
{
    particles::ParticleEffect fx(u8"collide");
    particles::ParticleSystem& sys = fx.AddSystem(10);
    sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
        particles::RangeFloat(10.0f, 10.0f);
    particles::CollisionBehavior& col = sys.AddBehavior<particles::CollisionBehavior>();
    col.planes[0] = particles::CollisionPlane{Float3{0.0f, 1.0f, 0.0f}, 0.0f}; // ground y=0
    col.bounce = 0.5f;
    col.friction = 0.0f;
    sys.emitter.mode = particles::EmissionMode::Burst;
    sys.emitter.burstCount = 1;

    particles::ParticleEffectInstance inst(fx);
    inst.Update(0.016f);
    REQUIRE(sys.AliveCount() == 1);
    // Drive it below the plane moving downward, then step: expect a push-out + upward bounce.
    (*sys.Streams().Positions())[0] = Float3{0.0f, -1.0f, 0.0f};
    (*sys.Streams().Velocities())[0] = Float3{0.0f, -2.0f, 0.0f};
    inst.Update(0.016f);
    CHECK((*sys.Streams().Velocities())[0].y == doctest::Approx(1.0f)); // -(-2)*0.5
    CHECK((*sys.Streams().Positions())[0].y >= -0.001f); // pushed to/above the surface
}

TEST_CASE("Seeded RNG: same seed reproduces spawns; Reset replays deterministically")
{
    auto build = [](particles::ParticleSystem& s)
    {
        s.AddInitializer<particles::PositionInitializer>().shape =
            particles::EmissionShape::Sphere(3.0f);
        s.AddInitializer<particles::LifetimeInitializer>().lifetime =
            particles::RangeFloat(5.0f, 5.0f);
        s.emitter.mode = particles::EmissionMode::Burst;
        s.emitter.burstCount = 16;
    };
    particles::ParticleEffect a(u8"a");
    particles::ParticleSystem& sa = a.AddSystem(100, 12345ull);
    build(sa);
    particles::ParticleEffect b(u8"b");
    particles::ParticleSystem& sb = b.AddSystem(100, 12345ull);
    build(sb);
    particles::ParticleEffectInstance ia(a), ib(b);
    ia.Update(0.016f);
    ib.Update(0.016f);
    REQUIRE(sa.AliveCount() == 16);
    REQUIRE(sb.AliveCount() == 16);
    CHECK(Length((*sa.Streams().Positions())[7] - (*sb.Streams().Positions())[7]) ==
          doctest::Approx(0.0f));

    const Float3 first = (*sa.Streams().Positions())[3];
    sa.Reset();
    ia.Update(0.016f);
    CHECK(Length((*sa.Streams().Positions())[3] - first) == doctest::Approx(0.0f));
}

TEST_CASE("Sub-emitter SpawnAt: inherits (adds) velocity and modulates color")
{
    particles::ParticleEffect fx(u8"inherit");
    particles::ParticleSystem& child = fx.AddSystem(100);
    child.AddInitializer<particles::VelocityInitializer>().baseVelocity = Float3{1.0f, 0.0f, 0.0f};
    child.AddInitializer<particles::ColorInitializer>().color =
        particles::RangeColor::Constant(Float4{1, 1, 1, 1});
    child.AddInitializer<particles::LifetimeInitializer>().lifetime =
        particles::RangeFloat(5.0f, 5.0f);

    child.SpawnAt(1, Float3{0, 0, 0}, Float3{0.0f, 5.0f, 0.0f}, Float4{1.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(child.AliveCount() == 1);
    const Float3 v = (*child.Streams().Velocities())[0];
    CHECK(v.x == doctest::Approx(1.0f)); // base
    CHECK(v.y == doctest::Approx(5.0f)); // + inherited
    const Float4 c = (*child.Streams().Colors())[0];
    CHECK(c.x == doctest::Approx(1.0f));
    CHECK(c.y == doctest::Approx(0.0f)); // white * red
    CHECK(c.z == doctest::Approx(0.0f));
}

TEST_CASE("CollisionBehavior: sphere + box obstacles push out and reflect")
{
    particles::ParticleEffect fx(u8"obstacles");
    particles::ParticleSystem& sys = fx.AddSystem(10);
    sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
        particles::RangeFloat(10.0f, 10.0f);
    particles::CollisionBehavior& col = sys.AddBehavior<particles::CollisionBehavior>();
    col.planeCount = 0;
    col.spheres[0] = particles::CollisionSphere{Float3{0, 0, 0}, 1.0f};
    col.sphereCount = 1;
    col.boxes[0] = particles::CollisionBox{Float3{5, 0, 0}, Float3{1, 1, 1}};
    col.boxCount = 1;
    col.bounce = 1.0f;
    col.friction = 0.0f;
    sys.emitter.mode = particles::EmissionMode::Burst;
    sys.emitter.burstCount = 2;

    particles::ParticleEffectInstance inst(fx);
    inst.Update(0.016f);
    REQUIRE(sys.AliveCount() == 2);

    // Particle 0: inside the sphere moving toward its centre -> pushed to the surface, velocity reversed.
    (*sys.Streams().Positions())[0] = Float3{0.5f, 0.0f, 0.0f};
    (*sys.Streams().Velocities())[0] = Float3{-1.0f, 0.0f, 0.0f}; // heading inward (toward -x)
    // Particle 1: just inside the box's top face, falling -> popped up to the top, velocity reversed to +y.
    (*sys.Streams().Positions())[1] =
        Float3{5.0f, 0.5f, 0.0f}; // box y-span [-1,1]; 0.5 nearest the +y face
    (*sys.Streams().Velocities())[1] = Float3{0.0f, -1.0f, 0.0f};
    inst.Update(0.016f);

    CHECK((*sys.Streams().Positions())[0].x >= 1.0f - 0.01f); // out to the sphere surface (r=1)
    CHECK((*sys.Streams().Velocities())[0].x == doctest::Approx(1.0f)); // reversed (bounce=1)
    CHECK((*sys.Streams().Positions())[1].y >= 1.0f - 0.01f); // popped up to the box top (y=1)
    CHECK((*sys.Streams().Velocities())[1].y ==
          doctest::Approx(1.0f)); // reflected off the top face
}

TEST_CASE("AlphaOverLifetime sets the envelope (no per-frame accumulation)")
{
    particles::ParticleStreamContainer streams(8);
    streams.EnsureStream(particles::ParticleStreamId::Color, particles::StreamElementType::Float4);
    streams.aliveCount = 1;
    (*streams.Colors())[0] = Float4{1, 1, 1, 1};
    (*streams.Ages())[0] = 0.5f;
    (*streams.Lifetimes())[0] = 1.0f;
    Random rng(1);
    particles::ParticleUpdateContext ctx{0.0f, 0.016f, Float3::Zero, &rng};
    particles::AlphaOverLifetimeBehavior a;
    a.curve = particles::ParticleCurveFloat::Linear(1.0f, 0.0f); // curve(0.5) = 0.5

    a.Update(streams, ctx);
    const f32 first = (*streams.Colors())[0].w;
    a.Update(streams, ctx); // same t across frames must NOT keep multiplying (the old *= bug -> 0)
    a.Update(streams, ctx);
    CHECK(first == doctest::Approx(0.5f));
    CHECK((*streams.Colors())[0].w == doctest::Approx(first)); // stable, not 0.5^3
}

TEST_CASE("particle reflection (batch 1): flat/range module types reflect their config")
{
    using namespace draconic::particles;
    RegisterParticleModules(); // registers module types + the range value types

    // A flat behavior: scalar + Float3 properties.
    const TypeInfo& gravity = GravityBehavior::StaticType();
    CHECK(PropertyCount(gravity) == 2u);
    const PropertyInfo* mult = FindProperty(gravity, "multiplier");
    const PropertyInfo* dir = FindProperty(gravity, "direction");
    REQUIRE(mult != nullptr);
    REQUIRE(dir != nullptr);
    GravityBehavior g;
    g.multiplier = 2.5f;
    Instance gi = Instance::From(&g);
    CHECK(GetProperty(*mult, gi).Get<f32>() == doctest::Approx(2.5f));

    // A range-typed field is Nested: recurse into RangeFloat's min/max in place.
    const TypeInfo& lifetime = LifetimeInitializer::StaticType();
    const PropertyInfo* lifeProp = FindProperty(lifetime, "lifetime");
    REQUIRE(lifeProp != nullptr);
    CHECK(IsNested(*lifeProp));
    CHECK(lifeProp->type == &TypeOf<RangeFloat>());

    LifetimeInitializer li;
    li.lifetime = RangeFloat(0.5f, 2.0f);
    Instance lii = Instance::From(&li);
    const Instance rangeInst(lifeProp->address(lii), lifeProp->type);
    const PropertyInfo* maxProp = FindProperty(TypeOf<RangeFloat>(), "max");
    REQUIRE(maxProp != nullptr);
    CHECK(GetProperty(*maxProp, rangeInst).Get<f32>() == doctest::Approx(2.0f));
}

TEST_CASE("particle reflection (batch 2): emission-shape initializers reflect")
{
    using namespace draconic::particles;
    RegisterParticleModules();

    // EmissionShape: flat struct whose `type` is a named enum (8 shapes).
    const PropertyInfo* typeProp = FindProperty(TypeOf<EmissionShape>(), "type");
    REQUIRE(typeProp != nullptr);
    CHECK(IsEnum(*typeProp->type));
    CHECK(Enumerators(*typeProp->type).Size() == 8u);
    CHECK(PropertyCount(TypeOf<EmissionShape>()) == 6u); // type/radius/extents/angle/arc/emitFromShell

    // PositionInitializer exposes its shape as a Nested EmissionShape + a localSpace flag.
    const TypeInfo& pos = PositionInitializer::StaticType();
    const PropertyInfo* shapeProp = FindProperty(pos, "shape");
    REQUIRE(shapeProp != nullptr);
    CHECK(IsNested(*shapeProp));
    CHECK(shapeProp->type == &TypeOf<EmissionShape>());

    PositionInitializer p;
    p.shape.type = EmissionShapeType::Cone;
    p.shape.radius = 3.0f;
    Instance pi = Instance::From(&p);
    const Instance shapeInst(shapeProp->address(pi), shapeProp->type);
    const PropertyInfo* radiusProp = FindProperty(TypeOf<EmissionShape>(), "radius");
    REQUIRE(radiusProp != nullptr);
    CHECK(GetProperty(*radiusProp, shapeInst).Get<f32>() == doctest::Approx(3.0f));
    CHECK(GetProperty(*FindProperty(TypeOf<EmissionShape>(), "type"), shapeInst)
              .Get<EmissionShapeType>() == EmissionShapeType::Cone);
}

TEST_CASE("particle reflection (batch 3): force + collision modules reflect their flat config")
{
    using namespace draconic::particles;
    RegisterParticleModules();

    const TypeInfo& attractor = AttractorBehavior::StaticType();
    CHECK(PropertyCount(attractor) == 3u); // strength/position/radius
    AttractorBehavior a;
    a.strength = 4.0f;
    Instance ai = Instance::From(&a);
    CHECK(GetProperty(*FindProperty(attractor, "strength"), ai).Get<f32>() == doctest::Approx(4.0f));

    // CollisionBehavior: 3 BoundedArray shape lists + 3 counts + 4 response scalars.
    const TypeInfo& collision = CollisionBehavior::StaticType();
    CHECK(PropertyCount(collision) == 10u);
    CHECK(FindProperty(collision, "bounce") != nullptr);
    CHECK(FindProperty(collision, "planes") != nullptr); // now a BoundedArray container

    // The collision shape element types reflect as flat value types.
    CHECK(PropertyCount(TypeOf<CollisionPlane>()) == 2u);  // normal/distance
    CHECK(PropertyCount(TypeOf<CollisionBox>()) == 2u);    // center/halfExtents
}

TEST_CASE("particle reflection (batch 4): curve behaviors reflect via BoundedArray - 20/20 modules")
{
    using namespace draconic::particles;
    RegisterParticleModules();

    // AlphaOverLifetime.curve is a Nested ParticleCurveFloat; its `keys` is a count-bound container.
    const TypeInfo& alpha = AlphaOverLifetimeBehavior::StaticType();
    const PropertyInfo* curveProp = FindProperty(alpha, "curve");
    REQUIRE(curveProp != nullptr);
    CHECK(IsNested(*curveProp));
    CHECK(curveProp->type == &TypeOf<ParticleCurveFloat>());

    AlphaOverLifetimeBehavior b;
    b.curve = ParticleCurveFloat::Linear(0.0f, 10.0f); // 2 keys
    Instance bi = Instance::From(&b);
    const Instance curveInst(curveProp->address(bi), curveProp->type);

    const PropertyInfo* keysProp = FindProperty(TypeOf<ParticleCurveFloat>(), "keys");
    REQUIRE(keysProp != nullptr);
    REQUIRE(IsContainer(*keysProp->type));
    const Instance keysInst(keysProp->address(curveInst), keysProp->type);
    const ContainerInfo& keys = *keysProp->type->container;

    // Size follows keyCount (2), not the capacity (8). Read the last key's value through it.
    CHECK(ContainerSize(keys, keysInst) == 2u);
    Variant key1 = ContainerGetAt(keys, keysInst, 1);
    const Instance keyInst(key1.ValuePointer(), key1.Type());
    CHECK(GetProperty(*FindProperty(TypeOf<CurveKeyFloat>(), "value"), keyInst).Get<f32>() ==
          doctest::Approx(10.0f));

    // CollisionBehavior's shape lists are BoundedArrays too (default: 1 ground plane).
    CollisionBehavior col;
    const PropertyInfo* planesProp = FindProperty(CollisionBehavior::StaticType(), "planes");
    REQUIRE(planesProp != nullptr);
    REQUIRE(IsContainer(*planesProp->type));
    Instance ci = Instance::From(&col);
    const Instance planesInst(planesProp->address(ci), planesProp->type);
    CHECK(ContainerSize(*planesProp->type->container, planesInst) == 1u); // planeCount default
}

TEST_CASE("particle reflection (batch 5): a polymorphic module array resolves each module's type")
{
    using namespace draconic::particles;
    RegisterParticleModules();

    // A heterogeneous behavior list (the shape ParticleSystem holds): two different concrete
    // module types must resolve to their OWN reflected property sets via the polymorphic container.
    Array<RefPtr<ParticleBehavior>> behaviors;
    RefPtr<GravityBehavior> gravity = MakeRef<GravityBehavior>(DefaultAllocator());
    gravity->multiplier = 3.0f;
    RefPtr<AttractorBehavior> attractor = MakeRef<AttractorBehavior>(DefaultAllocator());
    attractor->strength = 7.0f;
    behaviors.PushBack(gravity);
    behaviors.PushBack(attractor);

    const TypeInfo& listType = TypeOf<Array<RefPtr<ParticleBehavior>>>();
    REQUIRE(IsContainer(listType));
    const ContainerInfo& c = *listType.container;
    CHECK(IsPolymorphicContainer(c));

    Instance inst = Instance::From(&behaviors);
    CHECK(ContainerSize(c, inst) == 2u);

    Variant e0 = ContainerGetAt(c, inst, 0);
    CHECK(e0.Type() == &GravityBehavior::StaticType()); // dynamic type
    const Instance i0(e0.AsObject(), e0.Type());
    CHECK(GetProperty(*FindProperty(*e0.Type(), "multiplier"), i0).Get<f32>() ==
          doctest::Approx(3.0f));

    Variant e1 = ContainerGetAt(c, inst, 1);
    CHECK(e1.Type() == &AttractorBehavior::StaticType());
    const Instance i1(e1.AsObject(), e1.Type());
    CHECK(GetProperty(*FindProperty(*e1.Type(), "strength"), i1).Get<f32>() ==
          doctest::Approx(7.0f));
}

TEST_CASE("particle reflection (batch 6): polymorphic add/remove/move + EnumerateDerived")
{
    using namespace draconic::particles;
    RegisterParticleModules();

    Array<RefPtr<ParticleBehavior>> behaviors;
    const ContainerInfo& c = *TypeOf<Array<RefPtr<ParticleBehavior>>>().container;
    Instance inst = Instance::From(&behaviors);

    // createElement (via the registrant's serialization factory) builds a live element whose
    // dynamic type reflects + is editable in place.
    CHECK(ContainerCanCreateElement(c, GravityBehavior::StaticType())); // eligible
    Instance e = ContainerCreateElement(c, inst, 0, GravityBehavior::StaticType());
    REQUIRE(e.Pointer() != nullptr);
    CHECK(e.Type() == &GravityBehavior::StaticType());
    REQUIRE(behaviors.Size() == 1u);
    CHECK(SetProperty(*FindProperty(*e.Type(), "multiplier"), e, Variant::From(2.0f)).IsOk());
    CHECK(GetProperty(*FindProperty(*e.Type(), "multiplier"), e).Get<f32>() == doctest::Approx(2.0f));

    // The abstract base is not creatable -> not eligible, and create fails cleanly (no insert).
    CHECK_FALSE(ContainerCanCreateElement(c, ParticleBehavior::StaticType()));
    CHECK(ContainerCreateElement(c, inst, 0, ParticleBehavior::StaticType()).Pointer() == nullptr);
    CHECK(behaviors.Size() == 1u);

    // Add a second, reorder, remove (module arrays are order-sensitive).
    (void)ContainerCreateElement(c, inst, 1, AttractorBehavior::StaticType());
    REQUIRE(behaviors.Size() == 2u);
    CHECK(ContainerMoveElement(c, inst, 0, 1).IsOk());
    CHECK(behaviors[0]->GetType() == &AttractorBehavior::StaticType());
    CHECK(ContainerRemoveAt(c, inst, 0).IsOk());
    CHECK(behaviors.Size() == 1u);

    // EnumerateDerived lists the creatable behaviors (13), never the abstract base.
    Array<const TypeInfo*> derived;
    EnumerateDerived(ParticleBehavior::StaticType(), derived);
    CHECK(derived.Size() == 13u);
    for (const TypeInfo* t : derived)
    {
        CHECK(t != &ParticleBehavior::StaticType());
    }
}

TEST_CASE("particle reflection (batch 7): a whole effect traverses effect -> system -> module -> value")
{
    using namespace draconic::particles;
    RegisterParticleModules(); // also registers the effect/system/emitter value types + systems container

    // The effect-graph value types are published to the global registry (so the script harvest / the
    // reachability closure can reach them once a facade returns an effect handle).
    CHECK(GlobalTypeRegistry().FindById(TypeOf<ParticleEffect>().id) != nullptr);
    CHECK(GlobalTypeRegistry().FindById(TypeOf<ParticleSystem>().id) != nullptr);
    CHECK(GlobalTypeRegistry().FindById(TypeOf<ParticleEmitter>().id) != nullptr);

    // Author a live effect: one system, an emitter config, one initializer + one behavior.
    ParticleEffect effect(u8"Sparks");
    ParticleSystem& sys = effect.AddSystem(256);
    sys.name = String(u8"main");
    sys.emitter.spawnRate = 42.0f;
    sys.AddInitializer<PositionInitializer>();
    GravityBehavior& gravity = sys.AddBehavior<GravityBehavior>();
    gravity.multiplier = 5.0f;

    Instance effectInst = Instance::From(&effect);

    // effect.name is a plain reflected property; effect.systems is the UniquePtr container.
    CHECK(GetProperty(*FindProperty(TypeOf<ParticleEffect>(), "name"), effectInst).Get<String>() ==
          String(u8"Sparks"));
    const PropertyInfo* systemsProp = FindProperty(TypeOf<ParticleEffect>(), "systems");
    REQUIRE(systemsProp != nullptr);
    CHECK(IsNested(*systemsProp));
    REQUIRE(IsContainer(*systemsProp->type));
    const ContainerInfo& sysC = *systemsProp->type->container;
    CHECK_FALSE(IsPolymorphicContainer(sysC)); // homogeneous UniquePtr<ParticleSystem>
    const Instance systemsInst(systemsProp->address(effectInst), systemsProp->type);
    REQUIRE(ContainerSize(sysC, systemsInst) == 1u);

    // Descend into the system by ADDRESS (a UniquePtr element yields no Variant).
    CHECK(ContainerGetAt(sysC, systemsInst, 0).IsEmpty());
    Instance sys0 = ContainerAddressAt(sysC, systemsInst, 0);
    REQUIRE(sys0.Pointer() != nullptr);
    CHECK(sys0.Type() == &TypeOf<ParticleSystem>());
    CHECK(GetProperty(*FindProperty(*sys0.Type(), "name"), sys0).Get<String>() == String(u8"main"));

    // system.emitter (Nested value) -> spawnRate.
    const PropertyInfo* emitterProp = FindProperty(*sys0.Type(), "emitter");
    REQUIRE(emitterProp != nullptr);
    CHECK(IsNested(*emitterProp));
    const Instance emitterInst(emitterProp->address(sys0), emitterProp->type);
    CHECK(GetProperty(*FindProperty(*emitterProp->type, "spawnRate"), emitterInst).Get<f32>() ==
          doctest::Approx(42.0f));

    // system.behaviors (polymorphic container) -> concrete GravityBehavior -> multiplier value. The
    // uniform address path descends both the outer UniquePtr container and this RefPtr<Object> one.
    const PropertyInfo* behProp = FindProperty(*sys0.Type(), "behaviors");
    REQUIRE(behProp != nullptr);
    REQUIRE(IsContainer(*behProp->type));
    const ContainerInfo& behC = *behProp->type->container;
    const Instance behInst(behProp->address(sys0), behProp->type);
    REQUIRE(ContainerSize(behC, behInst) == 1u);
    Instance b0 = ContainerAddressAt(behC, behInst, 0);
    REQUIRE(b0.Pointer() != nullptr);
    CHECK(b0.Type() == &GravityBehavior::StaticType()); // dynamic type
    CHECK(GetProperty(*FindProperty(*b0.Type(), "multiplier"), b0).Get<f32>() == doctest::Approx(5.0f));

    // system.initializers likewise carries the one PositionInitializer.
    const PropertyInfo* initProp = FindProperty(*sys0.Type(), "initializers");
    REQUIRE(initProp != nullptr);
    const Instance initInst(initProp->address(sys0), initProp->type);
    REQUIRE(ContainerSize(*initProp->type->container, initInst) == 1u);
    CHECK(ContainerAddressAt(*initProp->type->container, initInst, 0).Type() ==
          &PositionInitializer::StaticType());

    // ParticleSystem has no default ctor, so the systems container cannot default-emplace (it needs a
    // capacity) - the flavor reports that cleanly rather than fabricating a broken system. Reorder /
    // remove still work.
    CHECK(ContainerEmplaceDefault(sysC, systemsInst, 1).Pointer() == nullptr);
    CHECK(ContainerSize(sysC, systemsInst) == 1u);
    CHECK(ContainerRemoveAt(sysC, systemsInst, 0).IsOk());
    CHECK(ContainerSize(sysC, systemsInst) == 0u);
}
