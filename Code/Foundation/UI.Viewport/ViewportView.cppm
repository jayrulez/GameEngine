// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Viewport - `foundation.ui.viewport`
//
// ViewportView: a retained-mode ui::View that hosts 3D-rendered content. It owns an offscreen
// color + depth render target, fires a render callback (OnRender) so the app draws 3D into those
// targets through the frame's command encoder, then displays the color target as an image via the
// UI's VGContext (registered into the per-window VGRenderer as an external texture).
//
// Adapted (NOT a faithful port) from Sedulous.UI.Viewport/ViewportView.bf on two axes, per design
// sign-off:
//   * Fit math is core::ContentFit (Stretch/Letterbox/Crop/IntegerScale) - the same value type used
//     for input hit-testing - instead of a bespoke ComputeContentRect/ScreenToTexture. Draw (DstRect/
//     SrcRect) and input (ToContent) share one computation so they can never drift.
//   * Input is a shell::InputSurface (gated, content-space IMouse/IKeyboard), NOT Sedulous's
//     IViewportInputHandler event list. The app runs an InputRouter, registers Surface(), and drives a
//     controller (e.g. FlyCamera) from the gated devices - occlusion-gated by IsHovered()/IsFocused().
//
// Barriers: RHI is explicit, so RenderContent brackets the app's OnRender with the color/
// depth state transitions (Sedulous's RHI tracked these implicitly) - the one necessary deviation.
//
// A single offscreen RT (not a per-frame ring) is correct: the RT is only GPU-touched (3D pass writes,
// UI pass samples) on one queue, so submission order serializes frame N+1's write after frame N's read.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.viewport;

import foundation.core;
import foundation.rhi;
import foundation.image;
import foundation.vg;
import foundation.vg.renderer;
import foundation.ui;
import foundation.shell;

using namespace foundation::core;
namespace shell = foundation::shell;

export namespace foundation::ui::viewport
{
    namespace rhi = foundation::rhi;
    namespace image = foundation::image;
    namespace vg = foundation::vg;

    class ViewportView;

    /// Invoked to render 3D content into the viewport's offscreen targets. The handler records into
    /// `encoder` - typically BeginRenderPass on view.ColorTargetView() + DepthTargetView(), clearing
    /// with view.ClearColor, drawing, then End(). The surrounding resource barriers are handled by
    /// RenderContent, so the handler only owns the pass(es) and draws.
    using ViewportRenderDelegate =
        Function<void(ViewportView& view, rhi::CommandEncoder& encoder, i32 frameIndex)>;

    class ViewportView : public View
    {
        RTTI_OBJECT(ViewportView, View)
    public:
        /// Render callback (set by the app). Fired by RenderContent while the color/depth targets are
        /// in their render states.
        ViewportRenderDelegate OnRender;

        /// Fired after the render target is (re)created, with the new pixel size. Lets a system mirror
        /// the viewport resolution (e.g. a runtime UI that must lay out at the same canvas).
        Function<void(u32 width, u32 height)> OnRenderTargetResized;

        /// Clear color for the 3D pass background (read by the render callback). sRGB, like every
        /// UI colour: ClearContent decodes it, and a render host decodes it for its render view.
        rhi::ClearColor ClearColor{0.347f, 0.347f, 0.377f, 1.0f};
        /// ClearColor decoded to linear: the value a render target is cleared with.
        [[nodiscard]] rhi::ClearColor LinearClearColor() const noexcept
        {
            return rhi::ClearColor{SrgbToLinear(ClearColor.r), SrgbToLinear(ClearColor.g),
                                   SrgbToLinear(ClearColor.b), ClearColor.a};
        }

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
                    MakeUnique<shell::InputSurface>(MemoryAllocator(), input, windowId, fit);
            }
            // A view mounted and laid out BEFORE its device arrived has a size and no targets,
            // and an unchanged size never lays out again - so it would paint its placeholder
            // forever (every editor preview page opens this way). Make them now.
            if (m_device != nullptr && m_textureWidth == 0 && m_layoutWidth > 0 && m_layoutHeight > 0)
            {
                ResizeRenderTarget(m_layoutWidth, m_layoutHeight);
            }
        }

        /// Re-bind the view to a different window's VGRenderer + window id. Call when a dockable panel
        /// hosting this viewport moves windows (undock into a float, redock into the main window): the
        /// color target is re-registered into the new window's renderer (so its UI can sample it) and the
        /// input surface is re-targeted so the router routes to the new window. The offscreen GPU targets
        /// themselves are unchanged. (This is the per-renderer undock path - it moves the RT between one
        /// renderer at a time; sampling ONE RT in TWO windows at once would need the shared external-
        /// texture cache, which is not implemented.)
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
        /// The color target viewed in its sRGB or UNORM twin; null when its format has none.
        [[nodiscard]] rhi::TextureView* ColorTargetAlternateView() const noexcept { return m_colorAlternateView; }
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

        /// One app keyboard: true while the HOST UI's keyboard focus sits on some view other
        /// than this viewport (a dialog field, another widget, another tab's viewport). The
        /// hosting page feeds this into its InputRouter's external keyboard capture each frame
        /// BEFORE Update(), so the viewport surface's focus yields and the hosted content
        /// (game UI, action bindings, camera keys - all reading the surface-gated source)
        /// stops receiving keys the editor widget is consuming. Null/absent focus does NOT
        /// count as elsewhere - clicking dead editor space must not mute a playing game.
        /// The host's keyboard focus is on this viewport (the view the user is playing in).
        [[nodiscard]] bool HostKeyboardFocusHere() const
        {
            return Context != nullptr && Context->GetFocusManager()->FocusedView() == this;
        }

        [[nodiscard]] bool HostKeyboardFocusElsewhere() const
        {
            if (Context == nullptr)
            {
                return false;
            }
            const View* focused = Context->GetFocusManager()->FocusedView();
            return focused != nullptr && focused != this;
        }

        /// The content reads the keyboard itself (a game running in the viewport) rather than
        /// through the host's bindings. The input surface already keeps the content from keys the
        /// host has; this is the other direction: while the surface has the keyboard, a key with
        /// no Ctrl, Alt or Gui held, Tab and the arrows included, is the content's and goes no
        /// further in the host, so the host's focus traversal and single key bindings never act
        /// on a key the game is also handling. A chord still reaches the host's shortcuts. Off
        /// for content that relies on the host's single key bindings (a scene editor's W/E/R).
        void SetCapturesKeys(bool captures) noexcept
        {
            m_capturesKeys = captures;
            WantsTabKey = captures;
        }
        [[nodiscard]] bool CapturesKeys() const noexcept { return m_capturesKeys; }

        /// The resolution the content draws at INSIDE the texture, fitted by `fit`, when it draws
        /// its own fitted image (a game at its render resolution, letterboxed in a panel-sized
        /// texture): the input surface then maps the pointer into that resolution. Nought is the
        /// texture's own.
        void SetContentResolution(u32 width, u32 height, FitMode fit,
                                  ContentRotation rotation = ContentRotation::None) noexcept
        {
            m_contentWidth = width;
            m_contentHeight = height;
            m_contentFit = fit;
            m_contentRotation = rotation;
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            if (ContentHasKeys() && !IsChord(e.Modifiers))
            {
                e.Handled = true;
            }
        }
        void OnKeyUp(KeyEventArgs& e) override
        {
            if (ContentHasKeys() && !IsChord(e.Modifiers))
            {
                e.Handled = true;
            }
        }

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
            if (foundation::ui::RootView* rv = foundation::core::Cast<foundation::ui::RootView>(root))
            {
                dpi = Max(rv->DpiScale, 0.01f);
            }
            const Float2 tl = LocalToScreen(Float2{0.0f, 0.0f});
            const Rectangle region{tl.x * dpi, tl.y * dpi, Width() * dpi, Height() * dpi};
            m_surface->SetRegion(region);
            m_surface->SetRotation(ContentRotation::None);
            const Float2 texture{static_cast<f32>(m_textureWidth), static_cast<f32>(m_textureHeight)};
            if (m_contentWidth > 0 && m_contentHeight > 0)
            {
                // The content draws at a resolution of its own inside the texture: the pointer
                // maps into THAT, through the content's own fit (and turn).
                const Float2 content{static_cast<f32>(m_contentWidth), static_cast<f32>(m_contentHeight)};
                m_surface->SetContentSize(content);
                m_surface->SetFitMode(m_contentFit);
                m_surface->SetRotation(m_contentRotation);
                if (m_fixedWidth > 0 && m_fixedHeight > 0 && texture.x > 0.0f && texture.y > 0.0f)
                {
                    // Two steps in one fit: the texture (a fixed size, a phone's screen) is fitted
                    // into the view, and the content into the texture. The content's own rectangle,
                    // carried through the first step, is where the pointer maps through the second.
                    const Rectangle drawn = ContentFit{region, texture, m_fitMode}.DstRect();
                    if (m_contentFit == FitMode::Crop)
                    {
                        m_surface->SetRegion(drawn); // no bars, a slice: the fit scales evenly
                    }
                    else
                    {
                        const Rectangle inside =
                            ContentFit{Rectangle{0.0f, 0.0f, texture.x, texture.y}, content, m_contentFit,
                                       m_contentRotation}
                                .DstRect();
                        const f32 sx = drawn.width / texture.x;
                        const f32 sy = drawn.height / texture.y;
                        m_surface->SetRegion(Rectangle{drawn.x + inside.x * sx, drawn.y + inside.y * sy,
                                                       inside.width * sx, inside.height * sy});
                        m_surface->SetFitMode(FitMode::Stretch); // the region IS the content's rectangle
                    }
                }
            }
            else
            {
                m_surface->SetContentSize(texture);
                m_surface->SetFitMode(m_fitMode);
            }
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
            color.clearValue = LinearClearColor();
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
            m_layoutWidth = w; // remembered for a device that arrives after this layout
            m_layoutHeight = h;
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
                // Live 3D content: the render target updates outside the UI's knowledge, so a
                // visible viewport keeps the UI damage chain alive (self-sustaining: draw ->
                // mark -> next frame draws). Refine to RT-content versioning when render-side
                // damage exists.
                Invalidate();
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
            m_imageRef = MakeUnique<image::ImageDataRef>(MemoryAllocator(), width, height);

            rhi::TextureDesc colorDesc =
                rhi::TextureDesc::RenderTarget(m_colorFormat, width, height, 1, u8"ViewportColor");
            // The 3D pass writes it, the UI pass samples it, and a capture (the editor's
            // viewport_screenshot) copies the finished image out: without CopySrc that copy is
            // outside the Vulkan spec (a lenient driver lets it through) and a validation error
            // on WebGPU. Found by the Beef side.
            colorDesc.usage = colorDesc.usage | rhi::TextureUsage::CopySrc;
            // Viewable in its sRGB or UNORM twin too, for a screen UI that blends encoded values
            // over the game (RenderOverlays' encodedTarget); a format with no twin has none.
            colorDesc.alternateViewFormat = rhi::SrgbTwin(m_colorFormat);
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
            if (colorDesc.alternateViewFormat != rhi::TextureFormat::Undefined)
            {
                rhi::TextureViewDesc alternateDesc{};
                alternateDesc.format = colorDesc.alternateViewFormat;
                if (!m_device->CreateTextureView(m_colorTexture, alternateDesc, m_colorAlternateView).IsOk())
                {
                    m_colorAlternateView = nullptr; // the screen UI then blends linear light
                }
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
            if (m_colorAlternateView != nullptr)
            {
                m_device->DestroyTextureView(m_colorAlternateView);
                m_colorAlternateView = nullptr;
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
        rhi::TextureView* m_colorAlternateView = nullptr; // in its sRGB or UNORM twin, if it has one
        rhi::Texture* m_depthTexture = nullptr;
        rhi::TextureView* m_depthView = nullptr;
        rhi::ResourceState m_colorState = rhi::ResourceState::Undefined;
        rhi::ResourceState m_depthState = rhi::ResourceState::Undefined;

        bool m_hostedTextInputWanted = false;
        u32 m_fixedWidth = 0;
        u32 m_fixedHeight = 0;
        u32 m_layoutWidth = 0; // the last laid-out size (targets may lag it: no device yet)
        u32 m_layoutHeight = 0;
        u32 m_textureWidth = 0;
        u32 m_textureHeight = 0;
        bool m_registered = false;
        FitMode m_fitMode = FitMode::Stretch;
        rhi::TextureFormat m_colorFormat = rhi::TextureFormat::RGBA16Float;
        rhi::TextureFormat m_depthFormat = rhi::TextureFormat::Depth32Float;

        // The content is reading the keyboard right now: it asked to, and the router gave its
        // surface the keyboard focus.
        [[nodiscard]] bool ContentHasKeys() const noexcept
        {
            return m_capturesKeys && m_surface && m_surface->Focused();
        }
        [[nodiscard]] static bool IsChord(KeyModifiers modifiers) noexcept
        {
            return HasFlag(modifiers, KeyModifiers::Ctrl) || HasFlag(modifiers, KeyModifiers::Alt) ||
                   HasFlag(modifiers, KeyModifiers::Gui);
        }

        UniquePtr<shell::InputSurface> m_surface;
        bool m_capturesKeys = false;
        u32 m_contentWidth = 0; // the content's own resolution inside the texture (0 = the texture's)
        u32 m_contentHeight = 0;
        FitMode m_contentFit = FitMode::Letterbox;
        ContentRotation m_contentRotation = ContentRotation::None; // the content presented turned
    };

    RTTI_DEFINE_OBJECT(ViewportView, "rtti::ui::viewport")
}
