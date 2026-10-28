// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// What can be done to a project's assets and groups, over a scratch project: a rename kept by the
// name rule (refused saying why, the default scene's path mirror following), a group's rename; a
// duplicate beside its source; a new group a folder on disk at once; a delete naming what goes, its
// pages closed first and the default scene let go with it; a group's delete with all it holds; and
// the export flag kept with the project.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <filesystem>

import foundation.core;
import foundation.content;
import editor.core;
import editor.app;

using namespace foundation::core;
using editor::app::AssetOperations;
namespace content = foundation::content;

namespace
{
    class OpsAsset : public ISerializable
    {
        RTTI_OBJECT(OpsAsset, ISerializable)
    public:
        int value = 0;
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "value", value); }
    };

    // Content (Crate, Barrel, Town (Bell)).
    struct Bed
    {
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        UniquePtr<AssetOperations> ops;
        Array<Guid> closed;
        content::Group* top = nullptr;
        content::Group* town = nullptr;
        content::Instance* crate = nullptr;
        content::Instance* barrel = nullptr;
        content::Instance* bell = nullptr;

        Bed()
        {
            std::error_code ec;
            std::filesystem::remove_all("asset_ops_project", ec);
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"asset_ops_project", u8"Ops").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), u8"asset_ops_project");
            REQUIRE(project);
            GlobalTypeRegistry().Register(OpsAsset::StaticType());
            RegisterSerializable<OpsAsset>();
            top = project->SourceDb().RootGroup();
            OpsAsset object;
            crate = top->CreateInstance(u8"Crate", OpsAsset::StaticType());
            barrel = top->CreateInstance(u8"Barrel", OpsAsset::StaticType());
            town = top->CreateGroup(u8"Town");
            bell = town->CreateInstance(u8"Bell", OpsAsset::StaticType());
            REQUIRE(crate != nullptr);
            REQUIRE(barrel != nullptr);
            REQUIRE(bell != nullptr);
            REQUIRE(crate->WriteObject(object).IsOk());
            REQUIRE(barrel->WriteObject(object).IsOk());
            REQUIRE(bell->WriteObject(object).IsOk());
            context.SetProject(project.Get());
            AssetOperations::Services services;
            services.context = &context;
            Bed* self = this;
            services.closePages = [self](const Guid& id) { self->closed.PushBack(id); };
            ops = MakeUnique<AssetOperations>(DefaultAllocator(), Move(services)); // no cook, no later: at once
        }
        ~Bed()
        {
            ops.Reset();
            context.SetProject(nullptr);
            project.Reset();
            std::error_code ec;
            std::filesystem::remove_all("asset_ops_project", ec);
        }
    };
}

RTTI_DEFINE_OBJECT(OpsAsset, "rtti::tests::assetops")

TEST_CASE("asset operations: a rename keeps the name rule, saying why not; the default scene's path follows; a group renames too")
{
    Bed bed;
    CHECK(bed.ops->Rename(bed.crate->Id(), u8"Barrel") == String(u8"Barrel is already in Content."));
    CHECK(bed.ops->Rename(bed.crate->Id(), u8"a/b") == String(u8"A name cannot hold /."));
    CHECK(bed.crate->Name() == StringView(u8"Crate"));

    bed.project->Settings().defaultSceneId = bed.crate->Id();
    bed.project->Settings().defaultScene = bed.crate->Path();
    CHECK(bed.ops->Rename(bed.crate->Id(), u8"Box").IsEmpty());
    CHECK(bed.crate->Name() == StringView(u8"Box"));
    CHECK(bed.project->Settings().defaultScene == String(u8"Box"));

    CHECK(bed.ops->RenameGroup(*bed.top, u8"Top") == String(u8"Content cannot be renamed."));
    CHECK(bed.ops->RenameGroup(*bed.town, u8"Village").IsEmpty());
    CHECK(bed.bell->Path() == String(u8"Village/Bell"));
}

TEST_CASE("asset operations: a duplicate lands beside its source; a new group is a folder at once")
{
    Bed bed;
    content::Instance* copy = bed.ops->Duplicate(bed.crate->Id());
    REQUIRE(copy != nullptr);
    CHECK(&copy->OwningGroup() == bed.top);
    CHECK(copy->Name() != bed.crate->Name());
    CHECK(copy->Id() != bed.crate->Id());

    content::Group* made = bed.ops->CreateGroup(*bed.town);
    REQUIRE(made != nullptr);
    CHECK(made->Name() == StringView(u8"Group"));
    CHECK(bed.project->SourceDb().Mount().Exists(made->Path().AsView())); // on disk while empty
    content::Group* second = bed.ops->CreateGroup(*bed.town);
    REQUIRE(second != nullptr);
    CHECK(second->Name() != made->Name());
}

TEST_CASE("asset operations: a delete names what goes, closes its pages first and lets the default scene go with it")
{
    Bed bed;
    const Guid ids[] = {bed.crate->Id(), bed.barrel->Id(), bed.bell->Id()};
    CHECK(bed.ops->Names(Span<const Guid>(ids, 1u)) == String(u8"Crate"));
    CHECK(bed.ops->Names(Span<const Guid>(ids, 2u)) == String(u8"Crate and Barrel"));
    CHECK(bed.ops->Names(Span<const Guid>(ids, 3u)) == String(u8"Crate, Barrel and Bell"));

    bed.project->Settings().defaultSceneId = bed.crate->Id();
    const Guid crate = bed.crate->Id();
    const Guid barrel = bed.barrel->Id();
    Array<Guid> both;
    both.PushBack(crate);
    both.PushBack(barrel);
    bed.ops->Delete(Move(both));
    CHECK(bed.project->SourceDb().GetInstance(crate) == nullptr);
    CHECK(bed.project->SourceDb().GetInstance(barrel) == nullptr);
    REQUIRE(bed.closed.Size() == 2u);
    CHECK(bed.closed[0] == crate);
    CHECK(bed.project->Settings().defaultSceneId.IsNil()); // gone with it

    // A group with all it holds; what showed it is moved to its parent first.
    const Guid bell = bed.bell->Id();
    content::Group* shownNext = nullptr;
    CHECK(AssetOperations::CountAssets(*bed.town) == 1u);
    bed.ops->DeleteGroup(*bed.town, [&shownNext](content::Group& next) { shownNext = &next; });
    CHECK(shownNext == bed.top);
    CHECK(bed.top->GetGroup(u8"Town") == nullptr);
    CHECK(bed.project->SourceDb().GetInstance(bell) == nullptr);
    CHECK(bed.closed.Size() == 3u);
}

TEST_CASE("asset operations: the export flag on an asset and on a group's contents, kept with the project")
{
    Bed bed;
    CHECK_FALSE(bed.ops->IsAlwaysExported(bed.crate->Id()));
    bed.ops->ToggleAlwaysExport(bed.crate->Id());
    CHECK(bed.ops->IsAlwaysExported(bed.crate->Id()));
    bed.ops->ToggleAlwaysExport(*bed.town);
    CHECK(bed.ops->IsAlwaysExported(*bed.town));
    // Kept: a project opened again reads them.
    UniquePtr<editor::EditorProject> again = editor::EditorProject::Open(DefaultAllocator(), u8"asset_ops_project");
    REQUIRE(again);
    CHECK(again->ExportRoots().HasInstance(bed.crate->Id()));
    CHECK(again->ExportRoots().HasGroup(u8"Town"));
    bed.ops->ToggleAlwaysExport(bed.crate->Id());
    CHECK_FALSE(bed.ops->IsAlwaysExported(bed.crate->Id()));
}

TEST_CASE("asset operations: a move takes assets and their files into another group; one whose name is taken there stays")
{
    Bed bed;
    Array<Guid> two;
    two.PushBack(bed.crate->Id());
    two.PushBack(bed.barrel->Id());
    String why;
    CHECK(bed.ops->CanMove(two.AsSpan(), *bed.town, why));
    bed.ops->MoveTo(two, *bed.town);
    CHECK(&bed.crate->OwningGroup() == bed.town);
    CHECK(&bed.barrel->OwningGroup() == bed.town);
    CHECK(bed.crate->ReadObject().Get() != nullptr); // its file moved with it: it reads where it is now
    CHECK_FALSE(bed.ops->CanMove(two.AsSpan(), *bed.town, why));
    CHECK(why == String(u8"Already in Town"));

    content::Instance* other = bed.top->CreateInstance(u8"Bell", OpsAsset::StaticType()); // Town has a Bell
    REQUIRE(other != nullptr);
    Array<Guid> bell;
    bell.PushBack(other->Id());
    CHECK_FALSE(bed.ops->CanMove(bell.AsSpan(), *bed.town, why));
    CHECK(why == String(u8"Bell is already in Town"));
    bed.ops->MoveTo(bell, *bed.town);
    CHECK(&other->OwningGroup() == bed.top); // stayed
}
