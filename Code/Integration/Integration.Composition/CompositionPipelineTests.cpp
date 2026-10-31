// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Integration.Composition: the engine composition checked against the pipeline registration.
// A Ref<T>'s runtime type leads to the factory description's cooked form, then to the builder
// that produces it, then to the asset type an author creates; the scene format reference relies
// on that chain. The test needs both Engine and Pipeline, so it lives here: the engine suites
// stay free of the pipeline, and the web build, which has no pipeline, never sees it.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs; // FindDataRoot, to find the engine library the shader cook reads
import foundation.resource;
import engine.composition;    // FullComposition: every factory description
import pipeline.core;         // BuilderRegistry
import pipeline.registration; // RegisterPipelineTypes and RegisterAllBuilders

using namespace foundation::core;

TEST_CASE("integration.composition: every factory the composition describes reads a cooked form that "
          "is a registered serializable some builder produces - the runtime-to-asset link the scene "
          "format reference joins")
{
    engine::RegisterAllResourceTypes();
    pipeline::RegisterPipelineTypes();
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::RegisterAllBuilders(builders, foundation::vfs::FindDataRoot());
    REQUIRE(builders.Count() == pipeline::kBuilderCount);

    usize checked = 0;
    engine::FullComposition().ForEachFactoryDescription(
        [&](const foundation::resource::ResourceModule& module,
            const foundation::resource::ResourceFactoryDesc& desc)
        {
            const TypeInfo* product = desc.product();
            const TypeInfo* cooked = desc.cooked();
            REQUIRE(product != nullptr);
            INFO("module: ", doctest::String(reinterpret_cast<const char*>(module.id.Data()),
                                             static_cast<unsigned>(module.id.Size())));
            INFO("factory for: ", doctest::String(product->name != nullptr ? product->name : "<unnamed>"));
            REQUIRE(cooked != nullptr);
            // Every cooked form the factory reads, including the extra ones (a render texture
            // reads its own record as well as a texture record), must be registered, and some
            // builder must produce exactly that form. That links the runtime type to the asset.
            desc.ForEachCooked(
                [&](const TypeInfo* form)
                {
                    REQUIRE(form != nullptr);
                    INFO("cooked form: ", doctest::String(form->name != nullptr ? form->name : "<unnamed>"));
                    CHECK(GlobalSerializableRegistry().Contains(form->id));
                    bool produced = false;
                    builders.ForEach([&](const pipeline::IAssetBuilder& builder)
                                     { produced = produced || builder.ProductType() == form; });
                    CHECK(produced);
                });
            ++checked;
        });
    CHECK(checked == engine::FullComposition().FactoryDescriptionCount());
}
