// Animation-graph creator path through a REAL project: create instance -> WriteObject(seeded)
// -> ReadObject (the page-open read). Regression coverage for the New-Asset flow (the original
// crash was a missing ctor init in the PAGE, but this pins the data path).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.content;
import draconic.animation;
import draconic.animation.resource;
import draconic.animation.editor;
import draconic.editor;
import draconic.editor.core;
import draconic.editor.scene;

using namespace draconic::foundation;
using namespace draconic::editor;
namespace anim = draconic::animation;

TEST_CASE("animation graph: creator path round-trips through a real project")
{
    anim::RegisterAnimationAssets();

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
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));
    EditorContext ctx;
    ctx.SetProject(project.Get());

    draconic::content::Group* root = project->SourceDb().RootGroup();
    draconic::content::Group* group = root->CreateGroup(u8"Animations");
    REQUIRE(group != nullptr);

    draconic::content::Instance* instance =
        group->CreateInstance(u8"AnimationGraph", anim::AnimationGraphAsset::StaticType());
    REQUIRE(instance != nullptr);
    anim::AnimationGraphAsset asset;
    SeedDefaultAnimationGraph(asset);
    REQUIRE(instance->WriteObject(asset).IsOk());

    // The page-open read path.
    RefPtr<ISerializable> obj = instance->ReadObject();
    REQUIRE(obj.Get() != nullptr);
    auto* readBack = Cast<anim::AnimationGraphAsset>(obj.Get());
    REQUIRE(readBack != nullptr);
    CHECK(readBack->source.layers.Size() == 1u);
    CHECK(readBack->layerStatePositions.Size() == 1u);
}
