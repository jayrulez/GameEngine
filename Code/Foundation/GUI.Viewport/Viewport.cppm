// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - `foundation.gui.viewport`: Viewport, a widget that shows content drawn into a colour
// target (and depth) of its own: a 3D scene, a game in an editor's panel.
//
// The application fills OnRender, which records into the targets on the frame's encoder; the
// widget places the barriers around it. The widget is hosted content (GUI.Hosting's
// IHostedContent). Once handed to the host (HostIn), it binds to the renderer of the window its
// node is in, and binds again when the node moves to another window. Its targets follow the node's
// size in that window's device pixels, or use a fixed resolution. Its drawing is recorded on that
// window's frame before the GUI samples the colour target. Its input surface is placed over the
// node, so the content's keyboard and mouse (Mouse(), Keyboard(), Touch()) are gated to it: a
// camera reads them while the node is hovered or focused (IsActive), and a drag begun in it keeps
// them past its edge.
//
// Content can also be drawn outside OnRender, by a renderer with a frame of its own that runs before
// the GUI's (an editor's scene renderer, one bracket for every page). That renderer imports the
// colour target in ColorState(), leaves it ready to sample, and reports this with
// SetColorState(ShaderRead); the widget then keeps what it drew.
//
// The widget has a fit (core::ContentFit, used for the picture and the pointer alike), its formats
// (RGBA16Float and Depth32Float by default), an optional fixed resolution, a clear while there is
// nothing defined to show, and the colour target's state for a renderer that owns its transitions.
// A game in a panel takes every key while focused, through the node's SetCapturesAllKeys; its
// release chord (Shift+F1 by default) frees the keys. For a game in a panel (the editor's Game tab)
// the widget also provides: the colour target's sRGB or UNORM twin view, through which the game's
// screen UI blends encoded values; the resolution the content draws at inside the targets, which
// the pointer maps into; hosted text input, where the game's own text field asks for the window's
// text input through the focused node; and a clear once content drawn from outside stops.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui.viewport;

import foundation.core;
import foundation.rhi;
import foundation.image;
import foundation.shell;
import foundation.vg;
import foundation.vg.renderer;
import foundation.gui;
import foundation.gui.hosting; // IHostedContent, GuiHost

using namespace foundation::core;
namespace core = foundation::core;
namespace rhi = foundation::rhi;
namespace image = foundation::image;
namespace shell = foundation::shell;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    class Viewport : public UIWidget, public IHostedContent
    {
        RTTI_OBJECT(Viewport, UIWidget)
    public:
        /// Records the content into the targets, as a pass on ColorView() and DepthView() cleared to
        /// LinearClearColor(). The targets are in their render states while it runs.
        using RenderHandler = Function<void(Viewport& viewport, rhi::CommandEncoder& encoder, i32 frameIndex)>;
        RenderHandler OnRender;
        /// Called when the targets are made again, with their new size (a camera's aspect follows it).
        Function<void(u32 width, u32 height)> OnResized;
        /// The colour shown where the content draws nothing, as authored (sRGB).
        Color ClearColor{0.347f, 0.347f, 0.377f, 1.0f};

        Viewport()
        {
            SetTag(core::StringView(u8"viewport"));
            SetTabFocusable(true); // a click focuses it, and the content then has the keyboard
        }
        ~Viewport() override
        {
            if (m_host != nullptr)
                m_host->RemoveContent(*this);
            ReleaseTargets();
        }
        Viewport(const Viewport&) = delete;
        Viewport& operator=(const Viewport&) = delete;

        /// Hands the widget to `host`, which from now on binds, places and draws it each frame while
        /// it is in a window.
        void HostIn(GuiHost& host)
        {
            if (m_host == &host)
                return;
            if (m_host != nullptr)
                m_host->RemoveContent(*this);
            m_host = &host;
            host.AddContent(*this);
        }

        [[nodiscard]] rhi::ClearColor LinearClearColor() const noexcept
        {
            return rhi::ClearColor{SrgbToLinear(ClearColor.r), SrgbToLinear(ClearColor.g), SrgbToLinear(ClearColor.b), ClearColor.a};
        }
        /// Returns how the targets' picture fits the node, which also decides how the pointer maps
        /// back into it.
        [[nodiscard]] FitMode GetFitMode() const noexcept { return m_fit; }
        void SetFitMode(FitMode mode)
        {
            m_fit = mode;
            Invalidate();
        }
        /// Returns the targets' formats, which the content's pipelines are built for.
        [[nodiscard]] rhi::TextureFormat ColorFormat() const noexcept { return m_colorFormat; }
        [[nodiscard]] rhi::TextureFormat DepthFormat() const noexcept { return m_depthFormat; }
        void SetFormats(rhi::TextureFormat color, rhi::TextureFormat depth)
        {
            if (color == m_colorFormat && depth == m_depthFormat)
                return;
            m_colorFormat = color;
            m_depthFormat = depth;
            if (m_width > 0 && m_height > 0)
                Resize(m_width, m_height);
        }
        /// Sets a fixed resolution, whatever the node's size; 0 by 0 uses the node's device pixels.
        void SetFixedResolution(u32 width, u32 height)
        {
            m_fixedWidth = width;
            m_fixedHeight = height;
            if (width > 0 && height > 0)
                Resize(width, height);
        }

        /// Sets the resolution the content draws at inside the targets, fitted by `fit` and turned by
        /// `rotation`, for content that draws its own fitted picture (a game at its render
        /// resolution, letterboxed in a target of the node's size). The pointer then maps into that
        /// resolution. 0 by 0 uses the targets' own.
        void SetContentResolution(u32 width, u32 height, FitMode fit, ContentRotation rotation = ContentRotation::None) noexcept
        {
            m_contentWidth = width;
            m_contentHeight = height;
            m_contentFit = fit;
            m_contentRotation = rotation;
        }
        /// Sets whether the hosted content has a text field taking text (a game's UI). While it does,
        /// the window's text input follows it, since this node is the GUI's focused one. The
        /// content's owner sets this each frame.
        void SetHostedTextInputWanted(bool wanted) noexcept { m_hostedTextInput = wanted; }
        [[nodiscard]] bool WantsTextInput() const override { return m_hostedTextInput; }
        /// Reports that content drawn from outside has stopped (a game ended). The picture is cleared
        /// to ClearColor on the next frame rather than keeping its last one.
        void ClearPicture() noexcept { m_clearPending = true; }

        [[nodiscard]] bool IsReady() const noexcept { return m_colorView != nullptr && m_depthView != nullptr; }
        /// Returns the colour target viewed in its sRGB or UNORM twin format, or null when its format
        /// has none. Drawing through it writes encoded values into an sRGB target without the
        /// hardware encoding them again.
        [[nodiscard]] rhi::TextureView* ColorAlternateView() const noexcept { return m_colorAlternateView; }
        [[nodiscard]] rhi::TextureView* ColorView() const noexcept { return m_colorView; }
        [[nodiscard]] rhi::TextureView* DepthView() const noexcept { return m_depthView; }
        [[nodiscard]] rhi::Texture* ColorTexture() const noexcept { return m_colorTexture; }
        /// Returns the colour target's state. A renderer drawing it outside OnRender imports it from
        /// this state (Undefined once it is made again), and reports the state it left it in with
        /// SetColorState.
        [[nodiscard]] rhi::ResourceState ColorState() const noexcept { return m_colorState; }
        void SetColorState(rhi::ResourceState state) noexcept { m_colorState = state; }
        /// Returns how many frames it cleared because it had nothing to show.
        [[nodiscard]] u32 ClearCount() const noexcept { return m_clears; }
        [[nodiscard]] u32 RenderWidth() const noexcept { return m_width; }
        [[nodiscard]] u32 RenderHeight() const noexcept { return m_height; }
        /// Returns the window it is bound to, or 0 for none.
        [[nodiscard]] u32 WindowId() const noexcept { return m_windowId; }

        /// The content's input, gated to the node; read it while IsActive().
        [[nodiscard]] shell::IMouse* Mouse() const noexcept { return m_surface ? m_surface->Mouse() : nullptr; }
        [[nodiscard]] shell::IKeyboard* Keyboard() const noexcept { return m_surface ? m_surface->Keyboard() : nullptr; }
        [[nodiscard]] shell::ITouch* Touch() const noexcept { return m_surface ? m_surface->Touch() : nullptr; }
        /// Returns whether the content should take input now, which it should while the node is
        /// hovered or focused.
        [[nodiscard]] bool IsActive() const noexcept { return IsHovered() || IsFocused(); }

        // === IHostedContent ===
        Node* HostedNode() override { return this; }
        void Bind(rhi::Device& device, vg::renderer::VGRenderer* renderer, shell::IInputManager* input, u32 windowId) override
        {
            m_device = &device;
            if (renderer != m_renderer)
            {
                if (m_registered && m_renderer != nullptr)
                {
                    m_device->WaitIdle(); // wait, because frames in flight sample it through the old renderer's registration
                    m_renderer->UnregisterExternalTexture(m_key.Get());
                    m_registered = false;
                }
                m_renderer = renderer;
                if (m_renderer != nullptr && m_colorView != nullptr)
                {
                    m_renderer->RegisterExternalTexture(m_key.Get(), m_colorView);
                    m_registered = true;
                }
            }
            m_windowId = windowId;
            if (input != nullptr && !m_surface)
                m_surface = MakeUnique<shell::InputSurface>(MemoryAllocator(), input, windowId,
                                                            ContentFit{Rectangle{0.0f, 0.0f, 1.0f, 1.0f}, Float2{1.0f, 1.0f}, m_fit});
            else if (m_surface)
                m_surface->SetWindow(windowId);
            Invalidate();
        }
        void Place(Rectangle windowPixels, Rectangle windowUnits, Float2 windowSize) override
        {
            // The targets follow the node in device pixels, unless a fixed resolution is set.
            const u32 width = m_fixedWidth > 0 ? m_fixedWidth : static_cast<u32>(Max(1.0f, windowPixels.width + 0.5f));
            const u32 height = m_fixedHeight > 0 ? m_fixedHeight : static_cast<u32>(Max(1.0f, windowPixels.height + 0.5f));
            if (width != m_width || height != m_height || (!IsReady() && m_device != nullptr))
                Resize(width, height);
            if (!m_surface)
                return;
            m_surface->SetWindowSize(windowSize);
            PlaceSurface(windowUnits); // the pointer arrives in window units
        }
        void Record(rhi::CommandEncoder& encoder, i32 frameIndex) override
        {
            if (!IsReady())
                return;
            // With no OnRender, a target drawn from outside (or cleared before) is ready to sample,
            // unless that content stopped and asked for a clear.
            if (!OnRender && m_colorState == rhi::ResourceState::ShaderRead && !m_clearPending)
                return;
            m_clearPending = false;
            encoder.TransitionTexture(m_colorTexture, m_colorState, rhi::ResourceState::RenderTarget);
            if (!OnRender)
            {
                // With nothing to draw, clear so the colour target still holds something defined to
                // sample.
                ++m_clears;
                rhi::RenderPassDesc pass;
                rhi::ColorAttachment color;
                color.view = m_colorView;
                color.loadOp = rhi::LoadOp::Clear;
                color.storeOp = rhi::StoreOp::Store;
                color.clearValue = LinearClearColor();
                pass.colorAttachments.Add(color);
                if (rhi::RenderPassEncoder* passEncoder = encoder.BeginRenderPass(pass))
                    passEncoder->End();
            }
            else
            {
                if (m_depthState != rhi::ResourceState::DepthStencilWrite)
                {
                    encoder.TransitionTexture(m_depthTexture, m_depthState, rhi::ResourceState::DepthStencilWrite);
                    m_depthState = rhi::ResourceState::DepthStencilWrite;
                }
                OnRender(*this, encoder, frameIndex);
            }
            encoder.TransitionTexture(m_colorTexture, rhi::ResourceState::RenderTarget, rhi::ResourceState::ShaderRead);
            m_colorState = rhi::ResourceState::ShaderRead;
        }
        shell::InputSurface* HostedSurface() override { return m_surface.Get(); }

    protected:
        /// Asks for 256 by 256 when nothing sizes it.
        [[nodiscard]] Float2 MeasureContent(Float2 available) override
        {
            (void)available;
            return Float2{256.0f, 256.0f};
        }
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect box = GetContentBounds();
            if (!m_registered || m_width == 0 || m_height == 0)
            {
                ctx.VG().FillRect(box.ToRectangle(), Color{0.098f, 0.098f, 0.118f, 1.0f}); // not bound yet
                return;
            }
            // Live content changes without the GUI knowing, so a shown viewport asks to be drawn every
            // frame.
            Invalidate();
            const ContentFit fit{box.ToRectangle(), Float2{static_cast<f32>(m_width), static_cast<f32>(m_height)}, m_fit};
            const Rectangle at = fit.DstRect();
            if (at.width < box.width || at.height < box.height)
                ctx.VG().FillRect(box.ToRectangle(), Color{0.0f, 0.0f, 0.0f, 1.0f}); // a letterbox's bars
            ctx.VG().DrawImage(m_key.Get(), at, fit.SrcRect(), Color::White);
        }

    private:
        // Places the input surface over the node, mapping the pointer into the targets, or into the
        // content's own resolution through its fit. With a fixed resolution too (a phone's screen)
        // this takes two steps: the targets are fitted into the node, then the content into the
        // targets.
        void PlaceSurface(Rectangle region)
        {
            m_surface->SetRegion(region);
            m_surface->SetRotation(ContentRotation::None);
            const Float2 targets{static_cast<f32>(m_width), static_cast<f32>(m_height)};
            if (m_contentWidth == 0 || m_contentHeight == 0)
            {
                m_surface->SetContentSize(targets);
                m_surface->SetFitMode(m_fit);
                return;
            }
            const Float2 content{static_cast<f32>(m_contentWidth), static_cast<f32>(m_contentHeight)};
            m_surface->SetContentSize(content);
            m_surface->SetFitMode(m_contentFit);
            m_surface->SetRotation(m_contentRotation);
            if (m_fixedWidth == 0 || m_fixedHeight == 0 || targets.x <= 0.0f || targets.y <= 0.0f)
                return;
            const Rectangle drawn = ContentFit{region, targets, m_fit}.DstRect();
            if (m_contentFit == FitMode::Crop)
            {
                m_surface->SetRegion(drawn); // a crop has no bars and shows a slice, so the fit scales evenly
                return;
            }
            const Rectangle inside = ContentFit{Rectangle{0.0f, 0.0f, targets.x, targets.y}, content, m_contentFit, m_contentRotation}.DstRect();
            const f32 sx = drawn.width / targets.x;
            const f32 sy = drawn.height / targets.y;
            m_surface->SetRegion(Rectangle{drawn.x + inside.x * sx, drawn.y + inside.y * sy, inside.width * sx, inside.height * sy});
            m_surface->SetFitMode(FitMode::Stretch); // the region is the content's rectangle
        }

        void Resize(u32 width, u32 height)
        {
            if (m_device == nullptr)
            {
                m_width = width;
                m_height = height;
                return;
            }
            if (m_colorTexture != nullptr || m_depthTexture != nullptr)
                m_device->WaitIdle(); // wait, because frames in flight sample the old targets
            if (m_registered && m_renderer != nullptr)
            {
                m_renderer->UnregisterExternalTexture(m_key.Get());
                m_registered = false;
            }
            DestroyTargets();
            m_width = width;
            m_height = height;
            m_colorState = rhi::ResourceState::Undefined;
            m_depthState = rhi::ResourceState::Undefined;
            m_key = MakeUnique<image::ImageDataRef>(MemoryAllocator(), width, height); // the key the renderer knows it by
            rhi::TextureDesc colorDesc = rhi::TextureDesc::RenderTarget(m_colorFormat, width, height, 1, u8"ViewportColor");
            colorDesc.usage = colorDesc.usage | rhi::TextureUsage::CopySrc; // a capture copies it out
            colorDesc.alternateViewFormat = rhi::SrgbTwin(m_colorFormat);
            if (!m_device->CreateTexture(colorDesc, m_colorTexture).IsOk() ||
                !m_device->CreateTextureView(m_colorTexture, rhi::TextureViewDesc{}, m_colorView).IsOk())
            {
                DestroyTargets();
                return;
            }
            if (colorDesc.alternateViewFormat != rhi::TextureFormat::Undefined)
            {
                rhi::TextureViewDesc alternate{};
                alternate.format = colorDesc.alternateViewFormat;
                if (!m_device->CreateTextureView(m_colorTexture, alternate, m_colorAlternateView).IsOk())
                    m_colorAlternateView = nullptr; // without the twin view, content meant for it draws linear light instead
            }
            rhi::TextureDesc depthDesc = rhi::TextureDesc::DepthBuffer(m_depthFormat, width, height, 1, u8"ViewportDepth");
            if (!m_device->CreateTexture(depthDesc, m_depthTexture).IsOk() ||
                !m_device->CreateTextureView(m_depthTexture, rhi::TextureViewDesc{}, m_depthView).IsOk())
            {
                DestroyTargets();
                return;
            }
            if (m_renderer != nullptr)
            {
                m_renderer->RegisterExternalTexture(m_key.Get(), m_colorView);
                m_registered = true;
            }
            Invalidate();
            if (OnResized)
                OnResized(width, height);
        }
        void DestroyTargets()
        {
            if (m_device == nullptr)
                return;
            if (m_depthView != nullptr)
                m_device->DestroyTextureView(m_depthView);
            if (m_depthTexture != nullptr)
                m_device->DestroyTexture(m_depthTexture);
            if (m_colorAlternateView != nullptr)
                m_device->DestroyTextureView(m_colorAlternateView);
            if (m_colorView != nullptr)
                m_device->DestroyTextureView(m_colorView);
            if (m_colorTexture != nullptr)
                m_device->DestroyTexture(m_colorTexture);
            m_depthView = nullptr;
            m_depthTexture = nullptr;
            m_colorView = nullptr;
            m_colorAlternateView = nullptr;
            m_colorTexture = nullptr;
        }
        void ReleaseTargets()
        {
            if (m_device != nullptr && (m_colorTexture != nullptr || m_depthTexture != nullptr))
                m_device->WaitIdle();
            if (m_registered && m_renderer != nullptr)
                m_renderer->UnregisterExternalTexture(m_key.Get());
            m_registered = false;
            DestroyTargets();
        }

        GuiHost* m_host = nullptr;
        rhi::Device* m_device = nullptr;
        vg::renderer::VGRenderer* m_renderer = nullptr; // the renderer of its window
        UniquePtr<image::ImageDataRef> m_key;
        rhi::Texture* m_colorTexture = nullptr;
        rhi::TextureView* m_colorView = nullptr;
        rhi::TextureView* m_colorAlternateView = nullptr; // the colour target in its sRGB or UNORM twin, if it has one
        rhi::Texture* m_depthTexture = nullptr;
        rhi::TextureView* m_depthView = nullptr;
        rhi::ResourceState m_colorState = rhi::ResourceState::Undefined;
        rhi::ResourceState m_depthState = rhi::ResourceState::Undefined;
        rhi::TextureFormat m_colorFormat = rhi::TextureFormat::RGBA16Float;
        rhi::TextureFormat m_depthFormat = rhi::TextureFormat::Depth32Float;
        UniquePtr<shell::InputSurface> m_surface;
        FitMode m_fit = FitMode::Stretch;
        u32 m_width = 0;
        u32 m_height = 0;
        u32 m_fixedWidth = 0;
        u32 m_fixedHeight = 0;
        u32 m_windowId = 0;
        u32 m_clears = 0;
        u32 m_contentWidth = 0; // the content's own resolution inside the targets (0 uses theirs)
        u32 m_contentHeight = 0;
        FitMode m_contentFit = FitMode::Stretch;
        ContentRotation m_contentRotation = ContentRotation::None;
        bool m_registered = false;
        bool m_hostedTextInput = false;
        bool m_clearPending = false;
    };

    RTTI_DEFINE_OBJECT(Viewport, "rtti::gui")
}
