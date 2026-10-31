// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Geometry.Pipeline - the geometry domain's New Asset creators: what File > New and
// asset_create make, with no editor (the source database and the sources folder are all a
// creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module geometry.pipeline;

import foundation.core;
import foundation.content;
import foundation.geometry;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    foundation::content::Instance*
    CreatePrimitiveMeshInstance(foundation::content::Group* target, StringView baseName,
                                RefPtr<foundation::geometry::StaticMesh> mesh)
    {
        if (target == nullptr || mesh.Get() == nullptr)
        {
            return nullptr;
        }
        foundation::content::Instance* instance = target->CreateInstance(
            target->UniqueInstanceName(baseName).AsView(), StaticMeshAsset::StaticType());
        if (instance == nullptr)
        {
            return nullptr;
        }
        StaticMeshAsset asset;
        MeshImporter::Import(*mesh, asset);
        // WriteMeshAsset, not a raw WriteObject: the geometry goes to the binary sidecar
        // stream, as all bulk data does.
        return WriteMeshAsset(*instance, asset).IsOk() ? instance : nullptr;
    }

    void RegisterGeometryCreators(AssetCreatorRegistry& registry)
    {
        // The procedural primitives, as static meshes under Meshes/ unless a group was picked.
        struct Entry
        {
            StringView label;
            RefPtr<foundation::geometry::StaticMesh> (*make)(IAllocator&);
        };
        static const Entry kEntries[] = {
            {u8"Cube", [](IAllocator& a) { return foundation::geometry::Primitives::Cube(a); }},
            {u8"Sphere", [](IAllocator& a) { return foundation::geometry::Primitives::Sphere(a); }},
            {u8"Plane", [](IAllocator& a) { return foundation::geometry::Primitives::Plane(a); }},
            {u8"Cylinder", [](IAllocator& a) { return foundation::geometry::Primitives::Cylinder(a); }},
            {u8"Cone", [](IAllocator& a) { return foundation::geometry::Primitives::Cone(a); }},
            {u8"Torus", [](IAllocator& a) { return foundation::geometry::Primitives::Torus(a); }},
        };
        IAllocator* allocator = &registry.Allocator();
        for (const Entry& entry : kEntries)
        {
            AssetCreator creator;
            creator.label = String(entry.label);
            creator.category = String(u8"Primitives");
            creator.type = &StaticMeshAsset::StaticType();
            creator.defaultGroup = String(u8"Meshes");
            creator.run = [allocator, make = entry.make,
                           base = String(entry.label)](const AssetCreationContext& context)
            {
                return CreatePrimitiveMeshInstance(context.Target(), context.NameOr(base.AsView()),
                                                   make(*allocator));
            };
            registry.Register(Move(creator));
        }
    }

    // The asset types' reflection: their display names (the editor shows them as the asset's kind).
    REFLECT_MEMBERS(StaticMeshAsset, "rtti::pipeline::geometry")
    {
        builder.DataVersion(5)
            .Attribute("displayName", String(u8"Static Mesh"));
    }

    REFLECT_MEMBERS(SkinnedMeshAsset, "rtti::pipeline::geometry")
    {
        builder.DataVersion(5)
            .Attribute("displayName", String(u8"Skinned Mesh"));
    }
}
