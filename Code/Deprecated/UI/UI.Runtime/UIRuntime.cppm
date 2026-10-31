// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::UI.Runtime - the `foundation.ui.runtime` module.
//
// UIHost draws foundation.ui on the runtime's multi-window graphics host (foundation.graphics). It
// owns one UIContext with one RootView per window, compiles the shared VG shaders once, and gives
// each window its own VGContext, VGRenderer and InputSurface, stored on the RenderWindow as a
// UIWindowData (the graphics::IRenderWindowData payload the host reserves for this).
//
// It does not depend on the toolkit: it renders any RootView, so games (one window) and the editor
// (many windows) share it. The docking and floating-window layer sits above it in
// foundation.ui.application, the only UI module that imports foundation.ui.toolkit.
//
// A game's IApplication owns a UIHost, calls AttachWindow(mainWindow, root) once, then calls
// Update(dt) from OnUpdate and RenderWindow(frame) from OnRenderWindow. The app sets up no swapchain
// or VG state of its own.

module;
#include "Core/Prelude.h"

export module foundation.ui.runtime;

import foundation.core;
import foundation.rhi;
import foundation.image;
import foundation.shaders;
import foundation.shaders.system; // ShaderSystemHost (cooked-pack-or-dev shader resolution)
import foundation.vfs;            // IFileSystem: the application's data mount (shaders live in it)
import foundation.shell;
import foundation.graphics;
import foundation.vg;
import foundation.vg.renderer;
import foundation.vg.svg;
import foundation.fonts;
import foundation.ui;
import foundation.ui.shell;

namespace core = foundation::core;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;
namespace shell = foundation::shell;
namespace graphics = foundation::graphics;
namespace vg = foundation::vg;
namespace image = foundation::image;
namespace fonts = foundation::fonts;

// foundation::ui::runtime nests in foundation::ui, so UIContext / RootView / InputManager / UiInputBridge /
// ShellClipboard resolve unqualified.
export namespace foundation::ui::runtime
{
    using core::f32;
    using core::i32;
    using core::u32;
    using core::u64;
    using core::u8;
    using core::usize;

    /// Per-window UI payload stored on a graphics::RenderWindow via SetData. Owns that window's VG
    /// context, renderer and input surface; shares ownership of its RootView with the UIContext.
    class UIWindowData final : public graphics::IRenderWindowData
    {
    public:
        explicit UIWindowData(core::IAllocator& allocator) noexcept : renderer(allocator) {}

        core::RefPtr<RootView> root;
        core::UniquePtr<vg::VGContext> vg;
        vg::renderer::VGRenderer renderer;
        core::UniquePtr<shell::InputSurface> surface;

        // VG quality targets, provided by the host for stencil-then-cover filling: a 4x MSAA
        // color target resolved into the backbuffer, and a stencil attachment for the fill
        // pipelines. Sized to the swapchain; Update() recreates them on resize (desktop hosts
        // only, where the WaitIdle runs outside any open frame). Without them the window draws a
        // plain single-sampled pass.
        vg::renderer::VGPassTargets targets;
        // When set, this window's UI blends encoded values, drawing through the backbuffer's
        // UNORM view in drawFormat; otherwise it blends linear light in the backbuffer's own format.
        bool encoded = false;
        rhi::TextureFormat drawFormat = rhi::TextureFormat::Undefined;
    };

    /// Renders foundation.ui on the runtime graphics host. Construct once (this compiles the VG shaders),
    /// attach a RootView per window, and call Update() and RenderWindow() from the app's OnUpdate and
    /// OnRenderWindow.
    class UIHost
    {
    public:
        // The allocator is required (the entry point chooses it) and roots the whole UI: the
        // shared UIContext, every view tree under it, and the host's own services.
        // `dataFileSystem` is the application's data mount (borrowed): the VG shaders resolve
        // from it (Shaders/ sources in dev, Shaders/shaders.dpak in a dist).
        UIHost(core::IAllocator& allocator, graphics::GraphicsDevice& device,
               shell::IShell& shellRef, fonts::IFontService& fontService,
               foundation::vfs::IFileSystem& dataFileSystem)
            : m_allocator(&allocator), m_device(&device), m_shell(&shellRef),
              m_fonts(&fontService), m_shaderHost(allocator), m_ctx(allocator)
        {
            InitShaders(dataFileSystem);
            m_ctx.SetFontService(&fontService);
            m_router =
                core::MakeUnique<shell::InputRouter>(allocator, shellRef.Input());
            m_bridge = core::MakeUnique<UiInputBridge>(allocator, &m_ctx);
            m_clipboard = core::MakeUnique<ShellClipboard>(allocator, &shellRef);
            m_ctx.SetClipboard(m_clipboard.Get());
            // Create the shared theme chrome glyphs before the app builds its theme, so every
            // theme references instances that can be baked into crisp icons. They are baked at the
            // first window attach or by BakeThemeIcons.
            ThemeIconSet::Get().Initialize();
        }

        ~UIHost()
        {
            // Per-window VGRenderers are disposed with their RenderWindow's payload (freed by the host
            // after a GPU idle), not here. The VG shader modules (m_vs/m_fs) are borrowed from the
            // shader host's ShaderSystem, which owns and frees them when it shuts down below.
            // The baked variants of the theme glyphs borrow our atlases, so detach them before the
            // atlases are freed (glyphs held by a sheet fall back to live vector drawing), then
            // drop the shared references.
            ThemeIconSet::Get().ClearBakedVariants();
            ThemeIconSet::Get().Shutdown();
        }

        UIHost(const UIHost&) = delete;
        UIHost& operator=(const UIHost&) = delete;

        /// The shared UI context (set the theme or stylesheet on it, look up focus, and so on).
        [[nodiscard]] UIContext& Context() noexcept { return m_ctx; }

        /// The user's UI-scale factor, multiplied onto every window's OS content scale (an editor
        /// preference for accessibility). Roots pick it up on the next Update; callers re-bake any
        /// baked icon sets themselves at the new effective scale.
        void SetUiScale(f32 scale) noexcept { m_uiScale = core::Clamp(scale, 0.5f, 3.0f); }
        [[nodiscard]] f32 UiScale() const noexcept { return m_uiScale; }

        /// The values a window's UI blends (VG's blend space), for windows added after the call.
        /// Encoded (the default) blends as browsers and design tools do: half black over white is
        /// 50% grey. It needs the window's swapchain to offer its UNORM view; otherwise that window
        /// blends linear light.
        void SetBlendSpace(vg::VGBlendSpace space) noexcept { m_blendSpace = space; }
        [[nodiscard]] vg::VGBlendSpace BlendSpace() const noexcept { return m_blendSpace; }

        /// Turns the damage gate off or on. Off, every frame lays out and redraws every window.
        /// The frame counters below show how often the gate skips work.
        void SetDamageGatingEnabled(bool enabled) noexcept { m_damageGateEnabled = enabled; }
        [[nodiscard]] bool DamageGatingEnabled() const noexcept { return m_damageGateEnabled; }
        [[nodiscard]] u64 FramesDrawn() const noexcept { return m_framesDrawn; }
        [[nodiscard]] u64 FramesSkipped() const noexcept { return m_framesSkipped; }
        /// Frames that also re-measured and laid out (a subset of FramesDrawn; the difference is the
        /// visual-only frames, such as hover, press or caret changes, that redraw over a valid layout).
        [[nodiscard]] u64 FramesLaidOut() const noexcept { return m_framesLaidOut; }

        /// Background clear color behind the UI (the theme usually paints an opaque root over it).
        /// Takes the theme's color as authored (sRGB, like every UI color). The swapchain is an
        /// sRGB format, and pass clear values are interpreted as linear and encoded by the
        /// hardware on store, so the sRGB components are linearized here. Otherwise the background
        /// would clear visibly lighter than the same color drawn by the UI, whose vertex path
        /// linearizes in VGRenderVertex.
        void SetClearColor(f32 r, f32 g, f32 b, f32 a = 1.0f) noexcept
        {
            m_clear = rhi::ClearColor(core::SrgbToLinear(r), core::SrgbToLinear(g),
                                      core::SrgbToLinear(b), a);
            m_clearEncoded = rhi::ClearColor(r, g, b, a); // an Encoded window clears as authored
        }

        /// Bake the shared theme chrome glyphs (close, chevrons, checkmark and so on) at the given
        /// UI scale, with the same pixel-snapped atlas bake the editor uses for its own icons. Call
        /// again after a UI-scale or DPI change: old variants are detached first, and drawables
        /// fall back to live vector drawing until the new bake completes.
        bool BakeThemeIcons(f32 scale = 1.0f)
        {
            ThemeIconSet& set = ThemeIconSet::Get();
            set.Initialize();
            set.ClearBakedVariants();
            core::Array<BakedSVGDrawable*> bakeable;
            set.CollectBakeable(bakeable);
            static constexpr u32 kChromeSizes[] = {10, 12, 14, 16, 20, 24, 32};
            core::Array<u32> sizes;
            for (const u32 base : kChromeSizes)
            {
                const u32 scaled = static_cast<u32>(static_cast<f32>(base) * scale + 0.5f);
                if (sizes.IsEmpty() || sizes[sizes.Size() - 1] != scaled)
                {
                    sizes.PushBack(scaled);
                }
            }
            m_themeIconsBaked = BakeSvgDrawables(
                core::Span<BakedSVGDrawable* const>(bakeable.Data(), bakeable.Size()),
                core::Span<const u32>(sizes.Data(), sizes.Size()));
            return m_themeIconsBaked;
        }

        /// Give a RenderWindow a RootView: builds its VGContext, its VGRenderer (against the window's
        /// swapchain format and the device frame ring) and its InputSurface, and stores the payload on
        /// the RenderWindow.
        void AttachWindow(graphics::RenderWindow* window, core::RefPtr<RootView> root)
        {
            if (window == nullptr || !root)
            {
                return;
            }
            // The first window attach means the device is live: bake the shared theme glyphs so
            // every UIHost app gets crisp pixel-snapped chrome icons. Scale-aware hosts re-bake via
            // BakeThemeIcons on DPI or UI-scale changes.
            if (!m_themeIconsBaked)
            {
                (void)BakeThemeIcons(m_uiScale);
            }

            auto data = core::MakeUnique<UIWindowData>(*m_allocator, *m_allocator);
            data->root = root;
            data->vg = core::MakeUnique<vg::VGContext>(*m_allocator, m_fonts);
            // Per-pixel radial and conic gradients need both shaders (an older cooked pack may
            // lack them); otherwise the renderer and context fall back to the affine LUT.
            const bool perPixelGrad = m_gradRadialFs != nullptr && m_gradConicFs != nullptr;

            // VG quality targets: 4x MSAA for fill and stroke edge antialiasing, and a stencil
            // attachment so stencil-then-cover handles holes, self-intersection and even-odd
            // fills. The renderer's pipelines must match the pass, so the target configuration is
            // decided before Initialize; any creation failure falls back to the plain pass.
            data->targets.Configure(*m_device->Raw());
            // Encoded when asked for and the swapchain offers its UNORM view; otherwise linear.
            data->encoded = m_blendSpace == vg::VGBlendSpace::Encoded &&
                            window->Swap()->AlternateViewFormat() != rhi::TextureFormat::Undefined;
            data->drawFormat = data->encoded ? window->Swap()->AlternateViewFormat() : window->Swap()->Format();
            const bool targetsOk =
                data->targets.Create(data->drawFormat, window->Window().Width(), window->Window().Height());
            vg::renderer::VGTargetConfig targetConfig =
                targetsOk ? data->targets.ConfigFor() : vg::renderer::VGTargetConfig{};
            targetConfig.blendSpace = data->encoded ? vg::VGBlendSpace::Encoded : vg::VGBlendSpace::Linear;
            data->renderer.Initialize(*m_device->Raw(), *m_vs, *m_fs, data->drawFormat,
                                      static_cast<i32>(m_device->FramesInFlight()), m_distanceFieldFragmentShader,
                                      m_gradRadialFs, m_gradConicFs, targetConfig, m_boxShadowFs, m_boxVs,
                                      m_boxFs, m_layerFs, m_blurVs, m_blurFs);
            data->vg->SetPerPixelGradients(perPixelGrad);
            data->vg->SetAnalyticBoxes(m_boxVs != nullptr && m_boxFs != nullptr);
            data->vg->SetLayers(m_layerFs != nullptr);
            data->vg->SetStencilFills(data->renderer.StencilFillsSupported());

            const f32 w = static_cast<f32>(window->Window().Width());
            const f32 h = static_cast<f32>(window->Window().Height());
            const core::ContentFit fit{core::Rectangle{0.0f, 0.0f, w, h}, core::Float2{w, h},
                                       core::FitMode::Stretch};
            data->surface = core::MakeUnique<shell::InputSurface>(
                *m_allocator, m_shell->Input(), window->Window().Id(), fit);

            root->ViewportSize = core::Float2{w, h};
            root->DpiScale = window->Window().ContentScale() * m_uiScale;
            m_ctx.AddRootView(root.Get());
            m_router->AddSurface(data->surface.Get());
            m_bridge->SetTextInputTarget(
                &window->Window()); // IME target (the last attach wins until Update follows focus)

            UIWindowData* raw = data.Get();
            // On a re-attach (the editor swaps a window between the project-manager root and the
            // shell root), SetData below destroys the previous payload: a live VGRenderer whose
            // pipelines, descriptor sets and buffers in-flight frames still reference. Idle the GPU
            // first, or replacing a window's root raises in-use validation errors and can crash
            // (the race DetachWindow describes).
            if (window->Data() != nullptr && m_device->Raw() != nullptr)
            {
                m_device->Raw()->WaitIdle();
            }
            window->SetData(static_cast<core::UniquePtr<UIWindowData>&&>(
                data)); // RenderWindow owns the payload
            m_attached.PushBack(Attached{window, raw});
        }

        /// Logical detach: stop routing input to the window and remove its root from the context, so
        /// nothing draws or hit-tests it any more. The GPU payload (the VGRenderer and the rest) stays
        /// on the RenderWindow and is freed when the host destroys the RenderWindow (CloseWindow), which
        /// waits for the GPU to idle first; freeing it here, without an idle, would race the GPU. So a
        /// docked window is destroyed with DetachWindow(rw) followed by IApplicationHost::CloseWindow(rw).
        void DetachWindow(graphics::RenderWindow* window)
        {
            for (usize i = 0; i < m_attached.Size(); ++i)
            {
                if (m_attached[i].window == window)
                {
                    UIWindowData* data = m_attached[i].data;
                    // A detached root gets no input and no ticks, so its open popups (menus,
                    // dropdowns) could never dismiss: they would freeze, and still be showing
                    // if the root were attached again. Close them all now.
                    if (PopupLayer* popups = data->root->GetPopupLayer())
                    {
                        popups->CloseAllPopups();
                    }
                    m_router->RemoveSurface(data->surface.Get());
                    m_ctx.RemoveRootView(data->root.Get());
                    // The window is closed after this, so the bridge's text input must drop it.
                    // Otherwise the hover pump would sync IME against the closed window until the
                    // next focus refresh, and crash.
                    m_bridge->ForgetWindow(&window->Window());
                    m_attached.RemoveAt(i);
                    return;
                }
            }
        }

        /// Once per frame: pump each window's input, tick the context, lay out every root.
        void Update(f32 deltaTime)
        {
            m_router->Update();

            // Cross-window drag: while a drag is in flight and the cursor is over or came from a
            // secondary window (a floating dock window follows the cursor, so it sits under the
            // pointer), route the drag to the main window, mapping the global mouse to main-relative
            // coordinates, so the main window's drop targets (such as a DockManager) see the drag-over
            // and accept the drop. The OS window under the cursor is bypassed; the main window is fed
            // explicitly.
            DragDropManager* dd = m_ctx.DragDrop();
            Attached* mainW = m_attached.IsEmpty() ? nullptr : &m_attached[0];
            const u32 focusedId =
                (m_shell->Input() != nullptr) ? m_shell->Input()->FocusedWindow() : 0;

            // Stuck-drag watchdog. A floated dock window chases the cursor during a drag, and on some
            // platforms (Wayland especially) the release event can land during a window handoff and
            // never reach the drag routing, which would leave the drag latched and the window
            // following the mouse. The shell's global button state is authoritative: if a drag is
            // active but the OS reports the left button up for a full frame, deliver the release at
            // the current global position through the same path a normal release takes.
            if (dd != nullptr && dd->IsDragging() && m_shell->Input() != nullptr &&
                m_shell->Input()->Mouse() != nullptr &&
                !m_shell->Input()->Mouse()->IsButtonDown(shell::MouseButton::Left))
            {
                if (m_dragReleaseMissedFrames++ >= 1) // a full frame with the button up means the release was missed
                {
                    if (mainW != nullptr)
                    {
                        PumpWindowAtGlobal(*mainW); // routes the up as a drop at the cursor
                    }
                    if (dd->IsDragging())
                    {
                        dd->CancelDrag(); // still latched (no drop target took it), so cancel it
                    }
                    m_dragReleaseMissedFrames = 0;
                }
            }
            else
            {
                m_dragReleaseMissedFrames = 0;
            }

            const bool crossWindowDrag =
                (dd != nullptr && dd->IsDragging() && mainW != nullptr && focusedId != 0 &&
                 focusedId != mainW->window->Window().Id());

            // Route the mouse to a single window per frame. Pumping windows that are not hovered
            // re-asserts their frozen last mouse position, which re-hovers them and overwrites the
            // shared hover and cursor state. In priority order:
            //   1. a cross-window drag goes to the main window (the float sits under the cursor);
            //   2. a captured view (a dock-window edge resize, a slider drag) gets its own window, fed
            //      live global-relative coordinates so it keeps working outside the window;
            //   3. otherwise the window under the pointer gets the normal surface pump, with scroll.
            Attached* captured = CapturedWindow();
            if (crossWindowDrag)
            {
                PumpWindowAtGlobal(*mainW);
            }
            else if (captured != nullptr)
            {
                PumpWindowAtGlobal(*captured);
            }
            else
            {
                const u32 hoverId =
                    (m_shell->Input() != nullptr) ? m_shell->Input()->HoverWindow() : 0;
                Attached* hoverW = FindByWindowId(hoverId);
                if (hoverW == nullptr)
                {
                    hoverW = mainW;
                }
                if (hoverW != nullptr)
                {
                    const f32 w = static_cast<f32>(hoverW->window->Window().Width());
                    const f32 h = static_cast<f32>(hoverW->window->Window().Height());
                    hoverW->data->surface->SetRegion(core::Rectangle{0.0f, 0.0f, w, h});
                    hoverW->data->surface->SetContentSize(core::Float2{w, h});
                    m_ctx.SetActiveInputRoot(hoverW->data->root.Get());
                    m_bridge->PumpFromSurface(*hoverW->data->surface);
                }
            }

            // Keyboard, text and IME follow the focused OS window: point the active input root and the
            // text-input target at the focused window's root, then dispatch its key and text events.
            if (shell::IInputManager* input = m_shell->Input())
            {
                Attached* focused = FindByWindowId(input->FocusedWindow());
                if (focused == nullptr && !m_attached.IsEmpty())
                {
                    focused = &m_attached[0];
                } // falls back to the main window
                if (focused != nullptr)
                {
                    m_ctx.SetActiveInputRoot(focused->data->root.Get());
                    m_bridge->SetTextInputTarget(&focused->window->Window());
                }
                for (const shell::InputEvent& ev : input->Events())
                {
                    switch (ev.kind)
                    {
                    case shell::InputEventKind::KeyDown:
                    case shell::InputEventKind::KeyUp:
                    case shell::InputEventKind::TextInput:
                        m_bridge->Dispatch(ev);
                        break;
                    default:
                        break;
                    }
                }
            }

            // Keep OS mouse events flowing across window edges while a drag or a captured interaction
            // (a dock-window resize, a slider) is in progress. Without an app-global capture the OS stops
            // delivering events once the cursor leaves the window, which stalls multi-window drags and
            // resizes at the edge and loses a release that happens outside the window.
            if (m_shell->Input() != nullptr)
            {
                if (shell::IMouse* mouse = m_shell->Input()->Mouse())
                {
                    const bool capturing = (dd != nullptr && dd->IsDragging()) ||
                                           (m_ctx.GetFocusManager()->CapturedView() != nullptr);
                    mouse->SetGlobalCapture(capturing);

                    // Push the hovered view's cursor to the OS (borderless floats have no WM to show resize
                    // grips). The mapping between UI and shell cursors lives in the bridge, with the rest
                    // of the enum translation.
                    m_bridge->SyncCursor(*mouse);
                }
            }

            m_ctx.BeginFrame(deltaTime);

            // The damage gate: one decision per frame for all windows. When nothing invalidated
            // and no window resized or changed scale, skip both layout and the draw-tree walk this
            // frame; RenderWindow re-encodes the retained VG batch, so presenting is unchanged.
            // Invalidation comes from input events, hover, focus and press changes, animations and
            // the focused-text caret (BeginFrame), the drag adorner, list timers, and popups and
            // tooltips when they show.
            bool structural = false;
            for (const Attached& a : m_attached)
            {
                const f32 newW = static_cast<f32>(a.window->Window().Width());
                const f32 newH = static_cast<f32>(a.window->Window().Height());
                const f32 newDpi = a.window->Window().ContentScale() * m_uiScale;
                if (a.data->root->ViewportSize.x != newW || a.data->root->ViewportSize.y != newH ||
                    a.data->root->DpiScale != newDpi)
                {
                    structural = true;
                    break;
                }
            }
            m_frameDamaged = !m_damageGateEnabled || structural || m_ctx.NeedsRedraw();
            if (!m_frameDamaged)
            {
                ++m_framesSkipped;
                return; // layout is still valid; RenderWindow reuses the retained batch
            }
            ++m_framesDrawn;

            // Layout runs only on layout damage. Visual-only frames (hover tint, press state,
            // focus ring, caret blink) redraw without re-measuring every window's whole tree, so
            // moving the mouse over a large dialog does not lay out the entire editor each frame.
            const bool layoutDamaged =
                !m_damageGateEnabled || structural || m_ctx.NeedsLayout();
            if (!layoutDamaged)
            {
                m_ctx.ClearLayoutDamage();
                return; // draw-only: RenderWindow re-walks OnDraw over the valid layout
            }
            ++m_framesLaidOut;

            for (Attached& a : m_attached)
            {
                a.data->root->ViewportSize =
                    core::Float2{static_cast<f32>(a.window->Window().Width()),
                                 static_cast<f32>(a.window->Window().Height())};
                // The effective scale is the OS content scale times the user's UI-scale preference,
                // refreshed per frame so both monitor moves and preference changes apply.
                a.data->root->DpiScale = a.window->Window().ContentScale() * m_uiScale;
                m_ctx.UpdateRootView(a.data->root.Get());

                // Resize the VG quality targets with the window. This happens here, outside any
                // open frame, so the idle wait cannot yield mid-frame (desktop hosts only; the web
                // player uses UISubsystem, not this host). In-flight frames may still reference the
                // old targets, hence the idle before recreating them.
                const u32 w = static_cast<u32>(a.window->Window().Width());
                const u32 h = static_cast<u32>(a.window->Window().Height());
                if (a.data->targets.NeedsResize(w, h))
                {
                    m_device->Raw()->WaitIdle();
                    (void)a.data->targets.Resize(w, h);
                }
            }
            m_ctx.ClearLayoutDamage(); // consumed by this frame's layout pass
        }

        /// Draw one window's RootView into its frame backbuffer. No-op for a non-UI or invalid frame.
        void RenderWindow(graphics::FrameContext& frame)
        {
            if (!frame.valid)
            {
                return;
            }
            UIWindowData* data = Find(frame.window);
            if (data == nullptr)
            {
                return;
            }

            if (m_frameDamaged)
            {
                data->vg->Clear();
                m_ctx.DrawRootView(data->root.Get(), *data->vg);
            }
            // Clean frames re-encode the retained batch built on the last damaged frame. The
            // backbuffer is redrawn every frame; only the tree walk and text shaping are skipped.
            vg::VGBatch& batch = data->vg->GetBatch();

            data->renderer.BeginFrame(static_cast<i32>(frame.frameIndex));
            const vg::renderer::VGRenderSlice slice = data->renderer.Prepare(
                batch, static_cast<i32>(frame.frameIndex), frame.width, frame.height);
            // Layers draw in passes of their own, before the window's.
            if (frame.encoder != nullptr)
            {
                data->renderer.RenderLayers(*frame.encoder, static_cast<i32>(frame.frameIndex), slice);
            }

            // Quality pass: render into the 4x MSAA color target (resolved into the
            // backbuffer by the pass) with the stencil attachment cleared to 0. Falls back
            // to the plain backbuffer pass when targets are absent or stale-sized (the
            // resize recreate happens in Update, outside the frame).
            if (data->targets.Matches(frame.width, frame.height) && frame.encoder != nullptr)
            {
                const rhi::RenderPassDesc rpd = data->targets.BeginPass(
                    *frame.encoder, data->encoded ? frame.backbufferAlternateView : frame.backbufferView,
                    data->encoded ? m_clearEncoded : m_clear, u8"UI (msaa+stencil)");
                if (rhi::RenderPassEncoder* rp = frame.encoder->BeginRenderPass(rpd))
                {
                    data->renderer.Render(*rp, frame.width, frame.height,
                                          static_cast<i32>(frame.frameIndex), slice);
                    rp->End();
                }
                return;
            }

            rhi::RenderPassEncoder* rp =
                data->encoded ? frame.BeginBackbufferPass(m_clearEncoded, frame.backbufferAlternateView)
                              : frame.BeginBackbufferPass(m_clear);
            if (rp != nullptr)
            {
                data->renderer.Render(*rp, frame.width, frame.height,
                                      static_cast<i32>(frame.frameIndex), slice);
            }
            frame.EndBackbufferPass();
        }

        /// Bake SVG drawables into a shared bitmap atlas. Each drawable, at each size, renders
        /// through the VG at 4x supersampling into an offscreen sRGB target, is read back, and is
        /// box-downsampled on the CPU. Antialiasing is baked into the texels once; BakedSVGDrawable
        /// then draws pixel-snapped quads, so every instance of an icon samples identical texels.
        /// The host owns the atlas CPU image and the variants borrow it; call again after a DPI
        /// change with scaled sizes. Synchronous (one small GPU round trip), so call it at startup
        /// or theme load, not per frame. Returns false and changes nothing on any failure, and the
        /// drawables keep drawing live vectors.
        bool BakeSvgDrawables(core::Span<BakedSVGDrawable* const> drawables,
                              core::Span<const u32> sizes)
        {
            constexpr u32 kSupersample = 4;
            constexpr u32 kPad = 1;
            constexpr u32 kAtlasWidth = 512;
            rhi::Device* device = m_device->Raw();
            if (device == nullptr || drawables.IsEmpty() || sizes.IsEmpty())
            {
                return false;
            }

            // Shelf layout in final atlas coordinates.
            struct Cell
            {
                BakedSVGDrawable* drawable = nullptr;
                u32 size = 0;
                u32 x = 0;
                u32 y = 0;
            };
            core::Array<Cell> cells;
            u32 cursorX = kPad;
            u32 cursorY = kPad;
            u32 rowHeight = 0;
            for (const u32 size : sizes)
            {
                for (BakedSVGDrawable* drawable : drawables)
                {
                    if (drawable == nullptr)
                    {
                        continue;
                    }
                    if (cursorX + size + kPad > kAtlasWidth)
                    {
                        cursorX = kPad;
                        cursorY += rowHeight + kPad;
                        rowHeight = 0;
                    }
                    cells.PushBack(Cell{drawable, size, cursorX, cursorY});
                    rowHeight = core::Max(rowHeight, size);
                    cursorX += size + kPad;
                }
            }
            if (cells.IsEmpty())
            {
                return false;
            }
            const u32 atlasHeight = cursorY + rowHeight + kPad;
            const u32 rtWidth = kAtlasWidth * kSupersample;
            const u32 rtHeight = atlasHeight * kSupersample;

            // Offscreen sRGB target, rendered into and copied out.
            rhi::TextureDesc rtDesc{};
            rtDesc.dimension = rhi::TextureDimension::Texture2D;
            rtDesc.format = rhi::TextureFormat::RGBA8UnormSrgb;
            rtDesc.width = rtWidth;
            rtDesc.height = rtHeight;
            rtDesc.depth = 1;
            rtDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            rtDesc.label = u8"icon bake";
            rhi::Texture* rt = nullptr;
            rhi::TextureView* rtView = nullptr;
            if (!device->CreateTexture(rtDesc, rt).IsOk() ||
                !device->CreateTextureView(rt, rhi::TextureViewDesc{}, rtView).IsOk())
            {
                if (rt != nullptr)
                {
                    device->DestroyTexture(rt);
                }
                return false;
            }

            // Render every cell at supersampled scale through a throwaway VG stack.
            vg::VGContext bakeVg(m_fonts);
            for (const Cell& cell : cells)
            {
                const core::Rectangle rect{static_cast<f32>(cell.x * kSupersample),
                                           static_cast<f32>(cell.y * kSupersample),
                                           static_cast<f32>(cell.size * kSupersample),
                                           static_cast<f32>(cell.size * kSupersample)};
                vg::svg::SVGRenderer::Render(bakeVg, cell.drawable->Document(), rect, {});
            }

            vg::renderer::VGRenderer bakeRenderer(*m_allocator);
            if (!bakeRenderer
                     .Initialize(*device, *m_vs, *m_fs, rhi::TextureFormat::RGBA8UnormSrgb, 1)
                     .IsOk())
            {
                device->DestroyTextureView(rtView);
                device->DestroyTexture(rt);
                return false;
            }
            bakeRenderer.BeginFrame(0);
            const vg::renderer::VGRenderSlice slice =
                bakeRenderer.Prepare(bakeVg.GetBatch(), 0, rtWidth, rtHeight);

            // Readback buffer (256-aligned rows for backend copy rules).
            const u32 rowPitch = ((rtWidth * 4u) + 255u) & ~255u;
            rhi::BufferDesc readDesc{};
            readDesc.size = static_cast<u64>(rowPitch) * rtHeight;
            readDesc.usage = rhi::BufferUsage::CopyDst;
            readDesc.memory = rhi::MemoryLocation::GpuToCpu;
            rhi::Buffer* readBuffer = nullptr;
            rhi::CommandPool* pool = nullptr;
            rhi::Fence* fence = nullptr;
            bool ok = device->CreateBuffer(readDesc, readBuffer).IsOk() &&
                      device->CreateCommandPool(rhi::QueueType::Graphics, pool) ==
                          core::ErrorCode::Ok &&
                      device->CreateFence(0, fence) == core::ErrorCode::Ok;
            rhi::CommandEncoder* encoder = nullptr;
            if (ok)
            {
                ok = pool->CreateEncoder(encoder) == core::ErrorCode::Ok && encoder != nullptr;
            }
            if (ok)
            {
                encoder->TransitionTexture(rt, rhi::ResourceState::Undefined,
                                           rhi::ResourceState::RenderTarget);
                rhi::ColorAttachment color{};
                color.view = rtView;
                color.loadOp = rhi::LoadOp::Clear;
                color.storeOp = rhi::StoreOp::Store;
                color.clearValue = rhi::ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                rhi::RenderPassDesc passDesc{};
                passDesc.colorAttachments.Add(color);
                passDesc.label = u8"icon bake";
                if (rhi::RenderPassEncoder* pass = encoder->BeginRenderPass(passDesc))
                {
                    bakeRenderer.Render(*pass, rtWidth, rtHeight, 0, slice);
                    pass->End();
                }
                encoder->TransitionTexture(rt, rhi::ResourceState::RenderTarget,
                                           rhi::ResourceState::CopySrc);
                rhi::BufferTextureCopyRegion region{};
                region.bytesPerRow = rowPitch;
                region.rowsPerImage = rtHeight;
                region.textureExtent = rhi::Extent3D{rtWidth, rtHeight, 1};
                encoder->CopyTextureToBuffer(rt, readBuffer, region);
                rhi::CommandBuffer* commands = encoder->Finish();
                rhi::Queue* queue = device->GetQueue(rhi::QueueType::Graphics, 0);
                rhi::CommandBuffer* buffers[1] = {commands};
                queue->Submit(core::Span<rhi::CommandBuffer* const>(buffers, 1), fence, 1);
                fence->Wait(1, ~0ull);
            }

            core::UniquePtr<image::OwnedImageData> atlas;
            if (ok)
            {
                const u8* pixels = static_cast<const u8*>(readBuffer->Map());
                ok = pixels != nullptr;
                if (ok)
                {
                    atlas = DownsampleBake(*m_allocator, pixels, rowPitch, kAtlasWidth, atlasHeight,
                                           kSupersample);
                    readBuffer->Unmap();
                }
            }

            // GPU cleanup (everything above completed via the fence).
            bakeRenderer.Dispose();
            if (encoder != nullptr)
            {
                pool->DestroyEncoder(encoder);
            }
            if (fence != nullptr)
            {
                device->DestroyFence(fence);
            }
            if (pool != nullptr)
            {
                device->DestroyCommandPool(pool);
            }
            if (readBuffer != nullptr)
            {
                device->DestroyBuffer(readBuffer);
            }
            device->DestroyTextureView(rtView);
            device->DestroyTexture(rt);
            if (!ok || !atlas)
            {
                return false;
            }

            // Distribute variants (they borrow the atlas; the host owns it).
            const image::ImageData* atlasImage = atlas.Get();
            for (BakedSVGDrawable* drawable : drawables)
            {
                if (drawable == nullptr)
                {
                    continue;
                }
                core::Array<BakedSVGDrawable::BakedVariant> variants;
                for (const Cell& cell : cells)
                {
                    if (cell.drawable != drawable)
                    {
                        continue;
                    }
                    BakedSVGDrawable::BakedVariant variant;
                    variant.atlas = atlasImage;
                    variant.srcRect =
                        core::Rectangle{static_cast<f32>(cell.x), static_cast<f32>(cell.y),
                                        static_cast<f32>(cell.size), static_cast<f32>(cell.size)};
                    variant.sizePx = static_cast<f32>(cell.size);
                    variants.PushBack(variant);
                }
                drawable->SetBakedVariants(core::Move(variants));
            }
            m_bakedIconAtlases.PushBack(core::Move(atlas));
            return true;
        }

        /// The per-window VGRenderer for an attached window, or null if not attached. Exposed so an app
        /// can register an external texture (such as a ui::viewport ViewportView's offscreen render target)
        /// into the same renderer that draws that window's UI, so the UI can sample it via DrawImage.
        [[nodiscard]] vg::renderer::VGRenderer* RendererFor(graphics::RenderWindow* window)
        {
            UIWindowData* data = Find(window);
            return data != nullptr ? &data->renderer : nullptr;
        }

        /// The attached window whose RootView is `root`, or null. Lets an app discover which window
        /// currently hosts a given view tree, such as a ViewportView whose dockable panel was floated into
        /// a new OS window, so the app can re-bind it (View::Root(), then WindowForRoot, then RendererFor).
        [[nodiscard]] graphics::RenderWindow* WindowForRoot(RootView* root)
        {
            if (root == nullptr)
            {
                return nullptr;
            }
            for (Attached& a : m_attached)
            {
                if (a.data->root.Get() == root)
                {
                    return a.window;
                }
            }
            return nullptr;
        }

    private:
        /// CPU half of the bake: sRGB-decode, average the supersample-squared premultiplied box,
        /// un-premultiply (the vg shader premultiplies its output, but DrawImage expects straight
        /// alpha and premultiplies again when it draws), then sRGB-encode.
        [[nodiscard]] static core::UniquePtr<image::OwnedImageData>
        DownsampleBake(core::IAllocator& allocator, const u8* pixels, u32 rowPitch,
                       u32 atlasWidth, u32 atlasHeight, u32 supersample)
        {
            // 256-entry decode LUT: the box filter touches supersample^2 texels per output
            // pixel; per-texel pow() would put the whole bake in the hundreds of ms.
            f32 srgbToLinear[256];
            for (u32 i = 0; i < 256; ++i)
            {
                srgbToLinear[i] = core::SrgbToLinear(static_cast<f32>(i) / 255.0f);
            }
            core::Array<u8> out(static_cast<usize>(atlasWidth) * atlasHeight * 4u);
            const f32 invCount = 1.0f / static_cast<f32>(supersample * supersample);
            for (u32 y = 0; y < atlasHeight; ++y)
            {
                for (u32 x = 0; x < atlasWidth; ++x)
                {
                    f32 r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
                    for (u32 sy = 0; sy < supersample; ++sy)
                    {
                        const u8* row = pixels +
                                        static_cast<usize>(y * supersample + sy) * rowPitch +
                                        static_cast<usize>(x) * supersample * 4u;
                        for (u32 sx = 0; sx < supersample; ++sx)
                        {
                            const u8* texel = row + static_cast<usize>(sx) * 4u;
                            r += srgbToLinear[texel[0]];
                            g += srgbToLinear[texel[1]];
                            b += srgbToLinear[texel[2]];
                            a += static_cast<f32>(texel[3]) / 255.0f;
                        }
                    }
                    r *= invCount;
                    g *= invCount;
                    b *= invCount;
                    a *= invCount;
                    if (a > 0.0001f)
                    {
                        r /= a;
                        g /= a;
                        b /= a;
                    }
                    u8* dst = out.Data() + (static_cast<usize>(y) * atlasWidth + x) * 4u;
                    dst[0] = static_cast<u8>(
                        core::Clamp(core::LinearToSrgb(r) * 255.0f + 0.5f, 0.0f, 255.0f));
                    dst[1] = static_cast<u8>(
                        core::Clamp(core::LinearToSrgb(g) * 255.0f + 0.5f, 0.0f, 255.0f));
                    dst[2] = static_cast<u8>(
                        core::Clamp(core::LinearToSrgb(b) * 255.0f + 0.5f, 0.0f, 255.0f));
                    dst[3] = static_cast<u8>(core::Clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
                }
            }
            return core::MakeUnique<image::OwnedImageData>(
                allocator, atlasWidth, atlasHeight, image::PixelFormat::RGBA8,
                core::Move(out), image::ImageColorSpace::Srgb);
        }


        struct Attached
        {
            graphics::RenderWindow* window = nullptr;
            UIWindowData* data = nullptr; // borrowed (the RenderWindow owns the payload)
        };

        [[nodiscard]] UIWindowData* Find(graphics::RenderWindow* window)
        {
            for (Attached& a : m_attached)
            {
                if (a.window == window)
                {
                    return a.data;
                }
            }
            return nullptr;
        }

        [[nodiscard]] Attached* FindByWindowId(u32 windowId)
        {
            if (windowId == 0)
            {
                return nullptr;
            }
            for (Attached& a : m_attached)
            {
                if (a.window->Window().Id() == windowId)
                {
                    return &a;
                }
            }
            return nullptr;
        }

        // The attached window that owns the currently mouse-captured view (a dock-window edge-resize, a
        // slider drag, ...), or null. Input must keep flowing to it even as the cursor leaves the window.
        [[nodiscard]] Attached* CapturedWindow()
        {
            View* captured = m_ctx.GetFocusManager()->CapturedView();
            if (captured == nullptr)
            {
                return nullptr;
            }
            RootView* capRoot = captured->Root();
            for (Attached& a : m_attached)
            {
                if (a.data->root.Get() == capRoot)
                {
                    return &a;
                }
            }
            return nullptr;
        }

        // Pump one window's mouse using live desktop-global coords mapped to that window's local space, so a
        // captured resize/drag keeps updating even when the cursor leaves the window (the surface freezes
        // its position when not hovered). Button state comes from the raw mouse, so the release ends it.
        void PumpWindowAtGlobal(Attached& a)
        {
            const f32 w = static_cast<f32>(a.window->Window().Width());
            const f32 h = static_cast<f32>(a.window->Window().Height());
            a.data->surface->SetRegion(core::Rectangle{0.0f, 0.0f, w, h});
            a.data->surface->SetContentSize(core::Float2{w, h});
            m_ctx.SetActiveInputRoot(a.data->root.Get());

            shell::IMouse* mouse =
                (m_shell->Input() != nullptr) ? m_shell->Input()->Mouse() : nullptr;
            if (mouse == nullptr)
            {
                return;
            }
            const f32 mx = mouse->GlobalX() - static_cast<f32>(a.window->Window().X());
            const f32 my = mouse->GlobalY() - static_cast<f32>(a.window->Window().Y());
            m_bridge->PumpMouseAt(mx, my, mouse);
        }

        // Resolve the VG shaders through the shared ShaderSystemHost over the data mount: cooked
        // WGSL from Shaders/shaders.dpak in a dist or the browser (no compiler), or on-demand DXC
        // over Shaders/ in dev. The VG shaders ship with the engine's shaders like every other
        // shader (vg.vs, vg.ps), so this is the same path the renderer uses.
        void InitShaders(foundation::vfs::IFileSystem& dataFileSystem)
        {
            if (!m_shaderHost.Initialize(*m_device->Raw(), dataFileSystem))
            {
                return; // no compiler and no pack: the UI is not drawn (logged, not a crash)
            }
            m_vs = m_shaderHost.GetVariant(u8"vg", shaders::ShaderStage::Vertex,
                                           shaders::ShaderFlags::None);
            m_fs = m_shaderHost.GetVariant(u8"vg", shaders::ShaderStage::Fragment,
                                           shaders::ShaderFlags::None);
            m_gradRadialFs = m_shaderHost.GetVariant(u8"vg_grad_radial",
                                                     shaders::ShaderStage::Fragment,
                                                     shaders::ShaderFlags::None);
            m_gradConicFs = m_shaderHost.GetVariant(u8"vg_grad_conic",
                                                    shaders::ShaderStage::Fragment,
                                                    shaders::ShaderFlags::None);
            // MSDF text fragment (the DistanceField draw-mode pipeline). With an older cooked pack
            // that lacks it, distance-field glyph runs fall back to the default sampler.
            m_distanceFieldFragmentShader = m_shaderHost.GetVariant(u8"vg_df", shaders::ShaderStage::Fragment,
                                             shaders::ShaderFlags::None);
            // Box-shadow fragment (the BoxShadow draw-mode pipeline). With a pack that lacks it,
            // shadows are skipped rather than drawn wrong.
            m_boxShadowFs = m_shaderHost.GetVariant(u8"vg_shadow", shaders::ShaderStage::Fragment,
                                                    shaders::ShaderFlags::None);
            // Analytic boxes (VGContext::DrawBox). With a pack that lacks them, boxes tessellate.
            m_boxVs = m_shaderHost.GetVariant(u8"vg_box", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
            m_boxFs = m_shaderHost.GetVariant(u8"vg_box", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
            // Layer composites (VGContext::PushLayer). With a pack that lacks them, layers fall
            // back to opacity pushes.
            m_layerFs = m_shaderHost.GetVariant(u8"vg_layer", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
            // Layer blurs. A pack that lacks them leaves blurred layers sharp.
            m_blurVs = m_shaderHost.GetVariant(u8"vg_blur", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
            m_blurFs = m_shaderHost.GetVariant(u8"vg_blur", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
        }
        core::IAllocator* m_allocator;

        graphics::GraphicsDevice* m_device; // borrowed
        rhi::ShaderModule* m_distanceFieldFragmentShader = nullptr; // MSDF text fragment (borrowed; null = no DF pipeline)
        shell::IShell* m_shell;             // borrowed
        fonts::IFontService* m_fonts;       // borrowed
        shaders::ShaderSystemHost m_shaderHost; // owns the ShaderSystem and the VG modules (ctor allocator)
        rhi::ShaderModule* m_vs = nullptr;      // borrowed from m_shaderHost
        rhi::ShaderModule* m_fs = nullptr;      // borrowed from m_shaderHost
        rhi::ShaderModule* m_gradRadialFs = nullptr; // per-pixel radial gradient (borrowed)
        rhi::ShaderModule* m_gradConicFs = nullptr;  // per-pixel conic gradient (borrowed)
        rhi::ShaderModule* m_boxShadowFs = nullptr;  // blurred rounded rect (borrowed)
        rhi::ShaderModule* m_boxVs = nullptr;        // analytic box (borrowed)
        rhi::ShaderModule* m_boxFs = nullptr;
        rhi::ShaderModule* m_layerFs = nullptr;      // layer composite (borrowed)
        rhi::ShaderModule* m_blurVs = nullptr;       // layer blur (borrowed)
        rhi::ShaderModule* m_blurFs = nullptr;

        UIContext m_ctx; // shared context; owns every window's RootView (allocator from the ctor)
        core::UniquePtr<shell::InputRouter> m_router;
        core::UniquePtr<UiInputBridge> m_bridge;
        core::UniquePtr<ShellClipboard> m_clipboard;
        core::Array<Attached> m_attached;
        core::u32 m_dragReleaseMissedFrames = 0; // stuck-drag watchdog (see Update)
        // Baked icon atlases (BakeSvgDrawables); the drawables' variants borrow these.
        core::Array<core::UniquePtr<image::OwnedImageData>> m_bakedIconAtlases;
        bool m_themeIconsBaked = false; // first AttachWindow bakes lazily; rebake via BakeThemeIcons
        bool m_damageGateEnabled = true; // SetDamageGatingEnabled(false) turns the gate off
        bool m_frameDamaged = true;      // frame-scoped (set in Update, read by RenderWindow)
        u64 m_framesDrawn = 0;
        u64 m_framesSkipped = 0;
        u64 m_framesLaidOut = 0;
        f32 m_uiScale = 1.0f; // user preference multiplier on the OS content scale
        // Default near-black, stored linear (matches an sRGB backbuffer's clear semantics).
        rhi::ClearColor m_clear = rhi::ClearColor(0.006f, 0.006f, 0.009f, 1.0f);
        rhi::ClearColor m_clearEncoded = rhi::ClearColor(core::LinearToSrgb(0.006f), core::LinearToSrgb(0.006f),
                                                         core::LinearToSrgb(0.009f), 1.0f);
        vg::VGBlendSpace m_blendSpace = vg::VGBlendSpace::Encoded;
    };
}
