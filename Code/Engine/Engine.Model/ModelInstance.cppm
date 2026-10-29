// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Model - `engine.model`: a cooked model (model::ModelResource) put into a scene as
// entities, outside any project's prefabs: what a host that cooks a model file at run time does
// with it (the Sandbox sample, the model viewer). One entity per model node under a root, a mesh
// component per node with a mesh (its own material list, from the manifest's slots), and, when the
// model is skinned and has clips, a skeletal animation component on the root playing the first
// clip into its skinned meshes. A skinned mesh sits in its skeleton's space, as the project's model
// prefab puts it (foundation::model::SkinnedMeshParent). The model's resources stay alive while the caller keeps its proxy.

module;
#include "Core/Prelude.h"

export module engine.model;

import foundation.core;
import foundation.scene;
import foundation.geometry;
import foundation.materials;
import foundation.animation;
import foundation.model.resource;
import engine.render;
import engine.animation;

using namespace foundation::core;

export namespace engine::model
{
    namespace scene = foundation::scene;

    /// What InstantiateModel made: the root every node hangs from, the entities with a skinned mesh,
    /// and whether the root plays a clip.
    struct ModelInstance
    {
        scene::EntityHandle root = scene::EntityHandle::Invalid();
        Array<scene::EntityHandle> skinned;
        bool animated = false;
    };

    struct InstantiateOptions
    {
        /// The root's own transform (its scale fits the model to a size, as the caller wants).
        Transform rootTransform;
        /// Put a skeletal animation component on the root playing the first clip (a skinned model
        /// with clips). Off when the caller drives the skeleton otherwise (an animation graph).
        bool animate = true;
    };

    /// Put `model` into `scene` under a new root named `name`. Needs the scene's mesh component
    /// manager (and the skeletal animation one to animate); without the mesh manager it makes
    /// nothing and returns an invalid root.
    inline ModelInstance InstantiateModel(scene::Scene& scene, foundation::model::ModelResource& model,
                                          StringView name, const InstantiateOptions& options = {})
    {
        ModelInstance instance;
        auto* meshes = scene.GetSystem<engine::render::MeshComponentManager>();
        if (meshes == nullptr)
        {
            return instance;
        }
        instance.root = scene.CreateEntity(name);
        scene.SetLocalTransform(instance.root, options.rootTransform);

        // All the model's materials; a mesh binds the ones its submeshes index (its slots).
        Array<RefPtr<foundation::materials::Material>> modelMaterials;
        modelMaterials.Reserve(model.materials.Size());
        for (auto& material : model.materials)
        {
            modelMaterials.PushBack(RefPtr<foundation::materials::Material>(material.Get()));
        }

        Array<scene::EntityHandle> entities;
        entities.Reserve(model.nodes.Size());
        for (const foundation::model::ModelNode& node : model.nodes)
        {
            const scene::EntityHandle e = scene.CreateEntity(node.name.AsView());
            scene.SetLocalTransform(e, node.localTransform);
            entities.PushBack(e);
        }
        Array<i32> slots;
        for (usize i = 0; i < model.nodes.Size(); ++i)
        {
            const foundation::model::ModelNode& node = model.nodes[i];
            const bool hasParent = node.parentIndex >= 0 && static_cast<usize>(node.parentIndex) < entities.Size();
            scene.SetParent(entities[i], hasParent ? entities[static_cast<usize>(node.parentIndex)] : instance.root);
            if (node.meshIndex < 0 || static_cast<usize>(node.meshIndex) >= model.meshes.Size())
            {
                continue;
            }
            foundation::geometry::StaticMesh* mesh = model.meshes[static_cast<usize>(node.meshIndex)].Get();
            if (mesh == nullptr)
            {
                continue;
            }
            engine::render::MeshComponent& component = meshes->Add(entities[i]);
            component.mesh = RefPtr<foundation::geometry::StaticMesh>(mesh);
            component.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
            // The mesh's own material list: its submeshes index it, slot 0 covering anything out of range.
            model.MeshMaterialIndices(static_cast<usize>(node.meshIndex), slots);
            Array<RefPtr<foundation::materials::Material>> meshMaterials;
            for (const i32 slot : slots)
            {
                meshMaterials.PushBack(slot >= 0 && static_cast<usize>(slot) < modelMaterials.Size()
                                           ? modelMaterials[static_cast<usize>(slot)]
                                           : RefPtr<foundation::materials::Material>{});
            }
            component.SetMaterials(meshMaterials);
            if (mesh->IsSkinned())
            {
                // In the skeleton's space: under its parent node at identity (SkinnedMeshParent).
                const i32 parent = foundation::model::SkinnedMeshParent(model.nodes.AsSpan(), model.skeletonParentNode, i);
                if (parent != foundation::model::kSkinnedMeshKeepsPlace)
                {
                    scene.SetParent(entities[i], parent >= 0 ? entities[static_cast<usize>(parent)] : instance.root);
                    scene.SetLocalTransform(entities[i], Transform{});
                }
                instance.skinned.PushBack(entities[i]);
            }
        }

        // A skinned model with clips: the root plays the first into its skinned meshes.
        if (options.animate && model.skeleton && !model.animations.IsEmpty() && model.animations[0] &&
            !instance.skinned.IsEmpty())
        {
            if (auto* animators = scene.GetSystem<engine::animation::SkeletalAnimationComponentManager>())
            {
                engine::animation::SkeletalAnimationComponent& animator = animators->Add(instance.root);
                animator.skeleton = model.skeleton.Get();
                animator.clip = model.animations[0].Get();
                for (const scene::EntityHandle e : instance.skinned)
                {
                    animator.meshEntities.PushBack(scene.GetEntityId(e));
                }
                instance.animated = true;
            }
        }
        return instance;
    }
}
