// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::GUI - the `engine.gui` module: the engine's GUI on the lab (E5 of
// Specs/gui-lab-evolution-3.md), Engine.UI's counterpart, beside it while both live.
//
// GuiResourceProvider answers the lab's resource requests by asset guid ("{guid}" or the bare
// guid), as every other reference in the engine: a document's markup and a sheet's CSS from their
// cooked products, an SVG from a vector image, an image from a texture (a render texture a camera
// draws into among them). The markup, the sheets and the gamekit need no change: they ask by string
// as they do in the sandbox.
//
// GuiSubsystem: one GUIContext and style manager for the application, the lab's built-in theme
// under the game's sheets (a list, in order, each an @import of its guid), the fonts; a screen tier
// (a root holding the gamekit's screen stack and an overlay layer above it, outside any scene, so
// it survives a scene swap) and a scene tier (a root per scene, drawn wherever the scene renders);
// both drawn into the engine's overlay passes through GUI.Hosting's GuiPassRenderer, above
// Engine.UI's. A sheet or document whose cooked product changes restyles the GUI (hot reload). The
// GUI ticks on unscaled time (menus move while the game is paused), so its work is in BeginFrame.
//
// Input (E6): the engine's active input source (the player's window, an editor Game tab's view)
// into one root a frame, through GUI.Shell's bridge (one per root, each root its own dispatcher): an
// occupied overlay layer is modal; else the root whose GUI is under the pointer (the screen tier
// first); else the screen tier while it holds a screen; else the first scene root with content. A
// press stays with its root until every button is up. Which scenes input may reach follows the input
// subsystem's binding, as Engine.UI's does. The action layer is told what the GUI took (the pointer
// over GUI that takes it, or a press that began there; the keys while text is being edited), added
// to what Engine.UI claims; the GUI tracks the input kind from the same source.
//
// Its components (E7), script surface (E8) and the player's settings (E9) come later.
// GCC modules hygiene: the renderer, the shaders and the textures live in GuiSubsystemImpl.cpp.

module;
#include "Core/Prelude.h"

export module engine.gui;

import foundation.core;
import engine.domain;
import foundation.runtime;
import foundation.scene;
import engine.scene;
import foundation.resource;
import foundation.rhi;
import foundation.vfs;
import foundation.fonts;
import foundation.image;
import foundation.vg.svg;
import foundation.render.api; // the two overlay roles
import foundation.shell;      // the text input target
import foundation.input;      // InputKindTracker
import engine.input;          // the active input source, the action runtimes
import experimental.gui;
import experimental.gui.gamekit;
import experimental.gui.shell; // GuiInputBridge

using namespace foundation::core;
namespace rhi = foundation::rhi;

export namespace engine::gui
{
    namespace lab = experimental::gui;
    namespace render = foundation::render;
    namespace scene = foundation::scene;
    namespace resource = foundation::resource;
    namespace image = foundation::image;

    /// The lab's resources by asset guid, through the application's resource manager.
    class GuiResourceProvider final : public lab::IResourceProvider
    {
    public:
        /// Told when an image key starts or stops standing for a texture (`view` null: it stops):
        /// the renderers that draw the GUI.
        using TextureSink = Function<void(const image::ImageData* key, rhi::TextureView* view)>;

        explicit GuiResourceProvider(IAllocator& allocator);
        ~GuiResourceProvider() override;
        GuiResourceProvider(const GuiResourceProvider&) = delete;
        GuiResourceProvider& operator=(const GuiResourceProvider&) = delete;

        /// Where the guids resolve; null (between projects) resolves nothing. Another manager lets
        /// go of what was bound: an image key stays valid for what holds it and stops showing.
        /// Borrowed.
        void SetResourceManager(resource::ResourceManager* resources);
        [[nodiscard]] resource::ResourceManager* GetResourceManager() const noexcept;
        /// The renderers' sink; told at once of every key that stands for a texture now.
        void SetTextureSink(TextureSink sink);

        /// A texture's image: one key per texture, its natural size the texture's.
        [[nodiscard]] const image::ImageData* LoadImage(StringView path) override;
        /// A document's markup or a sheet's CSS.
        [[nodiscard]] bool LoadText(StringView path, String& out) override;
        /// A vector image's picture, parsed once per product.
        [[nodiscard]] const foundation::vg::svg::SVGDocument* LoadSvg(StringView path) override;

        /// Brings what was read in line with the current products (a re-cook, a failed load): true
        /// when a document or sheet read before has changed since the last poll.
        bool Poll();

        /// The guid `path` names ("{guid}" or bare); false when it names none.
        [[nodiscard]] static bool ParseGuid(StringView path, Guid& out) noexcept;

    private:
        struct State;
        UniquePtr<State> m_state;
    };

    class GuiSubsystem final : public foundation::runtime::Subsystem,
                               public scene::ISceneObserver,
                               public render::ISceneOverlay,
                               public render::IScreenOverlay
    {
    public:
        /// `dataFileSystem` is the application's data mount (borrowed for the subsystem's life):
        /// the VG shaders and the built-in font come from it.
        GuiSubsystem(IAllocator& allocator, foundation::vfs::IFileSystem& dataFileSystem);
        ~GuiSubsystem() override;

        /// After Engine.UI's (-650), so a game's GUI sees the same frame; all work in BeginFrame.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -640; }

        [[nodiscard]] lab::GUIContext& Context() noexcept { return *m_context; }
        [[nodiscard]] const RefPtr<lab::GUIContext>& ContextRef() const noexcept { return m_context; }
        /// The style manager every root is styled through: the built-in theme, the game's sheets.
        [[nodiscard]] lab::StyleManager& Styles() noexcept { return m_styles; }
        [[nodiscard]] GuiResourceProvider& Resources() noexcept { return *m_resources; }
        /// The fonts the GUI's text draws with.
        [[nodiscard]] foundation::fonts::IFontService* Fonts() const noexcept;

        /// Where asset guids resolve (the application's resource manager); null resolves nothing.
        void SetResourceManager(resource::ResourceManager* resources);
        /// The game's sheets, over the built-in theme, each over the ones before it (a game's own
        /// over a shared one). Re-read when one's cooked product changes.
        void SetStyleSheets(Span<const Guid> sheets);
        [[nodiscard]] Span<const Guid> StyleSheets() const noexcept { return Span<const Guid>(m_sheets.Data(), m_sheets.Size()); }
        /// How many times the game's sheets were applied (set, or a cooked product changed).
        [[nodiscard]] u64 SheetGeneration() const noexcept { return m_sheetGeneration; }

        /// The screen tier, outside any scene: its root, the gamekit's screen stack in it, and the
        /// overlay layer above the stack (a loading screen, a system menu).
        [[nodiscard]] lab::SceneRoot* ScreenRoot() noexcept { return m_screenRoot.Get(); }
        [[nodiscard]] lab::ScreenStack* Screens() noexcept { return m_screens.Get(); }
        [[nodiscard]] lab::Node* OverlayLayer() noexcept { return m_overlay.Get(); }
        /// The scene tier's root for `scene`; null when the scene is unknown.
        [[nodiscard]] lab::SceneRoot* SceneRootFor(const scene::Scene& scene) noexcept;

        // ---- lifecycle ----
        void OnInit() override;
        void OnReady() override;
        void OnShutdown() override;
        void BeginFrame(f32 deltaTime) override;
        /// The action layer's pointer and keyboard claims cleared after the frame read them: the
        /// next frame's BeginFrame lane republishes them (Engine.UI's, then the GUI's added).
        void EndFrame() override;

        // ---- input ----
        /// The window whose platform text input (IME) follows the GUI's focus (the player's main
        /// window); null leaves it alone (an editor, whose own bridge owns it).
        void SetTextInputTarget(foundation::shell::IWindow* window);
        /// The root this frame's input went to; null before any input.
        [[nodiscard]] lab::SceneRoot* ActiveInputRoot() const noexcept { return m_activeRoot; }
        /// Whether the GUI took the pointer this frame (what the action layer was told).
        [[nodiscard]] bool PointerOverGui() const noexcept { return m_pointerTaken; }
        /// The input kind, from the active source each frame (the pointer, the keys, a pad).
        [[nodiscard]] foundation::input::InputKindTracker& GetInputKindTracker() noexcept { return m_inputKind; }

        // ---- the scene tier ----
        void OnSystemsReady(scene::Scene& scene) override;
        void OnDestroying(scene::Scene& scene) override;

        // ---- the overlay roles ----
        /// Above Engine.UI's (0) while both draw.
        [[nodiscard]] i32 OverlayOrder() const noexcept override { return 10; }
        /// The scene tier: the view's scene root, laid out at the view's viewport and drawn there.
        void Render(rhi::RenderPassEncoder& encoder, const render::SceneOverlayView& view) override;
        /// The screen tier, before the pass: the frame's bakes, then the screen root recorded with
        /// its offscreen layers on the host's encoder.
        void PrepareOverlay(rhi::CommandEncoder& encoder, const render::ScreenOverlayView& view) override;
        /// The screen tier: what PrepareOverlay recorded for this view, else recorded now.
        void Render(rhi::RenderPassEncoder& encoder, const render::ScreenOverlayView& view) override;

        /// The GPU side, by whoever owns graphics (the application's startup): the VG shaders from
        /// the data mount, the renderer. Idempotent; without it the overlays draw nothing.
        void EnsureRenderReady(rhi::Device& device, i32 frameCount);
        [[nodiscard]] bool IsRenderReady() const noexcept;

    private:
        struct RenderState; // the renderer and the shaders (impl unit)
        struct SceneTier
        {
            const scene::Scene* Scene = nullptr;
            RefPtr<lab::SceneRoot> Root;
            UniquePtr<lab::GuiInputBridge> Bridge;
        };

        void ApplySheets();
        void PumpInput();
        [[nodiscard]] lab::SceneRoot* PickInputRoot(bool pointer, Float2 point);
        [[nodiscard]] bool InputReaches(const SceneTier& tier) const noexcept;
        [[nodiscard]] lab::GuiInputBridge* BridgeOf(const lab::SceneRoot* root) noexcept;
        [[nodiscard]] UniquePtr<lab::GuiInputBridge> MakeBridge(lab::SceneRoot& root);
        void StyleAndLayOut(lab::SceneRoot& root, Float2 size);
        [[nodiscard]] bool ReadDataFile(StringView path, Array<u8>& out) const;

        TaggedAllocator m_allocator;
        foundation::vfs::IFileSystem* m_dataFileSystem; // borrowed
        RefPtr<lab::GUIContext> m_context;
        lab::StyleManager m_styles;
        UniquePtr<GuiResourceProvider> m_resources;
        UniquePtr<foundation::fonts::IFontService> m_fonts;
        Array<Guid> m_sheets;
        u64 m_sheetGeneration = 0;
        RefPtr<lab::SceneRoot> m_screenRoot;
        RefPtr<lab::ScreenStack> m_screens;
        RefPtr<lab::UIWidget> m_overlay;
        UniquePtr<lab::GuiInputBridge> m_screenBridge;
        Array<SceneTier> m_sceneTiers;
        UniquePtr<RenderState> m_render;
        render::ISceneRenderer* m_sceneRenderer = nullptr; // the overlays' registration
        render::IScreenRenderer* m_screenRenderer = nullptr;
        engine::input::InputSubsystem* m_input = nullptr;
        foundation::input::InputKindTracker m_inputKind;
        foundation::shell::IWindow* m_textInputTarget = nullptr;
        lab::SceneRoot* m_activeRoot = nullptr; // the root input went to last
        lab::SceneRoot* m_pressRoot = nullptr;  // the root a held press began in
        bool m_pressTaken = false;              // that press began over GUI that takes the pointer
        bool m_pointerTaken = false;
    };

    /// The GUI's domain (engine-composition.md): its resource modules (documents and sheets, the
    /// vector images, the fonts) beside the UI domain's while both live.
    [[nodiscard]] const engine::DomainModule& GuiDomain() noexcept;
}
