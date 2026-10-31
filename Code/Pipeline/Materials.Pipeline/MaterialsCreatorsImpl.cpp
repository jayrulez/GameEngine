// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Materials.Pipeline - the materials domain's New Asset creators: what File > New and
// asset_create make, with no editor (the source database and the sources folder are all a
// creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module materials.pipeline;

import foundation.core;
import foundation.content;
import foundation.materials;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    foundation::content::Instance* CreateMaterialInstance(foundation::content::Group* target,
                                                          StringView baseName, foundation::materials::BuiltinShading shading)
    {
        if (target == nullptr)
        {
            return nullptr;
        }
        const String name = target->UniqueInstanceName(baseName);
        foundation::content::Instance* instance =
            target->CreateInstance(name.AsView(), MaterialAsset::StaticType());
        if (instance == nullptr)
        {
            return nullptr;
        }
        RefPtr<foundation::materials::Material> built = foundation::materials::CreateBuiltin(shading, name.AsView());
        MaterialAsset asset;
        MaterialImporter::Import(*built, Guid{}, asset);
        return instance->WriteObject(asset).IsOk() ? instance : nullptr;
    }

    void RegisterMaterialCreators(AssetCreatorRegistry& registry)
    {
        // A material on each built-in ("PBR Material", "Unlit Material", "Phong Material"), under
        // Materials/ unless a group was picked.
        for (const foundation::materials::BuiltinShadingInfo& builtin : foundation::materials::kBuiltinShadings)
        {
            AssetCreator creator;
            creator.label = Format(u8"{} Material", builtin.label);
            creator.category = String(u8"Materials");
            creator.type = &MaterialAsset::StaticType();
            creator.defaultGroup = String(u8"Materials");
            const foundation::materials::BuiltinShading shading = builtin.shading;
            creator.run = [shading](const AssetCreationContext& context)
            { return CreateMaterialInstance(context.Target(), context.NameOr(u8"Material"), shading); };
            registry.Register(Move(creator));
        }
    }
}
