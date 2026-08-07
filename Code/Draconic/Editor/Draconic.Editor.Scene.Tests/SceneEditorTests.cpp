// draconic.editor.scene headless tests: EditorCamera orientation math and the scene asset
// creator (unique naming, SceneDocument primary, project round-trip). The page itself needs a
// running host/renderer and is exercised in the editor app (on-screen path).

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.content;
import draconic.scene;
import draconic.scene.resource;
import draconic.engine.render;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.editor.core;
import draconic.editor.scene;
import draconic.shell;

using namespace draconic::foundation;
using namespace draconic::editor;

namespace
{
    void RemoveProjectTree(StringView root)
    {
        FileDelete(PathJoin(root, u8"Project.xml"));
        FileDelete(PathJoin(root, u8"Content/Scenes/Scene.xasset"));
        FileDelete(PathJoin(root, u8"Content/Scenes/Scene.2.xasset"));
        RemoveDirectory(PathJoin(root, u8"Content/Scenes"));
        RemoveDirectory(PathJoin(root, u8"Content"));
        RemoveDirectory(PathJoin(root, u8"Sources"));
        RemoveDirectory(PathJoin(root, u8"Cooked"));
        RemoveDirectory(PathJoin(root, u8"Editor"));
        RemoveDirectory(PathJoin(root, u8".cache"));
        RemoveDirectory(root);
    }
}

TEST_CASE("editor-camera: orientation basis stays orthonormal under yaw/pitch")
{
    EditorCamera cam;
    cam.yaw = 0.7f;
    cam.pitch = -0.4f;

    const Float3 f = cam.Forward();
    const Float3 r = cam.Right();
    const Float3 u = cam.Up();

    CHECK(Dot(f, r) == doctest::Approx(0.0f).epsilon(0.001f));
    CHECK(Dot(f, u) == doctest::Approx(0.0f).epsilon(0.001f));
    CHECK(Dot(r, u) == doctest::Approx(0.0f).epsilon(0.001f));
    CHECK(Dot(f, f) == doctest::Approx(1.0f).epsilon(0.001f));

    // Defaults: looking mostly forward-down (-Z with a downward tilt).
    EditorCamera def;
    CHECK(def.Forward().z < 0.0f);
    CHECK(def.Forward().y < 0.0f);
}

TEST_CASE("editor-camera: LookAt aims forward at the target with a level horizon")
{
    EditorCamera cam;
    cam.position = Float3{6.0f, 5.0f, 10.0f};
    cam.LookAt(Float3{0.0f, 0.0f, 0.0f});

    // Forward matches the normalized direction to the target.
    const Float3 toTarget = Normalized(Float3{-6.0f, -5.0f, -10.0f});
    const Float3 f = cam.Forward();
    CHECK(f.x == doctest::Approx(toTarget.x).epsilon(0.001f));
    CHECK(f.y == doctest::Approx(toTarget.y).epsilon(0.001f));
    CHECK(f.z == doctest::Approx(toTarget.z).epsilon(0.001f));

    // No roll: the right vector stays in the ground plane (level horizon).
    CHECK(cam.Right().y == doctest::Approx(0.0f).epsilon(0.001f));

    // Orbit pivot moved to the target.
    CHECK(cam.focusDistance == doctest::Approx(12.688f).epsilon(0.001f));

    // The struct defaults agree with LookAt(origin) from the default position.
    EditorCamera def;
    CHECK(def.yaw == doctest::Approx(cam.yaw).epsilon(0.02f));
    CHECK(def.pitch == doctest::Approx(cam.pitch).epsilon(0.02f));

    // Degenerate target (== position) is a safe no-op.
    EditorCamera still;
    still.position = Float3{1.0f, 2.0f, 3.0f};
    const f32 yawBefore = still.yaw;
    still.LookAt(Float3{1.0f, 2.0f, 3.0f});
    CHECK(still.yaw == yawBefore);
}

namespace
{
    // Minimal input stubs: only what the camera's Update path reads.
    struct StubKeyboard final : draconic::shell::IKeyboard
    {
        [[nodiscard]] bool IsKeyDown(draconic::shell::KeyCode) const override { return false; }
        [[nodiscard]] bool IsKeyPressed(draconic::shell::KeyCode) const override { return false; }
        [[nodiscard]] bool IsKeyReleased(draconic::shell::KeyCode) const override { return false; }
        [[nodiscard]] draconic::shell::KeyModifiers Modifiers() const override { return {}; }
    };
    struct StubMouse final : draconic::shell::IMouse
    {
        f32 scrollY = 0.0f;
        [[nodiscard]] f32 X() const override { return 0; }
        [[nodiscard]] f32 Y() const override { return 0; }
        [[nodiscard]] f32 GlobalX() const override { return 0; }
        [[nodiscard]] f32 GlobalY() const override { return 0; }
        [[nodiscard]] f32 DeltaX() const override { return 0; }
        [[nodiscard]] f32 DeltaY() const override { return 0; }
        [[nodiscard]] f32 ScrollX() const override { return 0; }
        [[nodiscard]] f32 ScrollY() const override { return scrollY; }
        [[nodiscard]] bool IsButtonDown(draconic::shell::MouseButton) const override
        {
            return false;
        }
        [[nodiscard]] bool IsButtonPressed(draconic::shell::MouseButton) const override
        {
            return false;
        }
        [[nodiscard]] bool IsButtonReleased(draconic::shell::MouseButton) const override
        {
            return false;
        }
        [[nodiscard]] bool RelativeMode() const override { return false; }
        void SetRelativeMode(bool) override {}
        [[nodiscard]] bool CursorVisible() const override { return true; }
        void SetCursorVisible(bool) override {}
        void SetCursor(draconic::shell::CursorType) override {}
        void SetGlobalCapture(bool) override {}
    };
}

TEST_CASE("editor-camera: wheel dolly keeps the orbit pivot fixed at any zoom")
{
    EditorCamera cam;
    cam.position = Float3{0.0f, 0.0f, 5.0f};
    cam.LookAt(Float3{0.0f, 0.0f, 0.0f}); // pivot = origin, focusDistance = 5

    StubKeyboard kb;
    StubMouse mouse;
    mouse.scrollY = 1.0f; // one notch in

    // Zoom far past where the old fixed-step dolly (1.5/notch, floor at 1.0) would
    // have started dragging the pivot: the pivot must stay at the ORIGIN, so
    // position + Forward()*focusDistance == 0 every step, and the camera never
    // crosses to the far side.
    for (int i = 0; i < 40; ++i)
    {
        cam.Update(&kb, &mouse, 1.0f / 60.0f);
        const Float3 pivot = cam.position + cam.Forward() * cam.focusDistance;
        CHECK(Length(pivot) == doctest::Approx(0.0f).epsilon(0.001f));
        CHECK(cam.position.z > 0.0f); // still on the near side
    }
    CHECK(cam.focusDistance < 1.0f);      // allowed closer than the old 1.0 floor
    CHECK(cam.focusDistance >= 0.05f);    // but never through the pivot

    // Zooming back out retreats along the same axis, pivot still fixed.
    mouse.scrollY = -1.0f;
    for (int i = 0; i < 40; ++i)
    {
        cam.Update(&kb, &mouse, 1.0f / 60.0f);
    }
    const Float3 pivot = cam.position + cam.Forward() * cam.focusDistance;
    CHECK(Length(pivot) == doctest::Approx(0.0f).epsilon(0.001f));
    CHECK(cam.focusDistance > 4.0f); // roughly back out (exponential retreat)
}

TEST_CASE("editor-scene: CreateSceneInstance makes uniquely-named SceneDocument instances")
{
    GlobalTypeRegistry().Register(draconic::scene::SceneDocument::StaticType());
    RegisterSerializable<draconic::scene::SceneDocument>();

    const StringView dir = u8"draconic_editor_scene_test_project";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));

    EditorContext ctx;
    ctx.SetProject(project.Get());

    // No project -> null.
    EditorContext empty;
    CHECK(CreateSceneInstance(empty) == nullptr);

    draconic::content::Instance* first = CreateSceneInstance(ctx);
    REQUIRE(first != nullptr);
    CHECK(first->Name() == u8"Scene");
    CHECK(first->Path() == u8"Scenes/Scene");
    CHECK(first->TypeName() == u8"SceneDocument");

    // The primary object materialized and round-trips.
    RefPtr<ISerializable> obj = first->ReadObject();
    REQUIRE(obj.Get() != nullptr);
    auto* doc = Cast<draconic::scene::SceneDocument>(obj.Get());
    REQUIRE(doc != nullptr);
    CHECK(doc->name == u8"Scene");

    // Second create picks a unique name in the same group.
    draconic::content::Instance* second = CreateSceneInstance(ctx);
    REQUIRE(second != nullptr);
    CHECK(second->Name() == u8"Scene.2");
    CHECK(second->Id() != first->Id());

    RemoveProjectTree(dir);
}

TEST_CASE("hierarchy: collapse state survives snapshot rebuilds")
{
    scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    SceneHierarchyView hierarchy(edit);

    const Guid parent = edit.CreateEntity(u8"Parent");
    (void)edit.CreateEntity(u8"Child", parent);
    (void)edit.CreateEntity(u8"Sibling");
    hierarchy.Refresh();

    auto* flat = hierarchy.Tree()->InternalTreeView()->FlatAdapter();
    REQUIRE(flat != nullptr);
    CHECK(flat->ItemCount() == 3); // Parent (expanded) + Child + Sibling

    // Collapse Parent (pre-order nodeId 0), then force a rebuild by editing the scene.
    flat->Collapse(0);
    CHECK(flat->ItemCount() == 2);
    (void)edit.CreateEntity(u8"Another");
    hierarchy.Refresh();

    // SetAdapter recreated the flat view; Parent must STAY collapsed, new root visible.
    flat = hierarchy.Tree()->InternalTreeView()->FlatAdapter();
    CHECK(flat->ItemCount() == 3); // Parent (collapsed) + Sibling + Another
    CHECK_FALSE(flat->IsExpanded(0));

    // Re-expanding sticks across the next rebuild too.
    flat->Expand(0);
    (void)edit.CreateEntity(u8"YetAnother");
    hierarchy.Refresh();
    flat = hierarchy.Tree()->InternalTreeView()->FlatAdapter();
    CHECK(flat->ItemCount() == 5);
    CHECK(flat->IsExpanded(0));
}

// Repro: switching the selected entity must rebuild the inspector for the NEW entity.
TEST_CASE("inspector: rebuilds when the selection switches entities")
{
    scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    EditorContext editor;
    SceneInspectorView inspector(editor, edit);

    const Guid a = edit.CreateEntity(u8"Alpha");
    const Guid b = edit.CreateEntity(u8"Beta");

    edit.EntitySelection().Set(a);
    inspector.Refresh();
    auto* nameEditor = draconic::foundation::Cast<draconic::ui::toolkit::StringEditor>(
        inspector.Grid()->GetProperty(u8"Name"));
    REQUIRE(nameEditor != nullptr);
    CHECK(nameEditor->Value() == u8"Alpha");

    edit.EntitySelection().Set(b);
    inspector.Refresh();
    nameEditor = draconic::foundation::Cast<draconic::ui::toolkit::StringEditor>(
        inspector.Grid()->GetProperty(u8"Name"));
    REQUIRE(nameEditor != nullptr);
    CHECK(nameEditor->Value() == u8"Beta");

    // Clearing the selection empties the grid.
    edit.EntitySelection().Clear();
    inspector.Refresh();
    CHECK(inspector.Grid()->PropertyCount() == 0);
}

// Regression (user-reported): the hierarchy rebuild (any scene-revision change) must keep the
// selected entity's row selected - identity is the persistent Guid in EntitySelection.
// (CreateEntity intentionally MOVES the selection to the new entity; every other rebuild
// trigger must preserve it.)
TEST_CASE("hierarchy: selection survives snapshot rebuilds")
{
    scene::Scene scene;
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    SceneHierarchyView hierarchy(edit);

    const Guid a = edit.CreateEntity(u8"A");
    const Guid b = edit.CreateEntity(u8"B");
    const Guid c = edit.CreateEntity(u8"C");
    hierarchy.Refresh();

    auto selectedPos = [&]()
    { return hierarchy.Tree()->InternalTreeView()->InternalListView()->Selection.FirstSelected(); };

    edit.EntitySelection().Set(b);
    REQUIRE(selectedPos() == 1); // rows: A=0, B=1, C=2

    // Rename another entity (revision bump).
    edit.RenameEntity(a, u8"A2");
    hierarchy.Refresh();
    CHECK(edit.EntitySelection().Contains(b));
    CHECK(selectedPos() == 1);

    // Toggle another entity's active flag.
    edit.SetEntityActive(c, false);
    hierarchy.Refresh();
    CHECK(selectedPos() == 1);

    // Destroy ANOTHER entity: B stays selected at its shifted position.
    edit.DestroyEntity(a);
    hierarchy.Refresh();
    CHECK(edit.EntitySelection().Contains(b));
    CHECK(selectedPos() == 0); // rows now: B=0, C=1

    // Reparent B under C (keep-world): B stays selected at its new position in the tree.
    edit.ReparentEntity(b, c);
    hierarchy.Refresh();
    CHECK(edit.EntitySelection().Contains(b));
    CHECK(selectedPos() == 1); // rows: C=0, B (child)=1

    // Undo the reparent: still selected AND back in its exact old slot (before C) - the
    // undo used to append to the end of the root list, visually teleporting the row.
    commands.Undo();
    hierarchy.Refresh();
    CHECK(edit.EntitySelection().Contains(b));
    CHECK(selectedPos() == 0);
}

TEST_CASE("scene-editor: a new scene instance is seeded with a directional Sun")
{
    // Fresh scenes must be LIT out of the box - an authored "Sun" entity with a shadow-casting
    // directional light (saved content, not editor magic), angled down so shading has direction.
    GlobalTypeRegistry().Register(draconic::scene::SceneDocument::StaticType());
    RegisterSerializable<draconic::scene::SceneDocument>();

    const StringView dir = u8"draconic_newscene_seed_project";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));
    EditorContext ctx;
    ctx.SetProject(project.Get());

    draconic::content::Instance* instance = CreateSceneInstance(ctx);
    REQUIRE(instance != nullptr);

    draconic::scene::Scene loaded;
    loaded.AddSystem<draconic::render::LightComponentManager>();
    REQUIRE(draconic::scene::LoadScene(*instance, loaded).IsOk());

    draconic::scene::EntityHandle sun{};
    loaded.ForEachEntity(
        [&](draconic::scene::EntityHandle e)
        {
            if (loaded.GetEntityName(e) == u8"Sun")
            {
                sun = e;
            }
        });
    REQUIRE(sun.IsAssigned());
    auto* lights = loaded.GetSystem<draconic::render::LightComponentManager>();
    draconic::render::LightComponent* light = lights->Get(sun);
    REQUIRE(light != nullptr);
    CHECK(light->type == draconic::render::LightType::Directional);
    CHECK(light->castsShadows);
    // Angled, not identity: the light's forward must have a downward component.
    const Transform t = loaded.GetLocalTransform(sun);
    const Float3 forward = RotateVector(t.rotation, Float3{0, 0, -1});
    CHECK(forward.y < -0.5f);

    RemoveProjectTree(dir);
}
