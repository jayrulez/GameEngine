// draconic.particles:effect - the particle object model, ported from Sedulous.Particles
// (ParticleEmitter.bf, ParticleSystem.bf, ParticleEffect.bf, ParticleEffectInstance.bf).
//
//   ParticleEffect        - asset definition: a list of systems + sub-emitter links.
//   ParticleSystem        - the workhorse: emitter + initializers + behaviors + streams +
//                           simulator; runs the per-frame Update loop, spawn, LOD, events.
//   ParticleEmitter       - spawn timing only (continuous rate / bursts); returns a count.
//   ParticleEffectInstance- a runtime instance over a shared effect; drives Update + routes
//                           sub-emitter birth/death events between systems.
//
// Trails, mesh/texture/material refs, and serialization are later layers; this is the CPU
// simulation core driven by an effect built in code (or, later, by the cooked resource).

module;
#include "Draconic.Foundation/Prelude.h"
#include <utility> // std::move / std::forward (module-local; core Move/Forward also exist)

export module draconic.particles:effect;

import draconic.foundation;
import :types;
import :streams;
import :modules;

using namespace draconic::foundation;

export namespace draconic::particles
{
    // ---- ParticleEmitter (spawn timing only) -------------------------------------------------

    enum class EmissionMode : u8
    {
        Continuous,
        Burst,
        ContinuousAndBurst
    };

    class ParticleEmitter
    {
    public:
        EmissionMode mode = EmissionMode::Continuous;
        f32 spawnRate = 10.0f;    // particles/second (continuous)
        i32 burstCount = 0;       // particles per burst
        f32 burstInterval = 0.0f; // seconds between bursts (<=0 = single burst on first frame)
        i32 burstCycles = 0;      // 0 = infinite bursts
        bool isEmitting = true;
        f32 duration = 0.0f; // active emission window in seconds (0 = emit forever)
        bool looping = true; // when duration>0: restart the window (else emit once then stop)

        // Returns how many particles to spawn this frame (does not spawn). Accumulator model for
        // continuous; timer/cycles for bursts. Gated by the duration/looping window.
        [[nodiscard]] i32 CalculateSpawnCount(f32 deltaTime) noexcept
        {
            if (!isEmitting)
            {
                return 0;
            }
            // Duration window: outside it, stop (one-shot) or wrap + re-arm bursts (looping).
            if (duration > 0.0f)
            {
                m_cycleTime += deltaTime;
                if (m_cycleTime >= duration)
                {
                    if (!looping)
                    {
                        return 0;
                    }
                    while (m_cycleTime >= duration)
                    {
                        m_cycleTime -= duration;
                    }
                    m_singleBurstDone = false; // re-arm single/looping bursts for the new cycle
                    m_burstCyclesCompleted = 0;
                }
            }
            i32 count = 0;
            if (mode == EmissionMode::Continuous || mode == EmissionMode::ContinuousAndBurst)
            {
                m_spawnAccumulator += spawnRate * deltaTime;
                const i32 whole = static_cast<i32>(m_spawnAccumulator);
                count += whole;
                m_spawnAccumulator -= static_cast<f32>(whole);
            }
            if (mode == EmissionMode::Burst || mode == EmissionMode::ContinuousAndBurst)
            {
                if (burstInterval <= 0.0f)
                {
                    if (!m_singleBurstDone)
                    {
                        count += burstCount;
                        m_singleBurstDone = true;
                    }
                }
                else
                {
                    m_burstTimer += deltaTime;
                    while (m_burstTimer >= burstInterval &&
                           (burstCycles == 0 || m_burstCyclesCompleted < burstCycles))
                    {
                        count += burstCount;
                        m_burstTimer -= burstInterval;
                        ++m_burstCyclesCompleted;
                    }
                }
            }
            return count;
        }

        void Reset() noexcept
        {
            m_spawnAccumulator = 0.0f;
            m_burstTimer = 0.0f;
            m_burstCyclesCompleted = 0;
            m_singleBurstDone = false;
            m_cycleTime = 0.0f;
        }

    private:
        f32 m_cycleTime = 0.0f;
        f32 m_spawnAccumulator = 0.0f;
        f32 m_burstTimer = 0.0f;
        i32 m_burstCyclesCompleted = 0;
        bool m_singleBurstDone = false;
    };

    // ---- ParticleSystem ----------------------------------------------------------------------

    inline constexpr i32 kMaxEventsPerFrame = 64;

    class ParticleSystem
    {
    public:
        // Reflection hook (defined in ParticleEffectReflectionImpl.cpp): a member function so it can
        // reach the private module lists (m_initializers / m_behaviors) without exposing them.
        static void BuildReflection(draconic::foundation::TypeBuilder<ParticleSystem>& builder);

        // Config
        String name;
        SimulationMode desiredMode = SimulationMode::CPU;
        ParticleSpace simulationSpace = ParticleSpace::World;
        ParticleBlendMode blendMode = ParticleBlendMode::Alpha;
        ParticleRenderMode renderMode = ParticleRenderMode::Billboard;
        // Billboard atlas / flipbook sheet, as an OPAQUE cooked-resource GUID (the runtime lib doesn't
        // resolve it - the resource factory binds it to a Proxy<Texture>, the editor cook fills it from an
        // asset path). Null = untextured (renderer's soft-dot default). Sedulous-style ref-on-the-system.
        Guid textureRef{};
        bool sortParticles = false;
        // Soft particles: fade billboard alpha where it nears the opaque surface behind it (needs the
        // scene depth; the renderer supplies it). softDistance is the fade band in world units; disable
        // per-system by clearing softParticles. Read every frame at extract, so it toggles live.
        bool softParticles = true;
        f32 softDistance = 0.6f;
        // Trails: when renderMode==Trail (and trail.IsActive()), each particle records a position ring
        // that the render extractor expands into a camera-facing ribbon.
        TrailSettings trail = TrailSettings::Default();
        // Flipbook: animate the billboard UV rect across a texture-sheet grid (see FlipbookSettings).
        FlipbookSettings flipbook{};
        // Prewarm: on the first Update, simulate this many seconds so the effect starts already-running.
        f32 prewarmTime = 0.0f;
        // LOD (0 disables)
        f32 lodStartDistance = 0.0f;
        f32 lodCullDistance = 0.0f;
        f32 lodMinRate = 0.1f;
        // Runtime transform state
        Float3 position{0.0f, 0.0f, 0.0f};
        Float3 prevPosition{0.0f, 0.0f, 0.0f};

        ParticleEmitter emitter;

        explicit ParticleSystem(i32 maxParticles, u64 seed = 0x9E3779B97F4A7C15ull)
            : m_maxParticles(maxParticles), m_streams(maxParticles),
              m_simulator(MakeUnique<CPUSimulator>(DefaultAllocator())), m_random(seed)
        {
            m_seed = seed;
        }

        ParticleSystem(const ParticleSystem&) = delete;
        ParticleSystem& operator=(const ParticleSystem&) = delete;

        // Reseed the RNG for deterministic playback. Reset() re-applies this seed, so the same seed +
        // same inputs reproduce an identical particle sequence.
        void SetSeed(u64 seed) noexcept
        {
            m_seed = seed;
            m_random = Random(seed);
        }
        [[nodiscard]] u64 Seed() const noexcept { return m_seed; }

        [[nodiscard]] i32 MaxParticles() const noexcept { return m_maxParticles; }
        [[nodiscard]] i32 AliveCount() const noexcept { return m_streams.aliveCount; }
        [[nodiscard]] f32 TotalTime() const noexcept { return m_totalTime; }
        [[nodiscard]] f32 LODRateMultiplier() const noexcept { return m_lodRateMultiplier; }
        [[nodiscard]] bool IsLODCulled() const noexcept
        {
            return m_lodRateMultiplier <= 0.0f && m_streams.aliveCount == 0;
        }
        [[nodiscard]] ParticleStreamContainer& Streams() noexcept { return m_streams; }
        [[nodiscard]] const ParticleStreamContainer& Streams() const noexcept { return m_streams; }

        // Trail ring-buffer access for the render extractor (valid for [0, AliveCount)). TrailMaxPoints
        // is the ring length the points were allocated for; a point is at [particleIndex*maxPoints + slot].
        [[nodiscard]] i32 TrailMaxPoints() const noexcept { return m_trailCapacityPoints; }
        [[nodiscard]] Span<const ParticleTrailState> TrailStates() const noexcept
        {
            return Span<const ParticleTrailState>{m_trailStates.Data(),
                                                  static_cast<usize>(m_streams.aliveCount)};
        }
        [[nodiscard]] Span<const TrailPoint> TrailPoints() const noexcept
        {
            return Span<const TrailPoint>{m_trailPoints.Data(), m_trailPoints.Size()};
        }
        [[nodiscard]] SimulationMode ResolvedMode() const noexcept { return m_resolvedMode; }

        // Build helpers: construct+configure a module in place, declare its streams, own it. Modules are
        // reflected ISerializable (RefCounted) objects, so storage + reconstruction use RefPtr.
        template <typename T, typename... Args>
        T& AddInitializer(Args&&... args)
        {
            RefPtr<T> p = MakeRef<T>(DefaultAllocator(), std::forward<Args>(args)...);
            T& ref = *p;
            ref.DeclareStreams(m_streams);
            m_initializers.PushBack(RefPtr<ParticleInitializer>(std::move(p)));
            return ref;
        }
        template <typename T, typename... Args>
        T& AddBehavior(Args&&... args)
        {
            RefPtr<T> p = MakeRef<T>(DefaultAllocator(), std::forward<Args>(args)...);
            T& ref = *p;
            ref.DeclareStreams(m_streams);
            m_behaviors.PushBack(RefPtr<ParticleBehavior>(std::move(p)));
            return ref;
        }
        // Add a pre-built module (used by the cooked-resource factory after Serializables().Create).
        void AddInitializer(RefPtr<ParticleInitializer> p)
        {
            if (p)
            {
                p->DeclareStreams(m_streams);
                m_initializers.PushBack(std::move(p));
            }
        }
        void AddBehavior(RefPtr<ParticleBehavior> p)
        {
            if (p)
            {
                p->DeclareStreams(m_streams);
                m_behaviors.PushBack(std::move(p));
            }
        }

        // Remove a module by index (editor authoring). Already-declared streams stay allocated,
        // which is harmless - a module never removes a stream another module needs.
        void RemoveInitializer(i32 index)
        {
            if (index >= 0 && index < InitializerCount())
            {
                m_initializers.RemoveAt(static_cast<usize>(index));
            }
        }
        void RemoveBehavior(i32 index)
        {
            if (index >= 0 && index < BehaviorCount())
            {
                m_behaviors.RemoveAt(static_cast<usize>(index));
            }
        }

        // Reorder a module within its list (editor authoring; beyond Sedulous, whose module order was
        // fixed at add-time). Order matters: initializers run top-to-bottom at spawn, behaviors
        // top-to-bottom each step, so e.g. a ColorInitializer must precede a ColorOverLifetime tint.
        void MoveInitializer(i32 from, i32 to) { MoveInList(m_initializers, from, to); }
        void MoveBehavior(i32 from, i32 to) { MoveInList(m_behaviors, from, to); }

        // Resize the particle budget (editor authoring). Reallocates the stream container at the new
        // capacity and re-declares every module's streams into it; the alive set is cleared (the
        // effect restarts) and trail buffers self-heal on the next Step via EnsureTrailStorage.
        void SetMaxParticles(i32 newMax)
        {
            newMax = Max(newMax, 1);
            if (newMax == m_maxParticles)
            {
                return;
            }
            m_maxParticles = newMax;
            m_streams = ParticleStreamContainer(newMax);
            for (usize k = 0; k < m_initializers.Size(); ++k)
            {
                m_initializers[k]->DeclareStreams(m_streams);
            }
            for (usize k = 0; k < m_behaviors.Size(); ++k)
            {
                m_behaviors[k]->DeclareStreams(m_streams);
            }
            m_streams.aliveCount = 0;
            m_trailStates.Clear();
            m_trailPoints.Clear();
            m_trailCapacityPoints = 0;
        }

        // Module access for the resource serializer (writes each module's reflected tag + Serialize).
        [[nodiscard]] i32 InitializerCount() const noexcept
        {
            return static_cast<i32>(m_initializers.Size());
        }
        [[nodiscard]] i32 BehaviorCount() const noexcept
        {
            return static_cast<i32>(m_behaviors.Size());
        }
        [[nodiscard]] ParticleInitializer* GetInitializer(i32 i) noexcept
        {
            return (i >= 0 && i < InitializerCount()) ? m_initializers[static_cast<usize>(i)].Get()
                                                      : nullptr;
        }
        [[nodiscard]] ParticleBehavior* GetBehavior(i32 i) noexcept
        {
            return (i >= 0 && i < BehaviorCount()) ? m_behaviors[static_cast<usize>(i)].Get()
                                                   : nullptr;
        }

        // Resolve CPU/GPU/Auto against behavior support. GPU is stubbed (Phase 6) - Auto/GPU still
        // fall back to CPU here, but the decision is recorded for when the GPU simulator lands.
        void ResolveSimulationMode() noexcept
        {
            switch (desiredMode)
            {
            case SimulationMode::CPU:
                m_resolvedMode = SimulationMode::CPU;
                break;
            case SimulationMode::GPU:
            {
                m_resolvedMode = SimulationMode::GPU;
                for (usize i = 0; i < m_behaviors.Size(); ++i)
                {
                    if (m_behaviors[i]->Support() == BehaviorSupport::CPUOnly)
                    {
                        m_resolvedMode = SimulationMode::CPU;
                        break;
                    }
                }
                break;
            }
            case SimulationMode::Auto:
            {
                bool allSupportGpu = true;
                for (usize i = 0; i < m_behaviors.Size(); ++i)
                {
                    if (m_behaviors[i]->Support() == BehaviorSupport::CPUOnly)
                    {
                        allSupportGpu = false;
                        break;
                    }
                }
                m_resolvedMode = (allSupportGpu && m_maxParticles > 1024) ? SimulationMode::GPU
                                                                          : SimulationMode::CPU;
                break;
            }
            }
            // Until the GPU simulator exists, everything simulates on CPU.
        }

        // Per-frame entry point: runs a one-time prewarm (simulate `prewarmTime` seconds so the effect
        // appears already-running on its first visible frame), then the normal step.
        void Update(f32 deltaTime, Float3 cameraPos = Float3::Zero)
        {
            if (!m_prewarmed)
            {
                m_prewarmed = true;
                if (prewarmTime > 0.0f)
                {
                    const f32 h = 1.0f / 30.0f;
                    for (f32 remaining = prewarmTime; remaining > 1e-4f;)
                    {
                        const f32 s = Min(h, remaining);
                        Step(s, cameraPos);
                        remaining -= s;
                    }
                }
            }
            Step(deltaTime, cameraPos);
        }

        // The per-frame step - exact Sedulous order.
        void Step(f32 deltaTime, Float3 cameraPos = Float3::Zero)
        {
            m_totalTime += deltaTime; // 1
            m_deathCount = 0;
            m_birthCount = 0;                                        // 2
            m_lodRateMultiplier = CalculateLODMultiplier(cameraPos); // 3
            i32 spawnCount = emitter.CalculateSpawnCount(deltaTime); // 4
            spawnCount = (m_lodRateMultiplier <= 0.0f)
                             ? 0
                             : static_cast<i32>(static_cast<f32>(spawnCount) * m_lodRateMultiplier);
            SpawnParticles(spawnCount);
            ParticleUpdateContext ctx{m_totalTime, deltaTime, position, &m_random}; // 5
            m_simulator->Simulate(m_streams, m_behaviors, ctx);                     // 6
            IntegrateVelocityAndAge(deltaTime); // 7 (hardcoded finalize)
            if (trail.IsActive())
            {
                RecordTrailPoints();
            } // 8 (after positions integrated)
            CollectDeathEvents(); // 9 (before compaction)
            if (trail.IsActive())
            {
                CompactDeadWithTrails();
            } // 10 (swap trail state with the particle)
            else
            {
                m_streams.CompactDead();
            }
            prevPosition = position; // 11
        }

        // Spawn `count` new particles through the emitter timing (used by Update).
        void SpawnParticles(i32 count) { SpawnInternal(count, false, Float3::Zero); }
        // Spawn immediately, bypassing emitter timing (sub-emitter birth without inheritance).
        void SpawnImmediate(i32 count) { SpawnInternal(count, false, Float3::Zero); }
        // Spawn at a specific position (sub-emitter). inheritedVelocity is ADDED to each new particle's
        // initialized velocity; inheritedColor MODULATES its color - both no-ops at their defaults
        // (zero / white), so callers pass the already-factored parent velocity/color. (Beyond Sedulous,
        // whose SpawnAt dropped these.)
        void SpawnAt(i32 count, Float3 spawnPos, Float3 inheritedVelocity = Float3::Zero,
                     Float4 inheritedColor = Float4{1.0f, 1.0f, 1.0f, 1.0f})
        {
            SpawnInternal(count, true, spawnPos, true, inheritedVelocity, inheritedColor);
        }

        [[nodiscard]] Span<const ParticleEvent> DeathEvents() const noexcept
        {
            return Span<const ParticleEvent>{m_deathEvents, static_cast<usize>(m_deathCount)};
        }
        [[nodiscard]] Span<const ParticleEvent> BirthEvents() const noexcept
        {
            return Span<const ParticleEvent>{m_birthEvents, static_cast<usize>(m_birthCount)};
        }

        void Reset() noexcept
        {
            m_streams.aliveCount = 0;
            emitter.Reset();
            m_totalTime = 0.0f;
            m_prewarmed = false;
            m_random = Random(m_seed); // deterministic replay from the configured seed
        }

    private:
        // Reposition list[from] so it lands at final index `to` (order-preserving), via
        // remove-then-insert with the post-removal index shift.
        template <typename T>
        static void MoveInList(Array<T>& list, i32 from, i32 to)
        {
            const i32 n = static_cast<i32>(list.Size());
            if (from < 0 || from >= n || to < 0 || to >= n || from == to)
            {
                return;
            }
            // `to` is the desired FINAL index. After removing `from`, the list has n-1 elements and
            // Insert(to) lands the item exactly at final index `to` (valid since to <= n-1).
            T item = std::move(list[static_cast<usize>(from)]);
            list.RemoveAt(static_cast<usize>(from));
            list.Insert(static_cast<usize>(to), std::move(item));
        }

        void SpawnInternal(i32 count, bool overridePosition, Float3 spawnPos, bool inherit = false,
                           Float3 inheritedVelocity = Float3::Zero,
                           Float4 inheritedColor = Float4{1.0f, 1.0f, 1.0f, 1.0f})
        {
            if (count <= 0)
            {
                return;
            }
            // Local space: particles are stored relative to the emitter (origin) and re-based to world at
            // extract, so spawn positions/velocities are emitter-relative (i.e. zero emitter state).
            const bool local = (simulationSpace == ParticleSpace::Local);
            const Float3 emState = local ? Float3::Zero : position;
            const Float3 emitterVel =
                local
                    ? Float3::Zero
                    : (position - prevPosition) /
                          Max(m_totalTime, 0.001f); // Sedulous divides by TotalTime (kept faithful)
            for (usize k = 0; k < m_initializers.Size(); ++k)
            {
                m_initializers[k]->SetEmitterState(emState, emitterVel);
            }
            for (i32 n = 0; n < count; ++n)
            {
                if (m_streams.aliveCount >= m_maxParticles)
                {
                    break;
                }
                const i32 index = m_streams.aliveCount++;
                for (usize k = 0; k < m_initializers.Size(); ++k)
                {
                    m_initializers[k]->Initialize(m_streams, index, m_random);
                }
                if (overridePosition)
                {
                    (*m_streams.Positions())[index] = spawnPos;
                }
                if (inherit)
                {
                    if (CPUStream<Float3>* v = m_streams.Velocities())
                    {
                        (*v)[index] += inheritedVelocity;
                    } // add parent velocity
                    if (CPUStream<Float4>* c = m_streams.Colors())
                    {
                        Float4& cc = (*c)[index];
                        cc = Float4{cc.x * inheritedColor.x, cc.y * inheritedColor.y,
                                    cc.z * inheritedColor.z, cc.w * inheritedColor.w}; // modulate
                    }
                }
                RecordBirthEvent(index);
            }
        }

        void IntegrateVelocityAndAge(f32 deltaTime) noexcept
        {
            CPUStream<Float3>* pos = m_streams.Positions();
            CPUStream<Float3>* vel = m_streams.Velocities();
            CPUStream<f32>* ages = m_streams.Ages();
            const bool applyVel = (pos != nullptr && vel != nullptr);
            for (i32 i = 0; i < m_streams.aliveCount; ++i)
            {
                if (applyVel)
                {
                    (*pos)[i] += (*vel)[i] * deltaTime;
                }
                if (ages != nullptr)
                {
                    (*ages)[i] += deltaTime;
                }
            }
        }

        // Size the trail ring buffers to the current capacity/maxPoints (clears existing trails on resize).
        void EnsureTrailStorage()
        {
            if (m_trailCapacityPoints == trail.maxPoints &&
                static_cast<i32>(m_trailStates.Size()) == m_maxParticles)
            {
                return;
            }
            m_trailStates.Resize(static_cast<usize>(m_maxParticles));
            m_trailPoints.Resize(static_cast<usize>(m_maxParticles) *
                                 static_cast<usize>(trail.maxPoints));
            m_trailCapacityPoints = trail.maxPoints;
            for (usize i = 0; i < m_trailStates.Size(); ++i)
            {
                m_trailStates[i].Clear();
            }
        }

        // Record each live particle's current position into its ring, when enough time has passed OR it
        // moved far enough (or it's the first point). Newest point is at `head`; iterate backward to tail.
        void RecordTrailPoints()
        {
            EnsureTrailStorage();
            CPUStream<Float3>* pos = m_streams.Positions();
            if (pos == nullptr)
            {
                return;
            }
            CPUStream<Float4>* cols = m_streams.Colors();
            const i32 mp = trail.maxPoints;
            for (i32 i = 0; i < m_streams.aliveCount; ++i)
            {
                ParticleTrailState& st = m_trailStates[i];
                const Float3 p = (*pos)[i];
                const bool first = (st.count == 0);
                if (!first)
                {
                    const bool byTime = (m_totalTime - st.lastRecordTime) >= trail.recordInterval;
                    const bool byDist = Length(p - st.lastPosition) >= trail.minVertexDistance;
                    if (!byTime && !byDist)
                    {
                        continue;
                    }
                    st.head = (st.head + 1) % mp;
                }
                TrailPoint& tp = m_trailPoints[static_cast<usize>(i) * static_cast<usize>(mp) +
                                               static_cast<usize>(st.head)];
                tp.position = p;
                tp.width = trail.widthStart;
                tp.color =
                    (trail.useParticleColor && cols != nullptr) ? (*cols)[i] : trail.trailColor;
                tp.recordTime = m_totalTime;
                if (st.count < mp)
                {
                    ++st.count;
                }
                st.lastRecordTime = m_totalTime;
                st.lastPosition = p;
            }
        }

        // Compaction that keeps each particle's trail state/points aligned with its (swap-removed) slot.
        void CompactDeadWithTrails()
        {
            CPUStream<f32>* ages = m_streams.Ages();
            CPUStream<f32>* lifetimes = m_streams.Lifetimes();
            if (ages == nullptr || lifetimes == nullptr)
            {
                return;
            }
            EnsureTrailStorage();
            const i32 mp = trail.maxPoints;
            for (i32 i = m_streams.aliveCount - 1; i >= 0; --i)
            {
                if ((*ages)[i] < (*lifetimes)[i])
                {
                    continue;
                }
                const i32 last = m_streams.aliveCount - 1;
                if (i < last)
                {
                    m_trailStates[i] = m_trailStates[last];
                    for (i32 p = 0; p < mp; ++p)
                    {
                        m_trailPoints[static_cast<usize>(i) * static_cast<usize>(mp) +
                                      static_cast<usize>(p)] =
                            m_trailPoints[static_cast<usize>(last) * static_cast<usize>(mp) +
                                          static_cast<usize>(p)];
                    }
                }
                m_trailStates[last].Clear();
                m_streams.SwapRemove(i); // swaps the SoA streams + decrements aliveCount
            }
        }

        void RecordBirthEvent(i32 index) noexcept
        {
            if (m_birthCount >= kMaxEventsPerFrame)
            {
                return;
            }
            ParticleEvent e;
            e.position = (*m_streams.Positions())[index];
            if (CPUStream<Float3>* vel = m_streams.Velocities())
            {
                e.velocity = (*vel)[index];
            }
            if (CPUStream<Float4>* col = m_streams.Colors())
            {
                e.color = (*col)[index];
            }
            m_birthEvents[m_birthCount++] = e;
        }

        void CollectDeathEvents() noexcept
        {
            CPUStream<f32>* ages = m_streams.Ages();
            CPUStream<f32>* lifetimes = m_streams.Lifetimes();
            if (ages == nullptr || lifetimes == nullptr)
            {
                return;
            }
            CPUStream<Float3>* pos = m_streams.Positions();
            CPUStream<Float3>* vel = m_streams.Velocities();
            CPUStream<Float4>* col = m_streams.Colors();
            for (i32 i = 0; i < m_streams.aliveCount; ++i)
            {
                if ((*ages)[i] < (*lifetimes)[i])
                {
                    continue;
                }
                if (m_deathCount >= kMaxEventsPerFrame)
                {
                    break;
                }
                ParticleEvent e;
                if (pos != nullptr)
                {
                    e.position = (*pos)[i];
                }
                if (vel != nullptr)
                {
                    e.velocity = (*vel)[i];
                }
                if (col != nullptr)
                {
                    e.color = (*col)[i];
                }
                m_deathEvents[m_deathCount++] = e;
            }
        }

        [[nodiscard]] f32 CalculateLODMultiplier(Float3 cameraPos) const noexcept
        {
            if (lodStartDistance <= 0.0f && lodCullDistance <= 0.0f)
            {
                return 1.0f;
            } // disabled
            const f32 dist = Length(position - cameraPos);
            if (dist <= lodStartDistance)
            {
                return 1.0f;
            }
            if (lodCullDistance > 0.0f && dist >= lodCullDistance)
            {
                return 0.0f;
            }
            if (lodCullDistance <= lodStartDistance)
            {
                return 1.0f;
            }
            const f32 t = (dist - lodStartDistance) / (lodCullDistance - lodStartDistance);
            return Max(1.0f - t * (1.0f - lodMinRate), lodMinRate);
        }

        i32 m_maxParticles;
        Array<ParticleTrailState> m_trailStates; // per-particle (parallel to the streams)
        Array<TrailPoint> m_trailPoints; // flat ring buffers: [particleIndex*maxPoints + slot]
        i32 m_trailCapacityPoints = 0;
        ParticleStreamContainer m_streams;
        Array<RefPtr<ParticleInitializer>> m_initializers;
        Array<RefPtr<ParticleBehavior>> m_behaviors;
        UniquePtr<ParticleSimulator> m_simulator;
        Random m_random;
        f32 m_totalTime = 0.0f;
        f32 m_lodRateMultiplier = 1.0f;
        bool m_prewarmed = false;
        u64 m_seed = 0x9E3779B97F4A7C15ull;
        SimulationMode m_resolvedMode = SimulationMode::CPU;
        ParticleEvent m_deathEvents[kMaxEventsPerFrame]{};
        ParticleEvent m_birthEvents[kMaxEventsPerFrame]{};
        i32 m_deathCount = 0;
        i32 m_birthCount = 0;
    };

    // ---- ParticleEffect (asset definition) ---------------------------------------------------

    class ParticleEffect
    {
    public:
        // Reflection hook (defined in ParticleEffectReflectionImpl.cpp): a member function so it can
        // reach the private systems list (m_systems) without exposing it.
        static void BuildReflection(draconic::foundation::TypeBuilder<ParticleEffect>& builder);

        String name;

        explicit ParticleEffect(StringView effectName = StringView(u8"Effect")) : name(effectName)
        {
        }

        // Create + own a new system, returning a reference to configure it.
        ParticleSystem& AddSystem(i32 maxParticles, u64 seed = 0x9E3779B97F4A7C15ull)
        {
            UniquePtr<ParticleSystem> sys =
                MakeUnique<ParticleSystem>(DefaultAllocator(), maxParticles, seed);
            ParticleSystem& ref = *sys;
            m_systems.PushBack(std::move(sys));
            return ref;
        }

        void AddSubEmitterLink(SubEmitterLink link) { m_links.PushBack(link); }

        // Remove a system by index / drop everything (editor authoring + blob-restore rebuilds).
        void RemoveSystem(i32 index)
        {
            if (index >= 0 && static_cast<usize>(index) < m_systems.Size())
            {
                m_systems.RemoveAt(static_cast<usize>(index));
            }
        }
        void Clear()
        {
            m_systems.Clear();
            m_links.Clear();
        }

        [[nodiscard]] i32 SystemCount() const noexcept
        {
            return static_cast<i32>(m_systems.Size());
        }
        [[nodiscard]] ParticleSystem* GetSystem(i32 index) noexcept
        {
            if (index < 0 || static_cast<usize>(index) >= m_systems.Size())
            {
                return nullptr;
            }
            return m_systems[static_cast<usize>(index)].Get();
        }
        [[nodiscard]] Span<const SubEmitterLink> SubEmitterLinks() const noexcept
        {
            return Span<const SubEmitterLink>{m_links.Data(), m_links.Size()};
        }

    private:
        Array<UniquePtr<ParticleSystem>> m_systems;
        Array<SubEmitterLink> m_links;
    };

    // ---- ParticleEffectInstance (runtime) ----------------------------------------------------

    class ParticleEffectInstance
    {
    public:
        Float3 position{0.0f, 0.0f, 0.0f};
        bool isActive = true;

        explicit ParticleEffectInstance(ParticleEffect& effect) noexcept : m_effect(&effect) {}

        [[nodiscard]] ParticleEffect& Effect() const noexcept { return *m_effect; }

        void Update(f32 deltaTime, Float3 cameraPos = Float3::Zero)
        {
            if (!isActive || m_effect == nullptr)
            {
                return;
            }
            const i32 count = m_effect->SystemCount();
            for (i32 i = 0; i < count; ++i)
            {
                ParticleSystem* sys = m_effect->GetSystem(i);
                sys->position = position;
                sys->Update(deltaTime, cameraPos);
            }
            RouteSubEmitterEvents();
        }

        // True once every system has stopped emitting and drained its live particles.
        [[nodiscard]] bool IsFinished() const noexcept
        {
            if (m_effect == nullptr)
            {
                return true;
            }
            const i32 count = m_effect->SystemCount();
            for (i32 i = 0; i < count; ++i)
            {
                ParticleSystem* sys = m_effect->GetSystem(i);
                if (sys->AliveCount() > 0 || sys->emitter.isEmitting)
                {
                    return false;
                }
            }
            return true;
        }

        void Stop() noexcept
        {
            if (m_effect == nullptr)
            {
                return;
            }
            for (i32 i = 0; i < m_effect->SystemCount(); ++i)
            {
                m_effect->GetSystem(i)->emitter.isEmitting = false;
            }
        }

        // Resume/begin emission on every system (the counterpart to Stop). Also un-pauses the
        // instance (isActive). A fresh instance already emits - this restarts a Stop()ed one.
        void Play() noexcept
        {
            isActive = true;
            if (m_effect == nullptr)
            {
                return;
            }
            for (i32 i = 0; i < m_effect->SystemCount(); ++i)
            {
                m_effect->GetSystem(i)->emitter.isEmitting = true;
            }
        }

        void Reset() noexcept
        {
            if (m_effect == nullptr)
            {
                return;
            }
            for (i32 i = 0; i < m_effect->SystemCount(); ++i)
            {
                m_effect->GetSystem(i)->Reset();
            }
        }

    private:
        void RouteSubEmitterEvents()
        {
            const Span<const SubEmitterLink> links = m_effect->SubEmitterLinks();
            const i32 systemCount = m_effect->SystemCount();
            for (usize li = 0; li < links.Size(); ++li)
            {
                const SubEmitterLink& link = links[li];
                if (link.childSystemIndex < 0 || link.childSystemIndex >= systemCount)
                {
                    continue;
                }
                ParticleSystem* child = m_effect->GetSystem(link.childSystemIndex);
                for (i32 s = 0; s < systemCount; ++s)
                {
                    if (s == link.childSystemIndex)
                    {
                        continue;
                    } // don't route a system into itself
                    ParticleSystem* parent = m_effect->GetSystem(s);
                    const Span<const ParticleEvent> events =
                        (link.trigger == ParticleEventType::OnDeath) ? parent->DeathEvents()
                                                                     : parent->BirthEvents();
                    for (usize e = 0; e < events.Size(); ++e)
                    {
                        const ParticleEvent& evt = events[e];
                        if (link.probability < 1.0f)
                        {
                            // Deterministic spatial-hash gate (matches Sedulous - stable per position).
                            const u32 hash = static_cast<u32>(evt.position.x * 73856093.0f) ^
                                             static_cast<u32>(evt.position.y * 19349663.0f);
                            if (static_cast<f32>(hash % 1000u) / 1000.0f > link.probability)
                            {
                                continue;
                            }
                        }
                        if (link.inheritPosition)
                        {
                            const Float3 vel = link.inheritVelocity
                                                   ? evt.velocity * link.velocityInheritFactor
                                                   : Float3::Zero;
                            const Float4 col =
                                link.inheritColor ? evt.color : Float4{1.0f, 1.0f, 1.0f, 1.0f};
                            child->SpawnAt(link.spawnCount, evt.position, vel, col);
                        }
                        else
                        {
                            child->SpawnImmediate(link.spawnCount);
                        }
                    }
                }
            }
        }

        ParticleEffect* m_effect = nullptr;
    };
}
