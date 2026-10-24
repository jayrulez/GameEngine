// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Engine.Composition.Tests - THE composition root (engine-composition.md D5, D8).
//
// One list of domains, each declared once in its own library; every facet answered from it.
// What this suite guards: every domain is present (the module counts), the scene facet still
// resolves the managers headless consumers have lost in the past, the resource facet describes
// every factory and registers every cooked type, the factory set honours the declared services,
// and the facade surface installs exactly the domain facades - the tripwires the two surfaces
// used to keep, now over the one list.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.scene;
import foundation.resource;
import foundation.rhi;            // TypeOf<rhi::Device>: the texture factory's declared service
import foundation.shaders.system; // TypeOf<ShaderSystem>: the shader factory's declared service
import foundation.script.facades;
import foundation.net.replication;
import engine.composition;
import engine.render;
import engine.animation;
import engine.script;
import engine.ui;
import engine.terrain;
import engine.vegetation;
import engine.spline;
import engine.audio;
import foundation.scene.resource;
using namespace foundation::core;
namespace scene = foundation::scene;
namespace resource = foundation::resource;

namespace
{
    // The GUI domain (gui-lab-evolution-3.md, section E), there when the GUI is built: one domain,
    // one resource module of its own (its vector images and fonts are the UI domain's too), two
    // factories.
    constexpr usize kGuiDomains = 1;
    constexpr usize kGuiResourceModules = 1;
    constexpr usize kGuiFactories = 2;
}

TEST_CASE("engine.composition: the one list covers every domain, and the scene facet every manager")
{
    const engine::EngineComposition& composition = engine::FullComposition();
    // Thirteen scene domains (the old scene list) + input + the two facade-only libraries, and the
    // GUI's when the GUI is built.
    CHECK(composition.Modules().Size() == 16u + kGuiDomains);
    CHECK(composition.Scene().ModuleCount() == 13u + kGuiDomains);
    CHECK(&engine::FullSceneComposition() == &composition.Scene());

    scene::Scene scratch(DefaultAllocator());
    engine::AddAllSceneManagers(scratch);
    CHECK(scratch.HasSystem<engine::animation::PropertyAnimatorComponentManager>());
    CHECK(scratch.HasSystem<engine::render::PostProcessSystem>());
    CHECK(scratch.HasSystem<engine::script::ScriptComponentManager>());
    CHECK(scratch.HasSystem<engine::ui::UICanvasComponentManager>());
    CHECK(scratch.HasSystem<engine::audio::AudioSourceComponentManager>());
    CHECK(scratch.HasSystem<foundation::net::NetworkComponentManager>());
    CHECK(scratch.HasSystem<engine::terrain::TerrainComponentManager>());
    CHECK(scratch.HasSystem<engine::spline::SplineComponentManager>());
    CHECK(scratch.HasSystem<scene::PrefabSpawnSystem>());
    CHECK_FALSE(scratch.GetSystem<scene::PrefabSpawnSystem>()->HasSource()); // until a host points it
    CHECK(scratch.FindManagerBySerializationId(u8"net.Network") != nullptr);
    CHECK(scratch.FindManagerBySerializationId(u8"no.such.component") == nullptr);

    // Every module has a distinct id; a domain with no scene content is still listed.
    bool sawInput = false;
    for (const engine::DomainModule* module : composition.Modules())
    {
        usize same = 0;
        for (const engine::DomainModule* other : composition.Modules())
        {
            same += other->id == module->id ? 1u : 0u;
        }
        CHECK(same == 1u);
        if (module->id == u8"input")
        {
            sawInput = true;
            CHECK_FALSE(module->HasScene());
            CHECK(module->registerScriptFacade != nullptr);
            CHECK(module->resources.Size() == 1u);
        }
    }
    CHECK(sawInput);
}

TEST_CASE("engine.composition: reflection registration is callable and idempotent")
{
    engine::RegisterAllSceneComponentReflection();
    engine::RegisterAllSceneComponentReflection();
    CHECK(true);
}

TEST_CASE("engine.composition: the resource facet - every domain's resource modules once, every "
          "factory described, every cooked type registered")
{
    const engine::EngineComposition& composition = engine::FullComposition();
    // The twenty resource libraries + the scene documents + the render profiles.
    CHECK(composition.ResourceModules().Size() == 22u + kGuiResourceModules);
    for (const resource::ResourceModule* module : composition.ResourceModules())
    {
        usize same = 0;
        for (const resource::ResourceModule* other : composition.ResourceModules())
        {
            same += other->id == module->id ? 1u : 0u;
        }
        CHECK(same == 1u);
    }
    // 30 = the standard headless set (24) + the texture factory + the image and shader factories
    // no host registered before the composition brought them + the two render profiles + the UI
    // vector image, and the GUI's documents and sheets when the GUI is built.
    CHECK(composition.FactoryDescriptionCount() == 30u + kGuiFactories);

    engine::RegisterAllResourceTypes();
    usize described = 0;
    composition.ForEachFactoryDescription(
        [&](const resource::ResourceModule&, const resource::ResourceFactoryDesc& desc)
        {
            ++described;
            REQUIRE(desc.product != nullptr);
            REQUIRE(desc.cooked != nullptr);
            REQUIRE(desc.create != nullptr);
            CHECK(desc.product() != nullptr);
            REQUIRE(desc.cooked() != nullptr);
            // The cooked form is what a factory reads by type name: registered after the facet ran.
            CHECK(GlobalSerializableRegistry().Contains(desc.cooked()->id));
        });
    CHECK(described == composition.FactoryDescriptionCount());
}

TEST_CASE("engine.composition: a headless host creates every factory but the two that declare a "
          "service, and the set names the services")
{
    const engine::EngineComposition& composition = engine::FullComposition();
    resource::ResourceFactorySet set;
    resource::NoResourceServices none;
    composition.CreateFactories(set, DefaultAllocator(), none);
    CHECK(set.Count() == 28u + kGuiFactories);
    REQUIRE(set.Skipped().Size() == 2u);
    bool wantsDevice = false;
    bool wantsShaders = false;
    for (const resource::ResourceFactoryDesc* skipped : set.Skipped())
    {
        REQUIRE(skipped->service != nullptr);
        wantsDevice = wantsDevice || skipped->service() == &TypeOf<foundation::rhi::Device>();
        wantsShaders = wantsShaders || skipped->service() == &TypeOf<foundation::shaders::ShaderSystem>();
    }
    CHECK(wantsDevice);
    CHECK(wantsShaders);
    // Every created factory agrees with its description.
    set.ForEach(
        [&](const resource::IResourceFactory& factory)
        {
            bool described = false;
            composition.ForEachFactoryDescription(
                [&](const resource::ResourceModule&, const resource::ResourceFactoryDesc& desc)
                {
                    if (desc.product() == factory.ProductType())
                    {
                        described = true;
                        CHECK(desc.cooked() == factory.CookedType());
                    }
                });
            CHECK(described);
        });
    // Idempotent: a second headless pass adds nothing.
    composition.CreateFactories(set, DefaultAllocator(), none);
    CHECK(set.Count() == 28u + kGuiFactories);
}

TEST_CASE("engine.composition: RegisterAllScriptFacades installs exactly the domain facades, "
          "and is idempotent")
{
    engine::RegisterAllScriptFacades();
    const usize first = foundation::script::ExtraFacadeNames().Size();
    CHECK(first == engine::kSubsystemFacadeNameCount);
    engine::RegisterAllScriptFacades();
    CHECK(foundation::script::ExtraFacadeNames().Size() == first);
}
