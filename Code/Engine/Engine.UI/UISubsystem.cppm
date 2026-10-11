// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::UI - the `engine.ui` module.
//
// The game screen tier: UICanvasComponents reference cooked UIDocuments; the subsystem
// owns ONE UIContext (GameTheme default stylesheet; core controls only - never the
// toolkit), instantiates a fresh view tree per canvas, lays out against the render
// target, draws through the VG renderer into an overlay pass AFTER the scene (the
// player's backbuffer and the editor Game tab's viewport texture alike), and feeds UI
// input from the SAME device facades the action layer reads - publishing the
// pointer/keyboard consumption mask so UI-consumed input never reaches gameplay
// actions (raw facades stay unfiltered). UI ticks on UNSCALED time (menus animate
// while the game is paused), which is why the work runs in the BeginFrame lane.
//
// GCC modules hygiene: render/VG/shader contact + the REFLECT_* bodies live in
// UISubsystemImpl.cpp (implementation unit), same split as physics.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module engine.ui;

import foundation.core;
import engine.domain;
import foundation.runtime;
import foundation.scene;
import engine.scene;
import foundation.resource;
import foundation.shell;
import foundation.rhi;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.resource;
import foundation.vfs; // IFileSystem: the application's data mount (shaders + the default font)
import foundation.input;
import engine.input;
import foundation.render.api; // the two-tier overlay roles (ISceneOverlay/IScreenOverlay)
import foundation.ui;
import foundation.ui.shell; // UiInputBridge (key/text mapping + IME lifecycle)
import foundation.ui.gamekit; // ScreenStack over the screen-tier RootView
import foundation.ui.resource;
import engine.ui.script; // ViewGroup: a component's instantiated tree as a script handle
import foundation.image; // ImageData (the texture images an ImageView names)
import foundation.script;         // Object / IScriptContext / IScriptDelegate / the run-context service
import foundation.script.facades; // RegisterExtraFacadeName (the behavior-module prelude hook)

using namespace foundation::core;
using namespace foundation::ui;
namespace core = foundation::core;
namespace rhi = foundation::rhi;

export namespace engine::ui
{
    // Foundation aliases (sibling engine::* namespaces would otherwise shadow these).
    namespace render = foundation::render;
    namespace scene = foundation::scene;
    namespace script = foundation::script;

    namespace scene = foundation::scene;
    namespace script = foundation::script;


    enum class CanvasScalerMode : u8
    {
        ConstantPixel = 0,   // 1 UI px = 1 target px
        ReferenceResolution, // uniform-scale so referenceResolution fits the target
    };

    enum class CanvasRenderMode : u8
    {
        ScreenOverlay = 0, // drawn in the scene-overlay pass (the screen tier)
        RenderTexture,     // drawn into an offscreen texture (in-world screens)
    };

    // A screen-space UI canvas on an entity: menus/HUD ride in scenes and prefabs
    // (spawn/despawn = open/close). renderMode is implicitly ScreenOverlay.
    struct UICanvasComponent
    {
        // Authored:
        foundation::resource::Ref<UIDocument> document;
        foundation::resource::Ref<UITheme> theme; // optional override (nil = context theme)
        i32 order = 0;                          // draw/dispatch order (higher = on top)
        bool visible = true;
        bool interactive = true;
        CanvasScalerMode scalerMode = CanvasScalerMode::ConstantPixel;
        Float2 referenceResolution{1920.0f, 1080.0f};
        CanvasRenderMode renderMode = CanvasRenderMode::ScreenOverlay;
        u32 renderTextureWidth = 512; // RenderTexture mode: target size (px)
        u32 renderTextureHeight = 512;

        // Runtime (transient):
        RefPtr<View> root;                     // instantiated tree (template = document)
        // The instantiated tree as a script group handle (`root()` to scripts), whose finders reach
        // the views inside, as `ui::root()` reaches the HUD's. Null-valid before the tree is built.
        [[nodiscard]] engine::uiscript::ViewGroup rootGroup() const;
        RefPtr<ViewGroup> host;                // per-canvas host in the scene root (order + scaler)
        RefPtr<RootView> renderRoot;           // RenderTexture mode: standalone root (never a tier)
        const UIDocument* builtFrom = nullptr; // rebuild detector (hot reload)
        RefPtr<StyleSheet> themeSheet;         // parsed override (built on theme change)
        const UITheme* themeFrom = nullptr;
        // RenderTexture mode accessors (subsystem-owned GPU objects, refreshed by
        // RenderCanvasTextures; null until the first render / outside the mode).
        rhi::Texture* renderTexture = nullptr;
        rhi::TextureView* renderTextureView = nullptr;
    };

    inline void Serialize(ISerializer& ar, UICanvasComponent& c)
    {
        foundation::core::Serialize(ar, "document", c.document);
        foundation::core::Serialize(ar, "theme", c.theme);
        foundation::core::Serialize(ar, "order", c.order);
        foundation::core::Serialize(ar, "visible", c.visible);
        u8 scaler = static_cast<u8>(c.scalerMode);
        foundation::core::Serialize(ar, "scalerMode", scaler);
        c.scalerMode = static_cast<CanvasScalerMode>(scaler);
        foundation::core::Serialize(ar, "referenceResolution", c.referenceResolution);
        foundation::core::Serialize(ar, "interactive", c.interactive);
        u8 render = static_cast<u8>(c.renderMode);
        foundation::core::Serialize(ar, "renderMode", render);
        c.renderMode = static_cast<CanvasRenderMode>(render);
        foundation::core::Serialize(ar, "renderTextureWidth", c.renderTextureWidth);
        foundation::core::Serialize(ar, "renderTextureHeight", c.renderTextureHeight);
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager, UICanvasComponent& c)
    {
        c.document.Bind(manager);
        c.theme.Bind(manager);
    }

    class UICanvasComponentManager final
        : public scene::SerializableComponentManager<UICanvasComponent>
    {
    public:
        UICanvasComponentManager() : SerializableComponentManager<UICanvasComponent>(u8"ui.Canvas")
        {
        }
    };

    // ---- billboards (nameplates/health bars) - the Sedulous reference's
    // best-behaved tier, ported as-is: ONE shared layer under the canvases, one VG
    // batch; world position -> clip -> screen px; behind-camera anchors park off-screen
    // (clipped + unhit, no tree churn); distance scaling as a 2D view-transform. ----

    enum class BillboardOrientation : u8
    {
        Screen = 0,  // offset in ENTITY-LOCAL space (rides the entity's rotation)
        Cylindrical, // offset in WORLD space (a fixed lift above the anchor)
    };
    enum class BillboardScale : u8
    {
        Fixed = 0,
        Distance
    };

    struct UIBillboardComponent
    {
        // Authored:
        foundation::resource::Ref<UIDocument> document;
        Float3 offset{0.0f, 0.0f, 0.0f};
        BillboardOrientation orientation = BillboardOrientation::Cylindrical;
        BillboardScale scaleMode = BillboardScale::Fixed;
        f32 referenceDistance = 10.0f; // Distance mode: scale = clamp(ref/dist, min, max)
        f32 minScale = 0.3f;
        f32 maxScale = 2.0f;
        bool visible = true;

        // Runtime (transient):
        RefPtr<View> root;
        const UIDocument* builtFrom = nullptr;

        // The instantiated tree as a script group handle (`root()`), as UICanvasComponent's.
        [[nodiscard]] engine::uiscript::ViewGroup rootGroup() const;
    };

    inline void Serialize(ISerializer& ar, UIBillboardComponent& c)
    {
        foundation::core::Serialize(ar, "document", c.document);
        foundation::core::Serialize(ar, "offset", c.offset);
        u8 orientation = static_cast<u8>(c.orientation);
        foundation::core::Serialize(ar, "orientation", orientation);
        c.orientation = static_cast<BillboardOrientation>(orientation);
        u8 scale = static_cast<u8>(c.scaleMode);
        foundation::core::Serialize(ar, "scaleMode", scale);
        c.scaleMode = static_cast<BillboardScale>(scale);
        foundation::core::Serialize(ar, "referenceDistance", c.referenceDistance);
        foundation::core::Serialize(ar, "minScale", c.minScale);
        foundation::core::Serialize(ar, "maxScale", c.maxScale);
        foundation::core::Serialize(ar, "visible", c.visible);
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager,
                                 UIBillboardComponent& c)
    {
        c.document.Bind(manager);
    }

    class UIBillboardComponentManager final
        : public scene::SerializableComponentManager<UIBillboardComponent>
    {
    public:
        UIBillboardComponentManager()
            : SerializableComponentManager<UIBillboardComponent>(u8"ui.Billboard")
        {
        }
    };

    // ---- world tier (RT-quad panels) ----
    // A UI document ON A SURFACE IN THE WORLD: the panel renders its tree into an
    // offscreen target sized by PIXELS-PER-METER (uniform density) and drives a
    // sibling SpriteComponent (EntityOriented - the entity's plane) with it, so the
    // panel is ordinary scene content: depth, occlusion, TAA and post are correct by
    // construction. UNLIT by design. Interactive panels take the pointer via a camera
    // ray (the pump: ray -> nearest panel -> UV -> pixel injection into the panel's
    // standalone root), under the same per-surface scene-binding rules as every tier.
    struct UIWorldPanelComponent
    {
        // Authored:
        foundation::resource::Ref<UIDocument> document;
        foundation::resource::Ref<UITheme> theme; // optional override (nil = context theme)
        Float2 sizeMeters{1.6f, 0.9f};          // world extent of the quad
        f32 pixelsPerMeter = 200.0f;            // texture density (target = size * ppm)
        bool interactive = true;
        bool visible = true;

        // Runtime (transient):
        RefPtr<View> root;
        RefPtr<RootView> renderRoot; // standalone (never a tier root)
        const UIDocument* builtFrom = nullptr;
        RefPtr<StyleSheet> themeSheet;
        const UITheme* themeFrom = nullptr;
        rhi::Texture* renderTexture = nullptr; // subsystem-owned (accessors)
        rhi::TextureView* renderTextureView = nullptr;

        // The instantiated tree as a script group handle (`root()`), as UICanvasComponent's: a
        // meter over a guard's head is filled through it.
        [[nodiscard]] engine::uiscript::ViewGroup rootGroup() const;
    };

    inline void Serialize(ISerializer& ar, UIWorldPanelComponent& c)
    {
        foundation::core::Serialize(ar, "document", c.document);
        foundation::core::Serialize(ar, "theme", c.theme);
        foundation::core::Serialize(ar, "sizeMeters", c.sizeMeters);
        foundation::core::Serialize(ar, "pixelsPerMeter", c.pixelsPerMeter);
        foundation::core::Serialize(ar, "interactive", c.interactive);
        foundation::core::Serialize(ar, "visible", c.visible);
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager,
                                 UIWorldPanelComponent& c)
    {
        c.document.Bind(manager);
        c.theme.Bind(manager);
    }

    class UIWorldPanelComponentManager final
        : public scene::SerializableComponentManager<UIWorldPanelComponent>
    {
    public:
        UIWorldPanelComponentManager()
            : SerializableComponentManager<UIWorldPanelComponent>(u8"ui.WorldPanel")
        {
        }
    };

    // ---- world-panel pointer math (pure; unit-tested) ----

    /// The world-space pointer ray for `pointerPx` on a view of `viewSize`, through
    /// `camera` (unjittered). The near and far ends are the engine's NDC depths
    /// (core::projection - reverse-Z), never literals.
    inline void PointerRayFromCamera(const render::ViewCamera& camera, Float2 pointerPx,
                                     Float2 viewSize, Float3& outOrigin, Float3& outDirection)
    {
        const Float4x4 inverseViewProjection = Inverse(camera.ViewProjection());
        const f32 ndcX = (pointerPx.x / viewSize.x) * 2.0f - 1.0f;
        const f32 ndcY = 1.0f - (pointerPx.y / viewSize.y) * 2.0f;
        auto unproject = [&](f32 z)
        {
            const Float4 clip = Float4{ndcX, ndcY, z, 1.0f} * inverseViewProjection;
            const f32 w = clip.w != 0.0f ? clip.w : 1.0f;
            return Float3{clip.x / w, clip.y / w, clip.z / w};
        };
        outOrigin = unproject(projection::kNdcDepthNear);
        const Float3 far = unproject(projection::kNdcDepthFar);
        outDirection =
            Normalized(Float3{far.x - outOrigin.x, far.y - outOrigin.y, far.z - outOrigin.z});
    }

    struct WorldPanelHit
    {
        bool hit = false;
        f32 distance = 0.0f;   // along the ray (world units)
        Float2 uv{0.0f, 0.0f}; // 0..1 across the panel (v down, matching UI pixels)
    };

    /// Ray vs the panel's plane (entity world right/up span the quad; both faces hit).
    [[nodiscard]] inline WorldPanelHit RayHitWorldPanel(Float3 rayOrigin, Float3 rayDirection,
                                                        const Float4x4& panelWorld,
                                                        Float2 sizeMeters)
    {
        WorldPanelHit result;
        const Float3 center{panelWorld.m[3][0], panelWorld.m[3][1], panelWorld.m[3][2]};
        const Float3 right =
            Normalized(Float3{panelWorld.m[0][0], panelWorld.m[0][1], panelWorld.m[0][2]});
        const Float3 up =
            Normalized(Float3{panelWorld.m[1][0], panelWorld.m[1][1], panelWorld.m[1][2]});
        const Float3 normal = Cross(right, up);
        const f32 denominator = Dot(rayDirection, normal);
        if (Abs(denominator) < 1.0e-6f)
        {
            return result;
        } // parallel
        const Float3 toCenter{center.x - rayOrigin.x, center.y - rayOrigin.y,
                              center.z - rayOrigin.z};
        const f32 t = Dot(toCenter, normal) / denominator;
        if (t <= 0.0f)
        {
            return result;
        } // behind the pointer
        const Float3 point{rayOrigin.x + rayDirection.x * t, rayOrigin.y + rayDirection.y * t,
                           rayOrigin.z + rayDirection.z * t};
        const Float3 local{point.x - center.x, point.y - center.y, point.z - center.z};
        const f32 x = Dot(local, right);
        const f32 y = Dot(local, up);
        if (Abs(x) > sizeMeters.x * 0.5f || Abs(y) > sizeMeters.y * 0.5f)
        {
            return result;
        }
        result.hit = true;
        result.distance = t;
        result.uv = Float2{x / sizeMeters.x + 0.5f, 0.5f - y / sizeMeters.y};
        return result;
    }

    void RegisterUIComponentReflection();

    // Register the WORLD-space UI components' script `.of` facades (UICanvasComponent /
    // UIBillboardComponent / UIWorldPanelComponent) - reflection + registry + prelude names. Distinct
    // from the screen-tier `ui` facade (engine.ui.script): these are per-entity component surfaces.
    // Idempotent; the composition root + the app both call it. Distinct from the screen-tier
    // facade in engine.ui.script.
    void RegisterUiComponentScriptFacades();

    // THE game-UI manager set for a scene - injected by the subsystem at runtime AND by headless
    // scene consumers (Engine.Composition). The per-scene root-view plumbing is runtime-only and
    // stays with the subsystem. A manager added here reaches the composition (Engine.Composition)
    // automatically.
    inline void AddUISceneManagers(scene::Scene& scene)
    {
        scene.AddSystem<UICanvasComponentManager>();
        scene.AddSystem<UIBillboardComponentManager>();
        scene.AddSystem<UIWorldPanelComponentManager>();
    }

    class UISubsystem final : public foundation::runtime::Subsystem,
                              public scene::ISceneObserver,
                              public foundation::render::ISceneOverlay,
                              public foundation::render::IScreenOverlay
    {
    public:
        // Everything the game-UI subsystem allocates rolls up under the GameUI tag.
        // `dataFileSystem` is the application's data mount (borrowed for the subsystem's
        // lifetime): the VG shaders and the built-in default font come from it.
        UISubsystem(IAllocator& allocator,
                    foundation::vfs::IFileSystem& dataFileSystem); // defined in the impl unit
        ~UISubsystem() override; // defined in the impl unit (RenderState is opaque here)

        /// Before the scene subsystem so canvas visibility/trees are current for pages;
        /// lane choice matters more than order: ALL work runs in BeginFrame (raw dt).
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -650; }

        /// The window whose platform text input (IME) follows GAME UI focus: when the
        /// context's WantsTextInput() turns on/off (an EditText gains/loses focus), the
        /// pump starts/stops the window's text input. The PLAYER sets its main window;
        /// hosts whose IME another bridge owns (the editor - its UIHost reconciles from
        /// the EDITOR context, with ViewportView forwarding the game's wish) leave it
        /// null. Null also clears it.
        void SetTextInputTarget(foundation::shell::IWindow* window) noexcept
        {
            m_bridge.SetTextInputTarget(window);
        }

        [[nodiscard]] UIContext& Context() noexcept { return m_context; }

        /// The project-default UITheme (cooked .sss): parsed with the game palette and
        /// set as the context's stylesheet. Null / empty / parse failure falls back to
        /// the built-in GameTheme. Hosts call it at startup from the project manifest's
        /// defaultUiThemeId (player + editor); per-canvas theme overrides layer on top.
        void SetDefaultTheme(const UITheme* theme);
        /// The project-default FONT (a cooked, GUID-addressable fonts::Font product bound
        /// through the ResourceManager - the fonts-triad path). Swaps the context's font
        /// service to a ResourceFontService over the product's baked entries; null restores
        /// the TTF fallback service. Hosts call it at startup from the manifest's
        /// defaultUiFontId (player + editor), exactly like SetDefaultTheme. The product is
        /// BORROWED (the ResourceManager's cache keeps it alive).
        void SetDefaultFont(const foundation::fonts::Font* font);
        /// The project's other UI fonts, each its own family beside the default, which a label
        /// picks by `font-family` (a title face over the body text). Held like the default. They
        /// need a default font bound: the TTF fallback serves only its built-in face.
        void SetExtraFonts(Span<const foundation::fonts::Font* const> fonts);
        /// Where the game UI's images come from: an ImageView's `source` (and a stylesheet's
        /// image()) is a texture asset id, bound through `resources`; a render texture a camera
        /// draws into is a texture like any other. The application points it at its resource
        /// manager; null (between projects) resolves nothing. Borrowed.
        void SetResourceManager(foundation::resource::ResourceManager* resources);
        /// The screen tier at the game's render resolution: it lays out at `width` x `height` and
        /// draws, at the target's own resolution, into the rectangle `fit` puts that size in, so
        /// its text stays crisp at any window size. The pointer then arrives in render space, as
        /// the game's does. Nought on either axis goes back to the target's own size.
        void SetScreenResolution(u32 width, u32 height, FitMode fit);
        [[nodiscard]] bool HasScreenResolution() const noexcept { return m_screen.HasResolution(); }
        /// The scene-LESS screen tier's root (global overlays only; scene UI lives in
        /// per-scene roots - see SceneRoot).
        [[nodiscard]] RootView* ScreenRoot() noexcept { return m_screen.root.Get(); }
        /// The screen-tier ScreenStack: push/pop/replace of UIScreens over the screen
        /// root. Tier-owned so its lifetime matches the root. Backs the `ui` script facade's screen
        /// management (engine.ui.script installs a service pointing at this + ScreenRoot()).
        [[nodiscard]] foundation::ui::gamekit::ScreenStack& Screens() noexcept { return m_screen.stack; }

        // ---- a screen tier per run (the editor's Game tabs) ----
        // The player runs one game, so its runs share the one screen tier. The editor runs a
        // game per Game tab inside one application: with run screens on, each run (a run key,
        // as Scene::Run and the script context's run carry) gets its own screen root and stack,
        // a tab draws its own run's (SetRenderRun) at its own resolution, and input goes to the
        // run of the scene the input subsystem is bound to. The global overlay layer stays on
        // the shared tier, drawn above every run's.
        /// On: each run its own screen tier (the editor sets it once, as it sets the input's
        /// unbound-scene policy). Off (the default): every run uses the shared tier.
        void SetRunScreens(bool enabled) noexcept { m_runScreens = enabled; }
        [[nodiscard]] bool RunScreens() const noexcept { return m_runScreens; }
        /// `run`'s screen stack (made on first use), or the shared tier's (run screens off, no run).
        [[nodiscard]] foundation::ui::gamekit::ScreenStack& ScreensFor(const void* run);
        /// `run`'s screen root, made on first use (the shared tier's as ScreensFor).
        [[nodiscard]] RootView* ScreenRootFor(const void* run) { return ScreensFor(run).Root(); }
        /// The screen resolution of `run`'s tier (SetScreenResolution for the shared one).
        void SetScreenResolution(const void* run, u32 width, u32 height, FitMode fit);
        /// The run whose screen tier the next screen-overlay draw shows (null = the shared tier),
        /// set by a host around its RenderOverlays for that run's view.
        void SetRenderRun(const void* run) noexcept { m_renderRun = run; }
        /// The run ended: its screen tier and every screen on it go.
        void EndRunScreens(const void* run);
        /// The runs that have a screen tier of their own (introspection, tests).
        [[nodiscard]] usize RunScreenCount() const noexcept { return m_runTiers.Size(); }
        /// The scene tier's root for `scene` (canvases above a shared billboard layer);
        /// null if the scene is unknown.
        [[nodiscard]] RootView* SceneRoot(scene::Scene& scene) noexcept
        {
            SceneUI* ui = FindSceneUI(scene);
            return ui != nullptr ? ui->root.Get() : nullptr;
        }

        // ---- the scene-less SCREEN tier (Sedulous ScreenUIView) ----
        // Global overlays OUTSIDE any scene: they survive scene swaps (loading screens,
        // system menus) and draw ABOVE every scene's canvases in every target. Pushed
        // from code; the caller keeps the returned/passed view to remove it later.

        /// Instantiates `document` and attaches it topmost. Null if the markup fails.
        RefPtr<View> PushScreenOverlay(const UIDocument& document)
        {
            if (document.markup.IsEmpty() || m_screen.overlay.Get() == nullptr)
            {
                return {};
            }
            RefPtr<View> view = MarkupLoader::LoadFromString(m_context.Allocator(),
                                                             document.markup.AsView(), &m_context);
            if (view.Get() != nullptr)
            {
                m_screen.overlay->AddView(view.Get());
            }
            return view;
        }
        /// Instantiate a document's view tree WITHOUT attaching it - the caller attaches later
        /// (e.g. deferred through the mutation queue when pushing from inside an event handler).
        /// Null if the markup fails.
        [[nodiscard]] RefPtr<View> InstantiateScreenOverlay(const UIDocument& document)
        {
            if (document.markup.IsEmpty() || m_screen.overlay.Get() == nullptr)
            {
                return {};
            }
            return MarkupLoader::LoadFromString(m_context.Allocator(), document.markup.AsView(),
                                                &m_context);
        }
        /// Attaches an already-built view topmost (code-built overlays).
        void PushScreenOverlay(RefPtr<View> view)
        {
            if (view.Get() != nullptr && m_screen.overlay.Get() != nullptr)
            {
                m_screen.overlay->AddView(view.Get());
            }
        }
        void RemoveScreenOverlay(View* view)
        {
            if (view != nullptr && m_screen.overlay.Get() != nullptr)
            {
                m_screen.overlay->RemoveView(view);
            }
        }
        [[nodiscard]] usize ScreenOverlayCount() const noexcept
        {
            return m_screen.overlay.Get() != nullptr ? m_screen.overlay->ChildCount() : 0;
        }
        /// True while the global overlay layer should intercept input: it holds at least
        /// one HIT-TESTABLE child. Modal menus qualify; passive badges/watermarks pushed
        /// with IsHitTestVisible = false do not - they draw above every scene without
        /// shielding scene HUDs from the pointer. (PumpInput applies this every frame.)
        [[nodiscard]] bool OverlayLayerWantsInput() const noexcept
        {
            if (m_screen.overlay.Get() == nullptr)
            {
                return false;
            }
            for (usize i = 0; i < m_screen.overlay->ChildCount(); ++i)
            {
                const View* child = m_screen.overlay->GetChildAt(i);
                if (child != nullptr && child->IsHitTestVisible &&
                    child->Visibility == VisibilityValue::Visible)
                {
                    return true;
                }
            }
            return false;
        }

        // ---- lifecycle (definitions in UISubsystemImpl.cpp) ----
        void OnInit() override;
        void OnShutdown() override;
        void OnReady() override;
        void BeginFrame(f32 deltaTime) override;

        void OnSystemsReady(scene::Scene& scene) override
        {
            // The scene tier: each scene gets its own root (billboard layer BELOW its
            // canvases) that the scene-overlay pass draws wherever this scene renders.
            SceneUI ui;
            ui.scene = &scene;
            ui.root = MakeRef<RootView>(m_allocator);
            auto billboards = MakeRef<AbsoluteLayout>(m_allocator);
            billboards->IsHitTestVisible = false; // nameplates never eat clicks
            ui.billboardLayer = billboards;
            ui.root->AddView(ui.billboardLayer.Get());
            m_context.AddRootView(ui.root.Get());
            m_sceneUIs.PushBack(Move(ui));
        }
        void OnDestroying(scene::Scene& scene) override
        {
            for (usize i = 0; i < m_sceneUIs.Size(); ++i)
            {
                if (m_sceneUIs[i].scene != &scene)
                {
                    continue;
                }
                if (m_sceneUIs[i].root.Get() != nullptr)
                {
                    m_context.RemoveRootView(m_sceneUIs[i].root.Get());
                }
                m_sceneUIs.RemoveAt(i);
                break;
            }
        }

        // ---- overlay roles (the render layer's two-tier model) ----
        // The subsystem registers itself for BOTH: the scene tier draws each scene's
        // canvases + billboards inside the compose wherever that scene renders (with the
        // view's REAL camera); the screen tier draws the scene-less overlays once per
        // window target when the host calls IScreenRenderer::RenderOverlays.

        [[nodiscard]] i32 OverlayOrder() const noexcept override { return 0; } // both roles
        /// Scene tier: draws the view's scene root (matched by SceneKey) into the view's
        /// viewport rect - sub-rect views (split-screen) lay out at the viewport size and
        /// draw through the VG renderer's viewport seam, clipped to their rect.
        void Render(rhi::RenderPassEncoder& encoder, const render::SceneOverlayView& view) override;
        /// Screen tier: draws the global overlay root.
        void Render(rhi::RenderPassEncoder& encoder,
                    const render::ScreenOverlayView& view) override;

        /// The scene-tier per-view sync, split out for headless tests: canvas visibility
        /// plus billboard projection/scaling through the VIEW's camera (world -> clip ->
        /// NDC -> px in the target; behind-camera parks off-screen).
        void UpdateSceneView(scene::Scene& scene, const render::SceneOverlayView& view);

        /// RenderTexture canvases: draws every RT canvas's root into its subsystem-owned
        /// offscreen texture (create/resize on demand; orphaned targets destroyed). The
        /// HOST calls this on its command encoder BEFORE the scene render
        /// (DefaultApplication::OnRenderWindow / the Game tab / editor scene pages), so
        /// the scene can sample the result the same frame. Textures end in ShaderRead.
        /// Runs at most ONCE per UI frame - repeat calls from co-hosted pages no-op.
        /// Also refreshes the declarative material binding: the canvas ENTITY's own
        /// sprite/decal `texture` override tracks the canvas's current view (rebinds on
        /// resize, un-binds when the canvas/target goes away).
        void RenderCanvasTextures(rhi::CommandEncoder& encoder, i32 frameIndex);

        /// The offscreen texture view of `entity`'s RenderTexture canvas in `scene`
        /// (null when absent, not that mode, or not rendered yet). Also mirrored on the
        /// component (renderTexture/renderTextureView).
        [[nodiscard]] rhi::TextureView* CanvasRenderTextureView(scene::Scene& scene,
                                                                scene::EntityHandle entity) noexcept
        {
            auto* canvases = scene.GetSystem<UICanvasComponentManager>();
            UICanvasComponent* c = canvases != nullptr ? canvases->Get(entity) : nullptr;
            return c != nullptr ? c->renderTextureView : nullptr;
        }

        /// One-time GPU bring-up (shader compile + device wire) by whoever owns graphics
        /// (DefaultApplication's startup). Idempotent; without it overlay draws no-op.
        void EnsureRenderReady(rhi::Device& device, i32 frameCount);

        // ---- editor preview seam (the UIDocumentPage) ----
        // Previews render through THIS context - the GAME's fonts, theme, style
        // resolution, and VG path - into a DEDICATED RootView that is never attached to
        // the screen root (it cannot leak into game targets) and receives no input
        // (view-only; the ActiveInputRoot stays the screen root).

        /// Instantiates `document` into a fresh preview root. Null on parse failure.
        [[nodiscard]] RefPtr<RootView> CreatePreview(const UIDocument& document);
        void DestroyPreview(RootView* root);
        /// Draws a preview root into `target` via a Load-op pass on the caller's encoder
        /// (target in RenderTarget state; left there). Lays out at the given size.
        void RenderPreview(RootView& root, rhi::CommandEncoder& encoder, rhi::TextureView* target,
                           rhi::TextureFormat format, u32 width, u32 height, i32 frameIndex);

        /// True when any interactive canvas is under the pointer or holds text focus -
        /// mirrors the published consumption mask (tests + gameplay diagnostics).
        [[nodiscard]] bool PointerOverUI() const noexcept { return m_pointerConsumed; }
        /// How many UI renderers (one per target format) draw `image` from a texture: an image
        /// source, once drawn, is registered on every renderer that has drawn since (tests +
        /// diagnostics).
        [[nodiscard]] usize RenderersShowing(const foundation::image::ImageData* image) const;

    private:
        // Per-scene UI state: the scene tier's root + its billboard layer.
        struct SceneUI
        {
            scene::Scene* scene = nullptr;
            RefPtr<RootView> root;
            RefPtr<ViewGroup> billboardLayer; // BELOW the scene's canvases; one batch
        };
        [[nodiscard]] SceneUI* FindSceneUI(scene::Scene& scene) noexcept
        {
            for (SceneUI& ui : m_sceneUIs)
            {
                if (ui.scene == &scene)
                {
                    return &ui;
                }
            }
            return nullptr;
        }

        void SyncCanvases();
        void PumpInput();
        // Nothing is focused and a navigation input came: the top screen's default focus, or
        // failing a screen the first focusable anywhere. False when there is nothing to focus.
        bool LandFocus();
        // The cooked-font service over the default font and the extra ones, or the TTF fallback
        // when no default is bound.
        // A screen tier: the scene-less root, the screens pushed on it, and the game resolution it
        // lays out at. The shared one also holds the global overlay layer.
        struct ScreenTier
        {
            const void* run = nullptr; // null = the shared tier
            RefPtr<RootView> root;
            foundation::ui::gamekit::ScreenStack stack; // push/pop over root
            RefPtr<ViewGroup> overlay;                  // the shared tier only: ABOVE everything
            // The game's render resolution and how it fits its target, when it has one: the
            // tier lays out at that size and draws fitted where the game's image is.
            Float2 resolution{0.0f, 0.0f};
            FitMode fitMode = FitMode::Letterbox;
            Float2 targetSize{0.0f, 0.0f}; // the target the tier last drew into
            [[nodiscard]] bool HasResolution() const noexcept { return resolution.x > 0.0f && resolution.y > 0.0f; }
        };
        void RebuildFontService();
        // `run`'s own tier, or null (none made yet).
        [[nodiscard]] ScreenTier* FindRunTier(const void* run) noexcept;
        // The tier input reaches this frame: the run of the scene the input is bound to, when run
        // screens are on and that run has a tier; the shared tier otherwise.
        [[nodiscard]] ScreenTier& InputTier() noexcept;
        // The tier whose root is `root`, or null.
        [[nodiscard]] ScreenTier* TierOfRoot(const RootView* root) noexcept;
        static void ApplyResolution(ScreenTier& tier, u32 width, u32 height, FitMode fit);
        void DrawTier(ScreenTier& tier, rhi::RenderPassEncoder& encoder,
                      const render::ScreenOverlayView& view, bool stencil);
        // The screen resolution fitted into the last target the tier drew into.
        [[nodiscard]] static ContentFit ScreenFit(const ScreenTier& tier) noexcept;
        // A render-space point in the tier's layout units: offset by the part of the
        // resolution a crop leaves out, and nothing else.
        [[nodiscard]] static Float2 ScreenLayoutPoint(const ScreenTier& tier, Float2 point) noexcept;
        // The same point in the pixels the tier's input takes (layout units times its scale).
        [[nodiscard]] static Float2 ScreenPointerPoint(const ScreenTier& tier, Float2 point) noexcept;
        // RenderTexture canvas roots are STANDALONE context roots owned by their
        // component - this registry (strong refs, mark-sweep like the canvas hosts) is
        // how a vanished component (despawn/removal; managers have no destroy hook)
        // gets its root UNREGISTERED from the context, which stores roots non-owning.
        // The strong ref keeps a just-orphaned root alive until the sweep runs.
        struct TextureCanvasRoot
        {
            RefPtr<RootView> root;
            bool seen = false;
        };
        Array<TextureCanvasRoot> m_textureCanvasRoots;
        void DrawRootInto(RootView& root, rhi::CommandEncoder& encoder, rhi::TextureView* target,
                          rhi::TextureFormat format, u32 width, u32 height, i32 frameIndex);
        // Records one root into an ALREADY-ACTIVE render pass (the overlay-role contract).
        // (viewportX, viewportY) places the content rect within the pass's target
        // (split-screen sub-rect views); (0,0) for whole-target draws.
        void DrawRootInPass(RootView& root, rhi::RenderPassEncoder& encoder,
                            rhi::TextureFormat format, i32 viewportX, i32 viewportY, u32 width,
                            u32 height, i32 frameIndex, bool stencilCapable = false,
                            u32 sampleCount = 1);

        [[nodiscard]] bool ReadDataFile(StringView path, Array<u8>& outBytes) const; // impl unit

        TaggedAllocator m_allocator;
        foundation::vfs::IFileSystem* m_dataFileSystem; // borrowed (the application's data mount)
        UIContext m_context{m_allocator};
        UiInputBridge m_bridge{&m_context}; // key/text event mapping + IME sync
        ScreenTier m_screen;                     // the shared tier (root attached in init)
        Array<UniquePtr<ScreenTier>> m_runTiers; // a tier per run, with run screens on
        bool m_runScreens = false;
        const void* m_renderRun = nullptr;       // the run the next screen-overlay draw shows
        RefPtr<StyleSheet> m_theme;
        UniquePtr<foundation::fonts::TrueTypeFontService> m_fonts;
        UniquePtr<foundation::fonts::ResourceFontService>
            m_resourceFonts; // the cooked-font service when a default font product is bound
        // The bound default font and the project's other UI fonts. Strong, as the service holds
        // them: a re-cook replaces the manager's product, and a rebuild after it must not reach
        // a freed one.
        RefPtr<foundation::fonts::Font> m_defaultFont;
        Array<RefPtr<foundation::fonts::Font>> m_extraFonts;
        Array<SceneUI> m_sceneUIs;
        engine::input::InputSubsystem* m_input = nullptr;
        foundation::render::ISceneRenderer* m_sceneRenderer = nullptr; // overlay registration seam
        foundation::render::IScreenRenderer* m_screenRenderer = nullptr;
        u64 m_frameSerial = 0;              // gates VGRenderer::BeginFrame to once per frame
        u64 m_canvasTexturesSerial = ~0ull; // gates RenderCanvasTextures to once per frame

        // pointer edge tracking for the polled pump
        bool m_prevButtons[3] = {false, false, false};
        f32 m_prevWheel = 0.0f;
        bool m_pointerConsumed = false;

        // gamepad focus navigation (hold-repeat per direction)
        f32 m_navRepeat[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // Up/Down/Left/Right
        bool m_navHeld[4] = {false, false, false, false};
        // The view a pad's confirm (south) went down on, so its release activates that view and
        // nothing else; invalid with none. The back button (east) only needs to have gone down
        // while the UI was pumping.
        ViewId m_confirmPressedOn = ViewId::Invalid;
        bool m_backPressed = false;
        // The pointer is in use: it moved, clicked or scrolled since a key or a pad last drove the
        // UI. Only a live pointer hovers. Off until the pointer first moves, so a cursor resting
        // where the window opened lights nothing up.
        bool m_pointerLive = false;
        // A key or a pad has driven the UI, or a pad is connected: focus then shows its ring however
        // it arrived, since that is what the player navigates by.
        bool m_navigating = false;
        u32 m_traceFrame = 0; // ENV_INPUT_TRACE's sample counter
        void TraceUi(foundation::shell::IMouse* mouse); // ENV_INPUT_TRACE: what the UI holds
        f32 m_navDeltaTime = 0.0f;

        // impl-side render state (VG contexts/renderers/shaders), opaque here
        struct RenderState;
        UniquePtr<RenderState> m_render;
        // impl-side: the context's resource provider (texture asset id -> a UI image)
        struct TextureImages;
        UniquePtr<TextureImages> m_images;
        // A cooked theme's stylesheet, parsed with its own icons (@icon) and the texture images
        // (image()); null when it does not parse.
        [[nodiscard]] RefPtr<StyleSheet> ParseTheme(const UITheme& theme);
    };
}

export namespace engine::ui
{
    /// The aspect (width over height) of the AssiduousEngine logo with its wordmark.
    inline constexpr f32 kEngineLogoAspect = 931.0f / 200.0f;

    /// The loading screen a game shows while its first scene loads when it authors none (its
    /// loadingDocumentId is unset): the AssiduousEngine logo (`logoSvg`, the SVG text; left out
    /// when empty or unreadable) over a progress bar and a status line, centred on a dark screen.
    /// Its controls are named as an authored loading document's are: `progress` (the ProgressBar
    /// the player drives) and `status` (a Label).
    /// (Defined in the implementation unit: a body here left GCC unable to read the module back.)
    [[nodiscard]] RefPtr<foundation::ui::View> BuildDefaultLoadingScreen(IAllocator& allocator, StringView logoSvg);

    /// This domain's declaration (engine-composition.md D4): what it brings to a scene, to
    /// reflection, to the script surface and which resource modules come with it. Defined in the
    /// implementation unit (one instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& UiDomain() noexcept;
}
