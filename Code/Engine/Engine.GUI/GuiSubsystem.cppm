// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::GUI - the `engine.gui` module: the engine's GUI on foundation.gui.
//
// GuiResourceProvider answers the GUI's resource requests by asset guid ("{guid}" or the bare
// guid), as every other reference in the engine does. A document's markup and a sheet's CSS come
// from their cooked products, an SVG from a vector image, and an image from a texture (including
// a render texture a camera draws into). The markup, the sheets and the gamekit request resources
// by string, as they do in the sandbox, so they need no change.
//
// GuiSubsystem owns one GUIContext and style manager for the application, the fonts, and the
// GUI's built-in theme with the game's sheets layered over it (a list, in order, each an @import
// of its guid). It has two tiers, both drawn into the engine's overlay passes through
// GUI.Hosting's GuiPassRenderer. The screen tier is a root outside any scene, so it survives a
// scene swap; it holds the gamekit's screen stack with an overlay layer above it. The scene tier
// is one root per scene, drawn wherever that scene renders. When the cooked product of a sheet or
// document changes, the GUI is restyled (hot reload). The GUI ticks on unscaled time so that
// menus still move while the game is paused, which is why its work happens in BeginFrame.
//
// Input: each frame, the engine's active input source (the player's window, or the view of an
// editor Game tab) goes into one root through GUI.Shell's bridge. There is one bridge per root,
// and each root is its own dispatcher. The root is chosen in this order: the overlay layer when
// it is occupied (it is modal); otherwise the root whose GUI is under the pointer, checking the
// screen tier first; otherwise the screen tier while it holds a screen; otherwise the first scene
// root with content. A press stays with the root it began in until every button is up. The input
// subsystem's binding decides which scenes input may reach. The subsystem tells the action layer
// what the GUI took: the pointer while it is over GUI that takes it or while a press that began
// there is held, and the keys while text is being edited. These claims are added to what an
// earlier subsystem in the lane claimed. The GUI tracks the input kind from the same source.
//
// Components: `gui.Canvas` shows a document in its scene's root, optionally with a sheet of its
// own over the game's. A canvas is ordered among the scene's canvases and drawn at the target's
// pixels, or scaled to fit from a reference resolution. Despawning a canvas removes its tree, and
// re-cooking its document rebuilds the tree. `gui.Billboard` shows a document over its entity (a
// nameplate, a health bar) in a layer under the scene's canvases. Each view places the billboard
// through its camera as it draws: the billboard hangs by its bottom centre from the projected
// point, is hidden when that point is behind the camera, and scales with distance when asked to.
// A canvas may draw into a texture instead (a screen in the world, shown by its entity's sprite
// or decal). `gui.WorldPanel` shows a document on a quad in its entity's plane by driving the
// entity's sprite, and the pointer reaches it by a ray through the scene's camera. Both texture
// kinds are drawn in RenderTextures, which the host calls after the frame's bakes and before the
// scene draws.
//
// The script surface is the GUI's own: the `gui` facade, every widget reflected, and the
// gamekit's screen stack. ExposeToScript installs it per script context with the screen tier's
// root. `gui::root()` is the screen root, and `ScreenStack::of(gui::root().find("screens"))` is
// its stack, which pushes a document by its guid ("{guid}") through the provider. Each
// component's `.of(entity)` reaches the component's tree as a node (`root()`).
//
// The game's screen: the screen tier lays out at the game's render resolution. It draws at the
// target's own resolution into the rectangle the render resolution is fitted into, rotated when
// the game's orientation differs from the target's, as the game's image is. The pointer arrives
// in render space and is mapped into the tier's pixels. With run screens on (the editor), each
// run (the game in one Game tab) has a screen tier of its own. That tier is drawn for its tab
// (SetRenderRun), is fed the input of the scene the run is bound to, and is the `gui` root of the
// run's scripts. The overlay layer stays on the shared tier, above every run's tier.
//
// The renderer, the shaders and the textures live in GuiSubsystemImpl.cpp, so that GCC's module
// reader can read this interface back.

module;
#include "Core/Prelude.h"

export module engine.gui;

import foundation.core;
import engine.domain;
import foundation.runtime;
import foundation.run;        // the runs that have screen tiers, and their lifecycle
import foundation.scene;
import engine.scene;
import foundation.resource;
import foundation.rhi;
import foundation.vfs;
import foundation.fonts;
import foundation.fonts.resource; // the project's fonts (Font products)
import foundation.image;
import foundation.vg.svg;
import foundation.render.api; // the two overlay roles
import foundation.script;     // the script contexts the surface is installed in
import foundation.shell;      // the text input target
import foundation.input;      // InputKindTracker
import engine.input;          // the active input source, the action runtimes
import foundation.gui;
import foundation.gui.gamekit;
import foundation.gui.resources; // the documents and sheets components name
import foundation.gui.shell;     // GuiInputBridge

using namespace foundation::core;
using namespace foundation::gui;
namespace rhi = foundation::rhi;

export namespace engine::gui
{
    namespace render = foundation::render;
    namespace scene = foundation::scene;
    namespace resource = foundation::resource;
    namespace image = foundation::image;

    // ---- components ----

    enum class GuiScalerMode : u8
    {
        ConstantPixel = 0,   ///< one GUI pixel per target pixel
        ReferenceResolution, ///< laid out at the reference resolution, scaled uniformly to fit, centred
    };

    enum class GuiCanvasRenderMode : u8
    {
        ScreenOverlay = 0, ///< in its scene's GUI, over the view
        RenderTexture,     ///< into its own texture, which its entity's sprite or decal shows
    };

    /// A document shown in its scene's GUI: a menu, a HUD. It opens when spawned and closes when
    /// despawned.
    struct GuiCanvasComponent
    {
        // Authored:
        resource::Ref<GuiDocument> document;
        resource::Ref<GuiStyleSheet> sheet; ///< optional; applied over the game's sheets to this canvas alone
        i32 order = 0;                           ///< among the scene's canvases: higher on top
        bool visible = true;
        bool interactive = true;
        GuiScalerMode scalerMode = GuiScalerMode::ConstantPixel;
        Float2 referenceResolution{1920.0f, 1080.0f};
        GuiCanvasRenderMode renderMode = GuiCanvasRenderMode::ScreenOverlay;
        u32 renderTextureWidth = 512; ///< RenderTexture: the texture's size, in pixels
        u32 renderTextureHeight = 512;

        // Runtime:
        RefPtr<Node> root;       ///< the tree built from the document
        RefPtr<UIWidget> host;   ///< the widget holding the tree in the scene's root (order, scaler)
        RefPtr<Object> builtFrom;     ///< the document product the tree was built from; held so a new product never reuses its address
        RefPtr<Object> sheetFrom;     ///< the sheet product applied
        RefPtr<SceneRoot> renderRoot;     ///< RenderTexture: its own root, never in a scene's GUI
        rhi::Texture* renderTexture = nullptr; ///< RenderTexture: the texture, owned by the subsystem; null until first drawn
        rhi::TextureView* renderTextureView = nullptr;

        /// Returns the component's tree as a script node handle (`root()`), or null before the tree
        /// is built.
        [[nodiscard]] foundation::core::Variant rootNode() const;
    };

    inline void Serialize(ISerializer& ar, GuiCanvasComponent& c)
    {
        foundation::core::Serialize(ar, "document", c.document);
        foundation::core::Serialize(ar, "sheet", c.sheet);
        foundation::core::Serialize(ar, "order", c.order);
        foundation::core::Serialize(ar, "visible", c.visible);
        foundation::core::Serialize(ar, "interactive", c.interactive);
        u8 scaler = static_cast<u8>(c.scalerMode);
        foundation::core::Serialize(ar, "scalerMode", scaler);
        c.scalerMode = static_cast<GuiScalerMode>(scaler);
        foundation::core::Serialize(ar, "referenceResolution", c.referenceResolution);
        u8 render = static_cast<u8>(c.renderMode);
        foundation::core::Serialize(ar, "renderMode", render);
        c.renderMode = static_cast<GuiCanvasRenderMode>(render);
        foundation::core::Serialize(ar, "renderTextureWidth", c.renderTextureWidth);
        foundation::core::Serialize(ar, "renderTextureHeight", c.renderTextureHeight);
    }

    inline void ResolveResources(resource::ResourceManager& manager, GuiCanvasComponent& c)
    {
        c.document.Bind(manager);
        c.sheet.Bind(manager);
    }

    class GuiCanvasComponentManager final : public scene::SerializableComponentManager<GuiCanvasComponent>
    {
    public:
        GuiCanvasComponentManager() : SerializableComponentManager<GuiCanvasComponent>(u8"gui.Canvas") {}
    };

    enum class GuiBillboardOrientation : u8
    {
        Screen = 0,  ///< the offset in the entity's own space (it turns with the entity)
        Cylindrical, ///< the offset in world space (a fixed lift above the entity)
    };
    enum class GuiBillboardScale : u8
    {
        Fixed = 0, ///< the same size at any distance
        Distance,  ///< referenceDistance / distance, within minScale and maxScale
    };

    /// A document over its entity, facing the view: a nameplate, a health bar.
    struct GuiBillboardComponent
    {
        // Authored:
        resource::Ref<GuiDocument> document;
        resource::Ref<GuiStyleSheet> sheet; ///< optional; applied over the game's sheets to this billboard alone
        Float3 offset{0.0f, 0.0f, 0.0f};
        GuiBillboardOrientation orientation = GuiBillboardOrientation::Cylindrical;
        GuiBillboardScale scaleMode = GuiBillboardScale::Fixed;
        f32 referenceDistance = 10.0f;
        f32 minScale = 0.3f;
        f32 maxScale = 2.0f;
        bool visible = true;

        // Runtime:
        RefPtr<Node> root;
        RefPtr<Object> builtFrom;
        RefPtr<Object> sheetFrom;

        /// Returns the component's tree as a script node handle (`root()`), or null before the tree
        /// is built.
        [[nodiscard]] foundation::core::Variant rootNode() const;
    };

    inline void Serialize(ISerializer& ar, GuiBillboardComponent& c)
    {
        foundation::core::Serialize(ar, "document", c.document);
        foundation::core::Serialize(ar, "sheet", c.sheet);
        foundation::core::Serialize(ar, "offset", c.offset);
        u8 orientation = static_cast<u8>(c.orientation);
        foundation::core::Serialize(ar, "orientation", orientation);
        c.orientation = static_cast<GuiBillboardOrientation>(orientation);
        u8 scale = static_cast<u8>(c.scaleMode);
        foundation::core::Serialize(ar, "scaleMode", scale);
        c.scaleMode = static_cast<GuiBillboardScale>(scale);
        foundation::core::Serialize(ar, "referenceDistance", c.referenceDistance);
        foundation::core::Serialize(ar, "minScale", c.minScale);
        foundation::core::Serialize(ar, "maxScale", c.maxScale);
        foundation::core::Serialize(ar, "visible", c.visible);
    }

    inline void ResolveResources(resource::ResourceManager& manager, GuiBillboardComponent& c)
    {
        c.document.Bind(manager);
        c.sheet.Bind(manager);
    }

    class GuiBillboardComponentManager final : public scene::SerializableComponentManager<GuiBillboardComponent>
    {
    public:
        GuiBillboardComponentManager() : SerializableComponentManager<GuiBillboardComponent>(u8"gui.Billboard") {}
    };

    /// A document on a quad in the world, in its entity's plane (a sign, a terminal). The document
    /// is drawn into a texture at the panel's own density, and the entity's sprite (added when
    /// missing) is driven to show that texture, so the panel sits in the scene with its depth.
    /// When `interactive` is set, the pointer reaches the panel by a ray.
    struct GuiWorldPanelComponent
    {
        // Authored:
        resource::Ref<GuiDocument> document;
        resource::Ref<GuiStyleSheet> sheet; ///< optional; applied over the game's sheets to this panel alone
        Float2 sizeMeters{1.6f, 0.9f};           ///< the quad's extent in the world
        f32 pixelsPerMeter = 200.0f;             ///< the texture's density
        bool interactive = true;
        bool visible = true;

        // Runtime:
        RefPtr<Node> root;
        RefPtr<SceneRoot> renderRoot; ///< its own root, never in a scene's GUI
        RefPtr<Object> builtFrom;
        RefPtr<Object> sheetFrom;
        rhi::Texture* renderTexture = nullptr; ///< owned by the subsystem; null until first drawn
        rhi::TextureView* renderTextureView = nullptr;

        /// Returns the component's tree as a script node handle (`root()`), or null before the tree
        /// is built.
        [[nodiscard]] foundation::core::Variant rootNode() const;
    };

    inline void Serialize(ISerializer& ar, GuiWorldPanelComponent& c)
    {
        foundation::core::Serialize(ar, "document", c.document);
        foundation::core::Serialize(ar, "sheet", c.sheet);
        foundation::core::Serialize(ar, "sizeMeters", c.sizeMeters);
        foundation::core::Serialize(ar, "pixelsPerMeter", c.pixelsPerMeter);
        foundation::core::Serialize(ar, "interactive", c.interactive);
        foundation::core::Serialize(ar, "visible", c.visible);
    }

    inline void ResolveResources(resource::ResourceManager& manager, GuiWorldPanelComponent& c)
    {
        c.document.Bind(manager);
        c.sheet.Bind(manager);
    }

    class GuiWorldPanelComponentManager final : public scene::SerializableComponentManager<GuiWorldPanelComponent>
    {
    public:
        GuiWorldPanelComponentManager() : SerializableComponentManager<GuiWorldPanelComponent>(u8"gui.WorldPanel") {}
    };

    /// Returns the size of a world panel's texture in pixels: its extent at its density, kept
    /// within GPU-friendly limits.
    [[nodiscard]] Float2 WorldPanelPixels(const GuiWorldPanelComponent& panel) noexcept;

    /// Computes the world ray under `pointer` (in pixels) on a view of `viewSize` seen through
    /// `camera` (unjittered). The ray starts at the near plane and uses the engine's NDC depths.
    void GuiPointerRay(const render::ViewCamera& camera, Float2 pointer, Float2 viewSize, Float3& origin, Float3& direction) noexcept;

    struct GuiPanelHit
    {
        bool Hit = false;
        f32 Distance = 0.0f; ///< along the ray
        Float2 Uv{0.0f, 0.0f}; ///< 0 to 1 across the panel; v runs down, as GUI pixels do
    };
    /// Intersects the ray with a panel of `sizeMeters` in the plane of `panelWorld`, whose right
    /// and up axes span the panel. Either face can be hit.
    [[nodiscard]] GuiPanelHit RayHitGuiPanel(Float3 origin, Float3 direction, const Float4x4& panelWorld, Float2 sizeMeters) noexcept;

    /// Where a billboard shows in a view: the point it hangs from (its bottom centre), in the
    /// view's pixels, and its scale. The billboard is not shown when that point is behind the
    /// camera.
    struct GuiBillboardPlacement
    {
        bool Shown = false;
        Float2 Anchor{0.0f, 0.0f};
        f32 Scale = 1.0f;
    };
    /// Places `billboard`, on an entity at `entityWorld`, in a view of `viewport` pixels seen
    /// through `viewProjection` (unjittered) from `cameraPosition`.
    [[nodiscard]] GuiBillboardPlacement PlaceBillboard(const GuiBillboardComponent& billboard, const Float4x4& entityWorld,
                                                       const Float4x4& viewProjection, Float3 cameraPosition, Float2 viewport) noexcept;

    /// Registers the components' reflection, for the inspector and the scene format. Idempotent.
    void RegisterGuiComponentReflection();
    /// Registers the GUI's script surface: the GUI's own types (`gui`, the widgets, the gamekit)
    /// and each component's `.of(entity)`, with their names in the behaviour prelude. Idempotent;
    /// call it before a script manager binds the registry.
    void RegisterGuiScriptFacades();

    /// Adds the GUI's managers to a scene. The subsystem's scenes and headless scene consumers
    /// (Engine.Composition) both use it.
    inline void AddGuiSceneManagers(scene::Scene& scene)
    {
        scene.AddSystem<GuiCanvasComponentManager>();
        scene.AddSystem<GuiBillboardComponentManager>();
        scene.AddSystem<GuiWorldPanelComponentManager>();
    }

    /// Where a screen tier draws on its target. When it is not fitted, it draws plainly at the
    /// target's own size over the whole target. When it is fitted, the root lays out at `DrawSize`
    /// with display scale `DpiScale` and draws into `Rect` of the target through `Transform`, which
    /// maps the root's pixels onto the rectangle (identity unless the orientation rotates it).
    struct GuiScreenPlacement
    {
        bool Fitted = false;
        ContentRotation Turn = ContentRotation::None;
        Rectangle Rect{0.0f, 0.0f, 0.0f, 0.0f};
        Float2 DrawSize{0.0f, 0.0f};
        f32 DpiScale = 1.0f;
        Float4x4 Transform = Float4x4::Identity();
    };

    /// The GUI's resources by asset guid, through the application's resource manager.
    class GuiResourceProvider final : public IResourceProvider
    {
    public:
        /// Called when an image key starts or stops standing for a texture (`view` is null when it
        /// stops). The receivers are the renderers that draw the GUI.
        using TextureSink = Function<void(const image::ImageData* key, rhi::TextureView* view)>;

        explicit GuiResourceProvider(IAllocator& allocator);
        ~GuiResourceProvider() override;
        GuiResourceProvider(const GuiResourceProvider&) = delete;
        GuiResourceProvider& operator=(const GuiResourceProvider&) = delete;

        /// Sets the manager the guids resolve through; null (between projects) resolves nothing.
        /// Switching managers releases what the old one bound: an image key stays valid for
        /// whoever holds it, but stops showing. Borrowed.
        void SetResourceManager(resource::ResourceManager* resources);
        [[nodiscard]] resource::ResourceManager* GetResourceManager() const noexcept;
        /// Sets the renderers' sink, and tells it at once about every key that currently stands for
        /// a texture.
        void SetTextureSink(TextureSink sink);

        /// Returns a texture's image: one key per texture, with the texture's size as its natural
        /// size.
        [[nodiscard]] const image::ImageData* LoadImage(StringView path) override;
        /// Reads a document's markup or a sheet's CSS.
        [[nodiscard]] bool LoadText(StringView path, String& out) override;
        /// Returns a vector image's picture, parsed once per product.
        [[nodiscard]] const foundation::vg::svg::SVGDocument* LoadSvg(StringView path) override;

        /// Brings what was read in line with the current products (after a re-cook or a failed
        /// load). Returns true when a document or sheet read before has changed since the last poll.
        bool Poll();
        /// Returns whether a texture that an image asked for has arrived since the last call, and
        /// clears the flag. An image that asked before its texture loaded got nothing, and asks
        /// again the next time it is laid out.
        [[nodiscard]] bool TakeImagesArrived() noexcept
        {
            const bool arrived = m_imagesArrived;
            m_imagesArrived = false;
            return arrived;
        }

        /// Parses the guid `path` names ("{guid}" or bare); returns false when it names none.
        [[nodiscard]] static bool ParseGuid(StringView path, Guid& out) noexcept;

    private:
        struct State;
        UniquePtr<State> m_state;
        bool m_imagesArrived = false; // a texture an image asked for has arrived (TakeImagesArrived)
    };

    /// The guide to a game's touch controls, drawn over the game once a finger has touched the
    /// screen, so a desktop player never sees it. It shows every touch binding of the sets in effect
    /// (ActionRuntime::ForEachTouchControl). A button is a circle filling its zone, labelled with
    /// its action's name so that what is seen is what is tapped, and drawn stronger while held. An
    /// idle stick is a small ring low in its zone. A stick a finger holds is a ring where the finger
    /// came down and a knob where the finger is. Controls are drawn in a dark wash with a light
    /// edge, which reads on snow and at night alike. Regions are normalized over the guide. The
    /// guide takes no pointer input. Its bodies are in the implementation unit.
    class GuiTouchGuide final : public UIWidget
    {
    public:
        GuiTouchGuide();
        /// Gathers this frame's controls from every action runtime in play in `input`. `touched`
        /// says whether a finger has touched the screen.
        void Update(engine::input::InputSubsystem& input, bool touched);
        /// The controls shown (empty while hidden), for tests.
        [[nodiscard]] usize ControlCount() const noexcept { return m_controls.Size(); }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override;

    private:
        struct Shown
        {
            Rectangle region;  // normalized
            bool stick = false;
            bool down = false;
            bool held = false;
            Float2 anchor{};   // normalized
            Float2 finger{};
            f32 radius = 0.0f; // normalized, over the width
        };
        // Where a control's guide is drawn and how large, in the guide's units: a button fills its
        // zone; an idle stick is a small ring low in its zone, a third of the way in.
        [[nodiscard]] static Float2 GuideCentre(const Shown& shown, f32 w, f32 h) noexcept
        {
            const f32 across = shown.stick ? 0.3f : 0.5f;
            const f32 down = shown.stick ? 0.75f : 0.5f;
            return Float2{(shown.region.x + shown.region.width * across) * w, (shown.region.y + shown.region.height * down) * h};
        }
        [[nodiscard]] static f32 GuideRadius(const Shown& shown, f32 w, f32 h) noexcept
        {
            const f32 zoneW = shown.region.width * w;
            const f32 zoneH = shown.region.height * h;
            const f32 inscribed = (zoneW < zoneH ? zoneW : zoneH) * 0.5f;
            return shown.stick ? inscribed * 0.35f : (inscribed > 4.0f ? inscribed - 3.0f : inscribed);
        }
        // A control's name is sized at this share of its guide's diameter, but never below
        // kGuideMinFontSize.
        static constexpr f32 kGuideFontPerSize = 0.16f;
        static constexpr f32 kGuideMinFontSize = 15.0f;

        Array<Shown> m_controls;
        Array<RefPtr<Label>> m_labels; // one per control: its action's name
    };

    class GuiSubsystem final : public foundation::runtime::Subsystem,
                               public scene::ISceneObserver,
                               public render::ISceneOverlay,
                               public render::IScreenOverlay,
                               private foundation::run::IRunListener
    {
    public:
        /// `dataFileSystem` is the application's data mount, borrowed for the subsystem's life;
        /// the VG shaders and the built-in font come from it.
        GuiSubsystem(IAllocator& allocator, foundation::vfs::IFileSystem& dataFileSystem);
        ~GuiSubsystem() override;

        /// Runs early in the frame so a game's update sees the GUI's input; all of the work happens
        /// in BeginFrame.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -640; }

        [[nodiscard]] GUIContext& Context() noexcept { return *m_context; }
        [[nodiscard]] const RefPtr<GUIContext>& ContextRef() const noexcept { return m_context; }
        /// The style manager every root is styled through: the built-in theme, the game's sheets.
        [[nodiscard]] StyleManager& Styles() noexcept { return m_styles; }
        [[nodiscard]] GuiResourceProvider& Resources() noexcept { return *m_resources; }
        /// The fonts the GUI's text draws with.
        [[nodiscard]] foundation::fonts::IFontService* Fonts() const noexcept;

        /// Sets the project's fonts (defaultUiFontId, uiFontIds): the default and the others, each
        /// a family a label picks with `font-family`. Without a default the built-in font serves,
        /// and the others are not used until a default is set. The products are held.
        void SetFonts(const foundation::fonts::Font* defaultFont, Span<const foundation::fonts::Font* const> others);
        /// Counts the font services SetFonts replaced. They are kept until the GUI goes away,
        /// because a label keeps the fonts it was styled with until it is styled again, and a scene
        /// tier is styled only when its scene draws, which may never happen before the tier goes.
        /// Setting the same fonts again replaces nothing.
        [[nodiscard]] usize RetiredFontServices() const noexcept { return m_retiredFonts.Size(); }

        /// Where asset guids resolve (the application's resource manager); null resolves nothing.
        void SetResourceManager(resource::ResourceManager* resources);
        /// Sets the game's sheets, layered over the built-in theme, each over the ones before it (a
        /// game's own sheet over a shared one). They are read again when the cooked product of any
        /// of them changes.
        void SetStyleSheets(Span<const Guid> sheets);
        [[nodiscard]] Span<const Guid> StyleSheets() const noexcept { return Span<const Guid>(m_sheets.Data(), m_sheets.Size()); }
        /// How many times the game's sheets were applied (set, or a cooked product changed).
        [[nodiscard]] u64 SheetGeneration() const noexcept { return m_sheetGeneration; }

        /// The screen tier, outside any scene: its root, the gamekit's screen stack in it, and the
        /// overlay layer above the stack (a loading screen, a system menu).
        [[nodiscard]] SceneRoot* ScreenRoot() noexcept { return m_screen.Root.Get(); }
        [[nodiscard]] ScreenStack* Screens() noexcept { return m_screen.Screens.Get(); }

        // ---- the game's screen ----
        /// Lays the screen tier out at the game's render resolution, fitted into its target by
        /// `fit`, with its text kept crisp at the target's resolution. Zero on either axis means
        /// the target's own size.
        void SetScreenResolution(u32 width, u32 height, FitMode fit);
        [[nodiscard]] bool HasScreenResolution() const noexcept { return m_screen.HasResolution(); }
        /// Sets the game's orientation. On a target of the other shape, the screen tier lays out in
        /// the game's shape and draws rotated a quarter turn. Any never rotates.
        void SetScreenOrientation(ScreenOrientation orientation) noexcept { m_screen.Orientation = orientation; }
        /// Computes where a tier at `resolution` (0 x 0 for none), fitted by `fit` and held to
        /// `orientation`, draws on a target of size `target`.
        [[nodiscard]] static GuiScreenPlacement PlaceScreen(Float2 resolution, FitMode fit, ScreenOrientation orientation, Float2 target) noexcept;

        // ---- a screen tier per run (the editor's Game tabs) ----
        /// Turns per-run screen tiers on or off. When on (the editor sets it once), each run gets
        /// its own screen tier; when off (the player), there is one shared tier.
        void SetRunScreens(bool enabled) noexcept { m_runScreens = enabled; }
        [[nodiscard]] bool RunScreens() const noexcept { return m_runScreens; }
        /// Returns `run`'s screen stack, making the run's tier and root on first use. With run
        /// screens off, or with no run, it returns the shared tier's stack. A run's tier and every
        /// screen on it go away with the run's session (when the run stops, or when the run goes
        /// without stopping), as the run manager announces it.
        [[nodiscard]] ScreenStack* ScreensFor(foundation::run::IRun* run);
        [[nodiscard]] SceneRoot* ScreenRootFor(foundation::run::IRun* run);
        void SetScreenResolution(foundation::run::IRun* run, u32 width, u32 height, FitMode fit);
        void SetScreenOrientation(foundation::run::IRun* run, ScreenOrientation orientation);
        /// Sets the run whose screen tier the next screen overlay draws, beneath the shared tier's
        /// overlays (null for none). A host sets it around its overlays for that run's view.
        void SetRenderRun(foundation::run::IRun* run) noexcept { m_renderRun = run != nullptr ? run->Id() : 0; }
        [[nodiscard]] usize RunScreenCount() const noexcept { return m_runTiers.Size(); }
        [[nodiscard]] Node* OverlayLayer() noexcept { return m_overlay.Get(); }
        /// Returns whether the GUI that input reaches is editing text (a field has the focus), as of
        /// the last input pump. A host that owns the window's text input (the editor's Game tab)
        /// starts text input when this is true.
        [[nodiscard]] bool WantsTextInput() const noexcept { return m_textWanted; }
        /// The touch controls' guide, beneath the shared tier's screens; exposed for tests.
        [[nodiscard]] GuiTouchGuide* TouchGuide() noexcept { return m_touchGuide.Get(); }
        /// Builds `document` and shows it on the overlay layer, above every screen (a loading
        /// screen, a system menu). It is modal while it shows, unless it takes no pointer
        /// (`pointer-events: none` on its root, as for a watermark). Returns the tree, so the caller
        /// can take it off again, or null when the document does not build.
        RefPtr<Node> PushOverlay(const GuiDocument& document);
        /// Builds `document`'s tree in the GUI's context (its resources and fonts, with its
        /// `{{values}}` bound to the GUI's data) without placing it anywhere, so a native host can
        /// push it as a screen on Screens(). Returns null when the document does not build.
        [[nodiscard]] RefPtr<Node> BuildDocument(const GuiDocument& document);
        /// Sets the action runtime whose bindings the GUI's prompts (GUI.Gamekit's Prompt) name.
        /// The default application sets the runtime of its primary game instance; null means the
        /// input subsystem's own runtime. Changes to its bindings (a map loaded, a rebinding, a set
        /// enabled) are seen at the next frame, and every prompt reads them again.
        void SetPromptRuntime(const foundation::input::ActionRuntime* runtime) noexcept { m_promptRuntime = runtime; }
        [[nodiscard]] const foundation::input::ActionRuntime* PromptRuntime() const noexcept;
        [[nodiscard]] const InputPrompts& Prompts() const noexcept { return m_prompts; }
        /// Shows a tree made in code on the overlay layer.
        void PushOverlay(Node& overlay);
        void RemoveOverlay(Node& overlay);
        [[nodiscard]] usize OverlayCount() const noexcept { return m_overlay ? m_overlay->ChildCount() : 0; }
        /// The scene tier's root for `scene`; null when the scene is unknown.
        [[nodiscard]] SceneRoot* SceneRootFor(const scene::Scene& scene) noexcept;

        // ---- lifecycle ----
        void OnInit() override;
        void OnReady() override;
        void OnShutdown() override;
        void BeginFrame(f32 deltaTime) override;
        /// Clears the action layer's pointer and keyboard claims after the frame has read them. The
        /// next frame's BeginFrame lane publishes them again: an earlier subsystem's claim first,
        /// with the GUI's claim added to it.
        void EndFrame() override;

        // ---- scripts ----
        /// Gives the scripts of `scriptContext` the screen tier's root (`gui::root()`).
        void ExposeToScript(foundation::script::IScriptContext& scriptContext);

        // ---- input ----
        /// Sets the window whose platform text input (IME) follows the GUI's focus (the player's
        /// main window). Null leaves text input alone, as in an editor, whose own bridge owns it.
        void SetTextInputTarget(foundation::shell::IWindow* window);
        /// The root this frame's input went to; null before any input.
        [[nodiscard]] SceneRoot* ActiveInputRoot() const noexcept { return m_activeRoot; }
        /// Whether the GUI took the pointer this frame (what the action layer was told).
        [[nodiscard]] bool PointerOverGui() const noexcept { return m_pointerTaken; }
        /// Tracks the input kind (the pointer, the keys, a pad) from the active source each frame.
        [[nodiscard]] foundation::input::InputKindTracker& GetInputKindTracker() noexcept { return m_inputKind; }

        // ---- the scene tier ----
        void OnSystemsReady(scene::Scene& scene) override;
        void OnDestroying(scene::Scene& scene) override;

        // ---- the overlay roles ----
        /// Draws above an overlay at the default order (0).
        [[nodiscard]] i32 OverlayOrder() const noexcept override { return 10; }
        /// Draws the scene tier: the view's scene root, laid out at the view's viewport.
        void Render(rhi::RenderPassEncoder& encoder, const render::SceneOverlayView& view) override;
        /// Prepares the screen tier before the pass: runs the frame's bakes, then records the screen
        /// root with its offscreen layers on the host's encoder.
        void PrepareOverlay(rhi::CommandEncoder& encoder, const render::ScreenOverlayView& view) override;
        /// Draws the screen tier: what PrepareOverlay recorded for this view, or, when it recorded
        /// nothing, records the tier now.
        void Render(rhi::RenderPassEncoder& encoder, const render::ScreenOverlayView& view) override;

        // ---- an editor's preview ----
        /// Builds `markup` for an editor's page in the GUI's own context (the game's fonts, theme
        /// and sheets), in a root of its own that is never in a tier and takes no input. `css`, when
        /// given, is a sheet applied over this preview alone (for a sheet's page). What the loader
        /// dropped goes to `diagnostics`. Returns null when the markup does not build.
        [[nodiscard]] RefPtr<SceneRoot> CreatePreview(StringView markup, StringView css = {},
                                                           Array<GuiDiagnostic>* diagnostics = nullptr);
        /// Lays `root` out at `width` x `height` and draws it over the contents of `target`, in a
        /// pass of its own on `encoder`. `target` must be in RenderTarget state and is left in it.
        void RenderPreview(SceneRoot& root, rhi::CommandEncoder& encoder, rhi::TextureView* target, rhi::TextureFormat format,
                           u32 width, u32 height, u32 frameIndex);

        /// Sets up the GPU side: loads the VG shaders from the data mount and makes the renderer.
        /// Whoever owns graphics (the application's startup) calls it. Idempotent; until it runs,
        /// the overlays draw nothing.
        void EnsureRenderReady(rhi::Device& device, i32 frameCount);
        /// Draws the GUI textures. It runs the frame's bakes, then draws every render-texture canvas
        /// and world panel into its texture, making or resizing the texture as needed and leaving
        /// it ready to sample, and points each entity's sprite (and a canvas's decal) at it. The
        /// host calls it on its encoder before the scene draws, so the scene samples this frame's
        /// textures. It runs once a frame; a second call does nothing.
        void RenderTextures(rhi::CommandEncoder& encoder, u32 frameIndex);
        /// Counts the textures that canvases and panels currently draw into.
        [[nodiscard]] usize TextureCount() const noexcept;
        [[nodiscard]] bool IsRenderReady() const noexcept;

    private:
        struct RenderState; // the renderer and the shaders (impl unit)
        struct SceneTier
        {
            scene::Scene* Scene = nullptr;
            RefPtr<SceneRoot> Root;
            UniquePtr<GuiInputBridge> Bridge;
            Array<RefPtr<UIWidget>> Canvases; ///< its canvases' hosts, in the root
            RefPtr<UIWidget> Billboards;      ///< the layer, under the canvases, that holds the billboards
        };

        void ApplySheets();
        void ApplyUserAgentSheet();
        void PumpInput();
        void SyncCanvases(SceneTier& tier);
        void SyncBillboards(SceneTier& tier);
        void PlaceBillboards(SceneTier& tier, const render::SceneOverlayView& view);
        [[nodiscard]] static bool HasContent(const SceneTier& tier) noexcept;
        void SyncPanels(SceneTier& tier);
        // Keeps a component's tree in line with its document and sheet, rebuilding it when the
        // document's product changes.
        template <typename Component>
        bool RebuildIfChanged(Component& c);
        template <typename Component>
        void ApplySheet(Component& c, StringView name);
        // A root of its own for a texture (a render-texture canvas, a world panel), with its input.
        struct TextureRoot
        {
            scene::Scene* Scene = nullptr;
            scene::EntityHandle Entity{};
            RefPtr<SceneRoot> Root;
            UniquePtr<GuiInputBridge> Bridge;
            bool Seen = false;
        };
        RefPtr<SceneRoot> TextureRootFor(scene::Scene& scene, scene::EntityHandle entity, Node& content);
        // Where the pointer goes this frame: a root, and the point in it (for a panel, in the panel's
        // pixels).
        struct InputTarget
        {
            SceneRoot* Root = nullptr;
            Float2 Point{0.0f, 0.0f}; ///< in the root's pixels
            bool Mapped = false;      ///< the point was mapped from the pointer (into a panel or a fitted tier)
            bool Panel = false;
        };
        // A screen tier: a root holding a screen stack (the shared tier also holds the overlay
        // layer above it), its input, and the game's screen it lays out for.
        struct ScreenTier
        {
            foundation::run::RunId Run = 0; ///< 0: the shared tier
            RefPtr<SceneRoot> Root;
            RefPtr<ScreenStack> Screens;
            UniquePtr<GuiInputBridge> Bridge;
            Float2 Resolution{0.0f, 0.0f};
            FitMode Fit = FitMode::Letterbox;
            ScreenOrientation Orientation = ScreenOrientation::Any;
            Float2 TargetSize{0.0f, 0.0f}; ///< the target it last drew into
            ContentRotation Turn = ContentRotation::None;
            [[nodiscard]] bool HasResolution() const noexcept { return Resolution.x > 0.0f && Resolution.y > 0.0f; }
        };
        void MakeScreenTier(ScreenTier& tier);
        static void ApplyResolution(ScreenTier& tier, u32 width, u32 height, FitMode fit);
        [[nodiscard]] ScreenTier* FindRunTier(foundation::run::RunId run) noexcept;
        [[nodiscard]] ScreenTier* FindRunTier(const foundation::run::IRun* run) noexcept
        {
            return run != nullptr ? FindRunTier(run->Id()) : nullptr;
        }
        // Removes the run's tier and every screen on it.
        void EndRunScreens(foundation::run::RunId run);
        void OnRunStopping(foundation::run::IRun& run) override { EndRunScreens(run.Id()); }
        void OnRunDestroyed(foundation::run::IRun& run) override { EndRunScreens(run.Id()); }
        [[nodiscard]] ScreenTier* TierOfRoot(const SceneRoot* root) noexcept;
        // Returns the tier input reaches: the tier of the run whose scene the input is bound to, when
        // that run has one.
        [[nodiscard]] ScreenTier& InputTier() noexcept;
        // Maps a render-space point into the tier's root pixels, removing the crop's offset and
        // applying its scale.
        [[nodiscard]] static Float2 ScreenPointerPoint(const ScreenTier& tier, Float2 point) noexcept;
        [[nodiscard]] InputTarget PanelUnderPointer(Float2 point, bool centreAim);
        [[nodiscard]] InputTarget PickInputRoot(bool pointer, Float2 point, bool centreAim);
        [[nodiscard]] bool InputReaches(const SceneTier& tier) const noexcept;
        [[nodiscard]] GuiInputBridge* BridgeOf(const SceneRoot* root) noexcept;
        [[nodiscard]] UniquePtr<GuiInputBridge> MakeBridge(SceneRoot& root);
        void StyleAndLayOut(SceneRoot& root, Float2 size);
        [[nodiscard]] bool ReadDataFile(StringView path, Array<u8>& out) const;

        TaggedAllocator m_allocator;
        foundation::vfs::IFileSystem* m_dataFileSystem; // borrowed
        RefPtr<GUIContext> m_context;
        StyleManager m_styles;
        UniquePtr<GuiResourceProvider> m_resources;
        UniquePtr<foundation::fonts::IFontService> m_fonts;        // the built-in font
        UniquePtr<foundation::fonts::IFontService> m_projectFonts; // the project's fonts, when it names a default
        // Services SetFonts replaced, kept until the GUI goes (see RetiredFontServices). A label
        // holds its font from the service it was styled with, and input may lay a root out (a hit
        // test) before the frame's style pass gives the label the new one.
        Array<UniquePtr<foundation::fonts::IFontService>> m_retiredFonts;
        RefPtr<foundation::fonts::Font> m_defaultFont;
        Array<RefPtr<foundation::fonts::Font>> m_otherFonts;
        Array<Guid> m_sheets;
        u64 m_sheetGeneration = 0;
        ScreenTier m_screen;                     // the shared tier
        Array<UniquePtr<ScreenTier>> m_runTiers; // a tier per run, with run screens on
        bool m_runScreens = false;
        foundation::run::RunId m_renderRun = 0;
        foundation::run::IRunManager* m_runManager = nullptr; // subscribed to (OnReady .. OnShutdown)
        RefPtr<UIWidget> m_overlay;
        RefPtr<GuiTouchGuide> m_touchGuide; // beneath the shared tier's screens
        bool m_touchSeen = false;           // set once a finger has touched the screen; the guide then shows
        GuiScriptBinding m_scriptBinding; // what each script context is given
        Array<SceneTier> m_sceneTiers;
        Array<UniquePtr<TextureRoot>> m_textureRoots;
        u64 m_frameSerial = 0;
        u64 m_texturesSerial = ~0ull; // the frame RenderTextures last ran in
        Float2 m_panelPoint{0.0f, 0.0f}; // where a held press on a panel last was, in its pixels
        UniquePtr<RenderState> m_render;
        render::ISceneRenderer* m_sceneRenderer = nullptr; // where the overlays are registered
        render::IScreenRenderer* m_screenRenderer = nullptr;
        engine::input::InputSubsystem* m_input = nullptr;
        // The game kit's prompts, installed on the GUI's context. They read m_promptRuntime (or the
        // input subsystem's runtime when it is null) and are refreshed when its bindings' version
        // changes.
        InputPrompts m_prompts;
        const foundation::input::ActionRuntime* m_promptRuntime = nullptr;
        const foundation::input::ActionRuntime* m_promptsRead = nullptr;
        u64 m_promptsVersion = 0;
        void SyncPrompts();
        foundation::input::InputKindTracker m_inputKind;
        foundation::shell::IWindow* m_textInputTarget = nullptr;
        SceneRoot* m_activeRoot = nullptr; // the root input went to last
        SceneRoot* m_pressRoot = nullptr;  // the root a held press began in
        bool m_pressTaken = false;              // that press began over GUI that takes the pointer
        bool m_pointerTaken = false;
        bool m_textWanted = false;              // the root input went to is editing text
    };

    /// The GUI's domain: its resource modules (documents and sheets, the vector images, the
    /// fonts).
    [[nodiscard]] const engine::DomainModule& GuiDomain() noexcept;

    /// The aspect (width over height) of the AssiduousEngine logo with its wordmark.
    inline constexpr f32 kEngineLogoAspect = 931.0f / 200.0f;

    /// The loading screen a game shows while its first scene loads, when the game authors none (its
    /// guiLoadingDocumentId is unset). It shows the AssiduousEngine logo (`logoSvg`, the SVG text;
    /// left out when empty or unreadable) over a progress bar and "Loading", centred on a dark
    /// screen. The player drives the `Progress` bar (0 to 1).
    struct DefaultLoadingScreen
    {
        RefPtr<Node> Root;
        ProgressBar* Progress = nullptr; // in Root
    };
    /// Builds the default loading screen. It is defined in the implementation unit because a body
    /// here leaves GCC unable to read the module back.
    [[nodiscard]] DefaultLoadingScreen BuildDefaultLoadingScreen(IAllocator& allocator, StringView logoSvg);
}
