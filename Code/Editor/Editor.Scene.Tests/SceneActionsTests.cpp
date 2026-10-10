// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene tests - the scene editor's actions over a headless scene page beside a page
// that is not one: every action disabled off a scene page; simulate start / pause / stop with
// their enabled and checked states; the markers toggle; the gizmo toggles disabled without a
// gizmo; the entity actions over the selection's primary (create, create child, duplicate,
// delete, copy and paste through the editor's clipboard), locked while simulating; the prefab
// flows and the instance actions reaching the page; the hierarchy's menu items from the same
// declarations.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.ui;
import foundation.scene;
import editor.core;
import editor.scene;
import editor.camera;

using namespace foundation::core;
using namespace editor;
using namespace editor::SceneActionIds;
namespace scene = foundation::scene;
namespace ui = foundation::ui;

namespace
{
    class HeadlessScenePage final : public EditorPage, public ISceneEditorPage
    {
    public:
        explicit HeadlessScenePage(StringView title)
            : EditorPage(DefaultAllocator()), m_title(title),
              m_scene(DefaultAllocator(), u8"headless"), m_edit(m_scene, m_commands)
        {
            Provide<ISceneEditorPage>(*this);
        }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] Status Save() override { return Status{}; }
        [[nodiscard]] SceneEditContext& EditContext() noexcept override { return m_edit; }
        void StartSimulation() override
        {
            simulating = true;
            Commands().SetLocked(true);
        }
        void StopSimulation() override
        {
            simulating = false;
            paused = false;
            Commands().SetLocked(false);
        }
        void PauseSimulation(bool value) override { paused = value; }
        [[nodiscard]] bool IsSimulating() const noexcept override { return simulating; }
        [[nodiscard]] bool IsPaused() const noexcept override { return paused; }
        [[nodiscard]] GizmoController* Gizmos() noexcept override { return gizmos; }
        [[nodiscard]] bool CameraOwnsInput() const noexcept override { return cameraOwnsInput; }
        [[nodiscard]] EditorCamera* ViewportCamera() noexcept override { return hasViewport ? &camera : nullptr; }
        bool FrameEntities(Span<const Guid> entities, bool ease) override
        {
            framed.Clear();
            for (const Guid& id : entities)
            {
                framed.PushBack(id);
            }
            framedEased = ease;
            return !entities.IsEmpty();
        }
        [[nodiscard]] Status RequestViewportCapture(StringView) override { return Status{ErrorCode::NotSupported}; }
        [[nodiscard]] const ViewportCapture& LastViewportCapture() const noexcept override { return capture; }
        [[nodiscard]] bool MarkersShown() const noexcept override { return markers; }
        void SetMarkersShown(bool shown) override { markers = shown; }
        [[nodiscard]] bool AnimationPanelShown() const noexcept override { return animation; }
        void SetAnimationPanelShown(bool shown) override { animation = shown; }
        void CreatePrefabFromEntity(const Guid& entity) override { prefabFrom = entity; }
        void PickAndSpawnPrefab(const Guid& parent) override
        {
            spawnParent = parent;
            ++spawns;
        }
        void ApplyInstanceToPrefab(const Guid& root) override { applied = root; }
        void RevertInstance(const Guid& root) override { reverted = root; }

        bool simulating = false;
        bool paused = false;
        bool markers = true;
        bool animation = false;
        bool cameraOwnsInput = false;
        bool hasViewport = false;
        EditorCamera camera;
        Array<Guid> framed{DefaultAllocator()};
        bool framedEased = false;
        ViewportCapture capture;
        GizmoController* gizmos = nullptr;
        Guid prefabFrom;
        Guid spawnParent;
        u32 spawns = 0;
        Guid applied;
        Guid reverted;

    private:
        String m_title;
        scene::Scene m_scene;
        EditorCommandStack m_commands;
        SceneEditContext m_edit;
    };
    class PlainPage final : public EditorPage
    {
    public:
        PlainPage() : EditorPage(DefaultAllocator()) {}
        [[nodiscard]] StringView Title() const override { return u8"plain"; }
        [[nodiscard]] Status Save() override { return Status{}; }
    };
}

TEST_CASE("scene-actions: registered once, disabled off a scene page; simulate, markers and the "
          "gizmo toggles over the page's state")
{
    EditorContext context{DefaultAllocator()};
    RegisterSceneEditorActions(context);
    EditorActionRegistry& actions = context.Actions();
    REQUIRE(actions.Find(kSimulateStart) != nullptr);
    CHECK(actions.Find(kSimulateStart)->menuPath == u8"Scene/Simulate/Play");
    CHECK(actions.Find(kGizmoTranslate)->kind == EditorActionKind::Toggle);
    CHECK(actions.Shortcut(kGizmoTranslate).key == ui::KeyCode::W);
    CHECK(actions.Shortcut(kEntityDelete).key == ui::KeyCode::Delete);

    auto* plain = context.AdoptPage(
        UniquePtr<EditorPage>(DefaultAllocator().New<PlainPage>(), DefaultAllocator()));
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Bistro"), DefaultAllocator())));

    // Off a scene page (or with no subject) every scene action is disabled.
    for (const EditorActionDeclaration& action : actions.Actions())
    {
        CHECK_FALSE(actions.IsEnabled(action.id.AsView(), plain));
        CHECK_FALSE(actions.IsEnabled(action.id.AsView(), nullptr));
    }

    // Simulate: start enables pause and stop; pause is a toggle over the page's pause state.
    CHECK(actions.IsEnabled(kSimulateStart, page));
    CHECK_FALSE(actions.IsEnabled(kSimulatePause, page));
    CHECK_FALSE(actions.IsEnabled(kSimulateStop, page));
    CHECK(actions.Execute(kSimulateStart, page).IsOk());
    CHECK(page->simulating);
    CHECK_FALSE(actions.IsEnabled(kSimulateStart, page));
    CHECK(actions.IsEnabled(kSimulatePause, page));
    CHECK_FALSE(actions.IsChecked(kSimulatePause, page));
    CHECK(actions.Execute(kSimulatePause, page).IsOk());
    CHECK(page->paused);
    CHECK(actions.IsChecked(kSimulatePause, page));
    CHECK(actions.Execute(kSimulatePause, page).IsOk());
    CHECK_FALSE(page->paused);
    CHECK(actions.Execute(kSimulateStop, page).IsOk());
    CHECK_FALSE(page->simulating);
    CHECK(actions.Execute(kSimulateStop, page).Code() == ErrorCode::NotSupported);

    // Markers: a toggle over the page's flag.
    CHECK(actions.IsChecked(kMarkers, page));
    CHECK(actions.Execute(kMarkers, page).IsOk());
    CHECK_FALSE(page->markers);
    CHECK_FALSE(actions.IsChecked(kMarkers, page));

    // The animation panel: a toggle over the page's bottom panel, off to start.
    CHECK_FALSE(actions.IsChecked(kAnimationPanel, page));
    CHECK(actions.Execute(kAnimationPanel, page).IsOk());
    CHECK(page->animation);
    CHECK(actions.IsChecked(kAnimationPanel, page));
    CHECK(actions.Execute(kAnimationPanel, page).IsOk());
    CHECK_FALSE(page->animation);

    // Gizmo: disabled without a gizmo (a headless page); with one, the mode toggles are
    // exclusive and the space toggle flips.
    CHECK_FALSE(actions.IsEnabled(kGizmoTranslate, page));
    CHECK_FALSE(actions.IsEnabled(kGizmoWorldSpace, page));
    GizmoController gizmos(page->EditContext());
    page->gizmos = &gizmos;
    CHECK(actions.IsEnabled(kGizmoTranslate, page));
    CHECK(actions.Execute(kGizmoRotate, page).IsOk());
    CHECK(gizmos.Mode() == GizmoMode::Rotate);
    CHECK(actions.IsChecked(kGizmoRotate, page));
    CHECK_FALSE(actions.IsChecked(kGizmoTranslate, page));
    const bool worldBefore = gizmos.Space() == GizmoSpace::World;
    CHECK(actions.IsChecked(kGizmoWorldSpace, page) == worldBefore);
    CHECK(actions.Execute(kGizmoWorldSpace, page).IsOk());
    CHECK(actions.IsChecked(kGizmoWorldSpace, page) != worldBefore);
    // While the camera owns the input (a fly in progress) W/E/R are its keys: the mode
    // actions refuse and leave the mode alone; the space toggle is unaffected.
    page->cameraOwnsInput = true;
    CHECK_FALSE(actions.IsEnabled(kGizmoTranslate, page));
    CHECK(actions.Execute(kGizmoTranslate, page).Code() == ErrorCode::NotSupported);
    CHECK(gizmos.Mode() == GizmoMode::Rotate);
    CHECK(actions.IsChecked(kGizmoRotate, page)); // checked still reports the mode
    CHECK(actions.IsEnabled(kGizmoWorldSpace, page));
    page->cameraOwnsInput = false;
    CHECK(actions.IsEnabled(kGizmoTranslate, page));

    context.ClosePage(page);
    context.ClosePage(plain);
}

TEST_CASE("scene-actions: the entity actions act on the selection's primary, lock while "
          "simulating, go through the editor's clipboard, and reach the page's prefab flows; "
          "the hierarchy's menu items come from the same declarations")
{
    EditorContext context{DefaultAllocator()};
    RegisterSceneEditorActions(context);
    EditorActionRegistry& actions = context.Actions();
    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Bistro"), DefaultAllocator())));
    SceneEditContext& edit = page->EditContext();

    // No selection: create at the root is the one entity action on; the rest need a primary.
    CHECK(actions.IsEnabled(kEntityCreate, page));
    CHECK_FALSE(actions.IsEnabled(kEntityCreateChild, page));
    CHECK_FALSE(actions.IsEnabled(kEntityDuplicate, page));
    CHECK_FALSE(actions.IsEnabled(kEntityDelete, page));
    CHECK_FALSE(actions.IsEnabled(kEntityCopy, page));
    CHECK_FALSE(actions.IsEnabled(kEntityCreatePrefab, page));
    CHECK(actions.IsEnabled(kEntitySpawnPrefab, page));
    CHECK_FALSE(actions.IsEnabled(kEntitySpawnPrefabAsChild, page));
    CHECK_FALSE(actions.IsEnabled(kPrefabApply, page));
    CHECK(actions.Execute(kEntityCreate, page).IsOk());
    CHECK(edit.Scene().EntityCount() == 1u);
    // CreateEntity selects what it made: the primary is the new root.
    REQUIRE(edit.EntitySelection().Primary() != nullptr);
    const Guid root = *edit.EntitySelection().Primary();
    CHECK(actions.IsEnabled(kEntityCreateChild, page));
    CHECK(actions.Execute(kEntityCreateChild, page).IsOk());
    CHECK(edit.Scene().EntityCount() == 2u);
    const Guid child = *edit.EntitySelection().Primary();
    CHECK(edit.Scene().GetParent(edit.Resolve(child)) == edit.Resolve(root));

    // Duplicate the root's subtree beside it.
    edit.EntitySelection().Set(root);
    CHECK(actions.Execute(kEntityDuplicate, page).IsOk());
    CHECK(edit.Scene().EntityCount() == 4u);

    // Copy and paste ride the editor's clipboard: paste at root and as a child.
    CHECK_FALSE(actions.IsEnabled(kEntityPaste, page)); // nothing on the clipboard yet
    edit.EntitySelection().Set(child);
    CHECK(actions.Execute(kEntityCopy, page).IsOk());
    CHECK_FALSE(context.ClipboardData(u8"entities").IsEmpty());
    CHECK(actions.IsEnabled(kEntityPaste, page));
    CHECK(actions.Execute(kEntityPaste, page).IsOk());
    CHECK(edit.Scene().EntityCount() == 5u);
    edit.EntitySelection().Set(root);
    CHECK(actions.Execute(kEntityPasteAsChild, page).IsOk());
    CHECK(edit.Scene().EntityCount() == 6u);

    // The prefab flows reach the page with the primary (or its parent role). A paste selects
    // what it pasted, so the root is selected again first.
    edit.EntitySelection().Set(root);
    CHECK(actions.Execute(kEntityCreatePrefab, page).IsOk());
    CHECK(page->prefabFrom == root);
    CHECK(actions.Execute(kEntitySpawnPrefabAsChild, page).IsOk());
    CHECK(page->spawnParent == root);
    CHECK(actions.Execute(kEntitySpawnPrefab, page).IsOk());
    CHECK(page->spawnParent.IsNil());
    CHECK(page->spawns == 2u);
    // A plain entity is no prefab instance member: the instance actions stay off.
    CHECK_FALSE(actions.IsEnabled(kPrefabApply, page));
    CHECK_FALSE(actions.IsEnabled(kPrefabRevert, page));

    // Delete removes the primary's subtree.
    edit.EntitySelection().Set(root);
    CHECK(actions.Execute(kEntityDelete, page).IsOk());
    CHECK(edit.Scene().EntityCount() == 3u); // the root and its two children went

    // Simulating locks every edit: the entity actions are off, the simulate ones on.
    CHECK(actions.Execute(kSimulateStart, page).IsOk());
    CHECK_FALSE(actions.IsEnabled(kEntityCreate, page));
    CHECK_FALSE(actions.IsEnabled(kEntitySpawnPrefab, page));
    CHECK(actions.IsEnabled(kSimulateStop, page));
    CHECK(actions.Execute(kEntityCreate, page).Code() == ErrorCode::NotSupported);
    CHECK(actions.Execute(kSimulateStop, page).IsOk());
    CHECK(actions.IsEnabled(kEntityCreate, page));

    // The hierarchy builds its menu items from the same declarations over the page.
    auto menu = MakeRef<ui::ContextMenu>(DefaultAllocator());
    const StringView ids[] = {kEntityCreate, kEntityDelete, u8"nobody.home"};
    CHECK(AppendActionItems(*menu, actions, page, Span<const StringView>(ids, 3)) == 2u);
    REQUIRE(menu->ItemCount() == 2);
    CHECK(menu->ItemAt(0)->Label == u8"Create Entity");
    CHECK(menu->ItemAt(0)->Enabled);
    CHECK(menu->ItemAt(1)->Label == u8"Delete");
    CHECK_FALSE(menu->ItemAt(1)->Enabled); // nothing selected after the delete
    const usize before = edit.Scene().EntityCount();
    menu->ItemAt(0)->Action();
    CHECK(edit.Scene().EntityCount() == before + 1);

    context.ClosePage(page);
}

TEST_CASE("scene-actions: F frames the selection, eased, on a page with a viewport")
{
    EditorContext context{DefaultAllocator()};
    RegisterSceneEditorActions(context);
    EditorActionRegistry& actions = context.Actions();
    REQUIRE(actions.Find(kFrameSelection) != nullptr);
    CHECK(actions.Find(kFrameSelection)->menuPath == u8"Scene/Frame Selection");
    CHECK(actions.Shortcut(kFrameSelection).key == ui::KeyCode::F);
    CHECK(actions.Shortcut(kFrameSelection).modifiers == ui::KeyModifiers::None);

    auto* page = static_cast<HeadlessScenePage*>(context.AdoptPage(UniquePtr<EditorPage>(
        DefaultAllocator().New<HeadlessScenePage>(u8"Bistro"), DefaultAllocator())));

    // No viewport, or nothing selected: nothing to frame.
    CHECK_FALSE(actions.IsEnabled(kFrameSelection, page));
    page->hasViewport = true;
    CHECK_FALSE(actions.IsEnabled(kFrameSelection, page));

    Random rng(11);
    const Guid a = Guid::Generate(rng);
    const Guid b = Guid::Generate(rng);
    page->EditContext().EntitySelection().Set(a);
    page->EditContext().EntitySelection().Add(b);
    CHECK(actions.IsEnabled(kFrameSelection, page));

    // While the camera flies, its keys are its own.
    page->cameraOwnsInput = true;
    CHECK_FALSE(actions.IsEnabled(kFrameSelection, page));
    page->cameraOwnsInput = false;

    REQUIRE(actions.Execute(kFrameSelection, page).IsOk());
    REQUIRE(page->framed.Size() == 2);
    CHECK(page->framed[0] == a);
    CHECK(page->framed[1] == b);
    CHECK(page->framedEased);
    context.ClosePage(page);
}
