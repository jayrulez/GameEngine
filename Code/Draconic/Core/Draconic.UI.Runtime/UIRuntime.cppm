// Draconic::UIRuntime - the `draconic.ui.runtime` module.
//
// UIHost: the reusable bridge that draws draconic.ui on the runtime's multi-window graphics host
// (draconic.graphics). It owns ONE UIContext with N RootViews (one per window), compiles the shared VG
// shaders once, and gives each window its own VGContext + VGRenderer + InputSurface stashed on the
// RenderWindow as a UIWindowData (graphics::IRenderWindowData - the payload the host reserves for exactly
// this). Mirrors the Sedulous "one UIContext / N RootViews / per-window swapchain" model.
//
// TOOLKIT-FREE by design: it renders any RootView, so games (single-window) and the editor (multi-window)
// share it. The docking / floating-window workbench lives ABOVE this in draconic.ui.application, which is
// the only module that pulls in draconic.ui.toolkit.
//
// A game's IApplication owns a UIHost, AttachWindow(mainWindow, root) once, then calls Update(dt) from
// OnUpdate and RenderWindow(frame) from OnRenderWindow. No bespoke swapchain / VG bring-up in the app.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui.runtime;

import draconic.foundation;
import draconic.rhi;
import draconic.image;
import draconic.shaders;
import draconic.shaders.system; // ShaderSystemHost (cooked-pack-or-dev shader resolution)
import draconic.shell;
import draconic.graphics;
import draconic.vg;
import draconic.vg.renderer;
import draconic.vg.svg;
import draconic.fonts;
import draconic.ui;
import draconic.ui.shell;

namespace foundation = draconic::foundation;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;
namespace shell = draconic::shell;
namespace graphics = draconic::graphics;
namespace vg = draconic::vg;
namespace image = draconic::image;
namespace fonts = draconic::fonts;

// draconic::ui::runtime nests in draconic::ui, so UIContext / RootView / InputManager / UiInputBridge /
// ShellClipboard resolve unqualified.
export namespace draconic::ui::runtime
{
    using foundation::f32;
    using foundation::i32;
    using foundation::u32;
    using foundation::u64;
    using foundation::u8;
    using foundation::usize;

    /// Per-window UI payload stashed on a graphics::RenderWindow via SetData. Owns that window's VG
    /// context + renderer + input surface; shares ownership of its RootView with the UIContext.
    class UIWindowData final : public graphics::IRenderWindowData
    {
    public:
        foundation::RefPtr<RootView> root;
        foundation::UniquePtr<vg::VGContext> vg;
        vg::renderer::VGRenderer renderer;
        foundation::UniquePtr<shell::InputSurface> surface;

        // VG quality targets (host-provided per the stencil-then-cover design): a 4x MSAA
        // color target resolved into the backbuffer + a stencil attachment for the fill
        // pipelines. Sized to the swapchain; Update() recreates on resize (desktop hosts -
        // the WaitIdle there is outside any open frame). Null = plain single-sampled pass.
        rhi::Device* device = nullptr; // borrowed, for target destruction
        rhi::Texture* msaaColor = nullptr;
        rhi::TextureView* msaaColorView = nullptr;
        rhi::Texture* depthStencil = nullptr;
        rhi::TextureView* depthStencilView = nullptr;
        rhi::TextureFormat depthStencilFormat = rhi::TextureFormat::Undefined;
        u32 targetWidth = 0;
        u32 targetHeight = 0;

        ~UIWindowData() override { DestroyTargets(); }

        void DestroyTargets()
        {
            if (device == nullptr)
            {
                return;
            }
            if (msaaColorView != nullptr)
            {
                device->DestroyTextureView(msaaColorView);
                msaaColorView = nullptr;
            }
            if (msaaColor != nullptr)
            {
                device->DestroyTexture(msaaColor);
                msaaColor = nullptr;
            }
            if (depthStencilView != nullptr)
            {
                device->DestroyTextureView(depthStencilView);
                depthStencilView = nullptr;
            }
            if (depthStencil != nullptr)
            {
                device->DestroyTexture(depthStencil);
                depthStencil = nullptr;
            }
            targetWidth = 0;
            targetHeight = 0;
        }

        /// (Re)create the MSAA + stencil targets at the given size. Any failure tears
        /// everything down: the window falls back to the plain pass as a unit (a stencil
        /// pipeline without its attachment would be an invalid pass).
        [[nodiscard]] bool CreateTargets(rhi::TextureFormat colorFormat, u32 width, u32 height)
        {
            DestroyTargets();
            if (device == nullptr || width == 0 || height == 0 ||
                depthStencilFormat == rhi::TextureFormat::Undefined)
            {
                return false;
            }
            rhi::TextureDesc cd = rhi::TextureDesc::RenderTarget(colorFormat, width, height);
            cd.sampleCount = kMsaaSamples;
            cd.label = u8"UI MSAA color";
            if (!device->CreateTexture(cd, msaaColor).IsOk())
            {
                DestroyTargets();
                return false;
            }
            rhi::TextureDesc dd{};
            dd.dimension = rhi::TextureDimension::Texture2D;
            dd.format = depthStencilFormat;
            dd.width = width;
            dd.height = height;
            dd.depth = 1;
            dd.usage = rhi::TextureUsage::DepthStencil;
            dd.sampleCount = kMsaaSamples;
            dd.label = u8"UI stencil";
            if (!device->CreateTexture(dd, depthStencil).IsOk())
            {
                DestroyTargets();
                return false;
            }
            if (!device->CreateTextureView(msaaColor, rhi::TextureViewDesc{}, msaaColorView)
                     .IsOk() ||
                !device->CreateTextureView(depthStencil, rhi::TextureViewDesc{}, depthStencilView)
                     .IsOk())
            {
                DestroyTargets();
                return false;
            }
            targetWidth = width;
            targetHeight = height;
            return true;
        }

        static constexpr u32 kMsaaSamples = 4;
    };

    /// Renders draconic.ui on the runtime graphics host. Construct once (compiles the VG shaders), attach
    /// a RootView per window, drive Update()/RenderWindow() from the app's OnUpdate/OnRenderWindow.
    class UIHost
    {
    public:
        UIHost(graphics::GraphicsDevice& device, shell::IShell& shellRef,
               fonts::IFontService& fontService)
            : m_device(&device), m_shell(&shellRef), m_fonts(&fontService)
        {
            InitShaders();
            m_ctx.SetFontService(&fontService);
            m_router =
                foundation::MakeUnique<shell::InputRouter>(foundation::DefaultAllocator(), shellRef.Input());
            m_bridge = foundation::MakeUnique<UiInputBridge>(foundation::DefaultAllocator(), &m_ctx);
            m_clipboard = foundation::MakeUnique<ShellClipboard>(foundation::DefaultAllocator(), &shellRef);
            m_ctx.SetClipboard(m_clipboard.Get());
        }

        ~UIHost()
        {
            // Per-window VGRenderers are disposed with their RenderWindow's payload (freed by the host
            // after a GPU idle), not here. The VG shader modules (m_vs/m_fs) are BORROWED from the
            // shader host's ShaderSystem, which owns and frees them when it shuts down below.
        }

        UIHost(const UIHost&) = delete;
        UIHost& operator=(const UIHost&) = delete;

        /// The shared UI context (set the theme / stylesheet on it, look up focus, etc.).
        [[nodiscard]] UIContext& Context() noexcept { return m_ctx; }

        /// USER UI-scale factor multiplied onto every window's OS content scale (editor
        /// preference / accessibility). Roots pick it up next Update; callers re-bake any
        /// baked icon sets themselves at the new effective scale.
        void SetUiScale(f32 scale) noexcept { m_uiScale = foundation::Clamp(scale, 0.5f, 3.0f); }
        [[nodiscard]] f32 UiScale() const noexcept { return m_uiScale; }

        /// Background clear color behind the UI (the theme usually paints an opaque root over it).
        /// Takes the theme's color as authored (sRGB, like every UI color). The swapchain
        /// is an sRGB format, and pass CLEAR values are interpreted as LINEAR and hardware-
        /// encoded on store - so the sRGB components must be linearized here or the
        /// background clears visibly LIGHTER than the same color drawn by the UI (whose
        /// vertex path linearizes in VGRenderVertex). The project-manager screen, mostly
        /// bare background, showed this the loudest.
        void SetClearColor(f32 r, f32 g, f32 b, f32 a = 1.0f) noexcept
        {
            m_clear = rhi::ClearColor(foundation::SrgbToLinear(r), foundation::SrgbToLinear(g),
                                      foundation::SrgbToLinear(b), a);
        }

        /// Give a RenderWindow a RootView: builds its VGContext + VGRenderer (against the window's swap
        /// format + the device frame-ring) + InputSurface, and stashes the payload on the RenderWindow.
        void AttachWindow(graphics::RenderWindow* window, foundation::RefPtr<RootView> root)
        {
            if (window == nullptr || !root)
            {
                return;
            }

            auto data = foundation::MakeUnique<UIWindowData>(foundation::DefaultAllocator());
            data->root = root;
            data->vg = foundation::MakeUnique<vg::VGContext>(foundation::DefaultAllocator(), m_fonts);
            // Per-pixel radial/conic gradients only if both shaders resolved (a pre-cooked pack
            // may predate them); otherwise the renderer + context fall back to the affine LUT.
            const bool perPixelGrad = m_gradRadialFs != nullptr && m_gradConicFs != nullptr;

            // VG quality targets: 4x MSAA (fill/stroke edge AA) + a stencil attachment
            // (stencil-then-cover correctness for holes/self-intersection/even-odd). The
            // renderer's pipelines must match the pass, so the target config is decided
            // BEFORE Initialize; any creation failure falls back to the plain pass wholesale.
            data->device = m_device->Raw();
            data->depthStencilFormat = vg::renderer::PickStencilCapableFormat(
                *data->device, UIWindowData::kMsaaSamples);
            const bool targetsOk =
                data->CreateTargets(window->Swap()->Format(), window->Window().Width(),
                                    window->Window().Height());
            vg::renderer::VGTargetConfig targetConfig;
            if (targetsOk)
            {
                targetConfig.sampleCount = UIWindowData::kMsaaSamples;
                targetConfig.depthStencilFormat = data->depthStencilFormat;
            }
            data->renderer.Initialize(*m_device->Raw(), *m_vs, *m_fs, window->Swap()->Format(),
                                      static_cast<i32>(m_device->FramesInFlight()), m_dfFs,
                                      m_gradRadialFs, m_gradConicFs, targetConfig);
            data->vg->SetPerPixelGradients(perPixelGrad);
            data->vg->SetStencilFills(data->renderer.StencilFillsSupported());

            const f32 w = static_cast<f32>(window->Window().Width());
            const f32 h = static_cast<f32>(window->Window().Height());
            const foundation::ContentFit fit{foundation::Rectangle{0.0f, 0.0f, w, h}, foundation::Float2{w, h},
                                       foundation::FitMode::Stretch};
            data->surface = foundation::MakeUnique<shell::InputSurface>(
                foundation::DefaultAllocator(), m_shell->Input(), window->Window().Id(), fit);

            root->ViewportSize = foundation::Float2{w, h};
            root->DpiScale = window->Window().ContentScale() * m_uiScale;
            m_ctx.AddRootView(root.Get());
            m_router->AddSurface(data->surface.Get());
            m_bridge->SetTextInputTarget(
                &window->Window()); // IME target (last attach wins; refined per-window later)

            UIWindowData* raw = data.Get();
            // RE-attach (the editor swaps a window between the project-manager root and the
            // shell root): SetData below destroys the PREVIOUS payload - a live VGRenderer
            // whose pipelines/descriptor sets/buffers in-flight frames still reference. Idle
            // the GPU first; without this, replacing a window's root spews in-use validation
            // errors and can crash (the DetachWindow comment's race, hit for real).
            if (window->Data() != nullptr && m_device->Raw() != nullptr)
            {
                m_device->Raw()->WaitIdle();
            }
            window->SetData(static_cast<foundation::UniquePtr<UIWindowData>&&>(
                data)); // RenderWindow owns the payload
            m_attached.PushBack(Attached{window, raw});
        }

        /// Logical detach: stop routing input to the window and remove its root from the context, so
        /// nothing draws or hit-tests it anymore. The GPU payload (VGRenderer etc.) is deliberately LEFT
        /// on the RenderWindow - it is freed when the host destroys the RenderWindow (CloseWindow), which
        /// WaitIdles first. Freeing it here would race the GPU (no idle). So the docking-destroy path is
        /// DetachWindow(rw) followed by IApplicationHost::CloseWindow(rw).
        void DetachWindow(graphics::RenderWindow* window)
        {
            for (usize i = 0; i < m_attached.Size(); ++i)
            {
                if (m_attached[i].window == window)
                {
                    UIWindowData* data = m_attached[i].data;
                    // A detached root gets no input and no ticks, so its open popups (menus,
                    // dropdowns) can never dismiss - they would freeze and still be showing
                    // if the root is re-attached later. Close them all now.
                    if (PopupLayer* popups = data->root->GetPopupLayer())
                    {
                        popups->CloseAllPopups();
                    }
                    m_router->RemoveSurface(data->surface.Get());
                    m_ctx.RemoveRootView(data->root.Get());
                    m_attached.RemoveAt(i);
                    return;
                }
            }
        }

        /// Once per frame: pump each window's input, tick the context, lay out every root.
        void Update(f32 deltaTime)
        {
            m_router->Update();

            // Cross-window drag: while a drag is in flight and the cursor is over/from a SECONDARY window
            // (a floating dock window follows the cursor, so it sits under the pointer), route the drag to
            // the MAIN window using global-mouse -> main-relative coords so the main window's drop targets
            // (e.g. a DockManager) see the drag-over and accept the drop. Mirrors Sedulous's editor: the OS
            // window being under the cursor is bypassed; we feed the main window explicitly.
            DragDropManager* dd = m_ctx.DragDrop();
            Attached* mainW = m_attached.IsEmpty() ? nullptr : &m_attached[0];
            const u32 focusedId =
                (m_shell->Input() != nullptr) ? m_shell->Input()->FocusedWindow() : 0;
            const bool crossWindowDrag =
                (dd != nullptr && dd->IsDragging() && mainW != nullptr && focusedId != 0 &&
                 focusedId != mainW->window->Window().Id());

            // Route mouse to a SINGLE window per frame (Sedulous processes one active root, not all N):
            // pumping non-hovered windows re-asserts their frozen last mouse position, spuriously
            // re-hovering them and clobbering the shared hover/cursor state. Priority:
            //   1. cross-window drag  -> the MAIN window (float sits under the cursor; see above);
            //   2. a captured view    -> its window (a dock-window edge-resize, a slider drag, ...), fed
            //                            LIVE global-relative coords so it keeps working off-window;
            //   3. otherwise          -> the window under the pointer (normal surface pump, keeps scroll).
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
                    hoverW->data->surface->SetRegion(foundation::Rectangle{0.0f, 0.0f, w, h});
                    hoverW->data->surface->SetContentSize(foundation::Float2{w, h});
                    m_ctx.SetActiveInputRoot(hoverW->data->root.Get());
                    m_bridge->PumpFromSurface(*hoverW->data->surface);
                }
            }

            // Keyboard / text + IME follow the FOCUSED OS window: point the active input root and the
            // text-input target at the focused window's root, then dispatch its key/text events.
            if (shell::IInputManager* input = m_shell->Input())
            {
                Attached* focused = FindByWindowId(input->FocusedWindow());
                if (focused == nullptr && !m_attached.IsEmpty())
                {
                    focused = &m_attached[0];
                } // fallback: main
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
            // (dock-window resize, slider, ...) is in progress - without an app-global capture the OS stops
            // delivering events once the cursor leaves the window, stalling multi-window drag/resize at the
            // edge and losing a release that happens outside the window.
            if (m_shell->Input() != nullptr)
            {
                if (shell::IMouse* mouse = m_shell->Input()->Mouse())
                {
                    const bool capturing = (dd != nullptr && dd->IsDragging()) ||
                                           (m_ctx.GetFocusManager()->CapturedView() != nullptr);
                    mouse->SetGlobalCapture(capturing);

                    // Push the hovered view's cursor to the OS (borderless floats have no WM to show resize
                    // grips). The UI<->shell cursor mapping lives in the bridge, with the rest of the enum
                    // translation.
                    m_bridge->SyncCursor(*mouse);
                }
            }

            m_ctx.BeginFrame(deltaTime);
            for (Attached& a : m_attached)
            {
                a.data->root->ViewportSize =
                    foundation::Float2{static_cast<f32>(a.window->Window().Width()),
                                 static_cast<f32>(a.window->Window().Height())};
                // Effective scale = OS content scale x the user UI-scale preference;
                // refreshed per frame so monitor moves and preference changes both land.
                a.data->root->DpiScale = a.window->Window().ContentScale() * m_uiScale;
                m_ctx.UpdateRootView(a.data->root.Get());

                // Resize the VG quality targets with the window - here, OUTSIDE any open
                // frame, so the idle wait cannot yield mid-frame (desktop hosts only; the
                // web player uses UISubsystem, not this host). In-flight frames may still
                // reference the old targets, hence the idle before recreation.
                const u32 w = static_cast<u32>(a.window->Window().Width());
                const u32 h = static_cast<u32>(a.window->Window().Height());
                if (a.data->msaaColorView != nullptr && w != 0 && h != 0 &&
                    (a.data->targetWidth != w || a.data->targetHeight != h))
                {
                    m_device->Raw()->WaitIdle();
                    (void)a.data->CreateTargets(a.window->Swap()->Format(), w, h);
                }
            }
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

            data->vg->Clear();
            m_ctx.DrawRootView(data->root.Get(), *data->vg);
            vg::VGBatch& batch = data->vg->GetBatch();

            data->renderer.BeginFrame(static_cast<i32>(frame.frameIndex));
            const vg::renderer::VGRenderSlice slice = data->renderer.Prepare(
                batch, static_cast<i32>(frame.frameIndex), frame.width, frame.height);

            // Quality pass: render into the 4x MSAA color target (resolved into the
            // backbuffer by the pass) with the stencil attachment cleared to 0. Falls back
            // to the plain backbuffer pass when targets are absent or stale-sized (the
            // resize recreate happens in Update, outside the frame).
            if (data->msaaColorView != nullptr && data->targetWidth == frame.width &&
                data->targetHeight == frame.height && frame.encoder != nullptr)
            {
                // The host transitions only the backbuffer (BeginFrame); the per-window
                // MSAA + stencil attachments need their own transitions. From Undefined
                // every frame: both are fully cleared, previous contents discardable.
                frame.encoder->TransitionTexture(data->msaaColor, rhi::ResourceState::Undefined,
                                                 rhi::ResourceState::RenderTarget);
                frame.encoder->TransitionTexture(data->depthStencil,
                                                 rhi::ResourceState::Undefined,
                                                 rhi::ResourceState::DepthStencilWrite);
                rhi::ColorAttachment color{};
                color.view = data->msaaColorView;
                color.resolveTarget = frame.backbufferView;
                color.loadOp = rhi::LoadOp::Clear;
                color.storeOp = rhi::StoreOp::DontCare; // resolved; the MSAA texels can drop
                color.clearValue = m_clear;
                rhi::DepthStencilAttachment ds{};
                ds.view = data->depthStencilView;
                ds.depthLoadOp = rhi::LoadOp::Clear;
                ds.depthStoreOp = rhi::StoreOp::DontCare;
                ds.stencilLoadOp = rhi::LoadOp::Clear; // stencil-then-cover expects 0
                ds.stencilStoreOp = rhi::StoreOp::DontCare;
                ds.stencilClearValue = 0;
                rhi::RenderPassDesc rpd{};
                rpd.colorAttachments.Add(color);
                rpd.depthStencilAttachment = ds;
                rpd.label = u8"UI (msaa+stencil)";
                if (rhi::RenderPassEncoder* rp = frame.encoder->BeginRenderPass(rpd))
                {
                    data->renderer.Render(*rp, frame.width, frame.height,
                                          static_cast<i32>(frame.frameIndex), slice);
                    rp->End();
                }
                return;
            }

            rhi::RenderPassEncoder* rp = frame.BeginBackbufferPass(m_clear);
            if (rp != nullptr)
            {
                data->renderer.Render(*rp, frame.width, frame.height,
                                      static_cast<i32>(frame.frameIndex), slice);
            }
            frame.EndBackbufferPass();
        }

        /// Bake SVG drawables into a shared bitmap atlas: each drawable x size renders
        /// through the VG at 4x SUPERSAMPLE into an offscreen sRGB target, reads back, and
        /// box-downsamples on the CPU - the Godot-verified icon recipe (AA baked into the
        /// texels once; BakedSVGDrawable then draws pixel-snapped quads, so every instance
        /// of an icon samples identical texels). The atlas CPU image is owned HERE (the
        /// variants borrow it); call again after a DPI change with scaled sizes.
        /// Synchronous (one small GPU roundtrip) - call at startup/theme load, not per
        /// frame. Returns false untouched on any failure (drawables keep the live-vector
        /// fallback).
        bool BakeSvgDrawables(foundation::Span<BakedSVGDrawable* const> drawables,
                              foundation::Span<const u32> sizes)
        {
            constexpr u32 kSupersample = 4;
            constexpr u32 kPad = 1;
            constexpr u32 kAtlasWidth = 512;
            rhi::Device* device = m_device->Raw();
            if (device == nullptr || drawables.IsEmpty() || sizes.IsEmpty())
            {
                return false;
            }

            // Shelf layout in FINAL atlas coordinates.
            struct Cell
            {
                BakedSVGDrawable* drawable = nullptr;
                u32 size = 0;
                u32 x = 0;
                u32 y = 0;
            };
            foundation::Array<Cell> cells;
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
                    rowHeight = foundation::Max(rowHeight, size);
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

            // Offscreen sRGB target (render + copy-out).
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
                const foundation::Rectangle rect{static_cast<f32>(cell.x * kSupersample),
                                           static_cast<f32>(cell.y * kSupersample),
                                           static_cast<f32>(cell.size * kSupersample),
                                           static_cast<f32>(cell.size * kSupersample)};
                vg::svg::SVGRenderer::Render(bakeVg, cell.drawable->Document(), rect, {});
            }

            vg::renderer::VGRenderer bakeRenderer;
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
                          foundation::ErrorCode::Ok &&
                      device->CreateFence(0, fence) == foundation::ErrorCode::Ok;
            rhi::CommandEncoder* encoder = nullptr;
            if (ok)
            {
                ok = pool->CreateEncoder(encoder) == foundation::ErrorCode::Ok && encoder != nullptr;
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
                queue->Submit(foundation::Span<rhi::CommandBuffer* const>(buffers, 1), fence, 1);
                fence->Wait(1, ~0ull);
            }

            foundation::UniquePtr<image::OwnedImageData> atlas;
            if (ok)
            {
                const u8* pixels = static_cast<const u8*>(readBuffer->Map());
                ok = pixels != nullptr;
                if (ok)
                {
                    atlas = DownsampleBake(pixels, rowPitch, kAtlasWidth, atlasHeight,
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
                foundation::Array<BakedSVGDrawable::BakedVariant> variants;
                for (const Cell& cell : cells)
                {
                    if (cell.drawable != drawable)
                    {
                        continue;
                    }
                    BakedSVGDrawable::BakedVariant variant;
                    variant.atlas = atlasImage;
                    variant.srcRect =
                        foundation::Rectangle{static_cast<f32>(cell.x), static_cast<f32>(cell.y),
                                        static_cast<f32>(cell.size), static_cast<f32>(cell.size)};
                    variant.sizePx = static_cast<f32>(cell.size);
                    variants.PushBack(variant);
                }
                drawable->SetBakedVariants(foundation::Move(variants));
            }
            m_bakedIconAtlases.PushBack(foundation::Move(atlas));
            return true;
        }

        /// The per-window VGRenderer for an attached window, or null if not attached. Exposed so an app
        /// can register an external texture (e.g. a ui::viewport ViewportView's offscreen render target)
        /// into the same renderer that draws that window's UI, so the UI can sample it via DrawImage.
        [[nodiscard]] vg::renderer::VGRenderer* RendererFor(graphics::RenderWindow* window)
        {
            UIWindowData* data = Find(window);
            return data != nullptr ? &data->renderer : nullptr;
        }

        /// The attached window whose RootView is `root`, or null. Lets an app discover which window
        /// currently hosts a given view tree - e.g. a ViewportView whose dockable panel was floated into
        /// a new OS window, so the app can re-bind it (View::Root() -> WindowForRoot -> RendererFor).
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
        /// CPU half of the bake: sRGB-decode, average the SS x SS premultiplied box,
        /// UN-premultiply (the vg shader premultiplies its output, but DrawImage expects
        /// straight alpha - it premultiplies again at draw), sRGB-encode.
        [[nodiscard]] static foundation::UniquePtr<image::OwnedImageData>
        DownsampleBake(const u8* pixels, u32 rowPitch, u32 atlasWidth, u32 atlasHeight,
                       u32 supersample)
        {
            // 256-entry decode LUT: the box filter touches supersample^2 texels per output
            // pixel; per-texel pow() would put the whole bake in the hundreds of ms.
            f32 srgbToLinear[256];
            for (u32 i = 0; i < 256; ++i)
            {
                srgbToLinear[i] = foundation::SrgbToLinear(static_cast<f32>(i) / 255.0f);
            }
            foundation::Array<u8> out(static_cast<usize>(atlasWidth) * atlasHeight * 4u);
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
                        foundation::Clamp(foundation::LinearToSrgb(r) * 255.0f + 0.5f, 0.0f, 255.0f));
                    dst[1] = static_cast<u8>(
                        foundation::Clamp(foundation::LinearToSrgb(g) * 255.0f + 0.5f, 0.0f, 255.0f));
                    dst[2] = static_cast<u8>(
                        foundation::Clamp(foundation::LinearToSrgb(b) * 255.0f + 0.5f, 0.0f, 255.0f));
                    dst[3] = static_cast<u8>(foundation::Clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
                }
            }
            return foundation::MakeUnique<image::OwnedImageData>(
                foundation::DefaultAllocator(), atlasWidth, atlasHeight, image::PixelFormat::RGBA8,
                foundation::Move(out), image::ImageColorSpace::Srgb);
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

        // Pump one window's mouse using LIVE desktop-global coords mapped to that window's local space, so a
        // captured resize/drag keeps updating even when the cursor leaves the window (the surface freezes
        // its position when not hovered). Button state comes from the raw mouse, so the release ends it.
        void PumpWindowAtGlobal(Attached& a)
        {
            const f32 w = static_cast<f32>(a.window->Window().Width());
            const f32 h = static_cast<f32>(a.window->Window().Height());
            a.data->surface->SetRegion(foundation::Rectangle{0.0f, 0.0f, w, h});
            a.data->surface->SetContentSize(foundation::Float2{w, h});
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

        // Resolve the VG shaders through the shared ShaderSystemHost: cooked WGSL from shaders.dpak
        // in a dist/browser (no compiler), or on-demand DXC over Data/Shaders in dev. The VG shaders
        // ship in the engine corpus like every other shader (vg.vs / vg.ps), so this is the SAME path
        // the renderer uses - no more bespoke inline-HLSL compile here.
        void InitShaders()
        {
#ifdef DRACONIC_ENGINE_SHADER_DIR
            constexpr foundation::StringView kShaderRoot = u8"" DRACONIC_ENGINE_SHADER_DIR;
#else
            constexpr foundation::StringView kShaderRoot = u8"Shaders";
#endif
            if (!m_shaderHost.Initialize(*m_device->Raw(), kShaderRoot))
            {
                return; // no compiler and no pack - UI stays un-rendered (loud but not a crash)
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
            // MSDF text fragment (the DistanceField draw-mode pipeline); a pre-cooked pack
            // that predates it just means DF glyph runs fall back to the default sampler.
            m_dfFs = m_shaderHost.GetVariant(u8"vg_df", shaders::ShaderStage::Fragment,
                                             shaders::ShaderFlags::None);
        }

        graphics::GraphicsDevice* m_device; // borrowed
        rhi::ShaderModule* m_dfFs = nullptr; // MSDF text fragment (borrowed; null = no DF pipeline)
        shell::IShell* m_shell;             // borrowed
        fonts::IFontService* m_fonts;       // borrowed
        shaders::ShaderSystemHost m_shaderHost; // owns the ShaderSystem + the VG modules
        rhi::ShaderModule* m_vs = nullptr;      // borrowed from m_shaderHost
        rhi::ShaderModule* m_fs = nullptr;      // borrowed from m_shaderHost
        rhi::ShaderModule* m_gradRadialFs = nullptr; // per-pixel radial gradient (borrowed)
        rhi::ShaderModule* m_gradConicFs = nullptr;  // per-pixel conic gradient (borrowed)

        UIContext m_ctx; // shared context; owns N RootViews
        foundation::UniquePtr<shell::InputRouter> m_router;
        foundation::UniquePtr<UiInputBridge> m_bridge;
        foundation::UniquePtr<ShellClipboard> m_clipboard;
        foundation::Array<Attached> m_attached;
        // Default near-black, stored LINEAR (matches an sRGB backbuffer's clear semantics).
        // Baked icon atlases (BakeSvgDrawables): drawables' variants borrow these.
        foundation::Array<foundation::UniquePtr<image::OwnedImageData>> m_bakedIconAtlases;
        f32 m_uiScale = 1.0f; // user preference multiplier on the OS content scale
        rhi::ClearColor m_clear = rhi::ClearColor(0.006f, 0.006f, 0.009f, 1.0f);
    };
}
