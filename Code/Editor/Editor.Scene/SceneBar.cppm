// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_bar partition.
//
// The scene page's bar (Documentation/Specs/editor-gui.md, 5.3, layer 5, part 1): Save, Undo and
// Redo; Select, Move, Rotate and Scale as one choice; the space, its icon the space it is in;
// Snap, every drag snapping while it is on, with its increments in its menu (they had no UI:
// Ctrl snapped by fixed steps); Vertex Snap; Simulate, Pause and Stop, the state named beside
// them while it runs (layer 7, part 1: Simulate is named so, Play is the Game tab's); Frame; the
// property animation panel shown or not (the old bar's Animation, spec 5.6 gap 10). Each button is an action over the page
// (PageActionBar), so its tooltip says its keys and it is disabled where the action is (the
// transform choice while the camera owns the keys). The view's own controls are the HUD's chips,
// not the bar's (ruling 22).

module;
#include "Core/Prelude.h"

export module editor.scene:scene_bar;

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import :gizmo;
import :actions;
import :scene_page_interface;

using namespace foundation::core;

export namespace editor
{
    class SceneBar
    {
    public:
        /// An increment the snap menu offers; 0 for a move: the grid's cell.
        struct Increment
        {
            f32 value = 0.0f;
            const char8_t* label = nullptr;
        };
        static constexpr Increment kMoveIncrements[] = {
            {0.1f, u8"0.1 m"}, {0.25f, u8"0.25 m"}, {0.5f, u8"0.5 m"}, {1.0f, u8"1 m"}, {2.0f, u8"2 m"}, {0.0f, u8"The grid's cell"},
        };
        static constexpr Increment kRotateIncrements[] = {
            {5.0f, u8"5°"}, {15.0f, u8"15°"}, {45.0f, u8"45°"}, {90.0f, u8"90°"},
        };
        static constexpr Increment kScaleIncrements[] = {
            {0.1f, u8"0.1"}, {0.25f, u8"0.25"}, {0.5f, u8"0.5"},
        };
        enum class SnapKind : u8
        {
            Move,
            Rotate,
            Scale,
        };

        /// The bar's buttons, by their markup ids (`scene-save` ... `scene-frame`), found by
        /// `find`; `state` the label naming Simulate's state (null: none); `gizmos` the page's
        /// controller, asked each time (the tool may come later).
        SceneBar(IAllocator& allocator, EditorActionRegistry& actions, EditorPage& page, const Function<foundation::gui::ButtonBase*(StringView)>& find,
                 foundation::gui::Label* state, Function<GizmoController*()> gizmos)
            : m_bar(allocator, actions, page), m_page(&page), m_state(state), m_gizmos(Move(gizmos)), m_moveItems(allocator), m_rotateItems(allocator),
              m_scaleItems(allocator)
        {
            using namespace SceneActionIds;
            m_bar.Bind(find(u8"scene-save"), u8"file.save");
            m_bar.Bind(find(u8"scene-undo"), u8"edit.undo");
            m_bar.Bind(find(u8"scene-redo"), u8"edit.redo");
            m_bar.Bind(find(u8"scene-discard"), u8"page.discardChanges");
            m_bar.Bind(find(u8"scene-select"), kGizmoSelect);
            m_bar.Bind(find(u8"scene-move"), kGizmoTranslate);
            m_bar.Bind(find(u8"scene-rotate"), kGizmoRotate);
            m_bar.Bind(find(u8"scene-scale"), kGizmoScale);
            m_space = find(u8"scene-space");
            m_bar.Bind(m_space, kGizmoWorldSpace);
            m_bar.Bind(find(u8"scene-snap"), kGizmoSnap);
            m_bar.Bind(find(u8"scene-vertex-snap"), kGizmoVertexSnap);
            m_bar.Bind(find(u8"scene-simulate"), kSimulateStart);
            m_bar.Bind(find(u8"scene-pause"), kSimulatePause);
            m_bar.Bind(find(u8"scene-stop"), kSimulateStop);
            m_bar.Bind(find(u8"scene-frame"), kFrameSelection);
            m_bar.Bind(find(u8"scene-animation-panel"), kAnimationPanel);
            if (auto* snap = Cast<foundation::gui::ToolMenuButton>(find(u8"scene-snap")))
                BuildSnapMenu(snap->GetMenu());
            SyncState(); // hidden until it runs
        }
        SceneBar(const SceneBar&) = delete;
        SceneBar& operator=(const SceneBar&) = delete;

        /// Once a frame: each button as its action is now over the page; the space's icon and
        /// the increments' checks as the gizmo has them.
        void Sync()
        {
            m_bar.Sync();
            SyncState();
            GizmoController* gizmos = m_gizmos ? m_gizmos() : nullptr;
            if (gizmos == nullptr)
                return;
            const bool world = gizmos->Space() == GizmoSpace::World;
            if (m_space != nullptr && (!m_spaceShown || world != m_world))
            {
                m_space->SetIconSource(String(world ? StringView(u8"Editor/Icons/world.svg") : StringView(u8"Editor/Icons/local.svg")));
                m_world = world;
                m_spaceShown = true;
            }
            const TransformGizmo& gizmo = gizmos->Gizmo();
            Check(m_moveItems, kMoveIncrements, gizmos->MoveSnapsToGrid() ? 0.0f : gizmo.translateSnap);
            Check(m_rotateItems, kRotateIncrements, gizmo.rotateSnapDegrees);
            Check(m_scaleItems, kScaleIncrements, gizmo.scaleSnap);
        }

        /// Picks the `index`th increment of `kind`, as its menu item does.
        void PickIncrement(SnapKind kind, usize index)
        {
            GizmoController* gizmos = m_gizmos ? m_gizmos() : nullptr;
            if (gizmos == nullptr)
                return;
            TransformGizmo& gizmo = gizmos->Gizmo();
            switch (kind)
            {
            case SnapKind::Move:
                if (index < sizeof(kMoveIncrements) / sizeof(kMoveIncrements[0]))
                {
                    gizmos->SetMoveSnapsToGrid(kMoveIncrements[index].value == 0.0f);
                    if (kMoveIncrements[index].value > 0.0f)
                        gizmo.translateSnap = kMoveIncrements[index].value;
                }
                break;
            case SnapKind::Rotate:
                if (index < sizeof(kRotateIncrements) / sizeof(kRotateIncrements[0]))
                    gizmo.rotateSnapDegrees = kRotateIncrements[index].value;
                break;
            case SnapKind::Scale:
                if (index < sizeof(kScaleIncrements) / sizeof(kScaleIncrements[0]))
                    gizmo.scaleSnap = kScaleIncrements[index].value;
                break;
            }
            Sync();
        }
        [[nodiscard]] usize Bound() const noexcept { return m_bar.Count(); }
        /// What the state label says ("Simulating", "Paused"); empty while it shows nothing.
        [[nodiscard]] StringView State() const { return m_state != nullptr && m_state->IsVisible() ? m_state->GetText() : StringView{}; }

    private:
        // Simulate's state beside its buttons: shown while it runs, the edits locked meanwhile. Its
        // place is kept while it shows nothing, and its width is the sheet's whatever it says, so the
        // buttons centred beside it never move as it starts, pauses or stops.
        void SyncState()
        {
            if (m_state == nullptr)
                return;
            const ISceneEditorPage* scene = ServiceOf<ISceneEditorPage>(m_page);
            const bool running = scene != nullptr && scene->IsSimulating();
            const bool paused = running && scene->IsPaused();
            if (m_state->IsVisible() != running)
                m_state->SetVisible(running);
            if (!running || (m_stateShown && paused == m_paused))
                return;
            m_state->SetText(paused ? StringView(u8"Paused") : StringView(u8"Simulating"));
            m_state->RemoveClass(paused ? StringView(u8"playing") : StringView(u8"paused"));
            m_state->AddClass(paused ? StringView(u8"paused") : StringView(u8"playing"));
            m_state->SetTooltip(u8"The scene runs here; edits wait until Stop, which puts it back as it was");
            m_paused = paused;
            m_stateShown = true;
        }

        template <usize N>
        void AddIncrements(foundation::gui::Menu& menu, SnapKind kind, const Increment (&increments)[N], Array<foundation::gui::MenuItem*>& items)
        {
            SceneBar* self = this;
            for (usize i = 0; i < N; ++i)
                items.PushBack(menu.AddCheckItem(StringView(increments[i].label), false, [self, kind, i](bool) { self->PickIncrement(kind, i); }));
        }
        void BuildSnapMenu(foundation::gui::Menu& menu)
        {
            AddIncrements(*menu.AddSubMenu(u8"Move"), SnapKind::Move, kMoveIncrements, m_moveItems);
            AddIncrements(*menu.AddSubMenu(u8"Rotate"), SnapKind::Rotate, kRotateIncrements, m_rotateItems);
            AddIncrements(*menu.AddSubMenu(u8"Scale"), SnapKind::Scale, kScaleIncrements, m_scaleItems);
        }
        // The item whose increment is `value` checked, the others not (a value no item has
        // checks none).
        template <usize N>
        static void Check(Array<foundation::gui::MenuItem*>& items, const Increment (&increments)[N], f32 value)
        {
            for (usize i = 0; i < items.Size() && i < N; ++i)
            {
                const bool on = increments[i].value == value;
                if (items[i]->IsChecked() != on)
                    items[i]->SetCheckedSilently(on);
            }
        }

        app::PageActionBar m_bar;
        EditorPage* m_page;
        foundation::gui::Label* m_state;
        bool m_paused = false;
        bool m_stateShown = false;
        Function<GizmoController*()> m_gizmos;
        foundation::gui::ButtonBase* m_space = nullptr;
        bool m_world = true;
        bool m_spaceShown = false;
        Array<foundation::gui::MenuItem*> m_moveItems; // the menu's, which the button keeps
        Array<foundation::gui::MenuItem*> m_rotateItems;
        Array<foundation::gui::MenuItem*> m_scaleItems;
    };
}
