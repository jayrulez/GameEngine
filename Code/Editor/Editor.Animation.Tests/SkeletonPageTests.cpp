// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The skeleton page over its markup (Data/Editor/SkeletonPage.xml) and a scratch project, without
// a host, so there is no preview. Covers: the bones as a tree under their parents, read from the
// asset without a cook; a search keeping each bone found and the bones above it; the chosen bone's
// facts and the skeleton's, all read-only; a rotation described in degrees about an axis.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.animation;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    RefPtr<gui::Node> LoadMarkup(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    /// A scratch project holding one skeleton: Hips (Spine, Chest under it; LegL, LegR) and a
    /// second root, Prop.
    struct Bed
    {
        String dir = String(u8"scratch_skeleton_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        UniquePtr<SkeletonEditorPage> page;

        Bed()
        {
            pipeline::RegisterAnimationAssets();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            foundation::content::Instance* instance = project->SourceDb().RootGroup()->CreateInstance(u8"Rig", pipeline::SkeletonAsset::StaticType());
            REQUIRE(instance != nullptr);
            pipeline::SkeletonAsset asset;
            auto& source = asset.source;
            source.name = String(u8"rig");
            const auto bone = [&](StringView name, i32 parent, Float3 at, Quaternion turn)
            {
                source.boneNames.PushBack(String(name));
                source.parentIndices.PushBack(parent);
                source.translations.PushBack(at);
                source.rotations.PushBack(turn);
                source.scales.PushBack(Float3{1.0f, 1.0f, 1.0f});
                source.inverseBindPoses.PushBack(Float4x4::Identity());
            };
            bone(u8"Hips", -1, Float3{0.0f, 1.0f, 0.0f}, Quaternion{});
            bone(u8"Spine", 0, Float3{0.0f, 0.2f, 0.0f}, Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, kPi * 0.5f));
            bone(u8"Chest", 1, Float3{0.0f, 0.25f, 0.0f}, Quaternion{});
            bone(u8"LegL", 0, Float3{0.1f, -0.1f, 0.0f}, Quaternion{});
            bone(u8"LegR", 0, Float3{-0.1f, -0.1f, 0.0f}, Quaternion{});
            bone(u8"Prop", -1, Float3{0.5f, 1.0f, 0.0f}, Quaternion{});
            REQUIRE(instance->WriteObject(asset).IsOk());
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            page = MakeUnique<SkeletonEditorPage>(DefaultAllocator(), context, Move(services), *instance);
            REQUIRE_FALSE(page->LoadFailed());
        }
        ~Bed()
        {
            page.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        String Row(StringView path) { return String(page->BoneRows().GetValue(path, 0).AsString()); }
    };
}

TEST_CASE("skeleton page: the bones a tree under their parents, read from the asset; its facts read only")
{
    Bed bed;
    REQUIRE(bed.page->Skeleton() != nullptr);
    CHECK(bed.page->Tree() == String(u8"Hips\n  Spine\n    Chest\n  LegL\n  LegR\nProp"));
    CHECK(bed.page->Fact(u8"Bones") == String(u8"6"));
    CHECK(bed.page->Fact(u8"Roots") == String(u8"2"));
    CHECK(bed.page->Fact(u8"Deepest chain") == String(u8"2 below its root"));

    // The first bone is chosen at the start; choosing another updates the facts.
    CHECK(bed.page->ChosenBone() == 0);
    CHECK(bed.Row(u8"parent") == String(u8"None: a root"));
    CHECK(bed.Row(u8"children") == String(u8"3"));
    bed.page->ChooseBone(1);
    CHECK(bed.Row(u8"name") == String(u8"Spine"));
    CHECK(bed.Row(u8"index") == String(u8"1"));
    CHECK(bed.Row(u8"parent") == String(u8"Hips"));
    CHECK(bed.Row(u8"position") == String(u8"0, 0.2, 0"));
    CHECK(bed.Row(u8"rotation") == String(u8"90° about 0, 1, 0"));
    CHECK(bed.Row(u8"scale") == String(u8"1, 1, 1"));
    bed.page->ChooseBone(-1);
    CHECK(bed.Row(u8"name") == String(u8"None chosen"));
    CHECK(bed.Row(u8"parent").IsEmpty());

    // Every row is read-only.
    Array<gui::PropertyInfo> rows;
    bed.page->BoneRows().GetProperties(StringView{}, rows);
    REQUIRE(rows.Size() == 7u);
    for (const gui::PropertyInfo& info : rows)
        CHECK(info.ReadOnly);
}

TEST_CASE("skeleton page: a search keeps a bone found and the bones above it")
{
    Bed bed;
    bed.page->Search(u8"leg");
    CHECK(bed.page->Tree() == String(u8"Hips\n  LegL\n  LegR"));
    bed.page->Search(u8"chest");
    CHECK(bed.page->Tree() == String(u8"Hips\n  Spine\n    Chest"));
    bed.page->Search(u8"nothing like it");
    CHECK(bed.page->Tree().IsEmpty());
    bed.page->Search(u8"  ");
    CHECK(bed.page->Tree() == String(u8"Hips\n  Spine\n    Chest\n  LegL\n  LegR\nProp"));
}

TEST_CASE("skeleton page: a rotation said in degrees about an axis")
{
    CHECK(DescribeRotation(Quaternion{}) == String(u8"None"));
    CHECK(DescribeRotation(Quaternion::FromAxisAngle(Float3{1.0f, 0.0f, 0.0f}, kPi)) == String(u8"180° about 1, 0, 0"));
    // The shorter way round: -90 about +Y is 90 about -Y.
    CHECK(DescribeRotation(Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, -kPi * 0.5f)) == String(u8"90° about 0, -1, 0"));
}
