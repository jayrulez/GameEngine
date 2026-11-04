// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Runtime - :plugin_manifest partition.
//
// A plugin package's manifest, `<package>/Plugin.xml`: the one place its identity, parts,
// dependencies and platforms are written. The build reads it to declare the parts
// (cmake/Plugins.cmake, util_add_plugin), and the plugin host reads it to decide what loads;
// both read the same file, so nothing about a plugin is written twice.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.runtime:plugin_manifest;

import foundation.core;
import foundation.xml.serialization;

using namespace foundation::core;

export namespace foundation::runtime
{
    inline constexpr StringView kPluginManifestFile = u8"Plugin.xml";

    // The parts a package may have, named after the hosts that load them. A part's
    // module is `<Name>.<Kind>`, `<Name>` being the package's folder.
    enum class PluginPartKind : u8
    {
        Engine,   // runtime half: every host
        Pipeline, // asset types, importers, builders, creators: the editor and the CLI tools
        Tooling,  // headless tooling (MCP tools, export hooks): the editor, Export and Mcp
        Editor,   // editor UI only
    };

    inline constexpr PluginPartKind kPluginPartKinds[] = {PluginPartKind::Engine, PluginPartKind::Pipeline,
                                                          PluginPartKind::Tooling, PluginPartKind::Editor};

    [[nodiscard]] constexpr StringView PluginPartKindName(PluginPartKind kind) noexcept
    {
        switch (kind)
        {
        case PluginPartKind::Engine: return u8"Engine";
        case PluginPartKind::Pipeline: return u8"Pipeline";
        case PluginPartKind::Tooling: return u8"Tooling";
        case PluginPartKind::Editor: return u8"Editor";
        }
        return u8"";
    }

    // The kind a manifest names (exact, case-sensitive: the name is also a folder suffix).
    [[nodiscard]] constexpr Optional<PluginPartKind> ParsePluginPartKind(StringView name) noexcept
    {
        for (PluginPartKind kind : kPluginPartKinds)
        {
            if (PluginPartKindName(kind) == name)
            {
                return kind;
            }
        }
        return {};
    }

    // A version as manifests write it: "major.minor" or "major.minor.patch", decimal.
    struct PluginVersion
    {
        u32 major = 0;
        u32 minor = 0;
        u32 patch = 0;
    };

    [[nodiscard]] inline Optional<PluginVersion> ParsePluginVersion(StringView text) noexcept
    {
        u32 fields[3] = {0, 0, 0};
        u32 count = 0;
        usize start = 0;
        for (usize i = 0; i <= text.Size(); ++i)
        {
            if (i < text.Size() && text[i] != u8'.')
            {
                if (!IsDigit(text[i]))
                {
                    return {};
                }
                continue;
            }
            if (i == start || count == 3)
            {
                return {}; // an empty field, or a fourth one
            }
            const Optional<i64> value = ParseInt(text.SubStr(start, i - start));
            if (!value || *value < 0 || *value > 0xFFFFFFFFll)
            {
                return {};
            }
            fields[count++] = static_cast<u32>(*value);
            start = i + 1;
        }
        if (count < 2)
        {
            return {};
        }
        return PluginVersion{fields[0], fields[1], fields[2]};
    }

    // Whether `version` is `minimum` or later (a dependency's version against what a plugin asks).
    [[nodiscard]] constexpr bool VersionAtLeast(PluginVersion version, PluginVersion minimum) noexcept
    {
        if (version.major != minimum.major)
        {
            return version.major > minimum.major;
        }
        if (version.minor != minimum.minor)
        {
            return version.minor > minimum.minor;
        }
        return version.patch >= minimum.patch;
    }

    // Whether an engine at `engine` satisfies a plugin built for `required`: the same major, and
    // at least the required minor and patch. Before 1.0 a minor release may break plugins, so
    // there the minor must match too.
    [[nodiscard]] constexpr bool EngineSatisfies(PluginVersion engine, PluginVersion required) noexcept
    {
        if (engine.major != required.major)
        {
            return false;
        }
        if (engine.major == 0)
        {
            return engine.minor == required.minor && engine.patch >= required.patch;
        }
        if (engine.minor != required.minor)
        {
            return engine.minor > required.minor;
        }
        return engine.patch >= required.patch;
    }

    // A plugin id: lower case ASCII letters, digits and underscores, starting with a letter. It
    // keys the project's entry and names the generated entry symbols (`PluginPart_<id>_<Kind>`),
    // so it must be a C identifier, and it is stable: renaming it orphans every project's entry.
    [[nodiscard]] constexpr bool IsValidPluginId(StringView id) noexcept
    {
        if (id.IsEmpty() || !(id[0] >= u8'a' && id[0] <= u8'z'))
        {
            return false;
        }
        for (char8_t c : id)
        {
            const bool ok = (c >= u8'a' && c <= u8'z') || IsDigit(c) || c == u8'_';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    struct PluginDependency
    {
        String id;      // the plugin depended on
        String version; // its minimum version

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "id", id);
            foundation::core::Serialize(ar, "version", version);
        }
    };

    struct PluginManifestPart
    {
        String kind; // a PluginPartKind's name

        void Serialize(ISerializer& ar) { foundation::core::Serialize(ar, "kind", kind); }
    };

    // A platform the plugin supports and the files it deploys there (relative to the package).
    // Android, iOS and console entries later add their non-C++ pieces as more fields here.
    struct PluginPlatform
    {
        String platform; // the export platform's tag: "Linux64", "Win64", "Web"
        Array<String> deploy{};

        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "platform", platform);
            foundation::core::Serialize(ar, "deploy", deploy);
        }
    };

    // ADL hooks so Serialize(ar, Array<T>&) frames each record as its own object node.
    inline void Serialize(ISerializer& ar, PluginDependency& value)
    {
        ar.BeginObject();
        value.Serialize(ar);
        ar.EndObject();
    }
    inline void Serialize(ISerializer& ar, PluginManifestPart& value)
    {
        ar.BeginObject();
        value.Serialize(ar);
        ar.EndObject();
    }
    inline void Serialize(ISerializer& ar, PluginPlatform& value)
    {
        ar.BeginObject();
        value.Serialize(ar);
        ar.EndObject();
    }

    class PluginManifest final : public ISerializable
    {
        RTTI_OBJECT(PluginManifest, ISerializable)
    public:
        String id;            // stable, lower case (IsValidPluginId)
        String name;          // for display
        String version;       // the plugin's own version
        String engineVersion; // the engine version it was written for (EngineSatisfies)
        Array<PluginDependency> dependsOn;
        Array<String> provides; // named slots, at most one enabled provider each ("online.backend")
        Array<PluginManifestPart> parts;
        Array<PluginPlatform> platforms; // a platform not listed is unsupported

        [[nodiscard]] bool HasPart(PluginPartKind kind) const noexcept
        {
            for (const PluginManifestPart& part : parts)
            {
                if (part.kind.AsView() == PluginPartKindName(kind))
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] const PluginPlatform* FindPlatform(StringView platform) const noexcept
        {
            for (const PluginPlatform& entry : platforms)
            {
                if (entry.platform.AsView() == platform)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "id", id);
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "version", version);
            foundation::core::Serialize(ar, "engineVersion", engineVersion);
            foundation::core::Serialize(ar, "dependsOn", dependsOn);
            foundation::core::Serialize(ar, "provides", provides);
            foundation::core::Serialize(ar, "parts", parts);
            foundation::core::Serialize(ar, "platforms", platforms);
        }
    };

    // Checks what the reader cannot: the id's form, the versions, the part kinds, and that an
    // engine at `engineVersion` satisfies the plugin. False with the reason in `whyNot`; a host
    // refuses the plugin with that message and never crashes on it.
    [[nodiscard]] inline bool CheckPluginManifest(const PluginManifest& manifest, StringView engineVersion,
                                                  String& whyNot)
    {
        if (!IsValidPluginId(manifest.id.AsView()))
        {
            whyNot = Format(u8"plugin id '{}' is not lower case letters, digits and underscores starting with a letter",
                            manifest.id);
            return false;
        }
        if (!ParsePluginVersion(manifest.version.AsView()))
        {
            whyNot = Format(u8"plugin '{}': version '{}' is not major.minor[.patch]", manifest.id, manifest.version);
            return false;
        }
        const Optional<PluginVersion> required = ParsePluginVersion(manifest.engineVersion.AsView());
        if (!required)
        {
            whyNot = Format(u8"plugin '{}': engineVersion '{}' is not major.minor[.patch]", manifest.id,
                            manifest.engineVersion);
            return false;
        }
        const Optional<PluginVersion> engine = ParsePluginVersion(engineVersion);
        if (!engine || !EngineSatisfies(*engine, *required))
        {
            whyNot = Format(u8"plugin '{}' needs engine {}; this engine is {}", manifest.id, manifest.engineVersion,
                            engineVersion);
            return false;
        }
        for (usize i = 0; i < manifest.parts.Size(); ++i)
        {
            const StringView kind = manifest.parts[i].kind.AsView();
            if (!ParsePluginPartKind(kind))
            {
                whyNot = Format(u8"plugin '{}': unknown part kind '{}' (Engine, Pipeline, Tooling or Editor)",
                                manifest.id, kind);
                return false;
            }
            for (usize j = 0; j < i; ++j)
            {
                if (manifest.parts[j].kind.AsView() == kind)
                {
                    whyNot = Format(u8"plugin '{}': part '{}' is listed twice", manifest.id, kind);
                    return false;
                }
            }
        }
        for (const PluginDependency& dependency : manifest.dependsOn)
        {
            if (!IsValidPluginId(dependency.id.AsView()) || dependency.id == manifest.id)
            {
                whyNot = Format(u8"plugin '{}': dependency '{}' is not another plugin's id", manifest.id,
                                dependency.id);
                return false;
            }
            if (!ParsePluginVersion(dependency.version.AsView()))
            {
                whyNot = Format(u8"plugin '{}': dependency '{}' version '{}' is not major.minor[.patch]", manifest.id,
                                dependency.id, dependency.version);
                return false;
            }
        }
        return true;
    }

    // Reads a manifest from `stream` (the XML of a Plugin.xml).
    [[nodiscard]] inline Status ReadPluginManifest(IStream& stream, PluginManifest& out)
    {
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(stream, SerializeMode::Read);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        BeginVersionedPayload(*ctx->serializer, PluginManifest::StaticType());
        out.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        return ctx->serializer->IsOk() ? Status{} : ctx->serializer->GetStatus();
    }

    // Reads `<packageDir>/Plugin.xml`. NotFound when the package has none.
    [[nodiscard]] inline Status LoadPluginManifest(StringView packageDir, PluginManifest& out, IAllocator& allocator)
    {
        const String path = PathJoin(packageDir, kPluginManifestFile, allocator);
        FileStream file(path.AsView(), FileMode::Read);
        if (!file.IsValid())
        {
            return Status{ErrorCode::NotFound};
        }
        return ReadPluginManifest(file, out);
    }

    // Writes `manifest` as XML into `out` (the scaffold and the tests write manifests this way).
    [[nodiscard]] inline Status WritePluginManifest(PluginManifest& manifest, MemoryStream& out)
    {
        SerializerFactory factory = foundation::xml::XmlSerializerFactory();
        UniquePtr<SerializerContext> ctx = factory(out, SerializeMode::Write);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        BeginVersionedPayload(*ctx->serializer, PluginManifest::StaticType());
        manifest.Serialize(*ctx->serializer);
        EndVersionedPayload(*ctx->serializer);
        if (!ctx->serializer->IsOk())
        {
            return ctx->serializer->GetStatus();
        }
        ctx->Flush(out);
        return Status{};
    }

    RTTI_DEFINE_OBJECT_VERSIONED(PluginManifest, "rtti::foundation::runtime", 1)
}
