// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline.Registration.Tests - the drift tripwire.
//
// The composition root is only useful if it stays complete. These tests register into fresh
// local registries and assert the counts against the interface's explicit constants: a new
// builder or importer bumps its constant on purpose, and a lost or doubled registration fails.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import pipeline.core;
import pipeline.importer;
import pipeline.registration;
import foundation.content; // ContentDatabase: a plain source database, no editor
import foundation.vfs;

using namespace foundation::core;

TEST_CASE("pipeline.registration: RegisterAllBuilders populates exactly kBuilderCount builders")
{
    pipeline::BuilderRegistry registry{DefaultAllocator()};
    CHECK(registry.Count() == 0u);
    pipeline::RegisterAllBuilders(registry, foundation::vfs::FindDataRoot());
    CHECK(registry.Count() == pipeline::kBuilderCount);
}

TEST_CASE("pipeline.registration: RegisterAllImporters populates exactly kImporterCount importers")
{
    pipeline::ImporterRegistry registry{DefaultAllocator()};
    CHECK(registry.Count() == 0u);
    pipeline::RegisterAllImporters(registry);
    CHECK(registry.Count() == pipeline::kImporterCount);
}

TEST_CASE("pipeline.registration: RegisterPipelineTypes runs the whole set without faulting")
{
    // Smoke: every registrar the headless cook needs fires once, in order, on a real process.
    // There is no count to assert; this checks that the type, backend and cook registrations all
    // resolve and none abort.
    pipeline::RegisterPipelineTypes();

    // Routing still works after registration: a fresh builder set resolves by asset type name.
    pipeline::BuilderRegistry registry{DefaultAllocator()};
    pipeline::RegisterAllBuilders(registry, foundation::vfs::FindDataRoot());
    CHECK(registry.Count() == pipeline::kBuilderCount);
}

namespace
{
    // True when the null-terminated `ns` begins with `prefix`.
    bool NamespaceStartsWith(const char* ns, const char* prefix)
    {
        if (ns == nullptr)
        {
            return false;
        }
        for (usize i = 0; prefix[i] != '\0'; ++i)
        {
            if (ns[i] != prefix[i])
            {
                return false;
            }
        }
        return true;
    }
}

TEST_CASE("pipeline.registration: Pipeline-collection types carry the Pipeline domain, never Editor")
{
    // Asset/importer/cook types live in the Pipeline collection. Tagging their registrations
    // TypeDomain("Editor") is wrong: the headless CLI and MCP hosts have them without the editor,
    // and the player does not have them at all. Every rtti::pipeline authoring type must be
    // Pipeline, never Editor and never defaulted to Runtime.
    pipeline::RegisterPipelineTypes();
    const TypeRegistry& reg = GlobalTypeRegistry();
    const TypeDomain editor{StringView(u8"Editor")};

    usize pipelineDomained = 0;
    for (const TypeInfo* type : reg.All())
    {
        if (!NamespaceStartsWith(type->namespaceName, "rtti::pipeline"))
        {
            continue;
        }
        const TypeDomain domain = reg.DomainOf(type->id);
        CHECK(domain != editor); // the drift this test exists to catch
        if (domain == pipeline::kPipelineTypeDomain)
        {
            ++pipelineDomained;
        }
    }
    // The asset types (at least one per pipeline collection) carry the Pipeline domain, so they
    // are not silently defaulted to Runtime.
    CHECK(pipelineDomained >= 14);
}

TEST_CASE("pipeline.registration: every builder's ProductType is a registered serializable")
{
    // Tripwire: the cook driver stamps builder->ProductType() into the product envelope
    // and the factory reconstructs it via ReadObject, so the product type must be a registered
    // serializable (the cooked serialized form). A builder returning its runtime product type
    // instead breaks this; the test audits every registered builder.
    pipeline::RegisterPipelineTypes();
    pipeline::BuilderRegistry registry{DefaultAllocator()};
    pipeline::RegisterAllBuilders(registry, foundation::vfs::FindDataRoot());
    REQUIRE(registry.Count() == pipeline::kBuilderCount);

    registry.ForEach(
        [](const pipeline::IAssetBuilder& builder)
        {
            const TypeInfo* product = builder.ProductType();
            REQUIRE(product != nullptr);
            INFO("builder product type: ",
                 doctest::String(product->name != nullptr ? product->name : "<unnamed>"));
            CHECK(GlobalSerializableRegistry().Contains(product->id));
        });
}

// Every creator runs against a plain source database with no editor: the source database and a
// sources folder are all a creation needs, so a headless host creates through the same creators.
TEST_CASE("pipeline.registration: every creator makes its asset with no editor")
{
    pipeline::RegisterPipelineTypes(); // the script creators need the cooks
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    const usize scripts = pipeline::RegisterAllCreators(creators);
    CHECK(scripts % 3 == 0u); // three tiers per language with a cook
    CHECK(creators.Count() == pipeline::kCreatorCount + scripts);

    constexpr StringView kRoot = u8"scratch_creators_db";
    constexpr StringView kSources = u8"scratch_creators_sources";
    (void)CreateDirectory(kSources);
    {
        foundation::vfs::NativeFileSystem mount(kRoot, DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount,
                                                foundation::core::BinarySerializerFactory(),
                                                u8".xasset");
        const String sourcesRoot = PathJoin(GetCurrentDirectory().AsView(), kSources);
        for (const pipeline::AssetCreator& creator : creators.All())
        {
            CAPTURE(creator.label);
            REQUIRE(creator.type != nullptr);
            foundation::content::Instance* instance =
                creator.Create(nullptr, db.RootGroup(), sourcesRoot.AsView());
            REQUIRE(instance != nullptr);
            CHECK(instance->TypeName() == creator.TypeName());
            // A creator with a default group lands there unless one was picked.
            if (!creator.defaultGroup.IsEmpty())
            {
                CHECK(creator.TargetFor(nullptr, db.RootGroup()) ==
                      db.RootGroup()->GetGroup(creator.defaultGroup.AsView()));
            }
        }

        // A name asked for is used; the registry finds a creator by label and, when there is
        // only one, by type.
        const pipeline::AssetCreator* scene = creators.FindByLabel(u8"scene");
        REQUIRE(scene != nullptr);
        CHECK(scene->setsDefaultScene);
        foundation::content::Instance* named =
            scene->Create(nullptr, db.RootGroup(), sourcesRoot.AsView(), u8"Level1");
        REQUIRE(named != nullptr);
        CHECK(named->Name() == u8"Level1");
        CHECK(creators.FindByType(scene->TypeName()) == scene);
        const pipeline::AssetCreator* pbr = creators.FindByLabel(u8"PBR Material");
        REQUIRE(pbr != nullptr);
        CHECK(creators.FindByType(pbr->TypeName()) == nullptr); // PBR and Unlit share the type
    }
    (void)RemoveDirectoryRecursive(kRoot);
    (void)RemoveDirectoryRecursive(kSources);
}

// Tripwire: every type a project can hold declares its display name, the kind the editor names it
// by (an asset page's bar, the picker); a type without one shows a blank there. What a project holds
// is every asset type, and what a builder cooks or a creator makes (the scene and prefab documents
// derive no Asset). A new type is caught as soon as it is registered.
TEST_CASE("pipeline.registration: every type a project holds declares a display name")
{
    pipeline::RegisterPipelineTypes();
    const auto named = [](const TypeInfo& type)
    {
        INFO("type: ", doctest::String(type.name));
        CHECK_FALSE(TypeAttrString(type, "displayName", StringView{}).IsEmpty());
    };
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::RegisterAllBuilders(builders, foundation::vfs::FindDataRoot());
    builders.ForEach([&named](const pipeline::IAssetBuilder& builder)
                     {
                         if (const TypeInfo* source = builder.AssetType())
                             named(*source);
                     });
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    (void)pipeline::RegisterAllCreators(creators);
    for (const pipeline::AssetCreator& creator : creators.All())
        if (creator.type != nullptr)
            named(*creator.type);

    const TypeInfo& asset = pipeline::Asset::StaticType();
    usize assets = 0;
    for (const TypeInfo* type : GlobalTypeRegistry().All())
    {
        bool isAsset = false;
        for (const TypeInfo* b = type->base; b != nullptr && !isAsset; b = b->base)
            isAsset = b == &asset;
        if (!isAsset || !GlobalSerializableRegistry().Contains(type->id))
            continue; // not an asset, or an abstract one (no project holds it as itself)
        ++assets;
        named(*type);
    }
    CHECK(assets >= 30u);
}
