// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.Mcp: the project's plugins through the tools. A plugin linked into this test the way
// the generated registry links one, with a settings type labelled as the project's settings are,
// is loaded from the project's entries by a plugin host; plugin_list reports every entry against
// it and plugin_settings_set changes the loaded plugin's settings, while an entry whose plugin is
// missing keeps its settings exactly as written.

#include <doctest/doctest.h>

#include <initializer_list>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.content;
import foundation.runtime;
import pipeline.core;
import pipeline.importer;
import pipeline.registration;
import engine.project;
import editor.project;
import editor.mcp;

using namespace foundation::core;
using namespace foundation::mcp;
namespace json = foundation::json;
namespace runtime = foundation::runtime;
using foundation::json::JsonValue;

namespace plugintools
{
    // The fixture plugin's settings.
    class MeterSettings final : public ISerializable
    {
        RTTI_OBJECT(MeterSettings, ISerializable)
    public:
        u32 volume = 3;
        bool loud = false;
        String greeting = String(u8"hello");

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "volume", volume);
            foundation::core::Serialize(ar, "loud", loud);
            foundation::core::Serialize(ar, "greeting", greeting);
        }
    };

    REFLECT_MEMBERS(MeterSettings, "rtti::plugintools")
    {
        builder.DataVersion(1);
        builder.Property<&MeterSettings::volume>("volume")
            .PropAttribute(engine::project::kSettingLabelAttribute, String(u8"Volume"))
            .PropAttribute("range", Float4{0.0f, 10.0f, 1.0f, 0.0f});
        builder.Property<&MeterSettings::loud>("loud").PropAttribute(engine::project::kSettingLabelAttribute,
                                                                     String(u8"Loud"));
        builder.Property<&MeterSettings::greeting>("greeting")
            .PropAttribute(engine::project::kSettingLabelAttribute, String(u8"Greeting"));
    }
}

namespace
{
    constexpr char8_t kMeter[] = u8"meter";
    plugintools::MeterSettings* g_meterSettings = nullptr;

    void LoadMeter(runtime::PluginContext& context) { g_meterSettings = context.Settings<plugintools::MeterSettings>(); }
    void UnloadMeter(runtime::PluginContext&) { g_meterSettings = nullptr; }
    const runtime::PluginPart* MeterEngine()
    {
        static constexpr runtime::PluginPart part{kMeter, runtime::PluginPartKind::Engine, &LoadMeter, &UnloadMeter};
        return &part;
    }

    // The meter's manifest, as the generated registry hands it over: supported on this platform alone.
    [[nodiscard]] Array<byte> MeterManifest()
    {
        runtime::PluginManifest manifest;
        manifest.id = String(kMeter);
        manifest.name = String(u8"Meter");
        manifest.version = String(u8"1.2.0");
        manifest.engineVersion = String(engine::project::kEngineVersionString);
        manifest.parts.PushBack(runtime::PluginManifestPart{String(u8"Engine")});
        manifest.platforms.PushBack(runtime::PluginPlatform{String(GetHostPlatformName())});
        MemoryStream buffer;
        REQUIRE(runtime::WritePluginManifest(manifest, buffer).IsOk());
        Array<byte> bytes;
        for (byte b : buffer.Bytes())
        {
            bytes.PushBack(b);
        }
        bytes.PushBack(byte{0});
        return bytes;
    }

    // A tools/call's result: the parsed text, and whether it was an error.
    struct Called
    {
        JsonValue value;
        String text;
        bool error = false;
    };

    Called Call(McpServer& server, StringView tool, JsonValue arguments)
    {
        JsonValue params = JsonValue::MakeObject();
        params.Set(u8"name", JsonValue::MakeString(String(tool)));
        params.Set(u8"arguments", Move(arguments));
        JsonValue req = JsonValue::MakeObject();
        req.Set(u8"jsonrpc", JsonValue::MakeString(u8"2.0"));
        req.Set(u8"id", JsonValue::MakeNumber(1));
        req.Set(u8"method", JsonValue::MakeString(u8"tools/call"));
        req.Set(u8"params", Move(params));
        LineOutcome line = server.HandleLine(req.ToString().AsView());
        REQUIRE(line.state == LineState::Answered);
        JsonValue result = json::Parse(line.response.AsView()).value.Get(u8"result");
        Called called;
        called.error = result.Get(u8"isError").AsBool();
        called.text = result.Get(u8"content").At(0).Get(u8"text").AsString();
        if (!called.error)
        {
            called.value = JsonValue::Parse(called.text.AsView());
        }
        return called;
    }

    [[nodiscard]] JsonValue Args(StringView id, JsonValue settings)
    {
        JsonValue args = JsonValue::MakeObject();
        args.Set(u8"id", JsonValue::MakeString(String(id)));
        args.Set(u8"settings", Move(settings));
        return args;
    }

    [[nodiscard]] String SettingsText(const runtime::EnabledPlugin& entry)
    {
        return String(StringView(reinterpret_cast<const utf8char*>(entry.settings.Data()), entry.settings.Size()));
    }
}

TEST_CASE("integration.mcp: an agent lists the project's plugins and sets a loaded plugin's settings")
{
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    editor::EditorLogBuffer logBuffer{DefaultAllocator()};
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    McpServer server;
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    editor::mcp::RegisterEngineTools(server, session, builders, importers, creators, logBuffer,
                                     editor::mcp::EngineToolPaths{}, operations);
    editor::mcp::RegisterProjectOpenTools(server, session, owner);
    u32 changed = 0;
    session.onSettingsChanged = [&changed]() { ++changed; };

    const StringView dir = u8"mcp_plugins";
    (void)RemoveDirectoryRecursive(dir);
    {
        JsonValue create = JsonValue::MakeObject();
        create.Set(u8"directory", JsonValue::MakeString(String(dir)));
        create.Set(u8"name", JsonValue::MakeString(String(u8"Plugins")));
        REQUIRE_FALSE(Call(server, u8"project_create", Move(create)).error);
        JsonValue open = JsonValue::MakeObject();
        open.Set(u8"directory", JsonValue::MakeString(String(dir)));
        REQUIRE_FALSE(Call(server, u8"project_open", Move(open)).error);
    }

    // The project enables the meter (here and on the web, which its manifest lacks) and a plugin
    // this build does not have, whose settings nothing here can read.
    engine::project::ProjectSettings& settings = session.project->Settings();
    runtime::EnabledPlugin meter;
    meter.id = String(kMeter);
    meter.platforms.PushBack(String(GetHostPlatformName()));
    meter.platforms.PushBack(String(u8"Web"));
    settings.plugins.PushBack(Move(meter));
    runtime::EnabledPlugin steam;
    steam.id = String(u8"steam");
    const StringView unknown = u8"<object name=\"mystery\"><i32 name=\"appId\">480</i32></object>";
    steam.settings.Resize(unknown.Size());
    MemCopy(steam.settings.Data(), unknown.Data(), unknown.Size());
    settings.plugins.PushBack(Move(steam));
    REQUIRE(session.project->SaveSettings().IsOk());

    // The host's own plugins (project_open loaded the ones it links, none here): every entry is
    // missing, and nothing can be set.
    REQUIRE(session.plugins != nullptr);
    {
        const Called listed = Call(server, u8"plugin_list", JsonValue::MakeObject());
        REQUIRE_FALSE(listed.error);
        CHECK(listed.value.Get(u8"plugins").At(0).Get(u8"state").AsString() == String(u8"missing"));
        const Called set = Call(server, u8"plugin_settings_set", Args(kMeter, JsonValue::MakeObject()));
        CHECK(set.error);
        CHECK(set.text.AsView().Contains(u8"not in this build; its settings are kept as written"));
    }
    session.plugins = nullptr;
    {
        const Called set = Call(server, u8"plugin_settings_set", Args(kMeter, JsonValue::MakeObject()));
        CHECK(set.text.AsView().Contains(u8"this host loads no plugins; its settings are kept as written"));
    }

    runtime::SubsystemManager subsystems(DefaultAllocator());
    runtime::PluginHost host(DefaultAllocator(), subsystems, runtime::HostRole::Mcp,
                             engine::project::kEngineVersionString);
    const Array<byte> manifest = MeterManifest();
    const runtime::PluginPartEntryFn parts[] = {&MeterEngine};
    const runtime::LinkedPlugin linked[] = {{reinterpret_cast<const char8_t*>(manifest.Data()), parts, 1}};
    REQUIRE(host.AddLinked(Span<const runtime::LinkedPlugin>(linked, 1)).IsOk());
    (void)host.Load(Span<const runtime::EnabledPlugin>(settings.plugins.Data(), settings.plugins.Size()));
    REQUIRE(g_meterSettings != nullptr);
    session.plugins = &host;

    const Called listed = Call(server, u8"plugin_list", JsonValue::MakeObject());
    REQUIRE_FALSE(listed.error);
    CHECK(listed.value.Get(u8"platform").AsString() == String(GetHostPlatformName()));
    const JsonValue plugins = listed.value.Get(u8"plugins");
    REQUIRE(plugins.Count() == 2);
    const JsonValue first = plugins.At(0);
    CHECK(first.Get(u8"id").AsString() == String(kMeter));
    CHECK(first.Get(u8"state").AsString() == String(u8"loaded"));
    CHECK(first.Get(u8"name").AsString() == String(u8"Meter"));
    CHECK(first.Get(u8"version").AsString() == String(u8"1.2.0"));
    REQUIRE(first.Get(u8"unsupportedPlatforms").Count() == 1);
    CHECK(first.Get(u8"unsupportedPlatforms").At(0).AsString() == String(u8"Web"));
    CHECK(first.Get(u8"settings").Get(u8"volume").AsNumber() == doctest::Approx(3.0));
    CHECK(first.Get(u8"settings").Get(u8"greeting").AsString() == String(u8"hello"));
    CHECK(first.Get(u8"settingsEditable").AsBool());
    const JsonValue second = plugins.At(1);
    CHECK(second.Get(u8"state").AsString() == String(u8"missing"));
    CHECK(second.Get(u8"reason").AsString().AsView().Contains(u8"not in this build"));
    CHECK(second.Get(u8"settings").IsNull());
    CHECK_FALSE(second.Get(u8"settingsEditable").AsBool());

    // Refusals change nothing: a setting it does not have, a value out of range, a missing plugin,
    // an id the project does not enable.
    {
        JsonValue misspelled = JsonValue::MakeObject();
        misspelled.Set(u8"volumme", JsonValue::MakeNumber(5));
        const Called set = Call(server, u8"plugin_settings_set", Args(kMeter, Move(misspelled)));
        CHECK(set.error);
        CHECK(set.text.AsView().Contains(u8"has no setting 'volumme' (its settings: volume, loud, greeting)"));
        JsonValue tooLoud = JsonValue::MakeObject();
        tooLoud.Set(u8"volume", JsonValue::MakeNumber(11));
        tooLoud.Set(u8"greeting", JsonValue::MakeString(String(u8"never")));
        CHECK(Call(server, u8"plugin_settings_set", Args(kMeter, Move(tooLoud))).error);
        CHECK(g_meterSettings->greeting == String(u8"hello"));
        const Called missing = Call(server, u8"plugin_settings_set", Args(u8"steam", JsonValue::MakeObject()));
        CHECK(missing.error);
        CHECK(missing.text.AsView().Contains(u8"not in this build; its settings are kept as written"));
        CHECK(Call(server, u8"plugin_settings_set", Args(u8"absent", JsonValue::MakeObject())).error);
        CHECK(changed == 0u);
    }

    // A change reaches the plugin at once, the project's entry and the saved manifest.
    JsonValue change = JsonValue::MakeObject();
    change.Set(u8"volume", JsonValue::MakeNumber(7));
    change.Set(u8"loud", JsonValue::MakeBool(true));
    const Called set = Call(server, u8"plugin_settings_set", Args(kMeter, Move(change)));
    REQUIRE_FALSE(set.error);
    CHECK(changed == 1u);
    CHECK(set.value.Get(u8"settings").Get(u8"volume").AsNumber() == doctest::Approx(7.0));
    CHECK(g_meterSettings->volume == 7u);
    CHECK(g_meterSettings->loud);
    CHECK(g_meterSettings->greeting == String(u8"hello")); // not given: unchanged

    host.Unload();
    session.plugins = nullptr;
    owner.project.Reset();
    session.project = nullptr;
    UniquePtr<editor::EditorProject> reopened = editor::EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(reopened));
    const runtime::EnabledPlugin* saved = reopened->Settings().FindPlugin(kMeter);
    REQUIRE(saved != nullptr);
    plugintools::MeterSettings reread;
    REQUIRE(runtime::ReadPluginSettings(Span<const u8>(saved->settings.Data(), saved->settings.Size()), reread,
                                        DefaultAllocator())
                .IsOk());
    CHECK(reread.volume == 7u);
    CHECK(reread.loud);
    const runtime::EnabledPlugin* kept = reopened->Settings().FindPlugin(u8"steam");
    REQUIRE(kept != nullptr);
    CHECK(SettingsText(*kept).AsView().Contains(u8"<i32 name=\"appId\">480</i32>")); // kept, though unread

    reopened.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

TEST_CASE("integration.mcp: an agent enables and disables plugins in the project")
{
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    editor::EditorLogBuffer logBuffer{DefaultAllocator()};
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    McpServer server;
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    editor::mcp::RegisterEngineTools(server, session, builders, importers, creators, logBuffer,
                                     editor::mcp::EngineToolPaths{}, operations);
    editor::mcp::RegisterProjectOpenTools(server, session, owner);

    const StringView dir = u8"mcp_plugins_enable";
    (void)RemoveDirectoryRecursive(dir);
    {
        JsonValue create = JsonValue::MakeObject();
        create.Set(u8"directory", JsonValue::MakeString(String(dir)));
        create.Set(u8"name", JsonValue::MakeString(String(u8"Enable")));
        REQUIRE_FALSE(Call(server, u8"project_create", Move(create)).error);
        JsonValue open = JsonValue::MakeObject();
        open.Set(u8"directory", JsonValue::MakeString(String(dir)));
        REQUIRE_FALSE(Call(server, u8"project_open", Move(open)).error);
    }

    // The host knows the meter (this platform only), so its platforms are checked against it.
    runtime::SubsystemManager subsystems(DefaultAllocator());
    runtime::PluginHost host(DefaultAllocator(), subsystems, runtime::HostRole::Mcp,
                             engine::project::kEngineVersionString);
    const Array<byte> manifest = MeterManifest();
    const runtime::PluginPartEntryFn parts[] = {&MeterEngine};
    const runtime::LinkedPlugin linked[] = {{reinterpret_cast<const char8_t*>(manifest.Data()), parts, 1}};
    REQUIRE(host.AddLinked(Span<const runtime::LinkedPlugin>(linked, 1)).IsOk());
    session.plugins = &host;

    const auto enable = [&server](StringView id, std::initializer_list<const char8_t*> platforms)
    {
        JsonValue args = JsonValue::MakeObject();
        args.Set(u8"id", JsonValue::MakeString(String(id)));
        JsonValue list = JsonValue::MakeArray();
        for (const char8_t* platform : platforms)
        {
            list.Add(JsonValue::MakeString(String(StringView(platform))));
        }
        args.Set(u8"platforms", Move(list));
        return Call(server, u8"plugin_enable", Move(args));
    };
    const auto disable = [&server](StringView id)
    {
        JsonValue args = JsonValue::MakeObject();
        args.Set(u8"id", JsonValue::MakeString(String(id)));
        return Call(server, u8"plugin_disable", Move(args));
    };

    // Refusals leave the list as it was.
    const Called web = enable(kMeter, {u8"Web"});
    CHECK(web.error);
    CHECK(web.text.AsView().Contains(u8"plugin 'meter' does not support Web (it supports: "));
    CHECK(enable(u8"Steam!", {}).error);
    {
        JsonValue args = JsonValue::MakeObject();
        args.Set(u8"id", JsonValue::MakeString(String(u8"game")));
        args.Set(u8"path", JsonValue::MakeString(String(u8"Nowhere")));
        const Called noPackage = Call(server, u8"plugin_enable", Move(args));
        CHECK(noPackage.error);
        CHECK(noPackage.text.AsView().Contains(u8"'Nowhere' holds no plugin manifest"));
    }
    CHECK(session.project->Settings().plugins.IsEmpty());

    // The meter here, and a plugin this build does not know, which only the project can hold.
    const Called meter = enable(kMeter, {GetHostPlatformName().Data()});
    REQUIRE_FALSE(meter.error);
    REQUIRE(meter.value.Get(u8"plugins").Count() == 1);
    CHECK(meter.value.Get(u8"plugins").At(0).Get(u8"platforms").At(0).AsString() == String(GetHostPlatformName()));
    const Called steam = enable(u8"steam", {u8"Linux64", u8"Win64"});
    REQUIRE_FALSE(steam.error);
    CHECK(steam.value.Get(u8"plugins").At(1).Get(u8"state").AsString() == String(u8"missing"));

    // Enabling again changes its platforms and keeps its settings.
    runtime::EnabledPlugin* entry = session.project->Settings().FindPlugin(u8"steam");
    REQUIRE(entry != nullptr);
    const StringView kept = u8"<i32 name=\"appId\">480</i32>";
    entry->settings.Resize(kept.Size());
    MemCopy(entry->settings.Data(), kept.Data(), kept.Size());
    REQUIRE_FALSE(enable(u8"steam", {}).error);
    entry = session.project->Settings().FindPlugin(u8"steam");
    REQUIRE(entry != nullptr);
    CHECK(entry->platforms.IsEmpty());
    CHECK(SettingsText(*entry) == String(kept));

    // Disabling removes the entry; one the project does not enable is refused.
    const Called gone = disable(u8"steam");
    REQUIRE_FALSE(gone.error);
    CHECK(gone.value.Get(u8"plugins").Count() == 1);
    CHECK(disable(u8"steam").error);

    session.plugins = nullptr;
    owner.project.Reset();
    session.project = nullptr;
    UniquePtr<editor::EditorProject> reopened = editor::EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(reopened));
    REQUIRE(reopened->Settings().plugins.Size() == 1u);
    CHECK(reopened->Settings().plugins[0].id == String(kMeter));
    reopened.Reset();
    (void)RemoveDirectoryRecursive(dir);
}

namespace plugintools
{
    // A plugin's asset type, its product and its builder.
    class GadgetAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(GadgetAsset, pipeline::Asset)
    public:
        i32 sprockets = 4;
        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            foundation::core::Serialize(ar, "sprockets", sprockets);
        }
    };
    RTTI_DEFINE_OBJECT(GadgetAsset, "rtti::plugintools")

    class GadgetProduct final : public ISerializable
    {
        RTTI_OBJECT(GadgetProduct, ISerializable)
    public:
        i32 cooked = 0;
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "cooked", cooked); }
    };
    RTTI_DEFINE_OBJECT(GadgetProduct, "rtti::plugintools")

    class GadgetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override { return &GadgetAsset::StaticType(); }
        [[nodiscard]] const TypeInfo* ProductType() const override { return &GadgetProduct::StaticType(); }
        [[nodiscard]] u32 Version() const override { return 1; }
        [[nodiscard]] Status Build(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx) override
        {
            GadgetProduct product;
            product.cooked = static_cast<const GadgetAsset&>(asset).sprockets * 10;
            return ctx.output->WriteObject(product);
        }
    };
}

namespace
{
    constexpr char8_t kGadget[] = u8"gadget";

    // The gadget's Pipeline part: its types, its builder and its creator, through the host's
    // pipeline service.
    void LoadGadgetPipeline(runtime::PluginContext& context)
    {
        context.Types().Register(plugintools::GadgetAsset::StaticType());
        context.Types().Register(plugintools::GadgetProduct::StaticType());
        RegisterSerializable<plugintools::GadgetAsset>(context.Serializables());
        RegisterSerializable<plugintools::GadgetProduct>(context.Serializables());
        pipeline::PipelinePluginServices* services = context.Find<pipeline::PipelinePluginServices>();
        REQUIRE(services != nullptr);
        CHECK(services->AddBuilder(MakeUnique<plugintools::GadgetBuilder>(context.Allocator())));
        pipeline::AssetCreator creator;
        creator.label = String(u8"Gadget");
        creator.category = String(u8"Plugins");
        creator.type = &plugintools::GadgetAsset::StaticType();
        creator.run = [](const pipeline::AssetCreationContext& at)
        {
            plugintools::GadgetAsset gadget;
            return pipeline::CreateWrittenInstance(at.picked != nullptr ? at.picked : at.root,
                                                   at.name.IsEmpty() ? StringView(u8"Gadget") : at.name,
                                                   plugintools::GadgetAsset::StaticType(), gadget);
        };
        CHECK(services->AddCreator(Move(creator)));
    }
    const runtime::PluginPart* GadgetPipeline()
    {
        static constexpr runtime::PluginPart part{kGadget, runtime::PluginPartKind::Pipeline, &LoadGadgetPipeline, nullptr};
        return &part;
    }

    [[nodiscard]] Array<byte> GadgetManifest()
    {
        runtime::PluginManifest manifest;
        manifest.id = String(kGadget);
        manifest.name = String(u8"Gadget");
        manifest.version = String(u8"1.0");
        manifest.engineVersion = String(engine::project::kEngineVersionString);
        manifest.parts.PushBack(runtime::PluginManifestPart{String(u8"Pipeline")});
        manifest.platforms.PushBack(runtime::PluginPlatform{String(GetHostPlatformName())});
        MemoryStream buffer;
        REQUIRE(runtime::WritePluginManifest(manifest, buffer).IsOk());
        Array<byte> bytes;
        for (byte b : buffer.Bytes())
        {
            bytes.PushBack(b);
        }
        bytes.PushBack(byte{0});
        return bytes;
    }

    [[nodiscard]] bool HasCreator(const JsonValue& listed, StringView label)
    {
        const JsonValue creators = listed.Get(u8"creators");
        for (i64 i = 0; i < creators.Count(); ++i)
        {
            if (creators.At(static_cast<usize>(i)).Get(u8"label").AsString().AsView() == label)
            {
                return true;
            }
        }
        return false;
    }
}

// A plugin's Pipeline part adds to the host it loads in: through the standalone MCP host, a project
// that enables the plugin offers its creator, creates its asset and cooks it with its builder, and a
// project that does not, or the same host after the plugin unloads, has none of it.
TEST_CASE("integration.mcp: a plugin's asset type is created and cooked through the standalone host, and leaves with it")
{
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    editor::EditorLogBuffer logBuffer{DefaultAllocator()};
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    McpServer server;
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    editor::mcp::RegisterEngineTools(server, session, builders, importers, creators, logBuffer,
                                     editor::mcp::EngineToolPaths{}, operations);
    const Array<byte> manifest = GadgetManifest();
    const runtime::PluginPartEntryFn parts[] = {&GadgetPipeline};
    const runtime::LinkedPlugin linked[] = {{reinterpret_cast<const char8_t*>(manifest.Data()), parts, 1}};
    editor::mcp::RegisterProjectOpenTools(
        server, session, owner, &creators, StringView{},
        editor::mcp::ProjectPlugins{Span<const runtime::LinkedPlugin>(linked, 1),
                                    pipeline::PipelineRegistries{&builders, &importers, &creators}});
    const usize buildersBefore = builders.Count();

    const auto open = [&server](StringView dir)
    {
        JsonValue args = JsonValue::MakeObject();
        args.Set(u8"directory", JsonValue::MakeString(String(dir)));
        REQUIRE_FALSE(Call(server, u8"project_open", Move(args)).error);
    };
    const StringView dir = u8"mcp_plugin_pipeline";
    const StringView plain = u8"mcp_plugin_pipeline_plain";
    (void)RemoveDirectoryRecursive(dir);
    (void)RemoveDirectoryRecursive(plain);
    for (StringView project : {dir, plain})
    {
        JsonValue create = JsonValue::MakeObject();
        create.Set(u8"directory", JsonValue::MakeString(String(project)));
        create.Set(u8"name", JsonValue::MakeString(String(u8"Pipeline")));
        REQUIRE_FALSE(Call(server, u8"project_create", Move(create)).error);
    }

    // Enabled, then opened again: the plugin loads with the project.
    open(dir);
    CHECK_FALSE(HasCreator(Call(server, u8"asset_creators", JsonValue::MakeObject()).value, u8"Gadget"));
    {
        JsonValue args = JsonValue::MakeObject();
        args.Set(u8"id", JsonValue::MakeString(String(kGadget)));
        REQUIRE_FALSE(Call(server, u8"plugin_enable", Move(args)).error);
    }
    open(dir);
    REQUIRE(session.plugins != nullptr);
    CHECK(session.plugins->IsLoaded(kGadget));
    CHECK(builders.Count() == buildersBefore + 1u);
    CHECK(HasCreator(Call(server, u8"asset_creators", JsonValue::MakeObject()).value, u8"Gadget"));

    JsonValue create = JsonValue::MakeObject();
    create.Set(u8"creator", JsonValue::MakeString(String(u8"Gadget")));
    create.Set(u8"name", JsonValue::MakeString(String(u8"Sprocket")));
    const Called made = Call(server, u8"asset_create", Move(create));
    REQUIRE_FALSE(made.error);
    CHECK(made.value.Get(u8"type").AsString() == String(u8"GadgetAsset"));
    const Called cooked = Call(server, u8"asset_cook", JsonValue::MakeObject());
    REQUIRE_FALSE(cooked.error);
    CHECK(cooked.value.Get(u8"failed").AsNumber() == doctest::Approx(0.0));
    CHECK(cooked.value.Get(u8"cooked").AsNumber() >= 1.0); // the gadget, by the plugin's builder
    Guid id;
    REQUIRE(Guid::TryParse(made.value.Get(u8"guid").AsString().AsView(), id));
    RefPtr<ISerializable> read = session.project->CookedDb().ReadObject(id);
    auto* product = read ? Cast<plugintools::GadgetProduct>(read.Get()) : nullptr;
    REQUIRE(product != nullptr);
    CHECK(product->cooked == 40);

    // Another project, which does not enable it: the plugin and everything it added are gone.
    open(plain);
    CHECK_FALSE(session.plugins->IsLoaded(kGadget));
    CHECK(builders.Count() == buildersBefore);
    CHECK(builders.Find(&plugintools::GadgetAsset::StaticType()) == nullptr);
    CHECK_FALSE(HasCreator(Call(server, u8"asset_creators", JsonValue::MakeObject()).value, u8"Gadget"));

    session.plugins = nullptr;
    owner.plugins.Reset();
    session.project = nullptr;
    owner.project.Reset();
    (void)RemoveDirectoryRecursive(dir);
    (void)RemoveDirectoryRecursive(plain);
}

namespace
{
    constexpr char8_t kGreeter[] = u8"greeter";
    bool g_takenRefused = false;

    // The greeter's Tooling part: one tool of its own, and one whose name the engine holds.
    void LoadGreeterTooling(runtime::PluginContext& context)
    {
        editor::mcp::McpPluginServices* services = context.Find<editor::mcp::McpPluginServices>();
        REQUIRE(services != nullptr);
        CHECK(services->AddTools(
            [](editor::mcp::PluginTools& tools)
            {
                editor::mcp::ProjectSession* session = &tools.Session();
                CHECK(tools.RegisterTool(
                    String(u8"greeter_hello"), String(u8"Greets the open project"), SchemaBuilder().Build(),
                    ToolAnnotations::ReadOnly(),
                    [session](const JsonValue&) -> ToolResult
                    {
                        JsonValue out = JsonValue::MakeObject();
                        out.Set(u8"hello", JsonValue::MakeString(session->project != nullptr ? String(session->project->Name()) : String()));
                        return out;
                    }));
                g_takenRefused = !tools.RegisterTool(String(u8"project_info"), String(u8"an impostor"), SchemaBuilder().Build(),
                                                     ToolAnnotations::ReadOnly(),
                                                     [](const JsonValue&) -> ToolResult { return JsonValue::MakeObject(); });
            }));
    }
    const runtime::PluginPart* GreeterTooling()
    {
        static constexpr runtime::PluginPart part{kGreeter, runtime::PluginPartKind::Tooling, &LoadGreeterTooling, nullptr};
        return &part;
    }

    [[nodiscard]] Array<byte> GreeterManifest()
    {
        runtime::PluginManifest manifest;
        manifest.id = String(kGreeter);
        manifest.version = String(u8"1.0");
        manifest.engineVersion = String(engine::project::kEngineVersionString);
        manifest.parts.PushBack(runtime::PluginManifestPart{String(u8"Tooling")});
        manifest.platforms.PushBack(runtime::PluginPlatform{String(GetHostPlatformName())});
        MemoryStream buffer;
        REQUIRE(runtime::WritePluginManifest(manifest, buffer).IsOk());
        Array<byte> bytes;
        for (byte b : buffer.Bytes())
        {
            bytes.PushBack(b);
        }
        bytes.PushBack(byte{0});
        return bytes;
    }
}

// A plugin's Tooling part adds MCP tools, which every server the host serves offers: the standalone
// host's, there before the project opened, and a server started after the plugin loaded (the editor
// starts its own once the project is open). They leave every server with their plugin, and none may
// take a name a server already holds.
TEST_CASE("integration.mcp: a plugin's MCP tool is served by a server there before it loads and one started after, and leaves with it")
{
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    pipeline::ImporterRegistry importers{DefaultAllocator()};
    pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
    editor::EditorLogBuffer logBuffer{DefaultAllocator()};
    editor::mcp::McpPluginServices pluginTools{DefaultAllocator()};
    editor::mcp::ProjectSession session;
    editor::mcp::ProjectOwner owner;
    McpServer server;
    editor::mcp::InlineProjectOperations operations(session, builders, String(), String());
    editor::mcp::RegisterEngineTools(server, session, builders, importers, creators, logBuffer,
                                     editor::mcp::EngineToolPaths{}, operations);
    const Array<byte> manifest = GreeterManifest();
    const runtime::PluginPartEntryFn parts[] = {&GreeterTooling};
    const runtime::LinkedPlugin linked[] = {{reinterpret_cast<const char8_t*>(manifest.Data()), parts, 1}};
    pluginTools.AttachServer(server, session);
    editor::mcp::RegisterProjectOpenTools(
        server, session, owner, &creators, StringView{},
        editor::mcp::ProjectPlugins{Span<const runtime::LinkedPlugin>(linked, 1),
                                    pipeline::PipelineRegistries{&builders, &importers, &creators}, &pluginTools});
    const usize engineTools = server.ToolCount();

    const StringView dir = u8"mcp_plugin_tooling";
    const StringView plain = u8"mcp_plugin_tooling_plain";
    (void)RemoveDirectoryRecursive(dir);
    (void)RemoveDirectoryRecursive(plain);
    for (StringView project : {dir, plain})
    {
        JsonValue create = JsonValue::MakeObject();
        create.Set(u8"directory", JsonValue::MakeString(String(project)));
        create.Set(u8"name", JsonValue::MakeString(String(u8"Greeted")));
        REQUIRE_FALSE(Call(server, u8"project_create", Move(create)).error);
    }
    const auto open = [&server](StringView project)
    {
        JsonValue args = JsonValue::MakeObject();
        args.Set(u8"directory", JsonValue::MakeString(String(project)));
        REQUIRE_FALSE(Call(server, u8"project_open", Move(args)).error);
    };
    open(dir);
    {
        JsonValue args = JsonValue::MakeObject();
        args.Set(u8"id", JsonValue::MakeString(String(kGreeter)));
        REQUIRE_FALSE(Call(server, u8"plugin_enable", Move(args)).error);
    }
    g_takenRefused = false;
    open(dir); // opened again: its plugins load with it

    // The standalone host's server, there before the plugin loaded, serves its tool.
    CHECK(server.ToolCount() == engineTools + 1u);
    CHECK(g_takenRefused); // the engine's project_info keeps its name
    const Called hello = Call(server, u8"greeter_hello", JsonValue::MakeObject());
    REQUIRE_FALSE(hello.error);
    CHECK(hello.value.Get(u8"hello").AsString() == String(u8"Greeted"));
    CHECK(Call(server, u8"project_info", JsonValue::MakeObject()).value.Has(u8"settings")); // still the engine's

    // A server started after the plugin loaded serves it too, and leaves it as it stops.
    {
        McpServer later;
        later.RegisterTool(String(u8"project_info"), String(u8"the server's own"), SchemaBuilder().Build(),
                           ToolAnnotations::ReadOnly(), [](const JsonValue&) -> ToolResult { return JsonValue::MakeObject(); });
        pluginTools.AttachServer(later, session);
        CHECK(later.HasTool(u8"greeter_hello"));
        CHECK(pluginTools.ToolsServedBy(later).Size() == 1u);
        pluginTools.DetachServer(later);
        CHECK_FALSE(later.HasTool(u8"greeter_hello"));
    }

    // A project that does not enable it: the tool leaves the server with its plugin.
    open(plain);
    CHECK_FALSE(server.HasTool(u8"greeter_hello"));
    CHECK(server.ToolCount() == engineTools);

    session.plugins = nullptr;
    owner.plugins.Reset();
    session.project = nullptr;
    owner.project.Reset();
    pluginTools.DetachServer(server);
    (void)RemoveDirectoryRecursive(dir);
    (void)RemoveDirectoryRecursive(plain);
}
