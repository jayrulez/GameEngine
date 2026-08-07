// RenderStressTest - a deliberate worst-case renderer benchmark, ported from Sedulous's
// EngineRenderStressTest. It keeps us honest as rendering features land: a flat grid of
// spheres positioned so the camera sees ALL of them at once (frustum culling can't help),
// growable 8000 at a time. Two axes of stress:
//
//   * Batching   - by default every sphere shares ONE material + mesh, so the renderer
//                  should collapse them into a single instanced draw. Press U to give each
//                  sphere its OWN material (unique hue) → defeats batching → a draw per sphere.
//   * Static opt - press B for a sin-wave bob that rewrites EVERY sphere's transform each
//                  frame, so nothing can be cached as static (full extraction every frame).
//
// No HUD yet (UI/VG deferred): stats print to the console once per second (toggle H), and the
// inherited P key dumps the CPU scope tree + per-pass GPU timings. Fly camera: WASD/QE move,
// hold RMB (or Tab to capture) to look, Shift to move fast, Esc to exit.

#include "Draconic.Foundation/Prelude.h"
#include "imgui.h" // Dear ImGui (HUD) - used directly; engine integration is draconic.imgui

import draconic.foundation;
import draconic.rhi; // PresentMode (run the benchmark vsync-off)
import draconic.runtime;
import draconic.runtime.client;
import draconic.shell;
import draconic.runtime.desktop;
import draconic.shell.desktop;
import draconic.graphics;
import draconic.graphics.gpu;
import draconic.engine.defaultapp;
import draconic.scene;
import draconic.engine.scene;
import draconic.engine.render;
import draconic.imgui; // ImguiSubsystem (HUD)
import draconic.geometry;
import draconic.materials;

#include "../Common/FlyCamera.h" // shared free-fly camera (uses the imported runtime/core types)

namespace foundation = draconic::foundation;
namespace rhi = draconic::rhi;
namespace runtime = draconic::runtime;
namespace graphics = draconic::graphics;
namespace shell = draconic::shell;
namespace samples = draconic::samples;
namespace scene = draconic::scene;
namespace render = draconic::render;
namespace imgui = draconic::imgui;
namespace geometry = draconic::geometry;
namespace materials = draconic::materials;

namespace
{
    class StressTestApp final : public runtime::DefaultApplication
    {
        static constexpr foundation::i32 kSpheresPerBatch = 8000;
        static constexpr foundation::f32 kSphereSpacing = 1.5f;
        static constexpr foundation::f32 kSphereHeight = 2.5f; // base height above the floor (radius 0.5)
        static constexpr foundation::f32 kFloorBaseSize =
            500.0f; // base ground-plane size (scaled to cover the grid)

    public:
        // Run uncapped (vsync off) so the frame time reflects real CPU+GPU work, not the display
        // refresh. The numbers tear visually - that's fine for a benchmark. Switch to Fifo to cap.
        graphics::RenderWindowDesc MainRenderWindow() const override
        {
            graphics::RenderWindowDesc d;
            d.presentMode = rhi::PresentMode::Immediate;
            return d;
        }

        // Register the ImGui subsystem so the benchmark HUD can draw over the scene.
        void Configure(runtime::IApplicationHost& host) override
        {
            runtime::DefaultApplication::Configure(host);
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                host.Ctx().AddSubsystem<imgui::ImguiSubsystem>(*gfx->Raw(), gfx->FramesInFlight());
            }
        }

        void OnStartup(runtime::IApplicationHost& host) override
        {
            auto* scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"stress");

            // A modest ambient so unlit-facing hemispheres aren't pure black.
            if (auto* env = m_scene->GetSystem<render::EnvironmentSystem>())
            {
                env->Environment().ambientColor = foundation::Color{0.10f, 0.12f, 0.16f, 1.0f};
                env->Environment().ambientIntensity = 0.30f;
            }

            // Shared sphere material (gray PBR). Every sphere points at THIS one by default, so the
            // renderer can batch them. Unique mode (U) builds a per-sphere material instead.
            m_sharedMat = materials::CreatePBR(u8"stress.shared",
                                               foundation::Float4{0.7f, 0.7f, 0.7f, 1.0f}, 0.1f, 0.4f);

            // One sphere mesh, shared by all instances (matches Sedulous: radius 0.5, 16x8).
            m_sphere = geometry::Primitives::Sphere(0.5f, 16, 8);

            // Large ground plane so the bobbing spheres read against a surface.
            if (auto* meshes = m_scene->GetSystem<render::MeshComponentManager>())
            {
                m_ground = m_scene->CreateEntity(u8"ground");
                m_scene->SetLocalPosition(m_ground, foundation::Float3{0.0f, 0.0f, 0.0f});
                render::MeshComponent& gm = meshes->Add(m_ground);
                gm.mesh = geometry::Primitives::Plane(kFloorBaseSize, kFloorBaseSize);
                gm.SetMaterial(materials::CreatePBR(
                    u8"stress.ground", foundation::Float4{0.3f, 0.3f, 0.3f, 1.0f}, 0.0f, 0.8f));
            }

            // Directional key light.
            if (auto* lights = m_scene->GetSystem<render::LightComponentManager>())
            {
                scene::EntityHandle sun = m_scene->CreateEntity(u8"sun");
                foundation::Transform st = m_scene->GetLocalTransform(sun);
                st.rotation =
                    foundation::Quaternion::FromAxisAngle(foundation::Float3{1.0f, 0.0f, 0.0f}, -0.9f) *
                    foundation::Quaternion::FromAxisAngle(foundation::Float3{0.0f, 1.0f, 0.0f}, 0.5f);
                m_scene->SetLocalTransform(sun, st);
                render::LightComponent& sl = lights->Add(sun);
                sl.type = render::LightType::Directional;
                sl.color = foundation::Color{1.0f, 0.95f, 0.9f, 1.0f};
                sl.intensity = 1.5f;
                sl.castsShadows = true; // phase 5.1: the spheres cast shadows on the ground
                m_sun = sun; // K toggles its shadows (for shadowed-vs-unshadowed benchmarking)
            }

            // Fly camera, pulled well back + up so the whole grid is in frame (worst case for culling).
            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
            {
                render::CameraComponent& cam = cameras->Add(m_camera);
                cam.fovYRadians = 1.04719755f; // 60 deg
                cam.nearZ = 0.1f;
                cam.farZ = 2000.0f;
                cam.clearColor = foundation::Color{0.04f, 0.05f, 0.07f, 1.0f};
            }
            PushCameraToEntity();

            AddSphereBatch(); // start with one batch

            // Lower default exposure: the procedural-sky IBL + sun are bright, so AgX washes out at 1.0.
            if (auto* render = host.Ctx().GetSubsystem<render::RenderSubsystem>())
            {
                render->SetExposure(0.5f);
            }

            foundation::ConsoleWrite(
                u8"=== Render Stress Test ===\n"
                u8"  Space: +8000 spheres   Backspace: -8000\n"
                u8"  U: toggle unique materials (defeats batching)\n"
                u8"  B: toggle sin-wave bob (defeats static caching)\n"
                u8"  M: toggle MultiMesh (whole grid as ONE instanced set, O(1)/frame CPU)\n"
                u8"  H: toggle console stats   P: profiler dump\n"
                u8"  WASD/QE move, RMB look, Tab capture, Shift fast, Esc exit\n"
                u8"==========================\n");
        }

        // The default render path reads the camera's aspect straight from the component, so keep it in
        // sync with the backbuffer before delegating to DefaultApplication's single-view render.
        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            if (m_scene != nullptr && frame.height > 0)
            {
                if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
                {
                    if (render::CameraComponent* cam = cameras->Get(m_camera))
                    {
                        cam->aspect = static_cast<foundation::f32>(frame.width) /
                                      static_cast<foundation::f32>(frame.height);
                    }
                }
            }
            runtime::DefaultApplication::OnRenderWindow(host, frame);

            // HUD over the scene (backbuffer is RenderTarget after the default render path).
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->Render(frame);
            }
        }

        void OnUpdate(runtime::IApplicationHost& host, foundation::f32 deltaTime) override
        {
            runtime::DefaultApplication::OnUpdate(host, deltaTime); // inherited P-key profiler dump

            // Smooth the frame time every frame + build the ImGui HUD (drawn in OnRenderWindow).
            m_frameMs = m_frameMs * 0.9f + (deltaTime * 1000.0f) * 0.1f;
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->NewFrame(host.Shell() != nullptr ? host.Shell()->Input() : nullptr, deltaTime);
                BuildHud(host.Ctx().GetSubsystem<render::RenderSubsystem>());
            }
            if (m_scene == nullptr)
            {
                return;
            }

            auto* input = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
            shell::IKeyboard* kb = input != nullptr ? input->Keyboard() : nullptr;
            if (kb == nullptr)
            {
                return;
            } // mouse-look is handled inside m_fly.Update

            if (kb->IsKeyPressed(shell::KeyCode::Escape))
            {
                host.RequestExit(0);
                return;
            }

            // --- load controls ---
            if (kb->IsKeyPressed(shell::KeyCode::Space))
            {
                AddSphereBatch();
            }
            if (kb->IsKeyPressed(shell::KeyCode::Backspace))
            {
                RemoveLastBatch();
            }
            if (kb->IsKeyPressed(shell::KeyCode::U))
            {
                m_uniqueMaterials = !m_uniqueMaterials;
                RebuildSphereMaterials();
                foundation::ConsoleWrite(m_uniqueMaterials
                                       ? u8"Unique materials: ON (a draw per sphere)\n"
                                       : u8"Unique materials: OFF (shared, batched)\n");
            }
            if (kb->IsKeyPressed(shell::KeyCode::B))
            {
                m_bob = !m_bob;
                foundation::ConsoleWrite(m_bob ? u8"Sin-wave bob: ON (transforms rewritten every frame)\n"
                                         : u8"Sin-wave bob: OFF\n");
            }
            if (kb->IsKeyPressed(shell::KeyCode::M))
            {
                m_multiMesh = !m_multiMesh;
                RebuildMultiMesh();
                foundation::ConsoleWrite(
                    m_multiMesh
                        ? u8"MultiMesh: ON (whole grid = ONE instanced set, O(1)/frame CPU)\n"
                        : u8"MultiMesh: OFF (per-entity spheres)\n");
            }
            if (kb->IsKeyPressed(shell::KeyCode::H))
            {
                m_showStats = !m_showStats;
            }
            if (kb->IsKeyPressed(shell::KeyCode::T))
            { // toggle TAA (activates per-instance motion-vector prev-world path)
                if (auto* render = host.Ctx().GetSubsystem<render::RenderSubsystem>())
                {
                    const bool on = !render->TaaEnabled();
                    render->SetTaaEnabled(on);
                    foundation::ConsoleWrite(on ? u8"TAA: ON (motion vectors active)\n" : u8"TAA: OFF\n");
                }
            }
            if (kb->IsKeyPressed(shell::KeyCode::I))
            { // toggle prepass->forward instance-data sharing (A/B regression/perf)
                if (auto* render = host.Ctx().GetSubsystem<render::RenderSubsystem>())
                {
                    const bool on = !render->InstanceSharing();
                    render->SetInstanceSharing(on);
                    foundation::ConsoleWrite(
                        on ? u8"Instance sharing: ON (prepass builds once, forward reuses)\n"
                           : u8"Instance sharing: OFF (forward re-fills = old double-build)\n");
                }
            }
            if (kb->IsKeyPressed(shell::KeyCode::K))
            { // toggle directional shadows (Sedulous's 104k demo runs shadow-OFF)
                if (auto* lights = m_scene->GetSystem<render::LightComponentManager>())
                {
                    if (render::LightComponent* sl =
                            m_sun.IsAssigned() ? lights->Get(m_sun) : nullptr)
                    {
                        sl->castsShadows = !sl->castsShadows;
                        foundation::ConsoleWrite(
                            sl->castsShadows
                                ? u8"Directional shadows: ON (CSM)\n"
                                : u8"Directional shadows: OFF (matches Sedulous stress test)\n");
                    }
                }
            }

            m_fly.Update(host, deltaTime);
            PushCameraToEntity();

            // --- sin-wave bob: rewrite every sphere's Y each frame (no static optimization possible) ---
            m_time += deltaTime;
            if (m_bob)
            {
                auto* meshScene = m_scene;
                constexpr foundation::f32 amplitude = 1.0f, speed = 2.0f;
                for (scene::EntityHandle e : m_spheres)
                {
                    foundation::Transform t = meshScene->GetLocalTransform(e);
                    // Phase from world X/Z (stable as the grid grows). Bob AROUND the base height so the
                    // spheres stay above the floor (full, separated shadows) instead of dipping through it.
                    const foundation::f32 phase = (t.position.x + t.position.z) * 0.2f;
                    t.position.y = kSphereHeight + foundation::Sin(m_time * speed + phase) * amplitude;
                    meshScene->SetLocalTransform(e, t);
                }
            }
        }

        void OnShutdown(runtime::IApplicationHost&) override
        {
            foundation::ConsoleWrite(u8"=== Stress Test Shutdown ===\n");
        }

    private:
        // Push the fly camera's pose onto the camera entity (the default render path reads it).
        void PushCameraToEntity()
        {
            foundation::Transform t = m_scene->GetLocalTransform(m_camera);
            t.position = m_fly.position;
            t.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, t);
        }

        // Grow the ground plane to cover the current grid, and re-frame the fly camera so the whole grid
        // is in view (called on every batch change) - like AnimStressTest. Keeps the ground under the
        // whole field and the far plane wide enough that no spheres get frustum-far-culled.
        void FitFloorAndCamera()
        {
            const foundation::f32 gridWidth =
                static_cast<foundation::f32>(m_gridSize) * kSphereSpacing; // full grid extent
            // Floor: scale the base plane so it covers the grid + a margin (uniform XZ; Y stays flat).
            const foundation::f32 scale = foundation::Max(0.1f, (gridWidth + 40.0f) / kFloorBaseSize);
            foundation::Transform ft = m_scene->GetLocalTransform(m_ground);
            ft.scale = foundation::Float3{scale, 1.0f, scale};
            m_scene->SetLocalTransform(m_ground, ft);

            // Camera: pull back + up so the grid fits the 60° FOV, looking down at the center.
            const foundation::f32 extent = gridWidth * 0.5f + 6.0f;
            const foundation::f32 dist = extent / foundation::Tan(0.5236f) + 10.0f; // half of 60° = 0.5236 rad
            const foundation::f32 camY = extent * 0.55f + kSphereHeight;
            m_fly.position = foundation::Float3{0.0f, kSphereHeight + camY, dist};
            m_fly.yaw = 0.0f;
            m_fly.pitch = -foundation::Atan2(camY, dist); // look down onto the grid center
            if (auto* cameras = m_scene->GetSystem<render::CameraComponentManager>())
            {
                if (render::CameraComponent* cam = cameras->Get(m_camera))
                {
                    cam->farZ =
                        dist + extent * 2.0f + 200.0f; // cover the grid; don't far-cull spheres
                }
            }
            PushCameraToEntity();
        }

        // Spawn 8000 more spheres on the auto-sized grid. Position only depends on a global index, so
        // existing spheres keep their world positions when the grid widens.
        // Grid position of sphere `index` for the current grid size (existing spheres keep their spot as
        // the grid widens; only new indices use the new size).
        [[nodiscard]] foundation::Float3 SphereTranslation(foundation::i32 index) const
        {
            const foundation::i32 gx = index % m_gridSize;
            const foundation::i32 gz = index / m_gridSize;
            const foundation::f32 x =
                (static_cast<foundation::f32>(gx) - static_cast<foundation::f32>(m_gridSize) * 0.5f) *
                kSphereSpacing;
            const foundation::f32 z =
                (static_cast<foundation::f32>(gz) - static_cast<foundation::f32>(m_gridSize) * 0.5f) *
                kSphereSpacing;
            return foundation::Float3{x, kSphereHeight, z};
        }

        void AddSphereBatch()
        {
            const foundation::i32 startIndex = m_batchCount * kSpheresPerBatch;
            const foundation::i32 newTotal = (m_batchCount + 1) * kSpheresPerBatch;
            m_gridSize =
                static_cast<foundation::i32>(foundation::Ceil(foundation::Sqrt(static_cast<foundation::f32>(newTotal))));

            if (m_multiMesh)
            {
                // MultiMesh: no per-entity entities exist - grow the canonical transform list + the set.
                m_mmTransforms.Reserve(static_cast<foundation::u32>(newTotal));
                for (foundation::i32 i = 0; i < kSpheresPerBatch; ++i)
                {
                    m_mmTransforms.PushBack(
                        foundation::Float4x4::Translation(SphereTranslation(startIndex + i)));
                }
            }
            else
            {
                auto* meshes = m_scene->GetSystem<render::MeshComponentManager>();
                if (meshes == nullptr)
                {
                    return;
                }
                for (foundation::i32 i = 0; i < kSpheresPerBatch; ++i)
                {
                    const foundation::i32 index = startIndex + i;
                    scene::EntityHandle e = m_scene->CreateEntity(u8"sphere");
                    m_scene->SetLocalPosition(e, SphereTranslation(index));
                    render::MeshComponent& mc = meshes->Add(e);
                    mc.mesh = m_sphere;
                    AssignSphereMaterial(mc, index);
                    m_spheres.PushBack(e);
                }
            }

            ++m_batchCount;
            FitFloorAndCamera();
            if (m_multiMesh)
            {
                PushMultiMesh();
            }
            PrintCounts();
        }

        void RemoveLastBatch()
        {
            if (m_batchCount <= 0)
            {
                return;
            }
            if (m_multiMesh)
            {
                foundation::usize remove = static_cast<foundation::usize>(kSpheresPerBatch);
                if (remove > m_mmTransforms.Size())
                {
                    remove = m_mmTransforms.Size();
                }
                m_mmTransforms.Resize(m_mmTransforms.Size() - remove);
            }
            else
            {
                foundation::i32 removeCount = kSpheresPerBatch;
                if (static_cast<foundation::usize>(removeCount) > m_spheres.Size())
                {
                    removeCount = static_cast<foundation::i32>(m_spheres.Size());
                }
                for (foundation::i32 i = 0; i < removeCount; ++i)
                {
                    m_scene->DestroyEntity(m_spheres[m_spheres.Size() - 1]);
                    m_spheres.PopBack();
                    if (m_uniqueMaterials && !m_uniqueMats.IsEmpty())
                    {
                        m_uniqueMats.PopBack();
                    }
                }
            }
            --m_batchCount;
            FitFloorAndCamera();
            if (m_multiMesh)
            {
                PushMultiMesh();
            }
            PrintCounts();
        }

        // Point a sphere's mesh component at the right material for the current mode.
        void AssignSphereMaterial(render::MeshComponent& mc, foundation::i32 index)
        {
            if (m_uniqueMaterials)
            {
                const foundation::f32 hue = static_cast<foundation::f32>(index % 360) / 360.0f;
                const foundation::Float3 c = HsvToRgb(hue, 0.8f, 0.9f);
                foundation::RefPtr<materials::Material> m = materials::CreatePBR(
                    u8"stress.unique", foundation::Float4{c.x, c.y, c.z, 1.0f}, 0.1f, 0.4f);
                mc.SetMaterial(m);
                mc.color = foundation::Color{1.0f, 1.0f, 1.0f, 1.0f};
                m_uniqueMats.PushBack(static_cast<foundation::RefPtr<materials::Material>&&>(m));
            }
            else
            {
                mc.SetMaterial(m_sharedMat);
                mc.color = foundation::Color{1.0f, 1.0f, 1.0f, 1.0f};
            }
        }

        // Toggling unique mode re-points every existing sphere. Rebuild from scratch so material count
        // tracks the mode exactly (in shared mode we drop all the unique materials).
        void RebuildSphereMaterials()
        {
            auto* meshes = m_scene->GetSystem<render::MeshComponentManager>();
            if (meshes == nullptr)
            {
                return;
            }
            m_uniqueMats.Clear();
            for (foundation::usize i = 0; i < m_spheres.Size(); ++i)
            {
                if (render::MeshComponent* mc = meshes->Get(m_spheres[i]))
                {
                    AssignSphereMaterial(*mc, static_cast<foundation::i32>(i));
                }
            }
        }

        // Update the single InstancedMeshComponent from the canonical transform list (creates the set
        // entity on first use, destroys it when the list is empty). Called after m_mmTransforms changes.
        void PushMultiMesh()
        {
            auto* imm = m_scene->GetSystem<render::InstancedMeshComponentManager>();
            if (imm == nullptr)
            {
                return;
            }
            if (m_mmTransforms.IsEmpty())
            {
                if (m_multiMeshEntity.IsAssigned())
                {
                    m_scene->DestroyEntity(m_multiMeshEntity);
                    m_multiMeshEntity = {};
                }
                return;
            }
            if (!m_multiMeshEntity.IsAssigned())
            {
                m_multiMeshEntity = m_scene->CreateEntity(u8"multimesh");
            }
            render::InstancedMeshComponent& c = imm->Has(m_multiMeshEntity)
                                                    ? *imm->Get(m_multiMeshEntity)
                                                    : imm->Add(m_multiMeshEntity);
            c.mesh = m_sphere;
            c.material = m_sharedMat;
            c.SetInstances(
                foundation::Span<const foundation::Float4x4>{m_mmTransforms.Data(), m_mmTransforms.Size()});
        }

        // Convert between per-entity spheres and the single MultiMesh set when the mode flips. Entering
        // MultiMesh: capture the spheres' world transforms, DESTROY the per-entity entities (so extraction
        // never iterates them - a genuine O(1) extract), build the set. Leaving: destroy the set + respawn
        // the per-entity spheres from the captured transforms. AddSphereBatch/RemoveLastBatch then operate
        // on whichever representation is live, so the count always matches what's drawn.
        void RebuildMultiMesh()
        {
            auto* meshes = m_scene->GetSystem<render::MeshComponentManager>();
            if (meshes == nullptr)
            {
                return;
            }

            if (m_multiMesh)
            {
                m_mmTransforms.Clear();
                m_mmTransforms.Reserve(static_cast<foundation::u32>(m_spheres.Size()));
                for (scene::EntityHandle e : m_spheres)
                {
                    m_mmTransforms.PushBack(m_scene->GetWorldMatrix(e));
                }
                for (scene::EntityHandle e : m_spheres)
                {
                    m_scene->DestroyEntity(e);
                }
                m_spheres.Clear();
                m_uniqueMats
                    .Clear(); // per-entity unique materials are gone; the set uses one shared material
                PushMultiMesh();
            }
            else
            {
                if (m_multiMeshEntity.IsAssigned())
                {
                    m_scene->DestroyEntity(m_multiMeshEntity);
                    m_multiMeshEntity = {};
                }
                m_spheres.Reserve(static_cast<foundation::u32>(m_mmTransforms.Size()));
                for (foundation::usize i = 0; i < m_mmTransforms.Size(); ++i)
                {
                    const foundation::Float4x4& xf =
                        m_mmTransforms[i]; // translation-only spheres: read the position row
                    scene::EntityHandle e = m_scene->CreateEntity(u8"sphere");
                    m_scene->SetLocalPosition(e, foundation::Float3{xf.m[3][0], xf.m[3][1], xf.m[3][2]});
                    render::MeshComponent& mc = meshes->Add(e);
                    mc.mesh = m_sphere;
                    AssignSphereMaterial(mc, static_cast<foundation::i32>(i));
                    m_spheres.PushBack(e);
                }
                m_mmTransforms.Clear();
            }
            PrintCounts();
        }

        void PrintCounts()
        {
            const foundation::i32 count = m_multiMesh ? static_cast<foundation::i32>(m_mmTransforms.Size())
                                                : static_cast<foundation::i32>(m_spheres.Size());
            foundation::String s;
            foundation::AppendFormat(s, u8"  spheres {}  batches {}  grid {}x{}  materials {}{}\n", count,
                               m_batchCount, m_gridSize, m_gridSize,
                               (m_multiMesh || !m_uniqueMaterials)
                                   ? 1
                                   : static_cast<foundation::i32>(m_uniqueMats.Size()),
                               m_multiMesh ? u8"  [multimesh]" : u8"");
            foundation::ConsoleWrite(s.AsView());
        }

        // ImGui HUD: benchmark stats + controls (H toggles it). Built in OnUpdate, drawn in OnRenderWindow.
        void BuildHud(render::RenderSubsystem* render)
        {
            if (!m_showStats)
            {
                return;
            }
            ImGui::Begin("Render Stress Test");
            const float fps = m_frameMs > 0.001f ? 1000.0f / m_frameMs : 0.0f;
            ImGui::Text("%.0f fps   %.2f ms", static_cast<double>(fps),
                        static_cast<double>(m_frameMs));
            ImGui::Text("%s %d   batches %d   grid %dx%d", m_multiMesh ? "instances" : "spheres",
                        static_cast<int>(m_multiMesh ? m_mmTransforms.Size() : m_spheres.Size()),
                        m_batchCount, m_gridSize, m_gridSize);
            ImGui::Separator();
            if (ImGui::Button("+ batch (Space)"))
            {
                AddSphereBatch();
            }
            ImGui::SameLine();
            if (ImGui::Button("- batch (Backspace)"))
            {
                RemoveLastBatch();
            }
            // Scene toggles (also the hot-keys U/B/T/I/K).
            bool uniq = m_uniqueMaterials;
            if (ImGui::Checkbox("Unique materials (U)", &uniq))
            {
                m_uniqueMaterials = uniq;
                RebuildSphereMaterials();
            }
            ImGui::Checkbox("Sin-wave bob (B)", &m_bob);
            bool mm = m_multiMesh;
            if (ImGui::Checkbox("MultiMesh: whole grid as one instanced set (M)", &mm))
            {
                m_multiMesh = mm;
                RebuildMultiMesh();
            }
            if (m_multiMesh)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("O(1)/frame CPU");
            }
            if (render != nullptr)
            {
                bool taa = render->TaaEnabled();
                if (ImGui::Checkbox("TAA (T)", &taa))
                {
                    render->SetTaaEnabled(taa);
                }
                bool inst = render->InstanceSharing();
                if (ImGui::Checkbox("Instance sharing (I)", &inst))
                {
                    render->SetInstanceSharing(inst);
                }
                bool cull = render->ViewCulling();
                if (ImGui::Checkbox("View-frustum cull", &cull))
                {
                    render->SetViewCulling(cull);
                }
                if (cull)
                {
                    foundation::u32 culled = 0, total = 0;
                    render->ViewCullStats(culled, total);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%u/%u culled)", culled, total);
                }
            }
            if (m_scene != nullptr)
            {
                if (auto* lights = m_scene->GetSystem<render::LightComponentManager>())
                {
                    if (render::LightComponent* sl =
                            m_sun.IsAssigned() ? lights->Get(m_sun) : nullptr)
                    {
                        bool sh = sl->castsShadows;
                        if (ImGui::Checkbox("Directional shadows (K)", &sh))
                        {
                            sl->castsShadows = sh;
                        }
                    }
                }
            }
            if (render != nullptr)
            {
                ImGui::Separator();
                float exposure = render->Exposure();
                if (ImGui::SliderFloat("Exposure", &exposure, 0.05f, 4.0f))
                {
                    render->SetExposure(exposure);
                }
                bool bloomOn = render->BloomEnabled();
                if (ImGui::Checkbox("Bloom", &bloomOn))
                {
                    render->SetBloomEnabled(bloomOn);
                }
                float shadowDist = render->ShadowDistance();
                if (ImGui::SliderFloat("Shadow dist", &shadowDist, 50.0f, 1000.0f, "%.0f"))
                {
                    render->SetShadowDistance(shadowDist);
                }
                float shadowFade = render->ShadowFarFade();
                if (ImGui::SliderFloat("Shadow fade", &shadowFade, 2.0f, 150.0f, "%.0f"))
                {
                    render->SetShadowFarFade(shadowFade);
                }
            }
            ImGui::Separator();
            ImGui::TextUnformatted("H hide HUD   P profiler");
            ImGui::TextUnformatted("WASD/QE move   RMB look   Shift fast   Esc exit");
            ImGui::End();
        }

        static foundation::Float3 HsvToRgb(foundation::f32 h, foundation::f32 s, foundation::f32 v)
        {
            const foundation::i32 i = static_cast<foundation::i32>(h * 6.0f);
            const foundation::f32 f = h * 6.0f - static_cast<foundation::f32>(i);
            const foundation::f32 p = v * (1.0f - s);
            const foundation::f32 q = v * (1.0f - f * s);
            const foundation::f32 t = v * (1.0f - (1.0f - f) * s);
            switch (i % 6)
            {
            case 0:
                return foundation::Float3{v, t, p};
            case 1:
                return foundation::Float3{q, v, p};
            case 2:
                return foundation::Float3{p, v, t};
            case 3:
                return foundation::Float3{p, q, v};
            case 4:
                return foundation::Float3{t, p, v};
            default:
                return foundation::Float3{v, p, q};
            }
        }

        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_camera{};
        scene::EntityHandle m_sun{};
        scene::EntityHandle m_ground{};
        foundation::RefPtr<geometry::StaticMesh> m_sphere;
        foundation::RefPtr<materials::Material> m_sharedMat;
        foundation::Array<scene::EntityHandle> m_spheres;
        foundation::Array<foundation::RefPtr<materials::Material>> m_uniqueMats;

        foundation::i32 m_batchCount = 0;
        foundation::i32 m_gridSize = 0;
        bool m_uniqueMaterials = false;
        bool m_bob = false;
        bool m_multiMesh = false; // M: draw the whole grid as ONE InstancedMeshComponent
        scene::EntityHandle m_multiMeshEntity{}; // the single set entity (when m_multiMesh)
        foundation::Array<foundation::Float4x4>
            m_mmTransforms; // canonical instance transforms while in MultiMesh mode
                            // (the per-entity spheres are DESTROYED, not hidden)
        foundation::f32 m_time = 0.0f;

        // Fly camera, pulled well back + up so the whole grid is in frame (worst case for culling).
        samples::FlyCamera m_fly{.position = foundation::Float3{0.0f, 50.0f, 200.0f}, .pitch = -0.245f};

        // Stats
        bool m_showStats = true; // HUD visibility (H)
        foundation::f32 m_frameMs = 0.0f;
    };
}

int main(int, char**)
{
    auto shell = shell::CreateShell();
    graphics::GraphicsDeviceDesc gpuDesc{};
    auto gpu = graphics::CreateGraphicsDevice(gpuDesc);
    graphics::GraphicsDevice* device = gpu.HasValue() ? gpu.Value().Get() : nullptr;

    StressTestApp app;
    return runtime::RunApplication(app, *shell, device);
}
