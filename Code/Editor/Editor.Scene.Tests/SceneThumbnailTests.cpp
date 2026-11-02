// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene tests: a scene document's thumbnail. The cook keeps no recipe for a scene, so its
// generator names the content itself: the document and the view its page saved, so a new view
// makes a new thumbnail, and the same view keeps the cached one.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.settings;
import foundation.scene;
import foundation.scene.resource; // SceneDocument
import editor.core;
import editor.scene;

using namespace foundation::core;
using namespace editor;

TEST_CASE("scene thumbnail: queued with no cook record, and named by the document and the view its page saved")
{
    const StringView dir = u8"scratch_scene_thumbnail";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    foundation::content::Instance* level =
        project->SourceDb().RootGroup()->CreateInstance(u8"Level", foundation::scene::SceneDocument::StaticType());
    REQUIRE(level != nullptr);
    EditorContext context{DefaultAllocator()};
    context.SetProject(project.Get());
    RegisterSceneViewSettingsType();
    foundation::settings::Settings store{DefaultAllocator()};
    context.SetProjectEditorSettings(&store);

    ThumbnailService service;
    RegisterSceneThumbnailGenerators(service, context);
    const Guid id = level->Id();
    foundation::content::Instance* instance = level;
    service.Configure(Format(u8"{}/thumbs", dir).AsView(),
                      Function<foundation::content::Instance*(const Guid&)>{[instance](const Guid& asked) { return asked == instance->Id() ? instance : nullptr; }},
                      &context.Tasks(), context.LightLane(), Function<u64(const Guid&)>{[](const Guid&) { return u64{0}; }}, project->SourcesRoot().AsView());

    CHECK(service.Get(id).Get() == nullptr);
    CHECK(service.QueuedSceneJobs() == 1u); // no cook record, still queued
    const SceneThumbnailJob job = service.TakeSceneJob();
    REQUIRE(job.id == id);
    REQUIRE(job.generator != nullptr);

    const u64 unviewed = job.generator->ContentHash(*level);
    CHECK(unviewed != 0u);
    CHECK(job.generator->ContentHash(*level) == unviewed); // the same content, the same name

    // Its page closes having looked from somewhere: a new view, a new thumbnail.
    SceneViewPref pref{id};
    pref.hasCamera = true;
    pref.cameraPosition = Float3{40.0f, 12.0f, -8.0f};
    pref.cameraYaw = 1.2f;
    pref.cameraPitch = -0.3f;
    REQUIRE(SaveSceneViewPref(&store, pref));
    const u64 viewed = job.generator->ContentHash(*level);
    CHECK(viewed != unviewed);
    pref.cameraYaw = 0.4f;
    REQUIRE(SaveSceneViewPref(&store, pref));
    CHECK(job.generator->ContentHash(*level) != viewed);

    service.Reset();
    context.SetProjectEditorSettings(nullptr);
    context.SetProject(nullptr);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}
