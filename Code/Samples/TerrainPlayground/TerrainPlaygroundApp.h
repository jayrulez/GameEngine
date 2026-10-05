// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// TerrainPlaygroundApp - the terrain showcase, shared by the desktop and web entry points
// (Main.cpp / WebMain.cpp - the WebSceneApp.h pattern: this header uses the modules the including
// TU imports, so it declares NO imports of its own). Builds a heightfield IN CODE, wraps it in an
// in-memory TerrainResource (no cooking - Ref<> sub-resources take a direct product), attaches a
// TerrainComponent, and lets engine.terrain draw the chunked geo-mipmap terrain through its
// dedicated TerrainRenderer. An ImGui HUD tweaks the sun / LOD bias / heightfield live. WASD/RMB
// fly camera.
#ifndef SAMPLES_TERRAINPLAYGROUND_APP_H
#define SAMPLES_TERRAINPLAYGROUND_APP_H

#include "../Common/FlyCamera.h" // shared free-fly camera (WASD/QE + RMB-look)
#include "imgui.h"

namespace
{
    // This binary's composition root: the ONE ambient-allocator decision here.
    [[nodiscard]] foundation::core::IAllocator& AppRoot() noexcept
    {
        return foundation::core::DefaultAllocator();
    }
}


namespace samples
{
    namespace core = foundation::core;
    namespace runtime = foundation::runtime;
    namespace graphics = foundation::graphics;
    namespace scene = foundation::scene;
    namespace imgui = extensions::imgui;
    namespace hf = foundation::heightfield;
    namespace terrain = foundation::terrain;
    namespace geometry = foundation::geometry;
    namespace materials = foundation::materials;
    using namespace foundation::core; // RefPtr, MakeRef, DefaultAllocator, math helpers



    enum class TerrainType : int
    {
        Hills = 0,
        Dome = 1,
        Ripple = 2,
        Plateau = 3
    };

    constexpr core::i32 kGridSize = 257;      // 4x4 chunks (64k+1)
    constexpr core::f32 kWorldSize = 256.0f;  // XZ footprint
    constexpr core::f32 kMaxHeight = 60.0f;   // world Y range top
    constexpr core::f32 kCasterRadius = 8.0f; // the orbiting shadow-caster sphere

    // Rewrite every sample from the current controls, then BumpVersion so the GPU height texture and
    // the chunk model re-upload. Pattern in [0,1] -> scaled by `amplitude` -> the u16 range.
    inline void GenerateHeightfield(hf::Heightfield& grid, TerrainType type, core::f32 amplitude,
                                    core::f32 frequency)
    {
        const core::i32 n = grid.Size();
        const core::f32 c = static_cast<core::f32>(n - 1) * 0.5f;
        for (core::i32 z = 0; z < n; ++z)
        {
            for (core::i32 x = 0; x < n; ++x)
            {
                const core::f32 fx = static_cast<core::f32>(x);
                const core::f32 fz = static_cast<core::f32>(z);
                const core::f32 dx = (fx - c) / c; // -1..1 from center
                const core::f32 dz = (fz - c) / c;
                const core::f32 r = core::Min(core::Sqrt(dx * dx + dz * dz), 1.0f);
                core::f32 h = 0.0f;
                switch (type)
                {
                case TerrainType::Hills:
                    // Layered sines - a rolling landscape.
                    h = 0.5f + 0.25f * core::Sin(fx * frequency) * core::Cos(fz * frequency) +
                        0.15f * core::Sin(fx * frequency * 2.3f + 1.7f) +
                        0.10f * core::Cos(fz * frequency * 3.1f);
                    break;
                case TerrainType::Dome:
                    h = core::Cos(r * 1.5707963f); // 1 center .. 0 rim (smooth mound)
                    break;
                case TerrainType::Ripple:
                    h = 0.5f + 0.5f * core::Sin(r * 20.0f * frequency) * (1.0f - r);
                    break;
                case TerrainType::Plateau:
                    h = core::Clamp(1.5f - r * 2.2f, 0.0f, 1.0f); // flat top, sloped skirt
                    break;
                }
                h = core::Clamp(h * amplitude, 0.0f, 1.0f);
                grid.SetSample(x, z, static_cast<hf::Height>(h * 65535.0f));
            }
        }
        grid.BumpVersion();
    }

    constexpr core::i32 kSplatSize = 128;

    // Palette layer 0 one-hot on the x < 0 half; the base owns the rest.
    inline void PaintHalfSplat(terrain::SplatWeights& splat)
    {
        core::Span<core::u8> idx = splat.Indices();
        core::Span<core::u8> wts = splat.Weights();
        for (core::i32 y = 0; y < splat.Height(); ++y)
        {
            for (core::i32 x = 0; x < splat.Width() / 2; ++x)
            {
                const core::usize at = splat.TexelOffset(x, y);
                idx[at + 0] = 0;
                wts[at + 0] = 255;
            }
        }
        splat.BumpVersion();
    }

    class TerrainPlaygroundApp final : public engine::runtime::DefaultApplication
    {
    public:
        void Configure(runtime::IApplicationHost& host) override
        {
            engine::runtime::DefaultApplication::Configure(host);
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                host.Ctx().AddSubsystem<imgui::ImguiSubsystem>(*gfx->Raw(), gfx->FramesInFlight(),
                                                                    DataFileSystem());
            }
        }

        void OnStartup(runtime::IApplicationHost& host) override
        {
            auto* scenes = host.Ctx().GetSubsystem<engine::scene::SceneSubsystem>();
            if (scenes == nullptr)
            {
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"terrain");
            if (auto* env = m_scene->GetSystem<engine::render::EnvironmentSystem>())
            {
                env->Environment().ambientColor = core::Color{0.45f, 0.52f, 0.62f, 1.0f};
                env->Environment().ambientIntensity = 0.5f;
            }

            // Camera - high + pulled back, angled down over the terrain.
            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
            {
                engine::render::CameraComponent& cam = cameras->Add(m_camera);
                cam.clearColor = core::Color{0.55f, 0.70f, 0.90f, 1.0f}; // sky
            }
            m_fly.position = core::Float3{0.0f, 110.0f, 175.0f};
            m_fly.pitch = -0.45f;

            // The sun - a directional light the terrain shades against (driven by the HUD sliders).
            m_sun = m_scene->CreateEntity(u8"sun");
            if (auto* lights = m_scene->GetSystem<engine::render::LightComponentManager>())
            {
                engine::render::LightComponent& lc = lights->Add(m_sun);
                lc.type = engine::render::LightType::Directional;
                lc.color = core::Color{1.0f, 0.96f, 0.88f, 1.0f};
                lc.intensity = 1.0f;
                lc.castsShadows = true; // drives the CSM - terrain casts + receives
            }
            ApplySun();

            // The terrain: an in-memory heightfield + TerrainResource (direct product, no cook).
            m_heightfield =
                MakeRef<hf::Heightfield>(AppRoot(), kGridSize,
                                         core::Float2{kWorldSize, kWorldSize}, 0.0f, kMaxHeight);
            GenerateHeightfield(*m_heightfield, m_type, m_amplitude, m_frequency);
            ApplyHole();

            m_terrainResource = MakeRef<terrain::TerrainResource>(AppRoot());
            m_terrainResource->heightfield = m_heightfield; // Ref direct product (no proxy/guid)
            // A splat with palette layer 0 painted on the x < 0 half (no palette textures: the
            // terrain itself still shades from the height ramp; the paint drives the GRASS).
            m_splat = MakeRef<terrain::SplatWeights>(AppRoot(), kSplatSize, kSplatSize);
            PaintHalfSplat(*m_splat);
            m_terrainResource->weights = m_splat;

            m_terrain = m_scene->CreateEntity(u8"terrain");
            if (auto* mgr = m_scene->GetSystem<engine::terrain::TerrainComponentManager>())
            {
                engine::terrain::TerrainComponent& tc = mgr->Add(m_terrain);
                tc.terrain = m_terrainResource;
                tc.lodBias = m_lodBias;
            }

            // The grass: the terrain's vegetation component with one layer following splat
            // layer 0 - grass on the painted half, none on the other, thinning to nothing at
            // fadeEnd.
            if (auto* vegetation =
                    m_scene->GetSystem<engine::vegetation::TerrainVegetationComponentManager>())
            {
                engine::vegetation::TerrainVegetationComponent& c = vegetation->Add(m_terrain);
                engine::vegetation::ProceduralVegetationLayer layer;
                layer.name = core::String(u8"Grass");
                layer.mesh = geometry::Primitives::Cone(AppRoot(), 0.24f, 1.4f); // a tuft
                core::RefPtr<materials::Material> grass = materials::CreatePBR(
                    u8"grass", core::Float4{0.25f, 0.62f, 0.18f, 1.0f}, 0.0f, 0.85f);
                // Wind: the tufts sway (their tips; the cone's base sits at local y = -0.7).
                grass->SetDefaultFloat(u8"WindStrength", 0.18f);
                grass->SetDefaultFloat(u8"WindSpeed", 2.4f);
                grass->SetDefaultFloat(u8"WindHeight", 0.7f);
                layer.materials.PushBack(foundation::resource::Ref<materials::Material>(grass));
                layer.placement = foundation::vegetation::VegetationPlacement::Splat;
                layer.splatLayer = 0;
                layer.density = m_grassDensity;
                layer.scaleRange = core::Float2{0.7f, 1.4f};
                layer.maxSlopeDegrees = 40.0f;
                layer.fadeStart = m_grassFadeStart;
                layer.fadeEnd = m_grassFadeEnd;
                layer.castShadows = false;
                c.proceduralLayers.PushBack(layer);
            }

            // A floating sphere that orbits over the terrain - a moving shadow caster so the CSM is
            // obvious (its shadow sweeps across the surface). Meshes cast automatically (Opaque).
            m_caster = m_scene->CreateEntity(u8"caster");
            if (auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>())
            {
                engine::render::MeshComponent& mc = meshes->Add(m_caster);
                mc.mesh = geometry::Primitives::Sphere(AppRoot(), kCasterRadius);
                mc.SetMaterial(materials::CreatePBR(u8"caster", core::Float4{0.9f, 0.3f, 0.2f, 1.0f},
                                                    0.0f, 0.5f));
            }
            ApplyCaster();
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            engine::runtime::DefaultApplication::OnRenderWindow(host, frame);
            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->Render(frame);
            }
        }

        void OnUpdate(runtime::IApplicationHost& host, core::f32 deltaTime) override
        {
            engine::runtime::DefaultApplication::OnUpdate(host, deltaTime);
            m_frameSmooth = m_frameSmooth * 0.9f + deltaTime * 0.1f;

            if (auto* g = host.Ctx().GetSubsystem<imgui::ImguiSubsystem>())
            {
                g->NewFrame(host.Shell() != nullptr ? host.Shell()->Input() : nullptr, deltaTime);
                BuildHud();
            }

            m_fly.Update(host, deltaTime);
            if (m_scene != nullptr)
            {
                core::Transform camT = m_scene->GetLocalTransform(m_camera);
                camT.position = m_fly.position;
                camT.rotation = m_fly.Rotation();
                m_scene->SetLocalTransform(m_camera, camT);

                if (m_orbit)
                {
                    m_orbitTime += deltaTime * m_orbitSpeed;
                }
                ApplyCaster(); // re-apply each frame so height/radius sliders track live
            }
        }

    private:
        void ApplySun()
        {
            if (m_scene == nullptr)
            {
                return;
            }
            // Light forward = travel direction: yaw about Y, pitch down by the elevation.
            const core::Quaternion rot =
                core::Quaternion::FromAxisAngle(core::Float3{0.0f, 1.0f, 0.0f}, m_sunAzimuth) *
                core::Quaternion::FromAxisAngle(core::Float3{1.0f, 0.0f, 0.0f}, -m_sunElevation);
            core::Transform t = m_scene->GetLocalTransform(m_sun);
            t.rotation = rot;
            m_scene->SetLocalTransform(m_sun, t);
        }

        void ApplyCaster()
        {
            if (m_scene == nullptr)
            {
                return;
            }
            m_scene->SetLocalPosition(
                m_caster, core::Float3{m_orbitRadius * core::Cos(m_orbitTime), m_casterHeight,
                                       m_orbitRadius * core::Sin(m_orbitTime)});
        }

        void BuildHud()
        {
            ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("TerrainPlayground"))
            {
                ImGui::Text("fps: %.0f", 1.0f / core::Max(m_frameSmooth, 0.0001f));
                ImGui::Text("grid: %d x %d  (%.0f x %.0f m)", kGridSize, kGridSize, kWorldSize,
                            kWorldSize);
                ImGui::Separator();

                ImGui::TextDisabled("Sun");
                bool sunChanged = ImGui::SliderAngle("azimuth", &m_sunAzimuth, 0.0f, 360.0f);
                sunChanged |= ImGui::SliderAngle("elevation", &m_sunElevation, 5.0f, 90.0f);
                if (sunChanged)
                {
                    ApplySun();
                }

                ImGui::Separator();
                ImGui::TextDisabled("Level of detail");
                if (ImGui::SliderFloat("LOD bias", &m_lodBias, -2.0f, 4.0f, "%.2f"))
                {
                    if (auto* mgr =
                            m_scene->GetSystem<engine::terrain::TerrainComponentManager>())
                    {
                        if (auto* tc = mgr->Get(m_terrain))
                        {
                            tc->lodBias = m_lodBias;
                        }
                    }
                }
                ImGui::TextDisabled("+ = coarser sooner, - = hold detail out");

                ImGui::Separator();
                ImGui::TextDisabled("Heightfield");
                const char* kTypes[] = {"Hills", "Dome", "Ripple", "Plateau"};
                int type = static_cast<int>(m_type);
                bool regen = ImGui::Combo("type", &type, kTypes, 4);
                m_type = static_cast<TerrainType>(type);
                regen |= ImGui::SliderFloat("amplitude", &m_amplitude, 0.05f, 1.0f, "%.2f");
                regen |= ImGui::SliderFloat("frequency", &m_frequency, 0.01f, 0.3f, "%.3f");
                if (ImGui::Button("Regenerate") || regen)
                {
                    if (m_heightfield)
                    {
                        GenerateHeightfield(*m_heightfield, m_type, m_amplitude, m_frequency);
                    }
                }
                // A hole (Specs/terrain-holes.md): a cut disc off-centre; nothing draws, casts,
                // collides or grows there, and the sky shows through from above.
                bool holeChanged = ImGui::Checkbox("hole (a cut disc at -30, 70)", &m_hole);
                holeChanged |= ImGui::SliderFloat("hole radius", &m_holeRadius, 2.0f, 40.0f, "%.0f");
                if (holeChanged)
                {
                    ApplyHole();
                }

                ImGui::Separator();
                ImGui::TextDisabled("Grass (splat layer 0: the x < 0 half)");
                if (auto* vegetation =
                        m_scene->GetSystem<engine::vegetation::TerrainVegetationComponentManager>())
                {
                    auto* c = vegetation->Get(m_terrain);
                    if (auto* layer = (c != nullptr && !c->proceduralLayers.IsEmpty()) ? &c->proceduralLayers[0] : nullptr)
                    {
                        bool changed = ImGui::SliderFloat("density /m2", &m_grassDensity, 0.0f,
                                                          4.0f, "%.2f");
                        changed |= ImGui::SliderFloat("fade start", &m_grassFadeStart, 0.0f,
                                                      300.0f, "%.0f");
                        changed |= ImGui::SliderFloat("fade end", &m_grassFadeEnd, 10.0f, 400.0f,
                                                      "%.0f");
                        changed |= ImGui::Checkbox("grass casts shadows", &layer->castShadows);
                        changed |= ImGui::Checkbox("grass visible", &layer->visible);
                        if (changed)
                        {
                            layer->density = m_grassDensity;
                            layer->fadeStart = m_grassFadeStart;
                            layer->fadeEnd = core::Max(m_grassFadeEnd, m_grassFadeStart + 1.0f);
                        }
                        ImGui::Text("sets built %u, instances %u",
                                    static_cast<unsigned>(vegetation->BuiltSetCount()),
                                    static_cast<unsigned>(vegetation->InstanceCount()));
                    }
                }

                ImGui::Separator();
                ImGui::TextDisabled("Shadow caster (watch its shadow sweep the terrain)");
                ImGui::Checkbox("orbit", &m_orbit);
                ImGui::SliderFloat("caster height", &m_casterHeight, 12.0f, 130.0f, "%.0f");
                ImGui::SliderFloat("orbit radius", &m_orbitRadius, 0.0f, 115.0f, "%.0f");

                ImGui::Separator();
                ImGui::TextDisabled("WASD/QE fly, hold RMB to look, Shift = fast.");
            }
            ImGui::End();
        }

        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_camera{};
        scene::EntityHandle m_sun{};
        scene::EntityHandle m_terrain{};
        scene::EntityHandle m_caster{};
        RefPtr<hf::Heightfield> m_heightfield;
        RefPtr<terrain::SplatWeights> m_splat;
        RefPtr<terrain::TerrainResource> m_terrainResource;
        FlyCamera m_fly;
        core::f32 m_frameSmooth = 0.016f;

        // The hole: the whole plane solid, then one cut disc when enabled (the sample kept
        // separate from GenerateHeightfield so a regenerate keeps the cut where it is).
        void ApplyHole()
        {
            if (!m_heightfield)
            {
                return;
            }
            core::Array<core::u8> solid;
            solid.Resize(m_heightfield->Holes().Size());
            (void)m_heightfield->SetHoles(core::Span<const core::u8>(solid.Data(), solid.Size()));
            if (m_hole)
            {
                (void)hf::CutHoles(*m_heightfield, -30.0f, 70.0f, m_holeRadius);
            }
            m_heightfield->BumpVersion();
        }

        // HUD state.
        bool m_hole = true;
        core::f32 m_holeRadius = 16.0f;
        core::f32 m_sunAzimuth = 0.7f;
        core::f32 m_sunElevation = 0.9f;
        core::f32 m_lodBias = 0.0f;
        TerrainType m_type = TerrainType::Hills;
        core::f32 m_amplitude = 0.7f;
        core::f32 m_frequency = 0.06f;
        bool m_orbit = true;
        core::f32 m_orbitTime = 0.0f;
        core::f32 m_orbitSpeed = 0.6f;   // radians/sec
        core::f32 m_casterHeight = 70.0f;
        core::f32 m_orbitRadius = 75.0f;
        core::f32 m_grassDensity = 1.5f;
        core::f32 m_grassFadeStart = 90.0f;
        core::f32 m_grassFadeEnd = 180.0f;
    };
}

#endif // SAMPLES_TERRAINPLAYGROUND_APP_H
