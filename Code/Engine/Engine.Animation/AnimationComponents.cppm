// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Animation - the `:components` partition.
///
/// The scene-facing side of skeletal animation. Two components, each with a manager that ticks its
/// players every frame and feeds the resulting skinning matrices into the target MeshComponent(s)
/// for GPU skinning: SkeletalAnimationComponent (a single clip via AnimationPlayer) and
/// AnimationGraphComponent (a state machine / blend trees via AnimationGraphPlayer). This is what
/// replaces driving players by hand in app code - the engine now animates skinned meshes from the
/// scene tick.
///
/// It sits at the animation<->render seam (depends on both foundation.animation and the render
/// components); neither of those depends back on it.

module;
#include "Core/Prelude.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (compiles to nothing when disabled)

export module engine.animation:components;

import foundation.core;
import foundation.profiler;
import foundation.resource;
import foundation.scene;
import foundation.animation; // Skeleton, AnimationClip, AnimationPlayer, AnimationGraph(+Player)
import engine.render; // MeshComponentManager / MeshComponent (the feed target)
import foundation.script.facades; // script::Entity/Scene + CurrentRunResources (the SceneAnimation handle)

using namespace foundation::core;

export namespace engine::animation
{
    // Foundation aliases (sibling engine::* namespaces would otherwise shadow these).
    namespace scene = foundation::scene;
    namespace animation = foundation::animation;
    namespace script = foundation::script;
    using namespace foundation::animation; // bare foundation animation types (BoneTransform, ...)


    // Skeletal animation on an entity: a player over a (borrowed, shared) skeleton plays a clip and
    // produces per-bone skinning matrices each frame. The manager owns the player's lifetime + tick.
    // `meshEntities` are the entities whose MeshComponent receives the matrices (a character's skinned
    // mesh nodes); empty => feed the component's own entity. All borrowed resources must outlive the
    // component (the resource manager / model keeps the skeleton + clip alive).
    struct SkeletalAnimationComponent
    {
        // Resource refs: Guid-serialized + proxy-resolved (editor pickers/scene round-trip), or
        // direct runtime objects (samples/spawn code). The manager rebuilds the player when the
        // skeleton object changes (a pick or a hot reload).
        foundation::resource::Ref<animation::Skeleton> skeleton;
        foundation::resource::Ref<animation::AnimationClip> clip;
        UniquePtr<animation::AnimationPlayer> player;   // created lazily by the manager
        animation::Skeleton* playerSkeleton = nullptr;  // the skeleton the player was built for
        animation::AnimationClip* playerClip = nullptr; // the clip last handed to the player
        // Feed targets by stable guid (empty => own entity): serializable and prefab-remapped,
        // so ONE animator can drive a multi-part character's skinned mesh nodes.
        Array<scene::EntityRef> meshEntities;
        f32 speed = 1.0f;
        f32 startTime = 0.0f; // initial clock (desync a herd); applied on first tick
        bool autoPlay = true; // Play(clip) on first tick
    };

    // Persist the refs + tunables; the player and per-frame feed state are runtime-only.
    inline void Serialize(ISerializer& ar, SkeletalAnimationComponent& c)
    {
        foundation::core::Serialize(ar, "skeleton", c.skeleton);
        foundation::core::Serialize(ar, "clip", c.clip);
        foundation::core::Serialize(ar, "speed", c.speed);
        foundation::core::Serialize(ar, "startTime", c.startTime);
        foundation::core::Serialize(ar, "autoPlay", c.autoPlay);
        foundation::core::Serialize(ar, "meshEntities", c.meshEntities);
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager,
                                 SkeletalAnimationComponent& c)
    {
        c.skeleton.Bind(manager);
        c.clip.Bind(manager);
    }

    // Ticks every SkeletalAnimationComponent in ScenePhase::PostUpdate (the "animation" phase, before
    // render extraction): advance each player, then write its current + previous skinning matrices into
    // the target MeshComponent(s) (borrowed for the frame - the player, owned by the component, keeps
    // the matrix storage alive). Lazily creates each component's player on first tick.
    class SkeletalAnimationComponentManager final
        : public scene::SerializableComponentManager<SkeletalAnimationComponent>
    {
    public:
        SkeletalAnimationComponentManager()
            : scene::SerializableComponentManager<SkeletalAnimationComponent>(
                  u8"skeletal_animation")
        {
        }

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }

        // SIMULATION-GATED (user ruling 2026-08-18): animation is gameplay-side state and must
        // not advance in a non-simulating scene - watching things animate in the editor's edit
        // mode was distracting and wrong. Consumers that want live animation in a paused-looking
        // context (the bespoke preview pages) enable simulation on their PRIVATE preview scene
        // (PreviewViewport::SetSimulationEnabled) - scenes default to simulating, so players and
        // headless tests are unaffected.
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>();
            if (meshes == nullptr)
            {
                return;
            }

            PROFILE_SCOPE("Animation.Skeletal");
            ForEach(
                [&](SkeletalAnimationComponent& a, scene::EntityHandle owner)
                {
                    if (!m_scene->IsEffectivelyActive(owner))
                    {
                        return; // frozen: time does not advance
                    }
                    animation::Skeleton* skeleton = a.skeleton.Get();
                    if (skeleton == nullptr)
                    {
                        return;
                    }
                    // (Re)build the player when the skeleton object changed - first tick, an editor
                    // pick, or a hot reload swapping the product behind the ref.
                    if (a.player.Get() == nullptr || a.playerSkeleton != skeleton)
                    {
                        a.player =
                            MakeUnique<animation::AnimationPlayer>(DefaultAllocator(), *skeleton);
                        a.playerSkeleton = skeleton;
                        a.playerClip = nullptr; // (re)play below - the new player has no clip yet
                    }
                    // React to the CLIP changing independently of the skeleton (editor picks land one at
                    // a time; a hot reload swaps the product behind the ref mid-play). autoPlay starts the
                    // new clip; manual users drive a.player->Play themselves.
                    animation::AnimationClip* clip = a.clip.Get();
                    if (clip != a.playerClip)
                    {
                        a.playerClip = clip;
                        if (a.autoPlay && clip != nullptr)
                        {
                            a.player->Play(clip);
                            if (a.startTime != 0.0f)
                            {
                                a.player->SetCurrentTime(a.startTime);
                            }
                        }
                    }
                    a.player->speed = a.speed;
                    a.player->Update(deltaTime);
                    const Span<const Float4x4> mats = a.player->GetSkinningMatrices();
                    const Span<const Float4x4> prev = a.player->GetPrevSkinningMatrices();
                    const auto feed = [&](scene::EntityHandle e)
                    {
                        if (engine::render::MeshComponent* mc = meshes->Get(e))
                        {
                            mc->boneMatrices = mats.Data();
                            mc->prevBoneMatrices = prev.Data();
                            mc->boneCount = static_cast<u32>(mats.Size());
                        }
                    };
                    if (a.meshEntities.IsEmpty())
                    {
                        feed(owner);
                    }
                    else
                    {
                        for (const scene::EntityRef& r : a.meshEntities)
                        {
                            feed(m_scene->FindEntity(r.id)); // guid -> live handle each frame
                        }
                    }
                });
        }

    private:
        scene::Scene* m_scene = nullptr;
    };

    // State-machine-driven skeletal animation: a graph player (over a borrowed, shared skeleton +
    // AnimationGraph) evaluates the graph each frame - state transitions, blend trees, layer blending -
    // and produces per-bone skinning matrices. The richer counterpart to SkeletalAnimationComponent
    // (single clip); drive transitions via the player's parameters (SetFloat/SetBool/SetTrigger). Same
    // feed contract: `meshEntities` are the MeshComponents that receive the matrices (empty => own
    // entity). All borrowed resources must outlive the component.
    struct AnimationGraphComponent
    {
        foundation::resource::Ref<animation::Skeleton> skeleton;
        foundation::resource::Ref<animation::AnimationGraph> graph;
        UniquePtr<animation::AnimationGraphPlayer> player; // created lazily by the manager
        animation::Skeleton* playerSkeleton = nullptr;     // what the player was built for
        animation::AnimationGraph* playerGraph = nullptr;
        Array<scene::EntityRef> meshEntities; // feed targets by stable guid (empty => own entity)
        bool active = true;                   // evaluate this frame?
    };

    inline void Serialize(ISerializer& ar, AnimationGraphComponent& c)
    {
        foundation::core::Serialize(ar, "skeleton", c.skeleton);
        foundation::core::Serialize(ar, "graph", c.graph);
        foundation::core::Serialize(ar, "active", c.active);
        foundation::core::Serialize(ar, "meshEntities", c.meshEntities);
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager,
                                 AnimationGraphComponent& c)
    {
        c.skeleton.Bind(manager);
        c.graph.Bind(manager);
    }

    // Ticks every AnimationGraphComponent in ScenePhase::PostUpdate, same as the skeletal manager but
    // evaluating an AnimationGraphPlayer. Runs at a LOWER UpdateOrder (before SkeletalAnimationComponent-
    // Manager), mirroring Sedulous's graph-before-clip ordering; an entity is expected to use one or the
    // other (mixing both pushes to the same MeshComponent - the later writer wins).
    class AnimationGraphComponentManager final
        : public scene::SerializableComponentManager<AnimationGraphComponent>
    {
    public:
        AnimationGraphComponentManager()
            : scene::SerializableComponentManager<AnimationGraphComponent>(u8"animation_graph")
        {
        }

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }

        // Simulation-gated like the clip manager (see its comment).
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }

        // Run before the simple-clip manager (UpdateOrder 0) so the graph drives graph-backed entities.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -1; }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>();
            if (meshes == nullptr)
            {
                return;
            }

            PROFILE_SCOPE("Animation.Graph");
            ForEach(
                [&](AnimationGraphComponent& a, scene::EntityHandle owner)
                {
                    if (!m_scene->IsEffectivelyActive(owner))
                    {
                        return; // frozen
                    }
                    animation::Skeleton* skeleton = a.skeleton.Get();
                    animation::AnimationGraph* graph = a.graph.Get();
                    if (skeleton == nullptr || graph == nullptr)
                    {
                        return;
                    }
                    // (Re)build the player when either object changed - first tick, a pick, a reload.
                    if (a.player.Get() == nullptr || a.playerSkeleton != skeleton ||
                        a.playerGraph != graph)
                    {
                        a.player = MakeUnique<animation::AnimationGraphPlayer>(DefaultAllocator(),
                                                                               *graph, *skeleton);
                        a.playerSkeleton = skeleton;
                        a.playerGraph = graph;
                    }
                    if (!a.active)
                    {
                        return;
                    }
                    a.player->Update(deltaTime);
                    const Span<const Float4x4> mats = a.player->GetSkinningMatrices();
                    const Span<const Float4x4> prev = a.player->GetPrevSkinningMatrices();
                    const auto feed = [&](scene::EntityHandle e)
                    {
                        if (engine::render::MeshComponent* mc = meshes->Get(e))
                        {
                            mc->boneMatrices = mats.Data();
                            mc->prevBoneMatrices = prev.Data();
                            mc->boneCount = static_cast<u32>(mats.Size());
                        }
                    };
                    if (a.meshEntities.IsEmpty())
                    {
                        feed(owner);
                    }
                    else
                    {
                        for (const scene::EntityRef& r : a.meshEntities)
                        {
                            feed(m_scene->FindEntity(r.id)); // guid -> live handle each frame
                        }
                    }
                });
        }

    private:
        scene::Scene* m_scene = nullptr;
    };

    // Instanced skinning for CROWDS: the companion to a engine::render::InstancedMeshComponent (a "MultiMesh") that
    // makes its N instances animate at only M = poseCount unique phases. Each frame the manager samples the
    // clip at M evenly-spaced phases (advancing together on a shared clock) into a shared POSE POOL of M
    // skinning palettes, and feeds the pool to the target InstancedMeshComponent - which draws instance i
    // with pose (i % M). So a 30k crowd costs M palette computes, not 30k. Put it on the same entity as the
    // InstancedMeshComponent (empty target) or point `target` at it. Borrowed skeleton/clip must outlive it.
    struct InstancedSkinningComponent
    {
        animation::Skeleton* skeleton = nullptr;  // borrowed; shared across the crowd
        animation::AnimationClip* clip = nullptr; // borrowed; the clip the crowd plays
        u32 poseCount = 32; // M unique phase buckets (more = smoother spread, more compute)
        f32 speed = 1.0f;
        Array<scene::EntityHandle> targets; // InstancedMeshComponent entities to feed (a multi-part
        // character = one set per skinned mesh); empty => own entity

        // Manager-owned per-frame state (not authored).
        Array<Float4x4> posePool; // poseCount * boneCount skinning matrices, recomputed each frame
        Array<Float4x4>
            prevPosePool; // LAST frame's palettes (per-bone motion vectors); ping-ponged, not recomputed
        Array<BoneTransform> scratch; // boneCount scratch for SampleClip
        f32 time = 0.0f;              // shared clock (wrapped to clip duration)
        u32 boneCount = 0;
    };

    // Ticks every InstancedSkinningComponent in PostUpdate (before render extraction): advance the shared clock, sample
    // the clip at M phases into the pose pool, and hand the pool to the target InstancedMeshComponent.
    class InstancedSkinningComponentManager final : public scene::ComponentManager<InstancedSkinningComponent>
    {
    public:
        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }
        // Simulation-gated like the clip manager (see its comment).
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            auto* imm = m_scene->GetSystem<engine::render::InstancedMeshComponentManager>();
            if (imm == nullptr)
            {
                return;
            }
            PROFILE_SCOPE("Animation.InstancedSkinning");

            ForEach(
                [&](InstancedSkinningComponent& s, scene::EntityHandle owner)
                {
                    if (!m_scene->IsEffectivelyActive(owner))
                    {
                        return; // frozen
                    }
                    if (s.skeleton == nullptr || s.clip == nullptr || s.poseCount == 0)
                    {
                        return;
                    }
                    const u32 boneCount = static_cast<u32>(s.skeleton->BoneCount());
                    if (boneCount == 0)
                    {
                        return;
                    }
                    s.boneCount = boneCount;
                    const usize poolSize = static_cast<usize>(s.poseCount) * boneCount;

                    // Ping-pong: last frame's pool becomes this frame's PREV (per-bone motion vectors) - no re-sampling.
                    {
                        Array<Float4x4> tmp = static_cast<Array<Float4x4>&&>(s.posePool);
                        s.posePool = static_cast<Array<Float4x4>&&>(s.prevPosePool);
                        s.prevPosePool = static_cast<Array<Float4x4>&&>(tmp);
                    }
                    s.posePool.Resize(poolSize);
                    s.scratch.Resize(boneCount);

                    const f32 duration = (s.clip->duration > 0.0f) ? s.clip->duration : 1.0f;
                    s.time += deltaTime * s.speed;
                    while (s.time >= duration)
                    {
                        s.time -= duration;
                    }
                    while (s.time < 0.0f)
                    {
                        s.time += duration;
                    }

                    // M palettes at M evenly-spaced phases (the whole crowd cycles through the clip together).
                    for (u32 m = 0; m < s.poseCount; ++m)
                    {
                        f32 p = s.time +
                                (static_cast<f32>(m) / static_cast<f32>(s.poseCount)) * duration;
                        while (p >= duration)
                        {
                            p -= duration;
                        }
                        SampleClip(*s.clip, *s.skeleton, p,
                                   Span<BoneTransform>{s.scratch.Data(), s.scratch.Size()});
                        s.skeleton->ComputeSkinningMatrices(
                            Span<const BoneTransform>{s.scratch.Data(), s.scratch.Size()},
                            Span<Float4x4>{s.posePool.Data() + static_cast<usize>(m) * boneCount,
                                           boneCount});
                    }
                    // First frame (or pose-count change): no prev yet -> prev = current (zero motion).
                    if (s.prevPosePool.Size() != poolSize)
                    {
                        s.prevPosePool.Resize(poolSize);
                        if (poolSize > 0)
                        {
                            MemCopy(s.prevPosePool.Data(), s.posePool.Data(),
                                    poolSize * sizeof(Float4x4));
                        }
                    }

                    const auto feed = [&](scene::EntityHandle e)
                    {
                        if (engine::render::InstancedMeshComponent* c = imm->Get(e))
                        {
                            c->posePool =
                                s.posePool
                                    .Data(); // borrowed for the frame (the component keeps the storage alive)
                            c->prevPosePool =
                                s.prevPosePool
                                    .Data(); // last frame's palettes (per-bone motion vectors)
                            c->poseCount = s.poseCount;
                            c->boneCount = boneCount;
                        }
                    };
                    if (s.targets.IsEmpty())
                    {
                        feed(owner);
                    }
                    else
                    {
                        for (scene::EntityHandle e : s.targets)
                        {
                            feed(e);
                        }
                    }
                });
        }

    private:
        scene::Scene* m_scene = nullptr;
    };

    // A scene-bound ANIMATION handle (SceneAnimation.of(scene)): runtime WORLD ops on the animation
    // components that need the manager-owned runtime player (which the component data cannot reach) -
    // play/stop/pause a single-clip player, drive a graph's parameters, swap the clip by resource id.
    // Keyed by entity, mirroring ScenePhysics / SceneRender / SceneAudio (component = auto-reflected
    // DATA: speed/autoPlay/active; scene-handle = world ops). The players are built lazily by the
    // managers in PostUpdate, so control from a behavior's onUpdate sees them; a call before the first
    // animation tick (e.g. onStart) is a safe no-op.
    struct SceneAnimation
    {
        scene::Scene* scene = nullptr;

        // --- single-clip playback (SkeletalAnimationComponent) ---
        // Play the entity's currently-bound clip from the start (manual re-trigger; autoPlay covers
        // the first start). No-op if the player is not built yet or the entity has no skeletal anim.
        void play(foundation::script::Entity entity) const
        {
            SkeletalAnimationComponent* c = Skeletal(entity);
            if (c != nullptr && c->player.Get() != nullptr)
            {
                c->player->Play(c->clip.Get());
            }
        }
        void stop(foundation::script::Entity entity) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->Stop();
            }
        }
        void pause(foundation::script::Entity entity) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->Pause();
            }
        }
        void resume(foundation::script::Entity entity) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->Resume();
            }
        }
        [[nodiscard]] bool isPlaying(foundation::script::Entity entity) const
        {
            animation::AnimationPlayer* p = SkeletalPlayer(entity);
            return p != nullptr && p->State() == animation::PlaybackState::Playing;
        }
        [[nodiscard]] f32 time(foundation::script::Entity entity) const
        {
            animation::AnimationPlayer* p = SkeletalPlayer(entity);
            return p != nullptr ? p->CurrentTime() : 0.0f;
        }
        void setTime(foundation::script::Entity entity, f32 seconds) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->SetCurrentTime(seconds);
            }
        }
        // Swap the entity's animation clip to resource `id`, binding it through the run's resource
        // manager; the manager picks up the change next tick (autoPlay replays it).
        void setClip(foundation::script::Entity entity, Guid id) const
        {
            if (SkeletalAnimationComponent* c = Skeletal(entity))
            {
                c->clip.SetId(id);
                if (auto* resources = foundation::script::CurrentRunResources())
                {
                    c->clip.Bind(*resources);
                }
            }
        }

        // --- state-machine parameters (AnimationGraphComponent) ---
        void setFloat(foundation::script::Entity entity, String name, f32 value) const
        {
            if (animation::AnimationGraphPlayer* p = GraphPlayer(entity))
            {
                p->SetFloat(name.AsView(), value);
            }
        }
        void setBool(foundation::script::Entity entity, String name, bool value) const
        {
            if (animation::AnimationGraphPlayer* p = GraphPlayer(entity))
            {
                p->SetBool(name.AsView(), value);
            }
        }
        void setTrigger(foundation::script::Entity entity, String name) const
        {
            if (animation::AnimationGraphPlayer* p = GraphPlayer(entity))
            {
                p->SetTrigger(name.AsView());
            }
        }

        // --- inverse kinematics (TwoBoneIkComponent / AimIkComponent on the entity) ---
        // The world point the entity's IK components reach for while they name no target entity.
        // Defined in the implementation unit (this partition cannot see :ik).
        void setIkTarget(foundation::script::Entity entity, Float3 worldPosition) const;
        // Whether every IK component on the entity reached its target on the last solve, and the
        // largest miss (metres for a two-bone chain, radians for an aim).
        [[nodiscard]] bool ikReached(foundation::script::Entity entity) const;
        [[nodiscard]] f32 ikError(foundation::script::Entity entity) const;

        [[nodiscard]] static SceneAnimation of(foundation::script::Scene sceneHandle)
        {
            return SceneAnimation{sceneHandle.scene};
        }

    private:
        [[nodiscard]] SkeletalAnimationComponent* Skeletal(foundation::script::Entity entity) const
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            auto* manager = scene->GetSystem<SkeletalAnimationComponentManager>();
            return (manager != nullptr) ? manager->Get(entity.Handle()) : nullptr;
        }
        [[nodiscard]] animation::AnimationPlayer* SkeletalPlayer(foundation::script::Entity e) const
        {
            SkeletalAnimationComponent* c = Skeletal(e);
            return (c != nullptr) ? c->player.Get() : nullptr;
        }
        [[nodiscard]] animation::AnimationGraphPlayer* GraphPlayer(foundation::script::Entity e) const
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            auto* manager = scene->GetSystem<AnimationGraphComponentManager>();
            AnimationGraphComponent* c = (manager != nullptr) ? manager->Get(e.Handle()) : nullptr;
            return (c != nullptr) ? c->player.Get() : nullptr;
        }
    };

} // exported namespace

// Reflection (tooling: the editor inspector). The REFLECT_VALUE bodies +
// RegisterAnimationComponentReflection() live in AnimationSubsystemImpl.cpp, kept out of this
// interface partition (see gcc-module-interface-hygiene).
export namespace engine::animation
{
    void RegisterAnimationComponentReflection();

    // Surfaces the animation components to SCRIPT (Track A): SkeletalAnimationComponent.of(entity) /
    // AnimationGraphComponent.of(entity) for the DATA (speed/autoPlay/active), plus SceneAnimation.of(
    // scene) for the world ops (play/stop, graph params, clip swap). Registers + seeds + names them so
    // both backends bind. Called by the composition root (like RegisterRenderScriptFacade).
    void RegisterAnimationScriptFacade();
}
