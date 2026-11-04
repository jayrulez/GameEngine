// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// What a plugin's Engine part adds through EnginePluginServices: a domain and a scene manager,
// each reaching what it extends, and both gone when the host unloads.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.scene;
import foundation.resource;
import engine.composition;

using namespace foundation::core;
using namespace foundation::runtime;
namespace scene = foundation::scene;
namespace resource = foundation::resource;

namespace
{
    // Loads `ids` as a project's entries naming no platforms and no settings.
    PluginLoadReport LoadIds(PluginHost& host, Span<const StringView> ids)
    {
        Array<EnabledPlugin> entries;
        for (StringView id : ids)
        {
            EnabledPlugin entry;
            entry.id = String(id);
            entries.PushBack(Move(entry));
        }
        return host.Load(Span<const EnabledPlugin>(entries.Data(), entries.Size()));
    }
}

namespace
{
    int g_domainTypes = 0;
    void RegisterProbeTypes() { ++g_domainTypes; }
    const resource::ResourceModule kProbeResources{u8"services_probe", &RegisterProbeTypes, nullptr, 0};
    const resource::ResourceModule* const kProbeResourceList[] = {&kProbeResources};
    const engine::DomainModule kProbeDomain{
        .id = u8"services_probe", .resources = Span<const resource::ResourceModule* const>{kProbeResourceList, 1}};
    const engine::DomainModule kSceneDomain{.id = u8"services_scene",
                                            .installScene = [](scene::Scene&) {}};

    class ProbeManager final : public scene::SceneSystem
    {
    };
    void InstallProbeManager(scene::Scene& target) { target.AddSystem<ProbeManager>(); }

    bool g_refusedSceneDomain = false;

    void LoadProbe(PluginContext& context)
    {
        engine::EnginePluginServices* services = context.Find<engine::EnginePluginServices>();
        REQUIRE(services != nullptr);
        CHECK(services->AddDomain(kProbeDomain));
        g_refusedSceneDomain = !services->AddDomain(kSceneDomain);
        services->AddSceneManager(scene::SceneModuleContributions::Contribution{
            u8"services_probe", &InstallProbeManager, nullptr, TypeOf<ProbeManager>().id});
    }

    constexpr char8_t kProbeId[] = u8"services_probe";
    const PluginPart* ProbeEngine()
    {
        static constexpr PluginPart part{kProbeId, PluginPartKind::Engine, &LoadProbe, nullptr};
        return &part;
    }

    [[nodiscard]] Array<byte> ProbeManifest()
    {
        PluginManifest manifest;
        manifest.id = String(kProbeId);
        manifest.version = String(u8"0.1.0");
        manifest.engineVersion = String(u8"0.1");
        manifest.parts.PushBack(PluginManifestPart{String(u8"Engine")});
        manifest.platforms.PushBack(PluginPlatform{String(GetHostPlatformName())});
        MemoryStream buffer;
        REQUIRE(WritePluginManifest(manifest, buffer).IsOk());
        Array<byte> bytes;
        for (byte b : buffer.Bytes())
        {
            bytes.PushBack(b);
        }
        bytes.PushBack(byte{0});
        return bytes;
    }

    [[nodiscard]] bool HasModule(StringView id)
    {
        for (const engine::DomainModule* module : engine::FullComposition().Modules())
        {
            if (module->id == id)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("engine plugin services: a part's domain and scene manager arrive, and leave with the plugin")
{
    const Array<byte> manifest = ProbeManifest();
    const PluginPartEntryFn parts[] = {&ProbeEngine};
    const LinkedPlugin linked[] = {{reinterpret_cast<const char8_t*>(manifest.Data()), parts, 1}};
    const TypeId managerType = TypeOf<ProbeManager>().id;
    g_domainTypes = 0;

    SubsystemManager subsystems(DefaultAllocator());
    engine::EnginePluginServices services(DefaultAllocator());
    {
        PluginHost host(DefaultAllocator(), subsystems, HostRole::Player, u8"0.1.0");
        REQUIRE(services.Attach(host));
        REQUIRE(host.AddLinked(Span<const LinkedPlugin>(linked, 1)).IsOk());
        const StringView enabled[] = {kProbeId};
        REQUIRE(LoadIds(host, Span<const StringView>(enabled, 1)).loaded.Size() == 1u);

        CHECK(HasModule(u8"services_probe"));
        CHECK(g_domainTypes == 1);
        CHECK(g_refusedSceneDomain);
        CHECK_FALSE(HasModule(u8"services_scene"));
        CHECK(scene::SceneModuleContributions::Global().Contains(managerType));

        // A scene assembled from the composition gets the contributed manager.
        scene::Scene scratch(DefaultAllocator());
        engine::AddAllSceneManagers(scratch);
        CHECK(scratch.GetSystem<ProbeManager>() != nullptr);

        host.Unload();
        CHECK_FALSE(HasModule(u8"services_probe"));
        CHECK_FALSE(scene::SceneModuleContributions::Global().Contains(managerType));
    }
}
