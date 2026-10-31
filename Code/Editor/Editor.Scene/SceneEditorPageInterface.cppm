// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene - :scene_page_interface partition.
//
// ISceneEditorPage: what a scene (or prefab) page lets the rest of the editor act through, page by
// page; with several scenes open, everything here belongs to this page. It is published on the page
// (EditorPage::Provide) and found from any EditorPage* through EditorPage::Service, so a module
// that holds a page needs no cast and no knowledge of the page class.
//
// There are two surfaces for acting on a scene page, and this is the page-level one: the same one
// the page gives its own tools (its SceneEditContext, with the typed command vocabulary every edit
// goes through). The framework-level one, ViewportToolHostContext, is what a domain's provider
// tools receive at creation, limited to framework types, and it is not reachable from here.
// Simulation, the gizmo, the markers overlay and the prefab flows are page concerns (they lock the
// command stack, drive the toolbar and open the page's dialogs), so their control is part of this
// surface rather than the edit context; the scene editor's actions bind through it.
module;
#include "Core/Prelude.h"

export module editor.scene:scene_page_interface;

import foundation.core;
import foundation.scene;
import foundation.input; // ScriptedInputSource (viewport_run)
import editor.core;
import editor.camera;
import editor.viewporttools;
import :edit;
import :gizmo;

using namespace foundation::core;

export namespace editor
{
    /// A capture of the viewport's rendered colour target to a PNG: requested through the page,
    /// recorded on the next frame the viewport renders, written once the GPU has finished. A
    /// hidden viewport (an inactive dock tab) never renders, so a request over one stays
    /// Pending until the page comes to front.
    enum class ViewportCaptureState : u8
    {
        Idle,    ///< nothing requested
        Pending, ///< requested; not yet rendered and written
        Written, ///< the PNG is at `path`, `width` x `height`
        Failed,  ///< the copy or the write failed (log_read, category Screenshot, says why)
    };
    struct ViewportCapture
    {
        ViewportCaptureState state = ViewportCaptureState::Idle;
        String path;
        u32 width = 0;  // the written PNG: the pixels the viewport drew
        u32 height = 0;
        // The game's render resolution, when the view drew it scaled (a Game tab smaller than the
        // resolution it plays at); 0 means the written size is the render size.
        u32 renderWidth = 0;
        u32 renderHeight = 0;
    };

    /// A page running the game in a scene (a Game tab), as the scene tools read it: entity_inspect
    /// with `pie` reads the scene of the one with that id. Play in editor (editor.play) publishes
    /// it, through its IPieInstancePage.
    class IGameScenePage : public IPageService
    {
    public:
        /// The tab's id: `game-page` (kPrimaryPieId) for the primary, `game-page-1`,
        /// `game-page-2`, ... for the instances Play New Instance opens. The dock persists the tab
        /// under it too.
        [[nodiscard]] virtual StringView PieId() const noexcept = 0;
        /// Whether a run is in progress.
        [[nodiscard]] virtual bool IsRunning() const noexcept = 0;
        /// The scene the run is in now; null when it has none.
        [[nodiscard]] virtual foundation::scene::Scene* RunningScene() noexcept = 0;

    protected:
        ~IGameScenePage() = default;
    };

    /// The primary Game tab's PIE id.
    inline constexpr StringView kPrimaryPieId = u8"game-page";

    class ISceneEditorPage : public IPageService
    {
    public:
        /// This page's scene mutation mediator: its live scene, its command stack, its entity
        /// selection. Every edit goes through it as an undoable command.
        [[nodiscard]] virtual SceneEditContext& EditContext() noexcept = 0;

        /// Edit-mode Simulate: snapshot, run the live scene, restore on stop. Start and stop are
        /// no-ops when already in that state; pause holds a running simulation.
        virtual void StartSimulation() = 0;
        virtual void StopSimulation() = 0;
        virtual void PauseSimulation(bool paused) = 0;
        [[nodiscard]] virtual bool IsSimulating() const noexcept = 0;
        [[nodiscard]] virtual bool IsPaused() const noexcept = 0;

        /// The page's viewport tools (the select tool and the domains'); null on a page without a
        /// viewport. The tool actions toggle through it.
        [[nodiscard]] virtual ViewportToolManager* ViewportTools() noexcept = 0;
        /// The select tool's gizmo (mode, space); null on a page without a viewport.
        [[nodiscard]] virtual GizmoController* Gizmos() noexcept = 0;
        /// True while the viewport camera has the input (Alt orbit, right-button or captured
        /// fly): the plain keys belong to it then, so the gizmo mode chords (W, E, R) must not
        /// switch the mode mid-flight.
        [[nodiscard]] virtual bool CameraOwnsInput() const noexcept = 0;

        /// The entity markers overlay (every entity's marker in the viewport, not only the
        /// selected ones).
        [[nodiscard]] virtual bool MarkersShown() const noexcept = 0;
        virtual void SetMarkersShown(bool shown) = 0;

        /// The property animation panel under the viewport. When it is hidden and no other tab is
        /// open, the bottom dock takes no space at all.
        [[nodiscard]] virtual bool AnimationPanelShown() const noexcept = 0;
        virtual void SetAnimationPanelShown(bool shown) = 0;
        /// Shows the panel editing the clip `clip` (nil: the one it has) and animating `entity`
        /// (nil: the one it binds), asking first about an unsaved clip (an animator's Edit Clip).
        virtual void EditAnimationClip(const Guid& clip, const Guid& entity) = 0;

        /// The viewport's free-fly camera: the pose the scene is looked at from, which an agent
        /// moves to look from somewhere specific; null on a page without a viewport.
        [[nodiscard]] virtual EditorCamera* ViewportCamera() noexcept = 0;
        /// Frame entities in the viewport: the camera stands back along its view until the box
        /// holding them all, each with everything under it, fits, and pivots its orbit on the box's
        /// centre. The box is what the scene's systems measure (ISceneEntityBounds: meshes,
        /// colliders), or a small one at an entity's position when none does. Eases there when
        /// `ease`. False when no entity resolves or the page has no viewport.
        virtual bool FrameEntities(Span<const Guid> entities, bool ease) = 0;

        /// Ask for the viewport's next rendered frame as a PNG at `path` (the directory must
        /// exist). Replaces a pending request. NotSupported on a page without a viewport.
        [[nodiscard]] virtual Status RequestViewportCapture(StringView path) = 0;
        /// Capture the viewport as the project's launcher thumbnail (ProjectThumbnailPath): the
        /// scene alone, without the editor's overlays (grid, markers, gizmos, readouts, debug
        /// view) in the captured frame. NotSupported without a viewport or a project.
        [[nodiscard]] virtual Status RequestProjectThumbnail() = 0;
        /// The latest request's state, as it advances frame by frame.
        [[nodiscard]] virtual const ViewportCapture& LastViewportCapture() const noexcept = 0;

        /// A playtest of the viewport (viewport_run): from now on `source` stands in for the
        /// devices over it (its mouse in the viewport's render pixels, its keys as if the viewport
        /// had the focus), advanced by each frame's time. False on a page without a viewport.
        virtual bool BeginScriptedInput(UniquePtr<foundation::input::ScriptedInputSource> source) = 0;
        /// Ends it: whatever is held is released for a frame, then the devices are the viewport's
        /// again.
        virtual void EndScriptedInput() = 0;
        /// Seconds the playtest has run; negative when none does.
        [[nodiscard]] virtual f64 ScriptedInputTime() const noexcept = 0;
        /// The viewport's render size in pixels, the scripted mouse's space (0 x 0 without one).
        [[nodiscard]] virtual Float2 ViewportPixels() const noexcept = 0;

        /// The prefab flows the page owns (they open the page's dialogs and write assets): a prefab
        /// asset from an entity subtree; an instance spawned under `parent` (nil: the scene root)
        /// through the asset picker; an instance's deltas written back to its prefab; an instance's
        /// deltas discarded.
        virtual void CreatePrefabFromEntity(const Guid& entity) = 0;
        virtual void PickAndSpawnPrefab(const Guid& parent) = 0;
        virtual void ApplyInstanceToPrefab(const Guid& root) = 0;
        virtual void RevertInstance(const Guid& root) = 0;
    };
}
