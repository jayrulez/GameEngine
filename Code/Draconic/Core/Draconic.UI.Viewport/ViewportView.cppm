// Draconic UI Viewport - `draconic.ui.viewport`
//
// ViewportView: a retained-mode ui::View that hosts 3D-rendered content. It owns an offscreen
// color + depth render target, fires a render callback (OnRender) so the app draws 3D into those
// targets through the frame's command encoder, then displays the color target as an image via the
// UI's VGContext (registered into the per-window VGRenderer as an external texture).
//
// Adapted (NOT a faithful port) from Sedulous.UI.Viewport/ViewportView.bf on two axes, per design
// sign-off:
//   * Fit math is foundation::ContentFit (Stretch/Letterbox/Crop/IntegerScale) - the same value type used
//     for input hit-testing - instead of a bespoke ComputeContentRect/ScreenToTexture. Draw (DstRect/
//     SrcRect) and input (ToContent) share one computation so they can never drift.
//   * Input is a shell::InputSurface (gated, content-space IMouse/IKeyboard), NOT Sedulous's
//     IViewportInputHandler event list. The app runs an InputRouter, registers Surface(), and drives a
//     controller (e.g. FlyCamera) from the gated devices - occlusion-gated by IsHovered()/IsFocused().
//
// Barriers: draconic's RHI is explicit, so RenderContent brackets the app's OnRender with the color/
// depth state transitions (Sedulous's RHI tracked these implicitly) - the one necessary deviation.
//
// A single offscreen RT (not a per-frame ring) is correct: the RT is only GPU-touched (3D pass writes,
// UI pass samples) on one queue, so submission order serializes frame N+1's write after frame N's read.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.viewport;

import draconic.foundation;
import draconic.rhi;
import draconic.image;
import draconic.vg;
import draconic.vg.renderer;
import draconic.ui;
import draconic.shell;

using namespace draconic::foundation;

export namespace draconic::ui::viewport
{
    namespace rhi = draconic::rhi;
    namespace image = draconic::image;
    namespace vg = draconic::vg;

    class ViewportView;

    /// Invoked to render 3D content into the viewport's offscreen targets. The handler records into
    /// `encoder` - typically BeginRenderPass on view.ColorTargetView() + DepthTargetView(), clearing
    /// with view.ClearColor, drawing, then End(). The surrounding resource barriers are handled by
    /// RenderContent, so the handler only owns the pass(es) and draws.
    using ViewportRenderDelegate =
        Function<void(ViewportView& view, rhi::CommandEncoder& encoder, i32 frameIndex)>;

    class ViewportView : public View
    {
        DRACONIC_OBJECT(ViewportView, View)
    public:
        /// Render callback (set by the app). Fired by RenderContent while the color/depth targets are
        /// in their render states.
        ViewportRenderDelegate OnRender;

        /// Fired after the render target is (re)created, with the new pixel size. Lets a system mirror
        /// the viewport resolution (e.g. a runtime UI that must lay out at the same canvas).
        Function<void(u32 width, u32 height)> OnRenderTargetResized;

        /// Clear color for the 3D pass background (read by the render callback).
        rhi::ClearColor ClearColor{0.098f, 0.098f, 0.118f, 1.0f};

        ViewportView() { IsFocusable = true; }
        ~ViewportView() override { ReleaseResources(); }

        /// Release the GPU targets + external-texture registration eagerly, while the device and the
        /// per-window VGRenderer are still alive. Call from the app's shutdown BEFORE the window (and its
        /// VGRenderer) is torn down - the view may outlive the window inside a retained view tree, so the
        /// destructor must not be the thing that unregisters. Idempotent; the destructor then no-ops.
        void Shutdown()
        {
            ReleaseResources();
            m_renderer = nullptr;
            m_device = nullptr;
        }

        ViewportView(const ViewportView&) = delete;
        ViewportView& operator=(const ViewportView&) = delete;

        /// Wire the GPU device (graphics::GraphicsDevice::Raw()), the per-window VGRenderer that draws
        /// this view's window (UIHost::RendererFor(window)), and the shell input manager + window id used
        /// to build the gated input surface. Call once before the first layout.
        void Initialize(rhi::Device* device, vg::renderer::VGRenderer* renderer,
                        shell::IInputManager* input, u32 windowId)
        {
            m_device = device;
            m_renderer = renderer;
            if (input != nullptr && !m_surface)
            {
                const ContentFit fit{Rectangle{0, 0, 1, 1}, Float2{1, 1}, m_fitMode};
                m_surface =
                    MakeUnique<shell::InputSurface>(DefaultAllocator(), input, windowId, fit);
            }
        }

        /// Re-bind the view to a different window's VGRenderer + window id. Call when a dockable panel
        /// hosting this viewport moves windows (undock into a float, redock into the main window): the
        /// color target is re-registered into the new window's renderer (so its UI can sample it) and the
        /// input surface is re-targeted so the router routes to the new window. The offscreen GPU targets
        /// themselves are unchanged. (This is the per-renderer undock path - it moves the RT between one
        /// renderer at a time; sampling ONE RT in TWO windows at once would need the shared external-
        /// texture cache, still deferred.)
        void AttachToWindow(vg::renderer::VGRenderer* renderer, u32 windowId)
        {
            if (renderer != m_renderer)
            {
                if (m_registered && m_renderer != nullptr)
                {
                    if (m_device != nullptr)
                    {
                        m_device->WaitIdle();
                    }
                    m_renderer->UnregisterExternalTexture(m_imageRef.Get());
                    m_registered = false;
                }
                m_renderer = renderer;
                if (m_renderer != nullptr && m_colorView != nullptr)
                {
                    m_renderer->RegisterExternalTexture(m_imageRef.Get(), m_colorView);
                    m_registered = true;
                }
            }
            if (m_surface)
            {
                m_surface->SetWindow(windowId);
            }
        }

        // === Fit mode ===
        [[nodiscard]] FitMode GetFitMode() const noexcept { return m_fitMode; }
        void SetFitMode(FitMode mode) noexcept
        {
            m_fitMode = mode;
            if (m_surface)
            {
                m_surface->SetFitMode(mode);
            }
        }

        // === Render-target formats ===
        // The view owns the offscreen formats (single source of truth): the OnRender delegate builds its
        // pipeline from ColorFormat()/DepthFormat() so it always agrees with the actual attachments. HDR
        // defaults (RGBA16Float + Depth32Float); an LDR game-in-a-panel or a depth+stencil viewport can
        // override via SetFormats before first layout (or after - it recreates the targets).
        [[nodiscard]] rhi::TextureFormat ColorFormat() const noexcept { return m_colorFormat; }
        [[nodiscard]] rhi::TextureFormat DepthFormat() const noexcept { return m_depthFormat; }
        void SetFormats(rhi::TextureFormat color, rhi::TextureFormat depth)
        {
            if (color == m_colorFormat && depth == m_depthFormat)
            {
                return;
            }
            m_colorFormat = color;
            m_depthFormat = depth;
            if (m_textureWidth > 0 && m_textureHeight > 0)
            {
                ResizeRenderTarget(m_textureWidth, m_textureHeight);
            }
        }

        // === Render-target queries ===
        [[nodiscard]] bool IsReady() const noexcept
        {
            return m_colorView != nullptr && m_depthView != nullptr;
        }
        [[nodiscard]] rhi::TextureView* ColorTargetView() const noexcept { return m_colorView; }
        [[nodiscard]] rhi::TextureView* DepthTargetView() const noexcept { return m_depthView; }
        [[nodiscard]] rhi::Texture* ColorTexture() const noexcept { return m_colorTexture; }
        [[nodiscard]] u32 RenderWidth() const noexcept { return m_textureWidth; }
        [[nodiscard]] u32 RenderHeight() const noexcept { return m_textureHeight; }

        // === Fixed render resolution (preview modes) ===
        // 0x0 = follow the panel layout (default). A fixed size renders the content at that
        // resolution regardless of panel size - pair with FitMode::Letterbox so presentation
        // AND input (mouse + touch map through the same ContentFit) stay aspect-correct.
        void SetFixedResolution(u32 fixedWidth, u32 fixedHeight)
        {
            if (m_fixedWidth == fixedWidth && m_fixedHeight == fixedHeight)
            {
                return;
            }
            m_fixedWidth = fixedWidth;
            m_fixedHeight = fixedHeight;
            if (fixedWidth > 0 && fixedHeight > 0)
            {
                ResizeRenderTarget(fixedWidth, fixedHeight);
            }
            else if (Width() > 0.0f && Height() > 0.0f)
            {
                ResizeRenderTarget(static_cast<u32>(Max(1.0f, Width())),
                                   static_cast<u32>(Max(1.0f, Height())));
            }
        }
        [[nodiscard]] u32 FixedWidth() const noexcept { return m_fixedWidth; }
        [[nodiscard]] u32 FixedHeight() const noexcept { return m_fixedHeight; }

        /// For content renderers that manage target transitions THEMSELVES (e.g. a frame graph
        /// importing the color target via current/final states, like render::TargetState): read
        /// the tracked color state to feed the import, then record what the graph left behind.
        /// Resize resets the tracked state to Undefined. RenderContent-based content (which
        /// brackets its own barriers) never touches these.
        [[nodiscard]] rhi::ResourceState ColorState() const noexcept { return m_colorState; }
        void SetColorState(rhi::ResourceState state) noexcept { m_colorState = state; }

        // === Input ===
        /// Hosted-content text input: when the content this viewport hosts (an embedded
        /// game's UI) has a focused text editor, the viewport - the HOST context's
        /// focused view - reports WantsTextInput, so the host window's IME lifecycle
        /// (UiInputBridge::SyncTextInput over the host UIContext) follows the embedded
        /// focus without a second bridge fighting over StartTextInput/StopTextInput.
        /// Pushed per frame by the hosting page.
        void SetHostedTextInputWanted(bool wanted) noexcept { m_hostedTextInputWanted = wanted; }
        [[nodiscard]] bool WantsTextInput() const override { return m_hostedTextInputWanted; }
        [[nodiscard]] shell::InputSurface* Surface() const noexcept { return m_surface.Get(); }
        [[nodiscard]] shell::IMouse* Mouse() const noexcept
        {
            return m_surface ? m_surface->Mouse() : nullptr;
        }
        [[nodiscard]] shell::IKeyboard* Keyboard() const noexcept
        {
            return m_surface ? m_surface->Keyboard() : nullptr;
        }
        /// Content-normalized [0,1] touch points inside the drawn content only (the surface
        /// transforms + spatially gates - see InputSurface).
        [[nodiscard]] shell::ITouch* Touch() const noexcept
        {
            return m_surface ? m_surface->Touch() : nullptr;
        }

        /// Sync the input surface's region to this view's laid-out window-space rect (+ content size /
        /// fit). Call each frame after the UI has laid out and before the router's Update(). The region
        /// is always the real rect so coordinate transforms stay correct; gating (whether the camera
        /// actually reads the devices) is the app's IsHovered()/IsFocused() check.
        void SyncInputRegion()
        {
            if (!m_surface)
            {
                return;
            }
            View* root = this;
            while (root->Parent != nullptr)
            {
                root = root->Parent;
            }
            // The UI tree lays out in LOGICAL units - RootView divides the physical window by its
            // DpiScale (OS content scale x the editor UI-scale preference). But the InputRouter
            // transforms the RAW mouse, which is in PHYSICAL window pixels. So the surface region +
            // window size must be PHYSICAL, or hover/pick/gizmo drift by the scale factor at UI
            // scale != 100%. The CONTENT resolution stays the RT's own size (MakeMouseRay divides
            // the content mouse by RenderWidth), so only the region-space conversion is needed here.
            f32 dpi = 1.0f;
            if (draconic::ui::RootView* rv = draconic::foundation::Cast<draconic::ui::RootView>(root))
            {
                dpi = Max(rv->DpiScale, 0.01f);
            }
            const Float2 tl = LocalToScreen(Float2{0.0f, 0.0f});
            m_surface->SetRegion(
                Rectangle{tl.x * dpi, tl.y * dpi, Width() * dpi, Height() * dpi});
            m_surface->SetContentSize(
                Float2{static_cast<f32>(m_textureWidth), static_cast<f32>(m_textureHeight)});
            m_surface->SetFitMode(m_fitMode);
            // Window pixel size (the root view spans the client area, in physical pixels): the
            // touch transform converts normalized finger coords through it.
            m_surface->SetWindowSize(Float2{root->Width() * dpi, root->Height() * dpi});
        }

        // === 3D render ===

        /// Clears the color target to ClearColor and leaves it SHADER-READ - for hosts
        /// whose content is sometimes idle (the Game tab before Play): the UI samples the
        /// texture every frame, so an undrawn frame must still define its layout.
        void ClearContent(rhi::CommandEncoder& encoder)
        {
            if (!IsReady())
            {
                return;
            }
            encoder.TransitionTexture(m_colorTexture, m_colorState,
                                      rhi::ResourceState::RenderTarget);
            rhi::RenderPassDesc pass;
            rhi::ColorAttachment color;
            color.view = m_colorView;
            color.loadOp = rhi::LoadOp::Clear;
            color.storeOp = rhi::StoreOp::Store;
            color.clearValue = ClearColor;
            pass.colorAttachments.Add(color);
            if (rhi::RenderPassEncoder* rp = encoder.BeginRenderPass(pass))
            {
                rp->End();
            }
            encoder.TransitionTexture(m_colorTexture, rhi::ResourceState::RenderTarget,
                                      rhi::ResourceState::ShaderRead);
            m_colorState = rhi::ResourceState::ShaderRead;
        }

        /// Render the 3D content into the offscreen targets. Call from OnRenderWindow BEFORE the UIHost
        /// draws the window's UI (which samples this view's color target). Brackets the app's OnRender
        /// callback with the required color/depth state transitions.
        void RenderContent(rhi::CommandEncoder& encoder, i32 frameIndex)
        {
            if (!IsReady() || !OnRender)
            {
                return;
            }

            encoder.TransitionTexture(m_colorTexture, m_colorState,
                                      rhi::ResourceState::RenderTarget);
            if (m_depthState != rhi::ResourceState::DepthStencilWrite)
            {
                encoder.TransitionTexture(m_depthTexture, m_depthState,
                                          rhi::ResourceState::DepthStencilWrite);
                m_depthState = rhi::ResourceState::DepthStencilWrite;
            }

            OnRender(*this, encoder, frameIndex);

            encoder.TransitionTexture(m_colorTexture, rhi::ResourceState::RenderTarget,
                                      rhi::ResourceState::ShaderRead);
            m_colorState = rhi::ResourceState::ShaderRead;
        }

        // === Layout ===
    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize =
                Float2{constraints.ConstrainWidth(256.0f), constraints.ConstrainHeight(256.0f)};
        }

        void OnLayout(f32 /*left*/, f32 /*top*/, f32 width, f32 height) override
        {
            const u32 w = m_fixedWidth > 0 ? m_fixedWidth : static_cast<u32>(Max(1.0f, width));
            const u32 h = m_fixedHeight > 0 ? m_fixedHeight : static_cast<u32>(Max(1.0f, height));
            if (w != m_textureWidth || h != m_textureHeight)
            {
                ResizeRenderTarget(w, h);
            }
        }

        // === Draw ===
    public:
        void OnDraw(UIDrawContext& ctx) override
        {
            if (m_registered && m_textureWidth > 0 && m_textureHeight > 0)
            {
                const ContentFit fit{
                    Rectangle{0.0f, 0.0f, Width(), Height()},
                    Float2{static_cast<f32>(m_textureWidth), static_cast<f32>(m_textureHeight)},
                    m_fitMode};
                const Rectangle dst = fit.DstRect();
                // Letterbox/IntegerScale leave bars on one axis - paint them black so they don't show
                // stale framebuffer content.
                if (dst.width < Width() || dst.height < Height())
                {
                    ctx.VG().FillRect(Rectangle{0.0f, 0.0f, Width(), Height()},
                                      Color{0.0f, 0.0f, 0.0f, 1.0f});
                }
                ctx.VG().DrawImage(m_imageRef.Get(), dst, fit.SrcRect(), Color::White);
            }
            else
            {
                ctx.VG().FillRect(Rectangle{0.0f, 0.0f, Width(), Height()},
                                  Color{0.098f, 0.098f, 0.118f, 1.0f});
            }
        }

    private:
        void ResizeRenderTarget(u32 width, u32 height)
        {
            if (m_device == nullptr)
            {
                return;
            }

            // The GPU must be idle before we free targets it may still be sampling (Sedulous does the
            // same on resize). Also the invalidation point for the external-texture registration.
            if (m_colorTexture != nullptr || m_depthTexture != nullptr)
            {
                m_device->WaitIdle();
            }

            if (m_registered && m_renderer != nullptr)
            {
                m_renderer->UnregisterExternalTexture(m_imageRef.Get());
                m_registered = false;
            }

            DestroyTargets();

            m_textureWidth = width;
            m_textureHeight = height;
            m_colorState = rhi::ResourceState::Undefined;
            m_depthState = rhi::ResourceState::Undefined;

            // The identity key the VGRenderer maps to the external color view (dimensions only, no pixels).
            m_imageRef = MakeUnique<image::ImageDataRef>(DefaultAllocator(), width, height);

            rhi::TextureDesc colorDesc =
                rhi::TextureDesc::RenderTarget(m_colorFormat, width, height, 1, u8"ViewportColor");
            if (!m_device->CreateTexture(colorDesc, m_colorTexture).IsOk())
            {
                m_colorTexture = nullptr;
                return;
            }
            rhi::TextureViewDesc colorViewDesc{};
            colorViewDesc.format = m_colorFormat;
            if (!m_device->CreateTextureView(m_colorTexture, colorViewDesc, m_colorView).IsOk())
            {
                m_colorView = nullptr;
                return;
            }

            rhi::TextureDesc depthDesc =
                rhi::TextureDesc::DepthBuffer(m_depthFormat, width, height, 1, u8"ViewportDepth");
            if (!m_device->CreateTexture(depthDesc, m_depthTexture).IsOk())
            {
                m_depthTexture = nullptr;
                return;
            }
            rhi::TextureViewDesc depthViewDesc{};
            depthViewDesc.format = m_depthFormat;
            if (!m_device->CreateTextureView(m_depthTexture, depthViewDesc, m_depthView).IsOk())
            {
                m_depthView = nullptr;
                return;
            }

            if (m_renderer != nullptr)
            {
                m_renderer->RegisterExternalTexture(m_imageRef.Get(), m_colorView);
                m_registered = true;
            }

            if (OnRenderTargetResized)
            {
                OnRenderTargetResized(width, height);
            }
        }

        void DestroyTargets()
        {
            if (m_device == nullptr)
            {
                return;
            }
            if (m_depthView != nullptr)
            {
                m_device->DestroyTextureView(m_depthView);
                m_depthView = nullptr;
            }
            if (m_depthTexture != nullptr)
            {
                m_device->DestroyTexture(m_depthTexture);
                m_depthTexture = nullptr;
            }
            if (m_colorView != nullptr)
            {
                m_device->DestroyTextureView(m_colorView);
                m_colorView = nullptr;
            }
            if (m_colorTexture != nullptr)
            {
                m_device->DestroyTexture(m_colorTexture);
                m_colorTexture = nullptr;
            }
        }

        void ReleaseResources()
        {
            if (m_device != nullptr && (m_colorTexture != nullptr || m_depthTexture != nullptr))
            {
                m_device->WaitIdle();
            }
            if (m_registered && m_renderer != nullptr)
            {
                m_renderer->UnregisterExternalTexture(m_imageRef.Get());
                m_registered = false;
            }
            DestroyTargets();
        }

        rhi::Device* m_device = nullptr;
        vg::renderer::VGRenderer* m_renderer = nullptr;

        UniquePtr<image::ImageDataRef> m_imageRef;
        rhi::Texture* m_colorTexture = nullptr;
        rhi::TextureView* m_colorView = nullptr;
        rhi::Texture* m_depthTexture = nullptr;
        rhi::TextureView* m_depthView = nullptr;
        rhi::ResourceState m_colorState = rhi::ResourceState::Undefined;
        rhi::ResourceState m_depthState = rhi::ResourceState::Undefined;

        bool m_hostedTextInputWanted = false;
        u32 m_fixedWidth = 0;
        u32 m_fixedHeight = 0;
        u32 m_textureWidth = 0;
        u32 m_textureHeight = 0;
        bool m_registered = false;
        FitMode m_fitMode = FitMode::Stretch;
        rhi::TextureFormat m_colorFormat = rhi::TextureFormat::RGBA16Float;
        rhi::TextureFormat m_depthFormat = rhi::TextureFormat::Depth32Float;

        UniquePtr<shell::InputSurface> m_surface;
    };

    DRACONIC_DEFINE_OBJECT(ViewportView, "draconic::ui::viewport")
}
