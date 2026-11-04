// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Project - the `engine.project` module.
//
// The runtime-side project definition: the manifest payload (ProjectSettings), the fixed
// directory layout, and manifest load and save over a VFS root. Kept apart from the editor so
// shipping binaries (Engine.Player, dist builds) carry no editor code; the editor's
// EditorProject builds on top of this (mounts, DBs and per-user state stay editor-side).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module engine.project;

import foundation.core;
import foundation.vfs;
import foundation.xml.serialization;
import foundation.runtime; // the plugins' entries

using namespace foundation::core;
using namespace foundation;
namespace vfs = foundation::vfs;

export namespace engine::project
{
    // The engine version (distinct from per-type data versions): stamped into every saved
    // project manifest so tooling knows which engine authored a project and can route or migrate
    // it. Derived from the CMake project(VERSION) through the ENGINE_VERSION_* defines (the single
    // source of truth), so bumping the version is a one-line change in the root CMakeLists.
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

    // The GitHub repository (owner/name) whose releases carry what this engine downloads for its
    // own version, such as the sample games: ENGINE_RELEASE_REPOSITORY in the root CMakeLists.
#ifndef ENGINE_RELEASE_REPOSITORY
#define ENGINE_RELEASE_REPOSITORY u8"jayrulez/GameEngine"
#endif
    inline constexpr StringView kEngineReleaseRepository = ENGINE_RELEASE_REPOSITORY;

    /// Where `file` attached to this engine's own release is downloaded from: release
    /// v<version> of the release repository. A build between releases names a release that may
    /// not exist yet.
    [[nodiscard]] inline String EngineReleaseDownloadUrl(StringView file)
    {
        return Format(u8"https://github.com/{}/releases/download/v{}/{}", kEngineReleaseRepository, kEngineVersionString, file);
    }

    inline constexpr StringView kProjectManifestFile = u8"Project.xml";
    inline constexpr StringView kProjectContentDir = u8"Content";
    inline constexpr StringView kProjectSourcesDir = u8"Sources";
    inline constexpr StringView kProjectCookedDir = u8"Cooked";
    inline constexpr StringView kProjectEditorDir = u8"Editor";
    inline constexpr StringView kProjectCacheDir = u8".cache";
    // A project's own plugin packages (its game code, project-local plugins): Plugins/<Name>/, each a
    // Plugin.xml and its parts, built for development as shared libraries in a shared-engine tree
    // under the cache, which lists each part's library in kPluginLibrariesFile.
    inline constexpr StringView kProjectPluginsDir = u8"Plugins";
    inline constexpr StringView kProjectPluginsBuildDir = u8".cache/plugins-dev";
    inline constexpr StringView kPluginLibrariesFile = u8"plugin-libraries.txt";
    inline constexpr StringView kSourceAssetExtension = u8".xasset"; // readable, diffable envelopes
    inline constexpr StringView kCookedAssetExtension = u8".rasset"; // binary envelopes

    // Distribution layout (what the export CLI stages; the player detects dist by the pak).
    inline constexpr StringView kDistContentPak = u8"Content.pak";
    inline constexpr StringView kDistManifestFile = u8"player.xml";

    // A game's save file: `<project>.save.xml`, which the player
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
    // A setting the project's page sums the project up with carries `summary`, the short label it
    // shows there ("Input map" for the default input map).
    inline constexpr const char* kSettingSummaryAttribute = "summary";

    /// A plugin id from a package's name ("NativeSample" gives "nativesample"): lower case, anything
    /// but letters, digits and underscores an underscore, prefixed when it does not start with a
    /// letter, so it is always a valid id.
    [[nodiscard]] inline String PluginIdFromName(StringView name)
    {
        String id;
        for (char8_t c : name)
        {
            const char8_t lower = (c >= u8'A' && c <= u8'Z') ? static_cast<char8_t>(c - u8'A' + u8'a') : c;
            const bool ok = (lower >= u8'a' && lower <= u8'z') || (lower >= u8'0' && lower <= u8'9') || lower == u8'_';
            id.PushBack(ok ? lower : u8'_');
        }
        if (id.IsEmpty() || !(id[0] >= u8'a' && id[0] <= u8'z'))
        {
            id = Format(u8"game_{}", id);
        }
        return id;
    }

    /// How the player's window takes the screen.
    enum class WindowMode : u8
    {
        Windowed,   // a normal window of the configured size
        Fullscreen, // exclusive fullscreen at the configured size
        Borderless, // a borderless window covering the whole display, at the display's own size
    };

    // The shared, committed part of a project (the Project.xml payload), and also the dist
    // manifest (player.xml), which is the same shape without editor-only concerns.
    class ProjectSettings final : public ISerializable
    {
        RTTI_OBJECT(ProjectSettings, ISerializable)
    public:
        String name;
        String engineVersion; // engine that last saved this project (launcher and migration routing)
        Guid defaultSceneId;  // authoritative startup-scene reference (survives rename and move)
        String defaultScene;  // its source-DB path: the human-readable mirror, and the
                              // fallback when the guid does not resolve
        Guid startupScriptId; // authoritative game-script reference (a cooked ScriptClass asset;
                              // launch/update/exit), bound from the content DB by the player and
        // play-in-editor. Nil: none.
        String
            startupScript; // its source-DB path: the human-readable mirror
        Guid defaultInputMapId; // the input map the player binds at startup (nil: none)
        Guid defaultBusLayoutId; // the audio mixer layout applied at startup (nil: built-in)
        Guid defaultUiFontId; // the cooked FontResource the game UI defaults to (nil: the dev
                              // TTF fallback path)
        u32 renderMsaaSamples = 1; // scene-pass MSAA sample count (1 is off, 2, 4); the player and
                                   // play-in-editor apply it, clamped to the device at runtime.
                                   // The default of 1 leaves existing projects byte-identical.
        Array<Guid> uiFontIds; // cooked fonts the game UI loads beside the default one, each its
                               // own family a label picks by font-family (a title face beside
                               // the body text). Appended: a manifest saved before it reads none.

        // The engine GUI's sheets (foundation.gui's), in order over its built-in theme, each over
        // those before it (a game's own over a shared one). Appended: a manifest saved before it
        // reads none. Its fonts are the UI's (above).
        Array<Guid> guiStyleSheetIds;
        // The cooked GuiDocument the player's boot splash shows through the engine GUI while the
        // default scene loads (nil = the engine's own loading screen); its load progress
        // the GUI's data value `bootProgress` (0 to 1). Appended.
        Guid guiLoadingDocumentId;

        // The display (appended: a manifest saved before them reads these defaults).
        // The resolution the game draws at, fitted into whatever shows it (the player's window,
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
        // The shape the game is made for: Any takes the window's; Landscape or Portrait is
        // presented turned on a window of the other shape (a landscape game on a phone held
        // upright).
        ScreenOrientation orientation = ScreenOrientation::Any;

        // The plugins the project enables, in order, each with its platforms and settings. An entry
        // whose plugin is missing is kept as read and written back unchanged. Appended: a manifest
        // saved before it reads none.
        Array<foundation::runtime::EnabledPlugin> plugins;

        /// The project's entry for plugin `id`, or null when it enables no such plugin.
        [[nodiscard]] foundation::runtime::EnabledPlugin* FindPlugin(StringView id) noexcept
        {
            for (foundation::runtime::EnabledPlugin& entry : plugins)
            {
                if (entry.id.AsView() == id)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        /// The player's window in a few words: "1280 × 720, windowed", "1280 × 720, fullscreen",
        /// or "borderless, the display's size".
        [[nodiscard]] String WindowSummary() const
        {
            if (windowMode == WindowMode::Borderless)
                return String(u8"borderless, the display's size");
            return Format(u8"{} × {}, {}", windowWidth, windowHeight, windowMode == WindowMode::Fullscreen ? u8"fullscreen" : u8"windowed");
        }

        /// Whether the game draws at a fixed resolution rather than at its output's size.
        [[nodiscard]] bool HasRenderResolution() const noexcept { return renderWidth > 0 && renderHeight > 0; }

        /// Re-derives the human-readable path mirrors (defaultScene, startupScript) from their
        /// guids after a change: `pathOf` answers an asset's source-DB path, empty when unknown.
        void RefreshPathMirrors(const Function<String(const Guid&)>& pathOf)
        {
            defaultScene = defaultSceneId.IsNil() ? String() : pathOf(defaultSceneId);
            startupScript = startupScriptId.IsNil() ? String() : pathOf(startupScriptId);
        }

        // The current data version's layout (REFLECT_MEMBERS' DataVersion, 11). Versions 9 and 10
        // still read: both name the native game module by its library path (nativeModule), which
        // becomes the entry of the game's package (NativeModulePackageEntry); version 9 also
        // carries a theme and loading document for a UI library the engine no longer has, read and
        // dropped. Older versions are refused by the versioned-payload reader; outside a payload
        // (CopyProjectSettings) the version reads 0, the current layout.
        void Serialize(ISerializer& ar) override
        {
            const bool reading = ar.Mode() == SerializeMode::Read;
            const bool v9 = reading && ar.Version() == 9;
            const bool hasNativeModule = reading && (ar.Version() == 9 || ar.Version() == 10);
            Guid dropped;
            String nativeModule;
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "engineVersion", engineVersion);
            ar.Key("defaultSceneId");
            ar.GuidValue(defaultSceneId);
            foundation::core::Serialize(ar, "defaultScene", defaultScene);
            foundation::core::Serialize(ar, "startupScript", startupScript);
            if (hasNativeModule)
            {
                foundation::core::Serialize(ar, "nativeModule", nativeModule);
            }
            ar.Key("defaultInputMapId");
            ar.GuidValue(defaultInputMapId);
            ar.Key("defaultBusLayoutId");
            ar.GuidValue(defaultBusLayoutId);
            if (v9)
            {
                ar.Key("defaultUiThemeId");
                ar.GuidValue(dropped);
            }
            ar.Key("startupScriptId");
            ar.GuidValue(startupScriptId);
            ar.Key("defaultUiFontId");
            ar.GuidValue(defaultUiFontId);
            if (v9)
            {
                ar.Key("loadingDocumentId");
                ar.GuidValue(dropped);
            }
            foundation::core::Serialize(ar, "renderMsaaSamples", renderMsaaSamples);
            SerializeAppended(ar, "uiFontIds", uiFontIds);
            SerializeAppended(ar, "renderWidth", renderWidth);
            SerializeAppended(ar, "renderHeight", renderHeight);
            SerializeAppended(ar, "renderFit", renderFit);
            SerializeAppended(ar, "windowWidth", windowWidth);
            SerializeAppended(ar, "windowHeight", windowHeight);
            SerializeAppended(ar, "windowMode", windowMode);
            SerializeAppended(ar, "windowResizable", windowResizable);
            SerializeAppended(ar, "orientation", orientation);
            SerializeAppended(ar, "guiStyleSheetIds", guiStyleSheetIds);
            if (ar.Mode() != SerializeMode::Read || ar.HasKey("guiLoadingDocumentId")) // appended
            {
                ar.Key("guiLoadingDocumentId");
                ar.GuidValue(guiLoadingDocumentId);
            }
            SerializeAppended(ar, "plugins", plugins);
            if (!nativeModule.IsEmpty())
            {
                foundation::runtime::EnabledPlugin entry = NativeModulePackageEntry(nativeModule.AsView());
                if (!entry.id.IsEmpty() && FindPlugin(entry.id.AsView()) == nullptr)
                {
                    plugins.PushBack(Move(entry));
                }
            }
        }

        /// The entry a project before version 11 means by its nativeModule (a library path such as
        /// "Native/libGame.so" or "Native/Game.dll"): the game's package, Plugins/<Game>, its id the
        /// name in lower case. The package's own files are the game's to port; until they are there,
        /// the entry is missing. Empty when the path names no library.
        [[nodiscard]] static foundation::runtime::EnabledPlugin NativeModulePackageEntry(StringView nativeModule)
        {
            foundation::runtime::EnabledPlugin entry;
            StringView name = PathFilename(nativeModule);
            const usize dot = name.FindLast(u8'.');
            if (dot != StringView::kNotFound)
            {
                const StringView extension = name.SubStr(dot + 1, name.Size() - dot - 1);
                name = name.SubStr(0, dot);
                if (extension == StringView(u8"so") && name.StartsWith(u8"lib"))
                {
                    name = name.SubStr(3, name.Size() - 3);
                }
            }
            if (name.IsEmpty())
            {
                return entry;
            }
            entry.id = PluginIdFromName(name);
            entry.path = Format(u8"{}/{}", kProjectPluginsDir, name);
            return entry;
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
    /// per entry); unset (nil) ones are skipped. Export roots, asset_uses and project_health walk
    /// this, so a new asset setting reaches all three by being reflected.
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

    /// What became of a project's plugin entry in a host.
    enum class PluginState : u8
    {
        Loaded,
        NotEnabled, // not enabled on the host's platform: absent, as the entry asks
        Refused,    // known, and refused (the reason says why)
        Missing,    // not in this host: its entry and settings are kept as written
    };

    [[nodiscard]] constexpr StringView PluginStateName(PluginState state) noexcept
    {
        switch (state)
        {
        case PluginState::Loaded: return u8"loaded";
        case PluginState::NotEnabled: return u8"notEnabled";
        case PluginState::Refused: return u8"refused";
        case PluginState::Missing: return u8"missing";
        }
        return u8"";
    }

    struct PluginStatus
    {
        PluginState state = PluginState::Missing;
        String reason;                    // why it is not loaded; empty when it is
        Array<String> unsupportedPlatforms; // platforms the entry names that its manifest lacks
    };

    /// The status of `entry` in `host`, the process's plugin host after it loaded the project's
    /// entries; a null host is one that loads no plugins, where every entry is missing.
    [[nodiscard]] inline PluginStatus PluginStatusOf(const foundation::runtime::EnabledPlugin& entry,
                                                     const foundation::runtime::PluginHost* host,
                                                     IAllocator& allocator)
    {
        PluginStatus status;
        const StringView id = entry.id.AsView();
        if (host == nullptr)
        {
            status.reason = Format(u8"plugin '{}' is not loaded: this host loads no plugins", id);
            return status;
        }
        if (const foundation::runtime::PluginManifest* manifest = host->Manifest(id); manifest != nullptr)
        {
            status.unsupportedPlatforms = foundation::runtime::UnsupportedPlatforms(entry, *manifest, allocator);
        }
        if (host->IsLoaded(id))
        {
            status.state = PluginState::Loaded;
            return status;
        }
        const foundation::runtime::PluginLoadReport& report = host->Report();
        for (const String& absent : report.notEnabled)
        {
            if (absent.AsView() == id)
            {
                status.state = PluginState::NotEnabled;
                status.reason = Format(u8"plugin '{}' is not enabled on {}", id, host->Platform());
                return status;
            }
        }
        if (const foundation::runtime::PluginRefusal* refusal = report.FindRefusal(id);
            refusal != nullptr && host->Knows(id))
        {
            status.state = PluginState::Refused;
            status.reason = refusal->reason;
            return status;
        }
        status.reason = Format(u8"plugin '{}' is not in this build", id);
        return status;
    }

    /// One built part of a project's package, as the development build lists it.
    struct BuiltPluginPart
    {
        String id;
        foundation::runtime::PluginPartKind kind = foundation::runtime::PluginPartKind::Engine;
        String library; // the shared library's path
    };

    /// The parts a project's development build made (<project>/.cache/plugins-dev/plugin-libraries.txt,
    /// one `id|Kind|library` line each); empty when the project has never been built.
    [[nodiscard]] inline Array<BuiltPluginPart> ReadBuiltPluginParts(StringView projectDir, IAllocator& allocator)
    {
        Array<BuiltPluginPart> parts(allocator);
        const String listPath = PathJoin(PathJoin(projectDir, kProjectPluginsBuildDir, allocator).AsView(), kPluginLibrariesFile, allocator);
        Result<Array<byte>> bytes = ReadFile(listPath.AsView(), allocator);
        if (!bytes.HasValue())
        {
            return parts;
        }
        const StringView text(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        usize start = 0;
        for (usize i = 0; i <= text.Size(); ++i)
        {
            if (i < text.Size() && text[i] != u8'\n')
            {
                continue;
            }
            const StringView line = Trim(text.SubStr(start, i - start));
            start = i + 1;
            const usize first = line.Find(u8'|');
            const usize second = first == StringView::kNotFound ? StringView::kNotFound : line.Find(u8'|', first + 1);
            if (second == StringView::kNotFound)
            {
                continue;
            }
            const Optional<foundation::runtime::PluginPartKind> kind =
                foundation::runtime::ParsePluginPartKind(line.SubStr(first + 1, second - first - 1));
            if (!kind)
            {
                continue;
            }
            parts.PushBack(BuiltPluginPart{String(line.SubStr(0, first), allocator), *kind,
                                           String(line.SubStr(second + 1, line.Size() - second - 1), allocator)});
        }
        return parts;
    }

    /// Adds the project's own packages (the entries with a path) to `host` as development packages:
    /// each package's manifest, and the shared libraries its development build made of the parts
    /// the host's role loads. `stage` gives the path to load a library from (the editor loads a
    /// fresh copy at each reload); none loads it where it was built. A package with no manifest is
    /// left out, so its entry stays missing, and a part never built is refused when it loads. A
    /// plugin this binary links wins over its package.
    inline void AddProjectPackages(foundation::runtime::PluginHost& host, StringView projectDir, const ProjectSettings& settings,
                                   IAllocator& allocator, const Function<String(StringView)>& stage = {})
    {
        const Array<BuiltPluginPart> built = ReadBuiltPluginParts(projectDir, allocator);
        for (const foundation::runtime::EnabledPlugin& entry : settings.plugins)
        {
            if (entry.path.IsEmpty())
            {
                continue;
            }
            UniquePtr<foundation::runtime::PluginManifest> manifest = MakeUnique<foundation::runtime::PluginManifest>(allocator);
            const String packageDir = PathJoin(projectDir, entry.path.AsView(), allocator);
            if (!foundation::runtime::LoadPluginManifest(packageDir.AsView(), *manifest, allocator).IsOk() || manifest->id != entry.id)
            {
                LOG_WARNING(u8"Plugins", u8"plugin '{}': '{}' holds no Plugin.xml of that id", entry.id, entry.path);
                continue;
            }
            Array<foundation::runtime::PluginLibrary> libraries(allocator);
            for (const BuiltPluginPart& part : built)
            {
                if (part.id != entry.id || !foundation::runtime::RoleLoads(host.Role(), part.kind))
                {
                    continue;
                }
                String path = stage ? stage(part.library.AsView()) : String(part.library.AsView(), allocator);
                if (!path.IsEmpty())
                {
                    libraries.PushBack(foundation::runtime::PluginLibrary{part.kind, Move(path)});
                }
            }
            host.AddShared(Move(manifest), Span<const foundation::runtime::PluginLibrary>(libraries.Data(), libraries.Size()));
        }
    }

    /// Loads the plugins `settings` enables into `host`, the process's plugin host for its role,
    /// after adding the plugins this binary links. What loaded is logged, and what did not, each
    /// with why; the report says the same. A plugin that does not load leaves its entry as it is.
    inline foundation::runtime::PluginLoadReport LoadProjectPlugins(foundation::runtime::PluginHost& host,
                                                                    Span<const foundation::runtime::LinkedPlugin> linked,
                                                                    const ProjectSettings& settings)
    {
        if (!host.AddLinked(linked).IsOk())
        {
            LOG_ERROR(u8"Plugins", u8"a plugin linked into this build has a manifest that does not read");
        }
        foundation::runtime::PluginLoadReport report = host.Load(
            Span<const foundation::runtime::EnabledPlugin>(settings.plugins.Data(), settings.plugins.Size()));
        for (const String& id : report.loaded)
        {
            LOG_INFO(u8"Plugins", u8"plugin '{}' loaded ({})", id, foundation::runtime::HostRoleName(host.Role()));
        }
        for (const foundation::runtime::PluginRefusal& refusal : report.refused)
        {
            LOG_WARNING(u8"Plugins", u8"{}; its entry and settings are kept", refusal.reason);
        }
        return report;
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
    /// its own Serialize body, so every field written there is copied and no hand-written list
    /// of fields can drift from it.
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
