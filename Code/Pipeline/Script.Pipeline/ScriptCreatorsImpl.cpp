// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Script.Pipeline - the script domain's New Asset creators: one per tier (Behavior, Level, Game)
// for every registered script backend that has a cook, each seeded from the cook's tier starter
// (not hard-coded text) written to the sources folder.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module script.pipeline;

import foundation.core;
import foundation.content;
import foundation.script;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{
    foundation::content::Instance* CreateScriptInstance(const AssetCreationContext& context,
                                                        StringView languageId,
                                                        StringView extension, ScriptTier tier,
                                                        StringView baseName)
    {
        IScriptLanguageCook* cook = ScriptLanguageCookRegistry::Get().FindByLanguage(languageId);
        if (cook == nullptr)
        {
            return nullptr;
        }
        foundation::content::Group* target = context.Target();
        if (target == nullptr)
        {
            return nullptr;
        }
        // The name first: a behaviour's class is named after the asset that holds it.
        const String name = target->UniqueInstanceName(context.NameOr(baseName));
        AssetCreationContext named = context;
        named.name = name.AsView();
        String dottedExtension(u8".");
        dottedExtension.Append(extension);
        ScriptClassAsset asset;
        asset.language = String(languageId);
        const String starter = cook->NewAssetTemplate(tier, ClassNameFor(name.AsView(), baseName).AsView());
        return CreateLinkedTextAsset(named, baseName, dottedExtension.AsView(), starter.AsView(),
                                     ScriptClassAsset::StaticType(), asset);
    }

    usize RegisterScriptCreators(AssetCreatorRegistry& registry)
    {
        struct TierDesc
        {
            ScriptTier tier;
            StringView suffix;   // the label's suffix
            StringView baseName; // the unique-name stem
        };
        static const TierDesc kTiers[] = {
            {ScriptTier::Behavior, u8"Behavior", ScriptTierBaseName(ScriptTier::Behavior)},
            {ScriptTier::Level, u8"Level", ScriptTierBaseName(ScriptTier::Level)},
            {ScriptTier::Game, u8"Game", ScriptTierBaseName(ScriptTier::Game)},
        };
        usize registered = 0;
        for (const foundation::script::ScriptBackendDesc& backend :
             foundation::script::ScriptBackendRegistry::Get().All())
        {
            // A backend with no cook (compile and harvest) cannot seed a starter.
            if (ScriptLanguageCookRegistry::Get().FindByLanguage(backend.languageId.AsView()) ==
                nullptr)
            {
                continue;
            }
            const String extension = backend.fileExtensions.IsEmpty()
                                         ? String(backend.languageId.AsView())
                                         : String(backend.fileExtensions[0].AsView());
            const StringView displayName = backend.displayName.IsEmpty()
                                               ? backend.languageId.AsView()
                                               : backend.displayName.AsView();
            for (const TierDesc& t : kTiers)
            {
                AssetCreator creator;
                creator.label = Format(u8"{} {}", displayName, t.suffix);
                creator.category = String(u8"Scripts");
                creator.type = &ScriptClassAsset::StaticType();
                creator.run = [languageId = String(backend.languageId.AsView()), extension,
                               tier = t.tier, base = String(t.baseName)](
                                  const AssetCreationContext& context)
                {
                    return CreateScriptInstance(context, languageId.AsView(), extension.AsView(),
                                                tier, base.AsView());
                };
                registry.Register(Move(creator));
                ++registered;
            }
        }
        return registered;
    }
}
