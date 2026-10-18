// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Animation-graph creator path through a REAL project: create instance -> WriteObject(seeded)
// -> ReadObject (the page-open read). Regression coverage for the New-Asset flow (the original
// crash was a missing ctor init in the PAGE, but this pins the data path).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import pipeline.core;
import editor.core;
import editor.scene;

using namespace foundation::core;
using namespace pipeline;
using namespace editor;
namespace anim = foundation::animation;

TEST_CASE("animation graph: creator path round-trips through a real project")
{
    pipeline::RegisterAnimationAssets();

    const StringView dir = u8"graph_crash_repro_project";
    FileDelete(PathJoin(dir, u8"Project.xml"));
    FileDelete(PathJoin(dir, u8"Content/Animations/AnimationGraph.xasset"));
    RemoveDirectory(PathJoin(dir, u8"Content/Animations"));
    RemoveDirectory(PathJoin(dir, u8"Content"));
    RemoveDirectory(PathJoin(dir, u8"Sources"));
    RemoveDirectory(PathJoin(dir, u8"Cooked"));
    RemoveDirectory(PathJoin(dir, u8"Editor"));
    RemoveDirectory(PathJoin(dir, u8".cache"));
    RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    EditorContext ctx{DefaultAllocator()};
    ctx.SetProject(project.Get());

    foundation::content::Group* root = project->SourceDb().RootGroup();
    foundation::content::Group* group = root->CreateGroup(u8"Animations");
    REQUIRE(group != nullptr);

    foundation::content::Instance* instance =
        group->CreateInstance(u8"AnimationGraph", pipeline::AnimationGraphAsset::StaticType());
    REQUIRE(instance != nullptr);
    pipeline::AnimationGraphAsset asset;
    pipeline::SeedDefaultAnimationGraph(asset);
    REQUIRE(instance->WriteObject(asset).IsOk());

    // The page-open read path.
    RefPtr<ISerializable> obj = instance->ReadObject();
    REQUIRE(obj.Get() != nullptr);
    auto* readBack = Cast<pipeline::AnimationGraphAsset>(obj.Get());
    REQUIRE(readBack != nullptr);
    CHECK(readBack->source.layers.Size() == 1u);
    CHECK(readBack->layerStatePositions.Size() == 1u);
}
