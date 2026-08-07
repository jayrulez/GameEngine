/// Draconic::AnimationSubsystem - the `:components` partition.
///
/// The scene-facing side of skeletal animation. Two components, each with a manager that ticks its
/// players every frame and feeds the resulting skinning matrices into the target MeshComponent(s)
/// for GPU skinning: SkeletalAnimationComponent (a single clip via AnimationPlayer) and
/// AnimationGraphComponent (a state machine / blend trees via AnimationGraphPlayer). This is what
/// replaces driving players by hand in app code - the engine now animates skinned meshes from the
/// scene tick.
///
/// It sits at the animation<->render seam (depends on both draconic.animation and the render
/// components); neither of those depends back on it.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.engine.animation:components;

import draconic.foundation;
import draconic.resource;
import draconic.scene;
import draconic.animation; // Skeleton, AnimationClip, AnimationPlayer, AnimationGraph(+Player)
import draconic.engine.render; // MeshComponentManager / MeshComponent (the feed target)
import draconic.script.facades; // script::Entity/Scene + CurrentRunResources (the SceneAnimation handle)

using namespace draconic::foundation;
namespace scene = draconic::scene;
namespace animation = draconic::animation;

export namespace draconic::animation
{

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
        draconic::resource::Ref<animation::Skeleton> skeleton;
        draconic::resource::Ref<animation::AnimationClip> clip;
        UniquePtr<animation::AnimationPlayer> player;   // created lazily by the manager
        animation::Skeleton* playerSkeleton = nullptr;  // the skeleton the player was built for
        animation::AnimationClip* playerClip = nullptr; // the clip last handed to the player
        Array<scene::EntityHandle> meshEntities;        // feed targets (empty => own entity)
        f32 speed = 1.0f;
        f32 startTime = 0.0f; // initial clock (desync a herd); applied on first tick
        bool autoPlay = true; // Play(clip) on first tick
    };

    // Persist the refs + tunables; the player and per-frame feed state are runtime-only.
    // (meshEntities are transient handles - the model-spawn workflow re-wires them; they'll persist
    // once entity-reference serialization exists.)
    inline void Serialize(ISerializer& ar, SkeletalAnimationComponent& c)
    {
        draconic::foundation::Serialize(ar, "skeleton", c.skeleton);
        draconic::foundation::Serialize(ar, "clip", c.clip);
        draconic::foundation::Serialize(ar, "speed", c.speed);
        draconic::foundation::Serialize(ar, "startTime", c.startTime);
        draconic::foundation::Serialize(ar, "autoPlay", c.autoPlay);
    }

    inline void ResolveResources(draconic::resource::ResourceManager& manager,
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

        // Animation is gameplay-side state; only advance it while the scene is simulating? Keep it
        // always-on for now so apps animate without an explicit Start() (revisit with edit-mode).
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return false; }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            auto* meshes = m_scene->GetSystem<render::MeshComponentManager>();
            if (meshes == nullptr)
            {
                return;
            }

            ForEach(
                [&](SkeletalAnimationComponent& a, scene::EntityHandle owner)
                {
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
                        if (render::MeshComponent* mc = meshes->Get(e))
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
                        for (scene::EntityHandle e : a.meshEntities)
                        {
                            feed(e);
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
        draconic::resource::Ref<animation::Skeleton> skeleton;
        draconic::resource::Ref<animation::AnimationGraph> graph;
        UniquePtr<animation::AnimationGraphPlayer> player; // created lazily by the manager
        animation::Skeleton* playerSkeleton = nullptr;     // what the player was built for
        animation::AnimationGraph* playerGraph = nullptr;
        Array<scene::EntityHandle> meshEntities; // feed targets (empty => own entity)
        bool active = true;                      // evaluate this frame?
    };

    inline void Serialize(ISerializer& ar, AnimationGraphComponent& c)
    {
        draconic::foundation::Serialize(ar, "skeleton", c.skeleton);
        draconic::foundation::Serialize(ar, "graph", c.graph);
        draconic::foundation::Serialize(ar, "active", c.active);
    }

    inline void ResolveResources(draconic::resource::ResourceManager& manager,
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

        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return false; }

        // Run before the simple-clip manager (UpdateOrder 0) so the graph drives graph-backed entities.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -1; }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            auto* meshes = m_scene->GetSystem<render::MeshComponentManager>();
            if (meshes == nullptr)
            {
                return;
            }

            ForEach(
                [&](AnimationGraphComponent& a, scene::EntityHandle owner)
                {
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
                        if (render::MeshComponent* mc = meshes->Get(e))
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
                        for (scene::EntityHandle e : a.meshEntities)
                        {
                            feed(e);
                        }
                    }
                });
        }

    private:
        scene::Scene* m_scene = nullptr;
    };

    // Instanced skinning for CROWDS: the companion to a render::InstancedMeshComponent (a "MultiMesh") that
    // makes its N instances animate at only M = poseCount unique phases. Each frame the manager samples the
    // clip at M evenly-spaced phases (advancing together on a shared clock) into a shared POSE POOL of M
    // skinning palettes, and feeds the pool to the target InstancedMeshComponent - which draws instance i
    // with pose (i % M). So a 30k crowd costs M palette computes, not 30k. Put it on the same entity as the
    // InstancedMeshComponent (empty target) or point `target` at it. Borrowed skeleton/clip must outlive it.
    // See docs/design/instanced-mesh.md SS7.
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
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return false; }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            auto* imm = m_scene->GetSystem<render::InstancedMeshComponentManager>();
            if (imm == nullptr)
            {
                return;
            }

            ForEach(
                [&](InstancedSkinningComponent& s, scene::EntityHandle owner)
                {
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
                        if (render::InstancedMeshComponent* c = imm->Get(e))
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
        void play(draconic::script::Entity entity) const
        {
            SkeletalAnimationComponent* c = Skeletal(entity);
            if (c != nullptr && c->player.Get() != nullptr)
            {
                c->player->Play(c->clip.Get());
            }
        }
        void stop(draconic::script::Entity entity) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->Stop();
            }
        }
        void pause(draconic::script::Entity entity) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->Pause();
            }
        }
        void resume(draconic::script::Entity entity) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->Resume();
            }
        }
        [[nodiscard]] bool isPlaying(draconic::script::Entity entity) const
        {
            animation::AnimationPlayer* p = SkeletalPlayer(entity);
            return p != nullptr && p->State() == animation::PlaybackState::Playing;
        }
        [[nodiscard]] f32 time(draconic::script::Entity entity) const
        {
            animation::AnimationPlayer* p = SkeletalPlayer(entity);
            return p != nullptr ? p->CurrentTime() : 0.0f;
        }
        void setTime(draconic::script::Entity entity, f32 seconds) const
        {
            if (animation::AnimationPlayer* p = SkeletalPlayer(entity))
            {
                p->SetCurrentTime(seconds);
            }
        }
        // Swap the entity's animation clip to resource `id`, binding it through the run's resource
        // manager; the manager picks up the change next tick (autoPlay replays it).
        void setClip(draconic::script::Entity entity, Guid id) const
        {
            if (SkeletalAnimationComponent* c = Skeletal(entity))
            {
                c->clip.SetId(id);
                if (auto* resources = draconic::script::CurrentRunResources())
                {
                    c->clip.Bind(*resources);
                }
            }
        }

        // --- state-machine parameters (AnimationGraphComponent) ---
        void setFloat(draconic::script::Entity entity, String name, f32 value) const
        {
            if (animation::AnimationGraphPlayer* p = GraphPlayer(entity))
            {
                p->SetFloat(name.AsView(), value);
            }
        }
        void setBool(draconic::script::Entity entity, String name, bool value) const
        {
            if (animation::AnimationGraphPlayer* p = GraphPlayer(entity))
            {
                p->SetBool(name.AsView(), value);
            }
        }
        void setTrigger(draconic::script::Entity entity, String name) const
        {
            if (animation::AnimationGraphPlayer* p = GraphPlayer(entity))
            {
                p->SetTrigger(name.AsView());
            }
        }

        [[nodiscard]] static SceneAnimation of(draconic::script::Scene sceneHandle)
        {
            return SceneAnimation{sceneHandle.scene};
        }

    private:
        [[nodiscard]] SkeletalAnimationComponent* Skeletal(draconic::script::Entity entity) const
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            auto* manager = scene->GetSystem<SkeletalAnimationComponentManager>();
            return (manager != nullptr) ? manager->Get(entity.Handle()) : nullptr;
        }
        [[nodiscard]] animation::AnimationPlayer* SkeletalPlayer(draconic::script::Entity e) const
        {
            SkeletalAnimationComponent* c = Skeletal(e);
            return (c != nullptr) ? c->player.Get() : nullptr;
        }
        [[nodiscard]] animation::AnimationGraphPlayer* GraphPlayer(draconic::script::Entity e) const
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

// Reflection (tooling: the editor inspector). The DRACONIC_REFLECT_VALUE bodies +
// RegisterAnimationComponentReflection() live in AnimationSubsystemImpl.cpp, kept out of this
// interface partition (see gcc-module-interface-hygiene).
export namespace draconic::animation
{
    void RegisterAnimationComponentReflection();

    // Surfaces the animation components to SCRIPT (Track A): SkeletalAnimationComponent.of(entity) /
    // AnimationGraphComponent.of(entity) for the DATA (speed/autoPlay/active), plus SceneAnimation.of(
    // scene) for the world ops (play/stop, graph params, clip swap). Registers + seeds + names them so
    // both backends bind. Called by the composition root (like RegisterRenderScriptFacade).
    void RegisterAnimationScriptFacade();
}
