// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// editor.core - native-code scaffolding implementation (game-native-code.md N4).
//
// Generates the NativeSample reference shape into an existing project: the Native/
// directory with the canonical CMakeLists (util_add_engine_library; dev .so lands in
// Native/ itself, exactly where the manifest points) and a plugin skeleton whose
// OnLoad is the one registration entry. Kept in an impl unit; the templates are
// string-built here so the wizard and the committed fixture stay one shape.

module;
#include "Core/Prelude.h"

module editor.project;

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

    Status ScaffoldNativeModule(EditorProject& project)
    {
        if (!project.Settings().nativeModule.IsEmpty())
        {
            return Status{ErrorCode::AlreadyExists}; // declared already - nothing to scaffold
        }
        const String nativeDir = PathJoin(project.Directory(), u8"Native");
        if (DirectoryExists(nativeDir.AsView()))
        {
            return Status{ErrorCode::AlreadyExists}; // never overwrite user code
        }
        if (!CreateDirectory(nativeDir.AsView()))
        {
            return Status{ErrorCode::Internal};
        }

        const String target = NativeTargetNameFromProjectName(project.Settings().name.AsView());

        // Templates with an @T@ token (the house Format has no indexed placeholders,
        // and brace-heavy C++ text reads better untouched).
        auto instantiate = [&](StringView text)
        {
            String out;
            for (usize i = 0; i < text.Size();)
            {
                if (i + 3 <= text.Size() && text.Data()[i] == '@' && text.Data()[i + 1] == 'T' &&
                    text.Data()[i + 2] == '@')
                {
                    out.Append(target.AsView());
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
            u8"# @T@ - this project's native game module (game-native-code.md).\n"
            u8"#\n"
            u8"# Builds like any engine module: the dev shared library in shared-engine\n"
            u8"# builds (the editor/player load it via the manifest's nativeModule) and a\n"
            u8"# static library in ship builds (the exporter links it into the game player).\n"
            u8"# One source, both worlds.\n"
            u8"util_add_engine_library(@T@ ALIAS Game::@T@)\n"
            u8"target_sources(@T@ PRIVATE @T@Plugin.cpp)\n"
            u8"target_link_libraries(@T@ PRIVATE Foundation::Runtime Foundation::Core "
            u8"Foundation::Policy)\n"
            u8"# The dev module lands HERE - exactly where the manifest's nativeModule points -\n"
            u8"# so the editor/player load what was just built. Ship archives unaffected.\n"
            u8"# BOTH properties are required: RUNTIME_OUTPUT_DIRECTORY governs the .dll on\n"
            u8"# Windows, LIBRARY_OUTPUT_DIRECTORY the .so elsewhere. The Windows import\n"
            u8"# library (ARCHIVE_*) is deliberately left in the build tree - nothing loads it.\n"
            u8"set_target_properties(@T@ PROPERTIES\n"
            u8"    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}\n"
            u8"    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})\n");

        const String plugin = instantiate(
            u8"// @T@Plugin - this project's native game plugin (game-native-code.md).\n"
            u8"//\n"
            u8"// OnLoad(SubsystemManager&) is the ONE registration entry: components, subsystems, and\n"
            u8"// script facades all register explicitly here (never via static initializers),\n"
            u8"// and the engine reverses recorded registrations on unload/reload. The exported\n"
            u8"// CreatePlugin below is the whole contract: PluginHost dlopens it in dev builds,\n"
            u8"// and the ship player's stub references it statically.\n"
            u8"\n"
            u8"#include \"Core/Prelude.h\"\n"
            u8"#include \"Core/Log/Log.h\"\n"
            u8"\n"
            u8"import foundation.core;\n"
            u8"import foundation.runtime;\n"
            u8"\n"
            u8"using namespace foundation::core;\n"
            u8"using namespace foundation::runtime;\n"
            u8"\n"
            u8"#if defined(_WIN32)\n"
            u8"#define PLUGIN_EXPORT __declspec(dllexport)\n"
            u8"#else\n"
            u8"#define PLUGIN_EXPORT __attribute__((visibility(\"default\")))\n"
            u8"#endif\n"
            u8"\n"
            u8"namespace\n"
            u8"{\n"
            u8"    class @T@Plugin final : public IRuntimePlugin\n"
            u8"    {\n"
            u8"    public:\n"
            u8"        [[nodiscard]] StringView Name() const noexcept override\n"
            u8"        {\n"
            u8"            return u8\"@T@\";\n"
            u8"        }\n"
            u8"\n"
            u8"        void OnLoad(SubsystemManager&) override\n"
            u8"        {\n"
            u8"            LOG_INFO(u8\"Game\", u8\"@T@ native plugin loaded\");\n"
            u8"        }\n"
            u8"        void OnUnload(SubsystemManager&) override {}\n"
            u8"    };\n"
            u8"}\n"
            u8"\n"
            u8"extern \"C\" PLUGIN_EXPORT IRuntimePlugin* CreatePlugin()\n"
            u8"{\n"
            u8"    static @T@Plugin plugin;\n"
            u8"    return &plugin;\n"
            u8"}\n");

        const String cmakePath = PathJoin(nativeDir.AsView(), u8"CMakeLists.txt");
        const String pluginPath =
            PathJoin(nativeDir.AsView(), Format(u8"{}Plugin.cpp", target).AsView());
        auto asBytes = [](const String& text)
        {
            return Span<const byte>{reinterpret_cast<const byte*>(text.Data()), text.Size()};
        };
        if (!WriteFile(cmakePath.AsView(), asBytes(cmake)).IsOk() ||
            !WriteFile(pluginPath.AsView(), asBytes(plugin)).IsOk())
        {
            return Status{ErrorCode::Internal};
        }

        // Platform-shaped, because the manifest path is what the editor/player actually
        // dlopens: MSVC names a DLL "<Target>.dll" with no "lib" prefix, ELF names it
        // "lib<Target>.so". NativeTargetFromModulePath parses both back, so a manifest
        // authored on one platform still yields the right SHIP target name on the other -
        // only the dev-load path is platform-specific.
#if PLATFORM_WINDOWS
        project.Settings().nativeModule = Format(u8"Native/{}.dll", target);
#else
        project.Settings().nativeModule = Format(u8"Native/lib{}.so", target);
#endif
        return project.SaveSettings();
    }
} // namespace editor
