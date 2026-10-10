// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Project - the `engine.project` module.
//
// The RUNTIME-side project definition: the manifest payload (ProjectSettings), the fixed
// directory layout, and manifest load/save over a VFS root. Split out of the editor so
// shipping binaries (Engine.Player, dist builds) carry ZERO editor code - the editor's
// EditorProject builds on top of this (mounts, DBs, per-user state stay editor-side).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module engine.project;

import foundation.core;
import foundation.vfs;
import foundation.xml.serialization;

using namespace foundation::core;
using namespace foundation;
namespace vfs = foundation::vfs;

export namespace engine::project
{
    // The ENGINE version (distinct from per-type data versions): stamped into every saved
    // project manifest so tooling - the editor today, the launcher/project manager later - knows
    // which engine authored a project and can route/migrate accordingly. DERIVED from the CMake
    // project(VERSION) via the ENGINE_VERSION_* defines (the single source of truth), so bumping
    // the version is a one-line change in the root CMakeLists and never drifts from a copy here.
#ifndef ENGINE_VERSION_STRING
#define ENGINE_VERSION_MAJOR 0
#define ENGINE_VERSION_MINOR 0
#define ENGINE_VERSION_PATCH 0
#define ENGINE_VERSION_STRING u8"0.0.0-dev" // built without the policy defines (should not happen)
#endif
    inline constexpr u32 kEngineVersionMajor = ENGINE_VERSION_MAJOR;
    inline constexpr u32 kEngineVersionMinor = ENGINE_VERSION_MINOR;
    inline constexpr u32 kEngineVersionPatch = ENGINE_VERSION_PATCH;
    inline constexpr StringView kEngineVersionString = ENGINE_VERSION_STRING;

    inline constexpr StringView kProjectManifestFile = u8"Project.xml";
    inline constexpr StringView kProjectContentDir = u8"Content";
    inline constexpr StringView kProjectSourcesDir = u8"Sources";
    inline constexpr StringView kProjectCookedDir = u8"Cooked";
    inline constexpr StringView kProjectEditorDir = u8"Editor";
    inline constexpr StringView kProjectCacheDir = u8".cache";
    inline constexpr StringView kSourceAssetExtension = u8".xasset"; // readable/diffable envelopes
    inline constexpr StringView kCookedAssetExtension = u8".rasset"; // binary envelopes

    // Distribution layout (what the export CLI stages; the player detects dist by the pak).
    inline constexpr StringView kDistContentPak = u8"Content.pak";
    inline constexpr StringView kDistManifestFile = u8"player.xml";

    // A game's save file (Documentation/Specs/save-data.md): `<project>.save.xml`, which the player
    // keeps in the user data directory and play in editor in the project's Editor/ folder.
    [[nodiscard]] inline String SaveFileName(StringView projectName, IAllocator& allocator)
    {
        String name(projectName.IsEmpty() ? StringView(u8"project") : projectName, allocator);
        name.Append(u8".save.xml");
        return name;
    }

    // ProjectSettings reflects the settings the Project Settings dialog edits (and the MCP
    // project_settings_set sets), each with a `label`; an asset setting is a Guid property whose
    // `assetType` attribute names the asset type it takes (the picker's filter) and whose
    // `emptyText` says what unset means. A setting's `category` names the group it shows in (the
    // dialog's tab; none is "General"). The engine stamp and the path mirrors are not settings.
    inline constexpr const char* kSettingLabelAttribute = "label";
    inline constexpr const char* kSettingCategoryAttribute = "category";
    inline constexpr StringView kSettingDefaultCategory = u8"General";
    inline constexpr const char* kSettingAssetTypeAttribute = "assetType";
    inline constexpr const char* kSettingEmptyTextAttribute = "emptyText";

    /// How the player's window takes the screen.
    enum class WindowMode : u8
    {
        Windowed,   // a normal window of the configured size
        Fullscreen, // exclusive fullscreen at the configured size
        Borderless, // a borderless window covering the whole display, at the display's own size
    };

    // The shared, committed part of a project (Project.xml payload) - also the dist manifest
    // (player.xml), which is the same shape minus editor-only concerns.
    class ProjectSettings final : public ISerializable
    {
        RTTI_OBJECT(ProjectSettings, ISerializable)
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
        u32 renderMsaaSamples = 1; // scene-pass MSAA sample count (1 = off, 2, 4); the player and
                                   // play-in-editor apply it, capability-clamped at runtime (v9).
                                   // Default 1 keeps existing projects byte-identical.
        Array<Guid> uiFontIds; // cooked fonts the game UI loads BESIDE the default one, each its
                               // own family a label picks by font-family (a title face beside
                               // the body text). Appended: a manifest saved before it reads none.

        // The display (appended: a manifest saved before them reads these defaults).
        // The resolution the game DRAWS at, fitted into whatever shows it (the player's window,
        // the editor's Game tab) by renderFit. Nought on either axis draws at the output's own
        // size, which is what a game that adapts to any size wants.
        u32 renderWidth = 0;
        u32 renderHeight = 0;
        FitMode renderFit = FitMode::Letterbox; // how a fixed resolution fits another shape
        // The player's window: its size (ignored by Borderless, which takes the display's), how
        // it takes the screen, and whether the user may resize it. An export preset overrides
        // these per platform.
        u32 windowWidth = 1280;
        u32 windowHeight = 720;
        WindowMode windowMode = WindowMode::Windowed;
        bool windowResizable = true;

        /// Whether the game draws at a fixed resolution rather than at its output's size.
        [[nodiscard]] bool HasRenderResolution() const noexcept { return renderWidth > 0 && renderHeight > 0; }

        /// Re-derives the human-readable path mirrors (defaultScene, startupScript) from their
        /// guids after a change: `pathOf` answers an asset's source-DB path, empty when unknown.
        void RefreshPathMirrors(const Function<String(const Guid&)>& pathOf)
        {
            defaultScene = defaultSceneId.IsNil() ? String() : pathOf(defaultSceneId);
            startupScript = startupScriptId.IsNil() ? String() : pathOf(startupScriptId);
        }

        // One layout: the current data version (REFLECT_MEMBERS' DataVersion). A manifest
        // written under another version is refused by the versioned-payload reader.
        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "engineVersion", engineVersion);
            ar.Key("defaultSceneId");
            ar.GuidValue(defaultSceneId);
            foundation::core::Serialize(ar, "defaultScene", defaultScene);
            foundation::core::Serialize(ar, "startupScript", startupScript);
            foundation::core::Serialize(ar, "nativeModule", nativeModule);
            ar.Key("defaultInputMapId");
            ar.GuidValue(defaultInputMapId);
            ar.Key("defaultBusLayoutId");
            ar.GuidValue(defaultBusLayoutId);
            ar.Key("defaultUiThemeId");
            ar.GuidValue(defaultUiThemeId);
            ar.Key("startupScriptId");
            ar.GuidValue(startupScriptId);
            ar.Key("defaultUiFontId");
            ar.GuidValue(defaultUiFontId);
            ar.Key("loadingDocumentId");
            ar.GuidValue(loadingDocumentId);
            foundation::core::Serialize(ar, "renderMsaaSamples", renderMsaaSamples);
            SerializeAppended(ar, "uiFontIds", uiFontIds);
            SerializeAppended(ar, "renderWidth", renderWidth);
            SerializeAppended(ar, "renderHeight", renderHeight);
            SerializeAppended(ar, "renderFit", renderFit);
            SerializeAppended(ar, "windowWidth", windowWidth);
            SerializeAppended(ar, "windowHeight", windowHeight);
            SerializeAppended(ar, "windowMode", windowMode);
            SerializeAppended(ar, "windowResizable", windowResizable);
        }
    };

    /// A setting property's string attribute (kSettingLabelAttribute, kSettingAssetTypeAttribute,
    /// kSettingEmptyTextAttribute); null when the property has none.
    [[nodiscard]] inline const String* SettingAttribute(const PropertyInfo& property,
                                                        const char* key) noexcept
    {
        const Attribute* found =
            FindAttribute(property, StringView(reinterpret_cast<const utf8char*>(key)));
        return found != nullptr ? found->value.TryGet<String>() : nullptr;
    }

    /// The group a setting shows in: its `category`, or "General" when it names none.
    [[nodiscard]] inline StringView SettingCategory(const PropertyInfo& property) noexcept
    {
        const String* category = SettingAttribute(property, kSettingCategoryAttribute);
        return category != nullptr ? category->AsView() : kSettingDefaultCategory;
    }

    /// An asset setting: a Guid property naming one asset of its `assetType`.
    [[nodiscard]] inline bool IsAssetSetting(const PropertyInfo& property) noexcept
    {
        return property.type == &TypeOf<Guid>() &&
               SettingAttribute(property, kSettingAssetTypeAttribute) != nullptr;
    }

    /// An asset list setting: an Array<Guid> property naming assets of its `assetType`, in order.
    [[nodiscard]] inline bool IsAssetListSetting(const PropertyInfo& property) noexcept
    {
        return property.type == &TypeOf<Array<Guid>>() &&
               SettingAttribute(property, kSettingAssetTypeAttribute) != nullptr;
    }

    /// Every asset the settings name, each with the setting that names it (a list setting once
    /// per entry); unset (nil) ones are skipped. What export roots, asset_uses and project_health
    /// walk, so a new asset setting reaches all three by being reflected.
    inline void ForEachSettingAsset(const ProjectSettings& settings,
                                    const Function<void(const PropertyInfo&, const Guid&)>& visit)
    {
        const Instance instance(const_cast<ProjectSettings*>(&settings), &ProjectSettings::StaticType());
        for (const PropertyInfo& property : Properties(ProjectSettings::StaticType()))
        {
            if (IsAssetSetting(property))
            {
                const Guid& id = *static_cast<const Guid*>(property.address(instance));
                if (!id.IsNil())
                {
                    visit(property, id);
                }
            }
            else if (IsAssetListSetting(property))
            {
                for (const Guid& id : *static_cast<const Array<Guid>*>(property.address(instance)))
                {
                    if (!id.IsNil())
                    {
                        visit(property, id);
                    }
                }
            }
        }
    }

    /// Read a manifest (Project.xml / player.xml) from `root`. NotFound when absent.
    [[nodiscard]] inline Status LoadProjectSettings(vfs::IFileSystem& root, ProjectSettings& out,
                                                    StringView fileName = kProjectManifestFile)
    {
        UniquePtr<IStream> stream = root.Open(fileName, FileMode::Read);
        if (!stream)
        {
            return Status{ErrorCode::NotFound};
        }
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
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

    /// The manifest a player runs from `root`: a dist's player.xml, else a dev tree's
    /// Project.xml. NotFound with neither.
    [[nodiscard]] inline Status LoadPlayerManifest(vfs::IFileSystem& root, ProjectSettings& out)
    {
        if (LoadProjectSettings(root, out, kDistManifestFile).IsOk())
        {
            return Status{};
        }
        return LoadProjectSettings(root, out);
    }

    /// The one copy of a manifest (a ProjectSettings is serializable, so it has no copy): through
    /// its own Serialize body, so every field written there is copied. The project open and the
    /// dist manifest each kept a hand list of fields, and both drifted (loadingDocumentId and
    /// renderMsaaSamples were dropped by one, then the other).
    [[nodiscard]] inline Status CopyProjectSettings(const ProjectSettings& from, ProjectSettings& to)
    {
        MemoryStream buffer;
        {
            BinarySerializer writer(buffer, SerializeMode::Write);
            const_cast<ProjectSettings&>(from).Serialize(writer); // a write only reads the fields
            if (!writer.IsOk())
            {
                return writer.GetStatus();
            }
        }
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(buffer, SerializeMode::Read);
        to.Serialize(reader);
        return reader.IsOk() ? Status{} : reader.GetStatus();
    }

    /// Write a manifest to `root`.
    [[nodiscard]] inline Status SaveProjectSettings(vfs::IWritableFileSystem& writable,
                                                    ProjectSettings& settings,
                                                    StringView fileName = kProjectManifestFile)
    {
        settings.engineVersion = String(kEngineVersionString); // every save re-stamps
        MemoryStream buffer;
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
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

}
