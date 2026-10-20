// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Hosting - GuiHost on the Null RHI backend and the null shell: a window's root gets the
// window's size and its own renderer; a root is drawn when it needs a frame and its last batch
// encoded again when not; the baker serves every window's renderer and a detached window's forgets
// its bakes. (The null shell injects no input: routing is checked by the sandbox on the host.)
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
    bed.host->DetachWindow(*other);
    CHECK(bed.host->WindowCount() == 1u);
    CHECK(bed.host->WindowFor(otherRoot.Get()) == nullptr);
    CHECK_FALSE(gone->IsExternalTextureRegistered(key)); // the payload lives until its window goes
    CHECK(bed.host->RendererFor(bed.main.Get())->IsExternalTextureRegistered(key));
    bed.Frame(bed.main.Get());
    other.Reset();
}
