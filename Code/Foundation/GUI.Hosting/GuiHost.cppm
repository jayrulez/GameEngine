// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Hosting - :gui_host partition: GuiHost, the GUI on the runtime's multi-window graphics
// host (foundation.graphics).
//
// The application gets one GUIContext and one style manager, and each RenderWindow gets a SceneRoot
// with that window's VG context, VG renderer and pass targets (multisampling, and the stencil the
// fills use), stashed on the window as its payload. A window may hold more roots, called panes,
// each over a region of it with its own input surface and bridge (a root has its own dispatcher)
// and, when it belongs to another GUI (a tool's, such as the inspector), that GUI's style manager.
// The picture baker serves every window: a picture baked once registers with each window's
// renderer. An application constructs one host, attaches a root to its main window (and to each
// window it opens), and calls Update from its update and RenderWindow from its render.
//
// Input: the pointer goes to the window under it, or to the window a press began in until every
// button is up (the GUI has no capture to ask for, so a held press acts as one). Each of the
// window's panes is pumped through its own surface, and the router gates them by region. Keys,
// text and the gamepad go to the pane clicked last in the focused window, else to its first pane.
// A dropped file goes to the window it landed on. An application may take a pane's pointer for
// itself first (a tool picking what is under it) and draw over a window after its panes (a tool's
// highlights). A root draws when it needs a frame (NeedsFrame: something changed, an animation
// runs, a bake finished); otherwise its window re-encodes the batch it drew last, since the runtime
// host presents every frame.
//
// Windows of its own: given an opener from the application, the host opens and closes an OS window
// for a root, and tells the owner what happens to each window, identified by its root (moved,
// resized, focus, a close requested). Where the window system lets it place windows (not Wayland),
// it also says which root is under a screen point and where a point in a root is on the screen.
//
// Order of teardown: detach a window before its RenderWindow goes (its renderer leaves the baker's
// targets), and let the host go before the shader library whose modules it draws with.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module foundation.gui.hosting:gui_host;

import foundation.core;
import foundation.rhi;
import foundation.shell;
import foundation.graphics;
import foundation.fonts;
import foundation.vg;
import foundation.vg.renderer;
import foundation.gui;
import foundation.gui.shell; // GuiInputBridge, ShellClipboard
import foundation.input;       // InputKindTracker
import :vg_shaders;
import :picture_baker;
import :hosted_content;

using namespace foundation::core;
namespace core = foundation::core;
namespace rhi = foundation::rhi;
namespace shell = foundation::shell;
namespace graphics = foundation::graphics;
namespace vg = foundation::vg;
namespace fonts = foundation::fonts;

export namespace foundation::gui
{
    /// One window's GUI, the payload GuiHost stashes on its RenderWindow. The RenderWindow owns it and
    /// frees it after the GPU is done with it.
    class GuiWindowData final : public graphics::IRenderWindowData
    {
    public:
        /// A root over a region of the window (empty: all of it), with its own input.
        struct Pane
        {
            RefPtr<SceneRoot> Root;
            Rectangle Region{};
            StyleManager* Styles = nullptr; ///< another GUI's styles; null uses the host's
            UniquePtr<shell::InputSurface> Surface;
            UniquePtr<GuiInputBridge> Bridge;
        };

        explicit GuiWindowData(IAllocator& allocator, IAllocator& panes) noexcept : Panes(panes), Renderer(allocator), Retired(allocator) {}

        Array<UniquePtr<Pane>> Panes; ///< the first is the window's root
        UniquePtr<vg::VGContext> Canvas;
        vg::renderer::VGRenderer Renderer;
        UniquePtr<vg::renderer::VGPassTargets> Targets; ///< null: a plain single-sampled pass
        bool Multisampled = false; ///< the renderer was made for the multisampled pass (its targets exist) and never draws the plain one
        /// Targets a resize replaced, kept until the frames in flight that drew into them are done.
        struct RetiredTargets
        {
            UniquePtr<vg::renderer::VGPassTargets> Targets;
            u32 FramesLeft = 0;
        };
        Array<RetiredTargets> Retired;
        bool Encoded = false; ///< blends encoded values, through the backbuffer's UNORM view
        rhi::TextureFormat DrawFormat = rhi::TextureFormat::Undefined;
        bool NeedsFrame = true; ///< set by Update: the panes draw this frame, otherwise the last batch is encoded again
        u32 LoggedWidth = 0, LoggedHeight = 0; ///< the frame size its sizes were last logged at

        [[nodiscard]] SceneRoot* Root() const noexcept { return Panes.IsEmpty() ? nullptr : Panes[0]->Root.Get(); }
    };

    class GuiHost
    {
    public:
        /// Takes a pane's pointer for the application before the pane sees it (a tool picking what
        /// is under it, a boundary being dragged). Returns true when it did; the pane is then not
        /// pumped.
        using PointerFilter = Function<bool(SceneRoot& pane, shell::InputSurface& surface)>;
        /// Draws over a window after its panes, in the window's pixels.
        using Overlay = Function<void(graphics::RenderWindow& window, DrawContext& context)>;
        /// Opens and closes the OS windows the host makes for a root. The application supplies it
        /// (its IApplicationHost's OpenWindow and CloseWindow) and owns the windows.
        struct WindowOpener
        {
            Function<graphics::RenderWindow*(const shell::WindowSettings&)> Open;
            Function<void(graphics::RenderWindow&)> Close;
        };
        /// Told what happens to a window the host serves, identified by the window's root: moved,
        /// resized, focus gained or lost, a close requested. The host closes nothing itself; the
        /// window's owner decides.
        using WindowEventListener = Function<void(SceneRoot& root, const shell::WindowEvent& event)>;

        /// `fonts` is the font service the GUI's text and an SVG's text draw with; `shaders` the VG
        /// shaders (a VgShaderLibrary's), which outlive the host. All borrowed.
        GuiHost(IAllocator& allocator, graphics::GraphicsDevice& device, shell::IShell& shellRef, fonts::IFontService* fonts,
                const VgShaderSet& shaders)
            : m_allocator(allocator), m_device(device), m_shell(shellRef), m_fonts(fonts), m_shaders(shaders),
              m_context(MakeRef<GUIContext>(allocator)), m_attached(allocator), m_contents(allocator)
        {
            m_context->SetFontService(fonts);
            m_context->SetRootPlaces(&m_places); // a drag can continue from window to window
            m_styles.SetFontService(fonts);
            m_clipboard = MakeUnique<ShellClipboard>(allocator, &shellRef);
            m_router = MakeUnique<shell::InputRouter>(allocator, shellRef.Input());
            m_contentRouter = MakeUnique<shell::InputRouter>(allocator, shellRef.Input());
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
            m_context->SetRootPlaces(nullptr);
            for (Hosted& hosted : m_contents)
                Unbind(hosted);
            m_contents.Clear();
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

        /// Sets the user's scale, applied on top of every window's content scale (a preference, or
        /// for accessibility).
        void SetUiScale(f32 scale) noexcept { m_uiScale = core::Clamp(scale, 0.5f, 3.0f); }
        [[nodiscard]] f32 GetUiScale() const noexcept { return m_uiScale; }
        /// Sets how a window attached after this call blends. Encoded (the default) blends as
        /// browsers and design tools do, so half black over white gives a 50% grey; it applies when
        /// the swapchain offers a UNORM view. Otherwise the window blends Linear.
        void SetBlendSpace(vg::VGBlendSpace space) noexcept { m_blendSpace = space; }
        /// Sets what shows behind a root that does not cover its window, as authored (sRGB).
        void SetClearColor(Color color) noexcept { m_clear = color; }
        void SetPointerFilter(PointerFilter filter) { m_pointerFilter = Move(filter); }
        /// The input kind the host tracks from the shell's devices each frame and gives every pane:
        /// the pointer, the keyboard or a pad. An application reads it, or sets a starting kind.
        [[nodiscard]] foundation::input::InputKindTracker& GetInputKindTracker() noexcept { return m_inputKind; }
        /// Makes every pane's cursor follow its GUI again, after the application set one of its own
        /// (a boundary's resize cursor).
        void ResetCursors()
        {
            shell::IMouse* mouse = m_shell.Input() != nullptr ? m_shell.Input()->Mouse() : nullptr;
            for (const Attached& attached : m_attached)
                for (const UniquePtr<GuiWindowData::Pane>& pane : attached.Data->Panes)
                    pane->Bridge->SetCursorTarget(mouse);
        }
        void SetOverlay(Overlay overlay) { m_overlay = Move(overlay); }
        /// Adds content that its node shows but that draws outside the GUI (a viewport's scene).
        /// While it is added, the host binds, places and records it each frame. Remove it before it
        /// is destroyed.
        void AddContent(IHostedContent& content)
        {
            for (const Hosted& hosted : m_contents)
                if (hosted.Content == &content)
                    return;
            m_contents.PushBack(Hosted{&content, nullptr});
        }
        /// Whether `content`'s input surface is routed now (bound, and its node shown).
        [[nodiscard]] bool Routes(const IHostedContent& content) const
        {
            for (const Hosted& hosted : m_contents)
                if (hosted.Content == &content)
                    return hosted.Routed;
            return false;
        }
        void RemoveContent(IHostedContent& content)
        {
            for (usize i = 0; i < m_contents.Size(); ++i)
                if (m_contents[i].Content == &content)
                {
                    Unbind(m_contents[i]);
                    m_contents.RemoveAt(i);
                    return;
                }
        }
        /// Sets the observer told of each file the OS drops, before the window it landed on is told.
        void SetDropObserver(Function<void(const shell::DroppedFile&)> observer) { m_dropObserver = Move(observer); }

        /// Gives `window` the root `root` (made with Context()) over all of it: its canvas, renderer,
        /// targets and input. Attaching again replaces the window's panes.
        void AttachWindow(graphics::RenderWindow& window, RefPtr<SceneRoot> root)
        {
            if (!root || m_device.Raw() == nullptr || !m_shaders.IsUsable())
                return;
            if (Find(&window) != nullptr)
                DetachWindow(window);
            UniquePtr<GuiWindowData> data = MakeUnique<GuiWindowData>(m_allocator, m_allocator, m_allocator);
            data->Canvas = MakeUnique<vg::VGContext>(m_allocator, m_fonts);
            data->Encoded = m_blendSpace == vg::VGBlendSpace::Encoded && window.Swap() != nullptr &&
                            window.Swap()->AlternateViewFormat() != rhi::TextureFormat::Undefined;
            data->DrawFormat = window.Swap() == nullptr ? rhi::TextureFormat::RGBA8UnormSrgb
                               : data->Encoded         ? window.Swap()->AlternateViewFormat()
                                                       : window.Swap()->Format();
            const u32 width = window.Window().PixelWidth(), height = window.Window().PixelHeight();
            data->Targets = MakeTargets(data->DrawFormat, width, height);
            const bool targets = data->Targets.Get() != nullptr;
            data->Multisampled = targets;
            vg::renderer::VGTargetConfig config = targets ? data->Targets->ConfigFor() : vg::renderer::VGTargetConfig{};
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

            // The first window gives the baker its format, and every window samples the bakes.
            if (EnsureBaker(data->DrawFormat, config.blendSpace))
                m_baker->AddTarget(data->Renderer);

            GuiWindowData* raw = data.Get();
            // The payload is replaced while frames in flight may use it, so wait for them first.
            if (window.Data() != nullptr)
                m_device.Raw()->WaitIdle();
            window.SetData(Move(data));
            m_attached.PushBack(Attached{&window, raw});
            AddPane(window, Move(root), Rectangle{});
        }

        /// Adds another root to `window` (which must be attached first) over `region` of it in its
        /// pixels (empty: all of it), with its own input. `styles` styles it when it belongs to
        /// another GUI (a tool's); otherwise the host's styles do. It is drawn after the window's
        /// root and the panes added before it.
        void AddPane(graphics::RenderWindow& window, RefPtr<SceneRoot> root, Rectangle region, StyleManager* styles = nullptr)
        {
            GuiWindowData* data = Find(&window);
            if (data == nullptr || !root)
                return;
            UniquePtr<GuiWindowData::Pane> pane = MakeUnique<GuiWindowData::Pane>(m_allocator);
            pane->Root = root;
            pane->Region = region;
            pane->Styles = styles;
            const Rectangle area = AreaOf(window, region);
            pane->Surface = MakeUnique<shell::InputSurface>(m_allocator, m_shell.Input(), window.Window().Id(),
                                                            ContentFit{InWindowUnits(window, area), Float2{area.width, area.height}, FitMode::Stretch});
            m_router->AddSurface(pane->Surface.Get());
            pane->Bridge = MakeUnique<GuiInputBridge>(m_allocator, root->GetEventDispatcher());
            pane->Bridge->SetTextInputTarget(&window.Window());
            if (m_shell.Input() != nullptr)
                pane->Bridge->SetCursorTarget(m_shell.Input()->Mouse());
            pane->Bridge->SetInputKindTracker(&m_inputKind); // the pointer, the keys or a pad
            root->GetEventDispatcher()->SetClipboard(m_clipboard.Get());
            root->SetDisplayScale(window.Window().ContentScale() * m_uiScale);
            root->SetViewportSize(Float2{area.width, area.height});
            data->Panes.PushBack(Move(pane));
            data->NeedsFrame = true;
        }
        /// Moves a pane (the window's root too) to `region` of its window (empty: all of it).
        void SetPaneRegion(const SceneRoot& root, Rectangle region)
        {
            GuiWindowData::Pane* pane = FindPane(root);
            if (pane == nullptr || (pane->Region.x == region.x && pane->Region.y == region.y && pane->Region.width == region.width &&
                                    pane->Region.height == region.height))
                return; // the same again changes nothing
            pane->Region = region;
            if (GuiWindowData* data = FindData(root))
                data->NeedsFrame = true;
        }
        /// Takes a pane added with AddPane out of its window.
        void RemovePane(const SceneRoot& root)
        {
            for (const Attached& attached : m_attached)
                for (usize i = 1; i < attached.Data->Panes.Size(); ++i)
                    if (attached.Data->Panes[i]->Root.Get() == &root)
                    {
                        Forget(*attached.Data->Panes[i]);
                        attached.Data->Panes.RemoveAt(i);
                        attached.Data->NeedsFrame = true;
                        return;
                    }
        }

        // === Windows of its own ===
        /// Sets how the host opens a window for a root; without an opener it opens none.
        void SetWindowOpener(WindowOpener opener) { m_opener = Move(opener); }
        /// Whether the host can open windows of its own: it has an opener, and the shell opens
        /// windows beside the main one (not the web's single canvas).
        [[nodiscard]] bool CanOpenWindows() const noexcept { return m_opener.Open && m_opener.Close && Capabilities().multipleWindows; }
        /// Opens an OS window as `settings` say and gives it `root` (made with Context()). Returns
        /// the window, or null when none opened.
        graphics::RenderWindow* OpenWindow(RefPtr<SceneRoot> root, const shell::WindowSettings& settings)
        {
            if (!CanOpenWindows() || !root)
                return nullptr;
            graphics::RenderWindow* window = m_opener.Open(settings);
            if (window == nullptr)
                return nullptr;
            AttachWindow(*window, Move(root));
            if (Find(window) == nullptr)
            {
                m_opener.Close(*window); // attaching failed: close the window rather than leave it without its GUI
                return nullptr;
            }
            return window;
        }
        /// Closes the window OpenWindow opened for `root`: it is detached, then closed by the opener.
        void CloseWindow(const SceneRoot& root)
        {
            graphics::RenderWindow* window = WindowFor(&root);
            if (window == nullptr || !m_opener.Close)
                return;
            DetachWindow(*window);
            m_opener.Close(*window);
        }
        /// Adds a listener to the windows' events (the application's, docking's). Listeners are told
        /// in the order added. Returns a token for RemoveWindowEventListener.
        u32 AddWindowEventListener(WindowEventListener listener)
        {
            m_windowListeners.PushBack(WindowListener{m_nextWindowListener, Move(listener)});
            return m_nextWindowListener++;
        }
        void RemoveWindowEventListener(u32 token)
        {
            for (usize i = 0; i < m_windowListeners.Size(); ++i)
                if (m_windowListeners[i].Token == token)
                {
                    m_windowListeners.RemoveAt(i);
                    return;
                }
        }
        /// Whether windows stay at the places the host gives them and the pointer is read across
        /// them, as the shell says. Neither holds on Wayland, where a client can do neither, or on
        /// the web's single canvas.
        [[nodiscard]] bool PlacesWindows() const noexcept { return Capabilities().placesWindows; }
        /// The screens there are, as the shell last read them.
        [[nodiscard]] Span<const shell::Screen> Screens() const noexcept
        {
            const shell::IWindowManager* windows = m_shell.WindowManager();
            return windows != nullptr ? windows->Screens() : Span<const shell::Screen>{};
        }
        /// Returns the root under `screen` (in screen units); where windows overlap, it is the top
        /// pane in the window focused last. `local` receives the point in that root, in the root's
        /// pointer units. Returns null over no root, and always where the host cannot place windows,
        /// since every window's place is then unknown.
        SceneRoot* RootAt(Float2 screen, Float2* local = nullptr)
        {
            if (!PlacesWindows())
                return nullptr;
            const auto under = [&](const Attached& attached) -> SceneRoot*
            {
                const shell::IWindow& window = attached.Window->Window();
                // The screen is in window units; the panes are in the window's pixels.
                const Float2 ratio = PixelsPerUnit(window);
                const Float2 at{(screen.x - static_cast<f32>(window.X())) * ratio.x, (screen.y - static_cast<f32>(window.Y())) * ratio.y};
                if (at.x < 0.0f || at.y < 0.0f || at.x >= static_cast<f32>(window.PixelWidth()) || at.y >= static_cast<f32>(window.PixelHeight()) ||
                    window.IsMinimized())
                    return nullptr;
                for (usize i = attached.Data->Panes.Size(); i-- > 0;) // the pane drawn last is on top
                {
                    const GuiWindowData::Pane& pane = *attached.Data->Panes[i];
                    const Rectangle area = AreaOf(*attached.Window, pane.Region);
                    if (at.x >= area.x && at.y >= area.y && at.x < area.x + area.width && at.y < area.y + area.height)
                    {
                        if (local != nullptr)
                            *local = Float2{at.x - area.x, at.y - area.y};
                        return pane.Root.Get();
                    }
                }
                return nullptr;
            };
            for (const u32 id : m_focusOrder) // the window focused last first
                if (const Attached* attached = FindById(id))
                    if (SceneRoot* root = under(*attached))
                        return root;
            for (const Attached& attached : m_attached)
                if (SceneRoot* root = under(attached))
                    return root;
            return nullptr;
        }
        /// Converts `local` (a point in `root`, in its pointer units) to screen coordinates. Returns
        /// false when the root is in no window or the host cannot place windows. The GUI's drags ask
        /// this through IRootPlaces.
        bool ToScreen(const SceneRoot& root, Float2 local, Float2& screen)
        {
            if (!PlacesWindows())
                return false;
            for (const Attached& attached : m_attached)
                for (const UniquePtr<GuiWindowData::Pane>& pane : attached.Data->Panes)
                    if (pane->Root.Get() == &root)
                    {
                        const shell::IWindow& window = attached.Window->Window();
                        const Rectangle area = AreaOf(*attached.Window, pane->Region);
                        const Float2 ratio = PixelsPerUnit(window); // the pane's pixels back to window units
                        screen = Float2{static_cast<f32>(window.X()) + (area.x + local.x) / ratio.x,
                                        static_cast<f32>(window.Y()) + (area.y + local.y) / ratio.y};
                        return true;
                    }
            return false;
        }

        /// Stops drawing and feeding the window's panes and releases their roots; its renderer
        /// forgets the bakes. The rest of the payload stays on the RenderWindow, which frees it when
        /// it is destroyed (after the GPU is idle).
        void DetachWindow(graphics::RenderWindow& window)
        {
            for (usize i = 0; i < m_attached.Size(); ++i)
                if (m_attached[i].Window == &window)
                {
                    Forget(m_attached[i]);
                    m_attached.RemoveAt(i);
                    if (m_pressWindow == window.Window().Id())
                        m_pressWindow = 0;
                    for (usize f = 0; f < m_focusOrder.Size(); ++f)
                        if (m_focusOrder[f] == window.Window().Id())
                        {
                            m_focusOrder.RemoveAt(f);
                            break;
                        }
                    return;
                }
        }

        /// Gives each pane's root its window's size and scale now (Update does this too). An
        /// application that sizes its own nodes from a root's size calls this first, so they follow
        /// a resized window in the same frame rather than the next.
        void SizePanes()
        {
            for (const Attached& attached : m_attached)
                if (SizePanes(*attached.Data, *attached.Window, attached.Window->Window().ContentScale() * m_uiScale))
                    attached.Data->NeedsFrame = true;
        }

        /// Runs once per frame: passes each window's input to its panes, advances the GUI's clock,
        /// then styles and lays out every pane at its region's size and its window's scale, and asks
        /// whether it needs a frame. A pane of another GUI runs on that GUI's clock, which its owner
        /// advances.
        void Update(f32 deltaTime)
        {
            // What the player uses now, from this frame's devices, before the panes' bridges pass
            // it on with their input.
            foundation::input::ShellInputSource devices(m_shell.Input());
            m_inputKind.Update(devices);
            m_router->Update();
            ReportWindowEvents();
            RouteInput();
            m_context->Update(Duration::FromSeconds(static_cast<f64>(deltaTime)));
            for (const Attached& attached : m_attached)
            {
                GuiWindowData& data = *attached.Data;
                graphics::RenderWindow& window = *attached.Window;
                const f32 scale = window.Window().ContentScale() * m_uiScale;
                bool needs = SizePanes(data, window, scale) || data.NeedsFrame;
                for (const UniquePtr<GuiWindowData::Pane>& pane : data.Panes)
                {
                    StyleAndLayOut(*pane->Root, pane->Styles);
                    needs = pane->Root->NeedsFrame() || needs;
                }
                data.NeedsFrame = needs;
            }
            PlaceContents();
            // A content that changes as it is placed (a viewport made again at a new size, releasing
            // its old image) invalidates its node. Its window then draws, rather than encoding a batch
            // that still names what was released. This reads the root's own flag rather than
            // NeedsFrame, which closes the frame's stats.
            for (const Attached& attached : m_attached)
                for (const UniquePtr<GuiWindowData::Pane>& pane : attached.Data->Panes)
                    if (pane->Root->NeedsRedraw())
                        attached.Data->NeedsFrame = true;
            m_bakesRecorded = false;
        }

        /// Draws `frame`'s window: its panes when any needs a frame (else the batch drawn last) and
        /// the overlay, then the baker's queue (once a frame, before any pass samples it), the
        /// layers, and the pass.
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
                for (const UniquePtr<GuiWindowData::Pane>& pane : data->Panes)
                {
                    const Rectangle area = AreaOf(*frame.window, pane->Region);
                    data->Canvas->PushState();
                    data->Canvas->Translate(area.x, area.y);
                    DrawContext paneContext{*data->Canvas};
                    pane->Root->DrawFrame(paneContext);
                    data->Canvas->PopState();
                }
                if (m_overlay)
                    m_overlay(*frame.window, context);
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
            // Record what the window's nodes show from outside the GUI, before the GUI samples it.
            for (const Hosted& hosted : m_contents)
                if (hosted.Window == frame.window && hosted.Content->HostedNode()->IsTreeVisible())
                    hosted.Content->Record(*frame.encoder, static_cast<i32>(frame.frameIndex));
            // Log every size a window's drawing depends on, once per frame size: on a scaled display
            // the window's units, its pixels, the swapchain and the root's viewport must agree for the
            // drawing to fill the window.
            if (frame.width != data->LoggedWidth || frame.height != data->LoggedHeight)
            {
                data->LoggedWidth = frame.width;
                data->LoggedHeight = frame.height;
                const shell::IWindow& os = frame.window->Window();
                const SceneRoot* root = data->Root();
                LOG_INFO(u8"GUI", u8"window {}: {}x{} units, {}x{} pixels, content scale {}; frame {}x{}; swapchain {}x{}; root viewport {}x{}, size {}x{}, display scale {}",
                         os.Id(), os.Width(), os.Height(), os.PixelWidth(), os.PixelHeight(), os.ContentScale(), frame.width, frame.height,
                         frame.window->Swap() != nullptr ? frame.window->Swap()->Width() : 0u, frame.window->Swap() != nullptr ? frame.window->Swap()->Height() : 0u,
                         root != nullptr ? root->GetViewportSize().x : 0.0f, root != nullptr ? root->GetViewportSize().y : 0.0f,
                         root != nullptr ? root->GetSize().x : 0.0f, root != nullptr ? root->GetSize().y : 0.0f,
                         root != nullptr ? root->GetDisplayScale() : 0.0f);
            }
            vg::VGBatch& batch = data->Canvas->GetBatch();
            const i32 slot = static_cast<i32>(frame.frameIndex);
            data->Renderer.BeginFrame(slot);
            const vg::renderer::VGRenderSlice slice = data->Renderer.Prepare(batch, slot, frame.width, frame.height);
            data->Renderer.RenderLayers(*frame.encoder, slot, slice);
            const rhi::ClearColor clear = ClearFor(data->Encoded);
            rhi::TextureView* view = data->Encoded ? frame.backbufferAlternateView : frame.backbufferView;
            // The targets follow the backbuffer this frame draws into, not the window: while a window
            // is resized its swapchain need not match it, and a pass resolves only into its own size.
            // New targets are made at once and the old ones retired, then freed once the frames in
            // flight that drew into them are done. This avoids waiting for the GPU, which on the web
            // would yield to the browser while the canvas's texture is taken.
            RetireTargets(*data);
            if (data->Multisampled && (data->Targets.Get() == nullptr || !data->Targets->Matches(frame.width, frame.height)))
            {
                if (data->Targets.Get() != nullptr)
                    data->Retired.PushBack(GuiWindowData::RetiredTargets{Move(data->Targets), m_device.FramesInFlight()});
                data->Targets = MakeTargets(data->DrawFormat, frame.width, frame.height);
            }
            if (data->Targets.Get() != nullptr && data->Targets->Matches(frame.width, frame.height))
            {
                const rhi::RenderPassDesc pass = data->Targets->BeginPass(*frame.encoder, view, clear, u8"GUI");
                if (rhi::RenderPassEncoder* encoder = frame.encoder->BeginRenderPass(pass))
                {
                    data->Renderer.Render(*encoder, frame.width, frame.height, slot, slice);
                    encoder->End();
                }
                return;
            }
            // A renderer made for the multisampled pass cannot draw the plain one (its pipelines'
            // sample count is the targets'). With no targets at this size the frame is only cleared,
            // and the next frame draws again.
            if (rhi::RenderPassEncoder* encoder = frame.BeginBackbufferPass(clear, view))
            {
                if (!data->Multisampled)
                    data->Renderer.Render(*encoder, frame.width, frame.height, slot, slice);
                else
                    data->NeedsFrame = true;
            }
            frame.EndBackbufferPass();
        }

        /// Returns the multisampled targets `window`'s GUI draws into, sized to its backbuffer as
        /// each frame draws, or null when it has none (a plain single-sampled pass).
        [[nodiscard]] const vg::renderer::VGPassTargets* PassTargetsFor(const graphics::RenderWindow* window) noexcept
        {
            GuiWindowData* data = Find(window);
            return data != nullptr && data->Multisampled ? data->Targets.Get() : nullptr;
        }
        /// Returns how many replaced targets `window` still keeps for the frames in flight.
        [[nodiscard]] usize RetiredTargetsFor(const graphics::RenderWindow* window) noexcept
        {
            GuiWindowData* data = Find(window);
            return data != nullptr ? data->Retired.Size() : 0;
        }

        /// Returns the renderer drawing `window`'s GUI (where a viewport registers its target), or
        /// null when it has none.
        [[nodiscard]] vg::renderer::VGRenderer* RendererFor(const graphics::RenderWindow* window) noexcept
        {
            GuiWindowData* data = Find(window);
            return data != nullptr ? &data->Renderer : nullptr;
        }
        /// Returns the window that `root` is a pane of (as its root or another pane), or null when it
        /// is in none.
        [[nodiscard]] graphics::RenderWindow* WindowFor(const SceneRoot* root) noexcept
        {
            for (const Attached& attached : m_attached)
                for (const UniquePtr<GuiWindowData::Pane>& pane : attached.Data->Panes)
                    if (pane->Root.Get() == root)
                        return attached.Window;
            return nullptr;
        }
        /// Returns where `root` draws, in its window's pixels; empty when it is in no window.
        [[nodiscard]] Rectangle PaneArea(const SceneRoot& root) noexcept
        {
            for (const Attached& attached : m_attached)
                for (const UniquePtr<GuiWindowData::Pane>& pane : attached.Data->Panes)
                    if (pane->Root.Get() == &root)
                        return AreaOf(*attached.Window, pane->Region);
            return Rectangle{};
        }
        [[nodiscard]] usize WindowCount() const noexcept { return m_attached.Size(); }

        /// Styles `root` through `styles` (null: the host's) and lays it out, as Update does for each
        /// pane. This is for a root drawn elsewhere (offscreen).
        void StyleAndLayOut(SceneRoot& root, StyleManager* styles = nullptr)
        {
            StyleManager& manager = styles != nullptr ? *styles : m_styles;
            manager.SetMediaContext(root.GetMediaContext());
            manager.ApplyTree(root);
            root.UpdateLayout();
        }

        /// FramesDrawn counts frames in which a window's panes were drawn; FramesReused counts frames
        /// in which a window encoded its last batch again.
        [[nodiscard]] u64 FramesDrawn() const noexcept { return m_framesDrawn; }
        [[nodiscard]] u64 FramesReused() const noexcept { return m_framesReused; }

    private:
        friend class GuiOffscreen; // draws with the host's device, shaders and baker

        [[nodiscard]] shell::WindowCapabilities Capabilities() const noexcept
        {
            const shell::IWindowManager* windows = m_shell.WindowManager();
            return windows != nullptr ? windows->Capabilities() : shell::WindowCapabilities{};
        }

        // Sizes the panes' roots to their regions and the window's scale; returns whether any changed.
        bool SizePanes(GuiWindowData& data, graphics::RenderWindow& window, f32 scale)
        {
            bool changed = false;
            for (const UniquePtr<GuiWindowData::Pane>& pane : data.Panes)
            {
                SceneRoot& root = *pane->Root;
                const Rectangle area = AreaOf(window, pane->Region);
                if (root.GetDisplayScale() != scale)
                    root.SetDisplayScale(scale);
                if (root.GetViewportSize() != Float2{area.width, area.height})
                {
                    root.SetViewportSize(Float2{area.width, area.height});
                    changed = true;
                }
            }
            return changed;
        }

        // Initializes the baker's renderer with the first target's format and blend space. Returns
        // false when there is no baker or it failed to initialize.
        bool EnsureBaker(rhi::TextureFormat format, vg::VGBlendSpace space)
        {
            if (!m_baker)
                return false;
            if (!m_bakerReady)
            {
                m_bakerSetup.Format = format;
                m_bakerSetup.BlendSpace = space;
                m_bakerReady = m_baker->Initialize(m_bakerSetup).IsOk();
                if (m_bakerReady)
                    m_context->SetPictureBaker(m_baker.Get());
            }
            return m_bakerReady;
        }

        struct Attached
        {
            graphics::RenderWindow* Window = nullptr;
            GuiWindowData* Data = nullptr; // owned by the RenderWindow
        };

        // Returns a pane's area in its window's pixels (a region is given in pixels too), the whole
        // window by default. The GUI lays out to it and draws into the swapchain at the same size, so
        // a scaled display (more pixels than window units) is filled rather than drawn into a corner.
        [[nodiscard]] static Rectangle AreaOf(graphics::RenderWindow& window, Rectangle region) noexcept
        {
            if (region.width > 0.0f && region.height > 0.0f)
                return region;
            return Rectangle{0.0f, 0.0f, static_cast<f32>(window.Window().PixelWidth()), static_cast<f32>(window.Window().PixelHeight())};
        }
        // Returns how many of the window's pixels a window unit holds, across and down (1 on an
        // unscaled display).
        [[nodiscard]] static Float2 PixelsPerUnit(const shell::IWindow& window) noexcept
        {
            const f32 w = static_cast<f32>(window.Width());
            const f32 h = static_cast<f32>(window.Height());
            return Float2{w > 0.0f ? static_cast<f32>(window.PixelWidth()) / w : 1.0f, h > 0.0f ? static_cast<f32>(window.PixelHeight()) / h : 1.0f};
        }
        // Converts a pane's area to window units, in which the pointer arrives.
        [[nodiscard]] static Rectangle InWindowUnits(graphics::RenderWindow& window, Rectangle area) noexcept
        {
            const Float2 ratio = PixelsPerUnit(window.Window());
            return Rectangle{area.x / ratio.x, area.y / ratio.y, area.width / ratio.x, area.height / ratio.y};
        }
        // Makes a window's multisampled targets at `width` x `height`; returns null when the device
        // takes none.
        [[nodiscard]] UniquePtr<vg::renderer::VGPassTargets> MakeTargets(rhi::TextureFormat format, u32 width, u32 height)
        {
            UniquePtr<vg::renderer::VGPassTargets> targets = MakeUnique<vg::renderer::VGPassTargets>(m_allocator);
            targets->Configure(*m_device.Raw());
            if (!targets->Create(format, width, height))
                return UniquePtr<vg::renderer::VGPassTargets>{};
            return targets;
        }
        // Called as one of `data`'s frames begins: the retired targets age, and those no frame in
        // flight uses are freed.
        static void RetireTargets(GuiWindowData& data)
        {
            for (usize i = data.Retired.Size(); i > 0; --i)
                if (data.Retired[i - 1].FramesLeft-- <= 1)
                    data.Retired.RemoveAt(i - 1);
        }
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
        [[nodiscard]] GuiWindowData::Pane* FindPane(const SceneRoot& root) noexcept
        {
            for (const Attached& attached : m_attached)
                for (const UniquePtr<GuiWindowData::Pane>& pane : attached.Data->Panes)
                    if (pane->Root.Get() == &root)
                        return pane.Get();
            return nullptr;
        }
        [[nodiscard]] GuiWindowData* FindData(const SceneRoot& root) noexcept
        {
            for (const Attached& attached : m_attached)
                for (const UniquePtr<GuiWindowData::Pane>& pane : attached.Data->Panes)
                    if (pane->Root.Get() == &root)
                        return attached.Data;
            return nullptr;
        }

        struct Hosted
        {
            IHostedContent* Content = nullptr;
            graphics::RenderWindow* Window = nullptr; // the window it is bound to
            bool Routed = false; // its surface is in the router: it is bound and its node shown
        };

        // Binds each content to the window its node is in (again when that changes) and places it
        // over its node, then updates their router, so the application reads what it gates this
        // frame. A content whose node is not shown (a page behind another tab) leaves the router
        // until it is shown. It keeps the place it had while shown, so otherwise the router's last
        // match would give it the pointer meant for what is in front.
        void PlaceContents()
        {
            for (Hosted& hosted : m_contents)
            {
                Node* node = hosted.Content->HostedNode();
                SceneRoot* root = node != nullptr ? node->GetSceneRoot() : nullptr;
                graphics::RenderWindow* window = root != nullptr ? WindowFor(root) : nullptr;
                if (window != hosted.Window)
                {
                    Unbind(hosted);
                    if (window != nullptr)
                    {
                        hosted.Content->Bind(*m_device.Raw(), RendererFor(window), m_shell.Input(), window->Window().Id());
                        hosted.Window = window;
                    }
                }
                if (window == nullptr)
                    continue;
                const bool shown = node->IsTreeVisible();
                Route(hosted, shown);
                if (!shown)
                    continue; // its targets are kept for when it shows again
                const Rectangle pane = PaneArea(*root);
                const Rect bounds = node->GetScreenBounds(); // the root's device pixels
                const Rectangle pixels{pane.x + bounds.x, pane.y + bounds.y, bounds.width, bounds.height};
                hosted.Content->Place(pixels, InWindowUnits(*window, pixels),
                                      Float2{static_cast<f32>(window->Window().Width()), static_cast<f32>(window->Window().Height())});
            }
            m_contentRouter->Update();
        }
        // Puts the content's surface in the content router (`on`) or takes it out.
        void Route(Hosted& hosted, bool on)
        {
            shell::InputSurface* surface = hosted.Content->HostedSurface();
            if (surface == nullptr || hosted.Routed == on)
                return;
            if (on)
                m_contentRouter->AddSurface(surface);
            else
                m_contentRouter->RemoveSurface(surface);
            hosted.Routed = on;
        }
        void Unbind(Hosted& hosted)
        {
            if (hosted.Window == nullptr)
                return;
            Route(hosted, false);
            hosted.Content->Bind(*m_device.Raw(), nullptr, m_shell.Input(), 0);
            hosted.Window = nullptr;
        }

        void Forget(GuiWindowData::Pane& pane)
        {
            m_router->RemoveSurface(pane.Surface.Get());
            pane.Bridge->SetTextInputTarget(nullptr); // the window may close after this
        }
        // Forgets a window that is leaving. Its panes' roots are released now (the trees may hold
        // widgets showing models their application frees next), its renderer forgets the bakes, and
        // its hosted contents release it. The GPU half of the payload stays with the RenderWindow
        // until the GPU is done with it.
        void Forget(const Attached& attached)
        {
            for (const UniquePtr<GuiWindowData::Pane>& pane : attached.Data->Panes)
                Forget(*pane);
            attached.Data->Panes.Clear();
            for (Hosted& hosted : m_contents) // their window is going, so they release its renderer
                if (hosted.Window == attached.Window)
                    Unbind(hosted);
            if (m_baker)
                m_baker->RemoveTarget(attached.Data->Renderer);
        }

        // Reports what happened to the windows the host serves since the last pump. It keeps the
        // focus order (where windows overlap, the one focused last is on top) and tells the
        // listeners about each window by its root.
        void ReportWindowEvents()
        {
            const shell::IWindowManager* windows = m_shell.WindowManager();
            if (windows == nullptr)
                return;
            for (const shell::WindowEvent& event : windows->Events())
            {
                const Attached* attached = FindById(event.windowId);
                if (attached == nullptr || attached->Data->Panes.IsEmpty())
                    continue;
                if (event.type == shell::WindowEventType::FocusGained)
                {
                    for (usize f = 0; f < m_focusOrder.Size(); ++f)
                        if (m_focusOrder[f] == event.windowId)
                        {
                            m_focusOrder.RemoveAt(f);
                            break;
                        }
                    m_focusOrder.Insert(0, event.windowId);
                }
                const RefPtr<SceneRoot> root = attached->Data->Panes[0]->Root; // a listener may close it
                for (usize l = 0; l < m_windowListeners.Size(); ++l) // by index, since a listener may remove one
                {
                    const u32 token = m_windowListeners[l].Token;
                    m_windowListeners[l].Listener(*root, event);
                    if (l < m_windowListeners.Size() && m_windowListeners[l].Token != token)
                        --l; // it removed itself, so the next listener is at this index
                }
            }
        }

        void RouteInput()
        {
            shell::IInputManager* input = m_shell.Input();
            if (input == nullptr || m_attached.IsEmpty())
                return;
            // The pointer goes to the window a press began in while any button is held, and on the
            // frame the buttons are released (wherever the pointer is then, outside every window or
            // over another); otherwise it goes to the window under it.
            bool held = false;
            if (shell::IMouse* mouse = input->Mouse())
            {
                held = mouse->IsButtonDown(shell::MouseButton::Left) || mouse->IsButtonDown(shell::MouseButton::Middle) ||
                       mouse->IsButtonDown(shell::MouseButton::Right);
                mouse->SetGlobalCapture(held); // its events keep coming past the window's edge
            }
            const Attached* pointer = FindById(m_pressWindow);
            if (pointer == nullptr)
                pointer = FindById(input->HoverWindow());
            if (pointer == nullptr && m_attached.Size() == 1)
                pointer = &m_attached[0];
            if (pointer != nullptr)
            {
                graphics::RenderWindow& window = *pointer->Window;
                const Float2 size{static_cast<f32>(window.Window().Width()), static_cast<f32>(window.Window().Height())};
                for (const UniquePtr<GuiWindowData::Pane>& pane : pointer->Data->Panes)
                {
                    // The pointer arrives in window units; the pane lays out in pixels, so the surface
                    // maps the one to the other.
                    const Rectangle area = AreaOf(window, pane->Region);
                    pane->Surface->SetRegion(InWindowUnits(window, area));
                    pane->Surface->SetWindowSize(size); // places fingers
                    pane->Surface->SetContentSize(Float2{area.width, area.height});
                    if (!m_pointerFilter || !m_pointerFilter(*pane->Root, *pane->Surface))
                        pane->Bridge->PumpFromSurface(*pane->Surface);
                }
                if (held && m_pressWindow == 0)
                    m_pressWindow = window.Window().Id();
            }
            if (!held)
                m_pressWindow = 0; // its release was routed above
            // Keys, text and the gamepad go to the pane clicked last in the focused window, else to
            // its first pane.
            const Attached* focused = FindById(input->FocusedWindow());
            if (focused == nullptr)
                focused = &m_attached[0];
            GuiWindowData::Pane* keys = focused->Data->Panes.IsEmpty() ? nullptr : focused->Data->Panes[0].Get();
            for (usize i = 1; i < focused->Data->Panes.Size(); ++i)
                if (focused->Data->Panes[i]->Surface->Focused())
                    keys = focused->Data->Panes[i].Get();
            if (keys != nullptr)
                for (const shell::InputEvent& event : input->Events())
                    switch (event.kind)
                    {
                    case shell::InputEventKind::KeyDown:
                    case shell::InputEventKind::KeyUp:
                    case shell::InputEventKind::TextInput:
                    case shell::InputEventKind::GamepadButtonDown:
                    case shell::InputEventKind::GamepadButtonUp:
                    case shell::InputEventKind::GamepadAxis:
                        (void)keys->Bridge->Dispatch(event);
                        break;
                    default:
                        break;
                    }
            // Files the OS dropped go to the window they landed on (its root).
            Array<shell::DroppedFile> drops(m_allocator);
            m_shell.DrainDroppedFiles(drops);
            if (m_dropObserver)
                for (const shell::DroppedFile& drop : drops)
                    m_dropObserver(drop);
            if (!drops.IsEmpty())
                for (const Attached& attached : m_attached)
                    if (!attached.Data->Panes.IsEmpty())
                        (void)attached.Data->Panes[0]->Bridge->DispatchFileDrops(drops, attached.Window->Window().Id());
        }

        // Returns the clear colour for a window: as authored when it blends encoded, else in linear
        // light (an sRGB target encodes what a pass clears to).
        [[nodiscard]] rhi::ClearColor ClearFor(bool encoded) const noexcept
        {
            if (encoded)
                return rhi::ClearColor(m_clear.r, m_clear.g, m_clear.b, m_clear.a);
            return rhi::ClearColor(SrgbToLinear(m_clear.r), SrgbToLinear(m_clear.g), SrgbToLinear(m_clear.b), m_clear.a);
        }

        // Answers the GUI's drags about where its roots are, from the host's windows.
        class Places final : public IRootPlaces
        {
        public:
            explicit Places(GuiHost& host) noexcept : m_host(host) {}
            EventDispatcher* DispatcherAt(Float2 screen, Float2& local) override
            {
                SceneRoot* root = m_host.RootAt(screen, &local);
                return root != nullptr ? root->GetEventDispatcher() : nullptr;
            }
            bool ToScreen(const EventDispatcher& dispatcher, Float2 local, Float2& screen) override
            {
                for (const Attached& attached : m_host.m_attached)
                    for (const UniquePtr<GuiWindowData::Pane>& pane : attached.Data->Panes)
                        if (pane->Root->GetEventDispatcher() == &dispatcher)
                            return m_host.ToScreen(*pane->Root, local, screen);
                return false;
            }

        private:
            GuiHost& m_host;
        };

        IAllocator& m_allocator;
        graphics::GraphicsDevice& m_device;
        shell::IShell& m_shell;
        fonts::IFontService* m_fonts;
        VgShaderSet m_shaders; // borrowed modules
        RefPtr<GUIContext> m_context;
        StyleManager m_styles;
        UniquePtr<ShellClipboard> m_clipboard;
        UniquePtr<shell::InputRouter> m_router;
        foundation::input::InputKindTracker m_inputKind; // tracked by the host from the shell's devices
        UniquePtr<shell::InputRouter> m_contentRouter; // the hosted contents' surfaces
        UniquePtr<GpuPictureBaker> m_baker;
        PictureBakerSetup m_bakerSetup;
        bool m_bakerReady = false;
        bool m_bakesRecorded = false;
        Array<Attached> m_attached;
        Array<Hosted> m_contents;
        PointerFilter m_pointerFilter;
        Overlay m_overlay;
        Function<void(const shell::DroppedFile&)> m_dropObserver;
        u32 m_pressWindow = 0; // the window a held press began in
        WindowOpener m_opener;
        struct WindowListener
        {
            u32 Token = 0;
            WindowEventListener Listener;
        };
        Array<WindowListener> m_windowListeners{m_allocator};
        u32 m_nextWindowListener = 1;
        Places m_places{*this};
        Array<u32> m_focusOrder{m_allocator}; // window ids, most recently focused first
        f32 m_uiScale = 1.0f;
        vg::VGBlendSpace m_blendSpace = vg::VGBlendSpace::Encoded;
        Color m_clear{0.07f, 0.07f, 0.09f, 1.0f};
        u64 m_framesDrawn = 0;
        u64 m_framesReused = 0;
    };
}
