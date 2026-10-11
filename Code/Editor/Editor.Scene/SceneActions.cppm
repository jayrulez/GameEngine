// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene - :actions partition.
//
// The scene editor's actions, declared once (RegisterSceneEditorActions, from
// RegisterSceneEditor) and served everywhere from the registry: the Scene menu and the chords
// come from these declarations, the scene page's toolbar and the hierarchy's context menus
// execute through them, the palette and the MCP action bridge read them. Every action binds
// through the interface the subject page publishes (ServiceOf<ISceneEditorPage>): disabled on
// any other page, acting on THAT page's simulation, gizmo, markers or entity selection. The
// entity actions act on the selection's primary - what the hierarchy's right-click selected,
// what the viewport picked - so they are nullary like every action.
module;
#include "Core/Prelude.h"

export module editor.scene:actions;

import foundation.core;
import foundation.ui;
import foundation.scene;
import foundation.scene.resource;
import editor.core;
import :edit;
import :gizmo;
import :scene_page_interface;

using namespace foundation::core;
namespace scene = foundation::scene;

export namespace editor
{
    /// The ids, in one place: the toolbar and the hierarchy name what they show by these.
    namespace SceneActionIds
    {
        inline constexpr StringView kSimulateStart = u8"scene.simulate.start";
        inline constexpr StringView kSimulatePause = u8"scene.simulate.pause";
        inline constexpr StringView kSimulateStop = u8"scene.simulate.stop";
        inline constexpr StringView kGizmoTranslate = u8"scene.gizmo.translate";
        inline constexpr StringView kGizmoRotate = u8"scene.gizmo.rotate";
        inline constexpr StringView kGizmoScale = u8"scene.gizmo.scale";
        inline constexpr StringView kGizmoWorldSpace = u8"scene.gizmo.worldSpace";
        inline constexpr StringView kMarkers = u8"scene.view.markers";
        inline constexpr StringView kFrameSelection = u8"scene.view.frameSelection";
        inline constexpr StringView kAnimationPanel = u8"scene.view.animationPanel";
        inline constexpr StringView kSetProjectThumbnail = u8"scene.view.setProjectThumbnail";
        inline constexpr StringView kEntityCreate = u8"scene.entity.create";
        inline constexpr StringView kEntityCreateChild = u8"scene.entity.createChild";
        inline constexpr StringView kEntityDuplicate = u8"scene.entity.duplicate";
        inline constexpr StringView kEntityDelete = u8"scene.entity.delete";
        inline constexpr StringView kEntityCopy = u8"scene.entity.copy";
        inline constexpr StringView kEntityPaste = u8"scene.entity.paste";
        inline constexpr StringView kEntityPasteAsChild = u8"scene.entity.pasteAsChild";
        inline constexpr StringView kEntityCreatePrefab = u8"scene.entity.createPrefab";
        inline constexpr StringView kEntitySpawnPrefab = u8"scene.entity.spawnPrefab";
        inline constexpr StringView kEntitySpawnPrefabAsChild = u8"scene.entity.spawnPrefabAsChild";
        inline constexpr StringView kPrefabApply = u8"scene.prefab.apply";
        inline constexpr StringView kPrefabRevert = u8"scene.prefab.revert";
    }

    namespace detail
    {
        /// The scene page behind the subject, when it is one and is not simulating (edits are
        /// locked meanwhile); the entity actions bind through this.
        [[nodiscard]] inline ISceneEditorPage* EditablePage(EditorPage* page) noexcept
        {
            ISceneEditorPage* scene = ServiceOf<ISceneEditorPage>(page);
            return (scene != nullptr && !scene->IsSimulating()) ? scene : nullptr;
        }
        /// The primary selected entity of an editable page, nil when none.
        [[nodiscard]] inline Guid PrimaryOf(EditorPage* page) noexcept
        {
            ISceneEditorPage* scene = EditablePage(page);
            if (scene == nullptr)
            {
                return Guid{};
            }
            const Guid* primary = scene->EditContext().EntitySelection().Primary();
            return primary != nullptr ? *primary : Guid{};
        }
        /// The prefab instance root the primary selection belongs to, nil when it is no
        /// member of one (reachable from any member, acting on the whole owning instance).
        [[nodiscard]] inline Guid InstanceRootOf(EditorPage* page) noexcept
        {
            ISceneEditorPage* scene = EditablePage(page);
            const Guid primary = PrimaryOf(page);
            if (scene == nullptr || primary.IsNil())
            {
                return Guid{};
            }
            scene::PrefabMemberInfo member;
            return scene::FindPrefabMember(scene->EditContext().Scene(), primary, member)
                       ? member.state->rootEntityId
                       : Guid{};
        }
    }

    /// Declare the scene editor's actions on the context's registry. `context` is captured
    /// for the cross-page entity clipboard.
    inline void RegisterSceneEditorActions(EditorContext& context)
    {
        using namespace SceneActionIds;
        EditorActionRegistry& actions = context.Actions();
        EditorContext* ctx = &context;
        const auto Declare = [](StringView id, StringView label, StringView description,
                                StringView menuPath, i32 order)
        {
            EditorActionDeclaration d;
            d.id = String(id);
            d.label = String(label);
            d.description = String(description);
            d.menuPath = String(menuPath);
            d.menuOrder = order;
            return d;
        };
        const auto scenePage = [](EditorPage* page) { return ServiceOf<ISceneEditorPage>(page); };

        // --- Simulate: snapshot, run, restore ---
        {
            EditorActionDeclaration d = Declare(kSimulateStart, u8"Play",
                                                u8"Start the page's edit-mode Simulate from a "
                                                u8"snapshot (Stop restores it)",
                                                u8"Scene/Simulate/Play", 100);
            d.enabled = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                return scene != nullptr && !scene->IsSimulating();
            };
            d.execute = [scenePage](EditorPage* page) { scenePage(page)->StartSimulation(); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kSimulatePause, u8"Pause",
                                                u8"Hold the running Simulate; again resumes",
                                                u8"Scene/Simulate/Pause", 101);
            d.kind = EditorActionKind::Toggle;
            d.enabled = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                return scene != nullptr && scene->IsSimulating();
            };
            d.checked = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                return scene != nullptr && scene->IsPaused();
            };
            d.execute = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                scene->PauseSimulation(!scene->IsPaused());
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kSimulateStop, u8"Stop",
                                                u8"Stop the Simulate and restore the snapshot",
                                                u8"Scene/Simulate/Stop", 102);
            d.enabled = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                return scene != nullptr && scene->IsSimulating();
            };
            d.execute = [scenePage](EditorPage* page) { scenePage(page)->StopSimulation(); };
            (void)actions.Register(Move(d));
        }

        // --- Gizmo: the transform mode as three toggles, the space as one ---
        const auto gizmoMode = [&](StringView id, StringView label, StringView description,
                                   StringView menuPath, i32 order, GizmoMode mode,
                                   foundation::ui::KeyCode key)
        {
            EditorActionDeclaration d = Declare(id, label, description, menuPath, order);
            d.kind = EditorActionKind::Toggle;
            d.shortcut = EditorShortcut{key, foundation::ui::KeyModifiers::None};
            // Not while the camera flies: W, E and R are its movement keys then.
            d.enabled = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                return scene != nullptr && scene->Gizmos() != nullptr && !scene->CameraOwnsInput();
            };
            d.checked = [scenePage, mode](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                return scene != nullptr && scene->Gizmos() != nullptr &&
                       scene->Gizmos()->Mode() == mode;
            };
            d.execute = [scenePage, mode](EditorPage* page)
            { scenePage(page)->Gizmos()->SetMode(mode); };
            (void)actions.Register(Move(d));
        };
        gizmoMode(kGizmoTranslate, u8"Translate", u8"Move the selection with the gizmo",
                  u8"Scene/Gizmo/Translate", 100, GizmoMode::Translate, foundation::ui::KeyCode::W);
        gizmoMode(kGizmoRotate, u8"Rotate", u8"Rotate the selection with the gizmo",
                  u8"Scene/Gizmo/Rotate", 101, GizmoMode::Rotate, foundation::ui::KeyCode::E);
        gizmoMode(kGizmoScale, u8"Scale", u8"Scale the selection with the gizmo",
                  u8"Scene/Gizmo/Scale", 102, GizmoMode::Scale, foundation::ui::KeyCode::R);
        {
            EditorActionDeclaration d = Declare(kGizmoWorldSpace, u8"World Space",
                                                u8"The gizmo's axes in world space (off: the "
                                                u8"selection's local space)",
                                                u8"Scene/Gizmo/World Space", 200);
            d.kind = EditorActionKind::Toggle;
            d.enabled = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                return scene != nullptr && scene->Gizmos() != nullptr;
            };
            d.checked = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                return scene != nullptr && scene->Gizmos() != nullptr &&
                       scene->Gizmos()->Space() == GizmoSpace::World;
            };
            d.execute = [scenePage](EditorPage* page)
            {
                GizmoController* gizmos = scenePage(page)->Gizmos();
                gizmos->SetSpace(gizmos->Space() == GizmoSpace::World ? GizmoSpace::Local
                                                                       : GizmoSpace::World);
            };
            (void)actions.Register(Move(d));
        }

        // --- View ---
        {
            EditorActionDeclaration d = Declare(kFrameSelection, u8"Frame Selection",
                                                u8"Move the viewport camera so the selected "
                                                u8"entities fill the view, orbiting about them",
                                                u8"Scene/Frame Selection", 290);
            d.shortcut = EditorShortcut{foundation::ui::KeyCode::F, foundation::ui::KeyModifiers::None};
            d.readOnly = true; // the camera is the page's view, not the scene
            d.enabled = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                return scene != nullptr && scene->ViewportCamera() != nullptr &&
                       !scene->EditContext().EntitySelection().IsEmpty() && !scene->CameraOwnsInput();
            };
            d.execute = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                (void)scene->FrameEntities(scene->EditContext().EntitySelection().Items(), true);
            };
            (void)actions.Register(Move(d));
        }
        {
            // The launcher's picture of the project: this view, now (it is otherwise taken when
            // the default scene opens without one and when it is saved).
            EditorActionDeclaration d = Declare(kSetProjectThumbnail, u8"Set as Project Thumbnail",
                                                u8"Use this view as the picture the project launcher "
                                                u8"shows the project by",
                                                u8"Scene/Set as Project Thumbnail", 295);
            d.readOnly = true; // a picture of the scene, not an edit of it
            EditorContext* editor = &context;
            d.enabled = [scenePage, editor](EditorPage* page)
            { return scenePage(page) != nullptr && editor->Project() != nullptr; };
            d.execute = [scenePage, editor](EditorPage* page)
            {
                if (scenePage(page)->RequestProjectThumbnail().IsOk())
                {
                    editor->SetStatus(u8"The launcher will show this view for the project.");
                }
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kMarkers, u8"Entity Markers",
                                                u8"Show every entity's marker in the viewport, "
                                                u8"not only the selected ones",
                                                u8"Scene/Entity Markers", 300);
            d.kind = EditorActionKind::Toggle;
            d.enabled = [scenePage](EditorPage* page) { return scenePage(page) != nullptr; };
            d.checked = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                return scene != nullptr && scene->MarkersShown();
            };
            d.execute = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                scene->SetMarkersShown(!scene->MarkersShown());
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d =
                Declare(kAnimationPanel, u8"Animation",
                        u8"Show the property animation panel under the viewport",
                        u8"Scene/Animation Panel", 301);
            d.kind = EditorActionKind::Toggle;
            d.enabled = [scenePage](EditorPage* page) { return scenePage(page) != nullptr; };
            d.checked = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                return scene != nullptr && scene->AnimationPanelShown();
            };
            d.execute = [scenePage](EditorPage* page)
            {
                ISceneEditorPage* scene = scenePage(page);
                scene->SetAnimationPanelShown(!scene->AnimationPanelShown());
            };
            (void)actions.Register(Move(d));
        }

        // --- Entities, over the selection's primary; edits are locked while simulating ---
        {
            EditorActionDeclaration d = Declare(kEntityCreate, u8"Create Entity",
                                                u8"A new entity at the scene root",
                                                u8"Scene/Entity/Create Entity", 100);
            d.enabled = [](EditorPage* page) { return detail::EditablePage(page) != nullptr; };
            d.execute = [](EditorPage* page)
            { (void)detail::EditablePage(page)->EditContext().CreateEntity(u8"Entity"); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kEntityCreateChild, u8"Create Child",
                                                u8"A new entity under the selected one",
                                                u8"Scene/Entity/Create Child", 101);
            d.enabled = [](EditorPage* page) { return !detail::PrimaryOf(page).IsNil(); };
            d.execute = [](EditorPage* page)
            {
                (void)detail::EditablePage(page)->EditContext().CreateEntity(
                    u8"Entity", detail::PrimaryOf(page));
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kEntityDuplicate, u8"Duplicate",
                                                u8"Duplicate the selected entity's subtree "
                                                u8"beside it",
                                                u8"Scene/Entity/Duplicate", 200);
            d.shortcut = EditorShortcut{foundation::ui::KeyCode::D, foundation::ui::KeyModifiers::Ctrl};
            d.enabled = [](EditorPage* page) { return !detail::PrimaryOf(page).IsNil(); };
            d.execute = [](EditorPage* page)
            {
                (void)detail::EditablePage(page)->EditContext().DuplicateEntity(
                    detail::PrimaryOf(page));
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kEntityDelete, u8"Delete",
                                                u8"Delete the selected entity and its subtree",
                                                u8"Scene/Entity/Delete", 201);
            d.shortcut = EditorShortcut{foundation::ui::KeyCode::Delete, foundation::ui::KeyModifiers::None};
            d.enabled = [](EditorPage* page) { return !detail::PrimaryOf(page).IsNil(); };
            d.execute = [](EditorPage* page)
            { detail::EditablePage(page)->EditContext().DestroyEntity(detail::PrimaryOf(page)); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kEntityCopy, u8"Copy",
                                                u8"Copy the selected entity's subtree to the "
                                                u8"editor's clipboard",
                                                u8"Scene/Entity/Copy", 300);
            d.enabled = [](EditorPage* page) { return !detail::PrimaryOf(page).IsNil(); };
            d.execute = [ctx](EditorPage* page)
            {
                SceneEditContext& edit = detail::EditablePage(page)->EditContext();
                const Guid primary = detail::PrimaryOf(page);
                Array<byte> blob = edit.CopyEntity(primary);
                if (!blob.IsEmpty())
                {
                    const scene::EntityHandle handle = edit.Scene().FindEntity(primary);
                    const StringView name =
                        handle.IsAssigned() ? edit.Scene().GetEntityName(handle) : StringView{};
                    ctx->CopyToEditorClipboard(u8"entities", Move(blob),
                                               Format(u8"entity '{}'", name).AsView());
                }
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kEntityPaste, u8"Paste",
                                                u8"Paste the clipboard's entities at the scene "
                                                u8"root",
                                                u8"Scene/Entity/Paste", 301);
            d.enabled = [ctx](EditorPage* page)
            {
                return detail::EditablePage(page) != nullptr &&
                       !ctx->ClipboardData(u8"entities").IsEmpty();
            };
            d.execute = [ctx](EditorPage* page)
            {
                (void)detail::EditablePage(page)->EditContext().PasteEntities(
                    ctx->ClipboardData(u8"entities"));
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kEntityPasteAsChild, u8"Paste as Child",
                                                u8"Paste the clipboard's entities under the "
                                                u8"selected one",
                                                u8"Scene/Entity/Paste as Child", 302);
            d.enabled = [ctx](EditorPage* page)
            {
                return !detail::PrimaryOf(page).IsNil() &&
                       !ctx->ClipboardData(u8"entities").IsEmpty();
            };
            d.execute = [ctx](EditorPage* page)
            {
                (void)detail::EditablePage(page)->EditContext().PasteEntities(
                    ctx->ClipboardData(u8"entities"), detail::PrimaryOf(page));
            };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kEntityCreatePrefab, u8"Create Prefab from Selection",
                                                u8"A prefab asset from the selected entity's "
                                                u8"subtree, the subtree its first instance",
                                                u8"Scene/Entity/Create Prefab from Selection", 400);
            d.enabled = [](EditorPage* page) { return !detail::PrimaryOf(page).IsNil(); };
            d.execute = [](EditorPage* page)
            { detail::EditablePage(page)->CreatePrefabFromEntity(detail::PrimaryOf(page)); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kEntitySpawnPrefab, u8"Spawn Prefab...",
                                                u8"Pick a prefab and spawn an instance at the "
                                                u8"scene root",
                                                u8"Scene/Entity/Spawn Prefab...", 401);
            d.enabled = [](EditorPage* page) { return detail::EditablePage(page) != nullptr; };
            d.execute = [](EditorPage* page)
            { detail::EditablePage(page)->PickAndSpawnPrefab(Guid{}); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kEntitySpawnPrefabAsChild, u8"Spawn Prefab as Child",
                                                u8"Pick a prefab and spawn an instance under "
                                                u8"the selected entity",
                                                u8"Scene/Entity/Spawn Prefab as Child", 402);
            d.enabled = [](EditorPage* page) { return !detail::PrimaryOf(page).IsNil(); };
            d.execute = [](EditorPage* page)
            { detail::EditablePage(page)->PickAndSpawnPrefab(detail::PrimaryOf(page)); };
            (void)actions.Register(Move(d));
        }

        // --- Prefab instances, from any member of one ---
        {
            EditorActionDeclaration d = Declare(kPrefabApply, u8"Apply to Prefab",
                                                u8"Write the selected instance's changes back "
                                                u8"to its prefab asset",
                                                u8"Scene/Prefab/Apply to Prefab", 100);
            d.enabled = [](EditorPage* page) { return !detail::InstanceRootOf(page).IsNil(); };
            d.execute = [](EditorPage* page)
            { detail::EditablePage(page)->ApplyInstanceToPrefab(detail::InstanceRootOf(page)); };
            (void)actions.Register(Move(d));
        }
        {
            EditorActionDeclaration d = Declare(kPrefabRevert, u8"Revert Instance",
                                                u8"Discard the selected instance's changes, "
                                                u8"back to its prefab",
                                                u8"Scene/Prefab/Revert Instance", 101);
            d.enabled = [](EditorPage* page) { return !detail::InstanceRootOf(page).IsNil(); };
            d.execute = [](EditorPage* page)
            { detail::EditablePage(page)->RevertInstance(detail::InstanceRootOf(page)); };
            (void)actions.Register(Move(d));
        }
    }
}
