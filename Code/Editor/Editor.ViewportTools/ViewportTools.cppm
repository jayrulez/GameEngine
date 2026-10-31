// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::ViewportTools - the `editor.viewporttools` module.
//
// The viewport tool-mode framework. A 3D viewport hosts exactly one active tool at a time: select
// with gizmos is the default tool, and the terrain and vegetation brushes are modal tools. The
// viewport therefore has one input-routing path instead of a special case plus imitations of it.
//
// This is not in Editor.Core, because the tool contract deals in pick rays, pointer state and
// overlay drawing, while Editor.Core is UI-free (headless hosts link it). This library is the thin
// layer above it: domain editor libraries (Editor.Terrain and others) implement tools against this
// interface without linking the scene page, and the scene page hosts the manager and builds the
// per-frame input.
//
// Input is a plain struct the host fills each frame, as GizmoFrameInput is, so whole gestures can
// be scripted headlessly in tests. Changes go only through EditorCommandStack commands (one
// command or group per gesture), never direct writes, so agent edits over MCP and human tools land
// on the same undo path.

module;
#include "Core/Prelude.h"

export module editor.viewporttools;

import foundation.core;
import foundation.scene;
import foundation.render;
import foundation.shell;
import editor.core; // IAssetEditSink (saves live asset edits), EditorCommandStack

using namespace foundation::core;
namespace core = foundation::core;

export namespace editor
{
    // No exported namespace aliases here: editor.scene's partitions export `editor::scene` and
    // `editor::render` aliases of their own, and two modules exporting the same alias crash GCC's
    // module merger (an internal compiler error). Types are fully qualified instead.

    struct ViewportRay
    {
        Float3 origin{};
        Float3 direction{0.0f, 0.0f, -1.0f};
    };

    /// One frame of viewport input, built by the host. The page owns camera policy: it masks
    /// buttons and nulls `keyboard` while the camera owns the mouse, as it does for the gizmo. A
    /// plain struct, so tests can script whole gestures without a viewport.
    struct ViewportToolInput
    {
        ViewportRay ray{};
        Float3 cameraPosition{};
        Float3 cameraForward{0.0f, 0.0f, -1.0f};
        Float3 cameraUp{0.0f, 1.0f, 0.0f};
        f32 fovY = 1.0472f;
        f32 orthoHeight = 0.0f; // when > 0, an orthographic view this tall, in world units

        /// False when the pointer is not over the viewport this frame: a tool must keep any
        /// selection-tracking visuals in sync but clear hover, ignore buttons, and finish or
        /// abort a gesture in progress (the same contract as GizmoFrameInput).
        bool pointerValid = true;

        // The pointer in view pixels (y down, row 0 at the top) and the view's size, which a GPU
        // pick needs. Valid only when pointerValid is set and viewportWidth and viewportHeight are
        // non-zero.
        i32 pointerX = 0;
        i32 pointerY = 0;
        u32 viewportWidth = 0;
        u32 viewportHeight = 0;

        /// Strictly whether the pointer is over the viewport. pointerValid is also true when the
        /// view is focused but not hovered, so gizmo hotkeys keep working; gestures started by a
        /// click (selection pick, brush strokes) require pointerOver.
        bool pointerOver = true;

        bool leftPressed = false;
        bool leftDown = false;
        bool leftReleased = false;
        bool ctrl = false;
        bool shift = false;
        f32 wheelDelta = 0.0f;    // vertical scroll this frame (brush resize and the like)
        f32 deltaSeconds = 0.0f;  // frame time (a continuous brush scales its per-dab delta by this)

        /// True while edits are refused (Simulate mode). Tools may still hover and inspect and the
        /// host still picks the selection, but no tool may open a command or change anything.
        bool editingLocked = false;

        /// Nullable: null while the camera owns input (fly keys). Tools read key presses for their
        /// hotkeys (W, E, R and X on the select tool) and must tolerate null on any frame.
        foundation::shell::IKeyboard* keyboard = nullptr;
    };

    // The host's GPU pick interface (RenderSubsystem::RequestPick over the page's viewport key). A
    // tool asks for the entities under a pixel rect of the view and polls for the answer a few
    // frames later. Null on the host context means no GPU picking, and the tool's CPU pick is used
    // alone.
    class IViewportPicker
    {
    public:
        virtual ~IViewportPicker() = default;

        // Rect in view pixels. Returns 0 when nothing could answer (no renderer, no view).
        [[nodiscard]] virtual u32 RequestPick(i32 x, i32 y, u32 width, u32 height) = 0;

        // True once the request has an answer: `hits` are the live entities in the rect, nearest
        // first (at most one hit for a 1x1 rect). A request that expired answers with no hits.
        [[nodiscard]] virtual bool TryTakePick(u32 request,
                                               Array<foundation::scene::EntityHandle>& hits) = 0;
    };

    /// A modal interaction mode of a 3D viewport. Implementations live in domain editor
    /// libraries (or in the hosting page, for its default tool) and are owned by a
    /// ViewportToolManager.
    class IViewportTool
    {
    public:
        virtual ~IViewportTool() = default;

        /// Stable identifier ("select", "terrain.sculpt") used for palette state, tests and as a
        /// settings key. Never localized, and not to be renamed lightly.
        [[nodiscard]] virtual StringView Id() const = 0;

        /// Palette label.
        [[nodiscard]] virtual StringView DisplayName() const = 0;

        /// Palette group ("Terrain", "Vegetation"); empty when the tool stands alone. The host
        /// folds a category with two or more tools into one dropdown labelled by it, so a
        /// domain's brushes take one slot however many it registers. Never localized here.
        [[nodiscard]] virtual StringView Category() const { return {}; }

        /// The tool's palette icon, by its path under the data folder
        /// ("Editor/Icons/Types/SplineComponent.svg"). The host draws it, so the framework stays
        /// UI-free; when empty, the host shows the tool's name.
        [[nodiscard]] virtual StringView Icon() const { return {}; }

        /// Availability predicate. Registration is unconditional, but whether a tool applies
        /// depends on context (a terrain brush needs a terrain in the scene). Checked every frame
        /// for the active tool; the manager falls back to the default tool when this turns false.
        [[nodiscard]] virtual bool IsAvailable() const { return true; }

        /// Why IsAvailable() is false, for the person who just clicked the tool. The host shows it
        /// as a notice when it refuses the activation, since a silent refusal looks like a broken
        /// button. One sentence naming what the scene lacks.
        [[nodiscard]] virtual StringView UnavailableReason() const
        {
            return u8"This tool has nothing to work on in this scene.";
        }

        virtual void OnActivate() {}

        /// Must end (finish or abort) any gesture in progress so no half-applied command group
        /// survives a tool switch. The manager guarantees this is called before another tool
        /// activates.
        virtual void OnDeactivate() {}

        /// One frame. Returns true when the tool consumed the pointer (a hot handle or an active
        /// gesture); the host then suppresses its default click behavior (selection picking).
        virtual bool Update(const ViewportToolInput& input) = 0;

        /// Overlay drawing into the host viewport's debug-draw list (gizmo handles, brush
        /// cursor). Called every frame, for the active tool only.
        virtual void Draw(foundation::render::debug::DebugDraw& drawList) { (void)drawList; }

        /// One-line status readout the host shows in the viewport corner (empty for none).
        [[nodiscard]] virtual StringView StatusText() const { return {}; }
    };

    /// Owns the tools of one viewport host (a page) and routes input to the active one. The
    /// first tool added is the default: it is always available, active at start, and the
    /// fallback when the active tool deactivates or becomes unavailable.
    class ViewportToolManager
    {
    public:
        /// Adds a tool and takes ownership. The first Add sets both the default and the active
        /// tool.
        IViewportTool* Add(UniquePtr<IViewportTool> tool);

        [[nodiscard]] usize Count() const noexcept { return m_tools.Size(); }
        [[nodiscard]] IViewportTool* ToolAt(usize index) noexcept;
        [[nodiscard]] IViewportTool* FindById(StringView id) noexcept;
        [[nodiscard]] IViewportTool* ActiveTool() noexcept { return m_active; }

        /// Activate by id, deactivating the current tool first so its gesture always ends.
        /// False when the id is unknown or the tool reports unavailable. Activating the
        /// already-active tool is a no-op returning true.
        bool ActivateById(StringView id);

        /// Return to the default tool (no-op when it is already active or nothing was added).
        void ActivateDefault();

        /// Route one frame to the active tool. When the active tool reports unavailable, falls
        /// back to the default tool first. Returns the active tool's consumed flag.
        bool Update(const ViewportToolInput& input);

        /// Draw the active tool's overlay.
        void Draw(foundation::render::debug::DebugDraw& drawList);

    private:
        Array<UniquePtr<IViewportTool>> m_tools;
        IViewportTool* m_active = nullptr; // borrowed from m_tools; null until first Add
    };

    /// One palette group: the tools sharing a Category(), in registration order, or a single
    /// uncategorized tool. Groups come in first-appearance order, so a domain's registration
    /// order still decides where its dropdown sits.
    struct ViewportToolGroup
    {
        String category;      // empty for a lone tool
        Array<String> toolIds; // never empty
    };

    /// Groups every tool but the default (index 0) for a palette host.
    void GroupViewportTools(ViewportToolManager& manager, Array<ViewportToolGroup>& out);

    /// Everything a scene-viewport tool may need at creation, in types the framework knows.
    /// Page-specific context stays out of the interface; concrete tools that need more are created
    /// by the page directly, not through a provider.
    struct ViewportToolHostContext
    {
        foundation::scene::Scene* scene = nullptr;
        EditorCommandStack* commands = nullptr;
        Selection<Guid>* entitySelection = nullptr;

        /// Saves live asset edits, independent of any domain. A tool that edits a cooked product live
        /// registers a closure that writes the edit back to its source asset; the editor's save flow
        /// drains it and hands the closure the source database (see EditorContext::DrainAssetEdits).
        /// Borrowed (the sink outlives every tool); null in hosts and tests that cannot save assets.
        IAssetEditSink* assetEdits = nullptr;

        /// The editor context (thumbnails and the source content database for asset names), so a
        /// tool panel can show asset-backed choices with pictures (for example the splat layer
        /// picker showing layer albedo thumbnails). Borrowed; null in headless hosts and tests.
        EditorContext* editorContext = nullptr;

        /// The GPU pick interface (null for CPU picking only). Borrowed; the page owns it.
        IViewportPicker* picker = nullptr;
    };

    /// The tools a domain editor library adds (Editor.Terrain adds sculpt and splat, for example).
    /// Static lifetime; registered explicitly from the library's registrar, never discovered.
    class IViewportToolProvider
    {
    public:
        virtual ~IViewportToolProvider() = default;

        /// Create this provider's tools into `manager` for one host. Called once per page.
        virtual void CreateTools(ViewportToolManager& manager,
                                 const ViewportToolHostContext& context) = 0;
    };

    /// The global provider registry, where domains plug in their tools. Pages call CreateAll
    /// after adding their default tool, so provider tools can never become the default.
    class ViewportToolProviderRegistry
    {
    public:
        [[nodiscard]] static ViewportToolProviderRegistry& Get();

        /// Registers a provider (static lifetime, borrowed). Duplicate pointers are ignored so
        /// a re-run registrar stays idempotent.
        void Register(IViewportToolProvider* provider);

        [[nodiscard]] usize Count() const noexcept { return m_providers.Size(); }

        void CreateAll(ViewportToolManager& manager, const ViewportToolHostContext& context);

    private:
        Array<IViewportToolProvider*> m_providers;
    };
}
