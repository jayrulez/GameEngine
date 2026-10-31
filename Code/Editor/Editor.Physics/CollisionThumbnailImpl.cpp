// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Collision-shape GPU thumbnail generator (see CollisionThumbnail.cppm). Heavy imports here.

module;
#include "Core/Prelude.h"

module editor.physics;

import foundation.core;
import foundation.scene;
import foundation.resource;
import foundation.geometry;
import foundation.materials;
import foundation.physics.resource;
import engine.render;
import editor.core;

using namespace foundation::core;

namespace editor
{
    namespace scene = foundation::scene;
    namespace geometry = foundation::geometry;
    namespace materials = foundation::materials;
    namespace resource = foundation::resource;
    namespace physics = foundation::physics;

    namespace
    {
        class CollisionThumbnailGenerator final : public ISceneThumbnailGenerator
        {
        public:
            [[nodiscard]] Span<const StringView> AssetTypeNames() const override
            {
                static constexpr StringView kNames[] = {u8"CollisionShapeAsset"};
                return Span<const StringView>(kNames, 1);
            }

            [[nodiscard]] ThumbnailStageStep Stage(const Guid& id, scene::Scene& stage,
                                                   resource::ResourceManager& resources,
                                                   ThumbnailFraming& outFraming) override
            {
                engine::render::MeshComponent* component = Ensure(stage);
                if (component == nullptr)
                {
                    return ThumbnailStageStep::Failed;
                }
                m_proxy = resources.Bind<physics::CollisionShape>(id);
                physics::CollisionShape* shape = m_proxy ? m_proxy.Get() : nullptr;
                if (shape == nullptr)
                {
                    resource::ResourceHandle* handle = m_proxy.Handle();
                    if (handle == nullptr ||
                        handle->State() == resource::ResourceState::Failed)
                    {
                        return ThumbnailStageStep::Failed;
                    }
                    return ThumbnailStageStep::Pending;
                }
                if (!m_mesh)
                {
                    m_mesh = MakeRef<geometry::StaticMesh>(editor::EditorRootAllocator());
                }
                if (!BuildCollisionOutlineMesh(*shape, *m_mesh))
                {
                    return ThumbnailStageStep::Failed; // cooked without display triangles
                }

                component->mesh.SetId(Guid{});
                component->mesh = m_mesh.Get();
                if (!m_material)
                {
                    m_material = materials::CreatePBR(u8"ThumbCollisionDefault");
                }
                component->SetMaterial(m_material);

                Transform t;
                t.position = Float3{} - m_mesh->bounds.Center();
                stage.SetLocalTransform(m_display, t);
                outFraming.radius = Max(Length(m_mesh->bounds.Extents()), 0.05f);
                return ThumbnailStageStep::Ready;
            }

            void Unstage(scene::Scene& stage) override
            {
                if (auto* meshes = stage.GetSystem<engine::render::MeshComponentManager>())
                {
                    if (engine::render::MeshComponent* component = meshes->Get(m_display))
                    {
                        component->mesh.SetId(Guid{});
                        component->mesh.SetDirect(RefPtr<geometry::StaticMesh>{});
                    }
                }
                m_proxy = {};
                if (m_display.IsAssigned() && stage.IsValid(m_display))
                {
                    stage.SetActive(m_display, false);
                }
                if (m_sun.IsAssigned() && stage.IsValid(m_sun))
                {
                    stage.SetActive(m_sun, false);
                }
            }

        private:
            // A persistent display entity and private sun, created once and toggled per job, because
            // on the shared stage an idle generator must contribute nothing to other jobs.
            [[nodiscard]] engine::render::MeshComponent* Ensure(scene::Scene& stage)
            {
                auto* meshes = stage.GetSystem<engine::render::MeshComponentManager>();
                auto* lights = stage.GetSystem<engine::render::LightComponentManager>();
                if (meshes == nullptr || lights == nullptr)
                {
                    return nullptr;
                }
                if (!m_display.IsAssigned() || !stage.IsValid(m_display))
                {
                    m_display = stage.CreateEntity(u8"ThumbCollision");
                    meshes->Add(m_display);
                    m_sun = stage.CreateEntity(u8"ThumbCollisionSun");
                    Transform t;
                    t.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) *
                                 Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
                    stage.SetLocalTransform(m_sun, t);
                    engine::render::LightComponent& light = lights->Add(m_sun);
                    light.castsShadows = false;
                }
                stage.SetActive(m_display, true);
                stage.SetActive(m_sun, true);
                return meshes->Get(m_display);
            }

            scene::EntityHandle m_display{};
            scene::EntityHandle m_sun{};
            resource::Proxy<physics::CollisionShape> m_proxy;
            RefPtr<geometry::StaticMesh> m_mesh;
            RefPtr<materials::Material> m_material;
        };
    }

    bool BuildCollisionOutlineMesh(const physics::CollisionShape& shape, geometry::StaticMesh& mesh)
    {
        mesh.ClearForReload();
        const usize count = shape.outline.Size() - (shape.outline.Size() % 3);
        if (count < 3)
        {
            return false;
        }
        // Unwelded soup: one vertex per corner, so GenerateNormals yields the flat facets a
        // collision hull should read as.
        mesh.vertices.Reserve(count);
        mesh.indices.Resize(static_cast<u32>(count));
        for (usize i = 0; i < count; ++i)
        {
            mesh.vertices.PushBack(geometry::StaticMeshVertex{shape.outline[i], Float3{0, 1, 0},
                                                              Float2{}, 0xFFFFFFFFu,
                                                              Float4{1, 0, 0, 1}});
            mesh.indices.Add(static_cast<u32>(i));
        }
        mesh.GenerateNormals();
        mesh.GenerateTangents();
        mesh.CalculateBounds();
        mesh.subMeshes.PushBack(geometry::SubMesh{0, static_cast<i32>(mesh.IndexCount()), 0,
                                                  geometry::PrimitiveType::Triangles});
        return true;
    }

    UniquePtr<ISceneThumbnailGenerator> CreateCollisionThumbnailGenerator()
    {
        return UniquePtr<ISceneThumbnailGenerator>(
            editor::EditorRootAllocator().New<CollisionThumbnailGenerator>(), editor::EditorRootAllocator());
    }

    void RegisterCollisionThumbnailGenerator(ThumbnailService& service)
    {
        service.RegisterSceneGenerator(
            MakeUnique<CollisionThumbnailGenerator>(editor::EditorRootAllocator()));
    }
}
