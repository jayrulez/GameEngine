// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Hosting - :gui_host partition: GuiHost, the lab's GUI on the runtime's multi-window graphics
// host (foundation.graphics).
//
// One GUIContext and one style manager for the application, and a SceneRoot per RenderWindow with
// that window's VG context, VG renderer, pass targets (multisampling, and the stencil the fills
// use), input surface and input bridge (a root has a dispatcher of its own), stashed on the window
// as its payload. The picture baker serves every window: a picture baked once registers with each
// window's renderer. An application constructs one host, attaches a root to its main window (and
// to each window it opens), and calls Update from its update and RenderWindow from its render.
//
// Input: the pointer goes to the window under it, or to the window a press began in until every
// button is up (the lab has no capture to ask, so a held press is one); keys, text and the gamepad
// go to the focused window; a file dropped goes to the window it landed on. A root draws when it
// needs a frame (E8a's NeedsFrame: something changed, an animation runs, a bake landed); otherwise
// its window re-encodes the batch it drew last, since the runtime host presents every frame.
//
// Order of teardown: detach a window before its RenderWindow goes (its renderer leaves the baker's
// targets), and let the host go before the shader library whose modules it draws with.

module;
#include "Core/Prelude.h"

export module experimental.gui.hosting:gui_host;

import foundation.core;
import foundation.rhi;
import foundation.shell;
import foundation.graphics;
import foundation.fonts;
import foundation.vg;
import foundation.vg.renderer;
import experimental.gui;
import experimental.gui.shell; // GuiInputBridge, ShellClipboard
import :vg_shaders;
import :picture_baker;

using namespace foundation::core;
namespace core = foundation::core;
namespace rhi = foundation::rhi;
namespace shell = foundation::shell;
namespace graphics = foundation::graphics;
namespace vg = foundation::vg;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    /// One window's GUI, the payload GuiHost stashes on its RenderWindow (which owns it, and frees it
    /// after the GPU is done with it).
    class GuiWindowData final : public graphics::IRenderWindowData
    {
    public:
        explicit GuiWindowData(IAllocator& allocator) noexcept : Renderer(allocator) {}

        RefPtr<SceneRoot> Root;
        UniquePtr<vg::VGContext> Canvas;
        vg::renderer::VGRenderer Renderer;
        vg::renderer::VGPassTargets Targets; // none: a plain single-sampled pass
        UniquePtr<shell::InputSurface> Surface;
        UniquePtr<GuiInputBridge> Bridge;
        bool Encoded = false; ///< blends encoded values, through the backbuffer's UNORM view
        rhi::TextureFormat DrawFormat = rhi::TextureFormat::Undefined;
        bool NeedsFrame = true; ///< Update's verdict: the root draws this frame, else its last batch is encoded
    };

    class GuiHost
    {
    public:
        /// `fonts` is the font service the GUI's text and an SVG's text draw with; `shaders` the VG
        /// shaders (a VgShaderLibrary's), which outlive the host. All borrowed.
        GuiHost(IAllocator& allocator, graphics::GraphicsDevice& device, shell::IShell& shellRef, fonts::IFontService* fonts,
                const VgShaderSet& shaders)
            : m_allocator(allocator), m_device(device), m_shell(shellRef), m_fonts(fonts), m_shaders(shaders),
              m_context(MakeRef<GUIContext>(allocator)), m_attached(allocator)
        {
            m_context->SetFontService(fonts);
            m_styles.SetFontService(fonts);
            m_clipboard = MakeUnique<ShellClipboard>(allocator, &shellRef);
            m_router = MakeUnique<shell::InputRouter>(allocator, shellRef.Input());
            if (m_device.Raw() != nullptr && m_shaders.IsUsable())
            {
                m_baker = MakeUnique<GpuPictureBaker>(allocator, allocator, *m_device.Raw(), fonts);
                PictureBakerSetup setup;
                setup.VertexShader = m_shaders.Vertex;
                setup.FragmentShader = m_shaders.Fragment;
                setup.GradientRadialShader = m_shaders.GradientRadial;
                setup.GradientConicShader = m_shaders.GradientConic;
                setup.DistanceFieldShader = m_shaders.DistanceField;
                setup.FrameCount = static_cast<i32>(m_device.FramesInFlight());
                m_bakerSetup = setup; // initialized by the first window, which knows the format
            }
        }
        ~GuiHost()
        {
            for (const Attached& attached : m_attached)
                Forget(attached);
            m_attached.Clear();
            if (m_baker)
                m_baker->Dispose();
        }
        GuiHost(const GuiHost&) = delete;
        GuiHost& operator=(const GuiHost&) = delete;

        /// The GUI: its data, keymap, commands and resources. A root for a window is made with it.
        [[nodiscard]] GUIContext& Context() noexcept { return *m_context; }
        [[nodiscard]] RefPtr<GUIContext> ContextRef() const noexcept { return m_context; }
        /// The style manager every root is styled through: the theme, the application's sheet.
        [[nodiscard]] StyleManager& Styles() noexcept { return m_styles; }
        /// The baker every window's SVG skins draw from; null without a device or shaders.
        [[nodiscard]] GpuPictureBaker* Baker() noexcept { return m_baker.Get(); }

        /// The user's scale on every window's content scale (a preference, accessibility).
        void SetUiScale(f32 scale) noexcept { m_uiScale = core::Clamp(scale, 0.5f, 3.0f); }
        [[nodiscard]] f32 GetUiScale() const noexcept { return m_uiScale; }
        /// How a window attached after the call blends: Encoded (the default) as browsers and design
        /// tools do, half black over white a 50% grey, when its swapchain offers its UNORM view;
        /// Linear otherwise.
        void SetBlendSpace(vg::VGBlendSpace space) noexcept { m_blendSpace = space; }
        /// What shows behind a root that does not cover its window, as authored (sRGB).
        void SetClearColor(Color color) noexcept { m_clear = color; }

        /// Gives `window` the root `root` (made with Context()): its canvas, renderer, targets and
        /// input. Attaching again replaces the window's last root.
        void AttachWindow(graphics::RenderWindow& window, RefPtr<SceneRoot> root)
        {
            if (!root || m_device.Raw() == nullptr || !m_shaders.IsUsable())
                return;
            if (Find(&window) != nullptr)
                DetachWindow(window);
            UniquePtr<GuiWindowData> data = MakeUnique<GuiWindowData>(m_allocator, m_allocator);
            data->Root = root;
            data->Canvas = MakeUnique<vg::VGContext>(m_allocator, m_fonts);
            data->Encoded = m_blendSpace == vg::VGBlendSpace::Encoded && window.Swap() != nullptr &&
                            window.Swap()->AlternateViewFormat() != rhi::TextureFormat::Undefined;
            data->DrawFormat = window.Swap() == nullptr ? rhi::TextureFormat::RGBA8UnormSrgb
                               : data->Encoded         ? window.Swap()->AlternateViewFormat()
                                                       : window.Swap()->Format();
            const u32 width = window.Window().Width(), height = window.Window().Height();
            data->Targets.Configure(*m_device.Raw());
            const bool targets = data->Targets.Create(data->DrawFormat, width, height);
            vg::renderer::VGTargetConfig config = targets ? data->Targets.ConfigFor() : vg::renderer::VGTargetConfig{};
            config.blendSpace = data->Encoded ? vg::VGBlendSpace::Encoded : vg::VGBlendSpace::Linear;
            if (!data->Renderer
                     .Initialize(*m_device.Raw(), *m_shaders.Vertex, *m_shaders.Fragment, data->DrawFormat,
                                 static_cast<i32>(m_device.FramesInFlight()), m_shaders.DistanceField, m_shaders.GradientRadial,
                                 m_shaders.GradientConic, config, m_shaders.BoxShadow, m_shaders.BoxVertex, m_shaders.BoxFragment,
                                 m_shaders.Layer, m_shaders.BlurVertex, m_shaders.BlurFragment)
                     .IsOk())
                return;
            data->Canvas->SetPerPixelGradients(m_shaders.GradientRadial != nullptr && m_shaders.GradientConic != nullptr);
            data->Canvas->SetAnalyticBoxes(m_shaders.BoxVertex != nullptr && m_shaders.BoxFragment != nullptr);
            data->Canvas->SetLayers(m_shaders.Layer != nullptr);
            data->Canvas->SetStencilFills(data->Renderer.StencilFillsSupported());

            // Input: a surface over the whole window, through the router; the root's own bridge.
            const Float2 size{static_cast<f32>(width), static_cast<f32>(height)};
            data->Surface = MakeUnique<shell::InputSurface>(m_allocator, m_shell.Input(), window.Window().Id(),
                                                            ContentFit{Rectangle{0.0f, 0.0f, size.x, size.y}, size, FitMode::Stretch});
            m_router->AddSurface(data->Surface.Get());
            data->Bridge = MakeUnique<GuiInputBridge>(m_allocator, root->GetEventDispatcher());
            data->Bridge->SetTextInputTarget(&window.Window());
            if (m_shell.Input() != nullptr)
                data->Bridge->SetCursorTarget(m_shell.Input()->Mouse());
            root->GetEventDispatcher()->SetClipboard(m_clipboard.Get());
            root->SetDisplayScale(window.Window().ContentScale() * m_uiScale);
            root->SetViewportSize(size);

            // The bakes: the first window gives the baker its format, every window samples them.
            if (m_baker)
            {
                if (!m_bakerReady)
                {
                    m_bakerSetup.Format = data->DrawFormat;
                    m_bakerSetup.BlendSpace = config.blendSpace;
                    m_bakerReady = m_baker->Initialize(m_bakerSetup).IsOk();
                    if (m_bakerReady)
                        m_context->SetPictureBaker(m_baker.Get());
                }
                if (m_bakerReady)
                    m_baker->AddTarget(data->Renderer);
            }

            GuiWindowData* raw = data.Get();
            // A payload replaced while frames in flight use it: wait for them first.
            if (window.Data() != nullptr)
                m_device.Raw()->WaitIdle();
            window.SetData(Move(data));
            m_attached.PushBack(Attached{&window, raw});
        }

        /// Stops drawing and feeding the window's root; its renderer forgets the bakes. The payload
        /// stays on the RenderWindow, which frees it when it goes (after the GPU is idle).
        void DetachWindow(graphics::RenderWindow& window)
        {
            for (usize i = 0; i < m_attached.Size(); ++i)
                if (m_attached[i].Window == &window)
                {
                    Forget(m_attached[i]);
                    m_attached.RemoveAt(i);
                    if (m_pressWindow == window.Window().Id())
                        m_pressWindow = 0;
                    return;
                }
        }

        /// Once per frame: each window's input to its root, the GUI's clock, then every root styled
        /// and laid out at its window's size and scale, and asked whether it needs a frame.
        void Update(f32 deltaTime)
        {
            m_router->Update();
            RouteInput();
            m_context->Update(Duration::FromSeconds(static_cast<f64>(deltaTime)));
            for (const Attached& attached : m_attached)
            {
                GuiWindowData& data = *attached.Data;
                shell::IWindow& window = attached.Window->Window();
                const u32 width = window.Width(), height = window.Height();
                const f32 scale = window.ContentScale() * m_uiScale;
                bool resized = false;
                if (data.Root->GetDisplayScale() != scale)
                    data.Root->SetDisplayScale(scale);
                if (data.Root->GetViewportSize() != Float2{static_cast<f32>(width), static_cast<f32>(height)})
                {
                    data.Root->SetViewportSize(Float2{static_cast<f32>(width), static_cast<f32>(height)});
                    resized = true;
                }
                // The targets follow the window, outside any frame (in flight ones use them).
                if (data.Targets.NeedsResize(width, height))
                {
                    m_device.Raw()->WaitIdle();
                    (void)data.Targets.Resize(width, height);
                }
                m_styles.SetMediaContext(data.Root->GetMediaContext());
                m_styles.ApplyTree(*data.Root);
                data.Root->UpdateLayout();
                data.NeedsFrame = data.Root->NeedsFrame() || resized || data.NeedsFrame;
            }
            m_bakesRecorded = false;
        }

        /// Draws `frame`'s window: its root when it needs a frame (else the batch it drew last), the
        /// baker's queue (once a frame, before any pass samples it), the layers, then the pass.
        void RenderWindow(graphics::FrameContext& frame)
        {
            if (!frame.valid || frame.encoder == nullptr)
                return;
            GuiWindowData* data = Find(frame.window);
            if (data == nullptr)
                return;
            if (data->NeedsFrame)
            {
                data->Canvas->Clear();
                DrawContext context{*data->Canvas};
                data->Root->DrawFrame(context);
                data->NeedsFrame = false;
                ++m_framesDrawn;
            }
            else
                ++m_framesReused;
            if (m_baker && m_bakerReady && !m_bakesRecorded)
            {
                (void)m_baker->Record(*frame.encoder, static_cast<i32>(frame.frameIndex));
                m_bakesRecorded = true;
            }
            vg::VGBatch& batch = data->Canvas->GetBatch();
            const i32 slot = static_cast<i32>(frame.frameIndex);
            data->Renderer.BeginFrame(slot);
            const vg::renderer::VGRenderSlice slice = data->Renderer.Prepare(batch, slot, frame.width, frame.height);
            data->Renderer.RenderLayers(*frame.encoder, slot, slice);
            const rhi::ClearColor clear = ClearFor(*data);
            rhi::TextureView* view = data->Encoded ? frame.backbufferAlternateView : frame.backbufferView;
            if (data->Targets.Matches(frame.width, frame.height))
            {
                const rhi::RenderPassDesc pass = data->Targets.BeginPass(*frame.encoder, view, clear, u8"GUI");
                if (rhi::RenderPassEncoder* encoder = frame.encoder->BeginRenderPass(pass))
                {
                    data->Renderer.Render(*encoder, frame.width, frame.height, slot, slice);
                    encoder->End();
                }
                return;
            }
            if (rhi::RenderPassEncoder* encoder = frame.BeginBackbufferPass(clear, view))
                data->Renderer.Render(*encoder, frame.width, frame.height, slot, slice);
            frame.EndBackbufferPass();
        }

        /// The renderer drawing `window`'s GUI (where a viewport registers its target); null when it
        /// has none.
        [[nodiscard]] vg::renderer::VGRenderer* RendererFor(const graphics::RenderWindow* window) noexcept
        {
            GuiWindowData* data = Find(window);
            return data != nullptr ? &data->Renderer : nullptr;
        }
        /// The window `root` is attached to; null when it is in none.
        [[nodiscard]] graphics::RenderWindow* WindowFor(const SceneRoot* root) noexcept
        {
            for (const Attached& attached : m_attached)
                if (attached.Data->Root.Get() == root)
                    return attached.Window;
            return nullptr;
        }
        [[nodiscard]] usize WindowCount() const noexcept { return m_attached.Size(); }

        /// Frames a root was drawn in, and frames a window encoded its last batch again.
        [[nodiscard]] u64 FramesDrawn() const noexcept { return m_framesDrawn; }
        [[nodiscard]] u64 FramesReused() const noexcept { return m_framesReused; }

    private:
        struct Attached
        {
            graphics::RenderWindow* Window = nullptr;
            GuiWindowData* Data = nullptr; // the RenderWindow's
        };

        [[nodiscard]] GuiWindowData* Find(const graphics::RenderWindow* window) noexcept
        {
            for (const Attached& attached : m_attached)
                if (attached.Window == window)
                    return attached.Data;
            return nullptr;
        }
        [[nodiscard]] const Attached* FindById(u32 id) const noexcept
        {
            for (const Attached& attached : m_attached)
                if (id != 0 && attached.Window->Window().Id() == id)
                    return &attached;
            return nullptr;
        }

        void Forget(const Attached& attached)
        {
            m_router->RemoveSurface(attached.Data->Surface.Get());
            if (m_baker)
                m_baker->RemoveTarget(attached.Data->Renderer);
            attached.Data->Bridge->SetTextInputTarget(nullptr); // the window closes after this
        }

        void RouteInput()
        {
            shell::IInputManager* input = m_shell.Input();
            if (input == nullptr || m_attached.IsEmpty())
                return;
            // The pointer: the window a press began in while any button is held, else the one under it.
            bool held = false;
            if (shell::IMouse* mouse = input->Mouse())
            {
                held = mouse->IsButtonDown(shell::MouseButton::Left) || mouse->IsButtonDown(shell::MouseButton::Middle) ||
                       mouse->IsButtonDown(shell::MouseButton::Right);
                mouse->SetGlobalCapture(held); // its events keep coming past the window's edge
            }
            if (!held)
                m_pressWindow = 0;
            const Attached* pointer = FindById(m_pressWindow);
            if (pointer == nullptr)
                pointer = FindById(input->HoverWindow());
            if (pointer != nullptr)
            {
                const f32 width = static_cast<f32>(pointer->Window->Window().Width());
                const f32 height = static_cast<f32>(pointer->Window->Window().Height());
                pointer->Data->Surface->SetRegion(Rectangle{0.0f, 0.0f, width, height});
                pointer->Data->Surface->SetWindowSize(Float2{width, height});
                pointer->Data->Surface->SetContentSize(Float2{width, height});
                pointer->Data->Bridge->PumpFromSurface(*pointer->Data->Surface);
                if (held && m_pressWindow == 0)
                    m_pressWindow = pointer->Window->Window().Id();
            }
            // Keys, text and the gamepad: the focused window, else the first.
            const Attached* focused = FindById(input->FocusedWindow());
            if (focused == nullptr)
                focused = &m_attached[0];
            for (const shell::InputEvent& event : input->Events())
                switch (event.kind)
                {
                case shell::InputEventKind::KeyDown:
                case shell::InputEventKind::KeyUp:
                case shell::InputEventKind::TextInput:
                case shell::InputEventKind::GamepadButtonDown:
                case shell::InputEventKind::GamepadButtonUp:
                case shell::InputEventKind::GamepadAxis:
                    (void)focused->Data->Bridge->Dispatch(event);
                    break;
                default:
                    break;
                }
            // Files the OS dropped: the window they landed on.
            Array<shell::DroppedFile> drops(m_allocator);
            m_shell.DrainDroppedFiles(drops);
            for (const Attached& attached : m_attached)
                (void)attached.Data->Bridge->DispatchFileDrops(drops, attached.Window->Window().Id());
        }

        // The clear colour for a window: as authored when it blends encoded, else in linear light
        // (an sRGB target encodes what a pass clears to).
        [[nodiscard]] rhi::ClearColor ClearFor(const GuiWindowData& data) const noexcept
        {
            if (data.Encoded)
                return rhi::ClearColor(m_clear.r, m_clear.g, m_clear.b, m_clear.a);
            return rhi::ClearColor(SrgbToLinear(m_clear.r), SrgbToLinear(m_clear.g), SrgbToLinear(m_clear.b), m_clear.a);
        }

        IAllocator& m_allocator;
        graphics::GraphicsDevice& m_device;
        shell::IShell& m_shell;
        fonts::IFontService* m_fonts;
        VgShaderSet m_shaders; // borrowed modules
        RefPtr<GUIContext> m_context;
        StyleManager m_styles;
        UniquePtr<ShellClipboard> m_clipboard;
        UniquePtr<shell::InputRouter> m_router;
        UniquePtr<GpuPictureBaker> m_baker;
        PictureBakerSetup m_bakerSetup;
        bool m_bakerReady = false;
        bool m_bakesRecorded = false;
        Array<Attached> m_attached;
        u32 m_pressWindow = 0; // the window a held press began in
        f32 m_uiScale = 1.0f;
        vg::VGBlendSpace m_blendSpace = vg::VGBlendSpace::Encoded;
        Color m_clear{0.07f, 0.07f, 0.09f, 1.0f};
        u64 m_framesDrawn = 0;
        u64 m_framesReused = 0;
    };
}
