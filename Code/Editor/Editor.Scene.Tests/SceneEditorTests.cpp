// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// editor.scene headless tests: EditorCamera orientation math and the scene asset
// creator (unique naming, SceneDocument primary, project round-trip). The page itself needs a
// running host/renderer and is exercised in the editor app (on-screen path).

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.scene;
import foundation.scene.resource;
import foundation.resource;
import foundation.materials;
import pipeline.core;  // AssetCreatorRegistry
import scene.pipeline; // the Scene creator
import engine.render;
import foundation.ui;
import foundation.ui.toolkit;
import editor.app; // ContainerListEditor (the generic reflected-list row)
import editor.core;
import editor.scene;
import engine.animation;
import foundation.animation; // Skeleton (the bone picker's source)
import engine.script; // ScriptComponent: the behaviours section list
import engine.spline; // PathFollowComponent: an entity reference field
import engine.vegetation; // TerrainVegetationComponent: a reflected list of structs
import foundation.shell;
import foundation.settings;
import foundation.xml.serialization;

using namespace foundation::core;
using namespace editor;
namespace scene = foundation::scene;

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
    struct StubKeyboard final : foundation::shell::IKeyboard
    {
        [[nodiscard]] bool IsKeyDown(foundation::shell::KeyCode) const override { return false; }
        [[nodiscard]] bool IsKeyPressed(foundation::shell::KeyCode) const override { return false; }
        [[nodiscard]] bool IsKeyReleased(foundation::shell::KeyCode) const override { return false; }
        [[nodiscard]] foundation::shell::KeyModifiers Modifiers() const override { return {}; }
    };
    struct StubMouse final : foundation::shell::IMouse
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
        [[nodiscard]] bool IsButtonDown(foundation::shell::MouseButton) const override
        {
            return false;
        }
        [[nodiscard]] bool IsButtonPressed(foundation::shell::MouseButton) const override
        {
            return false;
        }
        [[nodiscard]] bool IsButtonReleased(foundation::shell::MouseButton) const override
        {
            return false;
        }
        [[nodiscard]] bool RelativeMode() const override { return false; }
        void SetRelativeMode(bool) override {}
        [[nodiscard]] bool CursorVisible() const override { return true; }
        void SetCursorVisible(bool) override {}
        void SetCursor(foundation::shell::CursorType) override {}
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

namespace
{
    // The Scene creator File > New runs (scene.pipeline), over a real project.
    foundation::content::Instance* CreateScene(EditorProject& project)
    {
        pipeline::AssetCreatorRegistry creators{DefaultAllocator()};
        pipeline::RegisterSceneCreators(creators);
        const pipeline::AssetCreator* scene = creators.FindByLabel(u8"Scene");
        REQUIRE(scene != nullptr);
        return scene->Create(nullptr, project.SourceDb().RootGroup(), project.SourcesRoot());
    }
}

TEST_CASE("editor-scene: the Scene creator makes uniquely-named SceneDocument instances")
{
    GlobalTypeRegistry().Register(foundation::scene::SceneDocument::StaticType());
    RegisterSerializable<foundation::scene::SceneDocument>();

    const StringView dir = u8"scratch_editor_scene_test_project";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    // No source database -> null.
    CHECK(pipeline::CreateSceneInstance(nullptr, u8"Scene", DefaultAllocator()) == nullptr);

    foundation::content::Instance* first = CreateScene(*project);
    REQUIRE(first != nullptr);
    CHECK(first->Name() == u8"Scene");
    CHECK(first->Path() == u8"Scenes/Scene");
    CHECK(first->TypeName() == u8"SceneDocument");

    // The primary object materialized and round-trips.
    RefPtr<ISerializable> obj = first->ReadObject();
    REQUIRE(obj.Get() != nullptr);
    auto* doc = Cast<foundation::scene::SceneDocument>(obj.Get());
    REQUIRE(doc != nullptr);
    CHECK(doc->name == u8"Scene");

    // Second create picks a unique name in the same group.
    foundation::content::Instance* second = CreateScene(*project);
    REQUIRE(second != nullptr);
    CHECK(second->Name() == u8"Scene.2");
    CHECK(second->Id() != first->Id());

    RemoveProjectTree(dir);
}

namespace
{
    class TestClipboard final : public foundation::ui::IClipboard
    {
    public:
        String stored;
        Status GetText(String& outText) override
        {
            outText = String(stored.AsView());
            return Status{};
        }
        Status SetText(StringView text) override
        {
            stored = String(text);
            return Status{};
        }
        [[nodiscard]] bool HasText() override { return !stored.IsEmpty(); }
    };
}

TEST_CASE("hierarchy: Copy ID puts the entity's persistent guid on the text clipboard")
{
    scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    auto hierarchyRef =
        foundation::core::MakeRef<SceneHierarchyView>(DefaultAllocator(), edit);
    const Guid entity = edit.CreateEntity(u8"Lantern");

    // A bare view (no UI context) has no clipboard: the action reports false, nothing crashes.
    CHECK_FALSE(hierarchyRef->CopyEntityId(entity));

    foundation::ui::UIContext ctx{DefaultAllocator()};
    auto root = foundation::core::MakeRef<foundation::ui::RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());
    TestClipboard clipboard;
    ctx.SetClipboard(&clipboard);
    root->AddView(hierarchyRef.Get());

    REQUIRE(hierarchyRef->CopyEntityId(entity));
    CHECK(clipboard.stored == Format(u8"{}", entity)); // the guid text, as the scene file spells it
    CHECK(clipboard.stored.Size() == 36u);
    CHECK_FALSE(hierarchyRef->CopyEntityId(Guid{})); // nil is never an id to copy

    // With the editor beside it, the copy says what it copied (Sedulous 714bfd5f).
    EditorContext editor{DefaultAllocator()};
    String notice;
    NoticeKind noticeKind = NoticeKind::Info;
    editor.OnNotice = [&](NoticeKind kind, StringView message)
    {
        noticeKind = kind;
        notice = String(message);
    };
    hierarchyRef->SetEditorContext(&editor);
    REQUIRE(hierarchyRef->CopyEntityId(entity));
    CHECK(noticeKind == NoticeKind::Success);
    CHECK(notice == u8"Copied entity ID");
    root->RemoveView(hierarchyRef.Get());
}

TEST_CASE("editor-context: a copy the user asks for says what it copied, or that it could not")
{
    EditorContext editor{DefaultAllocator()};
    String notice;
    NoticeKind noticeKind = NoticeKind::Info;
    editor.OnNotice = [&](NoticeKind kind, StringView message)
    {
        noticeKind = kind;
        notice = String(message);
    };
    TestClipboard clipboard;
    CHECK(editor.CopyText(&clipboard, u8"Content/Hero.xasset", u8"path"));
    CHECK(clipboard.stored == u8"Content/Hero.xasset");
    CHECK(noticeKind == NoticeKind::Success);
    CHECK(notice == u8"Copied path");

    // No clipboard (a headless view): a warning, not a silent no-op.
    CHECK_FALSE(editor.CopyText(nullptr, u8"x", u8"GUID"));
    CHECK(noticeKind == NoticeKind::Warning);
    CHECK(notice == u8"Could not copy GUID to the clipboard");

    // The typed editor clipboard: the slot holds the blob, and the toast names it.
    Array<byte> blob;
    blob.PushBack(byte{7});
    editor.CopyToEditorClipboard(u8"component", Move(blob), u8"component 'Light'");
    CHECK(editor.ClipboardKind() == u8"component");
    CHECK(editor.ClipboardData(u8"component").Size() == 1u);
    CHECK(notice == u8"Copied component 'Light'");
}

TEST_CASE("hierarchy: collapse state survives snapshot rebuilds")
{
    scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    auto hierarchyRef =
        foundation::core::MakeRef<SceneHierarchyView>(DefaultAllocator(), edit);
    SceneHierarchyView& hierarchy = *hierarchyRef;

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

TEST_CASE("hierarchy: a double-click frames the row's entity; it no longer starts a rename")
{
    scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    auto hierarchyRef = foundation::core::MakeRef<SceneHierarchyView>(DefaultAllocator(), edit);
    SceneHierarchyView& hierarchy = *hierarchyRef;
    const Guid table = edit.CreateEntity(u8"Table");
    hierarchy.Refresh();

    Array<Guid> framed{DefaultAllocator()};
    hierarchy.OnFrameEntity = [&framed](const Guid& id) { framed.PushBack(id); };
    foundation::ui::TreeView* tree = hierarchy.Tree()->InternalTreeView();

    // One click selects, and frames nothing.
    tree->OnItemClick.Invoke(foundation::ui::TreeView::ItemClickInfo{0, 1});
    CHECK(framed.IsEmpty());
    REQUIRE(edit.EntitySelection().Primary() != nullptr);
    CHECK(*edit.EntitySelection().Primary() == table);

    // A double-click selects and frames.
    tree->OnItemClick.Invoke(foundation::ui::TreeView::ItemClickInfo{0, 2});
    REQUIRE(framed.Size() == 1);
    CHECK(framed[0] == table);

    // The row leaves the double-click to the tree: renames are a slow click, F2 or the menu.
    RefPtr<foundation::ui::View> row = tree->TreeAdapter->CreateView(0);
    auto* label = Cast<foundation::ui::EditableLabel>(row.Get());
    REQUIRE(label != nullptr);
    CHECK_FALSE(label->DoubleClickToEdit.Value());
    CHECK(label->SlowClickToEdit.Value());
}

// Repro: switching the selected entity must rebuild the inspector for the NEW entity.
TEST_CASE("inspector: rebuilds when the selection switches entities")
{
    scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    EditorContext editor{DefaultAllocator()};
    auto inspectorRef =
        foundation::core::MakeRef<SceneInspectorView>(DefaultAllocator(), editor, edit);
    SceneInspectorView& inspector = *inspectorRef;

    const Guid a = edit.CreateEntity(u8"Alpha");
    const Guid b = edit.CreateEntity(u8"Beta");

    edit.EntitySelection().Set(a);
    inspector.Refresh();
    auto* nameEditor = foundation::core::Cast<foundation::ui::toolkit::StringEditor>(
        inspector.Grid()->GetProperty(u8"Name"));
    REQUIRE(nameEditor != nullptr);
    CHECK(nameEditor->Value() == u8"Alpha");

    edit.EntitySelection().Set(b);
    inspector.Refresh();
    nameEditor = foundation::core::Cast<foundation::ui::toolkit::StringEditor>(
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
    scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    auto hierarchyRef =
        foundation::core::MakeRef<SceneHierarchyView>(DefaultAllocator(), edit);
    SceneHierarchyView& hierarchy = *hierarchyRef;

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

    // Undo the reparent: still selected AND back in its exact old slot (before C). The
    // undo must not append to the end of the root list, which would visually teleport the row.
    commands.Undo();
    hierarchy.Refresh();
    CHECK(edit.EntitySelection().Contains(b));
    CHECK(selectedPos() == 0);
}

TEST_CASE("hierarchy: a selection-model change (keyboard nav) syncs the scene selection")
{
    scene::Scene scene{DefaultAllocator()};
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    auto hierarchyRef =
        foundation::core::MakeRef<SceneHierarchyView>(DefaultAllocator(), edit);
    SceneHierarchyView& hierarchy = *hierarchyRef;

    const Guid a = edit.CreateEntity(u8"A");
    const Guid b = edit.CreateEntity(u8"B");
    (void)edit.CreateEntity(u8"C");
    hierarchy.Refresh();

    // Arrow-key nav moves the ListView's SelectionModel via Selection.Select (NOT OnItemClick), so the
    // scene selection - which drives the inspector - must follow it. Rows: A=0, B=1, C=2.
    auto& listSel = hierarchy.Tree()->InternalTreeView()->InternalListView()->Selection;
    listSel.Select(1);
    CHECK(edit.EntitySelection().Contains(b));
    listSel.Select(0);
    CHECK(edit.EntitySelection().Contains(a));
}

TEST_CASE("scene-editor: a new scene instance is seeded with a directional Sun")
{
    // Fresh scenes must be LIT out of the box - an authored "Sun" entity with a shadow-casting
    // directional light (saved content, not editor magic), angled down so shading has direction.
    GlobalTypeRegistry().Register(foundation::scene::SceneDocument::StaticType());
    RegisterSerializable<foundation::scene::SceneDocument>();

    const StringView dir = u8"scratch_newscene_seed_project";
    RemoveProjectTree(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    foundation::content::Instance* instance = CreateScene(*project);
    REQUIRE(instance != nullptr);

    foundation::scene::Scene loaded{DefaultAllocator()};
    loaded.AddSystem<engine::render::LightComponentManager>();
    REQUIRE(foundation::scene::LoadScene(*instance, loaded).IsOk());

    foundation::scene::EntityHandle sun{};
    loaded.ForEachEntity(
        [&](foundation::scene::EntityHandle e)
        {
            if (loaded.GetEntityName(e) == u8"Sun")
            {
                sun = e;
            }
        });
    REQUIRE(sun.IsAssigned());
    auto* lights = loaded.GetSystem<engine::render::LightComponentManager>();
    engine::render::LightComponent* light = lights->Get(sun);
    REQUIRE(light != nullptr);
    CHECK(light->type == engine::render::LightType::Directional);
    CHECK(light->castsShadows);
    // Angled, not identity: the light's forward must have a downward component.
    const Transform t = loaded.GetLocalTransform(sun);
    const Float3 forward = RotateVector(t.rotation, Float3{0, 0, -1});
    CHECK(forward.y < -0.5f);

    RemoveProjectTree(dir);
}

TEST_CASE("scene-editor: the per-scene view state (grid + LOD) round-trips through the settings store")
{
    RegisterSceneViewSettingsType();
    const Guid sceneA{1, 2};
    const Guid sceneB{3, 4};

    foundation::settings::Settings store(foundation::core::DefaultAllocator());
    // No entry yet -> the caller's fallback (the page's current state) is returned untouched.
    const SceneViewPref fb{sceneA, true, false};
    CHECK(LoadSceneViewPref(&store, sceneA, fb).showGrid == true);
    CHECK(LoadSceneViewPref(&store, sceneA, fb).showLodOverlay == false);

    // Upsert two scenes with DIFFERENT grid + LOD state; nil guid / null store is a no-op.
    CHECK(SaveSceneViewPref(&store, SceneViewPref{sceneA, false, true}));
    CHECK(SaveSceneViewPref(&store, SceneViewPref{sceneB, true, false}));
    CHECK_FALSE(SaveSceneViewPref(&store, SceneViewPref{Guid{}, false, false}));
    CHECK_FALSE(SaveSceneViewPref(nullptr, SceneViewPref{sceneA, false, false}));

    // Read back: each scene keeps its OWN grid + LOD state (per-page), fallback ignored once saved.
    CHECK(LoadSceneViewPref(&store, sceneA, fb).showGrid == false);
    CHECK(LoadSceneViewPref(&store, sceneA, fb).showLodOverlay == true);
    CHECK(LoadSceneViewPref(&store, sceneB, fb).showGrid == true);
    CHECK(LoadSceneViewPref(&store, sceneB, fb).showLodOverlay == false);

    // Persist to XML (the file the app writes) and reload: the per-scene state survives.
    MemoryStream buf;
    REQUIRE(store.Save(buf, foundation::xml::XmlSerializerFactory()).IsOk());
    (void)buf.Seek(0, SeekOrigin::Begin);
    foundation::settings::Settings loaded(foundation::core::DefaultAllocator());
    REQUIRE(loaded.Load(buf, foundation::xml::XmlSerializerFactory()).IsOk());
    CHECK(LoadSceneViewPref(&loaded, sceneA, fb).showGrid == false);
    CHECK(LoadSceneViewPref(&loaded, sceneA, fb).showLodOverlay == true);
    CHECK(LoadSceneViewPref(&loaded, sceneB, fb).showGrid == true);

    // Toggling only ONE field updates in place (no duplicate row) and leaves the other intact.
    CHECK(SaveSceneViewPref(&loaded, SceneViewPref{sceneA, true, true})); // flip grid, keep LOD on
    CHECK(LoadSceneViewPref(&loaded, sceneA, fb).showGrid == true);
    CHECK(LoadSceneViewPref(&loaded, sceneA, fb).showLodOverlay == true);
    CHECK(loaded.Find<SceneViewSettings>()->prefs.Size() == 2u);

    // v3 field (showColliders): round-trips through XML per scene, and the versioned gate means a
    // default-false pref stays false after reload.
    CHECK(SaveSceneViewPref(&loaded, SceneViewPref{sceneA, true, true, true}));
    CHECK(SaveSceneViewPref(&loaded, SceneViewPref{sceneB, true, false, false}));
    MemoryStream buf3;
    REQUIRE(loaded.Save(buf3, foundation::xml::XmlSerializerFactory()).IsOk());
    (void)buf3.Seek(0, SeekOrigin::Begin);
    foundation::settings::Settings reloaded(foundation::core::DefaultAllocator());
    REQUIRE(reloaded.Load(buf3, foundation::xml::XmlSerializerFactory()).IsOk());
    CHECK(LoadSceneViewPref(&reloaded, sceneA, fb).showColliders == true);
    CHECK(LoadSceneViewPref(&reloaded, sceneB, fb).showColliders == false);
    CHECK(LoadSceneViewPref(&reloaded, sceneA, fb).showLodOverlay == true); // siblings intact

    // showMarkers (the entity origin crosses): defaults ON, and an OFF choice survives the XML
    // round trip per scene while its siblings stay put.
    CHECK(fb.showMarkers == true);
    CHECK(LoadSceneViewPref(&reloaded, sceneA, fb).showMarkers == true);
    CHECK(SaveSceneViewPref(&reloaded, SceneViewPref{sceneA, true, true, true, false}));
    MemoryStream buf4;
    REQUIRE(reloaded.Save(buf4, foundation::xml::XmlSerializerFactory()).IsOk());
    (void)buf4.Seek(0, SeekOrigin::Begin);
    foundation::settings::Settings again(foundation::core::DefaultAllocator());
    REQUIRE(again.Load(buf4, foundation::xml::XmlSerializerFactory()).IsOk());
    CHECK(LoadSceneViewPref(&again, sceneA, fb).showMarkers == false);
    CHECK(LoadSceneViewPref(&again, sceneA, fb).showColliders == true);
    CHECK(LoadSceneViewPref(&again, sceneB, fb).showMarkers == true); // untouched scene: default

    // showFps (the viewport frame-rate readout): defaults OFF and round-trips per scene.
    CHECK(fb.showFps == false);
    CHECK(SaveSceneViewPref(&again, SceneViewPref{sceneA, true, true, true, false, true}));
    MemoryStream buf5;
    REQUIRE(again.Save(buf5, foundation::xml::XmlSerializerFactory()).IsOk());
    (void)buf5.Seek(0, SeekOrigin::Begin);
    foundation::settings::Settings once(foundation::core::DefaultAllocator());
    REQUIRE(once.Load(buf5, foundation::xml::XmlSerializerFactory()).IsOk());
    CHECK(LoadSceneViewPref(&once, sceneA, fb).showFps == true);
    CHECK(LoadSceneViewPref(&once, sceneA, fb).showMarkers == false); // siblings intact
    CHECK(LoadSceneViewPref(&once, sceneB, fb).showFps == false);

    // v4 camera + selection: saved by a closing page, restored on reopen, per scene.
    {
        SceneViewPref view{sceneA, true, true, true, false, true};
        view.hasCamera = true;
        view.cameraPosition = Float3{1.5f, 2.5f, -3.0f};
        view.cameraYaw = 0.75f;
        view.cameraPitch = -0.25f;
        view.cameraFocusDistance = 8.0f;
        view.selection.PushBack(Guid{7, 7});
        view.selection.PushBack(Guid{8, 8});
        CHECK(SaveSceneViewPref(&once, view));
    }
    MemoryStream buf6;
    REQUIRE(once.Save(buf6, foundation::xml::XmlSerializerFactory()).IsOk());
    (void)buf6.Seek(0, SeekOrigin::Begin);
    foundation::settings::Settings twice(foundation::core::DefaultAllocator());
    REQUIRE(twice.Load(buf6, foundation::xml::XmlSerializerFactory()).IsOk());
    const SceneViewPref back = LoadSceneViewPref(&twice, sceneA, fb);
    CHECK(back.hasCamera);
    CHECK(back.cameraPosition.x == doctest::Approx(1.5f));
    CHECK(back.cameraPosition.z == doctest::Approx(-3.0f));
    CHECK(back.cameraYaw == doctest::Approx(0.75f));
    CHECK(back.cameraPitch == doctest::Approx(-0.25f));
    CHECK(back.cameraFocusDistance == doctest::Approx(8.0f));
    REQUIRE(back.selection.Size() == 2u);
    CHECK(back.selection[0] == Guid{7, 7});
    CHECK(back.selection[1] == Guid{8, 8});
    CHECK_FALSE(LoadSceneViewPref(&twice, sceneB, fb).hasCamera); // the other scene: never saved
    CHECK_FALSE(back.hasSplits); // that page saved no splits: the page's defaults stand

    // The page's split positions: saved by a closing page, restored on reopen, per scene.
    {
        SceneViewPref view = back;
        view.hasSplits = true;
        view.hierarchySplit = 0.31f;
        view.inspectorSplit = 0.64f;
        view.bottomDockSplit = 0.55f;
        view.gridPlane = 2; // YZ
        view.gridLines = true;
        CHECK(SaveSceneViewPref(&twice, view));
    }
    MemoryStream buf7;
    REQUIRE(twice.Save(buf7, foundation::xml::XmlSerializerFactory()).IsOk());
    (void)buf7.Seek(0, SeekOrigin::Begin);
    const String saved(StringView(reinterpret_cast<const utf8char*>(buf7.Bytes().Data()),
                                  buf7.Bytes().Size()));
    foundation::settings::Settings thrice(foundation::core::DefaultAllocator());
    REQUIRE(thrice.Load(buf7, foundation::xml::XmlSerializerFactory()).IsOk());
    const SceneViewPref splits = LoadSceneViewPref(&thrice, sceneA, fb);
    CHECK(splits.hasSplits);
    CHECK(splits.hierarchySplit == doctest::Approx(0.31f));
    CHECK(splits.inspectorSplit == doctest::Approx(0.64f));
    CHECK(splits.bottomDockSplit == doctest::Approx(0.55f));
    CHECK(splits.cameraYaw == doctest::Approx(0.75f)); // siblings intact
    CHECK(splits.gridPlane == 2u);                     // the grid's plane and style, per scene
    CHECK(splits.gridLines);
    CHECK(LoadSceneViewPref(&thrice, sceneB, fb).gridPlane == 0u);
    CHECK_FALSE(LoadSceneViewPref(&thrice, sceneB, fb).hasSplits);

    // A file saved before the split keys existed still loads: its prefs keep every other field,
    // and read no splits (the page keeps its defaults).
    String older;
    for (usize start = 0; start < saved.Size();)
    {
        usize end = start;
        while (end < saved.Size() && saved.AsView()[end] != u8'\n')
        {
            ++end;
        }
        const StringView line = saved.AsView().SubStr(start, end - start);
        if (!line.ContainsIgnoreCase(u8"split") && !line.ContainsIgnoreCase(u8"gridPlane") &&
            !line.ContainsIgnoreCase(u8"gridLines"))
        {
            older.Append(line);
            older.Append(u8"\n");
        }
        start = end + 1;
    }
    REQUIRE(older.Size() < saved.Size()); // the split keys were there, and are gone
    MemoryStream oldFile;
    (void)oldFile.Write(reinterpret_cast<const byte*>(older.Data()), older.Size());
    (void)oldFile.Seek(0, SeekOrigin::Begin);
    foundation::settings::Settings fromOld(foundation::core::DefaultAllocator());
    REQUIRE(fromOld.Load(oldFile, foundation::xml::XmlSerializerFactory()).IsOk());
    const SceneViewPref oldPref = LoadSceneViewPref(&fromOld, sceneA, fb);
    CHECK_FALSE(oldPref.hasSplits);
    CHECK(oldPref.gridPlane == 0u); // the ground, the shader grid
    CHECK_FALSE(oldPref.gridLines);
    CHECK(oldPref.hasCamera);
    CHECK(oldPref.cameraYaw == doctest::Approx(0.75f));
}

TEST_CASE("scene-editor: a v3 view pref (before markers / FPS / camera) reads through the "
          "legacy gate and a v4 one carries the new keys")
{
    RegisterSceneViewSettingsType();
    const TypeInfo& type = SceneViewSettings::StaticType(); // the versioned macro's TypeInfo
    CHECK(type.dataVersion == 4u);
    CHECK(type.minReadDataVersion == 3u);

    // A v3-shaped record: the pref body under a version-3 scope writes ONLY the v3 keys.
    MemoryStream v3;
    {
        BinarySerializer ar(v3, SerializeMode::Write);
        const SerializedDataVersion chain[] = {{type.id, 3u}};
        ar.PushVersionScope(chain, 1);
        SceneViewPref pref{Guid{1, 2}, false, true, true, false, true};
        pref.hasCamera = true; // must NOT reach the v3 record
        pref.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
    (void)v3.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer ar(v3, SerializeMode::Read);
        const SerializedDataVersion chain[] = {{type.id, 3u}};
        ar.PushVersionScope(chain, 1);
        SceneViewPref pref;
        pref.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
        CHECK(pref.scene == Guid{1, 2});
        CHECK(pref.showGrid == false);
        CHECK(pref.showLodOverlay == true);
        CHECK(pref.showColliders == true);
        CHECK(pref.showMarkers == true);   // the v4 defaults, not the writer's values
        CHECK(pref.showFps == false);
        CHECK_FALSE(pref.hasCamera);
        CHECK(pref.selection.IsEmpty());
    }

    // The same body under the current scope round-trips every key.
    MemoryStream v4;
    {
        BinarySerializer ar(v4, SerializeMode::Write);
        const SerializedDataVersion chain[] = {{type.id, 4u}};
        ar.PushVersionScope(chain, 1);
        SceneViewPref pref{Guid{1, 2}, false, true, true, false, true};
        pref.hasCamera = true;
        pref.cameraYaw = 0.5f;
        pref.selection.PushBack(Guid{9, 9});
        pref.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
    (void)v4.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer ar(v4, SerializeMode::Read);
        const SerializedDataVersion chain[] = {{type.id, 4u}};
        ar.PushVersionScope(chain, 1);
        SceneViewPref pref;
        pref.Serialize(ar);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
        CHECK(pref.showMarkers == false);
        CHECK(pref.showFps == true);
        CHECK(pref.hasCamera);
        CHECK(pref.cameraYaw == doctest::Approx(0.5f));
        REQUIRE(pref.selection.Size() == 1u);
        CHECK(pref.selection[0] == Guid{9, 9});
    }
}

TEST_CASE("scene-editor: the FPS overlay text reads the window's rate and mean frame time")
{
    CHECK(FrameRateOverlayText(0.0, 0) == u8"-- fps");
    CHECK(FrameRateOverlayText(0.5, 0) == u8"-- fps");
    CHECK(FrameRateOverlayText(0.5, 30) == u8"60 fps  16.7 ms");
    CHECK(FrameRateOverlayText(1.0, 144) == u8"144 fps  6.9 ms");
    CHECK(FrameRateOverlayText(0.5, 12) == u8"24 fps  41.7 ms");
}

TEST_CASE("inspector: an EntityRef list shows the referenced entities' NAMES")
{
    engine::animation::RegisterAnimationComponentReflection();
    scene::Scene scene{DefaultAllocator()};
    auto* anims = scene.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    EditorContext editor{DefaultAllocator()};
    auto inspectorRef =
        foundation::core::MakeRef<SceneInspectorView>(DefaultAllocator(), editor, edit);
    SceneInspectorView& inspector = *inspectorRef;

    const Guid rig = edit.CreateEntity(u8"Rig");
    const Guid body = edit.CreateEntity(u8"Body");
    engine::animation::SkeletalAnimationComponent& a =
        anims->Add(scene.FindEntity(rig));
    a.meshEntities.PushBack(foundation::scene::EntityRef{body});
    a.meshEntities.PushBack(foundation::scene::EntityRef{}); // unset slot
    a.meshEntities.PushBack(foundation::scene::EntityRef{Guid{1234, 5678}}); // dangling

    edit.EntitySelection().Set(rig);
    inspector.Refresh();
    auto* list = foundation::core::Cast<editor::app::ContainerListEditor>(
        inspector.Grid()->GetProperty(u8"Mesh Entities"));
    REQUIRE(list != nullptr);
    REQUIRE(list->slotNames.Size() == 3u);
    CHECK(list->slotNames[0] == u8"Body");      // named, never "<value>"
    CHECK(list->slotNames[1] == u8"None");      // nil ref
    CHECK(list->slotNames[2] == u8"(missing)"); // guid that resolves to no entity
}

TEST_CASE("inspector: a list of reflected structs gets a per-slot expander of leaf rows addressed by path")
{
    using engine::vegetation::TerrainVegetationComponent;
    using engine::vegetation::ProceduralVegetationLayer;
    engine::vegetation::RegisterVegetationComponentReflection();
    scene::Scene scene{DefaultAllocator()};
    engine::vegetation::AddVegetationSceneManagers(scene);
    auto* mgr = scene.GetSystem<engine::vegetation::TerrainVegetationComponentManager>();
    REQUIRE(mgr != nullptr);
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    EditorContext editor{DefaultAllocator()};
    auto inspectorRef =
        foundation::core::MakeRef<SceneInspectorView>(DefaultAllocator(), editor, edit);
    SceneInspectorView& inspector = *inspectorRef;

    const Guid terrain = edit.CreateEntity(u8"Terrain");
    TerrainVegetationComponent& c = mgr->Add(scene.FindEntity(terrain));
    ProceduralVegetationLayer grass;
    grass.name = String(u8"Grass");
    grass.density = 2.0f;
    c.proceduralLayers.PushBack(grass);
    c.proceduralLayers.PushBack(ProceduralVegetationLayer{}); // unnamed: falls back to the type label

    edit.EntitySelection().Set(terrain);
    inspector.Refresh();
    auto* list = foundation::core::Cast<editor::app::ContainerListEditor>(
        inspector.Grid()->GetProperty(u8"Procedural Layers"));
    REQUIRE(list != nullptr);
    REQUIRE(list->slotNames.Size() == 2u);
    CHECK(list->slotNames[0] == u8"Grass"); // the element's own name, not "<value>"
    CHECK(list->slotNames[1] != u8"Grass");

    // Every leaf field of each slot is a row in that slot's expander; the named slot titles
    // its expander with the name.
    const usize layerFields = Properties(TypeOf<ProceduralVegetationLayer>()).Size();
    usize slot1Rows = 0;
    usize slot2Rows = 0;
    bool slot1Density = false;
    bool slot1Mesh = false;
    for (usize i = 0; i < inspector.Grid()->PropertyCount(); ++i)
    {
        const foundation::ui::toolkit::PropertyEditor* row = inspector.Grid()->PropertyAt(i);
        if (row->Category() == u8"Procedural Layers 1: Grass")
        {
            ++slot1Rows;
            slot1Density |= row->Name() == u8"density";
            slot1Mesh |= row->Name() == u8"mesh";
        }
        else if (row->Category() == u8"Procedural Layers 2")
        {
            ++slot2Rows;
        }
    }
    CHECK(slot1Rows == layerFields);
    CHECK(slot2Rows == layerFields);
    CHECK(slot1Density);
    CHECK(slot1Mesh);
    // Each slot's expander sits inside the component's section, where the list's row is.
    CHECK(inspector.Grid()->CategoryParent(u8"Procedural Layers 1: Grass") == list->Category());
    CHECK(inspector.Grid()->CategoryParent(u8"Procedural Layers 2") == list->Category());

    // A list inside a slot (the layer's materials, one per mesh slot) is a list editor of its own
    // in that slot's expander, and edits reach that layer's list alone.
    editor::app::ContainerListEditor* slot1Materials = nullptr;
    for (usize i = 0; i < inspector.Grid()->PropertyCount(); ++i)
    {
        foundation::ui::toolkit::PropertyEditor* row = inspector.Grid()->PropertyAt(i);
        if (row->Category() == u8"Procedural Layers 1: Grass" && row->Name() == u8"Materials")
        {
            slot1Materials = foundation::core::Cast<editor::app::ContainerListEditor>(row);
        }
    }
    REQUIRE(slot1Materials != nullptr);
    CHECK(slot1Materials->slotNames.IsEmpty());
    REQUIRE(slot1Materials->OnAdd);
    slot1Materials->OnAdd();
    CHECK(c.proceduralLayers[0].materials.Size() == 1u);
    CHECK(c.proceduralLayers[1].materials.IsEmpty());

    // A path edit shows up on the next refresh through the same rows (the refresher pulls the
    // owner by path), and removing a slot rebuilds the grid without its expander.
    edit.SetComponentProperty(terrain, &TypeOf<TerrainVegetationComponent>(),
                              ComponentPropertyPath{"proceduralLayers", 0}, "density",
                              Variant::From<f32>(9.0f));
    CHECK(mgr->Get(scene.FindEntity(terrain))->proceduralLayers[0].density == 9.0f);
    mgr->Get(scene.FindEntity(terrain))->proceduralLayers.RemoveAt(1);
    inspector.Refresh(); // the list refresher sees the count change and forces a rebuild
    inspector.Refresh();
    bool slot2Present = false;
    for (usize i = 0; i < inspector.Grid()->PropertyCount(); ++i)
    {
        slot2Present |= inspector.Grid()->PropertyAt(i)->Category() == u8"Layers 2";
    }
    CHECK(!slot2Present);
}

// editor-lists-and-asset-slots P1 (Sedulous 71c65cf0): a generated asset list takes a dropped
// asset of its type. On a slot the drop is the slot's assignment, on the list it appends, each as
// ONE undo step; a wrong type changes nothing.
TEST_CASE("inspector: the materials list takes drops, each one undo step; a wrong type is refused")
{
    engine::render::RegisterRenderComponentReflection();
    scene::Scene scene{DefaultAllocator()};
    auto* meshes = scene.AddSystem<engine::render::MeshComponentManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    EditorContext editor{DefaultAllocator()};
    auto inspectorRef =
        foundation::core::MakeRef<SceneInspectorView>(DefaultAllocator(), editor, edit);
    SceneInspectorView& inspector = *inspectorRef;

    const Guid crate = edit.CreateEntity(u8"Crate");
    meshes->Add(scene.FindEntity(crate))
        .materials.PushBack(foundation::resource::Ref<foundation::materials::Material>{});
    edit.EntitySelection().Set(crate);
    inspector.Refresh();
    auto materials = [&]() -> auto& { return meshes->Get(scene.FindEntity(crate))->materials; };

    const Guid meshId{0x3333, 3};
    const Guid materialId{0x4444, 4};
    auto list = [&]()
    {
        return foundation::core::Cast<editor::app::ContainerListEditor>(
            inspector.Grid()->GetProperty(u8"Materials"));
    };
    REQUIRE(list() != nullptr);
    REQUIRE(list()->AcceptedTypes().Size() == 1u);
    auto* column = foundation::core::Cast<foundation::ui::ViewGroup>(list()->EditorView());
    auto* firstSlot = foundation::core::Cast<editor::app::AssetPickerSlot>(
        foundation::core::Cast<foundation::ui::ViewGroup>(column->GetChildAt(1))->GetChildAt(0));
    REQUIRE(firstSlot != nullptr);

    // A mesh is refused on the slot.
    auto mesh = MakeRef<editor::app::AssetDragData>(DefaultAllocator(), meshId,
                                                    StringView(u8"StaticMeshAsset"),
                                                    StringView(u8"crate"));
    const usize before = commands.Size();
    CHECK(firstSlot->OnDrop(mesh.Get(), 0, 0) == foundation::ui::DragDropEffects::None);
    CHECK(commands.Size() == before);
    CHECK(materials()[0].id.IsNil());

    // A material on the slot assigns it.
    auto material = MakeRef<editor::app::AssetDragData>(
        DefaultAllocator(), materialId, list()->AcceptedTypes()[0].AsView(), StringView(u8"red"));
    CHECK(firstSlot->OnDrop(material.Get(), 0, 0) == foundation::ui::DragDropEffects::Link);
    CHECK(materials()[0].id == materialId);
    CHECK(commands.Size() == before + 1);

    // A material on the list appends it.
    inspector.Refresh();
    inspector.Refresh();
    REQUIRE(list() != nullptr);
    foundation::ui::IDropTarget* zone = list()->EditorView()->AsDropTarget();
    REQUIRE(zone != nullptr);
    CHECK(zone->OnDrop(material.Get(), 0, 0) == foundation::ui::DragDropEffects::Link);
    REQUIRE(materials().Size() == 2u);
    CHECK(materials()[1].id == materialId);
    CHECK(commands.Size() == before + 2);

    // Undo takes back the append alone.
    commands.Undo();
    REQUIRE(materials().Size() == 1u);
    CHECK(materials()[0].id == materialId);
}

// editor-lists-and-asset-slots P2 (Sedulous 011b0db5): a script component's behaviours are a
// section list. The add icon and a dropped script class append, a section's move icon reorders,
// each one undo step, and after a move a section's rows edit the behaviour now in that place.
TEST_CASE("inspector: behaviours are sections whose rows follow a move")
{
    scene::Scene scene{DefaultAllocator()};
    auto* scripts = scene.AddSystem<engine::script::ScriptComponentManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    EditorContext editor{DefaultAllocator()};
    auto inspectorRef =
        foundation::core::MakeRef<SceneInspectorView>(DefaultAllocator(), editor, edit);
    SceneInspectorView& inspector = *inspectorRef;
    // A rebuild asked for by a refresher lands on the next refresh.
    auto settle = [&]()
    {
        inspector.Refresh();
        inspector.Refresh();
    };

    const Guid walker = edit.CreateEntity(u8"Walker");
    {
        engine::script::ScriptComponent& script = scripts->Add(scene.FindEntity(walker));
        engine::script::ScriptBehavior first;
        first.updateInterval = 1.0f;
        script.behaviors.PushBack(Move(first));
        engine::script::ScriptBehavior second;
        second.updateInterval = 2.0f;
        script.behaviors.PushBack(Move(second));
    }
    edit.EntitySelection().Set(walker);
    settle();
    auto live = [&]() -> engine::script::ScriptComponent&
    { return *scripts->Get(scene.FindEntity(walker)); };
    auto list = [&]()
    {
        return foundation::core::Cast<editor::app::ContainerListEditor>(
            inspector.Grid()->GetProperty(u8"Behaviors"));
    };
    REQUIRE(list() != nullptr);
    CHECK(list()->ElementsAsSections);
    CHECK(list()->slotNames.Size() == 2u);
    // Each behaviour's section sits inside the component's.
    const String firstSection =
        editor::SceneInspectorView::ScriptBehaviorSection(0, list()->slotNames[0].AsView());
    CHECK(inspector.Grid()->CategoryParent(firstSection.AsView()) == list()->Category());

    // The add icon: one behaviour, one undo step.
    const usize before = commands.Size();
    list()->OnAdd();
    CHECK(live().behaviors.Size() == 3u);
    CHECK(commands.Size() == before + 1);
    commands.Undo();
    CHECK(live().behaviors.Size() == 2u);
    settle();

    // A script class dropped on the list appends a behaviour running it; one undo reverts it.
    const Guid classId{0x5555, 5};
    auto dropped = MakeRef<editor::app::AssetDragData>(DefaultAllocator(), classId,
                                                       StringView(u8"ScriptClassAsset"),
                                                       StringView(u8"mover"));
    REQUIRE(list() != nullptr);
    foundation::ui::IDropTarget* zone = list()->EditorView()->AsDropTarget();
    REQUIRE(zone != nullptr);
    CHECK(zone->OnDrop(dropped.Get(), 0, 0) == foundation::ui::DragDropEffects::Link);
    REQUIRE(live().behaviors.Size() == 3u);
    CHECK(live().behaviors[2].script.id == classId);
    commands.Undo();
    CHECK(live().behaviors.Size() == 2u);
    settle();

    // The first section's move down swaps the two, one undo step.
    const String section = SceneInspectorView::ScriptBehaviorSection(0, u8"(none)");
    auto* actions = foundation::core::Cast<foundation::ui::ViewGroup>(
        inspector.Grid()->GetCategoryHeaderActions(section.AsView()));
    REQUIRE(actions != nullptr); // the section carries its element icons
    REQUIRE(actions->ChildCount() == 3u);
    foundation::core::Cast<foundation::ui::IconButton>(actions->GetChildAt(1))->FireClick();
    CHECK(live().behaviors[0].updateInterval == 2.0f);
    CHECK(live().behaviors[1].updateInterval == 1.0f);
    commands.Undo();
    CHECK(live().behaviors[0].updateInterval == 1.0f);
    commands.Redo();
    CHECK(live().behaviors[0].updateInterval == 2.0f);

    // After the rebuild, the first section's rows are the behaviour now first.
    settle();
    foundation::ui::toolkit::FloatEditor* interval = nullptr;
    for (usize i = 0; i < inspector.Grid()->PropertyCount(); ++i)
    {
        foundation::ui::toolkit::PropertyEditor* row = inspector.Grid()->PropertyAt(i);
        if (row->Name() == u8"Update Interval" && row->Category() == section.AsView())
        {
            interval = foundation::core::Cast<foundation::ui::toolkit::FloatEditor>(row);
        }
    }
    REQUIRE(interval != nullptr);
    CHECK(interval->Value() == 2.0);
    interval->Setter(5.0);
    CHECK(live().behaviors[0].updateInterval == 5.0f);
    CHECK(live().behaviors[1].updateInterval == 1.0f);
}

// editor-lists-and-asset-slots P3 (Sedulous 0fac1640): a hierarchy row dragged onto an entity
// slot assigns it, as one undo step; an asset, or a tree row naming nothing, is refused; an
// entity list takes one on a slot and appends one.
TEST_CASE("inspector: a hierarchy row drops on an entity slot")
{
    engine::spline::RegisterSplineComponentReflection();
    engine::animation::RegisterAnimationComponentReflection();
    scene::Scene scene{DefaultAllocator()};
    auto* followers = scene.AddSystem<engine::spline::PathFollowComponentManager>();
    auto* anims = scene.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    EditorContext editor{DefaultAllocator()};
    auto inspectorRef =
        foundation::core::MakeRef<SceneInspectorView>(DefaultAllocator(), editor, edit);
    SceneInspectorView& inspector = *inspectorRef;
    auto hierarchyRef = foundation::core::MakeRef<SceneHierarchyView>(DefaultAllocator(), edit);
    SceneHierarchyView& hierarchy = *hierarchyRef;

    const Guid cart = edit.CreateEntity(u8"Cart");
    const Guid track = edit.CreateEntity(u8"Track");
    followers->Add(scene.FindEntity(cart));
    hierarchy.Refresh();
    auto follower = [&]() -> engine::spline::PathFollowComponent&
    { return *followers->Get(scene.FindEntity(cart)); };

    // The hierarchy names the dragged row's entity: Track is the second root.
    auto drag = MakeRef<foundation::ui::toolkit::TreeDragData>(DefaultAllocator(), 1);
    hierarchy.DecorateDrag(*drag);
    CHECK(drag->ItemKind.AsView() == editor::app::AssetPickerSlot::EntityItemKind());
    CHECK(drag->ItemId == track);
    CHECK(drag->ItemName == u8"Track");

    edit.EntitySelection().Set(cart);
    inspector.Refresh();
    auto* row =
        foundation::core::Cast<editor::app::ResourceRefEditor>(inspector.Grid()->GetProperty(u8"spline"));
    REQUIRE(row != nullptr);
    auto* slot = foundation::core::Cast<editor::app::AssetPickerSlot>(row->EditorView());
    REQUIRE(slot != nullptr);

    // An asset is refused, and so is a row that names nothing.
    auto asset = MakeRef<editor::app::AssetDragData>(DefaultAllocator(), Guid{0x6666, 6},
                                                     StringView(u8"MaterialAsset"),
                                                     StringView(u8"red"));
    CHECK(slot->OnDrop(asset.Get(), 0, 0) == foundation::ui::DragDropEffects::None);
    auto bare = MakeRef<foundation::ui::toolkit::TreeDragData>(DefaultAllocator(), 0);
    CHECK(slot->CanAcceptDrop(bare.Get(), 0, 0) == foundation::ui::DragDropEffects::None);
    CHECK(follower().spline.id.IsNil());

    // The hierarchy row assigns, and one undo takes it back.
    CHECK(slot->OnDrop(drag.Get(), 0, 0) == foundation::ui::DragDropEffects::Link);
    CHECK(follower().spline.id == track);
    CHECK(row->ValueText() == StringView(u8"Track"));
    commands.Undo();
    CHECK(follower().spline.id.IsNil());

    // An entity list: a drop on its slot assigns, a drop on the list appends.
    anims->Add(scene.FindEntity(cart)).meshEntities.PushBack(foundation::scene::EntityRef{});
    inspector.Refresh();
    inspector.Refresh();
    auto list = [&]()
    {
        return foundation::core::Cast<editor::app::ContainerListEditor>(
            inspector.Grid()->GetProperty(u8"Mesh Entities"));
    };
    REQUIRE(list() != nullptr);
    auto* column = foundation::core::Cast<foundation::ui::ViewGroup>(list()->EditorView());
    auto* first = foundation::core::Cast<editor::app::AssetPickerSlot>(
        foundation::core::Cast<foundation::ui::ViewGroup>(column->GetChildAt(1))->GetChildAt(0));
    REQUIRE(first != nullptr);
    CHECK(first->OnDrop(drag.Get(), 0, 0) == foundation::ui::DragDropEffects::Link);
    CHECK(anims->Get(scene.FindEntity(cart))->meshEntities[0].id == track);
    inspector.Refresh();
    inspector.Refresh();
    REQUIRE(list() != nullptr);
    CHECK(list()->EditorView()->AsDropTarget()->OnDrop(drag.Get(), 0, 0) ==
          foundation::ui::DragDropEffects::Link);
    CHECK(anims->Get(scene.FindEntity(cart))->meshEntities.Size() == 2u);
}

// inverse-kinematics.md P4: a `boneName` field lists the bones of the animator above the entity,
// so a chain is picked, not typed; a name the skeleton lacks stays listed and says so.
TEST_CASE("inspector: a bone name field picks from the animator's skeleton")
{
    engine::animation::RegisterAnimationComponentReflection();
    scene::Scene scene{DefaultAllocator()};
    scene.AddSystem<engine::render::MeshComponentManager>();
    engine::animation::AddAnimationSceneManagers(scene);
    EditorCommandStack commands;
    SceneEditContext edit(scene, commands);
    EditorContext editor{DefaultAllocator()};
    auto inspectorRef = foundation::core::MakeRef<SceneInspectorView>(DefaultAllocator(), editor, edit);
    SceneInspectorView& inspector = *inspectorRef;

    RefPtr<foundation::animation::Skeleton> skeleton = MakeRef<foundation::animation::Skeleton>(DefaultAllocator(), 3);
    {
        Array<foundation::animation::Bone>& bones = skeleton->Bones();
        const char8_t* names[] = {u8"Thigh", u8"Shin", u8"Foot"};
        for (i32 i = 0; i < 3; ++i)
        {
            bones[static_cast<usize>(i)].index = i;
            bones[static_cast<usize>(i)].name = String(names[i]);
            bones[static_cast<usize>(i)].parentIndex = i - 1;
        }
        skeleton->BuildNameMap();
    }
    const Guid rider = edit.CreateEntity(u8"Rider");
    const Guid leg = edit.CreateEntity(u8"LegIk", rider);
    scene.GetSystem<engine::animation::SkeletalAnimationComponentManager>()
        ->Add(scene.FindEntity(rider))
        .skeleton.SetDirect(skeleton);
    auto* legs = scene.GetSystem<engine::animation::TwoBoneIkComponentManager>();
    legs->Add(scene.FindEntity(leg)).midBone = String(u8"Knee");
    auto live = [&]() -> engine::animation::TwoBoneIkComponent& { return *legs->Get(scene.FindEntity(leg)); };

    edit.EntitySelection().Set(leg);
    inspector.Refresh();
    auto* start = foundation::core::Cast<foundation::ui::toolkit::EnumEditor>(inspector.Grid()->GetProperty(u8"startBone"));
    REQUIRE(start != nullptr);
    REQUIRE(start->Items().Size() == 4u); // (none), Thigh, Shin, Foot
    CHECK(start->Items()[0] == u8"(none)");
    CHECK(start->Items()[2] == u8"Shin");
    CHECK(start->Value() == 0);
    start->Setter(1);
    CHECK(live().startBone == u8"Thigh");
    commands.Undo();
    CHECK(live().startBone.IsEmpty());

    auto* mid = foundation::core::Cast<foundation::ui::toolkit::EnumEditor>(inspector.Grid()->GetProperty(u8"midBone"));
    REQUIRE(mid != nullptr);
    REQUIRE(mid->Items().Size() == 5u);
    CHECK(mid->Items()[4] == u8"Knee (not in the skeleton)");
    CHECK(mid->Value() == 4);

    // With no animator above, the field is plain text.
    const Guid stray = edit.CreateEntity(u8"Stray");
    legs->Add(scene.FindEntity(stray));
    edit.EntitySelection().Set(stray);
    inspector.Refresh();
    CHECK(foundation::core::Cast<foundation::ui::toolkit::StringEditor>(inspector.Grid()->GetProperty(u8"startBone")) != nullptr);
}
