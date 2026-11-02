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

    REFLECT_ENUM(ModelUpAxis, "rtti::pipeline::modelimporter")
    {
        builder.Value("Y", ModelUpAxis::Y);
        builder.Value("Z", ModelUpAxis::Z);
    }

    // A clip's own settings, which the import window draws in the clip's details.
    REFLECT_MEMBERS(ClipImportSettings, "rtti::pipeline::modelimporter")
    {
        builder.Property<&ClipImportSettings::travel>("travel")
            .PropAttribute("displayName", String(u8"Root travel"))
            .PropAttribute("description", String(u8"The root's travel on the ground moves the character; the clip plays in place"))
            .Property<&ClipImportSettings::vertical>("vertical")
            .PropAttribute("displayName", String(u8"Root height"))
            .PropAttribute("description", String(u8"The root's rise and fall moves the character too (a climb; off for a walk, which keeps its bob)"))
            .Property<&ClipImportSettings::turn>("turn")
            .PropAttribute("displayName", String(u8"Root turn"))
            .PropAttribute("description", String(u8"The root's turn about up turns the character"));
    }

    // The import's settings, which the import window draws and an agent sets by label.
    REFLECT_MEMBERS(ModelImportOptions, "rtti::pipeline::modelimporter")
    {
        RttiRegisterEnum_ModelUpAxis(); // its values' names, before a setting of its type is drawn
        builder.Property<&ModelImportOptions::scale>("scale")
            .PropAttribute("displayName", String(u8"Scale"))
            .PropAttribute("description", String(u8"Every length times this: 0.01 for a file in centimetres, 100 for one in metres read as centimetres"))
            .Property<&ModelImportOptions::upAxis>("upAxis")
            .PropAttribute("displayName", String(u8"Up axis"))
            .PropAttribute("description", String(u8"The axis that is up in the model: Z turns a Z-up model upright"))
            .Property<&ModelImportOptions::importTextures>("importTextures")
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
            .PropAttribute("description", String(u8"The default for each new clip: it moves the character by its root's travel and turn, and plays in place (each clip sets its own in the import window and on its page; a re-import keeps what the clip says)"))
            .Property<&ModelImportOptions::collisionConvex>("collisionConvex")
            .PropAttribute("displayName", String(u8"Convex collision"))
            .PropAttribute("description", String(u8"Simplified convex hulls (dynamic-capable) instead of exact triangle meshes"))
            .PropAttribute("visibleWhen", String(u8"generateCollision"));
    }
}
