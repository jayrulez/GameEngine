// Headless ViewportView tests via the Null RHI backend: layout creates the offscreen color+depth
// targets and registers the color view into a VGRenderer as an external texture; OnDraw emits a
// textured quad that Prepare can turn into a valid slice (proving the external texture is usable
// end-to-end); RenderContent fires the render callback bracketed by state transitions.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;
import draconic.image;
import draconic.vg;
import draconic.vg.renderer;
import draconic.ui;
import draconic.ui.viewport;
import draconic.shell;
import draconic.shell.null;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;
namespace vg = draconic::vg;
namespace ui = draconic::ui;
using draconic::ui::viewport::ViewportView;

namespace
{
    rhi::ShaderModule* MakeModule(rhi::Device& d)
    {
        const u8 dummy[4] = {0, 0, 0, 0};
        rhi::ShaderModuleDesc desc{};
        desc.code = Span<const u8>(dummy, 4);
        rhi::ShaderModule* m = nullptr;
        (void)d.CreateShaderModule(desc, m);
        return m;
    }
}

TEST_CASE(
    "ui.viewport: layout creates targets + registers an external texture usable by the renderer")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);

    vg::renderer::VGRenderer renderer;
    REQUIRE(
        renderer.Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, /*frameCount*/ 2)
            .IsOk());

    ViewportView view;
    CHECK(view.IsFocusable);     // input target
    CHECK_FALSE(view.IsReady()); // no targets before layout
    view.Initialize(&device, &renderer, /*input*/ nullptr, /*windowId*/ 0);

    // Layout drives ResizeRenderTarget -> color+depth created, color view registered.
    view.Layout(0.0f, 0.0f, 200.0f, 150.0f);
    CHECK(view.IsReady());
    CHECK(view.RenderWidth() == 200u);
    CHECK(view.RenderHeight() == 150u);
    CHECK(view.ColorTargetView() != nullptr);
    CHECK(view.DepthTargetView() != nullptr);

    // OnDraw emits a textured quad referencing the viewport's image key.
    vg::VGContext vgctx;
    ui::UIDrawContext dctx(vgctx, 1.0f);
    view.OnDraw(dctx);
    vg::VGBatch& batch = vgctx.GetBatch();
    REQUIRE(batch.textures.Size() >= 1u);

    // Because the key is registered as an external texture, Prepare builds a valid slice for it
    // (an unregistered pixel-less ImageDataRef would yield no bind group).
    renderer.BeginFrame(0);
    const vg::renderer::VGRenderSlice slice = renderer.Prepare(batch, 0, 800, 600);
    CHECK(slice.isValid);

    renderer.Dispose();
    device.DestroyShaderModule(vs);
    device.DestroyShaderModule(fs);
}

TEST_CASE("ui.viewport: RenderContent fires the callback bracketed by transitions")
{
    rhi::null::NullDevice device{DefaultAllocator()};

    ViewportView view;
    view.Initialize(&device, /*renderer*/ nullptr, nullptr, 0);
    view.Layout(0.0f, 0.0f, 64.0f, 64.0f);
    REQUIRE(view.IsReady());

    rhi::CommandPool* pool = nullptr;
    REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
    rhi::CommandEncoder* enc = nullptr;
    REQUIRE(pool->CreateEncoder(enc).IsOk());

    // No callback -> no-op.
    view.RenderContent(*enc, 0);

    bool fired = false;
    i32 seenFrame = -1;
    ViewportView* seenSelf = nullptr;
    view.OnRender = [&](ViewportView& v, rhi::CommandEncoder&, i32 frameIndex)
    {
        fired = true;
        seenFrame = frameIndex;
        seenSelf = &v;
    };
    view.RenderContent(*enc, 3);
    CHECK(fired);
    CHECK(seenFrame == 3);
    CHECK(seenSelf == &view);

    pool->DestroyEncoder(enc);
    device.DestroyCommandPool(pool);
}

TEST_CASE("ui.viewport: resize re-registers; teardown is clean")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);

    vg::renderer::VGRenderer renderer;
    REQUIRE(renderer.Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 1).IsOk());

    {
        ViewportView view;
        view.Initialize(&device, &renderer, nullptr, 0);
        view.Layout(0.0f, 0.0f, 128.0f, 96.0f);
        CHECK(view.RenderWidth() == 128u);

        // A new size recreates + re-registers the targets.
        view.Layout(0.0f, 0.0f, 64.0f, 64.0f);
        CHECK(view.RenderWidth() == 64u);
        CHECK(view.RenderHeight() == 64u);
        CHECK(view.IsReady());
        // ~ViewportView unregisters + destroys targets (no double-free under ASAN).
    }

    renderer.Dispose();
    device.DestroyShaderModule(vs);
    device.DestroyShaderModule(fs);
}

TEST_CASE("ui.viewport: AttachToWindow re-registers into a new renderer (undock path)")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);

    // Two renderers standing in for two windows' per-window VGRenderers.
    vg::renderer::VGRenderer rendererA;
    vg::renderer::VGRenderer rendererB;
    REQUIRE(rendererA.Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 1).IsOk());
    REQUIRE(rendererB.Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 1).IsOk());

    ViewportView view;
    view.Initialize(&device, &rendererA, nullptr, 0);
    view.Layout(0.0f, 0.0f, 100.0f, 100.0f);
    REQUIRE(view.IsReady());

    // The color target draws in renderer A (docked in the main window).
    {
        vg::VGContext vgctx;
        ui::UIDrawContext dctx(vgctx, 1.0f);
        view.OnDraw(dctx);
        rendererA.BeginFrame(0);
        CHECK(rendererA.Prepare(vgctx.GetBatch(), 0, 640, 480).isValid);
    }

    // Undock into window B: re-bind. Now B can sample it; A no longer references it.
    view.AttachToWindow(&rendererB, /*windowId*/ 7);
    {
        vg::VGContext vgctx;
        ui::UIDrawContext dctx(vgctx, 1.0f);
        view.OnDraw(dctx);
        rendererB.BeginFrame(0);
        CHECK(rendererB.Prepare(vgctx.GetBatch(), 0, 640, 480).isValid);
    }

    rendererA.Dispose();
    rendererB.Dispose();
    device.DestroyShaderModule(vs);
    device.DestroyShaderModule(fs);
}

TEST_CASE("ui.viewport: default fit mode is Stretch; SetFitMode updates it")
{
    ViewportView view;
    CHECK(view.GetFitMode() == FitMode::Stretch);
    view.SetFitMode(FitMode::Letterbox);
    CHECK(view.GetFitMode() == FitMode::Letterbox);
}

TEST_CASE("ui.viewport: owns the RT formats (HDR default) + SetFormats recreates targets")
{
    rhi::null::NullDevice device{DefaultAllocator()};

    ViewportView view;
    CHECK(view.ColorFormat() == rhi::TextureFormat::RGBA16Float);
    CHECK(view.DepthFormat() == rhi::TextureFormat::Depth32Float);

    view.Initialize(&device, nullptr, nullptr, 0);
    view.Layout(0.0f, 0.0f, 80.0f, 60.0f);
    REQUIRE(view.IsReady());

    // Reconfigure to an LDR color + depth+stencil format: targets recreate, formats reported.
    view.SetFormats(rhi::TextureFormat::RGBA8Unorm, rhi::TextureFormat::Depth32FloatStencil8);
    CHECK(view.ColorFormat() == rhi::TextureFormat::RGBA8Unorm);
    CHECK(view.DepthFormat() == rhi::TextureFormat::Depth32FloatStencil8);
    CHECK(view.IsReady());
    CHECK(view.RenderWidth() == 80u);
}

TEST_CASE("ui.viewport: SyncInputRegion emits a PHYSICAL surface region at DpiScale != 1 (picking)")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    rhi::ShaderModule* vs = MakeModule(device);
    rhi::ShaderModule* fs = MakeModule(device);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    vg::renderer::VGRenderer renderer;
    REQUIRE(renderer.Initialize(device, *vs, *fs, rhi::TextureFormat::BGRA8UnormSrgb, 2).IsOk());

    draconic::shell::NullInputManager input; // input != null -> the view creates an InputSurface

    // Heap-allocate (RefPtr): the view becomes a child of a RootView, which owns its children.
    auto root = MakeRef<ui::RootView>(DefaultAllocator());
    auto view = MakeRef<ViewportView>(DefaultAllocator());
    view->Initialize(&device, &renderer, &input, /*windowId*/ 0);
    REQUIRE(view->Surface() != nullptr);

    root->DpiScale = 1.25f;    // OS content scale x the editor UI-scale preference
    root->AddView(view.Get()); // parents the view so SyncInputRegion can walk to the root
    view->Layout(0.0f, 0.0f, 200.0f, 150.0f); // LOGICAL rect (as the DpiScale-scaled UI lays out)

    view->SyncInputRegion();
    // The InputRouter transforms the raw mouse in PHYSICAL window pixels, so the surface region
    // must be physical (logical x DpiScale) - otherwise hover/pick/gizmo drift by the scale factor.
    // The content resolution stays the RT's own size (MakeMouseRay divides by RenderWidth).
    const ContentFit& fit = view->Surface()->Fit();
    CHECK(fit.region.width == doctest::Approx(200.0f * 1.25f));
    CHECK(fit.region.height == doctest::Approx(150.0f * 1.25f));
    CHECK(fit.contentSize.x == doctest::Approx(200.0f)); // RT resolution, unscaled
    CHECK(fit.contentSize.y == doctest::Approx(150.0f));

    // Regression guard: at 100% the region equals the logical rect (behavior unchanged).
    root->DpiScale = 1.0f;
    view->SyncInputRegion();
    CHECK(view->Surface()->Fit().region.width == doctest::Approx(200.0f));
    CHECK(view->Surface()->Fit().region.height == doctest::Approx(150.0f));

    renderer.Dispose();
    device.DestroyShaderModule(vs);
    device.DestroyShaderModule(fs);
}
