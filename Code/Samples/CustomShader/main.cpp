// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// CustomShader - a LOW-LEVEL custom shader end to end: a toon (cel) shader, one file with VSMain
// and PSMain (Data/Samples/Shaders/toon/toon.hlsl), registered with the render subsystem's shader
// system under the name "toon", and a material built in code whose properties lay out the
// shader's set-2 block. Nothing in the renderer knows about it: the material's property list
// derives its bind-group layout and pipeline (the material-driven renderer), and the shader keeps
// the frame's contract (sets 0, 1 and 3, the vertex input and the five G-buffer targets).
//
// What it exercises: distinct draws and an instanced batch of one material (the INSTANCED vertex
// variant), a masked material (the ALPHA_TEST variant), a sun's cascaded shadows and a point
// light's cube shadow received by toon surfaces, toon casters in those shadows (the built-in
// depth shaders), TAA (the motion vectors the toon pixel shader writes) and, beside it, the
// engine's PBR for comparison, and the glTF Fox, skinned and walking, in toon (the SKINNED
// variant). WASD/RMB fly; Esc quits.

#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Runtime.Client/AppMain.h"
#include <cmath>

import foundation.core;
import foundation.rhi; // Device (the texture factory the cooked Fox needs)
import foundation.vfs; // DataPath (engine data lives under the app-resolved data root)
import foundation.runtime;
import foundation.runtime.client;
import engine.defaultapp;
import foundation.shell;
import foundation.runtime.desktop;
import foundation.shell.desktop;
import foundation.graphics;
import foundation.graphics.gpu;
import foundation.scene;
import foundation.geometry;
import foundation.materials;
import foundation.shaders;
import foundation.shaders.system;
import engine.scene;
import engine.render;
import engine.composition;  // FullComposition: the factory set every host composes from
import foundation.content;  // ContentDatabase (the cooked Fox)
import foundation.resource; // ResourceManager + Proxy
import foundation.model;    // ModelResource
import foundation.animation;
import engine.animation;    // SkeletalAnimationComponent: the SKINNED vertex variant
import modelimporter;       // LoadAndCook

#include "../Common/FlyCamera.h" // after the imports: uses foundation::core/runtime types

namespace core = foundation::core;
namespace runtime = foundation::runtime;
namespace graphics = foundation::graphics;
namespace shell = foundation::shell;
namespace scene = foundation::scene;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;
namespace vfs = foundation::vfs;
namespace content = foundation::content;
namespace resource = foundation::resource;
namespace model = foundation::model;
namespace animation = foundation::animation;

using core::f32;

namespace
{
    // This binary's composition root: the ONE ambient-allocator decision here.
    [[nodiscard]] core::IAllocator& AppRoot() noexcept
    {
        return core::DefaultAllocator();
    }

    // The toon material: the properties toon.hlsl's block declares, in its order, so the builder
    // lays them out where the generated set-2 block puts them (both pack by HLSL's rules), then
    // the albedo map and its sampler, t0 and s0.
    [[nodiscard]] core::RefPtr<materials::Material> CreateToon(core::StringView name,
                                                               core::Float4 baseColor,
                                                               core::Float4 shadowColor,
                                                               f32 bands = 3.0f)
    {
        return materials::MaterialBuilder(name)
            .Shader(u8"toon")
            .VertexLayout(materials::VertexLayoutType::Mesh)
            .Color(u8"BaseColor", baseColor)
            .Color(u8"ShadowColor", shadowColor)
            .Color(u8"RimColor", core::Float4{1.0f, 0.95f, 0.85f, 1.0f})
            .Float(u8"Bands", bands)
            .Float(u8"RimPower", 3.0f)
            .Float(u8"RimIntensity", 0.6f)
            .Float(u8"AlphaCutoff", 0.5f)
            .Texture(u8"AlbedoMap")
            .Sampler(u8"MainSampler")
            .Build();
    }

    class CustomShaderApp final : public engine::runtime::DefaultApplication
    {
    public:
        void OnStartup(runtime::IApplicationHost& host) override
        {
            auto* render = host.Subsystems().GetSubsystem<engine::render::RenderSubsystem>();
            if (render == nullptr || render->Shaders() == nullptr ||
                !RegisterToonShader(*render->Shaders()))
            {
                return;
            }
            m_scene = PrimaryScenes().CreateScene(u8"custom-shader");
            if (auto* env = m_scene->GetSystem<engine::render::EnvironmentSystem>())
            {
                env->Environment().ambientColor = core::Color{0.45f, 0.5f, 0.6f, 1.0f};
                env->Environment().ambientIntensity = 0.6f;
            }
            if (auto* post = m_scene->GetSystem<engine::render::PostProcessSystem>())
            {
                post->Post().aaMode = engine::render::AaMode::TAA; // the toon motion vectors in use
            }

            m_camera = m_scene->CreateEntity(u8"camera");
            if (auto* cameras = m_scene->GetSystem<engine::render::CameraComponentManager>())
            {
                cameras->Add(m_camera);
            }
            m_fly.position = core::Float3{0.0f, 4.0f, 13.0f};
            m_fly.pitch = -0.25f;
            m_fly.moveSpeed = 6.0f;
            m_fly.fastSpeed = 18.0f;

            BuildScene();
            BuildLights();
            SpawnToonFox(host);

            m_scene->Start();
            m_scene->SetSimulationEnabled(true);
            core::ConsoleWrite(u8"CustomShader: a toon shader from Data/Samples/Shaders/toon.\n"
                               u8"WASD/RMB-look fly, Esc quits.\n");
        }

        void OnUpdate(runtime::IApplicationHost& host, f32 deltaTime) override
        {
            engine::runtime::DefaultApplication::OnUpdate(host, deltaTime);
            m_fly.Update(host, deltaTime);
            if (m_scene == nullptr)
            {
                return;
            }
            core::Transform t = m_scene->GetLocalTransform(m_camera);
            t.position = m_fly.position;
            t.rotation = m_fly.Rotation();
            m_scene->SetLocalTransform(m_camera, t);

            // Motion for TAA to reproject: the ring of cubes turns, the torus spins.
            m_time += deltaTime;
            core::Transform ring = m_scene->GetLocalTransform(m_ring);
            ring.rotation = core::Quaternion::FromAxisAngle(core::Float3{0, 1, 0}, m_time * 0.4f);
            m_scene->SetLocalTransform(m_ring, ring);
            core::Transform spin = m_scene->GetLocalTransform(m_torus);
            spin.rotation = core::Quaternion::FromAxisAngle(core::Float3{1, 0, 0}, m_time * 1.3f);
            m_scene->SetLocalTransform(m_torus, spin);

            auto* input = host.Shell() != nullptr ? host.Shell()->Input() : nullptr;
            if (input != nullptr && input->Keyboard()->IsKeyPressed(shell::KeyCode::Escape))
            {
                host.RequestExit(0);
            }
        }

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override
        {
            RenderFrame(host, frame);
            FinishFrame(host, frame); // the --screenshot copy
        }

    private:
        // The shader, one file with both stages (VSMain and PSMain), read from the data root. Its
        // properties block becomes the generated set-2 declarations (as a shader asset's cook does
        // it), and each stage is registered by name with its entry point and the variants it
        // declares. A registered source wins over the cooked pack and compiles on first use (DXC,
        // so a dev or editor build: a compiler-free dist cannot compile it). Every module made from
        // it records its entry, so the renderer's pipelines run VSMain and PSMain unbidden.
        bool RegisterToonShader(shaders::ShaderSystem& shaderSystem)
        {
            const core::String path =
                foundation::vfs::DataPath(DataRoot(), u8"Samples/Shaders/toon/toon.hlsl");
            core::Result<core::Array<core::byte>> bytes = core::ReadFile(path.AsView());
            if (!bytes.HasValue())
            {
                LOG_ERROR(u8"CustomShader", u8"missing the toon shader: {}", path);
                return false;
            }
            const core::StringView text(
                reinterpret_cast<const core::utf8char*>(bytes.Value().Data()),
                bytes.Value().Size());
            core::String expanded;
            const shaders::ShaderPropertiesBlock block =
                shaders::ExpandShaderProperties(text, u8"toon.hlsl", expanded);
            for (const shaders::ShaderPropertyError& e : block.errors)
            {
                LOG_ERROR(u8"CustomShader", u8"toon.hlsl:{}: {}", e.line, e.message);
            }
            if (!block.IsOk())
            {
                return false;
            }
            shaderSystem.RegisterSource(
                u8"toon", shaders::ShaderStage::Vertex, expanded.AsView(), u8"VSMain",
                shaders::ParseStageVariantDirective(text, shaders::ShaderStage::Vertex).mask);
            shaderSystem.RegisterSource(
                u8"toon", shaders::ShaderStage::Fragment, expanded.AsView(), u8"PSMain",
                shaders::ParseStageVariantDirective(text, shaders::ShaderStage::Fragment).mask);
            return true;
        }

        scene::EntityHandle AddMesh(core::StringView name,
                                    const core::RefPtr<geometry::StaticMesh>& mesh,
                                    const core::RefPtr<materials::Material>& material,
                                    core::Float3 position,
                                    scene::EntityHandle parent = scene::EntityHandle::Invalid())
        {
            scene::EntityHandle e = m_scene->CreateEntity(name);
            if (parent.IsAssigned())
            {
                m_scene->SetParent(e, parent);
            }
            m_scene->SetLocalPosition(e, position);
            engine::render::MeshComponent& mc =
                m_scene->GetSystem<engine::render::MeshComponentManager>()->Add(e);
            mc.mesh = mesh;
            mc.SetMaterial(material);
            return e;
        }

        void BuildScene()
        {
            core::IAllocator& a = AppRoot();
            const core::RefPtr<geometry::StaticMesh> sphere =
                geometry::Primitives::Sphere(a, 0.8f, 48, 24);
            const core::RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(a, 0.7f);
            const core::RefPtr<geometry::StaticMesh> plane =
                geometry::Primitives::Plane(a, 30.0f, 30.0f);

            // The floor: toon, so it RECEIVES the shadows the toon shader samples itself.
            (void)AddMesh(u8"floor", plane,
                          CreateToon(u8"ToonFloor", core::Float4{0.75f, 0.75f, 0.72f, 1.0f},
                                     core::Float4{0.35f, 0.38f, 0.5f, 1.0f}, 2.0f),
                          core::Float3{0.0f, 0.0f, 0.0f});

            // Five toon styles in a row: distinct materials, distinct draws.
            const struct
            {
                core::Float4 base;
                core::Float4 shade;
                f32 bands;
            } styles[5] = {
                {{0.9f, 0.2f, 0.2f, 1.0f}, {0.35f, 0.08f, 0.15f, 1.0f}, 2.0f},
                {{0.2f, 0.45f, 0.95f, 1.0f}, {0.1f, 0.1f, 0.35f, 1.0f}, 3.0f},
                {{0.3f, 0.85f, 0.35f, 1.0f}, {0.1f, 0.3f, 0.2f, 1.0f}, 4.0f},
                {{1.0f, 0.8f, 0.25f, 1.0f}, {0.45f, 0.25f, 0.1f, 1.0f}, 3.0f},
                {{0.7f, 0.35f, 0.9f, 1.0f}, {0.25f, 0.1f, 0.35f, 1.0f}, 5.0f},
            };
            for (int i = 0; i < 5; ++i)
            {
                (void)AddMesh(u8"style", sphere,
                              CreateToon(u8"ToonStyle", styles[i].base, styles[i].shade,
                                         styles[i].bands),
                              core::Float3{-5.0f + 2.5f * static_cast<f32>(i), 0.8f, 0.0f});
            }

            // A ring of twelve cubes sharing ONE material and mesh: drawn as an instanced batch,
            // the INSTANCED vertex variant of the toon shader.
            m_ring = m_scene->CreateEntity(u8"ring");
            m_scene->SetLocalPosition(m_ring, core::Float3{0.0f, 0.5f, -5.0f});
            const core::RefPtr<materials::Material> ringToon =
                CreateToon(u8"ToonRing", core::Float4{0.95f, 0.6f, 0.2f, 1.0f},
                           core::Float4{0.4f, 0.15f, 0.1f, 1.0f}, 3.0f);
            for (int i = 0; i < 12; ++i)
            {
                const f32 angle = 6.2831853f * static_cast<f32>(i) / 12.0f;
                (void)AddMesh(u8"ringCube", cube, ringToon,
                              core::Float3{3.0f * std::cos(angle), 0.0f, 3.0f * std::sin(angle)},
                              m_ring);
            }

            // A spinning torus (motion vectors under TAA) and, beside it, the engine's PBR on the
            // same mesh, for comparison.
            const core::RefPtr<geometry::StaticMesh> torus =
                geometry::Primitives::Torus(a, 0.8f, 0.3f);
            m_torus = AddMesh(u8"torus", torus,
                              CreateToon(u8"ToonTorus", core::Float4{0.25f, 0.85f, 0.85f, 1.0f},
                                         core::Float4{0.05f, 0.2f, 0.3f, 1.0f}, 3.0f),
                              core::Float3{-3.0f, 1.3f, 3.5f});
            (void)AddMesh(u8"pbr", sphere,
                          materials::CreatePBR(u8"Pbr", core::Float4{0.25f, 0.85f, 0.85f, 1.0f},
                                               0.0f, 0.4f),
                          core::Float3{3.0f, 0.8f, 3.5f});

            // A masked toon material: drawn through the shader's ALPHA_TEST variant (with no
            // texture its alpha is 1 throughout, so nothing is cut: the variant compiles and
            // draws).
            core::RefPtr<materials::Material> masked =
                CreateToon(u8"ToonMasked", core::Float4{0.9f, 0.9f, 0.95f, 1.0f},
                           core::Float4{0.3f, 0.3f, 0.45f, 1.0f}, 3.0f);
            masked->pipeline.blendMode = materials::BlendMode::Masked;
            (void)AddMesh(u8"masked", cube, masked, core::Float3{0.0f, 2.6f, 2.0f});
        }

        void BuildLights()
        {
            auto* lights = m_scene->GetSystem<engine::render::LightComponentManager>();
            if (lights == nullptr)
            {
                return;
            }
            scene::EntityHandle sun = m_scene->CreateEntity(u8"sun");
            core::Transform st = m_scene->GetLocalTransform(sun);
            st.rotation = core::Quaternion::FromAxisAngle(core::Float3{0, 1, 0}, -0.7f) *
                          core::Quaternion::FromAxisAngle(core::Float3{1, 0, 0}, -0.8f);
            m_scene->SetLocalTransform(sun, st);
            engine::render::LightComponent& sl = lights->Add(sun);
            sl.type = engine::render::LightType::Directional;
            sl.intensity = 2.5f;
            sl.castsShadows = true;

            scene::EntityHandle lamp = m_scene->CreateEntity(u8"lamp");
            m_scene->SetLocalPosition(lamp, core::Float3{2.0f, 2.5f, -3.0f});
            engine::render::LightComponent& pl = lights->Add(lamp);
            pl.type = engine::render::LightType::Point;
            pl.color = core::Color{1.0f, 0.5f, 0.2f, 1.0f};
            pl.intensity = 30.0f;
            pl.range = 9.0f;
            pl.castsShadows = true;
        }

        // The Fox (glTF, skinned and animated), cooked at startup into this sample's own output
        // database, then drawn with toon materials that keep its texture: the SKINNED variant.
        void SpawnToonFox(runtime::IApplicationHost& host)
        {
            const core::String outputDir = vfs::DataPath(DataRoot(), u8"Output/CustomShader");
            const core::String foxPath =
                vfs::DataPath(DataRoot(), u8"Samples/Models/Fox/glTF/Fox.gltf");
            m_contentFs =
                core::MakeUnique<vfs::NativeFileSystem>(AppRoot(), outputDir.AsView(), AppRoot());
            m_contentDb = core::MakeUnique<content::ContentDatabase>(
                AppRoot(), AppRoot(), *m_contentFs, core::BinarySerializerFactory(), u8".rasset");
            m_resources =
                core::MakeUnique<resource::ResourceManager>(AppRoot(), AppRoot(), *m_contentDb);
            resource::ResourceServiceTable services;
            if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            {
                services.Add<foundation::rhi::Device>(gfx->Raw());
            }
            engine::FullComposition().CreateFactories(m_factories, AppRoot(), services);
            m_factories.Register(*m_resources);
            engine::RegisterAllResourceTypes();

            core::Guid modelGuid;
            if (pipeline::LoadAndCook(foxPath.AsView(), *m_contentDb, u8"Fox", modelGuid) !=
                model::ModelLoadResult::Ok)
            {
                LOG_ERROR(u8"CustomShader", u8"the Fox did not cook: {}", foxPath);
                return;
            }
            m_fox = m_resources->Bind<model::ModelResource>(modelGuid);
            if (!m_fox || !m_fox->skeleton || m_fox->animations.IsEmpty())
            {
                return;
            }

            // One toon material per Fox material, keeping its albedo texture.
            core::Array<core::RefPtr<materials::Material>> toonMats;
            for (auto& source : m_fox->materials)
            {
                core::RefPtr<materials::Material> toon =
                    CreateToon(u8"ToonFox", core::Float4{1.0f, 1.0f, 1.0f, 1.0f},
                               core::Float4{0.45f, 0.2f, 0.12f, 1.0f}, 3.0f);
                if (source)
                {
                    const core::isize albedo = source->GetPropertyIndex(u8"AlbedoMap");
                    if (albedo >= 0)
                    {
                        toon->SetDefaultTexture(
                            u8"AlbedoMap",
                            source->GetDefaultTexture(static_cast<core::usize>(albedo)));
                    }
                }
                toonMats.PushBack(toon);
            }

            const core::Float3 extent = m_fox->boundsMax - m_fox->boundsMin;
            const f32 fit = 2.0f / core::Max(extent.x, core::Max(extent.y, extent.z));
            scene::EntityHandle root = m_scene->CreateEntity(u8"fox");
            core::Transform rootT;
            rootT.position = core::Float3{5.5f, 0.0f, 3.0f};
            rootT.scale = core::Float3{fit, fit, fit};
            m_scene->SetLocalTransform(root, rootT);

            auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>();
            core::Array<scene::EntityHandle> entities;
            core::Array<scene::EntityHandle> skinned;
            for (const pipeline::ModelNode& node : m_fox->nodes)
            {
                scene::EntityHandle e = m_scene->CreateEntity(node.name.AsView());
                m_scene->SetLocalTransform(e, node.localTransform);
                entities.PushBack(e);
            }
            for (core::usize i = 0; i < m_fox->nodes.Size(); ++i)
            {
                const pipeline::ModelNode& node = m_fox->nodes[i];
                const bool hasParent = node.parentIndex >= 0 &&
                                       static_cast<core::usize>(node.parentIndex) < entities.Size();
                m_scene->SetParent(entities[i],
                                   hasParent ? entities[static_cast<core::usize>(node.parentIndex)]
                                             : root);
                if (node.meshIndex < 0 ||
                    static_cast<core::usize>(node.meshIndex) >= m_fox->meshes.Size())
                {
                    continue;
                }
                geometry::StaticMesh* mesh =
                    m_fox->meshes[static_cast<core::usize>(node.meshIndex)].Get();
                if (mesh == nullptr)
                {
                    continue;
                }
                engine::render::MeshComponent& mc = meshes->Add(entities[i]);
                mc.mesh = core::RefPtr<geometry::StaticMesh>(mesh);
                core::Array<core::i32> slots;
                m_fox->MeshMaterialIndices(static_cast<core::usize>(node.meshIndex), slots);
                core::Array<core::RefPtr<materials::Material>> meshMats;
                for (const core::i32 slot : slots)
                {
                    meshMats.PushBack(static_cast<core::usize>(slot) < toonMats.Size()
                                          ? toonMats[static_cast<core::usize>(slot)]
                                          : core::RefPtr<materials::Material>{});
                }
                mc.SetMaterials(meshMats);
                if (mesh->IsSkinned())
                {
                    skinned.PushBack(entities[i]);
                }
            }

            auto* anims =
                m_scene->GetSystem<engine::animation::SkeletalAnimationComponentManager>();
            if (anims != nullptr && !skinned.IsEmpty())
            {
                engine::animation::SkeletalAnimationComponent& a = anims->Add(root);
                a.skeleton = m_fox->skeleton.Get();
                // The Fox's clips are Survey, Walk and Run: Walk if it is there.
                const core::usize walk = core::Min<core::usize>(1, m_fox->animations.Size() - 1);
                a.clip = m_fox->animations[walk].Get();
                for (scene::EntityHandle e : skinned)
                {
                    a.meshEntities.PushBack(m_scene->GetEntityId(e));
                }
            }
        }

        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_camera;
        scene::EntityHandle m_ring;
        scene::EntityHandle m_torus;
        f32 m_time = 0.0f;
        // The cooked Fox's pipeline state, outliving its entities.
        core::UniquePtr<vfs::NativeFileSystem> m_contentFs;
        core::UniquePtr<content::ContentDatabase> m_contentDb;
        core::UniquePtr<resource::ResourceManager> m_resources;
        resource::ResourceFactorySet m_factories;
        resource::Proxy<model::ModelResource> m_fox;
        samples::FlyCamera m_fly;
    };
}

APP_MAIN(CustomShaderApp)
