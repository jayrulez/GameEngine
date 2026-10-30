// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - EditorNativeModule: a project's native code is staged as a numbered copy under the
// project's cache (the declared path, else this platform's spelling of the same target), and a
// reload with no module declared says so and touches nothing.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <filesystem> // remove_all (the scratch project)

import foundation.core;
import foundation.runtime;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;

namespace
{
    struct ScratchProject
    {
        UniquePtr<EditorProject> project;
        String dir;
        explicit ScratchProject(StringView leaf) : dir(leaf)
        {
            std::error_code ec;
            std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"Native").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(project);
        }
        ~ScratchProject()
        {
            project = nullptr;
            std::error_code ec;
            std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
        }
        void WriteModule(StringView relative, StringView bytes)
        {
            const String full = PathJoin(dir.AsView(), relative);
            (void)CreateDirectories(PathParent(full.AsView()));
            REQUIRE(WriteFile(full.AsView(), Span<const byte>(reinterpret_cast<const byte*>(bytes.Data()), bytes.Size())).IsOk());
        }
    };
}

TEST_CASE("native module: staged as a numbered copy, from the declared path or this platform's spelling of it")
{
    ScratchProject bench(u8"native-module-project");
    app::EditorNativeModule native(DefaultAllocator());
#if PLATFORM_WINDOWS
    bench.WriteModule(u8"Native/Game.dll", u8"module bytes");
    bench.project->Settings().nativeModule = String(u8"Native/libGame.so"); // declared on the other platform
#else
    bench.WriteModule(u8"Native/libGame.so", u8"module bytes");
    bench.project->Settings().nativeModule = String(u8"Native/Game.dll"); // declared on the other platform
#endif
    const String first = native.StageCopy(*bench.project);
    REQUIRE_FALSE(first.IsEmpty());
    CHECK(FileExists(first.AsView()));
    const String second = native.StageCopy(*bench.project);
    CHECK(second != first); // a reload maps a fresh path
    Result<Array<byte>> staged = ReadFile(second.AsView());
    REQUIRE(staged.HasValue());
    CHECK(staged.Value().Size() == 12u);

    bench.project->Settings().nativeModule = String(u8"Native/libMissing.so");
    CHECK(native.StageCopy(*bench.project).IsEmpty());
}

TEST_CASE("native module: a reload with none declared says so and loads nothing")
{
    ScratchProject bench(u8"native-module-none");
    EditorContext context{DefaultAllocator()};
    String said;
    context.OnNotice = [&said](NoticeKind, StringView message) { said = String(message); };
    foundation::runtime::SubsystemManager runtime{DefaultAllocator()};
    app::EditorNativeModule native(DefaultAllocator());
    native.Reload(bench.project.Get(), context, runtime, nullptr);
    CHECK_FALSE(native.IsLoaded());
    CHECK(said == String(u8"No native module declared (Project Settings > Native module)."));
}
