// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::ModelImporter - the model manifest's reflection: its display name (the editor shows it
// as the asset's kind). Kept out of the interface, as REFLECT_* bodies are.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module modelimporter;

import foundation.core;

using namespace foundation::core;

namespace pipeline
{
    REFLECT_MEMBERS(ModelManifestAsset, "rtti::pipeline::modelimporter")
    {
        builder.Attribute("displayName", String(u8"Model"));
    }

    // The import's settings, which the import window draws and an agent sets by label.
    REFLECT_MEMBERS(ModelImportOptions, "rtti::pipeline::modelimporter")
    {
        builder.Property<&ModelImportOptions::importTextures>("importTextures")
            .PropAttribute("displayName", String(u8"Textures"))
            .PropAttribute("description", String(u8"Import the model's images as texture assets"))
            .Property<&ModelImportOptions::importMaterials>("importMaterials")
            .PropAttribute("displayName", String(u8"Materials"))
            .PropAttribute("description", String(u8"Import PBR materials (textures wire in when they import too)"))
            .Property<&ModelImportOptions::importAnimations>("importAnimations")
            .PropAttribute("displayName", String(u8"Animations"))
            .PropAttribute("description", String(u8"Import the skeleton and animation clips"))
            .Property<&ModelImportOptions::generatePrefab>("generatePrefab")
            .PropAttribute("displayName", String(u8"Generate prefab"))
            .PropAttribute("description", String(u8"Create a spawnable prefab of the model's node hierarchy; re-import regenerates it"))
            .Property<&ModelImportOptions::generateScene>("generateScene")
            .PropAttribute("displayName", String(u8"Generate scene"))
            .PropAttribute("description", String(u8"Create a standalone scene of the model's node hierarchy; re-import regenerates it"))
            .Property<&ModelImportOptions::generateCollision>("generateCollision")
            .PropAttribute("displayName", String(u8"Generate collision"))
            .PropAttribute("description", String(u8"Cook a collision shape per mesh and add colliders (+ a static rigid body) to the generated prefab"))
            .Property<&ModelImportOptions::generateLods>("generateLods")
            .PropAttribute("displayName", String(u8"Generate LODs"))
            .PropAttribute("description", String(u8"Simplified LOD chains for large static meshes (10k+ triangles); meshes with authored _LOD1/_LOD2 levels keep those instead"))
            .Property<&ModelImportOptions::rootMotion>("rootMotion")
            .PropAttribute("displayName", String(u8"Root motion"))
            .PropAttribute("description", String(u8"New clips move the character by their root's travel and turn, and play in place (each clip's page can change it; a re-import keeps what the clip says)"))
            .Property<&ModelImportOptions::collisionConvex>("collisionConvex")
            .PropAttribute("displayName", String(u8"Convex collision"))
            .PropAttribute("description", String(u8"Simplified convex hulls (dynamic-capable) instead of exact triangle meshes"))
            .PropAttribute("visibleWhen", String(u8"generateCollision"));
    }
}
