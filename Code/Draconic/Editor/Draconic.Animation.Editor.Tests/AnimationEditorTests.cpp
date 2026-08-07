// Animation editor: cook a SkeletonAsset through its builder into the content DB, then load it back
// through the resource factory and verify the runtime skeleton. Exercises the authoring -> cook ->
// product path (the model importer that fills the source is deferred).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.editor;
import draconic.animation;
import draconic.animation.resource;
import draconic.animation.editor;

using namespace draconic::foundation;
using namespace draconic::vfs;
using namespace draconic::resource;
using namespace draconic::animation;

TEST_CASE("skeleton asset: builder cooks into the content DB, factory loads it back")
{
    GlobalTypeRegistry().Register(SkeletonSource::StaticType());
    RegisterSerializable<SkeletonSource>();
    GlobalTypeRegistry().Register(Skeleton::StaticType());

    FileDelete(u8"draconic_anim_ed_db/skel.rasset");
    RemoveDirectory(u8"draconic_anim_ed_db");
    NativeFileSystem mount(u8"draconic_anim_ed_db");

    Guid id;
    {
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"skel", SkeletonSource::StaticType());
        id = inst->Id();

        // Author a skeleton asset (source populated from a runtime skeleton - model importer later).
        Skeleton skel{2};
        skel.Bones()[0].index = 0;
        skel.Bones()[0].parentIndex = -1;
        skel.Bones()[0].name = String{u8"root"};
        skel.Bones()[1].index = 1;
        skel.Bones()[1].parentIndex = 0;
        skel.Bones()[1].name = String{u8"child"};
        skel.BuildNameMap();
        skel.FindRootBones();
        skel.BuildChildIndices();
        skel.ComputeInverseBindPoses();

        SkeletonAsset asset;
        asset.fileName = draconic::vfs::SourcePath(u8"models/char.gltf");
        SkeletonSource::FromSkeleton(skel, asset.source);

        SkeletonAssetBuilder builder;
        draconic::editor::AssetBuildContext ctx;
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    SkeletonFactory factory;
    ResourceManager manager(db);
    manager.AddFactory(&factory);

    Proxy<Skeleton> skel = manager.Bind<Skeleton>(id);
    REQUIRE(skel);
    CHECK(skel->BoneCount() == 2);
    CHECK(skel->FindBone(u8"child") == 1);
    CHECK(skel->RootBones().Size() == 1);

    FileDelete(u8"draconic_anim_ed_db/skel.rasset");
    RemoveDirectory(u8"draconic_anim_ed_db");
}
