// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// propertyanimation.pipeline - the cook path end to end: author a PropertyAnimationClipAsset, cook it
// with the builder into a content DB, then load the cooked product through the runtime factory and
// sample it, covering the asset, source, product and resource chain.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import foundation.propertyanimation;
import foundation.propertyanimation.resource;
import propertyanimation.pipeline;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::propertyanimation;
using namespace pipeline;

TEST_CASE("propanim.pipeline: asset cooks to the content DB, factory loads + samples it")
{
    RegisterPropertyAnimationAssets(); // asset, source and resource types

    FileDelete(u8"scratch_propanim_pipe_db/clip.rasset");
    RemoveDirectory(u8"scratch_propanim_pipe_db");
    NativeFileSystem mount(u8"scratch_propanim_pipe_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        auto* inst =
            db.RootGroup()->CreateInstance(u8"clip", PropertyAnimationClipSource::StaticType());
        id = inst->Id();

        // Author a clip: a Float3 position track 0..(10,0,0) over 1s.
        PropertyAnimationClip clip;
        PropertyTrack t;
        t.componentType = String(u8"Transform");
        t.propertyPath = String(u8"position");
        t.kind = TrackValueKind::Float3;
        CurveKey a;
        a.time = 0.0f;
        a.value = 0.0f;
        CurveKey b;
        b.time = 1.0f;
        b.value = 10.0f;
        t.channels[0].AddKey(a);
        t.channels[0].AddKey(b);
        clip.tracks.PushBack(Move(t));
        clip.duration = clip.ComputeDuration();

        PropertyAnimationClipAsset asset;
        PropertyAnimationClipSource::FromClip(clip, asset.source);

        PropertyAnimationClipAssetBuilder builder;
        REQUIRE(builder.AssetType() == &PropertyAnimationClipAsset::StaticType());
        REQUIRE(builder.ProductType() == &PropertyAnimationClipSource::StaticType());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // Load the cooked product through the runtime factory.
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    PropertyAnimationClipFactory factory;
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<PropertyAnimationClipResource> res = manager.Bind<PropertyAnimationClipResource>(id);
    REQUIRE(res);
    CHECK(res->clip.duration == doctest::Approx(1.0f));
    REQUIRE(res->clip.tracks.Size() == 1u);
    CHECK(res->clip.tracks[0].propertyPath.AsView() == StringView(u8"position"));
    CHECK(res->clip.tracks[0].Sample(0.5f).Get<Float3>().x == doctest::Approx(5.0f));

    FileDelete(u8"scratch_propanim_pipe_db/clip.rasset");
    FileDelete(u8"scratch_propanim_pipe_db/clip.data.bin");
    RemoveDirectory(u8"scratch_propanim_pipe_db");
}

TEST_CASE("propanim.pipeline: asset embeds + serializes its source (undo/save round-trip shape)")
{
    RegisterPropertyAnimationAssets();

    PropertyAnimationClipAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"Clips/pulse.rasset");
    asset.source.duration = 2.5f;
    asset.source.trackKind.PushBack(static_cast<u8>(TrackValueKind::Float));
    asset.source.trackComponent.PushBack(String(u8"Light"));
    asset.source.trackPath.PushBack(String(u8"intensity"));

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        asset.Serialize(writer);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    PropertyAnimationClipAsset restored;
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        restored.Serialize(reader);
    }
    CHECK(restored.fileName.View() == StringView(u8"Clips/pulse.rasset"));
    CHECK(restored.source.duration == doctest::Approx(2.5f));
    REQUIRE(restored.source.trackPath.Size() == 1u);
    CHECK(restored.source.trackPath[0].AsView() == StringView(u8"intensity"));
}
