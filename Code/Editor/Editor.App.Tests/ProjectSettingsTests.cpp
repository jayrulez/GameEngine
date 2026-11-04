// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Project Settings over its markup (Data/Editor/ProjectSettings.xml). The manifest's labelled
// settings are the grid's rows, in their categories. An asset setting is a reference (a slot) to the
// source type it names and a list setting is a list of them; an enum shows its choices by name; MSAA
// offers the render subsystem's levels; and the project's engine version is read only. Save writes
// the rows back (dropping a list's empty and repeated entries) and persists them; Cancel leaves the
// manifest as it was. The project's plugins follow, a group each: its state in the plugin host, and
// a loaded plugin's settings, which Save writes into its entry.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.scene.resource;
import foundation.runtime; // the project's plugins
import engine.project;
import editor.core;
import editor.app;

using namespace foundation::core;
using editor::app::ProjectSettingsDialog;
using editor::app::ResourceRefEditor;
using editor::app::ResourceRefServices;
namespace gui = foundation::gui;

namespace
{
    RefPtr<gui::Node> LoadSettings()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(editor::app::kProjectSettingsDialogPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    String Text(const Guid& id)
    {
        utf8char chars[37];
        id.ToChars(chars);
        return String(StringView(chars, 36));
    }
    struct Bed
    {
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        ResourceRefServices refs;
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        UniquePtr<ProjectSettingsDialog> dialog;
        const Guid scene{0x5ce0u, 0x1u};
        const Guid font{0xf0u, 0x2u};

        Bed()
        {
            (void)RemoveDirectoryRecursive(u8"project_settings_project");
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"project_settings_project", u8"Settings").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), u8"project_settings_project");
            REQUIRE(project);
            GlobalTypeRegistry().Register(foundation::scene::SceneDocument::StaticType());
            project->Settings().defaultSceneId = scene;
            project->Settings().uiFontIds.PushBack(font);
            project->Settings().renderMsaaSamples = 4;
            context.SetProject(project.Get());
            root->SetSize(Float2{900.0f, 700.0f});
            dialog = MakeUnique<ProjectSettingsDialog>(DefaultAllocator(), DefaultAllocator(), context, refs,
                                                       Span<const foundation::runtime::LinkedPlugin>{}, LoadSettings());
            REQUIRE(dialog->Open(*root));
            (void)root->UpdateLayout();
        }
        ~Bed()
        {
            dialog.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(u8"project_settings_project");
        }
        gui::PropertyBag& Bag() { return *dialog->Rows(); }
        void Edit(StringView path, const gui::Variant& value)
        {
            Bag().BeginEdit(path);
            Bag().CommitEdit(path, gui::PropertyWrite{value});
        }
    };
}

TEST_CASE("project settings: the labelled settings as rows in their categories; assets as references to the type they name; choices by name")
{
    Bed bed;
    gui::PropertyBag& bag = bed.Bag();
    const gui::PropertyInfo* name = bag.FindInfo(u8"name");
    REQUIRE(name != nullptr);
    CHECK(name->DisplayName == String(u8"Name"));
    CHECK(name->Category == String(u8"General"));
    CHECK(bag.GetValue(u8"name", 0).AsString() == StringView(u8"Settings"));

    const gui::PropertyInfo* scene = bag.FindInfo(u8"defaultSceneId");
    REQUIRE(scene != nullptr);
    CHECK(scene->Kind == gui::PropertyKind::Reference);
    CHECK(scene->ReferenceType == &foundation::scene::SceneDocument::StaticType());
    CHECK(scene->Category == String(u8"Startup"));
    CHECK(bag.GetValue(u8"defaultSceneId", 0).AsString() == Text(bed.scene).AsView());
    gui::PropertyEditorRegistry& editors = bed.dialog->Grid()->GetEditors();
    CHECK(Cast<ResourceRefEditor>(editors.Create(editors.Choose(*scene), DefaultAllocator()).Get()) != nullptr); // its slot

    const gui::PropertyInfo* fonts = bag.FindInfo(u8"uiFontIds");
    REQUIRE(fonts != nullptr);
    CHECK(fonts->Kind == gui::PropertyKind::List);
    CHECK(bag.ElementCount(u8"uiFontIds", 0) == 1u);
    CHECK(bag.GetValue(u8"uiFontIds.0", 0).AsString() == Text(bed.font).AsView());

    const gui::PropertyInfo* msaa = bag.FindInfo(editor::app::kProjectSettingsDialogMsaaSetting);
    REQUIRE(msaa != nullptr);
    CHECK(msaa->Kind == gui::PropertyKind::Enum);
    REQUIRE(msaa->Choices.Size() == 3u);
    CHECK(msaa->Choices[2].Name == String(u8"4x"));
    CHECK(bag.GetValue(editor::app::kProjectSettingsDialogMsaaSetting, 0).AsInt() == 4);

    const gui::PropertyInfo* mode = bag.FindInfo(u8"windowMode");
    REQUIRE(mode != nullptr);
    CHECK(mode->Kind == gui::PropertyKind::Enum);
    CHECK(mode->Choices[0].Name == String(u8"Windowed"));

    const gui::PropertyInfo* width = bag.FindInfo(u8"windowWidth");
    REQUIRE(width != nullptr);
    CHECK(width->HasRange);
    CHECK(width->Min == doctest::Approx(1.0));

    const gui::PropertyInfo* version = bag.FindInfo(editor::app::kProjectSettingsDialogEngineVersionRow);
    REQUIRE(version != nullptr);
    CHECK(version->ReadOnly);
}

TEST_CASE("project settings: Save writes the rows back, a list's empty and repeated entries dropped, and persists; Cancel leaves it")
{
    Bed bed;
    bed.Edit(u8"name", gui::Variant(StringView(u8"Renamed")));
    // Cancel first: the manifest stays as it was.
    bed.dialog->Close();
    CHECK(bed.project->Settings().name == String(u8"Settings"));
    CHECK(bed.project->Settings().defaultSceneId == bed.scene);

    REQUIRE(bed.dialog->Open(*bed.root));
    CHECK(bed.Bag().GetValue(u8"name", 0).AsString() == StringView(u8"Settings")); // the rows again, from the manifest
    bed.Edit(u8"name", gui::Variant(StringView(u8"Renamed")));
    bed.Edit(u8"defaultSceneId", gui::Variant(StringView{}));
    bed.Edit(editor::app::kProjectSettingsDialogMsaaSetting, gui::Variant(i64{2}));
    bed.Bag().ResizeList(u8"uiFontIds", 0, 3);
    bed.Bag().SetValue(u8"uiFontIds.1", 0, gui::Variant(Text(bed.font).AsView()));
    REQUIRE(bed.dialog->Save());
    CHECK_FALSE(bed.dialog->IsOpen());
    const engine::project::ProjectSettings& saved = bed.project->Settings();
    CHECK(saved.name == String(u8"Renamed"));
    CHECK(saved.defaultSceneId.IsNil());
    CHECK(saved.renderMsaaSamples == 2u);
    REQUIRE(saved.uiFontIds.Size() == 1u);
    CHECK(saved.uiFontIds[0] == bed.font);

    // Persisted: a project opened again reads it.
    UniquePtr<editor::EditorProject> again = editor::EditorProject::Open(DefaultAllocator(), u8"project_settings_project");
    REQUIRE(again);
    CHECK(again->Settings().name == String(u8"Renamed"));
}

namespace dialogprobe
{
    class MeterSettings final : public ISerializable
    {
        RTTI_OBJECT(MeterSettings, ISerializable)
    public:
        u32 volume = 3;
        String greeting = String(u8"hello");
        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "volume", volume);
            foundation::core::Serialize(ar, "greeting", greeting);
        }
    };

    REFLECT_MEMBERS(MeterSettings, "rtti::dialogprobe")
    {
        builder.DataVersion(1);
        builder.Property<&MeterSettings::volume>("volume").PropAttribute(engine::project::kSettingLabelAttribute,
                                                                         String(u8"Volume"));
        builder.Property<&MeterSettings::greeting>("greeting")
            .PropAttribute(engine::project::kSettingLabelAttribute, String(u8"Greeting"));
    }
}

namespace
{
    constexpr char8_t kMeter[] = u8"meter";
    void LoadMeter(foundation::runtime::PluginContext& context) { (void)context.Settings<dialogprobe::MeterSettings>(); }
    const foundation::runtime::PluginPart* MeterEngine()
    {
        static constexpr foundation::runtime::PluginPart part{kMeter, foundation::runtime::PluginPartKind::Engine,
                                                              &LoadMeter, nullptr};
        return &part;
    }
}

TEST_CASE("project settings: a group per plugin entry, its state, and a loaded plugin's settings, which Save writes into its entry")
{
    Bed bed;
    bed.dialog->Close();
    engine::project::ProjectSettings& settings = bed.project->Settings();
    foundation::runtime::EnabledPlugin meter;
    meter.id = String(kMeter);
    settings.plugins.PushBack(Move(meter));
    foundation::runtime::EnabledPlugin steam;
    steam.id = String(u8"steam");
    const StringView unknown = u8"<i32 name=\"appId\">480</i32>";
    steam.settings.Resize(unknown.Size());
    MemCopy(steam.settings.Data(), unknown.Data(), unknown.Size());
    settings.plugins.PushBack(Move(steam));

    foundation::runtime::PluginManifest manifest;
    manifest.id = String(kMeter);
    manifest.name = String(u8"Meter");
    manifest.version = String(u8"1.0");
    manifest.engineVersion = String(engine::project::kEngineVersionString);
    manifest.parts.PushBack(foundation::runtime::PluginManifestPart{String(u8"Engine")});
    manifest.platforms.PushBack(foundation::runtime::PluginPlatform{String(GetHostPlatformName())});
    MemoryStream xml;
    REQUIRE(foundation::runtime::WritePluginManifest(manifest, xml).IsOk());
    Array<byte> manifestText;
    for (byte b : xml.Bytes())
        manifestText.PushBack(b);
    manifestText.PushBack(byte{0});
    const foundation::runtime::PluginPartEntryFn parts[] = {&MeterEngine};
    const foundation::runtime::LinkedPlugin linked[] = {{reinterpret_cast<const char8_t*>(manifestText.Data()), parts, 1}};
    foundation::runtime::SubsystemManager subsystems(DefaultAllocator());
    foundation::runtime::PluginHost host(DefaultAllocator(), subsystems, foundation::runtime::HostRole::Editor,
                                         engine::project::kEngineVersionString);
    REQUIRE(host.AddLinked(Span<const foundation::runtime::LinkedPlugin>(linked, 1)).IsOk());
    (void)host.Load(Span<const foundation::runtime::EnabledPlugin>(settings.plugins.Data(), settings.plugins.Size()));
    bed.context.SetPlugins(&host);
    REQUIRE(bed.dialog->Open(*bed.root));

    gui::PropertyBag& bag = bed.Bag();
    const gui::PropertyInfo* group = bag.FindInfo(u8"plugins.meter");
    REQUIRE(group != nullptr);
    CHECK(group->Kind == gui::PropertyKind::Group);
    CHECK(group->DisplayName == String(u8"Meter"));
    CHECK(group->Category == String(editor::app::kProjectSettingsDialogPluginsCategory));
    CHECK(bag.GetValue(u8"plugins.meter.state", 0).AsString() == StringView(u8"loaded"));
    (void)bed.root->UpdateLayout();
    CHECK(bed.dialog->Grid()->FindRow(u8"plugins.meter.state") >= 0); // the grid reaches it, open
    REQUIRE(bag.FindInfo(u8"plugins.meter.volume") != nullptr);
    CHECK(bag.FindInfo(u8"plugins.meter.volume")->DisplayName == String(u8"Volume"));
    CHECK(bag.GetValue(u8"plugins.meter.volume", 0).AsInt() == 3);
    CHECK(bag.GetValue(u8"plugins.steam.state", 0).AsString().StartsWith(u8"missing: plugin 'steam' is not in this build"));
    CHECK(bag.FindInfo(u8"plugins.steam.appId") == nullptr); // its settings are not read here

    bed.Edit(u8"plugins.meter.volume", gui::Variant(i64{8}));
    bed.Edit(u8"plugins.meter.greeting", gui::Variant(StringView(u8"hi")));
    REQUIRE(bed.dialog->Save());
    auto* live = Cast<dialogprobe::MeterSettings>(host.Settings(kMeter));
    REQUIRE(live != nullptr);
    CHECK(live->volume == 8u);
    dialogprobe::MeterSettings saved;
    const foundation::runtime::EnabledPlugin* entry = settings.FindPlugin(kMeter);
    REQUIRE(entry != nullptr);
    REQUIRE(foundation::runtime::ReadPluginSettings(Span<const u8>(entry->settings.Data(), entry->settings.Size()), saved,
                                                    DefaultAllocator())
                .IsOk());
    CHECK(saved.volume == 8u);
    CHECK(saved.greeting == String(u8"hi"));
    const Array<u8>& kept = settings.FindPlugin(u8"steam")->settings;
    CHECK(StringView(reinterpret_cast<const utf8char*>(kept.Data()), kept.Size()) == unknown);

    bed.context.SetPlugins(nullptr);
    host.Unload();
}

namespace
{
    // A package folder in the project's Plugins/ with a manifest of `id`.
    void WriteDialogPackage(StringView projectDir, StringView folder, StringView id)
    {
        foundation::runtime::PluginManifest manifest;
        manifest.id = String(id);
        manifest.name = String(u8"Lantern");
        manifest.version = String(u8"0.3.0");
        manifest.engineVersion = String(engine::project::kEngineVersionString);
        manifest.parts.PushBack(foundation::runtime::PluginManifestPart{String(u8"Engine")});
        manifest.platforms.PushBack(foundation::runtime::PluginPlatform{String(u8"Linux64")});
        const String dir = PathJoin(PathJoin(projectDir, u8"Plugins").AsView(), folder);
        REQUIRE(CreateDirectories(dir.AsView()));
        MemoryStream xml;
        REQUIRE(foundation::runtime::WritePluginManifest(manifest, xml).IsOk());
        REQUIRE(WriteFile(PathJoin(dir.AsView(), u8"Plugin.xml").AsView(), xml.Bytes()).IsOk());
    }
}

// A package extracted into the project's Plugins folder shows unchecked under Plugins, and one that
// cannot be enabled says why with no check box; checking it and saving adds its entry, and
// unchecking that entry and saving removes it, leaving the folder, so it is offered again.
TEST_CASE("project settings: a package in Plugins/ is offered unchecked; Save adds what is checked and removes what is not")
{
    Bed bed;
    bed.dialog->Close();
    WriteDialogPackage(u8"project_settings_project", u8"Lantern", u8"lantern");
    WriteDialogPackage(u8"project_settings_project", u8"LanternCopy", u8"lantern");
    REQUIRE(bed.dialog->Open(*bed.root));

    gui::PropertyBag& bag = bed.Bag();
    REQUIRE(bag.FindInfo(u8"available.0") != nullptr);
    CHECK(bag.FindInfo(u8"available.0")->DisplayName == String(u8"Lantern 0.3.0 (Plugins/Lantern)"));
    CHECK(bag.FindInfo(u8"available.0")->Category == String(editor::app::kProjectSettingsDialogPluginsCategory));
    REQUIRE(bag.FindInfo(u8"available.0.enabled") != nullptr);
    CHECK_FALSE(bag.GetValue(u8"available.0.enabled", 0).AsBool());
    CHECK(bag.GetValue(u8"available.0.state", 0).AsString() == StringView(u8"available: Engine part on Linux64"));
    CHECK(bag.FindInfo(u8"available.1.enabled") == nullptr); // the copy is not offered
    CHECK(bag.GetValue(u8"available.1.state", 0).AsString().StartsWith(u8"not offered: plugin 'lantern' is also in"));

    // The grid shows them, open: a row the grid cannot reach through its parent never shows.
    (void)bed.root->UpdateLayout();
    CHECK(bed.dialog->Grid()->FindRow(u8"available.0.enabled") >= 0);
    CHECK(bed.dialog->Grid()->FindRow(u8"available.1.state") >= 0);

    bed.Edit(u8"available.0.enabled", gui::Variant(true));
    REQUIRE(bed.dialog->Save());
    const foundation::runtime::EnabledPlugin* entry = bed.project->Settings().FindPlugin(u8"lantern");
    REQUIRE(entry != nullptr);
    CHECK(entry->path == String(u8"Plugins/Lantern"));
    CHECK(entry->platforms.IsEmpty());

    REQUIRE(bed.dialog->Open(*bed.root));
    gui::PropertyBag& again = bed.Bag();
    REQUIRE(again.FindInfo(u8"plugins.lantern.enabled") != nullptr);
    CHECK(bed.dialog->Grid()->FindRow(u8"plugins.lantern.enabled") >= 0);
    CHECK(again.GetValue(u8"plugins.lantern.enabled", 0).AsBool());
    bed.Edit(u8"plugins.lantern.enabled", gui::Variant(false));
    REQUIRE(bed.dialog->Save());
    CHECK(bed.project->Settings().FindPlugin(u8"lantern") == nullptr);
    CHECK(FileExists(u8"project_settings_project/Plugins/Lantern/Plugin.xml"));

    REQUIRE(bed.dialog->Open(*bed.root));
    REQUIRE(bed.Bag().FindInfo(u8"available.0.enabled") != nullptr); // offered again
}
