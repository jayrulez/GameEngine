// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Hosting - GuiHost on the Null RHI backend and the null shell: a window's root gets the
// window's size and its own renderer; a root is drawn when it needs a frame and its last batch
// encoded again when not; the baker serves every window's renderer and a detached window's forgets
// its bakes; a window opened for a root through the application's opener, its events told by root,
// the root under a screen point, and closed again (D6). (The null shell injects no input: routing is
// checked by the sandbox on the host.)
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.rhi;
import foundation.shell;
import foundation.shell.null;
import foundation.graphics;
import foundation.graphics.null;
import foundation.image;
import foundation.vg.svg;
import foundation.vg.renderer;
import experimental.gui;
import experimental.gui.hosting;

using namespace foundation::core;
using namespace experimental::gui;
namespace rhi = foundation::rhi;
namespace shell = foundation::shell;
namespace graphics = foundation::graphics;

namespace
{
    rhi::ShaderModule* MakeModule(rhi::Device& d)
    {
        const u8 dummy[4] = {0, 0, 0, 0};
        rhi::ShaderModuleDesc desc{};
        desc.code = Span<const u8>(dummy, 4);
        desc.entryPoint = u8"main";
        rhi::ShaderModule* m = nullptr;
        (void)d.CreateShaderModule(desc, m);
        return m;
    }

    // A null shell and device, the two VG shaders every draw needs, and a host on them.
    struct Bed
    {
        shell::NullShell shell;
        UniquePtr<graphics::GraphicsDevice> device = graphics::CreateNullGraphicsDevice(2).Value();
        rhi::ShaderModule* vs = MakeModule(*device->Raw());
        rhi::ShaderModule* fs = MakeModule(*device->Raw());
        UniquePtr<graphics::RenderWindow> main = device->CreateRenderWindow(*shell.MainWindow(), graphics::RenderWindowDesc{}).Value();
        UniquePtr<GuiHost> host;

        Bed()
        {
            VgShaderSet shaders;
            shaders.Vertex = vs;
            shaders.Fragment = fs;
            host = MakeUnique<GuiHost>(DefaultAllocator(), DefaultAllocator(), *device, shell, nullptr, shaders);
        }
        ~Bed()
        {
            host.Reset();
            main.Reset();
            device->Raw()->DestroyShaderModule(fs);
            device->Raw()->DestroyShaderModule(vs);
        }
        RefPtr<SceneRoot> MakeRoot() { return MakeRef<SceneRoot>(DefaultAllocator(), host->ContextRef()); }
        // One frame of the windows: the host's update, then each rendered, then the device's turn.
        void Frame(graphics::RenderWindow* first, graphics::RenderWindow* second = nullptr)
        {
            host->Update(1.0f / 60.0f);
            graphics::RenderWindow* const windows[2] = {first, second};
            for (graphics::RenderWindow* window : windows)
            {
                if (window == nullptr)
                    continue;
                graphics::FrameContext frame = window->BeginFrame();
                REQUIRE(frame.valid);
                host->RenderWindow(frame);
                window->EndFrame(frame);
            }
            device->AdvanceFrame();
        }
    };
}

TEST_CASE("gui.hosting: a window's root takes the window's size and a renderer of its own")
{
    Bed bed;
    RefPtr<SceneRoot> root = bed.MakeRoot();
    bed.host->AttachWindow(*bed.main, root);
    CHECK(bed.host->WindowCount() == 1u);
    CHECK(bed.host->RendererFor(bed.main.Get()) != nullptr);
    CHECK(bed.host->WindowFor(root.Get()) == bed.main.Get());
    const shell::IWindow& window = *bed.shell.MainWindow();
    CHECK(root->GetViewportSize().x == doctest::Approx(static_cast<f32>(window.Width())));
    CHECK(root->GetViewportSize().y == doctest::Approx(static_cast<f32>(window.Height())));
    CHECK(bed.host->Context().GetPictureBaker() == bed.host->Baker()); // its skins bake
    // A resized window resizes its root.
    static_cast<shell::NullWindow*>(bed.shell.MainWindow())->Resize(640, 360);
    bed.Frame(bed.main.Get());
    CHECK(root->GetViewportSize().x == doctest::Approx(640.0f));
    CHECK(root->GetViewportSize().y == doctest::Approx(360.0f));
}

TEST_CASE("gui.hosting: SizePanes gives the roots a resized window's size before the frame's update")
{
    Bed bed;
    RefPtr<SceneRoot> root = bed.MakeRoot();
    bed.host->AttachWindow(*bed.main, root);
    bed.Frame(bed.main.Get());
    static_cast<shell::NullWindow*>(bed.shell.MainWindow())->Resize(700, 420);
    bed.host->SetPaneRegion(*root, Rectangle{0.0f, 0.0f, 500.0f, 420.0f}); // an application's layout of its panes
    bed.host->SizePanes(); // what it lays out from is this frame's, not the last's
    CHECK(root->GetViewportSize().x == doctest::Approx(500.0f));
    CHECK(root->GetViewportSize().y == doctest::Approx(420.0f));
    const u64 drawn = bed.host->FramesDrawn();
    bed.Frame(bed.main.Get());
    CHECK(bed.host->FramesDrawn() == drawn + 1); // the new size is drawn
}

TEST_CASE("gui.hosting: a root draws when it needs a frame, else its window encodes the last batch again")
{
    Bed bed;
    RefPtr<SceneRoot> root = bed.MakeRoot();
    RefPtr<Panel> panel = MakeRef<Panel>(DefaultAllocator());
    root->AddChild(panel.Get());
    bed.host->AttachWindow(*bed.main, root);
    bed.Frame(bed.main.Get());
    CHECK(bed.host->FramesDrawn() == 1u);
    bed.Frame(bed.main.Get()); // nothing changed
    CHECK(bed.host->FramesDrawn() == 1u);
    CHECK(bed.host->FramesReused() == 1u);
    panel->Invalidate(); // something did
    bed.Frame(bed.main.Get());
    CHECK(bed.host->FramesDrawn() == 2u);
}

TEST_CASE("gui.hosting: the baker serves every window, and a detached window's renderer forgets its bakes")
{
    Bed bed;
    Result<shell::IWindow*> second = bed.shell.WindowManager()->CreateWindow(shell::WindowSettings{});
    REQUIRE(second.HasValue());
    UniquePtr<graphics::RenderWindow> other = bed.device->CreateRenderWindow(*second.Value(), graphics::RenderWindowDesc{}).Value();
    RefPtr<SceneRoot> mainRoot = bed.MakeRoot();
    RefPtr<SceneRoot> otherRoot = bed.MakeRoot();
    bed.host->AttachWindow(*bed.main, mainRoot);
    bed.host->AttachWindow(*other, otherRoot);
    CHECK(bed.host->WindowCount() == 2u);
    REQUIRE(bed.host->Baker() != nullptr);

    const foundation::vg::svg::SVGDocument document =
        foundation::vg::svg::SVGLoader::Load(StringView(u8"<svg width='8' height='8'><rect width='8' height='8'/></svg>")).Value();
    CHECK(bed.host->Baker()->Baked(document, 16, 16) == nullptr); // queued
    bed.Frame(bed.main.Get(), other.Get());
    const foundation::image::ImageData* key = bed.host->Baker()->Baked(document, 16, 16);
    REQUIRE(key != nullptr);
    CHECK(bed.host->RendererFor(bed.main.Get())->IsExternalTextureRegistered(key));
    CHECK(bed.host->RendererFor(other.Get())->IsExternalTextureRegistered(key));

    foundation::vg::renderer::VGRenderer* gone = bed.host->RendererFor(other.Get());
    const u32 held = otherRoot->RefCount();
    bed.host->DetachWindow(*other);
    CHECK(otherRoot->RefCount() == held - 1); // the root let go at once, not when the window goes
    CHECK(bed.host->WindowCount() == 1u);
    CHECK(bed.host->WindowFor(otherRoot.Get()) == nullptr);
    CHECK_FALSE(gone->IsExternalTextureRegistered(key)); // the payload lives until its window goes
    CHECK(bed.host->RendererFor(bed.main.Get())->IsExternalTextureRegistered(key));
    bed.Frame(bed.main.Get());
    other.Reset();
}

TEST_CASE("gui.hosting: an offscreen target draws a root at its own size, timed, and reads it back")
{
    Bed bed;
    RefPtr<SceneRoot> root = bed.MakeRoot();
    RefPtr<Panel> panel = MakeRef<Panel>(DefaultAllocator());
    root->AddChild(panel.Get());
    GuiOffscreen offscreen{DefaultAllocator(), *bed.host, 320, 200};
    REQUIRE(offscreen.IsReady());
    GuiOffscreen::Times times;
    foundation::image::Image picture;
    const String path = PathJoin(GetCurrentDirectory().AsView(), StringView(u8"gui-hosting-offscreen.png"));
    CHECK(offscreen.Picture(*root, path.AsView(), picture, &times));
    CHECK(FileExists(path.AsView()));
    CHECK(root->GetViewportSize().x == doctest::Approx(320.0f));
    CHECK(root->GetViewportSize().y == doctest::Approx(200.0f));
    CHECK(picture.Width() == 320u);
    CHECK(picture.Height() == 200u);
    CHECK(times.Draw >= 0.0);
    CHECK(times.Gpu >= 0.0);
    CHECK(root->GetLastFrameStats().FramesDrawn == 1u); // a frame of the root's, counted as a window's are
    CHECK(offscreen.Draw(*root)); // again, nothing read back or written
}

TEST_CASE("gui.hosting: a pane is another root over a region of a window, laid out at its size")
{
    Bed bed;
    static_cast<shell::NullWindow*>(bed.shell.MainWindow())->Resize(800, 600);
    RefPtr<SceneRoot> root = bed.MakeRoot();
    RefPtr<SceneRoot> pane = bed.MakeRoot();
    bed.host->AttachWindow(*bed.main, root);
    bed.host->SetPaneRegion(*root, Rectangle{0.0f, 0.0f, 600.0f, 600.0f});
    bed.host->AddPane(*bed.main, pane, Rectangle{600.0f, 0.0f, 200.0f, 600.0f});
    bed.Frame(bed.main.Get());
    CHECK(root->GetViewportSize().x == doctest::Approx(600.0f));
    CHECK(pane->GetViewportSize().x == doctest::Approx(200.0f));
    CHECK(pane->GetViewportSize().y == doctest::Approx(600.0f));
    CHECK(bed.host->WindowFor(pane.Get()) == bed.main.Get());
    CHECK(bed.host->PaneArea(*pane).x == doctest::Approx(600.0f));
    const u64 drawn = bed.host->FramesDrawn();
    bool overlaid = false;
    bed.host->SetOverlay([&overlaid](graphics::RenderWindow&, DrawContext&) { overlaid = true; });
    bed.host->SetPaneRegion(*pane, Rectangle{560.0f, 0.0f, 240.0f, 600.0f}); // the pane widened
    bed.host->SetPaneRegion(*root, Rectangle{0.0f, 0.0f, 560.0f, 600.0f});
    bed.Frame(bed.main.Get());
    CHECK(bed.host->FramesDrawn() == drawn + 1);
    CHECK(overlaid);
    CHECK(pane->GetViewportSize().x == doctest::Approx(240.0f));
    const u64 before = bed.host->FramesDrawn();
    bed.host->SetPaneRegion(*pane, Rectangle{560.0f, 0.0f, 240.0f, 600.0f}); // the same again, as a host's layout does each frame
    bed.Frame(bed.main.Get());
    CHECK(bed.host->FramesDrawn() == before); // nothing to draw
    bed.host->RemovePane(*pane);
    CHECK(bed.host->WindowFor(pane.Get()) == nullptr);
    CHECK(bed.host->WindowFor(root.Get()) == bed.main.Get());
}

TEST_CASE("gui.hosting: a window opened for a root, its events told by its root, the root under a screen point, closed again")
{
    Bed bed;
    RefPtr<SceneRoot> mainRoot = bed.MakeRoot();
    bed.host->AttachWindow(*bed.main, mainRoot);
    CHECK_FALSE(bed.host->CanOpenWindows());
    CHECK(bed.host->OpenWindow(bed.MakeRoot(), shell::WindowSettings{}) == nullptr); // no opener, no window

    auto& windows = *static_cast<shell::NullWindowManager*>(bed.shell.WindowManager());
    Array<UniquePtr<graphics::RenderWindow>> opened;
    int closed = 0;
    GuiHost::WindowOpener opener;
    opener.Open = [&](const shell::WindowSettings& settings) -> graphics::RenderWindow*
    {
        Result<shell::IWindow*> made = windows.CreateWindow(settings);
        if (!made.HasValue())
            return nullptr;
        opened.PushBack(bed.device->CreateRenderWindow(*made.Value(), graphics::RenderWindowDesc{}).Value());
        return opened.Back().Get();
    };
    opener.Close = [&](graphics::RenderWindow& window)
    {
        ++closed;
        windows.DestroyWindow(&window.Window());
    };
    bed.host->SetWindowOpener(Move(opener));
    REQUIRE(bed.host->CanOpenWindows());
    CHECK(bed.host->PlacesWindows()); // the null shell is no Wayland

    shell::WindowSettings settings;
    settings.positioned = true;
    settings.x = 1500;
    settings.y = 100;
    settings.width = 300;
    settings.height = 200;
    RefPtr<SceneRoot> second = bed.MakeRoot();
    graphics::RenderWindow* window = bed.host->OpenWindow(second, settings);
    REQUIRE(window != nullptr);
    CHECK(bed.host->WindowCount() == 2u);
    CHECK(bed.host->WindowFor(second.Get()) == window);
    bed.Frame(bed.main.Get(), window);
    CHECK(second->GetViewportSize().x == doctest::Approx(300.0f));

    // Where each root is on the screen.
    Float2 local;
    CHECK(bed.host->RootAt(Float2{1550.0f, 150.0f}, &local) == second.Get());
    CHECK(local.x == doctest::Approx(50.0f));
    CHECK(local.y == doctest::Approx(50.0f));
    CHECK(bed.host->RootAt(Float2{10.0f, 10.0f}) == mainRoot.Get());
    CHECK(bed.host->RootAt(Float2{-50.0f, -50.0f}) == nullptr);
    Float2 screen;
    REQUIRE(bed.host->ToScreen(*second, Float2{5.0f, 6.0f}, screen));
    CHECK(screen.x == doctest::Approx(1505.0f));
    CHECK(screen.y == doctest::Approx(106.0f));
    // The same, as its GUI's drags ask it.
    IRootPlaces* places = bed.host->Context().GetRootPlaces();
    REQUIRE(places != nullptr);
    CHECK(places->DispatcherAt(Float2{1550.0f, 150.0f}, local) == second->GetEventDispatcher());
    REQUIRE(places->ToScreen(*second->GetEventDispatcher(), Float2{5.0f, 6.0f}, screen));
    CHECK(screen.x == doctest::Approx(1505.0f));

    // Overlapping, the window focused last is on top.
    window->Window().SetPosition(5, 5);
    windows.PostEvent(shell::WindowEvent{shell::WindowEventType::FocusGained, window->Window().Id()});
    bed.Frame(bed.main.Get(), window);
    windows.ClearEvents();
    CHECK(bed.host->RootAt(Float2{50.0f, 50.0f}) == second.Get());
    windows.PostEvent(shell::WindowEvent{shell::WindowEventType::FocusGained, bed.main->Window().Id()});
    bed.Frame(bed.main.Get(), window);
    windows.ClearEvents();
    CHECK(bed.host->RootAt(Float2{50.0f, 50.0f}) == mainRoot.Get());

    // Its events, told by its root; a close asked for closes nothing by itself.
    Array<shell::WindowEvent> told;
    const SceneRoot* toldRoot = nullptr;
    bed.host->SetOnWindowEvent([&](SceneRoot& root, const shell::WindowEvent& event)
    {
        toldRoot = &root;
        told.PushBack(event);
    });
    windows.PostEvent(shell::WindowEvent{shell::WindowEventType::Moved, window->Window().Id(), 0, 0, 40, 30});
    windows.PostEvent(shell::WindowEvent{shell::WindowEventType::CloseRequested, window->Window().Id()});
    windows.PostEvent(shell::WindowEvent{shell::WindowEventType::Moved, 999u}); // no window of its own
    bed.Frame(bed.main.Get(), window);
    windows.ClearEvents();
    REQUIRE(told.Size() == 2u);
    CHECK(toldRoot == second.Get());
    CHECK(told[0].type == shell::WindowEventType::Moved);
    CHECK(told[0].x == 40);
    CHECK(told[1].type == shell::WindowEventType::CloseRequested);
    CHECK(bed.host->WindowCount() == 2u);

    bed.host->CloseWindow(*second);
    CHECK(closed == 1);
    CHECK(bed.host->WindowCount() == 1u);
    CHECK(bed.host->WindowFor(second.Get()) == nullptr);
    CHECK(bed.host->RootAt(Float2{50.0f, 50.0f}) == mainRoot.Get());
    bed.Frame(bed.main.Get());
}
