// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// EditorProject tests: the create and open round-trip of the fixed project layout (manifest,
// subdirectories, and the source (XML) and cooked (binary) content databases).

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.xml.serialization;
import engine.project;
import editor.core;

using namespace foundation::core;
using namespace editor;

namespace
{
    class TestMaterial final : public ISerializable
    {
        RTTI_OBJECT(TestMaterial, ISerializable)
    public:
        i32 shininess = 0;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "shininess", shininess);
        }
    };

    void RemoveProjectTree(StringView root)
    {
        FileDelete(PathJoin(root, u8"Project.xml"));
        FileDelete(PathJoin(root, u8"Content/materials/steel.xasset"));
        FileDelete(PathJoin(root, u8"Cooked/materials/steel.rasset"));
        RemoveDirectory(PathJoin(root, u8"Content/materials"));
        RemoveDirectory(PathJoin(root, u8"Cooked/materials"));
        RemoveDirectory(PathJoin(root, u8"Content"));
        RemoveDirectory(PathJoin(root, u8"Sources"));
        RemoveDirectory(PathJoin(root, u8"Cooked"));
        RemoveDirectory(PathJoin(root, u8"Editor"));
        RemoveDirectory(PathJoin(root, u8".cache"));
        RemoveDirectory(root);
    }
}

RTTI_DEFINE_OBJECT(TestMaterial, "rtti::editor::editor::test")

TEST_CASE("editor-project: create scaffolds the layout and open round-trips the manifest")
{
    const StringView dir = u8"scratch_editor_test_project";
    RemoveProjectTree(dir);

    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"Test Project").IsOk());
    CHECK(FileExists(PathJoin(dir, u8"Project.xml")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Content")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Sources")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Cooked")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Editor")));
    CHECK(DirectoryExists(PathJoin(dir, u8".cache")));

    // Creating again fails: the manifest already exists.
    CHECK(EditorProject::Create(DefaultAllocator(), dir, u8"Test Project").Code() == ErrorCode::AlreadyExists);

    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    CHECK(project->Name() == u8"Test Project");
    // (The manifest's data version is in the versioned-payload envelope, not a field.)
    CHECK(project->Settings().defaultScene.IsEmpty());
    CHECK(project->SourceDb().RootGroup() != nullptr);
    CHECK(project->CookedDb().RootGroup() != nullptr);
    CHECK(project->SourcesRoot() == PathJoin(dir, u8"Sources"));
    CHECK(project->EditorStateRoot() == PathJoin(dir, u8"Editor"));
    // The save's name, which play in editor keeps under Editor/ and the player in the user data
    // directory: the same name in both, so a save copied between them is found.
    CHECK(engine::project::SaveFileName(project->Settings().name.AsView(), DefaultAllocator()) ==
          u8"Test Project.save.xml");
    CHECK(engine::project::SaveFileName(u8"", DefaultAllocator()) == u8"project.save.xml");

    RemoveProjectTree(dir);
}

TEST_CASE("editor-project: open fails without a manifest")
{
    const StringView dir = u8"scratch_editor_test_project_missing";
    RemoveProjectTree(dir);
    CHECK(!static_cast<bool>(EditorProject::Open(DefaultAllocator(), dir)));
    RemoveProjectTree(dir);
}

TEST_CASE("editor-project: an unreadable manifest logs an error (missing one stays silent)")
{
    struct CaptureSink final : ILogSink
    {
        usize errors = 0;
        String lastMessage;
        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            if (level == LogLevel::Error && category == u8"Project")
            {
                ++errors;
                lastMessage = String(message);
            }
        }
    };
    CaptureSink sink;
    GlobalLogger().AddSink(&sink);

    // Absent manifest: the scaffold path, with no error noise.
    const StringView missingDir = u8"scratch_editor_test_project_silent";
    RemoveProjectTree(missingDir);
    CHECK(!static_cast<bool>(EditorProject::Open(DefaultAllocator(), missingDir)));
    CHECK(sink.errors == 0);

    // A present but unparseable manifest (for example a pre-versioning format) fails loudly:
    // the app shell only surfaces this in the status bar, so the console line is the signal.
    const StringView dir = u8"scratch_editor_test_project_corrupt";
    RemoveProjectTree(dir);
    REQUIRE(CreateDirectory(dir));
    {
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        const StringView garbage = u8"<root><string name=\"name\">P</string></root>";
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml",
                           Span<const byte>(reinterpret_cast<const byte*>(garbage.Data()),
                                            garbage.Size()))
                    .IsOk());
    }
    CHECK(!static_cast<bool>(EditorProject::Open(DefaultAllocator(), dir)));
    CHECK(sink.errors == 1);
    const auto contains = [](StringView haystack, StringView needle)
    {
        if (needle.Size() > haystack.Size())
        {
            return false;
        }
        for (usize i = 0; i + needle.Size() <= haystack.Size(); ++i)
        {
            if (haystack.SubStr(i, needle.Size()) == needle)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(contains(sink.lastMessage.AsView(), u8"Project.xml"));

    GlobalLogger().RemoveSink(&sink);
    RemoveProjectTree(dir);
    RemoveProjectTree(missingDir);
}

TEST_CASE("editor-project: settings changes persist through SaveSettings")
{
    const StringView dir = u8"scratch_editor_test_project_save";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    Guid savedId;

    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        REQUIRE(Guid::TryParse(u8"6ba7b810-9dad-11d1-80b4-00c04fd430c8",
                               project->Settings().defaultSceneId));
        REQUIRE(Guid::TryParse(u8"6ba7b811-9dad-11d1-80b4-00c04fd430c8",
                               project->Settings().guiLoadingDocumentId));
        REQUIRE(Guid::TryParse(u8"6ba7b812-9dad-11d1-80b4-00c04fd430c8",
                               project->Settings().defaultInputMapId));
        project->Settings().defaultScene = String(u8"scenes/main");
        savedId = project->Settings().defaultSceneId;
        Guid fontId;
        REQUIRE(Guid::TryParse(u8"6ba7b813-9dad-11d1-80b4-00c04fd430c8", fontId));
        project->Settings().defaultUiFontId = fontId; // the Default UI font settings row
        CHECK(project->SaveSettings().IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().defaultScene == u8"scenes/main");
        CHECK(project->Settings().defaultSceneId == savedId); // guid is authoritative
        Guid loadingId;
        REQUIRE(Guid::TryParse(u8"6ba7b811-9dad-11d1-80b4-00c04fd430c8", loadingId));
        CHECK(project->Settings().guiLoadingDocumentId == loadingId); // an appended field round-trips
        Guid mapId;
        REQUIRE(Guid::TryParse(u8"6ba7b812-9dad-11d1-80b4-00c04fd430c8", mapId));
        // Open's per-field settings move must not drop defaultInputMapId (silent data loss).
        CHECK(project->Settings().defaultInputMapId == mapId);
        Guid fontId;
        REQUIRE(Guid::TryParse(u8"6ba7b813-9dad-11d1-80b4-00c04fd430c8", fontId));
        CHECK(project->Settings().defaultUiFontId == fontId); // Default UI font round-trips
    }

    RemoveProjectTree(dir);
}

TEST_CASE("editor-project: source db is XML, cooked db is binary, both round-trip")
{
    GlobalTypeRegistry().Register(TestMaterial::StaticType());
    RegisterSerializable<TestMaterial>();

    const StringView dir = u8"scratch_editor_test_project_dbs";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());

    Guid sourceId;
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));

        // Author a source instance and a cooked instance.
        auto* srcGroup = project->SourceDb().RootGroup()->CreateGroup(u8"materials");
        REQUIRE(srcGroup != nullptr);
        auto* src = srcGroup->CreateInstance(u8"steel", TestMaterial::StaticType());
        REQUIRE(src != nullptr);
        sourceId = src->Id();
        TestMaterial mat;
        mat.shininess = 7;
        REQUIRE(src->WriteObject(mat).IsOk());

        auto* cookedGroup = project->CookedDb().RootGroup()->CreateGroup(u8"materials");
        auto* cooked = cookedGroup->CreateInstance(u8"steel", TestMaterial::StaticType());
        REQUIRE(cooked != nullptr);
        REQUIRE(cooked->WriteObject(mat).IsOk());
    }

    // Envelope extensions match the source/cooked split: source readable XML, cooked binary.
    CHECK(FileExists(PathJoin(dir, u8"Content/materials/steel.xasset")));
    CHECK(FileExists(PathJoin(dir, u8"Cooked/materials/steel.rasset")));

    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        RefPtr<ISerializable> obj = project->SourceDb().ReadObject(sourceId);
        REQUIRE(obj.Get() != nullptr);
        TestMaterial* mat = Cast<TestMaterial>(obj.Get());
        REQUIRE(mat != nullptr);
        CHECK(mat->shininess == 7);
    }

    RemoveProjectTree(dir);
}

TEST_CASE("project: manifest round-trips the startup-script asset guid under a versioned payload")
{
    const StringView dir = u8"scratch_project_v2_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);

    const Guid scriptId = Guid{0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().startupScriptId.IsNil());
        project->Settings().startupScriptId = scriptId;
        REQUIRE(project->SaveSettings().IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().startupScriptId == scriptId);
    }

    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}

TEST_CASE("project: this engine's release downloads come from its own version's release")
{
    // The repository CMake stamps (ENGINE_RELEASE_REPOSITORY), at v<the engine's version>.
    CHECK(engine::project::kEngineReleaseRepository.ContainsIgnoreCase(u8"/")); // owner/name
    const String url = engine::project::EngineReleaseDownloadUrl(u8"samples.json");
    const String expected = Format(u8"https://github.com/{}/releases/download/v{}/samples.json", engine::project::kEngineReleaseRepository,
                                   engine::project::kEngineVersionString);
    CHECK(url == expected);
    CHECK(url.AsView().StartsWith(u8"https://github.com/"));
}

TEST_CASE("project: manifests carry the engine version stamp; a stale-version manifest is refused")
{
    const StringView dir = u8"scratch_project_engine_ver_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);

    // Every save stamps the current engine version (the launcher's routing signal).
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().engineVersion == engine::project::kEngineVersionString);
        // Comparing settings against the constant cannot catch the ENGINE_VERSION_* defines
        // silently not reaching the target; the fallback value can. A build stamped 0.0.0-dev
        // means project(VERSION) did not reach it.
        CHECK(engine::project::kEngineVersionString != StringView(u8"0.0.0-dev"));
    }

    // A manifest stamped with another ProjectSettings data version (here 1, which had no
    // engineVersion key) is refused: Open fails instead of guessing a layout. The project
    // has to be re-saved by the build that wrote it (or recreated).
    {
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        engine::project::ProjectSettings stale;
        stale.name = String(u8"P");
        stale.defaultScene = String(u8"Scenes/S");
        MemoryStream buffer;
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(buffer, SerializeMode::Write);
        const SerializedDataVersion chain[] = {
            {engine::project::ProjectSettings::StaticType().id, 1u}};
        ctx->serializer->Key("dataVersions");
        u32 count = 1;
        ctx->serializer->BeginArray(count);
        u64 typeId = chain[0].typeId;
        u32 version = chain[0].version;
        ctx->serializer->Key("type");
        ctx->serializer->Scalar(&typeId, ScalarKind::UInt64);
        ctx->serializer->Key("version");
        ctx->serializer->Scalar(&version, ScalarKind::UInt32);
        ctx->serializer->EndArray();
        ctx->serializer->PushVersionScope(chain, 1);
        stale.Serialize(*ctx->serializer);
        ctx->serializer->PopVersionScope();
        REQUIRE(ctx->serializer->IsOk());
        ctx->Flush(buffer);
        REQUIRE(root.AsWritable()->Save(u8"Project.xml", buffer.Bytes()).IsOk());

        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        CHECK_FALSE(static_cast<bool>(project));
    }
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
}

// A version 9 manifest still opens: the theme and loading document of the removed UI are read and
// dropped, and the fields after them land where they were.
TEST_CASE("project: a version 9 manifest opens without the old UI's two settings")
{
    const StringView dir = u8"scratch_project_v9_manifest";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    const String text = Format(
        u8"<root>\n"
        u8"<array name=\"dataVersions\" count=\"1\"><u64 name=\"type\">{}</u64><u32 name=\"version\">9</u32></array>\n"
        u8"<string name=\"name\">Old</string>\n"
        u8"<string name=\"engineVersion\">0.1.0</string>\n"
        u8"<string name=\"defaultSceneId\">00000000-0000-0000-0000-000000000000</string>\n"
        u8"<string name=\"defaultScene\"/>\n"
        u8"<string name=\"startupScript\"/>\n"
        u8"<string name=\"nativeModule\"/>\n"
        u8"<string name=\"defaultInputMapId\">00000000-0000-0000-0000-000000000000</string>\n"
        u8"<string name=\"defaultBusLayoutId\">00000000-0000-0000-0000-000000000000</string>\n"
        u8"<string name=\"defaultUiThemeId\">6ba7b811-9dad-11d1-80b4-00c04fd430c8</string>\n"
        u8"<string name=\"startupScriptId\">00000000-0000-0000-0000-000000000000</string>\n"
        u8"<string name=\"defaultUiFontId\">6ba7b813-9dad-11d1-80b4-00c04fd430c8</string>\n"
        u8"<string name=\"loadingDocumentId\">6ba7b812-9dad-11d1-80b4-00c04fd430c8</string>\n"
        u8"<u32 name=\"renderMsaaSamples\">4</u32>\n"
        u8"</root>\n",
        engine::project::ProjectSettings::StaticType().id);
    {
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()->Save(u8"Project.xml", Span<const byte>(reinterpret_cast<const byte*>(text.CStr()), text.Size())).IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().name == u8"Old");
        Guid fontId;
        REQUIRE(Guid::TryParse(u8"6ba7b813-9dad-11d1-80b4-00c04fd430c8", fontId));
        CHECK(project->Settings().defaultUiFontId == fontId); // read past the dropped theme
        CHECK(project->Settings().renderMsaaSamples == 4u);   // and past the dropped document
        CHECK(project->Settings().guiLoadingDocumentId.IsNil());
        REQUIRE(project->SaveSettings().IsOk()); // saved at the current version, without them
    }
    {
        const String manifest = PathJoin(dir, u8"Project.xml");
        const Result<Array<byte>> saved = ReadFile(manifest.AsView());
        REQUIRE(saved.HasValue());
        const StringView savedText(reinterpret_cast<const utf8char*>(saved.Value().Data()), saved.Value().Size());
        CHECK_FALSE(savedText.ContainsIgnoreCase(u8"\"defaultUiThemeId\""));
        CHECK_FALSE(savedText.ContainsIgnoreCase(u8"\"loadingDocumentId\"")); // guiLoadingDocumentId stays
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().renderMsaaSamples == 4u);
    }
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
}

// The settings that the Project Settings dialog and project_settings_set edit come from the
// type's reflection, not a list: every asset setting is a Guid naming the asset type it takes,
// every setting is labelled, and the path mirrors follow their guids.
TEST_CASE("editor-project: the settings describe themselves, and the path mirrors follow the guids")
{
    const TypeInfo& type = engine::project::ProjectSettings::StaticType();
    CHECK(type.dataVersion == 10u); // version 10 drops the old UI's theme and loading document
    CHECK(type.minReadDataVersion == 9u); // a version 9 manifest still reads
    u32 assets = 0;
    u32 lists = 0;
    for (const PropertyInfo& property : Properties(type))
    {
        CAPTURE(property.name);
        REQUIRE(engine::project::SettingAttribute(property, engine::project::kSettingLabelAttribute) != nullptr);
        if (const String* assetType =
                engine::project::SettingAttribute(property, engine::project::kSettingAssetTypeAttribute))
        {
            // One asset, or a list of them (the other UI fonts, the GUI's sheets).
            CHECK((engine::project::IsAssetSetting(property) || engine::project::IsAssetListSetting(property)));
            assets += engine::project::IsAssetSetting(property) ? 1u : 0u;
            lists += engine::project::IsAssetListSetting(property) ? 1u : 0u;
            CHECK_FALSE(assetType->IsEmpty());
            CHECK(engine::project::SettingAttribute(property, engine::project::kSettingEmptyTextAttribute) != nullptr);
        }
    }
    CHECK(assets == 6u); // scene, script, input map, bus layout, UI font, GUI loading screen
    CHECK(lists == 2u); // uiFontIds, guiStyleSheetIds
    const PropertyInfo* scene = FindProperty(type, "defaultSceneId");
    REQUIRE(scene != nullptr);
    CHECK(*engine::project::SettingAttribute(*scene, engine::project::kSettingAssetTypeAttribute) ==
          u8"SceneDocument");

    engine::project::ProjectSettings settings;
    Random rng(5);
    settings.defaultSceneId = Guid::Generate(rng);
    settings.RefreshPathMirrors([](const Guid&) { return String(u8"Scenes/Arena"); });
    CHECK(settings.defaultScene == u8"Scenes/Arena");
    CHECK(settings.startupScript.IsEmpty()); // no script: no mirror
}

// The other UI fonts are a list appended to the manifest. A manifest without it (no uiFontIds
// key, the same data version) still opens with none, one saved with it round-trips the list, the
// dist manifest's copy carries it, and the walk over the asset settings (export roots,
// asset_uses, project_health) visits each entry.
TEST_CASE("project: the other UI fonts are an appended list every asset walk reaches")
{
    const StringView dir = u8"scratch_project_ui_fonts_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());

    // Saved before the list: strip its key from the manifest the create wrote.
    {
        {
            UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
            REQUIRE(static_cast<bool>(project));
            project->Settings().defaultUiFontId = Guid{1, 2};
            REQUIRE(project->SaveSettings().IsOk());
        }
        Result<Array<byte>> bytes = ReadFile(PathJoin(dir, u8"Project.xml").AsView());
        REQUIRE(bytes.HasValue());
        String older(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
        const usize start = older.AsView().Find(u8"<array name=\"uiFontIds\"");
        REQUIRE(start != StringView::kNotFound);
        const usize close = older.AsView().Find(u8"/>", start);
        REQUIRE(close != StringView::kNotFound);
        CHECK(older.AsView().SubStr(start, close - start).Find(u8'<', 1) == StringView::kNotFound); // empty: <array .../>
        older.Remove(start, close + 2 - start);
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml",
                           Span<const byte>(reinterpret_cast<const byte*>(older.Data()), older.Size()))
                    .IsOk());
    }
    const Guid title{0x51, 0x52};
    const Guid caption{0x53, 0x54};
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project)); // an older manifest still opens
        CHECK(project->Settings().defaultUiFontId == Guid{1, 2});
        CHECK(project->Settings().uiFontIds.IsEmpty());
        project->Settings().uiFontIds.PushBack(title);
        project->Settings().uiFontIds.PushBack(caption);
        REQUIRE(project->SaveSettings().IsOk());
    }
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    REQUIRE(project->Settings().uiFontIds.Size() == 2u);
    CHECK(project->Settings().uiFontIds[0] == title);
    CHECK(project->Settings().uiFontIds[1] == caption);

    engine::project::ProjectSettings dist;
    REQUIRE(engine::project::CopyProjectSettings(project->Settings(), dist).IsOk());
    CHECK(dist.uiFontIds.Size() == 2u);

    Array<Guid> visited;
    engine::project::ForEachSettingAsset(project->Settings(),
                                         [&visited](const PropertyInfo&, const Guid& id) { visited.PushBack(id); });
    CHECK(visited.Size() == 3u); // the default font, then the list's two
    CHECK(visited[1] == title);
    CHECK(visited[2] == caption);

    project.Reset();
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}

// The engine GUI's sheets are an appended, ordered list: a manifest without it opens with none,
// one saved with it round-trips the order, the dist copy carries it, and every asset walk visits
// each sheet.
TEST_CASE("project: the GUI's sheets are an appended list, in order, every asset walk reaches")
{
    const StringView dir = u8"scratch_project_gui_sheets_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    {
        // Saved before the list: no key for it.
        Result<Array<byte>> bytes = ReadFile(PathJoin(dir, u8"Project.xml").AsView());
        REQUIRE(bytes.HasValue());
        String older(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
        const usize start = older.AsView().Find(u8"<array name=\"guiStyleSheetIds\"");
        REQUIRE(start != StringView::kNotFound);
        const usize close = older.AsView().Find(u8"/>", start);
        REQUIRE(close != StringView::kNotFound);
        older.Remove(start, close + 2 - start);
        // And the GUI loading screen, appended after it.
        const usize splash = older.AsView().Find(u8"guiLoadingDocumentId");
        REQUIRE(splash != StringView::kNotFound);
        const usize open = older.AsView().FindLast(u8'<', splash);
        const usize end = older.AsView().Find(u8'>', splash);
        REQUIRE(open != StringView::kNotFound);
        REQUIRE(end != StringView::kNotFound);
        usize stop = end + 1;
        if (older.AsView()[end - 1] != u8'/') // <guid ...>...</guid>: through its closing tag
            stop = older.AsView().Find(u8'>', older.AsView().Find(u8"</", end)) + 1;
        older.Remove(open, stop - open);
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml", Span<const byte>(reinterpret_cast<const byte*>(older.Data()), older.Size()))
                    .IsOk());
    }
    const Guid shared{0x61, 0x62};
    const Guid game{0x63, 0x64};
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project)); // an older manifest still opens
        CHECK(project->Settings().guiStyleSheetIds.IsEmpty());
        CHECK(project->Settings().guiLoadingDocumentId.IsNil());
        project->Settings().guiLoadingDocumentId = Guid{0x65, 0x66};
        project->Settings().guiStyleSheetIds.PushBack(shared);
        project->Settings().guiStyleSheetIds.PushBack(game);
        REQUIRE(project->SaveSettings().IsOk());
    }
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    REQUIRE(project->Settings().guiStyleSheetIds.Size() == 2u);
    CHECK(project->Settings().guiStyleSheetIds[0] == shared); // order kept: the game's sheet applies over the shared one
    CHECK(project->Settings().guiStyleSheetIds[1] == game);

    engine::project::ProjectSettings dist;
    REQUIRE(engine::project::CopyProjectSettings(project->Settings(), dist).IsOk());
    CHECK(dist.guiStyleSheetIds.Size() == 2u);
    CHECK(project->Settings().guiLoadingDocumentId == Guid{0x65, 0x66});
    CHECK(dist.guiLoadingDocumentId == Guid{0x65, 0x66});

    usize visited = 0;
    engine::project::ForEachSettingAsset(project->Settings(),
                                         [&](const PropertyInfo&, const Guid& id) { visited += (id == shared || id == game) ? 1u : 0u; });
    CHECK(visited == 2u);

    project.Reset();
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}

// The display settings (render resolution and fit, the player's window) are appended, so a
// manifest without them opens with their defaults; once set, they round-trip and the dist copy
// carries them.
TEST_CASE("project: the display settings are appended, with defaults an older manifest reads")
{
    const StringView dir = u8"scratch_project_display_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());

    // Saved before them: strip every display element from the manifest the create wrote.
    {
        Result<Array<byte>> bytes = ReadFile(PathJoin(dir, u8"Project.xml").AsView());
        REQUIRE(bytes.HasValue());
        String text(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
        const StringView keys[] = {u8"renderWidth", u8"renderHeight", u8"renderFit", u8"windowWidth",
                                   u8"windowHeight", u8"windowMode", u8"windowResizable", u8"orientation"};
        for (StringView key : keys)
        {
            const usize name = text.AsView().Find(Format(u8"name=\"{}\"", key).AsView());
            REQUIRE(name != StringView::kNotFound);
            const usize start = text.AsView().FindLast(u8'<', name);
            const usize close = text.AsView().Find(u8"</", name);
            const usize end = text.AsView().Find(u8'>', close);
            text.Remove(start, end + 1 - start);
        }
        CHECK_FALSE(text.AsView().Contains(u8"renderWidth"));
        foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml", Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()))
                    .IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project)); // an older manifest still opens
        const engine::project::ProjectSettings& settings = project->Settings();
        CHECK(settings.renderWidth == 0u);
        CHECK(settings.renderHeight == 0u);
        CHECK_FALSE(settings.HasRenderResolution()); // draws at its output's size
        CHECK(settings.renderFit == FitMode::Letterbox);
        CHECK(settings.windowWidth == 1280u);
        CHECK(settings.windowHeight == 720u);
        CHECK(settings.windowMode == engine::project::WindowMode::Windowed);
        CHECK(settings.windowResizable);
        CHECK(settings.orientation == ScreenOrientation::Any); // takes the window's shape
        project->Settings().renderWidth = 640;
        project->Settings().renderHeight = 360;
        project->Settings().renderFit = FitMode::IntegerScale;
        project->Settings().windowMode = engine::project::WindowMode::Borderless;
        project->Settings().windowResizable = false;
        project->Settings().orientation = ScreenOrientation::Landscape;
        REQUIRE(project->SaveSettings().IsOk());
    }
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    engine::project::ProjectSettings dist;
    REQUIRE(engine::project::CopyProjectSettings(project->Settings(), dist).IsOk());
    CHECK(dist.HasRenderResolution());
    CHECK(dist.renderWidth == 640u);
    CHECK(dist.renderHeight == 360u);
    CHECK(dist.renderFit == FitMode::IntegerScale);
    CHECK(dist.windowMode == engine::project::WindowMode::Borderless);
    CHECK_FALSE(dist.windowResizable);
    CHECK(dist.orientation == ScreenOrientation::Landscape);

    // Reflected for the dialog and the tools: the enums name their choices.
    const PropertyInfo* fit = FindProperty(engine::project::ProjectSettings::StaticType(), "renderFit");
    REQUIRE(fit != nullptr);
    CHECK(EnumeratorCount(*fit->type) == 4u);
    const PropertyInfo* mode = FindProperty(engine::project::ProjectSettings::StaticType(), "windowMode");
    REQUIRE(mode != nullptr);
    CHECK(EnumeratorCount(*mode->type) == 3u);
    const PropertyInfo* orientation = FindProperty(engine::project::ProjectSettings::StaticType(), "orientation");
    REQUIRE(orientation != nullptr);
    CHECK(EnumeratorCount(*orientation->type) == 3u);

    project.Reset();
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}

// The player reads its manifest before its window exists, so it can open the window the project
// asks for. It reads a dist's player.xml first, else a dev tree's Project.xml.
TEST_CASE("project: the player's manifest is the dist's, else the project's")
{
    const StringView dir = u8"scratch_player_manifest_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)FileDelete(PathJoin(dir, u8"player.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(CreateDirectory(dir));
    foundation::vfs::NativeFileSystem root(dir, foundation::core::DefaultAllocator());
    {
        engine::project::ProjectSettings none;
        CHECK(engine::project::LoadPlayerManifest(root, none).Code() == ErrorCode::NotFound);
    }
    engine::project::ProjectSettings project;
    project.name = String(u8"Dev");
    project.windowWidth = 800;
    REQUIRE(engine::project::SaveProjectSettings(*root.AsWritable(), project).IsOk());
    {
        engine::project::ProjectSettings read;
        REQUIRE(engine::project::LoadPlayerManifest(root, read).IsOk());
        CHECK(read.name == StringView(u8"Dev"));
        CHECK(read.windowWidth == 800u);
    }
    engine::project::ProjectSettings dist;
    dist.name = String(u8"Shipped");
    dist.windowMode = engine::project::WindowMode::Fullscreen;
    REQUIRE(engine::project::SaveProjectSettings(*root.AsWritable(), dist, engine::project::kDistManifestFile).IsOk());
    {
        engine::project::ProjectSettings read;
        REQUIRE(engine::project::LoadPlayerManifest(root, read).IsOk());
        CHECK(read.name == StringView(u8"Shipped")); // the dist's wins
        CHECK(read.windowMode == engine::project::WindowMode::Fullscreen);
    }
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)FileDelete(PathJoin(dir, u8"player.xml"));
    (void)RemoveDirectory(dir);
}
