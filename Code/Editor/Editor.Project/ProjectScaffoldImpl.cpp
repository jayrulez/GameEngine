// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// editor.project - the game's native code, scaffolded.
//
// Generates the NativeSample shape into an existing project: the game's plugin package under
// Plugins/<Game>, its Plugin.xml and its Engine part (the part's CMakeLists, declared from the
// manifest, and a source whose Load is the one registration point), and the project's entry for
// it. The templates are built as strings in this implementation unit.

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

    Status ScaffoldGamePackage(EditorProject& project)
    {
        const String name = NativeTargetNameFromProjectName(project.Settings().name.AsView());
        const String id = engine::project::PluginIdFromName(name.AsView());
        const String path = Format(u8"{}/{}", engine::project::kProjectPluginsDir, name);
        const String packageDir = PathJoin(project.Directory(), path.AsView());
        if (project.Settings().FindPlugin(id.AsView()) != nullptr || DirectoryExists(packageDir.AsView()))
        {
            return Status{ErrorCode::AlreadyExists}; // never overwrite the game's code
        }
        const String partDir = PathJoin(packageDir.AsView(), Format(u8"{}.Engine", name).AsView());
        if (!CreateDirectories(partDir.AsView()))
        {
            return Status{ErrorCode::Internal};
        }

        // Templates use an @T@ token for the package's name, because Format has no indexed
        // placeholders and C++ text full of braces reads better left as it is.
        auto instantiate = [&](StringView text)
        {
            String out;
            for (usize i = 0; i < text.Size();)
            {
                if (i + 3 <= text.Size() && text.Data()[i] == '@' && text.Data()[i + 1] == 'T' &&
                    text.Data()[i + 2] == '@')
                {
                    out.Append(name.AsView());
                    i += 3;
                }
                else
                {
                    out.PushBack(text.Data()[i]);
                    ++i;
                }
            }
            return out;
        };

        const String cmake = instantiate(
            u8"# @T@.Engine - this game's native code: its plugin package's Engine part, which every\n"
            u8"# host loads. util_add_plugin_part declares it from Plugin.xml; it builds like any engine\n"
            u8"# library, a shared one for the editor (Project > Build Project Plugins) and a static one\n"
            u8"# the exported player links.\n"
            u8"util_add_plugin_part()\n"
            u8"target_sources(${PLUGIN_PART} PRIVATE @T@Engine.cpp)\n"
            u8"target_link_libraries(${PLUGIN_PART} PRIVATE Foundation::Runtime Foundation::Core Foundation::Policy)\n");

        const String source = instantiate(
            u8"// @T@.Engine - this game's native code.\n"
            u8"//\n"
            u8"// Load is the one registration point: subsystems, scene managers, script facades and types\n"
            u8"// all register through the context here, and what the context records is reversed when the\n"
            u8"// plugin unloads (a reload, the project closing). Unload removes what the recording cannot,\n"
            u8"// such as a subsystem Load added.\n"
            u8"\n"
            u8"#include \"Core/Prelude.h\"\n"
            u8"#include \"Core/Log/Log.h\"\n"
            u8"#include \"Runtime/PluginEntry.h\"\n"
            u8"\n"
            u8"import foundation.core;\n"
            u8"import foundation.runtime;\n"
            u8"\n"
            u8"namespace\n"
            u8"{\n"
            u8"    void Load(foundation::runtime::PluginContext&)\n"
            u8"    {\n"
            u8"        LOG_INFO(u8\"Game\", u8\"@T@ loaded\");\n"
            u8"    }\n"
            u8"    void Unload(foundation::runtime::PluginContext&) {}\n"
            u8"}\n"
            u8"\n"
            u8"PLUGIN_PART(&Load, &Unload)\n");

        foundation::runtime::PluginManifest manifest;
        manifest.id = id;
        manifest.name = project.Settings().name;
        manifest.version = String(u8"0.1.0");
        manifest.engineVersion = Format(u8"{}.{}", engine::project::kEngineVersionMajor, engine::project::kEngineVersionMinor);
        manifest.parts.PushBack(foundation::runtime::PluginManifestPart{String(u8"Engine")});
        for (StringView platform : {StringView(u8"Linux64"), StringView(u8"Win64"), StringView(u8"Web")})
        {
            manifest.platforms.PushBack(foundation::runtime::PluginPlatform{String(platform)});
        }
        MemoryStream written;
        if (!foundation::runtime::WritePluginManifest(manifest, written).IsOk())
        {
            return Status{ErrorCode::Internal};
        }
        auto asBytes = [](const String& text)
        {
            return Span<const byte>{reinterpret_cast<const byte*>(text.Data()), text.Size()};
        };
        const String manifestPath = PathJoin(packageDir.AsView(), foundation::runtime::kPluginManifestFile);
        if (!WriteFile(manifestPath.AsView(), written.Bytes()).IsOk() ||
            !WriteFile(PathJoin(partDir.AsView(), u8"CMakeLists.txt").AsView(), asBytes(cmake)).IsOk() ||
            !WriteFile(PathJoin(partDir.AsView(), Format(u8"{}Engine.cpp", name).AsView()).AsView(), asBytes(source)).IsOk())
        {
            return Status{ErrorCode::Internal};
        }

        foundation::runtime::EnabledPlugin entry;
        entry.id = id;
        entry.path = path;
        project.Settings().plugins.PushBack(Move(entry));
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
