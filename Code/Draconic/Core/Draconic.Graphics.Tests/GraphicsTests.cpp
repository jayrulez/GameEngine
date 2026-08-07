// Headless tests for the RHI render host (GraphicsDevice + RenderWindow +
// FrameContext) over the Null RHI backend + null shell - no GPU required.
// Covers device bring-up, per-window frame begin/end, the frame-in-flight ring,
// multi-window rendering, resize, and the minimized-skip path.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.graphics;
import draconic.graphics.null;
import draconic.shell;
import draconic.shell.null;

using namespace draconic::foundation;
using namespace draconic::graphics; // GraphicsDevice etc. (moved from draconic::runtime)
using namespace draconic::shell;

TEST_CASE("graphics: GraphicsDevice brings up over the null backend")
{
    auto created = CreateNullGraphicsDevice();
    REQUIRE(created.HasValue());
    UniquePtr<GraphicsDevice>& gd = created.Value();
    REQUIRE(gd.Get() != nullptr);
    CHECK(gd->Raw() != nullptr);
    CHECK(gd->GfxQueue() != nullptr);
    CHECK(gd->FramesInFlight() == 2u);
    CHECK(gd->CurrentFrame() == 0u);
}

TEST_CASE("graphics: a window renders, and the frame ring advances")
{
    NullShell shell;
    auto created = CreateNullGraphicsDevice(2);
    REQUIRE(created.HasValue());
    UniquePtr<GraphicsDevice>& gd = created.Value();

    auto rwResult = gd->CreateRenderWindow(*shell.MainWindow(), RenderWindowDesc{});
    REQUIRE(rwResult.HasValue());
    UniquePtr<RenderWindow>& rw = rwResult.Value();

    // Frame 0
    FrameContext f0 = rw->BeginFrame();
    CHECK(f0.valid);
    CHECK(f0.frameIndex == 0u);
    CHECK(f0.window == rw.Get());
    CHECK(f0.encoder != nullptr);
    CHECK(f0.backbufferView != nullptr);
    rw->EndFrame(f0);
    gd->AdvanceFrame();
    CHECK(gd->CurrentFrame() == 1u);

    // Frame 1 uses the next ring slot
    FrameContext f1 = rw->BeginFrame();
    CHECK(f1.valid);
    CHECK(f1.frameIndex == 1u);
    rw->EndFrame(f1);
    gd->AdvanceFrame();
    CHECK(gd->CurrentFrame() == 0u); // wraps with framesInFlight == 2

    // Frame 2 wraps back to slot 0 and must wait the slot-0 fence cleanly.
    FrameContext f2 = rw->BeginFrame();
    CHECK(f2.valid);
    CHECK(f2.frameIndex == 0u);
    rw->EndFrame(f2);
}

TEST_CASE("graphics: two windows render independently in one app frame")
{
    NullShell shell;
    IWindowManager* wm = shell.WindowManager();
    Result<IWindow*> second = wm->CreateWindow(WindowSettings{});
    REQUIRE(second.HasValue());

    auto created = CreateNullGraphicsDevice(2);
    REQUIRE(created.HasValue());
    UniquePtr<GraphicsDevice>& gd = created.Value();

    auto a = gd->CreateRenderWindow(*shell.MainWindow(), RenderWindowDesc{});
    auto b = gd->CreateRenderWindow(*second.Value(), RenderWindowDesc{});
    REQUIRE(a.HasValue());
    REQUIRE(b.HasValue());

    // Both windows render in the same app frame at the same ring index, then the
    // device advances once.
    FrameContext fa = a.Value()->BeginFrame();
    FrameContext fb = b.Value()->BeginFrame();
    CHECK(fa.valid);
    CHECK(fb.valid);
    CHECK(fa.frameIndex == fb.frameIndex);
    CHECK(fa.window != fb.window);
    a.Value()->EndFrame(fa);
    b.Value()->EndFrame(fb);
    gd->AdvanceFrame();
    CHECK(gd->CurrentFrame() == 1u);
}

TEST_CASE("graphics: SyncSize resizes the swapchain when the window changes")
{
    NullShell shell;
    auto created = CreateNullGraphicsDevice();
    REQUIRE(created.HasValue());
    UniquePtr<GraphicsDevice>& gd = created.Value();

    auto rwResult = gd->CreateRenderWindow(*shell.MainWindow(), RenderWindowDesc{});
    REQUIRE(rwResult.HasValue());
    UniquePtr<RenderWindow>& rw = rwResult.Value();

    CHECK_FALSE(rw->SyncSize()); // nothing changed yet

    static_cast<NullWindow*>(shell.MainWindow())->Resize(1600, 900);
    CHECK(rw->SyncSize()); // picked up the change
    CHECK(rw->Swap()->Width() == 1600u);
    CHECK(rw->Swap()->Height() == 900u);
    CHECK_FALSE(rw->SyncSize()); // stable again
}

TEST_CASE("graphics: FrameContext reports the BACKBUFFER size, not the live window size")
{
    // On web the canvas can resize between SyncSize and BeginFrame; the frame must describe
    // the attachment it actually renders into, or downstream viewports/scissors go out of
    // bounds and WebGPU validation drops the whole command buffer.
    NullShell shell;
    auto created = CreateNullGraphicsDevice();
    REQUIRE(created.HasValue());
    UniquePtr<GraphicsDevice>& gd = created.Value();

    auto rwResult = gd->CreateRenderWindow(*shell.MainWindow(), RenderWindowDesc{});
    REQUIRE(rwResult.HasValue());
    UniquePtr<RenderWindow>& rw = rwResult.Value();

    // Resize the window WITHOUT SyncSize: the swapchain (and therefore the frame) must
    // stay at the old size until the host syncs.
    const u32 oldW = rw->Swap()->Width();
    const u32 oldH = rw->Swap()->Height();
    static_cast<NullWindow*>(shell.MainWindow())->Resize(oldW + 320, oldH + 240);
    FrameContext frame = rw->BeginFrame();
    REQUIRE(frame.valid);
    CHECK(frame.width == oldW);
    CHECK(frame.height == oldH);
    rw->EndFrame(frame);
    gd->AdvanceFrame();
}

TEST_CASE("graphics: a minimized window yields an invalid frame")
{
    NullShell shell;
    auto created = CreateNullGraphicsDevice();
    REQUIRE(created.HasValue());
    UniquePtr<GraphicsDevice>& gd = created.Value();

    auto rwResult = gd->CreateRenderWindow(*shell.MainWindow(), RenderWindowDesc{});
    REQUIRE(rwResult.HasValue());
    UniquePtr<RenderWindow>& rw = rwResult.Value();

    static_cast<NullWindow*>(shell.MainWindow())->SetMinimized(true);
    FrameContext f = rw->BeginFrame();
    CHECK_FALSE(f.valid);
    rw->EndFrame(f); // must be a harmless no-op
}
