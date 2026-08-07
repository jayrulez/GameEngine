// EditorProject tests: create/open round-trip of the fixed project layout (docs/design/editor.md
// §3.9) - manifest, subdirectories, source (XML) + cooked (binary) content databases.

#include <doctest/doctest.h>
#include <string>

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.xml.serialization;
import draconic.engine.project;
import draconic.editor.core;

using namespace draconic::foundation;
using namespace draconic::editor;

namespace
{
    class TestMaterial final : public ISerializable
    {
        DRACONIC_OBJECT(TestMaterial, ISerializable)
    public:
        i32 shininess = 0;

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "shininess", shininess);
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

DRACONIC_DEFINE_OBJECT(TestMaterial, "draconic::editor::test")

TEST_CASE("editor-project: create scaffolds the layout and open round-trips the manifest")
{
    const StringView dir = u8"draconic_editor_test_project";
    RemoveProjectTree(dir);

    REQUIRE(EditorProject::Create(dir, u8"Test Project").IsOk());
    CHECK(FileExists(PathJoin(dir, u8"Project.xml")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Content")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Sources")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Cooked")));
    CHECK(DirectoryExists(PathJoin(dir, u8"Editor")));
    CHECK(DirectoryExists(PathJoin(dir, u8".cache")));

    // Creating again fails: the manifest already exists.
    CHECK(EditorProject::Create(dir, u8"Test Project").Code() == ErrorCode::AlreadyExists);

    UniquePtr<EditorProject> project = EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));
    CHECK(project->Name() == u8"Test Project");
    // (The manifest's data version rides the versioned-payload envelope now, not a field.)
    CHECK(project->Settings().defaultScene.IsEmpty());
    CHECK(project->SourceDb().RootGroup() != nullptr);
    CHECK(project->CookedDb().RootGroup() != nullptr);
    CHECK(project->SourcesRoot() == PathJoin(dir, u8"Sources"));
    CHECK(project->EditorStateRoot() == PathJoin(dir, u8"Editor"));

    RemoveProjectTree(dir);
}

TEST_CASE("editor-project: open fails without a manifest")
{
    const StringView dir = u8"draconic_editor_test_project_missing";
    RemoveProjectTree(dir);
    CHECK(!static_cast<bool>(EditorProject::Open(dir)));
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

    // Absent manifest: the scaffold path - no error noise.
    const StringView missingDir = u8"draconic_editor_test_project_silent";
    RemoveProjectTree(missingDir);
    CHECK(!static_cast<bool>(EditorProject::Open(missingDir)));
    CHECK(sink.errors == 0);

    // Present-but-unparseable manifest (e.g. a pre-versioning format): loud failure -
    // the app shell only surfaces this in the status bar, the console line is the signal.
    const StringView dir = u8"draconic_editor_test_project_corrupt";
    RemoveProjectTree(dir);
    REQUIRE(CreateDirectory(dir));
    {
        draconic::vfs::NativeFileSystem root(dir);
        const StringView garbage = u8"<root><string name=\"name\">P</string></root>";
        REQUIRE(root.AsWritable()
                    ->Save(u8"Project.xml",
                           Span<const byte>(reinterpret_cast<const byte*>(garbage.Data()),
                                            garbage.Size()))
                    .IsOk());
    }
    CHECK(!static_cast<bool>(EditorProject::Open(dir)));
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
    const StringView dir = u8"draconic_editor_test_project_save";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    Guid savedId;

    {
        UniquePtr<EditorProject> project = EditorProject::Open(dir);
        REQUIRE(static_cast<bool>(project));
        REQUIRE(Guid::TryParse(u8"6ba7b810-9dad-11d1-80b4-00c04fd430c8",
                               project->Settings().defaultSceneId));
        REQUIRE(Guid::TryParse(u8"6ba7b811-9dad-11d1-80b4-00c04fd430c8",
                               project->Settings().defaultUiThemeId));
        REQUIRE(Guid::TryParse(u8"6ba7b812-9dad-11d1-80b4-00c04fd430c8",
                               project->Settings().defaultInputMapId));
        project->Settings().defaultScene = String(u8"scenes/main");
        savedId = project->Settings().defaultSceneId;
        CHECK(project->SaveSettings().IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().defaultScene == u8"scenes/main");
        CHECK(project->Settings().defaultSceneId == savedId); // guid is authoritative
        Guid themeId;
        REQUIRE(Guid::TryParse(u8"6ba7b811-9dad-11d1-80b4-00c04fd430c8", themeId));
        CHECK(project->Settings().defaultUiThemeId == themeId); // v5 field round-trips
        Guid mapId;
        REQUIRE(Guid::TryParse(u8"6ba7b812-9dad-11d1-80b4-00c04fd430c8", mapId));
        // Open's per-field settings move used to DROP defaultInputMapId (silent data loss).
        CHECK(project->Settings().defaultInputMapId == mapId);
    }

    RemoveProjectTree(dir);
}

TEST_CASE("editor-project: source db is XML, cooked db is binary, both round-trip")
{
    GlobalTypeRegistry().Register(TestMaterial::StaticType());
    RegisterSerializable<TestMaterial>();

    const StringView dir = u8"draconic_editor_test_project_dbs";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());

    Guid sourceId;
    {
        UniquePtr<EditorProject> project = EditorProject::Open(dir);
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

    // Envelope extensions match the §3.9 split: source readable XML, cooked binary.
    CHECK(FileExists(PathJoin(dir, u8"Content/materials/steel.xasset")));
    CHECK(FileExists(PathJoin(dir, u8"Cooked/materials/steel.rasset")));

    {
        UniquePtr<EditorProject> project = EditorProject::Open(dir);
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
    const StringView dir = u8"draconic_project_v2_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);

    const Guid scriptId = Guid{0x1122334455667788ull, 0x99AABBCCDDEEFF00ull};
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    {
        UniquePtr<EditorProject> project = EditorProject::Open(dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().startupScriptId.IsNil());
        project->Settings().startupScriptId = scriptId;
        REQUIRE(project->SaveSettings().IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().startupScriptId == scriptId);
    }

    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}

TEST_CASE("project: manifests carry the engine version stamp; a v1 manifest migrates")
{
    const StringView dir = u8"draconic_project_engine_ver_test";
    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);

    // Every save stamps the CURRENT engine version (the launcher's routing signal).
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    {
        UniquePtr<EditorProject> project = EditorProject::Open(dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().engineVersion == draconic::project::kEngineVersionString);
    }

    // MIGRATION IN ANGER: a manifest written by the v1 ProjectSettings layout (no
    // engineVersion key) still opens - Serialize's `ar.Version() >= 2` branch skips the
    // missing field; the next save upgrades the file to v2 with the stamp.
    {
        draconic::vfs::NativeFileSystem root(dir);
        draconic::project::ProjectSettings v1;
        v1.name = String(u8"P");
        v1.defaultScene = String(u8"Scenes/S");
        MemoryStream buffer;
        SerializerFactory factory = draconic::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(buffer, SerializeMode::Write);
        const SerializedDataVersion chain[] = {
            {draconic::project::ProjectSettings::StaticType().id, 1u}};
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
        v1.Serialize(*ctx->serializer); // v1 branch: engineVersion NOT written
        ctx->serializer->PopVersionScope();
        REQUIRE(ctx->serializer->IsOk());
        ctx->Flush(buffer);
        REQUIRE(root.AsWritable()->Save(u8"Project.xml", buffer.Bytes()).IsOk());

        UniquePtr<EditorProject> project = EditorProject::Open(dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().defaultScene == u8"Scenes/S");
        CHECK(project->Settings().engineVersion.IsEmpty()); // v1 data had no stamp
        CHECK(project->Settings().defaultSceneId.IsNil());  // ...and no scene guid (v3)
        REQUIRE(project->SaveSettings().IsOk());
    }
    {
        UniquePtr<EditorProject> project = EditorProject::Open(dir);
        REQUIRE(static_cast<bool>(project));
        CHECK(project->Settings().engineVersion == draconic::project::kEngineVersionString);
    }

    (void)FileDelete(PathJoin(dir, u8"Project.xml"));
}
