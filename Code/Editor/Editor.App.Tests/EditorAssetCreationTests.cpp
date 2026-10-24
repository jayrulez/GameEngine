// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - EditorAssetCreation: a create with no project, or one its creator cannot write,
// says so; one that writes its asset opens its page, and a scene becomes the project's default
// when it has none.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <filesystem> // remove_all (the scratch project)

import foundation.core;
import foundation.content;
import pipeline.core;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;

namespace
{
    class CreatedThing : public Object
    {
        RTTI_OBJECT(CreatedThing, Object)
    };
}
RTTI_DEFINE_OBJECT(CreatedThing, "rtti::editortest")

TEST_CASE("asset creation: refused with no project or a failed write; a written scene opens and becomes the default")
{
    EditorContext context{DefaultAllocator()};
    String said;
    context.OnNotice = [&said](NoticeKind, StringView message) { said = String(message); };
    EditorCookService cook;
    pipeline::BuilderRegistry builders{DefaultAllocator()};
    foundation::content::Instance* opened = nullptr;
    int created = 0;
    app::EditorAssetCreation::Services services;
    services.context = &context;
    services.cook = &cook;
    services.builders = &builders;
    services.openPage = [&opened](foundation::content::Instance& instance) { opened = &instance; };
    services.created = [&created](foundation::content::Instance&) { ++created; };
    app::EditorAssetCreation creation(Move(services));

    pipeline::AssetCreator failing;
    failing.label = String(u8"scene");
    creation.CreateAndOpen(failing);
    CHECK(said == String(u8"Create failed: no project is open."));

    const String dir(u8"asset-creation-project");
    std::error_code ec;
    std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"Create").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(project);
    creation.SetProject(project.Get());
    creation.CreateAndOpen(failing); // no run: nothing written
    CHECK(said == String(u8"Create failed: the scene could not be written."));
    CHECK(opened == nullptr);

    pipeline::AssetCreator scene;
    scene.label = String(u8"Scene");
    scene.setsDefaultScene = true;
    scene.run = [](const pipeline::AssetCreationContext& at) { return at.root->CreateInstance(u8"Level", CreatedThing::StaticType()); };
    project->Settings().defaultSceneId = Guid{};
    project->Settings().defaultScene = String{};
    creation.CreateAndOpen(scene);
    REQUIRE(opened != nullptr);
    CHECK(created == 1);
    CHECK(project->Settings().defaultSceneId == opened->Id());

    project = nullptr;
    std::filesystem::remove_all(reinterpret_cast<const char*>(dir.CStr()), ec);
}
