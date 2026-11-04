// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// editor.project - the game's native code, scaffolded.
//
// Generates the NativeSample shape into an existing project: the game's plugin package under
// Plugins/<Game>, its Plugin.xml and the parts asked for (each a CMakeLists, declared from the
// manifest, and a source whose Load is the one registration point and finds the service its host
// hands a part of its kind), and the project's entry for it. A package already there gains the parts
// it lacks. The templates are built as strings in this implementation unit.

module;
#include "Core/Prelude.h"

#include <initializer_list>

module editor.project;

import foundation.runtime;
import engine.project;

using namespace foundation::core;

namespace editor
{
    String NativeTargetNameFromProjectName(StringView projectName)
    {
        String name;
        for (usize i = 0; i < projectName.Size(); ++i)
        {
            const char c = static_cast<char>(projectName.Data()[i]);
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9' && !name.IsEmpty());
            if (ok)
            {
                name.PushBack(projectName.Data()[i]);
            }
        }
        return name.IsEmpty() ? String(u8"Native") : name;
    }

    namespace
    {
        // What a scaffolded part of each kind links, imports and does at load: the bare entry for
        // Engine, and for the others the service its host hands it, ready to add through.
        struct PartTemplate
        {
            StringView links;
            StringView imports;
            StringView load;
        };

        [[nodiscard]] PartTemplate TemplateOf(foundation::runtime::PluginPartKind kind)
        {
            using foundation::runtime::PluginPartKind;
            switch (kind)
            {
            case PluginPartKind::Engine:
                return PartTemplate{
                    u8"Foundation::Runtime Foundation::Core Foundation::Policy", u8"",
                    u8"        // Subsystems, scene managers, script facades and types register through the context.\n"
                    u8"        LOG_INFO(u8\"Game\", u8\"@T@ loaded\");\n"};
            case PluginPartKind::Pipeline:
                return PartTemplate{
                    u8"Pipeline::Importer Foundation::Runtime Foundation::Core Foundation::Policy",
                    u8"import pipeline.importer; // PipelinePluginServices\n",
                    u8"        // The host's pipeline registries: AddBuilder, AddImporter and AddCreator, each\n"
                    u8"        // reversed when the plugin unloads. The editor and the CLI tools load this part.\n"
                    u8"        pipeline::PipelinePluginServices* pipeline = context.Find<pipeline::PipelinePluginServices>();\n"
                    u8"        if (pipeline == nullptr)\n"
                    u8"            return;\n"
                    u8"        LOG_INFO(u8\"Game\", u8\"@T@ pipeline loaded\");\n"};
            case PluginPartKind::Tooling:
                return PartTemplate{
                    u8"Editor::Mcp Foundation::Runtime Foundation::Core Foundation::Policy",
                    u8"import editor.mcp; // McpPluginServices\n",
                    u8"        // MCP tools every MCP host serves while the plugin is loaded (AddTools). The editor,\n"
                    u8"        // the exporter and the MCP host load this part.\n"
                    u8"        editor::mcp::McpPluginServices* tools = context.Find<editor::mcp::McpPluginServices>();\n"
                    u8"        if (tools == nullptr)\n"
                    u8"            return;\n"
                    u8"        LOG_INFO(u8\"Game\", u8\"@T@ tooling loaded\");\n"};
            case PluginPartKind::Editor:
                return PartTemplate{
                    u8"Editor::Core Foundation::Runtime Foundation::Core Foundation::Policy",
                    u8"import editor.core; // EditorContext\n",
                    u8"        // The editor's context: its actions, inspector extras and MCP contributions. Only the\n"
                    u8"        // editor loads this part.\n"
                    u8"        editor::EditorContext* editor = context.Find<editor::EditorContext>();\n"
                    u8"        if (editor == nullptr)\n"
                    u8"            return;\n"
                    u8"        LOG_INFO(u8\"Game\", u8\"@T@ editor loaded\");\n"};
            }
            return PartTemplate{};
        }

        // `text` with @T@ as the package's name and @K@ as the part's kind (Format has no indexed
        // placeholders, and C++ text full of braces reads better left as it is).
        [[nodiscard]] String Instantiate(StringView text, StringView name, StringView kind)
        {
            String out;
            for (usize i = 0; i < text.Size();)
            {
                if (i + 3 <= text.Size() && text[i] == u8'@' && text[i + 2] == u8'@' && (text[i + 1] == u8'T' || text[i + 1] == u8'K'))
                {
                    out.Append(text[i + 1] == u8'T' ? name : kind);
                    i += 3;
                }
                else
                {
                    out.PushBack(text[i]);
                    ++i;
                }
            }
            return out;
        }

        [[nodiscard]] Status WritePart(StringView packageDir, StringView name, foundation::runtime::PluginPartKind kind)
        {
            const StringView kindName = foundation::runtime::PluginPartKindName(kind);
            const PartTemplate part = TemplateOf(kind);
            const String partDir = PathJoin(packageDir, Format(u8"{}.{}", name, kindName).AsView());
            if (DirectoryExists(partDir.AsView()))
            {
                return Status{ErrorCode::AlreadyExists}; // never overwrite the game's code
            }
            if (!CreateDirectories(partDir.AsView()))
            {
                return Status{ErrorCode::Internal};
            }
            const String cmake = Instantiate(
                Format(u8"# @T@.@K@ - this game's native code: its plugin package's @K@ part. util_add_plugin_part\n"
                       u8"# declares it from Plugin.xml; it builds like any engine library, a shared one for the editor\n"
                       u8"# (Project > Build Project Plugins) and a static one where a host links it.\n"
                       u8"util_add_plugin_part()\n"
                       u8"target_sources(${{PLUGIN_PART}} PRIVATE @T@@K@.cpp)\n"
                       u8"target_link_libraries(${{PLUGIN_PART}} PRIVATE {})\n",
                       part.links)
                    .AsView(),
                name, kindName);
            const String source = Instantiate(
                Format(u8"// @T@.@K@ - this game's native code, the part of its plugin package the hosts that load\n"
                       u8"// @K@ parts load.\n"
                       u8"//\n"
                       u8"// Load is the one registration point: what the context and the host's services record is\n"
                       u8"// reversed when the plugin unloads (a reload, the project closing). Unload removes what the\n"
                       u8"// recording cannot, such as a subsystem Load added.\n"
                       u8"\n"
                       u8"#include \"Core/Prelude.h\"\n"
                       u8"#include \"Core/Log/Log.h\"\n"
                       u8"#include \"Runtime/PluginEntry.h\"\n"
                       u8"\n"
                       u8"import foundation.core;\n"
                       u8"import foundation.runtime;\n"
                       u8"{}"
                       u8"\n"
                       u8"namespace\n"
                       u8"{{\n"
                       u8"    void Load(foundation::runtime::PluginContext& context)\n"
                       u8"    {{\n"
                       u8"        (void)context;\n"
                       u8"{}"
                       u8"    }}\n"
                       u8"    void Unload(foundation::runtime::PluginContext&) {{}}\n"
                       u8"}}\n"
                       u8"\n"
                       u8"PLUGIN_PART(&Load, &Unload)\n",
                       part.imports, part.load)
                    .AsView(),
                name, kindName);
            const auto asBytes = [](const String& text)
            { return Span<const byte>{reinterpret_cast<const byte*>(text.Data()), text.Size()}; };
            if (!WriteFile(PathJoin(partDir.AsView(), u8"CMakeLists.txt").AsView(), asBytes(cmake)).IsOk() ||
                !WriteFile(PathJoin(partDir.AsView(), Format(u8"{}{}.cpp", name, kindName).AsView()).AsView(), asBytes(source)).IsOk())
            {
                return Status{ErrorCode::Internal};
            }
            return Status{};
        }
    }

    Optional<GamePackage> FindGamePackage(const EditorProject& project)
    {
        const String name = NativeTargetNameFromProjectName(project.Settings().name.AsView());
        GamePackage package;
        package.id = engine::project::PluginIdFromName(name.AsView());
        package.name = name;
        package.path = Format(u8"{}/{}", engine::project::kProjectPluginsDir, name);
        const String packageDir = PathJoin(project.Directory(), package.path.AsView());
        foundation::runtime::PluginManifest manifest;
        if (!foundation::runtime::LoadPluginManifest(packageDir.AsView(), manifest, editor::EditorRootAllocator()).IsOk())
        {
            return {};
        }
        for (foundation::runtime::PluginPartKind kind : foundation::runtime::kPluginPartKinds)
        {
            if (manifest.HasPart(kind))
            {
                package.parts.PushBack(kind);
            }
        }
        return package;
    }

    Status ScaffoldGamePackage(EditorProject& project, Span<const foundation::runtime::PluginPartKind> parts)
    {
        const String name = NativeTargetNameFromProjectName(project.Settings().name.AsView());
        const String id = engine::project::PluginIdFromName(name.AsView());
        const String path = Format(u8"{}/{}", engine::project::kProjectPluginsDir, name);
        const String packageDir = PathJoin(project.Directory(), path.AsView());

        // The package as it is (its manifest), or a new one; then the parts asked for that it lacks.
        foundation::runtime::PluginManifest manifest;
        const bool exists = foundation::runtime::LoadPluginManifest(packageDir.AsView(), manifest, editor::EditorRootAllocator()).IsOk();
        if (!exists)
        {
            if (DirectoryExists(packageDir.AsView()) || project.Settings().FindPlugin(id.AsView()) != nullptr)
            {
                return Status{ErrorCode::AlreadyExists}; // a folder or an entry that is not this package: leave it
            }
            manifest.id = id;
            manifest.name = project.Settings().name;
            manifest.version = String(u8"0.1.0");
            manifest.engineVersion = Format(u8"{}.{}", engine::project::kEngineVersionMajor, engine::project::kEngineVersionMinor);
            for (StringView platform : {StringView(u8"Linux64"), StringView(u8"Win64"), StringView(u8"Web")})
            {
                manifest.platforms.PushBack(foundation::runtime::PluginPlatform{String(platform)});
            }
        }
        bool added = false;
        for (foundation::runtime::PluginPartKind kind : parts)
        {
            if (manifest.HasPart(kind))
            {
                continue;
            }
            if (const Status written = WritePart(packageDir.AsView(), name.AsView(), kind); !written.IsOk())
            {
                return written;
            }
            manifest.parts.PushBack(foundation::runtime::PluginManifestPart{String(foundation::runtime::PluginPartKindName(kind))});
            added = true;
        }
        if (!added)
        {
            return Status{ErrorCode::AlreadyExists}; // every part asked for is there
        }
        // The parts in load order, the order the build adds them in.
        Array<foundation::runtime::PluginManifestPart> ordered;
        for (foundation::runtime::PluginPartKind kind : foundation::runtime::kPluginPartKinds)
        {
            if (manifest.HasPart(kind))
            {
                ordered.PushBack(foundation::runtime::PluginManifestPart{String(foundation::runtime::PluginPartKindName(kind))});
            }
        }
        manifest.parts = Move(ordered);
        MemoryStream written;
        if (!foundation::runtime::WritePluginManifest(manifest, written).IsOk() ||
            !WriteFile(PathJoin(packageDir.AsView(), foundation::runtime::kPluginManifestFile).AsView(), written.Bytes()).IsOk())
        {
            return Status{ErrorCode::Internal};
        }
        if (project.Settings().FindPlugin(id.AsView()) == nullptr)
        {
            foundation::runtime::EnabledPlugin entry;
            entry.id = id;
            entry.path = path;
            project.Settings().plugins.PushBack(Move(entry));
        }
        return project.SaveSettings();
    }

    Array<String> ProjectPackageDirs(const EditorProject& project)
    {
        Array<String> dirs;
        for (const foundation::runtime::EnabledPlugin& entry : project.Settings().plugins)
        {
            if (entry.path.IsEmpty())
            {
                continue;
            }
            String dir = PathJoin(project.Directory(), entry.path.AsView());
            if (FileExists(PathJoin(dir.AsView(), foundation::runtime::kPluginManifestFile).AsView()))
            {
                dirs.PushBack(Move(dir));
            }
        }
        return dirs;
    }
} // namespace editor
