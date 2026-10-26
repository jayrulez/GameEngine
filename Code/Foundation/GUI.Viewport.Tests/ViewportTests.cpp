// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Viewport - the Viewport widget hosted on GuiHost, on the Null RHI backend and the null shell: bound
// to the renderer of the window its node is in, its targets at the node's device pixels and its
// colour target registered with that renderer, its drawing recorded on that window's frame (a clear
// when it has none), bound again when its node moves to another window, and let go when it leaves;
// what a game in a panel needs: a clear once outside drawing stops, the colour target's twin view,
// hosted text input, and the pointer mapped into the content's own resolution.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.rhi;
import foundation.shell;
import foundation.shell.null;
import foundation.graphics;
import foundation.graphics.null;
import foundation.vg.renderer;
import foundation.gui;
import foundation.gui.hosting;
import foundation.gui.viewport;

using namespace foundation::core;
using namespace foundation::gui;
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

    struct Bed
    {
        shell::NullShell shell;
        UniquePtr<graphics::GraphicsDevice> device = graphics::CreateNullGraphicsDevice(2).Value();
        rhi::ShaderModule* vs = MakeModule(*device->Raw());
        rhi::ShaderModule* fs = MakeModule(*device->Raw());
        UniquePtr<graphics::RenderWindow> main = device->CreateRenderWindow(*shell.MainWindow(), graphics::RenderWindowDesc{}).Value();
        UniquePtr<GuiHost> host;
        RefPtr<SceneRoot> root;
        RefPtr<Viewport> viewport = MakeRef<Viewport>(DefaultAllocator());

        Bed()
        {
            VgShaderSet shaders;
            shaders.Vertex = vs;
            shaders.Fragment = fs;
            host = MakeUnique<GuiHost>(DefaultAllocator(), DefaultAllocator(), *device, shell, nullptr, shaders);
            static_cast<shell::NullWindow*>(shell.MainWindow())->Resize(800, 600);
            root = MakeRef<SceneRoot>(DefaultAllocator(), host->ContextRef());
            root->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Start; });
            viewport->EditLayoutStyle([](LayoutStyle& s) {
                s.Width = LayoutLength::Px(320.0f);
                s.Height = LayoutLength::Px(200.0f);
            });
            root->AddChild(viewport.Get());
            host->AttachWindow(*main, root);
            viewport->HostIn(*host);
        }
        ~Bed()
        {
            viewport.Reset();
            root.Reset();
            host.Reset();
            main.Reset();
            device->Raw()->DestroyShaderModule(fs);
            device->Raw()->DestroyShaderModule(vs);
        }
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

TEST_CASE("gui.viewport: bound to its window's renderer, its targets at the node's device pixels, its drawing recorded")
{
    Bed bed;
    int renders = 0;
    bed.viewport->OnRender = [&renders](Viewport& viewport, rhi::CommandEncoder&, i32)
    {
        ++renders;
        CHECK(viewport.IsReady());
    };
    bed.Frame(bed.main.Get());
    CHECK(bed.viewport->WindowId() == bed.shell.MainWindow()->Id());
    REQUIRE(bed.viewport->IsReady());
    CHECK(bed.viewport->RenderWidth() == 320u);
    CHECK(bed.viewport->RenderHeight() == 200u);
    CHECK(renders == 1); // on the window's frame
    CHECK(bed.viewport->Mouse() != nullptr);
    bed.Frame(bed.main.Get());
    CHECK(renders == 2); // live content: every frame

    // At twice the density, twice the pixels.
    bed.host->SetUiScale(2.0f);
    bed.Frame(bed.main.Get());
    CHECK(bed.viewport->RenderWidth() == 640u);
    CHECK(bed.viewport->RenderHeight() == 400u);
}

TEST_CASE("gui.viewport: with nothing to draw it clears; hidden it records nothing")
{
    Bed bed;
    bed.Frame(bed.main.Get());
    CHECK(bed.viewport->IsReady()); // cleared to its colour, sampled by the GUI
    int renders = 0;
    bed.viewport->OnRender = [&renders](Viewport&, rhi::CommandEncoder&, i32) { ++renders; };
    bed.viewport->SetVisible(false);
    bed.Frame(bed.main.Get());
    CHECK(renders == 0);
}

TEST_CASE("gui.viewport: drawn outside OnRender, its picture is kept; made again, it clears until it is drawn")
{
    Bed bed;
    bed.Frame(bed.main.Get());
    REQUIRE(bed.viewport->IsReady());
    CHECK(bed.viewport->ClearCount() == 1u); // nothing defined yet: cleared, ready to sample
    CHECK(bed.viewport->ColorState() == rhi::ResourceState::ShaderRead);
    bed.Frame(bed.main.Get());
    CHECK(bed.viewport->ClearCount() == 1u); // what it holds is kept

    // A renderer with its own frame draws it, from the state it was in, and leaves it to sample.
    bed.viewport->SetColorState(rhi::ResourceState::RenderTarget);
    bed.viewport->SetColorState(rhi::ResourceState::ShaderRead);
    bed.Frame(bed.main.Get());
    CHECK(bed.viewport->ClearCount() == 1u); // not cleared over

    // Made again at another size: undefined until something draws it.
    bed.viewport->EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(400.0f); });
    bed.Frame(bed.main.Get());
    CHECK(bed.viewport->RenderWidth() == 400u);
    CHECK(bed.viewport->ClearCount() == 2u);
}

TEST_CASE("gui.viewport: moved to another window it is bound there; taken out of every window it lets go")
{
    Bed bed;
    Result<shell::IWindow*> second = bed.shell.WindowManager()->CreateWindow(shell::WindowSettings{});
    REQUIRE(second.HasValue());
    UniquePtr<graphics::RenderWindow> other = bed.device->CreateRenderWindow(*second.Value(), graphics::RenderWindowDesc{}).Value();
    RefPtr<SceneRoot> otherRoot = MakeRef<SceneRoot>(DefaultAllocator(), bed.host->ContextRef());
    bed.host->AttachWindow(*other, otherRoot);
    bed.Frame(bed.main.Get(), other.Get());
    CHECK(bed.viewport->WindowId() == bed.shell.MainWindow()->Id());

    int renders = 0;
    bed.viewport->OnRender = [&renders](Viewport&, rhi::CommandEncoder&, i32) { ++renders; };
    bed.viewport->RemoveFromParent();
    otherRoot->AddChild(bed.viewport.Get()); // a panel floated into a window of its own
    bed.Frame(bed.main.Get(), other.Get());
    CHECK(bed.viewport->WindowId() == second.Value()->Id());
    CHECK(renders == 1); // on the other window's frame only

    bed.viewport->RemoveFromParent();
    bed.Frame(bed.main.Get(), other.Get());
    CHECK(bed.viewport->WindowId() == 0u);
    CHECK(renders == 1);
    bed.host->DetachWindow(*other);
    other.Reset();
}

TEST_CASE("gui.viewport: content drawn outside that stopped is cleared once on request, then kept")
{
    Bed bed;
    bed.Frame(bed.main.Get());
    REQUIRE(bed.viewport->ClearCount() == 1u);
    bed.viewport->SetColorState(rhi::ResourceState::ShaderRead); // drawn outside
    bed.viewport->ClearPicture();
    bed.Frame(bed.main.Get());
    CHECK(bed.viewport->ClearCount() == 2u);
    CHECK(bed.viewport->ColorState() == rhi::ResourceState::ShaderRead);
    bed.Frame(bed.main.Get());
    CHECK(bed.viewport->ClearCount() == 2u); // kept
}

TEST_CASE("gui.viewport: an sRGB target has its twin view; a float one has none")
{
    Bed bed;
    bed.Frame(bed.main.Get());
    CHECK(bed.viewport->ColorAlternateView() == nullptr); // RGBA16Float: no twin
    bed.viewport->SetFormats(rhi::TextureFormat::RGBA8UnormSrgb, bed.viewport->DepthFormat());
    bed.Frame(bed.main.Get());
    CHECK(bed.viewport->ColorAlternateView() != nullptr);
}

TEST_CASE("gui.viewport: the hosted content's text input is the node's; the pointer maps into the content's own resolution")
{
    Bed bed;
    CHECK_FALSE(bed.viewport->WantsTextInput());
    bed.viewport->SetHostedTextInputWanted(true);
    CHECK(bed.viewport->WantsTextInput());

    bed.Frame(bed.main.Get());
    shell::InputSurface* surface = bed.viewport->HostedSurface();
    REQUIRE(surface != nullptr);
    CHECK(surface->Fit().contentSize.x == doctest::Approx(320.0f)); // the targets' own
    bed.viewport->SetContentResolution(1920, 1080, FitMode::Letterbox);
    bed.Frame(bed.main.Get());
    CHECK(surface->Fit().contentSize.x == doctest::Approx(1920.0f));
    CHECK(surface->Fit().contentSize.y == doctest::Approx(1080.0f));
    CHECK(surface->Fit().mode == FitMode::Letterbox);

    // At a fixed resolution too: the region is the content's rectangle inside the drawn targets.
    bed.viewport->SetFixedResolution(160, 200); // a tall screen in a wide node
    bed.viewport->SetFitMode(FitMode::Letterbox);
    bed.Frame(bed.main.Get());
    const Rectangle region = surface->Fit().region;
    CHECK(surface->Fit().mode == FitMode::Stretch);
    CHECK(region.width == doctest::Approx(160.0f)); // the 160 by 200 targets drawn at 160 by 200, centred
    CHECK(region.height == doctest::Approx(90.0f)); // a 16:9 picture letterboxed across their width
    CHECK(region.x == doctest::Approx(80.0f));
}
