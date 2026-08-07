// Phase 5 (editor + resource) - full content-DB round-trip: SaveScene captures a live
// scene into a content-DB instance (SceneDocument primary + "scene" data stream), and
// LoadScene reads it back into a fresh scene whose managers were injected beforehand.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.scene;
import draconic.scene.resource;
import draconic.scene.editor;

using namespace draconic::foundation;
using namespace draconic::vfs;
using namespace draconic::scene;

namespace
{
    struct Tag
    {
        i32 team = 0;
    };
    void Serialize(ISerializer& ar, Tag& t) { draconic::foundation::Serialize(ar, "team", t.team); }

    class TagManager : public SerializableComponentManager<Tag>
    {
    public:
        TagManager() : SerializableComponentManager<Tag>(u8"demo.Tag") {}
    };

    void RemoveTree()
    {
        FileDelete(u8"draconic_scene_db/level.rasset");
        FileDelete(u8"draconic_scene_db/level.scene.bin");
        RemoveDirectory(u8"draconic_scene_db");
    }
}

TEST_CASE("SaveScene -> content DB -> LoadScene round-trips a scene")
{
    GlobalTypeRegistry().Register(SceneDocument::StaticType());
    RegisterSerializable<SceneDocument>();

    RemoveTree();
    NativeFileSystem mount(u8"draconic_scene_db");

    Guid id;
    Guid heroId, foeId;
    {
        // author + save a live scene
        Scene scene(u8"arena");
        TagManager* tags = scene.AddSystem<TagManager>();
        EntityHandle hero = scene.CreateEntity(u8"hero");
        EntityHandle foe = scene.CreateEntity(u8"foe");
        scene.SetParent(foe, hero);
        tags->Add(hero).team = 1;
        tags->Add(foe).team = 2;
        heroId = scene.GetEntityId(hero);
        foeId = scene.GetEntityId(foe);

        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"level", SceneDocument::StaticType());
        id = inst->Id();
        REQUIRE(SaveScene(scene, *inst).IsOk());
    }

    {
        // load into a fresh scene whose manager is injected first (as a subsystem would)
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.GetInstance(id);
        REQUIRE(inst != nullptr);

        // the SceneDocument primary carries the name for discovery
        RefPtr<ISerializable> doc = inst->ReadObject();
        SceneDocument* sd = Cast<SceneDocument>(doc.Get());
        REQUIRE(sd != nullptr);
        CHECK(sd->name == u8"arena");

        Scene scene;
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
