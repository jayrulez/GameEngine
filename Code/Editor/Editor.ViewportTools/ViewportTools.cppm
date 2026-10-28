// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::ViewportTools - the `editor.viewporttools` module.
//
// The viewport TOOL-MODE framework (terrain spec, editing-experience decisions 2026-08-10): a 3D
// viewport hosts exactly one active tool at a time - select+gizmos is the DEFAULT tool, terrain
// sculpting / splat painting / future scatter brushes are modal tools - so the viewport has ONE
// input-routing path instead of a special case plus N imitations.
//
// Deliberately NOT in Editor.Core: the tool contract speaks pick rays, pointer state and overlay
// drawing, and Editor.Core is UI-free (headless hosts link it). This lib is the thin layer above
// it: domain editor libs (Editor.Terrain, ...) implement tools against this interface WITHOUT
// linking the scene page; the scene page hosts the manager and builds the per-frame input.
//
// Input is a PLAIN STRUCT the host fills each frame (the GizmoFrameInput precedent - whole
// gesture sessions are scriptable headlessly in tests). Mutation goes through EditorCommandStack
// commands ONLY (one command/group per gesture) - never direct writes; that is what makes agent
// writes (MCP P2b) and human tools land on the same undo path.

module;
#include "Core/Prelude.h"

export module editor.viewporttools;

import foundation.core;
import foundation.scene;
import foundation.render;
import foundation.shell;
import editor.core; // IAssetEditSink (the asset-edit persistence transport), EditorCommandStack

using namespace foundation::core;
namespace core = foundation::core;

export namespace editor
{
    // NOTE: deliberately NO exported namespace aliases here (editor.scene's partitions export
    // `editor::scene`/`editor::render` aliases of their own; two modules exporting the same
    // alias ICEs GCC's module merger). Types are fully qualified instead.

    struct ViewportRay
    {
        Float3 origin{};
        Float3 direction{0.0f, 0.0f, -1.0f};
    };

    /// One frame of viewport input, built by the HOST (the page owns camera policy: it masks
    /// buttons and nulls `keyboard` while the camera owns the mouse, exactly as it did for the
    /// gizmo). A plain struct so tests script whole gestures without a viewport.
    struct ViewportToolInput
    {
        ViewportRay ray{};
        Float3 cameraPosition{};
        Float3 cameraForward{0.0f, 0.0f, -1.0f};
        Float3 cameraUp{0.0f, 1.0f, 0.0f};
        f32 fovY = 1.0472f;
        f32 orthoHeight = 0.0f; // > 0: an orthographic view this tall, in world units

        /// False when the pointer is not over the viewport this frame: a tool must keep any
        /// selection-tracking visuals in sync but clear hover, ignore buttons, and finish or
        /// abort an in-flight gesture (the GizmoFrameInput contract).
        bool pointerValid = true;

        // The pointer in VIEW pixels (y down, row 0 = top) + the view's size: what a GPU pick
        // needs. Valid only when pointerValid and viewportWidth/Height are non-zero.
        i32 pointerX = 0;
        i32 pointerY = 0;
        u32 viewportWidth = 0;
        u32 viewportHeight = 0;

        /// Strictly "the pointer is over the viewport" (pointerValid also admits
        /// focused-but-not-hovered so gizmo hotkeys keep working); click-initiated gestures
        /// (selection pick, brush strokes) require THIS.
        bool pointerOver = true;

        bool leftPressed = false;
        bool leftDown = false;
        bool leftReleased = false;
        bool ctrl = false;
        bool shift = false;
        f32 wheelDelta = 0.0f;    // vertical scroll this frame (brush resize etc.)
        f32 deltaSeconds = 0.0f;  // frame time (a continuous brush scales its per-dab delta by this)

        /// True while edits are refused (Simulate mode): tools may still hover/inspect and the
        /// host still picks selection, but no tool may open a command or mutate anything.
        bool editingLocked = false;

        /// Nullable. Null while the camera owns input (fly keys) - tools read key EDGES for
        /// their hotkeys (W/E/R/X on the select tool) and must tolerate null every frame.
        foundation::shell::IKeyboard* keyboard = nullptr;
    };

    /// A modal interaction mode of a 3D viewport. Implementations live in domain editor libs
    /// (or the hosting page, for its default tool) and are OWNED by a ViewportToolManager.
    // The host's GPU pick seam (RenderSubsystem::RequestPick over the page's viewport key): a
    // tool asks for the entities under a pixel rect of the view and polls for the answer a few
    // frames later. Null on the host context = no GPU picking (the tool's CPU pick stands alone).
    class IViewportPicker
    {
    public:
        virtual ~IViewportPicker() = default;

        // Rect in view pixels. Returns 0 when nothing could answer (no renderer, no view).
        [[nodiscard]] virtual u32 RequestPick(i32 x, i32 y, u32 width, u32 height) = 0;

        // True once the request answered: `hits` are the live entities in the rect, nearest-first
        // for a 1x1 rect (one hit at most). A request that expired answers with no hits.
        [[nodiscard]] virtual bool TryTakePick(u32 request,
                                               Array<foundation::scene::EntityHandle>& hits) = 0;
    };

    class IViewportTool
    {
    public:
        virtual ~IViewportTool() = default;

        /// Stable identifier ("select", "terrain.sculpt") - palette state, tests, future
        /// settings persistence key. Never localized, never renamed casually.
        [[nodiscard]] virtual StringView Id() const = 0;

        /// Palette label.
        [[nodiscard]] virtual StringView DisplayName() const = 0;

        /// Palette group ("Terrain", "Vegetation"); empty = the tool stands alone. The host
        /// folds a category with two or more tools into one dropdown labelled by it, so a
        /// domain's brushes take one slot however many it registers. Never localized here.
        [[nodiscard]] virtual StringView Category() const { return {}; }

        /// Its icon, by its path under the data folder ("Editor/Icons/Types/SplineComponent.svg"):
        /// the palette's picture of it. Data the host draws, so the framework stays UI-free; empty
        /// = the host shows its name.
        [[nodiscard]] virtual StringView Icon() const { return {}; }

        /// Availability predicate: registration is unconditional, RELEVANCE is contextual (a
        /// terrain brush needs a terrain in the scene). Checked every frame for the active
        /// tool - the manager falls back to the default tool when this turns false.
        [[nodiscard]] virtual bool IsAvailable() const { return true; }

        /// Why IsAvailable() is false, for the person who just clicked the tool: the host shows
        /// it as a notice when it refuses the activation (a silent snap-back reads as a dead
        /// button). One sentence naming what the scene lacks.
        [[nodiscard]] virtual StringView UnavailableReason() const
        {
            return u8"This tool has nothing to work on in this scene.";
        }

        virtual void OnActivate() {}

        /// Must end (finish or abort) any in-flight gesture so no half-applied command group
        /// survives a tool switch. The manager guarantees this is called before another tool
        /// activates.
        virtual void OnDeactivate() {}

        /// One frame. Returns true when the tool CONSUMED the pointer (hot handle or active
        /// gesture) - the host then suppresses its default click behavior (selection picking).
        virtual bool Update(const ViewportToolInput& input) = 0;

        /// Overlay drawing into the host viewport's debug-draw list (gizmo handles, brush
        /// cursor). Called every frame for the ACTIVE tool only.
        virtual void Draw(foundation::render::debug::DebugDraw& drawList) { (void)drawList; }

        /// One-line status readout the host shows in the viewport corner ("" = nothing).
        [[nodiscard]] virtual StringView StatusText() const { return {}; }
    };

    /// Owns the tools of ONE viewport host (a page) and routes input to the active one. The
    /// FIRST tool added is the default: always available, activated at start, fallen back to
    /// when the active tool deactivates or becomes unavailable.
    class ViewportToolManager
    {
    public:
        /// Adds and takes ownership. The first Add sets the default AND active tool.
        IViewportTool* Add(UniquePtr<IViewportTool> tool);

        [[nodiscard]] usize Count() const noexcept { return m_tools.Size(); }
        [[nodiscard]] IViewportTool* ToolAt(usize index) noexcept;
        [[nodiscard]] IViewportTool* FindById(StringView id) noexcept;
        [[nodiscard]] IViewportTool* ActiveTool() noexcept { return m_active; }

        /// Activate by id (deactivating the current tool first - gesture-end guaranteed).
        /// False when the id is unknown or the tool reports unavailable. Activating the
        /// already-active tool is a no-op returning true.
        bool ActivateById(StringView id);

        /// Return to the default tool (no-op when it is already active or nothing was added).
        void ActivateDefault();

        /// Route one frame to the active tool. Falls back to the default tool FIRST when the
        /// active one reports unavailable. Returns the active tool's consumed flag.
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
        String category;      // empty = a lone tool
        Array<String> toolIds; // never empty
    };

    /// Groups every tool but the default (index 0) for a palette host.
    void GroupViewportTools(ViewportToolManager& manager, Array<ViewportToolGroup>& out);

    /// Everything a scene-viewport tool may need at CREATION, in framework-known types only
    /// (page-specific context stays out of the interface; concrete tools that need more are
    /// created by the page directly, not through a provider).
    struct ViewportToolHostContext
    {
        foundation::scene::Scene* scene = nullptr;
        EditorCommandStack* commands = nullptr;
        Selection<Guid>* entitySelection = nullptr;

        /// Persist-a-live-asset-edit transport (domain-free): a tool that edits a cooked product live
        /// registers a closure that writes the edit back to its SOURCE asset; the editor save flow
        /// drains it, handing the closure the source DB (see EditorContext::DrainAssetEdits). Borrowed
        /// (the sink outlives every tool); null in hosts/tests that do not support asset persistence.
        IAssetEditSink* assetEdits = nullptr;

        /// The editor context (thumbnails + the source content DB for asset names), so a tool PANEL can
        /// present asset-backed choices richly (e.g. the splat layer picker showing layer albedo
        /// thumbnails). Borrowed; null in headless hosts / tests.
        EditorContext* editorContext = nullptr;

        /// GPU pick seam (null = CPU picking only). Borrowed; the page owns it.
        IViewportPicker* picker = nullptr;
    };

    /// A domain editor lib's tool contribution ("Editor.Terrain adds sculpt + splat"). Static
    /// lifetime; registered explicitly from the lib's registrar (house rule: explicit
    /// registration + count tripwire, never discovery).
    class IViewportToolProvider
    {
    public:
        virtual ~IViewportToolProvider() = default;

        /// Create this provider's tools into `manager` for one host. Called once per page.
        virtual void CreateTools(ViewportToolManager& manager,
                                 const ViewportToolHostContext& context) = 0;
    };

    /// The global provider registry (the composition seam). Pages call CreateAll AFTER adding
    /// their default tool, so provider tools can never become the default.
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
