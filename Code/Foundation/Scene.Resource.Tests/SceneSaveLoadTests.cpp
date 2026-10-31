// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Full content-DB round-trip: SaveScene captures a live scene into a content-DB
// instance (SceneDocument primary and "scene" data stream), and
// LoadScene reads it back into a fresh scene whose managers were injected beforehand.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.scene;
import foundation.scene.resource;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::scene;

namespace
{
    struct Tag
    {
        i32 team = 0;
    };
    void Serialize(ISerializer& ar, Tag& t) { foundation::core::Serialize(ar, "team", t.team); }

    class TagManager : public SerializableComponentManager<Tag>
    {
    public:
        TagManager() : SerializableComponentManager<Tag>(u8"demo.Tag") {}
    };

    void RemoveTree()
    {
        FileDelete(u8"scratch_scene_db/level.rasset");
        FileDelete(u8"scratch_scene_db/level.scene.bin");
        FileDelete(u8"scratch_scene_db/level.scene.data");
        RemoveDirectory(u8"scratch_scene_db");
    }
}

TEST_CASE("SaveScene -> content DB -> LoadScene round-trips a scene")
{
    GlobalTypeRegistry().Register(SceneDocument::StaticType());
    RegisterSerializable<SceneDocument>();

    RemoveTree();
    NativeFileSystem mount(u8"scratch_scene_db", DefaultAllocator());

    Guid id;
    Guid heroId, foeId;
    {
        // author + save a live scene
        Scene scene(DefaultAllocator(), u8"arena");
        TagManager* tags = scene.AddSystem<TagManager>();
        EntityHandle hero = scene.CreateEntity(u8"hero");
        EntityHandle foe = scene.CreateEntity(u8"foe");
        scene.SetParent(foe, hero);
        tags->Add(hero).team = 1;
        tags->Add(foe).team = 2;
        heroId = scene.GetEntityId(hero);
        foeId = scene.GetEntityId(foe);

        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"level", SceneDocument::StaticType());
        id = inst->Id();
        REQUIRE(SaveScene(scene, *inst).IsOk());
    }

    {
        // load into a fresh scene whose manager is injected first (as a subsystem would)
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.GetInstance(id);
        REQUIRE(inst != nullptr);

        // the SceneDocument primary carries the name for discovery
        RefPtr<ISerializable> doc = inst->ReadObject();
        SceneDocument* sd = Cast<SceneDocument>(doc.Get());
        REQUIRE(sd != nullptr);
        CHECK(sd->name == u8"arena");

        Scene scene{DefaultAllocator()};
        TagManager* tags = scene.AddSystem<TagManager>();
        REQUIRE(LoadScene(*inst, scene).IsOk());

        CHECK(scene.Name() == u8"arena");
        CHECK(scene.EntityCount() == 2);
        EntityHandle hero = scene.FindEntity(heroId);
        EntityHandle foe = scene.FindEntity(foeId);
        REQUIRE(hero.IsAssigned());
        REQUIRE(foe.IsAssigned());
        CHECK(scene.GetParent(foe) == hero);
        REQUIRE(tags->Has(hero));
        REQUIRE(tags->Has(foe));
        CHECK(tags->Get(hero)->team == 1);
        CHECK(tags->Get(foe)->team == 2);
    }

    RemoveTree();
}

TEST_CASE("LoadScene reports the reader's verdict: a stale component payload is a failed load")
{
    GlobalTypeRegistry().Register(SceneDocument::StaticType());
    RegisterSerializable<SceneDocument>();

    RemoveTree();
    NativeFileSystem mount(u8"scratch_scene_db", DefaultAllocator());
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount,
                                            foundation::core::BinarySerializerFactory(), u8".rasset");
    auto* inst = db.RootGroup()->CreateInstance(u8"level", SceneDocument::StaticType());
    {
        Scene scene(DefaultAllocator(), u8"arena");
        scene.AddSystem<TagManager>()->Add(scene.CreateEntity(u8"hero")).team = 1;
        REQUIRE(SaveScene(scene, *inst).IsOk());
    }

    // Re-stamp the Tag record's data version in the text stream (Tag is unreflected: version 0)
    // to one this build never wrote, which is what a source saved by a later build looks like.
    {
        UniquePtr<IStream> stream = inst->ReadData(u8"scene");
        REQUIRE(stream.Get() != nullptr);
        Array<byte> text;
        text.Resize(static_cast<usize>(stream->Size()));
        REQUIRE(stream->Read(text.Data(), text.Size()) == text.Size());
        const StringView stamp = u8"<u32 name=\"version\">0</u32>";
        bool restamped = false;
        for (usize i = 0; !restamped && i + stamp.Size() <= text.Size(); ++i)
        {
            if (StringView(reinterpret_cast<const utf8char*>(text.Data() + i), stamp.Size()) == stamp)
            {
                text[i + stamp.Size() - 7] = static_cast<byte>('9'); // the digit before "</u32>"
                restamped = true;
            }
        }
        REQUIRE(restamped);
        REQUIRE(inst->WriteData(u8"scene", Span<const byte>{text.Data(), text.Size()},
                                foundation::content::StreamEncoding::Text)
                    .IsOk());
    }

    Scene scene{DefaultAllocator()};
    (void)scene.AddSystem<TagManager>();
    const Status loaded = LoadScene(*inst, scene);
    CHECK_FALSE(loaded.IsOk());
    CHECK(loaded.Code() == ErrorCode::NotSupported);

    RemoveTree();
}
