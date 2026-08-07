// Render-bundle pass: a render pass whose body is supplied by render bundles (the rendergraph
// extension that lets parallel command recording run inside the frame graph). Driven on the
// Null RHI so Execute actually runs the pass.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;
import draconic.rendergraph;

using namespace draconic::foundation;
using namespace draconic::rendergraph;
namespace rhi = draconic::rhi;

namespace
{
    struct GraphHarness
    {
        rhi::null::NullDevice device{DefaultAllocator()};
        rhi::Texture* tex = nullptr;
        rhi::TextureView* view = nullptr;
        rhi::CommandPool* pool = nullptr;
        rhi::CommandEncoder* enc = nullptr;

        bool Init()
        {
            if (!device
                     .CreateTexture(
                         rhi::TextureDesc::RenderTarget(rhi::TextureFormat::BGRA8Unorm, 64, 64),
                         tex)
                     .IsOk())
            {
                return false;
            }
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::BGRA8Unorm;
            if (!device.CreateTextureView(tex, vd, view).IsOk())
            {
                return false;
            }
            if (!device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk())
            {
                return false;
            }
            return pool->CreateEncoder(enc).IsOk();
        }
        ~GraphHarness()
        {
            if (pool)
            {
                device.DestroyCommandPool(pool);
            }
            if (view)
            {
                device.DestroyTextureView(view);
            }
            if (tex)
            {
                device.DestroyTexture(tex);
            }
        }
    };
}

TEST_CASE("rg.bundle: a bundle pass records bundles before the pass + executes them")
{
    GraphHarness h;
    REQUIRE(h.Init());

    RenderGraph graph(&h.device);
    graph.SetOutputSize(64, 64);
    graph.BeginFrame(0);
    const RGHandle color = graph.ImportTarget(u8"BB", h.tex, h.view, rhi::ResourceState::Present);

    bool ran = false;
    usize bundleCount = 0;
    graph.AddRenderPass(
        u8"BundlePass",
        [&](PassBuilder& b)
        {
            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
            b.NeverCull();
            b.SetBundleExecute(
                [&](rhi::CommandEncoder& enc, Array<rhi::RenderBundle*>& out)
                {
                    ran =
                        true; // called with the encoder in the recording state, before the pass begins
                    rhi::RenderBundleDesc bd{};
                    bd.colorFormats[0] = rhi::TextureFormat::BGRA8Unorm;
                    bd.colorFormatCount = 1;
                    bd.width = 64;
                    bd.height = 64;
                    if (rhi::RenderBundleEncoder* be = enc.CreateRenderBundleEncoder(bd))
                    {
                        out.PushBack(be->Finish());
                    }
                    bundleCount = out.Size();
                });
        });

    CHECK(graph.Execute(h.enc).IsOk());
    CHECK(ran);               // the bundle callback ran during execution
    CHECK(bundleCount == 1u); // it produced a bundle for the graph to ExecuteBundles
}

TEST_CASE("rg.bundle: a bundle pass that produces no bundles still runs (clears only)")
{
    GraphHarness h;
    REQUIRE(h.Init());

    RenderGraph graph(&h.device);
    graph.SetOutputSize(64, 64);
    graph.BeginFrame(0);
    const RGHandle color = graph.ImportTarget(u8"BB", h.tex, h.view, rhi::ResourceState::Present);

    bool ran = false;
    graph.AddRenderPass(u8"EmptyBundlePass",
                        [&](PassBuilder& b)
                        {
                            b.SetColorTarget(0, color, rhi::LoadOp::Clear, rhi::StoreOp::Store);
                            b.NeverCull();
                            b.SetBundleExecute([&](rhi::CommandEncoder&, Array<rhi::RenderBundle*>&)
                                               { ran = true; });
                        });

    CHECK(graph.Execute(h.enc).IsOk());
    CHECK(ran);
}
