// draconic.engine.particles:components - the ECS bridge between draconic.particles (the CPU
// sim) and the renderer. Ported in spirit from Sedulous's ParticleComponent(Manager).
//
//   ParticleEffectComponent        - attaches a ParticleEffect to an entity; owns its runtime instance.
//   ParticleEffectComponentManager - a SceneSystem that (a) ticks every instance in PostUpdate
//                                    (scene-driven sim) and (b) packs live billboards into
//                                    ParticleBillboardRenderData during render extraction (the
//                                    IRenderDataProvider role - called by the ParticleSubsystem).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include "Draconic.Profiler/Profiler.h" // DRACONIC_PROFILE_SCOPE (compiles to nothing when disabled)
#include <algorithm>           // std::sort (per-particle back-to-front ordering)

export module draconic.engine.particles:components;

import draconic.foundation;
import draconic.profiler;
import draconic.rhi;                // TextureView (billboard texture)
import draconic.scene;              // Scene, ComponentManager, EntityHandle, ScenePhase
import draconic.render;             // ExtractedScene, RenderCategories, MultiMeshRenderData
import draconic.geometry;           // StaticMesh (mesh-mode particles)
import draconic.materials;          // Material (mesh-mode particles)
import draconic.particles;          // ParticleEffect / ParticleEffectInstance / ParticleSystem
import draconic.resource;           // Proxy (cooked-resource handle)
import draconic.particles.resource; // ParticleEffectResource + CloneEffect (cooked-effect path)
import draconic.texture;            // Texture::View() for resolved per-system textures
import draconic.texture.resource;
import draconic.script.facades; // script::Entity/Scene + CurrentRunResources (the SceneParticles handle)
import :renderdata;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;
namespace scene = draconic::scene;
namespace render = draconic::render;
namespace geometry = draconic::geometry;
namespace materials = draconic::materials;
namespace resource = draconic::resource;
namespace texture = draconic::texture;

export namespace draconic::particles
{
    // Attach a particle effect to an entity. The effect is borrowed (the app owns it now; a cooked
    // ParticleEffectResource later). SetEffect spins up the runtime instance.
    struct ParticleEffectComponent
    {
        ParticleEffect* effect = nullptr; // borrowed
        UniquePtr<ParticleEffectInstance> instance;
        rhi::TextureView* texture =
            nullptr; // billboard atlas (borrowed); null = untextured (soft dot)
        // Mesh-mode systems (ParticleRenderMode::Mesh) draw this mesh per particle through the instanced-
        // mesh path. Held while attached; per-particle transform = position * axis/rotation * (size*meshScale).
        draconic::resource::Ref<geometry::StaticMesh> mesh;
        draconic::resource::Ref<materials::Material> material;
        f32 meshScale = 1.0f;
        // Light-mode systems (ParticleRenderMode::Light) add a point light per particle (capped) to the
        // scene's clustered-forward light list, so particles illuminate their surroundings; they also draw
        // the billboard glow. Intensity scales by particle alpha (fades out); range is fixed per emitter.
        f32 lightIntensity = 4.0f;
        f32 lightRange = 4.0f;
        bool visible = true;

        // Cooked-resource path: a guid-serialized, hot-reload-following ref to the cooked effect +
        // the component's own independent clone (stable address across component moves - the
        // instance borrows *ownedEffect). The manager attaches/re-attaches when the resolved
        // product changes (a pick, a load, or a hot reload).
        draconic::resource::Ref<ParticleEffectResource> effectAsset;
        UniquePtr<ParticleEffect> ownedEffect;
        ParticleEffectResource* attachedResource = nullptr; // what ownedEffect was cloned from

        // Code path: attach a borrowed, app-owned effect (tests/samples building effects in code).
        void SetEffect(ParticleEffect& fx)
        {
            effect = &fx;
            instance = MakeUnique<ParticleEffectInstance>(DefaultAllocator(), fx);
        }
        // Cooked path: attach a bound ParticleEffectResource. The component clones the cooked template
        // into its own effect (so multiple entities don't share live particle state) and simulates that;
        // the resolved per-system textures come from the resource (effectAsset->SystemTexture(i)).
        void SetEffect(resource::Proxy<ParticleEffectResource> res)
        {
            effectAsset.SetProxy(res);
            AttachResource(res.Get());
        }

        // (Re)clone `res` as this component's live effect. Called by SetEffect and by the manager
        // when the ref's resolved product changes.
        void AttachResource(ParticleEffectResource* res)
        {
            attachedResource = res;
            ownedEffect = MakeUnique<ParticleEffect>(DefaultAllocator());
            if (res != nullptr)
            {
                CloneEffect(res->Effect(), *ownedEffect);
            }
            effect = ownedEffect.Get();
            instance = MakeUnique<ParticleEffectInstance>(DefaultAllocator(), *ownedEffect);
        }
    };

    // Persist the resource refs + tunables; the live instance/clone and the raw view are runtime-only.
    inline void Serialize(ISerializer& ar, ParticleEffectComponent& c)
    {
        draconic::foundation::Serialize(ar, "effect", c.effectAsset);
        draconic::foundation::Serialize(ar, "mesh", c.mesh);
        draconic::foundation::Serialize(ar, "material", c.material);
        draconic::foundation::Serialize(ar, "meshScale", c.meshScale);
        draconic::foundation::Serialize(ar, "lightIntensity", c.lightIntensity);
        draconic::foundation::Serialize(ar, "lightRange", c.lightRange);
        draconic::foundation::Serialize(ar, "visible", c.visible);
    }

    inline void ResolveResources(resource::ResourceManager& manager, ParticleEffectComponent& c)
    {
        c.effectAsset.Bind(manager);
        c.mesh.Bind(manager);
        c.material.Bind(manager);
    }

    class ParticleEffectComponentManager final
        : public scene::SerializableComponentManager<ParticleEffectComponent>,
          public render::IRenderDataProvider
    {
    public:
        ParticleEffectComponentManager()
            : scene::SerializableComponentManager<ParticleEffectComponent>(u8"particle_effect")
        {
        }

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return false; }

        // The dispatch id of the ParticleRenderer (set by the ParticleSubsystem after it registers it).
        void SetBillboardRendererId(u16 id) noexcept { m_billboardRendererId = id; }

        // Scene-driven sim: advance every instance in PostUpdate (before render extraction).
        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            DRACONIC_PROFILE_SCOPE("Particles.Simulate");
            ForEach(
                [&](ParticleEffectComponent& c, scene::EntityHandle owner)
                {
                    // Attach/re-attach when the ref's resolved product changed (a pick, a scene load's
                    // resolve pass, or a hot reload swapping the product behind the proxy).
                    ParticleEffectResource* res = c.effectAsset.Get();
                    if (res != c.attachedResource && res != nullptr)
                    {
                        c.AttachResource(res);
                    }
                    if (!c.instance)
                    {
                        return;
                    }
                    c.instance->position = m_scene->GetWorldPosition(owner);
                    c.instance->Update(deltaTime, m_cameraPos);
                });
        }

        // render::IRenderDataProvider: pack each visible billboard system's live particles into a
        // ParticleBillboardRenderData batch and add it to the snapshot. Called during render extraction
        // (this scene's turn). Mesh-mode systems are handled elsewhere (instanced-mesh, later).
        void ExtractRenderData(render::ExtractedScene& snapshot) override
        {
            DRACONIC_PROFILE_SCOPE("Particles.Extract");
            const u16 billboardRendererId = m_billboardRendererId;
            m_scratchUsed = 0;
            m_xformUsed = 0;
            m_tintUsed = 0;
            m_trailUsed = 0;
            ForEach(
                [&](ParticleEffectComponent& c, scene::EntityHandle owner)
                {
                    if (!c.visible || !c.instance)
                    {
                        return;
                    }
                    // The emitter's world transform - used to re-base Local-space particles at extract.
                    m_emitterWorld = (m_scene != nullptr) ? m_scene->GetWorldMatrix(owner)
                                                          : Float4x4::Identity();
                    ParticleEffect& fx = c.instance->Effect();
                    for (i32 s = 0; s < fx.SystemCount(); ++s)
                    {
                        ParticleSystem* sys = fx.GetSystem(s);
                        if (sys == nullptr)
                        {
                            continue;
                        }
                        if (sys->renderMode == ParticleRenderMode::Mesh)
                        {
                            ExtractMeshSystem(*sys, c, owner, s, snapshot);
                            continue;
                        }
                        if (sys->renderMode == ParticleRenderMode::Trail)
                        {
                            ExtractTrailSystem(*sys, c, s, snapshot);
                            continue;
                        }
                        const i32 alive = sys->AliveCount();
                        if (alive <= 0)
                        {
                            continue;
                        }
                        // Light-mode: illuminate the scene with a capped set of point lights (drawn as billboards too).
                        if (sys->renderMode == ParticleRenderMode::Light)
                        {
                            ExtractLights(*sys, c, snapshot);
                        }

                        Array<ParticleBillboardInstance>& scratch = AcquireScratch();
                        scratch.Resize(static_cast<usize>(alive));
                        Float3 boundsMin{1e30f, 1e30f, 1e30f}, boundsMax{-1e30f, -1e30f, -1e30f};
                        const i32* order =
                            (sys->sortParticles && sys->blendMode == ParticleBlendMode::Alpha)
                                ? BuildBackToFrontOrder(*sys)
                                : nullptr;
                        PackBillboards(*sys, scratch.Data(), boundsMin, boundsMax, order);

                        ParticleBillboardRenderData* rd =
                            snapshot.Add<ParticleBillboardRenderData>();
                        if (rd == nullptr)
                        {
                            continue;
                        }
                        rd->category = render::RenderCategories::Transparent;
                        rd->rendererId = billboardRendererId;
                        rd->instances = scratch.Data();
                        rd->count = static_cast<u32>(alive);
                        rd->texture = SystemTextureView(c, s);
                        rd->blend = sys->blendMode;
                        const Float3 center = (boundsMin + boundsMax) * 0.5f;
                        rd->worldCenter = center;
                        rd->worldRadius = Length(boundsMax - center) + LargestSize(*sys);
                    }
                });
        }

    private:
        // Map a render mode to the shader's orientation mode (0 camera / 1 camera-about-Y / 2 world-XY).
        [[nodiscard]] static f32 OrientationMode(ParticleRenderMode m) noexcept
        {
            switch (m)
            {
            case ParticleRenderMode::VerticalBillboard:
                return 1.0f;
            case ParticleRenderMode::HorizontalBillboard:
                return 2.0f;
            default:
                return 0.0f; // Billboard / Stretched
            }
        }

        // `order` (nullable) remaps output slot -> source particle index; null = identity. Sorted
        // back-to-front for alpha systems so overlapping transparents composite correctly.
        void PackBillboards(ParticleSystem& sys, ParticleBillboardInstance* out, Float3& bmin,
                            Float3& bmax, const i32* order = nullptr)
        {
            ParticleStreamContainer& st = sys.Streams();
            CPUStream<Float3>* pos = st.Positions();
            CPUStream<Float2>* sizes = st.Sizes();
            CPUStream<Float4>* cols = st.Colors();
            CPUStream<f32>* rots = st.Rotations();
            CPUStream<Float3>* vels = st.Velocities();
            CPUStream<f32>* ages = st.Ages();
            CPUStream<f32>* lifes = st.Lifetimes();
            const bool stretch = (sys.renderMode == ParticleRenderMode::StretchedBillboard);
            const f32 modeF = OrientationMode(sys.renderMode);
            const f32 softDist = sys.softParticles ? sys.softDistance : 0.0f; // 0 => no soft fade
            const bool flip = sys.flipbook.IsActive();
            const i32 alive = sys.AliveCount();
            const Float4x4 xform =
                sys.simulationSpace == ParticleSpace::Local ? m_emitterWorld : Float4x4::Identity();
            const bool localXform = sys.simulationSpace == ParticleSpace::Local;
            for (i32 k = 0; k < alive; ++k)
            {
                const i32 i = (order != nullptr) ? order[k] : k;
                Float3 p = (pos != nullptr) ? (*pos)[i] : Float3::Zero;
                if (localXform)
                {
                    p = TransformPoint(p, xform);
                }
                const Float2 sz = (sizes != nullptr) ? (*sizes)[i] : Float2{0.1f, 0.1f};
                ParticleBillboardInstance& o = out[k];
                o.positionSize = Float4{p.x, p.y, p.z, sz.x};
                o.sizeRotMode =
                    Float4{sz.y, (rots != nullptr) ? (*rots)[i] : 0.0f, modeF, softDist};
                o.color = (cols != nullptr) ? (*cols)[i] : Float4{1.0f, 1.0f, 1.0f, 1.0f};
                const f32 lifeRatio = (ages != nullptr && lifes != nullptr && (*lifes)[i] > 0.0f)
                                          ? ((*ages)[i] / (*lifes)[i])
                                          : 0.0f;
                o.uvRect = flip ? sys.flipbook.FrameUV(lifeRatio, (*ages)[i])
                                : Float4{0.0f, 0.0f, 1.0f, 1.0f};
                Float3 v = (stretch && vels != nullptr) ? (*vels)[i] : Float3::Zero;
                if (localXform && stretch)
                {
                    v = TransformDirection(v, xform);
                }
                o.velocity = Float4{v.x, v.y, v.z, stretch ? 0.1f : 0.0f};
                bmin = Float3{Min(bmin.x, p.x), Min(bmin.y, p.y), Min(bmin.z, p.z)};
                bmax = Float3{Max(bmax.x, p.x), Max(bmax.y, p.y), Max(bmax.z, p.z)};
            }
        }

        // Fill `order` with alive-particle indices sorted back-to-front (farthest camera distance first)
        // so an alpha-blended system composites correctly. Local-space positions are transformed first.
        const i32* BuildBackToFrontOrder(ParticleSystem& sys)
        {
            DRACONIC_PROFILE_SCOPE("Particles.Sort");
            const i32 alive = sys.AliveCount();
            CPUStream<Float3>* pos = sys.Streams().Positions();
            if (pos == nullptr)
            {
                return nullptr;
            }
            const bool localXform = sys.simulationSpace == ParticleSpace::Local;
            m_sortOrder.Resize(static_cast<usize>(alive));
            m_sortDist.Resize(static_cast<usize>(alive));
            for (i32 i = 0; i < alive; ++i)
            {
                Float3 p = (*pos)[i];
                if (localXform)
                {
                    p = TransformPoint(p, m_emitterWorld);
                }
                m_sortOrder[static_cast<usize>(i)] = i;
                m_sortDist[static_cast<usize>(i)] = LengthSquared(p - m_cameraPos);
            }
            const f32* dist = m_sortDist.Data();
            std::sort(m_sortOrder.Data(), m_sortOrder.Data() + alive,
                      [dist](i32 a, i32 b) { return dist[a] > dist[b]; }); // farthest first
            return m_sortOrder.Data();
        }

        [[nodiscard]] static f32 LargestSize(ParticleSystem& sys) noexcept
        {
            CPUStream<Float2>* sizes = sys.Streams().Sizes();
            if (sizes == nullptr)
            {
                return 0.5f;
            }
            f32 m = 0.0f;
            for (i32 i = 0; i < sys.AliveCount(); ++i)
            {
                m = Max(m, Max((*sizes)[i].x, (*sizes)[i].y));
            }
            return m;
        }

        // Per-batch scratch pool (UniquePtr so growth never moves a live buffer; a render-data points
        // into one, valid for the frame). Reused each extraction.
        Array<ParticleBillboardInstance>& AcquireScratch()
        {
            if (m_scratchUsed >= m_scratch.Size())
            {
                m_scratch.PushBack(
                    MakeUnique<Array<ParticleBillboardInstance>>(DefaultAllocator()));
            }
            return *m_scratch[m_scratchUsed++];
        }

        // Maps a mesh-particle material's blend preset to a render category (mirrors the render
        // subsystem's CategoryForMaterial). Opaque/Masked keep their pass; everything else is Transparent.
        [[nodiscard]] static render::RenderCategory
        MeshCategoryFor(const materials::Material* m) noexcept
        {
            if (m == nullptr)
            {
                return render::RenderCategories::Opaque;
            }
            switch (m->pipeline.blendMode)
            {
            case materials::BlendMode::Opaque:
                return render::RenderCategories::Opaque;
            case materials::BlendMode::Masked:
                return render::RenderCategories::Masked;
            default:
                return render::RenderCategories::Transparent;
            }
        }

        // Mesh-mode: emit a MultiMeshRenderData drawn by the shared mesh renderer (the instanced-mesh
        // path). Per-particle world transform + tint; a bumped version re-uploads the (dynamic) set each
        // frame. No new renderer needed - particles reuse the InstancedMesh persistent-buffer machinery.
        void ExtractMeshSystem(ParticleSystem& sys, ParticleEffectComponent& c,
                               scene::EntityHandle owner, i32 sysIndex,
                               render::ExtractedScene& snapshot)
        {
            const i32 alive = sys.AliveCount();
            if (alive <= 0 || c.mesh.Get() == nullptr)
            {
                return;
            }

            Array<Float4x4>& xf = AcquireXformScratch();
            Array<Color>& tint = AcquireTintScratch();
            xf.Resize(static_cast<usize>(alive));
            tint.Resize(static_cast<usize>(alive));
            Float3 bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f};
            PackMeshTransforms(sys, c.meshScale, xf.Data(), tint.Data(), bmin, bmax);

            render::MultiMeshRenderData* rd = snapshot.Add<render::MultiMeshRenderData>();
            if (rd == nullptr)
            {
                return;
            }
            rd->multiMesh = true;
            rd->key = (static_cast<u64>(owner.index) << 16) |
                      static_cast<u64>(static_cast<u32>(sysIndex) & 0xFFFFu);
            rd->transforms = xf.Data();
            rd->tints = tint.Data();
            rd->instanceCount = static_cast<u32>(alive);
            rd->version = ++m_meshVersion; // dynamic: transforms change every frame -> re-upload
            rd->mesh = c.mesh.Get();
            rd->material = c.material.Get();
            rd->rendererId = 0; // the mesh renderer (id 0)
            // Category from the material's blend mode (like regular meshes + Sedulous): an opaque material
            // stays Opaque; a transparent/additive one routes to the Transparent pass (ResolveMultiMesh
            // still instances it - the set sorts as one item, fine for additive / approximate for alpha).
            rd->category = MeshCategoryFor(c.material.Get());
            const Float3 center = (bmin + bmax) * 0.5f;
            rd->worldCenter = center;
            rd->worldRadius = Length(bmax - center) + LargestSize(sys) * c.meshScale;
        }

        // Light-mode: add a point light per particle (capped + evenly strided across the set to stay
        // under the forward light budget), colored by the particle, intensity faded by its alpha.
        void ExtractLights(ParticleSystem& sys, ParticleEffectComponent& c,
                           render::ExtractedScene& snapshot)
        {
            CPUStream<Float3>* pos = sys.Streams().Positions();
            if (pos == nullptr)
            {
                return;
            }
            CPUStream<Float4>* cols = sys.Streams().Colors();
            const i32 alive = sys.AliveCount();
            const i32 cap = Min(alive, kLightParticleCap);
            const i32 step = Max(1, alive / Max(cap, 1));
            i32 added = 0;
            for (i32 i = 0; added < cap && i < alive; i += step, ++added)
            {
                render::GpuLight g;
                g.positionWS = (sys.simulationSpace == ParticleSpace::Local)
                                   ? TransformPoint((*pos)[i], m_emitterWorld)
                                   : (*pos)[i];
                g.range = c.lightRange;
                const Float4 cv = (cols != nullptr) ? (*cols)[i] : Float4{1.0f, 1.0f, 1.0f, 1.0f};
                g.color = Float3{cv.x, cv.y, cv.z};
                g.intensity = c.lightIntensity * cv.w; // fade with the particle's alpha
                g.directionWS = Float3{0.0f, -1.0f, 0.0f};
                g.type = 1.0f;         // point (LightType::Point)
                g.shadowIndex = -1.0f; // particles don't cast shadows
                snapshot.AddLight(g);
            }
        }

        // Trail-mode: build camera-facing ribbon geometry from each live particle's recorded point ring.
        // Each pair of consecutive points becomes a quad (2 tris) whose "side" is perpendicular to both
        // the segment and the view direction, so the ribbon always faces the camera. Width tapers
        // widthStart -> widthEnd along the trail; alpha fades with each point's age. The vertices are
        // packed into a frame-lived scratch buffer and handed off as one ParticleTrailRenderData batch.
        // Resolve a system's billboard/trail texture: the cooked resource's per-system Proxy<Texture>
        // (hot-reload-following) when this component is resource-driven, else the code-set c.texture.
        [[nodiscard]] static rhi::TextureView* SystemTextureView(const ParticleEffectComponent& c,
                                                                 i32 systemIndex)
        {
            if (ParticleEffectResource* res = c.effectAsset.Get())
            {
                if (texture::Texture* t = res->SystemTexture(systemIndex).Get())
                {
                    return t->View();
                }
            }
            return c.texture;
        }

        void ExtractTrailSystem(ParticleSystem& sys, ParticleEffectComponent& c, i32 systemIndex,
                                render::ExtractedScene& snapshot)
        {
            const i32 mp = sys.TrailMaxPoints();
            const i32 alive = sys.AliveCount();
            if (mp < 2 || alive <= 0)
            {
                return;
            }
            const TrailSettings& t = sys.trail;
            const Span<const ParticleTrailState> states = sys.TrailStates();
            const Span<const TrailPoint> points = sys.TrailPoints();
            const f32 now = sys.TotalTime();
            const f32 invLife = 1.0f / Max(t.lifetime, 1e-3f);

            const bool localXform = sys.simulationSpace == ParticleSpace::Local;

            Array<TrailVertex>& verts = AcquireTrailScratch();
            verts.Clear();
            Float3 bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f};

            auto push = [&](const Float3& p, f32 v, const Float4& col)
            {
                verts.PushBack(TrailVertex{
                    p, Float2{0.5f, v},
                    col}); // u=0.5: sample the dot's opaque center column, v across for soft edges
                bmin = Float3{Min(bmin.x, p.x), Min(bmin.y, p.y), Min(bmin.z, p.z)};
                bmax = Float3{Max(bmax.x, p.x), Max(bmax.y, p.y), Max(bmax.z, p.z)};
            };

            for (i32 pi = 0; pi < alive; ++pi)
            {
                const ParticleTrailState& st = states[static_cast<usize>(pi)];
                const i32 n = Min(st.count, mp);
                if (n < 2)
                {
                    continue;
                }
                const TrailPoint* base = &points[static_cast<usize>(pi) * static_cast<usize>(mp)];
                for (i32 k = 0; k + 1 < n; ++k)
                {
                    // Ring order is newest -> oldest: index 0 sits at head, walking backwards.
                    const TrailPoint& p0 = base[((st.head - k) % mp + mp) % mp];
                    const TrailPoint& p1 = base[((st.head - (k + 1)) % mp + mp) % mp];
                    const Float3 pos0 =
                        localXform ? TransformPoint(p0.position, m_emitterWorld) : p0.position;
                    const Float3 pos1 =
                        localXform ? TransformPoint(p1.position, m_emitterWorld) : p1.position;
                    const Float3 seg = pos0 - pos1;
                    if (LengthSquared(seg) < 1e-8f)
                    {
                        continue;
                    }
                    const Float3 mid = (pos0 + pos1) * 0.5f;
                    const Float3 view = Normalized(mid - m_cameraPos);
                    Float3 side = Cross(Normalized(seg), view);
                    if (LengthSquared(side) < 1e-8f)
                    {
                        continue;
                    }
                    side = Normalized(side);

                    const f32 f0 =
                        static_cast<f32>(k) / static_cast<f32>(n - 1); // 0 at newest, 1 at oldest
                    const f32 f1 = static_cast<f32>(k + 1) / static_cast<f32>(n - 1);
                    const f32 w0 = Lerp(t.widthStart, t.widthEnd, f0) * 0.5f;
                    const f32 w1 = Lerp(t.widthStart, t.widthEnd, f1) * 0.5f;
                    const f32 a0 = Clamp(1.0f - (now - p0.recordTime) * invLife, 0.0f, 1.0f);
                    const f32 a1 = Clamp(1.0f - (now - p1.recordTime) * invLife, 0.0f, 1.0f);
                    const Float4 c0{p0.color.x, p0.color.y, p0.color.z, p0.color.w * a0};
                    const Float4 c1{p1.color.x, p1.color.y, p1.color.z, p1.color.w * a1};

                    const Float3 l0 = pos0 + side * w0, r0 = pos0 - side * w0;
                    const Float3 l1 = pos1 + side * w1, r1 = pos1 - side * w1;
                    push(l0, 0.0f, c0);
                    push(r0, 1.0f, c0);
                    push(l1, 0.0f, c1); // tri 1
                    push(r0, 1.0f, c0);
                    push(r1, 1.0f, c1);
                    push(l1, 0.0f, c1); // tri 2
                }
            }

            if (verts.IsEmpty())
            {
                return;
            }
            ParticleTrailRenderData* rd = snapshot.Add<ParticleTrailRenderData>();
            if (rd == nullptr)
            {
                return;
            }
            rd->category = render::RenderCategories::Transparent;
            rd->rendererId =
                m_billboardRendererId; // trails ride the same particle rendererId (told apart by particleKind)
            rd->vertices = verts.Data();
            rd->vertexCount = static_cast<u32>(verts.Size());
            rd->texture = SystemTextureView(c, systemIndex);
            rd->blend = sys.blendMode;
            const Float3 center = (bmin + bmax) * 0.5f;
            rd->worldCenter = center;
            rd->worldRadius = Length(bmax - center) + t.widthStart;
        }

        void PackMeshTransforms(ParticleSystem& sys, f32 meshScale, Float4x4* xf, Color* tint,
                                Float3& bmin, Float3& bmax)
        {
            ParticleStreamContainer& st = sys.Streams();
            CPUStream<Float3>* pos = st.Positions();
            CPUStream<Float2>* sizes = st.Sizes();
            CPUStream<Float4>* cols = st.Colors();
            CPUStream<Float3>* axes = st.Axes();
            CPUStream<f32>* rots = st.Rotations();
            const bool localXform = sys.simulationSpace == ParticleSpace::Local;
            const i32 alive = sys.AliveCount();
            for (i32 i = 0; i < alive; ++i)
            {
                const Float3 p = (pos != nullptr) ? (*pos)[i] : Float3::Zero;
                const f32 sz = ((sizes != nullptr) ? (*sizes)[i].x : 0.1f) * meshScale;
                Transform t;
                t.position = p;
                t.scale = Float3{sz, sz, sz};
                if (rots != nullptr)
                {
                    const Float3 axis = (axes != nullptr) ? (*axes)[i] : Float3::UnitY;
                    t.rotation = Quaternion::FromAxisAngle(
                        (LengthSquared(axis) > 1e-6f) ? Normalized(axis) : Float3::UnitY,
                        (*rots)[i]);
                }
                xf[i] = localXform ? (t.ToMatrix() * m_emitterWorld)
                                   : t.ToMatrix(); // local particle -> world via emitter
                const Float3 wp = localXform ? TransformPoint(p, m_emitterWorld) : p;
                const Float4 cv = (cols != nullptr) ? (*cols)[i] : Float4{1.0f, 1.0f, 1.0f, 1.0f};
                tint[i] = Color{cv.x, cv.y, cv.z, cv.w};
                bmin = Float3{Min(bmin.x, wp.x), Min(bmin.y, wp.y), Min(bmin.z, wp.z)};
                bmax = Float3{Max(bmax.x, wp.x), Max(bmax.y, wp.y), Max(bmax.z, wp.z)};
            }
        }

        Array<Float4x4>& AcquireXformScratch()
        {
            if (m_xformUsed >= m_xformScratch.Size())
            {
                m_xformScratch.PushBack(MakeUnique<Array<Float4x4>>(DefaultAllocator()));
            }
            return *m_xformScratch[m_xformUsed++];
        }
        Array<Color>& AcquireTintScratch()
        {
            if (m_tintUsed >= m_tintScratch.Size())
            {
                m_tintScratch.PushBack(MakeUnique<Array<Color>>(DefaultAllocator()));
            }
            return *m_tintScratch[m_tintUsed++];
        }
        Array<TrailVertex>& AcquireTrailScratch()
        {
            if (m_trailUsed >= m_trailScratch.Size())
            {
                m_trailScratch.PushBack(MakeUnique<Array<TrailVertex>>(DefaultAllocator()));
            }
            return *m_trailScratch[m_trailUsed++];
        }

        // Max point lights one Light system contributes/frame. Kept well above a Light system's typical
        // alive count so the selection stride stays 1 (every particle gets a light) - a smaller cap strides
        // across the set and, because swap-remove reshuffles indices each frame, makes lights pop in/out.
        // Still far under the clustered-forward budget (256/view).
        static constexpr i32 kLightParticleCap = 200;

        scene::Scene* m_scene = nullptr;
        Float3 m_cameraPos{0.0f, 0.0f, 0.0f};
        Float4x4 m_emitterWorld =
            Float4x4::Identity(); // current component's emitter transform (Local space)
        Array<i32> m_sortOrder;   // back-to-front particle order (alpha systems)
        Array<f32> m_sortDist;    // squared camera distance per particle (sort key)
        u16 m_billboardRendererId = 0;
        Array<UniquePtr<Array<ParticleBillboardInstance>>> m_scratch;
        usize m_scratchUsed = 0;
        Array<UniquePtr<Array<Float4x4>>> m_xformScratch; // mesh-particle world transforms
        Array<UniquePtr<Array<Color>>> m_tintScratch;     // mesh-particle per-instance tints
        usize m_xformUsed = 0;
        usize m_tintUsed = 0;
        Array<UniquePtr<Array<TrailVertex>>>
            m_trailScratch; // trail ribbon vertices (per Trail-mode system)
        usize m_trailUsed = 0;
        u32 m_meshVersion = 0; // bumped per mesh batch so the instanced-mesh path re-uploads
    };

    // A scene-bound PARTICLES handle (SceneParticles.of(scene)): runtime WORLD ops on particle effects
    // that need the component's manager-owned runtime ParticleEffectInstance (which the component data
    // cannot reach) - play/stop/restart/pause emission, query, and swap the effect by resource id.
    // Keyed by entity, mirroring ScenePhysics / SceneRender / SceneAudio / SceneAnimation (component =
    // auto-reflected DATA: visible/meshScale/light*; scene-handle = world ops). The instance is built
    // lazily by the manager when the effect resolves, so control from onUpdate sees it; a call before
    // the effect is attached is a safe no-op.
    struct SceneParticles
    {
        scene::Scene* scene = nullptr;

        // Begin/resume emission on the entity's effect. No-op if no effect is attached yet.
        void play(draconic::script::Entity entity) const
        {
            if (ParticleEffectInstance* i = Instance(entity))
            {
                i->Play();
            }
        }
        // Stop emitting (live particles finish out).
        void stop(draconic::script::Entity entity) const
        {
            if (ParticleEffectInstance* i = Instance(entity))
            {
                i->Stop();
            }
        }
        // Reset to empty and begin emitting fresh (a one-shot re-trigger).
        void restart(draconic::script::Entity entity) const
        {
            if (ParticleEffectInstance* i = Instance(entity))
            {
                i->Reset();
                i->Play();
            }
        }
        // Pause/resume the whole simulation for this effect (freezes live particles too).
        void pause(draconic::script::Entity entity, bool paused) const
        {
            if (ParticleEffectInstance* i = Instance(entity))
            {
                i->isActive = !paused;
            }
        }
        // True while the effect is still emitting or has live particles.
        [[nodiscard]] bool isPlaying(draconic::script::Entity entity) const
        {
            ParticleEffectInstance* i = Instance(entity);
            return i != nullptr && !i->IsFinished();
        }
        // Swap the entity's effect to resource `id`, binding it through the run's resource manager;
        // the manager re-attaches (re-clones) the new effect next tick.
        void setEffect(draconic::script::Entity entity, Guid id) const
        {
            if (ParticleEffectComponent* c = Component(entity))
            {
                c->effectAsset.SetId(id);
                if (auto* resources = draconic::script::CurrentRunResources())
                {
                    c->effectAsset.Bind(*resources);
                }
            }
        }

        [[nodiscard]] static SceneParticles of(draconic::script::Scene sceneHandle)
        {
            return SceneParticles{sceneHandle.scene};
        }

    private:
        [[nodiscard]] ParticleEffectComponent* Component(draconic::script::Entity entity) const
        {
            if (scene == nullptr)
            {
                return nullptr;
            }
            auto* manager = scene->GetSystem<ParticleEffectComponentManager>();
            return (manager != nullptr) ? manager->Get(entity.Handle()) : nullptr;
        }
        [[nodiscard]] ParticleEffectInstance* Instance(draconic::script::Entity entity) const
        {
            ParticleEffectComponent* c = Component(entity);
            return (c != nullptr) ? c->instance.Get() : nullptr;
        }
    };

} // exported namespace

// Reflection (tooling). The DRACONIC_REFLECT_VALUE body + RegisterParticleComponentReflection()
// live in ParticleSubsystemImpl.cpp (kept out of this interface; see gcc-module-interface-hygiene).
export namespace draconic::particles
{
    void RegisterParticleComponentReflection();

    // Surfaces the particle component to SCRIPT (Track A): ParticleEffectComponent.of(entity) for the
    // DATA (visible/meshScale/light*), plus SceneParticles.of(scene) for the world ops (play/stop/
    // restart/pause/isPlaying/setEffect). Registers + seeds + names them. Called by the composition
    // root (like RegisterRenderScriptFacade).
    void RegisterParticleScriptFacade();
}
