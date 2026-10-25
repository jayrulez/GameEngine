// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The scene editor's services, registered without a page: the export's seams set (a scene's text
// source transcoded to the binary wire, its references scanned), and an import that is not a
// model passing the model-prefab listener by.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.runtime;
import foundation.runtime.client; // IApplicationHost
import foundation.shell;
import foundation.graphics;
import foundation.scene;
import foundation.scene.resource;
import engine.composition;
import editor.core;
import editor.scene;

using namespace foundation::core;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene;

namespace
{
    // Headless host stub: the listener only keeps it.
    class StubHost final : public runtime::IApplicationHost
    {
    public:
        runtime::Context& Ctx() noexcept override { return m_context; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* OpenWindow(const foundation::shell::WindowSettings&, const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        runtime::Context m_context{DefaultAllocator()};
    };
}

TEST_CASE("scene services: the export's seams set; a scene's text source staged as the binary wire, its references scanned")
{
    const StringView dir = u8"scratch_scene_services_project";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    {
        UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        editor::EditorContext context{DefaultAllocator()};
        StubHost host;
        CHECK_FALSE(static_cast<bool>(context.SceneStreamStager));
        editor::RegisterSceneEditorServices(context, host);
        REQUIRE(static_cast<bool>(context.SceneStreamStager));
        REQUIRE(static_cast<bool>(context.SceneRefScanner));

        foundation::content::Instance* level = project->SourceDb().RootGroup()->CreateInstance(u8"Level", scene::SceneDocument::StaticType());
        REQUIRE(level != nullptr);
        {
            scene::Scene authored(DefaultAllocator(), u8"authored");
            engine::AddAllSceneManagers(authored);
            (void)authored.CreateEntity(u8"Crate");
            REQUIRE(scene::SaveScene(authored, *level).IsOk());
        }
        Array<byte> text;
        {
            UniquePtr<IStream> stream = level->ReadData(engine::kSceneStream);
            REQUIRE(stream.Get() != nullptr);
            text.Resize(static_cast<usize>(stream->Size()));
            (void)stream->Read(text.Data(), text.Size());
        }
        Array<byte> wire;
        CHECK(context.SceneStreamStager(*level, wire));
        CHECK_FALSE(wire.IsEmpty());
        bool verbatim = wire.Size() == text.Size();
        for (usize i = 0; verbatim && i < wire.Size(); ++i)
            verbatim = wire[i] == text[i];
        CHECK_FALSE(verbatim); // transcoded, not the source as it is

        Array<Guid> resources;
        Array<Guid> prefabs;
        CHECK(context.SceneRefScanner(*level, project->SourceDb(), resources, prefabs));
        CHECK(prefabs.IsEmpty());

        // Not a model's import: the prefab listener lets it by, saying nothing.
        context.NotifyImported(*level, nullptr);
        CHECK(project->SourceDb().RootGroup()->GetInstance(u8"Level") == level);
    }
    (void)RemoveDirectoryRecursive(dir);
}
