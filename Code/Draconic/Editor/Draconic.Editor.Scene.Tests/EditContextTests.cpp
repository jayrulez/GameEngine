// SceneEditContext tests (headless live Scene): every mutation is an undoable command by Guid.
// Covers create (redo keeps the SAME Guid), rename (merge), reparent (cycle-refusal, undo),
// destroy (undo restores the FULL subtree - names, transforms, hierarchy, active flags, and
// serializable components - the fix for Sedulous's lossy destroy-undo), and selection pruning.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include <initializer_list>
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.scene;
import draconic.scene.resource;
import draconic.engine.render;
import draconic.editor.core;
import draconic.editor.scene;
import draconic.content;
import draconic.materials;
import draconic.materials.editor;

using namespace draconic::foundation;
using namespace draconic::editor;
namespace scene = draconic::scene;

namespace
{
    struct HealthComponent
    {
        i32 amount = 0;
    };

    void Serialize(ISerializer& ar, HealthComponent& c)
    {
        draconic::foundation::Serialize(ar, "amount", c.amount);
    }

    class HealthManager final : public scene::SerializableComponentManager<HealthComponent>
    {
    public:
        HealthManager() : SerializableComponentManager(u8"test.health") {}
    };
}

TEST_CASE("scene-edit: create entity - undo/redo keeps the same Guid, parents apply")
{
    scene::Scene scene(u8"t");
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    const Guid parent = edit.CreateEntity(u8"Parent");
    REQUIRE(parent != Guid{});
    CHECK(scene.EntityCount() == 1);
    CHECK(edit.EntitySelection().Contains(parent)); // created entity becomes the selection

    const Guid child = edit.CreateEntity(u8"Child", parent);
    REQUIRE(child != Guid{});
    CHECK(scene.GetParent(edit.Resolve(child)) == edit.Resolve(parent));

    commands.Undo(); // child gone
    CHECK(scene.EntityCount() == 1);
    CHECK(!edit.Resolve(child).IsAssigned());

    commands.Redo(); // child back with the SAME Guid + parent
    CHECK(scene.EntityCount() == 2);
    REQUIRE(edit.Resolve(child).IsAssigned());
    CHECK(scene.GetParent(edit.Resolve(child)) == edit.Resolve(parent));
}

TEST_CASE("scene-edit: rename merges and undoes to the original")
{
    scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    const Guid id = edit.CreateEntity(u8"Original");
    edit.RenameEntity(id, u8"First");
    edit.RenameEntity(id, u8"Second"); // merges into the previous rename
    CHECK(scene.GetEntityName(edit.Resolve(id)) == u8"Second");

    commands.Undo(); // ONE undo reverts both renames
    CHECK(scene.GetEntityName(edit.Resolve(id)) == u8"Original");
    commands.Redo();
    CHECK(scene.GetEntityName(edit.Resolve(id)) == u8"Second");
}

TEST_CASE("scene-edit: reparent - undo restores, cycles and no-ops are refused")
{
    scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    const Guid a = edit.CreateEntity(u8"A");
    const Guid b = edit.CreateEntity(u8"B", a);
    const Guid c = edit.CreateEntity(u8"C");
    const usize baseline = commands.Size();

    edit.ReparentEntity(b, c); // A/B -> C/B
    CHECK(scene.GetParent(edit.Resolve(b)) == edit.Resolve(c));

    commands.Undo();
    CHECK(scene.GetParent(edit.Resolve(b)) == edit.Resolve(a));

    // A cycle (parent A under its own subtree via B... i.e. A under B) is refused + dropped.
    commands.Redo();           // back to C/B
    edit.ReparentEntity(a, b); // a's descendant?? b is not under a anymore, so this is LEGAL
    CHECK(scene.GetParent(edit.Resolve(a)) == edit.Resolve(b));
    edit.ReparentEntity(c, a); // c is now an ancestor of a?? a is under b under c: cycle -> refused
    CHECK(scene.GetParent(edit.Resolve(c)) == scene::EntityHandle::Invalid());

    // Self-parenting refused; no-op reparent (same parent) refused.
    edit.ReparentEntity(b, b);
    CHECK(scene.GetParent(edit.Resolve(b)) == edit.Resolve(c));
    const usize before = commands.Size();
    edit.ReparentEntity(b, c); // already C's child - dropped, no undo pollution
    CHECK(commands.Size() == before);
    (void)baseline;
}

TEST_CASE("scene-edit: destroy undo restores the full subtree with components")
{
    scene::Scene scene;
    auto* health = scene.AddSystem<HealthManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    const Guid root = edit.CreateEntity(u8"Root");
    const Guid childA = edit.CreateEntity(u8"ChildA", root);
    const Guid grand = edit.CreateEntity(u8"Grandchild", childA);
    const Guid childB = edit.CreateEntity(u8"ChildB", root);

    // Non-default state that must survive the round-trip.
    Transform t;
    t.position = Float3{1.0f, 2.0f, 3.0f};
    scene.SetLocalTransform(edit.Resolve(childA), t);
    scene.SetActive(edit.Resolve(childB), false);
    health->Add(edit.Resolve(grand)).amount = 77;
    health->Add(edit.Resolve(root)).amount = 5;

    edit.EntitySelection().Set(grand);
    edit.DestroyEntity(root);

    CHECK(scene.EntityCount() == 0);
    CHECK(health->ComponentCount() == 0);
    CHECK(edit.EntitySelection().IsEmpty()); // doomed subtree deselected

    commands.Undo();

    CHECK(scene.EntityCount() == 4);
    REQUIRE(edit.Resolve(root).IsAssigned());
    REQUIRE(edit.Resolve(childA).IsAssigned());
    REQUIRE(edit.Resolve(grand).IsAssigned());
    REQUIRE(edit.Resolve(childB).IsAssigned());

    // Hierarchy restored.
    CHECK(scene.GetParent(edit.Resolve(childA)) == edit.Resolve(root));
    CHECK(scene.GetParent(edit.Resolve(grand)) == edit.Resolve(childA));
    CHECK(scene.GetParent(edit.Resolve(childB)) == edit.Resolve(root));

    // Names, transform, active flag restored.
    CHECK(scene.GetEntityName(edit.Resolve(grand)) == u8"Grandchild");
    CHECK(scene.GetLocalTransform(edit.Resolve(childA)).position.y == doctest::Approx(2.0f));
    CHECK(!scene.IsActive(edit.Resolve(childB)));

    // Components restored with their data.
    REQUIRE(health->HasComponent(edit.Resolve(grand)));
    CHECK(health->Get(edit.Resolve(grand))->amount == 77);
    REQUIRE(health->HasComponent(edit.Resolve(root)));
    CHECK(health->Get(edit.Resolve(root))->amount == 5);

    // Redo destroys it all again.
    commands.Redo();
    CHECK(scene.EntityCount() == 0);
    CHECK(health->ComponentCount() == 0);
}

TEST_CASE("scene-edit: destroying a missing entity is a safe no-op")
{
    scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    edit.DestroyEntity(Guid{1, 2});
    CHECK(commands.Size() == 0);
    edit.RenameEntity(Guid{1, 2}, u8"nope"); // failed execute -> dropped
    CHECK(commands.Size() == 0);
}

TEST_CASE("scene-edit: sibling reorder command with undo/redo")
{
    scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    const Guid a = edit.CreateEntity(u8"A");
    const Guid b = edit.CreateEntity(u8"B");
    const Guid c = edit.CreateEntity(u8"C");

    // Move C before A: order c, a, b.
    edit.MoveEntityBefore(c, a);
    CHECK(scene.GetFirstRoot() == edit.Resolve(c));

    // Undo restores the exact old position (C was last).
    commands.Undo();
    CHECK(scene.GetFirstRoot() == edit.Resolve(a));
    CHECK(scene.GetNextSibling(edit.Resolve(b)) == edit.Resolve(c));
    commands.Redo();
    CHECK(scene.GetFirstRoot() == edit.Resolve(c));

    // Move A to the END of the root list (empty sibling): c, b, a.
    edit.MoveEntityBefore(a, Guid{});
    CHECK(scene.GetNextSibling(edit.Resolve(b)) == edit.Resolve(a));
    commands.Undo(); // back to c, a, b (A was before B)
    CHECK(scene.GetNextSibling(edit.Resolve(c)) == edit.Resolve(a));
    CHECK(scene.GetNextSibling(edit.Resolve(a)) == edit.Resolve(b));

    // No-op move (already before B) is dropped, not pushed.
    const usize size = commands.Size();
    edit.MoveEntityBefore(a, b);
    CHECK(commands.Size() == size);

    // Cycle refused: moving A before a slot under its own subtree.
    edit.ReparentEntity(b, a); // b under a
    const Guid d = edit.CreateEntity(u8"D", b);
    const usize size2 = commands.Size();
    edit.MoveEntityBefore(a, d); // slot parent = b, inside a's subtree
    CHECK(commands.Size() == size2);
}

TEST_CASE("scene-edit: reparent preserves the world transform; undo restores the exact local")
{
    scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    const Guid parent = edit.CreateEntity(u8"Parent");
    const Guid child = edit.CreateEntity(u8"Child");

    Transform tp;
    tp.position = Float3{5.0f, 0.0f, 0.0f};
    tp.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.5f);
    scene.SetLocalTransform(edit.Resolve(parent), tp);
    Transform tc;
    tc.position = Float3{1.0f, 2.0f, 3.0f};
    scene.SetLocalTransform(edit.Resolve(child), tc);

    const Float4x4 worldBefore = scene.ComposeWorldMatrix(edit.Resolve(child));

    edit.ReparentEntity(child, parent);
    const Float4x4 worldAfter = scene.ComposeWorldMatrix(edit.Resolve(child));
    for (i32 r = 0; r < 4; ++r)
        for (i32 c = 0; c < 4; ++c)
            CHECK(worldAfter.m[r][c] == doctest::Approx(worldBefore.m[r][c]).epsilon(0.001f));

    // Undo: exact original local (bit-identical restore, not a decompose round-trip).
    commands.Undo();
    CHECK(scene.GetLocalTransform(edit.Resolve(child)).position.x == 1.0f);
    CHECK(scene.GetLocalTransform(edit.Resolve(child)).position.y == 2.0f);
    CHECK(scene.GetParent(edit.Resolve(child)) == scene::EntityHandle::Invalid());

    // Redo reproduces the preserved world again.
    commands.Redo();
    const Float4x4 worldRedo = scene.ComposeWorldMatrix(edit.Resolve(child));
    for (i32 r = 0; r < 4; ++r)
        for (i32 c = 0; c < 4; ++c)
            CHECK(worldRedo.m[r][c] == doctest::Approx(worldBefore.m[r][c]).epsilon(0.001f));

    // Same-parent reorder keeps the local EXACT (no decompose noise).
    const Guid s1 = edit.CreateEntity(u8"S1", parent);
    Transform ts;
    ts.position = Float3{0.25f, 0.5f, 0.75f};
    scene.SetLocalTransform(edit.Resolve(s1), ts);
    edit.MoveEntityBefore(s1, child); // reorder within `parent`
    CHECK(scene.GetLocalTransform(edit.Resolve(s1)).position.x == 0.25f);
    CHECK(scene.GetLocalTransform(edit.Resolve(s1)).position.z == 0.75f);
}

namespace
{
    // A reflected (but NOT serializable) component - exercises the property paths and the
    // reflected-snapshot remove-undo.
    enum class TestMode : u32
    {
        Off = 0,
        Slow = 1,
        Fast = 2
    };

    struct WidgetComponent
    {
        f32 speed = 1.0f;
        bool spin = false;
        Float3 offset{0, 0, 0};
        TestMode mode = TestMode::Off;
    };

    class WidgetManager final : public scene::ComponentManager<WidgetComponent>
    {
    };
}

DRACONIC_REFLECT_ENUM(TestMode, "draconic::editor::test")
{
    builder.Value("Off", TestMode::Off);
    builder.Value("Slow", TestMode::Slow);
    builder.Value("Fast", TestMode::Fast);
}

DRACONIC_REFLECT_VALUE(WidgetComponent, "draconic::editor::test")
{
    builder.Property<&WidgetComponent::speed>("speed")
        .Property<&WidgetComponent::spin>("spin")
        .Property<&WidgetComponent::offset>("offset")
        .Property<&WidgetComponent::mode>("mode");
}

TEST_CASE("scene-edit: transform + active commands (merge, undo)")
{
    scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    const Guid id = edit.CreateEntity(u8"E");
    const usize baseline = commands.Size();

    // A "drag": many transform sets merge into ONE undo entry.
    Transform t;
    for (i32 i = 1; i <= 5; ++i)
    {
        t.position = Float3{static_cast<f32>(i), 0, 0};
        edit.SetLocalTransform(id, t);
    }
    CHECK(commands.Size() == baseline + 1);
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.x == doctest::Approx(5.0f));
    commands.Undo();
    CHECK(scene.GetLocalTransform(edit.Resolve(id)).position.x == doctest::Approx(0.0f));

    edit.SetEntityActive(id, false);
    CHECK(!scene.IsActive(edit.Resolve(id)));
    commands.Undo();
    CHECK(scene.IsActive(edit.Resolve(id)));
    edit.SetEntityActive(id, true); // no-op - dropped
    CHECK(commands.Size() == baseline + 1);
}

TEST_CASE("scene-edit: component property commands (variant + raw enum, merge, undo)")
{
    DraconicRegisterEnum_TestMode();
    DraconicRegisterValue_WidgetComponent();

    scene::Scene scene;
    auto* widgets = scene.AddSystem<WidgetManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    const Guid id = edit.CreateEntity(u8"E");
    const TypeInfo* type = widgets->ComponentType();

    // Add via command; undo removes; redo re-adds.
    edit.AddComponent(id, type);
    CHECK(widgets->HasComponent(edit.Resolve(id)));
    commands.Undo();
    CHECK(!widgets->HasComponent(edit.Resolve(id)));
    commands.Redo();
    REQUIRE(widgets->HasComponent(edit.Resolve(id)));
    edit.AddComponent(id, type); // already present - dropped
    const usize afterAdd = commands.Size();

    // Variant path with merge: a scrub of `speed` is one undo entry.
    edit.SetComponentProperty(id, type, "speed", Variant::From<f32>(2.0f));
    edit.SetComponentProperty(id, type, "speed", Variant::From<f32>(3.5f));
    CHECK(commands.Size() == afterAdd + 1);
    CHECK(widgets->Get(edit.Resolve(id))->speed == doctest::Approx(3.5f));
    commands.Undo();
    CHECK(widgets->Get(edit.Resolve(id))->speed == doctest::Approx(1.0f));
    commands.Redo();

    // Different property does NOT merge.
    edit.SetComponentProperty(id, type, "offset", Variant::From<Float3>(Float3{1, 2, 3}));
    CHECK(commands.Size() == afterAdd + 2);
    CHECK(widgets->Get(edit.Resolve(id))->offset.y == doctest::Approx(2.0f));

    // Raw (enum) path through PropertyInfo::address.
    edit.SetComponentPropertyRaw(id, type, "mode", static_cast<i64>(TestMode::Fast));
    CHECK(widgets->Get(edit.Resolve(id))->mode == TestMode::Fast);
    commands.Undo();
    CHECK(widgets->Get(edit.Resolve(id))->mode == TestMode::Off);

    // Remove undo restores the reflected state (non-serializable manager).
    widgets->Get(edit.Resolve(id))->spin = true;
    edit.RemoveComponent(id, type);
    CHECK(!widgets->HasComponent(edit.Resolve(id)));
    commands.Undo();
    REQUIRE(widgets->HasComponent(edit.Resolve(id)));
    CHECK(widgets->Get(edit.Resolve(id))->speed == doctest::Approx(3.5f));
    CHECK(widgets->Get(edit.Resolve(id))->spin);
    CHECK(widgets->Get(edit.Resolve(id))->offset.z == doctest::Approx(3.0f));
}

TEST_CASE("scene-edit: remove-component undo via serialization blob (serializable manager)")
{
    scene::Scene scene;
    auto* health = scene.AddSystem<HealthManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    const Guid id = edit.CreateEntity(u8"E");

    health->Add(edit.Resolve(id)).amount = 42;
    edit.RemoveComponent(id, health->ComponentType());
    CHECK(!health->HasComponent(edit.Resolve(id)));
    commands.Undo();
    REQUIRE(health->HasComponent(edit.Resolve(id)));
    CHECK(health->Get(edit.Resolve(id))->amount == 42); // full fidelity via the blob
}

// Regression (user-reported): undoing an entity destroy restored the entity but LOST its
// LightComponent - destroy-undo snapshots components through SERIALIZABLE managers only, and
// the light/camera/probe managers weren't serializable (which also silently dropped them from
// scene saves).
TEST_CASE("edit-context: destroy-undo restores light components (and their values)")
{
    scene::Scene scene;
    scene.AddSystem<draconic::render::LightComponentManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    const Guid id = edit.CreateEntity(u8"Sun");
    auto* lights = scene.GetSystem<draconic::render::LightComponentManager>();
    REQUIRE(lights != nullptr);
    {
        draconic::render::LightComponent& light = lights->Add(edit.Resolve(id));
        light.type = draconic::render::LightType::Spot;
        light.intensity = 3.5f;
        light.range = 42.0f;
        light.castsShadows = true;
    }

    edit.DestroyEntity(id);
    CHECK_FALSE(edit.Resolve(id).IsAssigned());

    commands.Undo();
    const scene::EntityHandle restored = edit.Resolve(id);
    REQUIRE(restored.IsAssigned());
    draconic::render::LightComponent* light = lights->Get(restored);
    REQUIRE(light != nullptr);                               // the component came back...
    CHECK(light->type == draconic::render::LightType::Spot); // ...with its exact values
    CHECK(light->intensity == doctest::Approx(3.5f));
    CHECK(light->range == doctest::Approx(42.0f));
    CHECK(light->castsShadows);
}

TEST_CASE("edit-context: duplicate entity - fresh guids, subtree + components, one undo")
{
    scene::Scene scene;
    scene.AddSystem<draconic::render::LightComponentManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    const Guid parent = edit.CreateEntity(u8"Rig");
    const Guid child = edit.CreateEntity(u8"Lamp", parent);
    scene.SetLocalPosition(edit.Resolve(parent), Float3{3, 0, 0});
    auto* lights = scene.GetSystem<draconic::render::LightComponentManager>();
    {
        draconic::render::LightComponent& light = lights->Add(edit.Resolve(child));
        light.intensity = 7.0f;
    }

    const Guid copy = edit.DuplicateEntity(parent);
    REQUIRE(copy != Guid{});
    CHECK(copy != parent); // fresh identity
    const scene::EntityHandle copyRoot = edit.Resolve(copy);
    REQUIRE(copyRoot.IsAssigned());
    CHECK(scene.GetEntityName(copyRoot) == u8"Rig (2)"); // copies are distinguishable
    CHECK(scene.GetLocalTransform(copyRoot).position.x == doctest::Approx(3.0f));
    CHECK(!scene.GetParent(copyRoot).IsAssigned()); // sibling of the original (root)
    REQUIRE(edit.EntitySelection().Primary() != nullptr);
    CHECK(*edit.EntitySelection().Primary() == copy); // the copy becomes the selection

    // The child came along, with its component values, under the COPY (not the original).
    REQUIRE(scene.GetChildCount(copyRoot) == 1u);
    const scene::EntityHandle copyChild = scene.GetFirstChild(copyRoot);
    CHECK(scene.GetEntityName(copyChild) == u8"Lamp");
    CHECK(scene.GetEntityId(copyChild) != child);
    draconic::render::LightComponent* light = lights->Get(copyChild);
    REQUIRE(light != nullptr);
    CHECK(light->intensity == doctest::Approx(7.0f));

    // One undo removes the whole copy; redo brings it back with the SAME fresh guids.
    commands.Undo();
    CHECK_FALSE(edit.Resolve(copy).IsAssigned());
    CHECK(edit.Resolve(parent).IsAssigned()); // original untouched
    commands.Redo();
    REQUIRE(edit.Resolve(copy).IsAssigned());
    CHECK(lights->Get(scene.GetFirstChild(edit.Resolve(copy))) != nullptr);
}

TEST_CASE("edit-context: copy/paste entities across scenes with fresh guids")
{
    scene::Scene sceneA;
    sceneA.AddSystem<draconic::render::LightComponentManager>();
    EditorCommandStack commandsA;
    SceneEditContext editA(sceneA, commandsA);

    const Guid src = editA.CreateEntity(u8"Prop");
    const Guid srcChild = editA.CreateEntity(u8"Bulb", src);
    {
        auto* lights = sceneA.GetSystem<draconic::render::LightComponentManager>();
        lights->Add(editA.Resolve(srcChild)).range = 12.0f;
    }
    const Array<byte> blob = editA.CopyEntity(src);
    REQUIRE(!blob.IsEmpty());

    // Paste into a DIFFERENT scene (its own command stack), under a chosen parent.
    scene::Scene sceneB;
    sceneB.AddSystem<draconic::render::LightComponentManager>();
    EditorCommandStack commandsB;
    SceneEditContext editB(sceneB, commandsB);
    const Guid target = editB.CreateEntity(u8"Holder");

    const Guid pasted = editB.PasteEntities(Span<const byte>{blob.Data(), blob.Size()}, target);
    REQUIRE(pasted != Guid{});
    const scene::EntityHandle root = editB.Resolve(pasted);
    REQUIRE(root.IsAssigned());
    CHECK(sceneB.GetEntityName(root) == u8"Prop");
    CHECK(sceneB.GetEntityId(sceneB.GetParent(root)) == target);
    REQUIRE(sceneB.GetChildCount(root) == 1u);
    auto* lightsB = sceneB.GetSystem<draconic::render::LightComponentManager>();
    draconic::render::LightComponent* light = lightsB->Get(sceneB.GetFirstChild(root));
    REQUIRE(light != nullptr);
    CHECK(light->range == doctest::Approx(12.0f));

    // The same blob pastes AGAIN (fresh guids every time); the source scene never changed.
    const Guid pasted2 = editB.PasteEntities(Span<const byte>{blob.Data(), blob.Size()});
    REQUIRE(pasted2 != Guid{});
    CHECK(pasted2 != pasted);
    CHECK(editA.Resolve(src).IsAssigned());

    // Undo in B removes only the last paste.
    commandsB.Undo();
    CHECK_FALSE(editB.Resolve(pasted2).IsAssigned());
    CHECK(editB.Resolve(pasted).IsAssigned());
}

TEST_CASE("edit-context: copy/paste component - add, overwrite, and exact undo")
{
    scene::Scene scene;
    scene.AddSystem<draconic::render::LightComponentManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    auto* lights = scene.GetSystem<draconic::render::LightComponentManager>();

    const Guid a = edit.CreateEntity(u8"A");
    const Guid b = edit.CreateEntity(u8"B");
    lights->Add(edit.Resolve(a)).intensity = 9.0f;

    const Array<byte> blob = edit.CopyComponent(a, &TypeOf<draconic::render::LightComponent>());
    REQUIRE(!blob.IsEmpty());
    CHECK(SceneEditContext::PeekComponentTypeId(Span<const byte>{blob.Data(), blob.Size()}) ==
          u8"light");

    // Paste onto an entity WITHOUT the component: adds it. Undo removes it again.
    REQUIRE(edit.PasteComponent(b, Span<const byte>{blob.Data(), blob.Size()}));
    REQUIRE(lights->Get(edit.Resolve(b)) != nullptr);
    CHECK(lights->Get(edit.Resolve(b))->intensity == doctest::Approx(9.0f));
    commands.Undo();
    CHECK(lights->Get(edit.Resolve(b)) == nullptr);
    commands.Redo();
    REQUIRE(lights->Get(edit.Resolve(b)) != nullptr);

    // Paste onto an entity WITH the component: overwrites; undo restores the prior values.
    lights->Get(edit.Resolve(b))->intensity = 1.0f;
    REQUIRE(edit.PasteComponent(b, Span<const byte>{blob.Data(), blob.Size()}));
    CHECK(lights->Get(edit.Resolve(b))->intensity == doctest::Approx(9.0f));
    commands.Undo();
    REQUIRE(lights->Get(edit.Resolve(b)) != nullptr);
    CHECK(lights->Get(edit.Resolve(b))->intensity == doctest::Approx(1.0f));
}

TEST_CASE("edit-context: generic container mutation (MutateComponent-style add) persists + undoes")
{
    draconic::render::RegisterRenderComponentReflection();
    scene::Scene scene;
    scene.AddSystem<draconic::render::MeshComponentManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    auto* meshes = scene.GetSystem<draconic::render::MeshComponentManager>();
    const Guid a = edit.CreateEntity(u8"Mesh");
    (void)meshes->Add(edit.Resolve(a)); // empty materials

    const TypeInfo* type = &TypeOf<draconic::render::MeshComponent>();
    const PropertyInfo* matProp = FindProperty(*type, "materials");
    REQUIRE(matProp != nullptr);
    REQUIRE(matProp->type != nullptr);
    REQUIRE(IsContainer(*matProp->type)); // the reflected container is registered
    const ContainerInfo& ci = *matProp->type->container;

    auto containerInstance = [&]() -> Instance
    {
        Instance comp = meshes->GetComponentInstance(edit.Resolve(a));
        return Instance(matProp->address(comp), matProp->type);
    };

    // Step 1: does emplace on the live component add an element?
    CHECK(ContainerSize(ci, containerInstance()) == 0u);

    // Replicate MutateComponent exactly: snapshot A, emplace, snapshot B, restore A, paste B.
    Array<byte> before = edit.CopyComponent(a, type);
    REQUIRE(!before.IsEmpty()); // MeshComponent is serializable
    (void)ContainerEmplaceDefault(ci, containerInstance(), ContainerSize(ci, containerInstance()));
    CHECK(ContainerSize(ci, containerInstance()) == 1u); // emplace added it to the live component
    Array<byte> after = edit.CopyComponent(a, type);
    REQUIRE(!after.IsEmpty());

    // Restore live to A (non-undoable), then paste B as the undo step - the MutateComponent tail.
    {
        MemoryStream buffer;
        (void)buffer.Write(before.Data(), before.Size());
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(buffer, SerializeMode::Read);
        String typeId;
        draconic::foundation::Serialize(ar, "type", typeId);
        meshes->ReadComponent(ar, edit.Resolve(a));
    }
    CHECK(ContainerSize(ci, containerInstance()) == 0u); // restored
    REQUIRE(edit.PasteComponent(a, Span<const byte>{after.Data(), after.Size()}));
    CHECK(ContainerSize(ci, containerInstance()) == 1u); // paste re-applied the add (one undo step)

    commands.Undo();
    CHECK(ContainerSize(ci, containerInstance()) == 0u); // undo removes it
    commands.Redo();
    CHECK(ContainerSize(ci, containerInstance()) == 1u);
}

namespace
{
    // A scene-level settings block (see SceneSerializeTests' FogSystem): the inspector's
    // no-entity-selected view edits these through SetSceneSettingProperty commands.
    struct WindSettings
    {
        f32 speed = 1.0f;
    };

    class WindSystem final : public scene::SceneSystem
    {
    public:
        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override
        {
            return &TypeOf<WindSettings>();
        }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &settings; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"wind"; }
        void SerializeSettings(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "speed", settings.speed);
        }
        WindSettings settings;
    };

}

DRACONIC_REFLECT_VALUE(WindSettings, "draconic::editor::test")
{
    builder.DataVersion(1).Property<&WindSettings::speed>("speed");
}

namespace
{
    void RegisterWindReflection() { DraconicRegisterValue_WindSettings(); }
}

TEST_CASE("edit-context: scene-setting edits are undoable commands and merge like scrubs")
{
    RegisterWindReflection();
    scene::Scene scene(u8"s");
    WindSystem* wind = scene.AddSystem<WindSystem>();
    draconic::editor::EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    const TypeInfo* type = &TypeOf<WindSettings>();

    // Two consecutive edits to the same property MERGE into one undo entry...
    edit.SetSceneSettingProperty(type, "speed", Variant::From<f32>(2.0f));
    edit.SetSceneSettingProperty(type, "speed", Variant::From<f32>(3.0f));
    CHECK(wind->settings.speed == 3.0f);

    // ...whose undo restores the ORIGINAL value in one step.
    commands.Undo();
    CHECK(wind->settings.speed == 1.0f);
    commands.Redo();
    CHECK(wind->settings.speed == 3.0f);
}

TEST_CASE("material creator: PBR/Unlit presets land in Materials/ with the right shader")
{
    draconic::materials::RegisterMaterialAsset();
    const StringView dir = u8"draconic_editor_mat_creator_test";
    auto scrub = [&]()
    {
        FileDelete(PathJoin(dir, u8"Project.xml"));
        FileDelete(PathJoin(dir, u8"Content/Materials/Material.xasset"));
        FileDelete(PathJoin(dir, u8"Content/Materials/Material.2.xasset"));
        RemoveDirectory(PathJoin(dir, u8"Content/Materials"));
        for (StringView sub : {u8"Content", u8"Sources", u8"Cooked", u8"Editor", u8".cache"})
        {
            RemoveDirectory(PathJoin(dir, sub));
        }
        RemoveDirectory(dir);
    };
    scrub();
    REQUIRE(draconic::editor::EditorProject::Create(dir, u8"P").IsOk());
    UniquePtr<draconic::editor::EditorProject> project = draconic::editor::EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));
    draconic::editor::EditorContext ctx;
    ctx.SetProject(project.Get());

    // PBR: the lit property set on the "forward" shader; lands in Materials/ (unique names).
    draconic::content::Instance* pbr = CreateMaterialInstance(ctx, nullptr, /*unlit*/ false);
    REQUIRE(pbr != nullptr);
    CHECK(pbr->Path() == u8"Materials/Material");
    {
        RefPtr<ISerializable> object = pbr->ReadObject();
        auto* asset = Cast<draconic::materials::MaterialAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->source.shaderName == u8"forward");
        bool hasMetallic = false;
        for (const String& n : asset->source.propNames)
        {
            if (n.AsView() == u8"Metallic")
            {
                hasMetallic = true;
            }
        }
        CHECK(hasMetallic);
    }

    // Unlit: BaseColor + AlbedoMap only, on the "unlit" shader.
    draconic::content::Instance* unlit = CreateMaterialInstance(ctx, nullptr, /*unlit*/ true);
    REQUIRE(unlit != nullptr);
    CHECK(unlit->Path() == u8"Materials/Material.2");
    {
        RefPtr<ISerializable> object = unlit->ReadObject();
        auto* asset = Cast<draconic::materials::MaterialAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->source.shaderName == u8"unlit");
        bool hasMetallic = false, hasBase = false;
        for (const String& n : asset->source.propNames)
        {
            if (n.AsView() == u8"Metallic")
            {
                hasMetallic = true;
            }
            if (n.AsView() == u8"BaseColor")
            {
                hasBase = true;
            }
        }
        CHECK(hasBase);
        CHECK_FALSE(hasMetallic);
    }

    project.Reset();
    scrub();
}

// --- Inspector property-attribute conventions ---

TEST_CASE("scene-edit: ParsePropertyCondition grammar")
{
    PropertyCondition c;

    // Truthy form.
    CHECK(ParsePropertyCondition(u8"castsShadows", c));
    CHECK(c.prop.AsView() == StringView(u8"castsShadows"));
    CHECK(c.values.IsEmpty());
    CHECK(MatchesPropertyCondition(c, 1));
    CHECK(!MatchesPropertyCondition(c, 0));

    // Value-list form.
    CHECK(ParsePropertyCondition(u8"type=1,2", c));
    CHECK(c.prop.AsView() == StringView(u8"type"));
    REQUIRE(c.values.Size() == 2u);
    CHECK(MatchesPropertyCondition(c, 1));
    CHECK(MatchesPropertyCondition(c, 2));
    CHECK(!MatchesPropertyCondition(c, 0));
    CHECK(!MatchesPropertyCondition(c, 3));

    CHECK(ParsePropertyCondition(u8"mode=-1", c));
    REQUIRE(c.values.Size() == 1u);
    CHECK(c.values[0] == -1);
    CHECK(MatchesPropertyCondition(c, -1));

    // Malformed specs refuse cleanly.
    CHECK(!ParsePropertyCondition(u8"", c));
    CHECK(!ParsePropertyCondition(u8"=1", c));
    CHECK(!ParsePropertyCondition(u8"type=", c));
    CHECK(!ParsePropertyCondition(u8"type=1,,2", c));
    CHECK(!ParsePropertyCondition(u8"type=x", c));
}

TEST_CASE("scene-edit: PrettifyPropertyName")
{
    CHECK(PrettifyPropertyName(u8"castsShadows").AsView() == StringView(u8"Casts Shadows"));
    CHECK(PrettifyPropertyName(u8"fovYRadians").AsView() == StringView(u8"Fov Y Radians"));
    CHECK(PrettifyPropertyName(u8"type").AsView() == StringView(u8"Type"));
    CHECK(PrettifyPropertyName(u8"skyIntensity").AsView() == StringView(u8"Sky Intensity"));
    CHECK(PrettifyPropertyName(u8"IBL").AsView() == StringView(u8"IBL"));
    CHECK(PrettifyPropertyName(u8"ReflectionProbe").AsView() == StringView(u8"Reflection Probe"));
}

TEST_CASE("scene-edit: render components carry the inspector attribute annotations")
{
    draconic::render::RegisterRenderComponentReflection();

    // Spot-only light angles hide behind the type condition; intensity gets a slider.
    const TypeInfo& light = TypeOf<draconic::render::LightComponent>();
    const PropertyInfo* inner = FindProperty(light, "innerAngle");
    REQUIRE(inner != nullptr);
    const draconic::foundation::Attribute* vis = FindAttribute(*inner, u8"visibleWhen");
    REQUIRE(vis != nullptr);
    PropertyCondition c;
    REQUIRE(ParsePropertyCondition(vis->value.TryGet<String>()->AsView(), c));
    CHECK(c.prop.AsView() == StringView(u8"type"));
    CHECK(MatchesPropertyCondition(c, 2));  // Spot
    CHECK(!MatchesPropertyCondition(c, 0)); // Directional

    const PropertyInfo* intensity = FindProperty(light, "intensity");
    REQUIRE(intensity != nullptr);
    const draconic::foundation::Attribute* range = FindAttribute(*intensity, u8"range");
    REQUIRE(range != nullptr);
    CHECK(range->value.TryGet<Float4>() != nullptr);

    // Environment: turbidity is Analytic-only with a bounded slider.
    const TypeInfo& env = TypeOf<draconic::render::EnvironmentSettings>();
    const PropertyInfo* turbidity = FindProperty(env, "turbidity");
    REQUIRE(turbidity != nullptr);
    CHECK(FindAttribute(*turbidity, u8"range") != nullptr);
    const draconic::foundation::Attribute* tvis = FindAttribute(*turbidity, u8"visibleWhen");
    REQUIRE(tvis != nullptr);
    REQUIRE(ParsePropertyCondition(tvis->value.TryGet<String>()->AsView(), c));
    CHECK(MatchesPropertyCondition(c, 1));  // Analytic
    CHECK(!MatchesPropertyCondition(c, 3)); // HDREquirect

    // Display-name override on the shared zenith/color slot.
    const PropertyInfo* zenith = FindProperty(env, "skyZenith");
    REQUIRE(zenith != nullptr);
    const draconic::foundation::Attribute* label = FindAttribute(*zenith, u8"displayName");
    REQUIRE(label != nullptr);
    CHECK(label->value.TryGet<String>()->AsView() == StringView(u8"Sky Zenith / Color"));
}

TEST_CASE("scene-edit: spawn prefab instance - undoable, redo recreates the SAME guids")
{
    // Author a template in a scratch scene and capture its payload.
    scene::Scene author(u8"author");
    HealthManager* authorHealth = author.AddSystem<HealthManager>();
    scene::EntityHandle root = author.CreateEntity(u8"Barrel");
    authorHealth->Add(root).amount = 12;
    MemoryStream payload;
    REQUIRE(scene::CapturePrefab(author, root, payload).IsOk());
    Array<byte> bytes;
    for (byte b : payload.Bytes())
    {
        bytes.PushBack(b);
    }

    scene::Scene scene(u8"level");
    HealthManager* health = scene.AddSystem<HealthManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    const Guid prefabId{0xAB, 0xCD};
    Array<byte> spawnBytes = bytes;
    const Guid rootId = edit.SpawnPrefabInstance(prefabId, Move(spawnBytes));
    REQUIRE(!rootId.IsNil());
    scene::EntityHandle live = edit.Resolve(rootId);
    REQUIRE(live.IsAssigned());
    CHECK(health->Has(live));
    CHECK(scene.PrefabInstanceCount() == 1u);

    commands.Undo();
    CHECK(!edit.Resolve(rootId).IsAssigned());
    CHECK(scene.PrefabInstanceCount() == 0u);

    commands.Redo();
    scene::EntityHandle back = edit.Resolve(rootId); // the SAME guid
    REQUIRE(back.IsAssigned());
    CHECK(health->Has(back));
    CHECK(scene.PrefabInstanceCount() == 1u);
}

TEST_CASE("scene-edit: replace entity with prefab instance is ONE undo step")
{
    scene::Scene author(u8"author");
    (void)author.AddSystem<HealthManager>();
    scene::EntityHandle tmpl = author.CreateEntity(u8"Crate");
    MemoryStream payload;
    REQUIRE(scene::CapturePrefab(author, tmpl, payload).IsOk());
    Array<byte> bytes;
    for (byte b : payload.Bytes())
    {
        bytes.PushBack(b);
    }

    scene::Scene scene(u8"level");
    (void)scene.AddSystem<HealthManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);

    const Guid parent = edit.CreateEntity(u8"Props");
    const Guid before = edit.CreateEntity(u8"Before", parent);
    const Guid original = edit.CreateEntity(u8"OldCrate", parent);
    const Guid after = edit.CreateEntity(u8"After", parent);
    {
        scene::EntityHandle h = edit.Resolve(original);
        draconic::foundation::Transform t = scene.GetLocalTransform(h);
        t.position = Float3{4, 5, 6};
        scene.SetLocalTransform(h, t);
    }

    const Guid prefabId{0x99, 0x11};
    const Guid instanceRoot = edit.ReplaceWithPrefabInstance(original, prefabId, Move(bytes));
    REQUIRE(!instanceRoot.IsNil());
    CHECK(!edit.Resolve(original).IsAssigned()); // original replaced
    scene::EntityHandle inst = edit.Resolve(instanceRoot);
    REQUIRE(inst.IsAssigned());
    CHECK(scene.GetParent(inst) == edit.Resolve(parent));                // same parent
    CHECK(Abs(scene.GetLocalTransform(inst).position.x - 4.0f) < 1e-4f); // same placement
    // Same SIBLING SLOT: Before -> instance -> After (spawn otherwise appends at the end).
    scene::EntityHandle first = scene.GetFirstChild(edit.Resolve(parent));
    CHECK(first == edit.Resolve(before));
    CHECK(scene.GetNextSibling(first) == inst);
    CHECK(scene.GetNextSibling(inst) == edit.Resolve(after));

    commands.Undo(); // ONE step: instance gone, original restored
    CHECK(!edit.Resolve(instanceRoot).IsAssigned());
    CHECK(edit.Resolve(original).IsAssigned());
    CHECK(scene.PrefabInstanceCount() == 0u);
}

TEST_CASE("scene-edit: revert component to prefab baseline is undoable")
{
    scene::Scene author(u8"author");
    HealthManager* authorHealth = author.AddSystem<HealthManager>();
    scene::EntityHandle tmpl = author.CreateEntity(u8"Guard");
    authorHealth->Add(tmpl).amount = 30;
    MemoryStream payload;
    REQUIRE(scene::CapturePrefab(author, tmpl, payload).IsOk());
    Array<byte> bytes;
    for (byte b : payload.Bytes())
    {
        bytes.PushBack(b);
    }

    scene::Scene scene(u8"level");
    HealthManager* health = scene.AddSystem<HealthManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    const Guid rootId = edit.SpawnPrefabInstance(Guid{0x5, 0x6}, Move(bytes));
    REQUIRE(!rootId.IsNil());
    scene::EntityHandle live = edit.Resolve(rootId);

    // Override, then revert (undoable), then undo the revert.
    health->Get(live)->amount = 31;
    const TypeInfo* type = scene.FindManagerBySerializationId(u8"test.health")->ComponentType();
    REQUIRE(edit.RevertComponentToBaseline(rootId, type));
    CHECK(health->Get(live)->amount == 30); // back to baseline
    commands.Undo();
    CHECK(health->Get(live)->amount == 31); // the override is restored

    // A user-ADDED component reverts by removal.
    scene::EntityHandle plain = scene.CreateEntity(u8"NotAMember");
    CHECK(!edit.RevertComponentToBaseline(scene.GetEntityId(plain), type)); // non-member no-op
}
