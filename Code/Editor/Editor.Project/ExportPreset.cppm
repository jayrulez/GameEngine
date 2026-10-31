// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :export_preset partition.
//
// Export presets are named, per-platform descriptions of how to produce a shippable dist: which
// export template (player and runtime sidecars) to use, plus game-specific extra files and output
// naming. They are project-local and can be committed, since they reference a template by id or
// platform, never by an absolute path. They are saved to <project>/export_presets.xml (versioned
// XML in the ProjectSettings shape). The Tools.Export CLI and the editor's Export menu both drive
// the export from these, so a preset produces the same dist whichever of them triggers it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.project:export_preset;

import foundation.core;
import foundation.vfs;
import foundation.xml.serialization;
import foundation.settings;
import engine.project; // WindowMode (a preset's own window)

using namespace foundation::core;

export namespace editor
{
    namespace vfs = foundation::vfs;

    inline constexpr StringView kExportPresetsFile = u8"export_presets.xml";

    // One export target, a plain value type (copyable and movable) so a project can hold an
    // Array<ExportPreset>; only the root set below is a versioned-payload object.
    //
    // It references an ExportTemplate (see :export_template) rather than an absolute path, so a
    // preset is portable and can be committed: `templateId` picks a template exactly, or when blank
    // the installed template for `platform` is used (the host's implicit template counts).
    // `additionalFiles` are game-specific extras (icon, config, data) staged into the dist on top of
    // the template's own runtime sidecars.
    struct ExportPreset
    {
        String name;         // "Windows Desktop"
        String platform;     // "Win64" or "Linux64" (the Bin/<Config>/<Platform> tag)
        String config;       // "Debug", "Release" or "RelWithDebInfo"; "" means Release (the default)
        String templateId;   // which template; "" resolves by (platform, config)
        String playerName;   // output executable name; "" uses the template's player basename
        String outputSubdir; // output directory relative to the export root; "" uses the sanitized `name`
        Array<String> additionalFiles; // game-specific extra files (beyond the template's sidecars)
        bool stageSymbols =
            false; // stage the template's symbols[] into the dist (by default they are left out)
        bool pruneToReachable = false; // ship only the closure of the entry points (by default
                                       // everything is packed, for teams that do not manage
                                       // reachability)
        // This platform draws at its own resolution rather than the project's (a handheld's native
        // panel, say): the dist's player.xml carries these instead.
        bool overridesRender = false;
        u32 renderWidth = 0;
        u32 renderHeight = 0;
        FitMode renderFit = FitMode::Letterbox;
        // This platform's window differs from the project's (fullscreen on a console-like device,
        // say): the dist's player.xml carries these instead.
        bool overridesWindow = false;
        u32 windowWidth = 1280;
        u32 windowHeight = 720;
        engine::project::WindowMode windowMode = engine::project::WindowMode::Windowed;
        bool windowResizable = true;
        // This platform is held its own way (a phone-facing Web preset says Landscape, so a game
        // that takes any shape on a desktop is presented turned on a phone held upright).
        bool overridesOrientation = false;
        ScreenOrientation orientation = ScreenOrientation::Any;
        // The dist keeps its custom shaders' HLSL (a game that wants modders to read them). Off:
        // the pack carries the shaders as bytecode and the dist ships no shader source, as the
        // engine's own shaders ship none.
        bool includeShaderSource = false;

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "platform", platform);
            foundation::core::Serialize(ar, "templateId", templateId);
            foundation::core::Serialize(ar, "playerName", playerName);
            foundation::core::Serialize(ar, "outputSubdir", outputSubdir);
            foundation::core::Serialize(ar, "additionalFiles", additionalFiles);
            // The config axis: a preset selects (platform, config) and opts symbols in or out
            // (config "" resolves to Release).
            foundation::core::Serialize(ar, "config", config);
            foundation::core::Serialize(ar, "stageSymbols", stageSymbols);
            // Closure pruning (false packs the whole cooked directory).
            foundation::core::Serialize(ar, "pruneToReachable", pruneToReachable);
            // The display overrides are appended, so a preset file written without them reads none.
            SerializeAppended(ar, "overridesRender", overridesRender);
            SerializeAppended(ar, "renderWidth", renderWidth);
            SerializeAppended(ar, "renderHeight", renderHeight);
            SerializeAppended(ar, "renderFit", renderFit);
            SerializeAppended(ar, "overridesWindow", overridesWindow);
            SerializeAppended(ar, "windowWidth", windowWidth);
            SerializeAppended(ar, "windowHeight", windowHeight);
            SerializeAppended(ar, "windowMode", windowMode);
            SerializeAppended(ar, "windowResizable", windowResizable);
            SerializeAppended(ar, "overridesOrientation", overridesOrientation);
            SerializeAppended(ar, "orientation", orientation);
            SerializeAppended(ar, "includeShaderSource", includeShaderSource);
        }
    };

    // ExportPreset's reflection: every field a preset editor sets, each with a `label` (the
    // engine.project settings convention), and the sizes with their `range`. The MCP preset tools
    // build their arguments, checks and answers from it. Call before reading TypeOf<ExportPreset>().
    void RegisterExportPresetReflection();

    // ADL hook so Serialize(ar, Array<ExportPreset>&) resolves each element to the member above.
    // It frames each element as its own object node (the generic Array<T> helper does not), so the
    // per-element keys stay separate on read. Declared before ExportPresetSet so it is visible at
    // that template's instantiation point.
    inline void Serialize(ISerializer& ar, ExportPreset& p)
    {
        ar.BeginObject();
        p.Serialize(ar);
        ar.EndObject();
    }

    // A project's set of export presets: the root, versioned payload of export_presets.xml.
    class ExportPresetSet final : public ISerializable
    {
        RTTI_OBJECT(ExportPresetSet, ISerializable)
    public:
        Array<ExportPreset> presets;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "presets", presets);
        }

        // Finds a preset by name (case-sensitive); null when absent.
        [[nodiscard]] const ExportPreset* Find(StringView presetName) const
        {
            for (const ExportPreset& p : presets)
            {
                if (p.name.AsView() == presetName)
                {
                    return &p;
                }
            }
            return nullptr;
        }
    };

    // Fills `out` with the built-in default: one preset targeting the host platform, sourcing the
    // player from the driving tool's own directory. Used when a project has no export_presets.xml.
    inline void DefaultExportPresets(ExportPresetSet& out)
    {
        ExportPreset host;
        host.platform = String(GetHostPlatformName());
        host.name = host.platform;
        host.name += u8" Desktop";
        host.outputSubdir = host.platform;
        out.presets.Clear();
        out.presets.PushBack(Move(host));
    }

    // Reads export_presets.xml from `root`. NotFound when absent (the caller falls back to defaults).
    [[nodiscard]] inline Status LoadExportPresets(vfs::IFileSystem& root, ExportPresetSet& out,
                                                  StringView fileName = kExportPresetsFile)
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
        BeginVersionedPayload(*ctx->serializer, ExportPresetSet::StaticType());
        out.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        return ctx->serializer->IsOk() ? Status{} : ctx->serializer->GetStatus();
    }

    // Writes export_presets.xml to `root`.
    [[nodiscard]] inline Status SaveExportPresets(vfs::IWritableFileSystem& writable,
                                                  ExportPresetSet& presets,
                                                  StringView fileName = kExportPresetsFile)
    {
        MemoryStream buffer;
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(buffer, SerializeMode::Write);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        BeginVersionedPayload(*ctx->serializer, ExportPresetSet::StaticType());
        presets.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        if (!ctx->serializer->IsOk())
        {
            return ctx->serializer->GetStatus();
        }
        ctx->Flush(buffer);
        return writable.Save(fileName, buffer.Bytes());
    }

    // Editor-level export preferences: a Settings section (foundation.settings store), not
    // project-local and not in export_presets.xml. It holds the templates-root override: when
    // non-empty it wins over $ENV_TEMPLATES_DIR and the built-in <user-data>/templates default (see
    // ResolveTemplatesRoot in :export_template), letting the user point the editor at a shared or
    // checked-out templates directory. Empty (the default) keeps the environment-then-default
    // behaviour.
    class EditorExportSettings final : public ISerializable
    {
        RTTI_OBJECT(EditorExportSettings, ISerializable)
    public:
        String templatesRoot; // "" means $ENV_TEMPLATES_DIR, else <user-data>/templates

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "templatesRoot", templatesRoot);
        }
    };

    namespace settings = foundation::settings;

    RTTI_DEFINE_OBJECT_VERSIONED(ExportPresetSet, "rtti::editor::editor", 3)
    RTTI_DEFINE_OBJECT_VERSIONED(EditorExportSettings, "rtti::editor::editor", 1)
}
