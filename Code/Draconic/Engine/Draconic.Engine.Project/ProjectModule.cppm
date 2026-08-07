// Draconic::Project - the `draconic.engine.project` module.
//
// The RUNTIME-side project definition: the manifest payload (ProjectSettings), the fixed
// directory layout, and manifest load/save over a VFS root. Split out of the editor so
// shipping binaries (Draconic.Engine.Player, dist builds) carry ZERO editor code - the editor's
// EditorProject builds on top of this (mounts, DBs, per-user state stay editor-side).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.engine.project;

import draconic.foundation;
import draconic.vfs;
import draconic.xml.serialization;

using namespace draconic::foundation;

export namespace draconic::project
{
    // The ENGINE version (distinct from per-type data versions): stamped into every saved
    // project manifest so tooling - the editor today, the launcher/project manager later -
    // knows which engine authored a project and can route/migrate accordingly.
    inline constexpr u32 kEngineVersionMajor = 0;
    inline constexpr u32 kEngineVersionMinor = 1;
    inline constexpr u32 kEngineVersionPatch = 0;
    inline constexpr StringView kEngineVersionString = u8"0.1.0";

    inline constexpr StringView kProjectManifestFile = u8"Project.xml";
    inline constexpr StringView kProjectContentDir = u8"Content";
    inline constexpr StringView kProjectSourcesDir = u8"Sources";
    inline constexpr StringView kProjectCookedDir = u8"Cooked";
    inline constexpr StringView kProjectEditorDir = u8"Editor";
    inline constexpr StringView kProjectCacheDir = u8".cache";
    inline constexpr StringView kSourceAssetExtension = u8".xasset"; // readable/diffable envelopes
    inline constexpr StringView kCookedAssetExtension = u8".rasset"; // binary envelopes

    // Shipped-dist layout (what the export CLI stages; the player detects dist by the pak).
    inline constexpr StringView kDistContentPak = u8"Content.pak";
    inline constexpr StringView kDistManifestFile = u8"player.xml";

    // The shared, committed part of a project (Project.xml payload) - also the dist manifest
    // (player.xml), which is the same shape minus editor-only concerns.
    class ProjectSettings final : public ISerializable
    {
        DRACONIC_OBJECT(ProjectSettings, ISerializable)
    public:
        String name;
        String engineVersion; // engine that last saved this project (launcher/migration routing)
        Guid defaultSceneId;  // AUTHORITATIVE startup-scene reference (rename/move-proof)
        String defaultScene;  // its source-DB path - the human-readable mirror (and the
                              // fallback for v2 manifests that predate the guid)
        Guid startupScriptId; // AUTHORITATIVE game-script reference (a cooked ScriptClass asset;
                              // launch/update/exit), bound from the content DB by the player and
        // play-in-editor. nil = none. (v6; supersedes the startupScript path.)
        String
            startupScript; // its source-DB path - the human-readable mirror (and the v<6 fallback)
        String nativeModule;    // RESERVED: optional native game module (tagged for later planning)
        Guid defaultInputMapId; // the input map the player binds at startup (nil = none; v4)
        Guid defaultBusLayoutId; // the audio mixer layout applied at startup (nil = built-in; v5)
        Guid
            defaultUiThemeId; // the cooked UITheme the game UI defaults to (nil = built-in GameTheme; v5)
        Guid defaultUiFontId; // the cooked FontResource the game UI defaults to (nil = the dev
                              // TTF fallback path; v7 - the fonts-triad manifest hook)
        Guid loadingDocumentId; // the cooked UIDocument shown as the boot splash while the default
                                // scene loads (nil = the built-in default splash; v8, task #123)

        // Migration branches on ar.Version() - the type's data version is written/read by
        // the manifest helpers below (DRACONIC_DEFINE_OBJECT_VERSIONED sets the current one).
        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "name", name);
            if (ar.Version() >= 2) // v2 added the engine stamp
            {
                draconic::foundation::Serialize(ar, "engineVersion", engineVersion);
            }
            if (ar.Version() >= 3) // v3 made the default scene guid-authoritative
            {
                ar.Key("defaultSceneId");
                ar.GuidValue(defaultSceneId);
            }
            draconic::foundation::Serialize(ar, "defaultScene", defaultScene);
            draconic::foundation::Serialize(ar, "startupScript", startupScript);
            draconic::foundation::Serialize(ar, "nativeModule", nativeModule);
            if (ar.Version() >= 4) // v4 added the default input map
            {
                ar.Key("defaultInputMapId");
                ar.GuidValue(defaultInputMapId);
            }
            if (ar.Version() >= 5) // v5 added the default audio bus layout + UI theme
            {
                ar.Key("defaultBusLayoutId");
                ar.GuidValue(defaultBusLayoutId);
                ar.Key("defaultUiThemeId");
                ar.GuidValue(defaultUiThemeId);
            }
            if (ar.Version() >=
                6) // v6 made the startup script guid-authoritative (a ScriptClass asset)
            {
                ar.Key("startupScriptId");
                ar.GuidValue(startupScriptId);
            }
            if (ar.Version() >= 7) // v7 added the default UI font (fonts triad)
            {
                ar.Key("defaultUiFontId");
                ar.GuidValue(defaultUiFontId);
            }
            if (ar.Version() >= 8) // v8 added the boot-splash loading document (task #123)
            {
                ar.Key("loadingDocumentId");
                ar.GuidValue(loadingDocumentId);
            }
        }
    };

    /// Read a manifest (Project.xml / player.xml) from `root`. NotFound when absent.
    [[nodiscard]] inline Status LoadProjectSettings(vfs::IFileSystem& root, ProjectSettings& out,
                                                    StringView fileName = kProjectManifestFile)
    {
        UniquePtr<IStream> stream = root.Open(fileName, FileMode::Read);
        if (!stream)
        {
            return Status{ErrorCode::NotFound};
        }
        SerializerFactory factory = draconic::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(*stream, SerializeMode::Read);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        BeginVersionedPayload(*ctx->serializer, ProjectSettings::StaticType());
        out.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        return ctx->serializer->IsOk() ? Status{} : ctx->serializer->GetStatus();
    }

    /// Write a manifest to `root`.
    [[nodiscard]] inline Status SaveProjectSettings(vfs::IWritableFileSystem& writable,
                                                    ProjectSettings& settings,
                                                    StringView fileName = kProjectManifestFile)
    {
        settings.engineVersion = String(kEngineVersionString); // every save re-stamps
        MemoryStream buffer;
        SerializerFactory factory = draconic::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(buffer, SerializeMode::Write);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        BeginVersionedPayload(*ctx->serializer, ProjectSettings::StaticType());
        settings.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        if (!ctx->serializer->IsOk())
        {
            return ctx->serializer->GetStatus();
        }
        ctx->Flush(buffer);
        return writable.Save(fileName, buffer.Bytes());
    }

    DRACONIC_DEFINE_OBJECT_VERSIONED(ProjectSettings, "draconic::project", 8)
}
