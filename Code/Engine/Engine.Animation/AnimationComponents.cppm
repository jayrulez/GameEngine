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


    // What an animator does with the root motion its clips carry (root-motion.md P2).
    enum class RootMotionMode : u8
    {
        Ignore,    // nothing: the character stays where its game puts it (the default)
        Entity,    // the animator's entity moves and turns by it (a non-physics actor)
        Character, // the nearest character at or above it walks by it (it still collides) and turns
        Script,    // held for SceneAnimation.rootMotionTranslation / rootMotionYaw
    };

    // An animator's root motion as of its last tick (Script mode reads it; the others use it).
    struct RootMotionRuntime
    {
        Float3 worldTranslation{}; // the last tick's travel, in the world
        f32 yaw = 0.0f;            // and its turn about up, radians
        bool drivingCharacter = false; // a character's move is ours to zero when we stop
        scene::EntityHandle character = scene::EntityHandle::Invalid();
    };

    namespace root_motion_apply
    {
        [[nodiscard]] inline scene::ISceneCharacterMotion* Mover(scene::Scene& scene)
        {
            scene::ISceneCharacterMotion* found = nullptr;
            scene.ForEachSystem(
                [&](scene::SceneSystem& system)
                {
                    if (found == nullptr)
                    {
                        found = system.AsCharacterMotion();
                    }
                });
            return found;
        }

        /// A character we were walking stops: one zero move, so it does not walk on by our last.
        inline void Release(scene::Scene& scene, RootMotionRuntime& rt)
        {
            if (rt.drivingCharacter && scene.IsValid(rt.character))
            {
                if (scene::ISceneCharacterMotion* mover = Mover(scene))
                {
                    mover->MoveCharacter(rt.character, Float3{});
                }
            }
            rt.drivingCharacter = false;
            rt.character = scene::EntityHandle::Invalid();
        }

        /// The entity whose world is the skeleton's model space (the first mesh it feeds, else its own).
        [[nodiscard]] inline scene::EntityHandle ModelEntity(scene::Scene& scene, Span<const scene::EntityRef> meshes,
                                                             scene::EntityHandle owner)
        {
            for (const scene::EntityRef& r : meshes)
            {
                const scene::EntityHandle e = scene.FindEntity(r.id);
                if (scene.IsValid(e))
                {
                    return e;
                }
            }
            return owner;
        }

        /// Turns `entity` by `yaw` about the model's up (`upWorld`), seen in its own frame.
        inline void Turn(scene::Scene& scene, scene::EntityHandle entity, f32 yaw, Float3 upWorld)
        {
            if (yaw == 0.0f)
            {
                return;
            }
            Float3 axis = TransformDirection(upWorld, Inverse(scene.ComposeWorldMatrix(entity)));
            axis = LengthSquared(axis) > 1.0e-12f ? Normalized(axis) : Float3{0.0f, 1.0f, 0.0f};
            Transform t = scene.GetLocalTransform(entity);
            t.rotation = Normalized(t.rotation * Quaternion::FromAxisAngle(axis, yaw));
            scene.SetLocalTransform(entity, t);
        }

        /// An animator's tick of root motion, by its mode (root-motion.md P2). The delta is in the
        /// skeleton's model space (the first mesh entity's world), so it is carried into the world
        /// through that, whatever lies between the animator and its mesh (an armature at rest).
        inline void Apply(scene::Scene& scene, scene::EntityHandle owner, Span<const scene::EntityRef> meshes,
                          const scene::EntityRef& target, RootMotionMode mode, RootMotionRuntime& rt,
                          const animation::RootMotionDelta& delta, f32 deltaTime)
        {
            const Float4x4 model = scene.ComposeWorldMatrix(ModelEntity(scene, meshes, owner));
            const Float3 upWorld = TransformDirection(Float3{0.0f, 1.0f, 0.0f}, model);
            rt.worldTranslation = TransformDirection(delta.translation, model);
            rt.yaw = delta.yaw;
            if (mode != RootMotionMode::Character)
            {
                Release(scene, rt);
            }
            if (mode == RootMotionMode::Entity)
            {
                // The named entity (a gameplay root holding the model), else the animator's own:
                // moved in its parent's space, turned about the model's up.
                scene::EntityHandle moved = owner;
                if (!target.IsNil())
                {
                    const scene::EntityHandle named = scene.FindEntity(target.id);
                    if (!scene.IsValid(named))
                    {
                        return;
                    }
                    moved = named;
                }
                const scene::EntityHandle parent = scene.GetParent(moved);
                const Float3 local = scene.IsValid(parent)
                                         ? TransformDirection(rt.worldTranslation, Inverse(scene.ComposeWorldMatrix(parent)))
                                         : rt.worldTranslation;
                Transform t = scene.GetLocalTransform(moved);
                t.position = t.position + local;
                scene.SetLocalTransform(moved, t);
                Turn(scene, moved, delta.yaw, upWorld);
                return;
            }
            if (mode != RootMotionMode::Character)
            {
                return;
            }
            scene::ISceneCharacterMotion* mover = Mover(scene);
            scene::EntityHandle e = owner;
            for (u32 depth = 0; mover != nullptr && scene.IsValid(e) && depth < 1024u; ++depth)
            {
                if (mover->HasCharacter(e))
                {
                    break;
                }
                e = scene.GetParent(e);
            }
            if (mover == nullptr || !scene.IsValid(e) || !mover->HasCharacter(e))
            {
                Release(scene, rt);
                return;
            }
            if (rt.drivingCharacter && rt.character != e)
            {
                Release(scene, rt);
            }
            // A velocity for the next fixed step (the controller collides and slides; one step late).
            Float3 velocity = deltaTime > 0.0f ? rt.worldTranslation * (1.0f / deltaTime) : Float3{};
            velocity.y = 0.0f;
            mover->MoveCharacter(e, velocity);
            rt.drivingCharacter = true;
            rt.character = e;
            Turn(scene, e, delta.yaw, upWorld);
        }
    }

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
        RootMotionMode rootMotion = RootMotionMode::Ignore; // v2
        scene::EntityRef rootMotionTarget; // v2: what Entity mode moves (empty: this entity)
        RootMotionRuntime rootMotionState; // runtime
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
        if (ar.Mode() == SerializeMode::Write || ar.Version() >= 2) // v1 records read as Ignore
        {
            foundation::core::Serialize(ar, "rootMotion", c.rootMotion);
            foundation::core::Serialize(ar, "rootMotionTarget", c.rootMotionTarget);
        }
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

    protected:
        // A character this animator was walking stops with it.
        void OnComponentDestroyed(SkeletalAnimationComponent& c, scene::EntityHandle) override
        {
            if (m_scene != nullptr)
            {
                root_motion_apply::Release(*m_scene, c.rootMotionState);
            }
        }

    public:
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
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
                        return; // frozen: time does not advance
                    }
                    animation::Skeleton* skeleton = a.skeleton.Get();
                    if (skeleton == nullptr)
                    {
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
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
                    root_motion_apply::Apply(
                        *m_scene, owner, Span<const scene::EntityRef>{a.meshEntities.Data(), a.meshEntities.Size()},
                        a.rootMotionTarget, a.rootMotion, a.rootMotionState, a.player->ConsumeRootMotion(), deltaTime);
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
        RootMotionMode rootMotion = RootMotionMode::Ignore; // v2
        scene::EntityRef rootMotionTarget; // v2: what Entity mode moves (empty: this entity)
        RootMotionRuntime rootMotionState; // runtime
    };

    inline void Serialize(ISerializer& ar, AnimationGraphComponent& c)
    {
        foundation::core::Serialize(ar, "skeleton", c.skeleton);
        foundation::core::Serialize(ar, "graph", c.graph);
        foundation::core::Serialize(ar, "active", c.active);
        foundation::core::Serialize(ar, "meshEntities", c.meshEntities);
        if (ar.Mode() == SerializeMode::Write || ar.Version() >= 2) // v1 records read as Ignore
        {
            foundation::core::Serialize(ar, "rootMotion", c.rootMotion);
            foundation::core::Serialize(ar, "rootMotionTarget", c.rootMotionTarget);
        }
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

    protected:
        void OnComponentDestroyed(AnimationGraphComponent& c, scene::EntityHandle) override
        {
            if (m_scene != nullptr)
            {
                root_motion_apply::Release(*m_scene, c.rootMotionState);
            }
        }

    public:
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
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
                        return; // frozen
                    }
                    animation::Skeleton* skeleton = a.skeleton.Get();
                    animation::AnimationGraph* graph = a.graph.Get();
                    if (skeleton == nullptr || graph == nullptr)
                    {
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
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
                        root_motion_apply::Release(*m_scene, a.rootMotionState);
                        return;
                    }
                    a.player->Update(deltaTime);
                    root_motion_apply::Apply(
                        *m_scene, owner, Span<const scene::EntityRef>{a.meshEntities.Data(), a.meshEntities.Size()},
                        a.rootMotionTarget, a.rootMotion, a.rootMotionState, a.player->ConsumeRootMotion(), deltaTime);
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

        // --- root motion (Script mode) ---
        // The entity's animator's travel over its last tick, in the world, and its turn (radians
        // about up): a script steering by the clip's own speed (an animator in Script mode moves
        // nothing itself). Zero for an entity with no animator.
        [[nodiscard]] Float3 rootMotionTranslation(foundation::script::Entity entity) const
        {
            const RootMotionRuntime* rt = RootMotionOf(entity);
            return rt != nullptr ? rt->worldTranslation : Float3{};
        }
        [[nodiscard]] f32 rootMotionYaw(foundation::script::Entity entity) const
        {
            const RootMotionRuntime* rt = RootMotionOf(entity);
            return rt != nullptr ? rt->yaw : 0.0f;
        }

        [[nodiscard]] static SceneAnimation of(foundation::script::Scene sceneHandle)
        {
            return SceneAnimation{sceneHandle.scene};
        }

    private:
        [[nodiscard]] const RootMotionRuntime* RootMotionOf(foundation::script::Entity entity) const
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            if (auto* graphs = scene->GetSystem<AnimationGraphComponentManager>())
            {
                if (const AnimationGraphComponent* g = graphs->Get(entity.Handle()))
                {
                    return &g->rootMotionState;
                }
            }
            const SkeletalAnimationComponent* c = Skeletal(entity);
            return c != nullptr ? &c->rootMotionState : nullptr;
        }
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
