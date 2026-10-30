// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The runtime's factory set is the engine composition's (engine-composition.md D6): attaching a
// manager registers every factory the composition describes and this host's services allow.
// Incident 2026-08-12: FontFactory existed and was tested, but NO host ever registered it -
// Bind<Font> failed silently in every runtime, masked in the editor by the dev-tree TTF
// fallback, and only visible in a dist export. The pins below keep that class of failure loud;
// the count is the composition's, so a factory a domain declares can no longer be left behind.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import foundation.resource;
import foundation.content;
import foundation.shell;
import foundation.graphics;
import foundation.vfs;
import foundation.xml.serialization;
import foundation.fonts.resource;
import foundation.texture.resource;
import foundation.heightfield;          // Heightfield product type (terrain factory pins)
import foundation.terrain.resource;     // TerrainResource + Splatmap product types
import engine.defaultapp;
import engine.composition; // FullComposition: the set the runtime composes from
import engine.gui; // the engine GUI every game has with the GUI built

using namespace foundation::core;
namespace runtime = foundation::runtime;

namespace
{
    // Headless host stub: no shell, no graphics, no windows - exactly the shape a CLI/test
    // process presents. Null Graphics() also exercises the texture-factory device gating.
    class StubHost final : public runtime::IApplicationHost
    {
    public:
        runtime::SubsystemManager& Subsystems() noexcept override { return m_context; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override
        {
            return nullptr;
        }
        foundation::graphics::RenderWindow*
        OpenWindow(const foundation::shell::WindowSettings&,
                   const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        runtime::SubsystemManager m_context{DefaultAllocator()};
    };
}

TEST_CASE("defaultapp: attaching a manager registers the composition's headless factory set, with "
          "the font and terrain pins")
{
    foundation::vfs::NativeFileSystem mount(u8"scratch_defaultapp_factories", foundation::core::DefaultAllocator());
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(),
                                            u8".xasset");
    foundation::resource::ResourceManager resources(foundation::core::DefaultAllocator(), db, nullptr);
    StubHost host;

    engine::runtime::DefaultApplication app;
    app.AttachResourceManager(&resources, host);

    // The runtime's set IS the composition's headless set: what a host with no device and no
    // shader system can create. A factory a domain declares is in both by construction.
    foundation::resource::ResourceFactorySet headless;
    foundation::resource::NoResourceServices none;
    engine::FullComposition().CreateFactories(headless, foundation::core::DefaultAllocator(), none);
    CHECK(resources.FactoryCount() == headless.Count());
    CHECK(resources.FactoryCount() == engine::FullComposition().FactoryDescriptionCount() - 2u);
    headless.ForEach([&](const foundation::resource::IResourceFactory& factory)
                     { CHECK(resources.HasFactory(factory.ProductType()->id)); });

    // The incident pin: the cooked default-UI font product MUST be constructible in every
    // runtime host - a dist player has no dev-tree TTF fallback.
    CHECK(resources.HasFactory(foundation::fonts::Font::StaticType().id));

    // Terrain pins: a cooked Terrain binds its whole CPU ref chain (bundle + grid + splat raster).
    CHECK(resources.HasFactory(foundation::terrain::TerrainResource::StaticType().id));
    CHECK(resources.HasFactory(foundation::heightfield::Heightfield::StaticType().id));
    CHECK(resources.HasFactory(foundation::terrain::SplatWeights::StaticType().id));

    // Device gating documented: no GraphicsDevice on the host means no texture factory.
    CHECK(!resources.HasFactory(foundation::texture::Texture::StaticType().id));
}

TEST_CASE("defaultapp: with the GUI built every game has the engine GUI, and it resolves guids through the game's resource manager")
{
    foundation::vfs::NativeFileSystem mount(u8"scratch_defaultapp_gui", foundation::core::DefaultAllocator());
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(),
                                            u8".xasset");
    foundation::resource::ResourceManager resources(foundation::core::DefaultAllocator(), db, nullptr);
    StubHost host;
    engine::runtime::DefaultApplication app;
    app.Configure(host);
    REQUIRE(app.GUI() != nullptr);
    CHECK(host.Subsystems().GetSubsystem<engine::gui::GuiSubsystem>() == app.GUI());
    app.AttachResourceManager(&resources, host);
    CHECK(app.GUI()->Resources().GetResourceManager() == &resources);
    app.AttachResourceManager(nullptr, host);
    CHECK(app.GUI()->Resources().GetResourceManager() == nullptr);
}
