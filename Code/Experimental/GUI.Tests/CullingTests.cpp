// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - culling tests (E8b of Specs/gui-lab-evolution.md): a node whose subtree's ink misses the
// clip is not drawn, and culling never changes what shows. The differential test draws the same
// tree with culling off and on and requires the same geometry where it can show: every triangle
// that lands inside its command's scissor (in a layer that itself shows), the same in both.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vg;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;
namespace vg = foundation::vg;
using core::f32;
using core::u64;
using core::usize;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    core::RefPtr<UIWidget> Add(Node& parent, const char8_t* tag, const char8_t* cls = u8"")
    {
        auto w = Make<UIWidget>();
        w->SetTag(SV(tag));
        if (cls[0] != 0)
            w->AddClass(SV(cls));
        parent.AddChild(w.Get());
        return w;
    }

    // What can show of a batch: a hash per triangle that lands inside its command's scissor (or in
    // an unscissored command), in order, with what it is drawn with. A layer's commands show only
    // when its composite does; layers count by their order among those that show (a culled node's
    // layer shifts the others' indices), textures by identity.
    core::Array<u64> VisibleGeometry(const vg::VGBatch& batch)
    {
        const auto triangleShows = [&](const vg::VGCommand& cmd, usize first) -> bool
        {
            if (!cmd.scissored && cmd.clipMode != vg::VGClipMode::Scissor)
                return true;
            core::Float2 mn{1e30f, 1e30f}, mx{-1e30f, -1e30f};
            for (usize k = 0; k < 3; ++k)
            {
                const usize at = first + k;
                const core::Float2 p = cmd.drawMode == vg::VGDrawMode::Box ? batch.boxVertices[at].position
                                                                           : batch.vertices[batch.indices[at]].position;
                mn = core::Float2{core::Min(mn.x, p.x), core::Min(mn.y, p.y)};
                mx = core::Float2{core::Max(mx.x, p.x), core::Max(mx.y, p.y)};
            }
            const core::Rectangle& c = cmd.clipRect;
            return mx.x > c.x && mn.x < c.x + c.width && mx.y > c.y && mn.y < c.y + c.height;
        };
        const auto colours = [](const vg::VGCommand& cmd)
        {
            return cmd.fillPhase != vg::VGFillPhase::StencilWrite && cmd.fillPhase != vg::VGFillPhase::ClipApply &&
                   cmd.fillPhase != vg::VGFillPhase::ClipClear;
        };
        // Which layers show: one whose composite has a triangle that shows, inside a layer that shows.
        core::Array<bool> layerShows;
        layerShows.Resize(batch.layers.Size());
        for (usize c = batch.commands.Size(); c-- > 0;) // a child layer composites after its own commands
        {
            const vg::VGCommand& cmd = batch.commands[c];
            if (cmd.compositeLayer < 0)
                continue;
            const bool parentShows = cmd.layer < 0 || layerShows[static_cast<usize>(cmd.layer)];
            for (usize i = 0; parentShows && i + 3 <= static_cast<usize>(cmd.indexCount); i += 3)
                if (triangleShows(cmd, static_cast<usize>(cmd.startIndex) + i))
                    layerShows[static_cast<usize>(cmd.compositeLayer)] = true;
        }
        core::Array<core::i32> ordinal;
        ordinal.Resize(batch.layers.Size());
        core::i32 next = 0;
        for (usize l = 0; l < batch.layers.Size(); ++l)
            ordinal[l] = layerShows[l] ? next++ : -1;

        core::Array<u64> out;
        for (const vg::VGCommand& cmd : batch.commands)
        {
            if (!colours(cmd) || (cmd.layer >= 0 && !layerShows[static_cast<usize>(cmd.layer)]))
                continue;
            const void* texture = cmd.textureIndex >= 0 ? batch.textures[static_cast<usize>(cmd.textureIndex)] : nullptr;
            const core::i32 state[6] = {static_cast<core::i32>(cmd.drawMode), static_cast<core::i32>(cmd.blendMode),
                                        static_cast<core::i32>(cmd.fillPhase), cmd.layer >= 0 ? ordinal[static_cast<usize>(cmd.layer)] : -1,
                                        cmd.compositeLayer >= 0 ? ordinal[static_cast<usize>(cmd.compositeLayer)] : -1,
                                        cmd.backdrop ? 1 : 0};
            u64 stateHash = core::HashBytes(state, sizeof(state));
            stateHash = core::HashBytes(&texture, sizeof(texture), stateHash);
            for (usize i = 0; i + 3 <= static_cast<usize>(cmd.indexCount); i += 3)
            {
                const usize first = static_cast<usize>(cmd.startIndex) + i;
                if (!triangleShows(cmd, first))
                    continue;
                u64 h = stateHash;
                for (usize k = 0; k < 3; ++k)
                {
                    const usize at = first + k;
                    if (cmd.drawMode == vg::VGDrawMode::Box)
                        h = core::HashBytes(&batch.boxVertices[at], sizeof(vg::VGBoxVertex), h);
                    else
                        h = core::HashBytes(&batch.vertices[batch.indices[at]], sizeof(vg::VGVertex), h);
                }
                out.PushBack(h);
            }
        }
        return out;
    }

    // A long scrolled page of what reaches past its box: outer shadows, rounded clips holding more
    // than they show, rotated boxes, blurred groups, and a box half off the window.
    struct Page
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<UIWidget> scroller;
        StyleManager styles{CSSParser::Parse(SV(
            u8"page { width: 400px; height: 300px; overflow: auto; flex-direction: column; gap: 6px; }"
            u8"row { height: 30px; flex-direction: row; gap: 8px; }"
            u8"card { width: 60px; height: 24px; background-color: #336699; border-radius: 8px;"
            u8"       box-shadow: 0 6px 18px rgba(0, 0, 0, 0.5); }"
            u8"clip { width: 50px; height: 24px; overflow: hidden; border-radius: 10px; background-color: #444444; }"
            u8"clip > inner { width: 120px; height: 60px; background-color: #aa3344; }"
            u8"spin { width: 30px; height: 10px; background-color: #66aa33; transform: rotate(60deg); }"
            u8"fx { width: 40px; height: 20px; filter: blur(3px); }"
            u8"fx > dot { width: 20px; height: 20px; background-color: #ffffff; }"
            u8"edge { position: absolute; left: 380px; top: 40px; width: 60px; height: 30px; background-color: #cc9900;"
            u8"       box-shadow: 0 0 12px 4px #000000; }"))};

        Page()
        {
            root->SetSize(core::Float2{400.0f, 300.0f});
            scroller = Add(*root.Get(), u8"page");
            for (int i = 0; i < 80; ++i)
            {
                auto row = Add(*scroller.Get(), u8"row");
                (void)Add(*row.Get(), u8"card");
                auto clip = Add(*row.Get(), u8"clip");
                (void)Add(*clip.Get(), u8"inner");
                (void)Add(*row.Get(), u8"spin");
                auto fx = Add(*row.Get(), u8"fx");
                (void)Add(*fx.Get(), u8"dot");
            }
            (void)Add(*root.Get(), u8"edge");
        }
        core::Array<u64> Draw(bool culling, core::u32* culled = nullptr)
        {
            styles.ApplyTree(*root.Get());
            root->UpdateLayout();
            vg::VGContext canvas;
            DrawContext dc{canvas};
            dc.SetCulling(culling);
            root->DrawFrame(dc);
            if (culled != nullptr)
                *culled = root->GetLastFrameStats().NodesCulled;
            return VisibleGeometry(canvas.GetBatch());
        }
    };
}

TEST_CASE("culling: the same geometry shows with culling on as off, at the top, scrolled and at the end")
{
    Page page;
    constexpr f32 kScrolls[3] = {0.0f, 731.0f, 100000.0f};
    for (const f32 scroll : kScrolls)
    {
        CAPTURE(scroll);
        page.root->UpdateLayout();
        page.scroller->SetScrollOffset(core::Float2{0.0f, scroll});
        core::u32 culledOff = 0, culledOn = 0;
        const core::Array<u64> reference = page.Draw(false, &culledOff);
        const core::Array<u64> culled = page.Draw(true, &culledOn);
        CHECK(culledOff == 0u);
        CHECK(culledOn > 60u); // most of the 80 rows are out of view (a culled row counts once)
        REQUIRE(reference.Size() == culled.Size());
        bool same = true;
        for (usize i = 0; i < reference.Size(); ++i)
            same = same && reference[i] == culled[i];
        CHECK(same);
        CHECK(reference.Size() > 0u);
    }
}

TEST_CASE("culling: a node's ink reaches its outer shadow and its filter, and its subtree's past its box")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 300.0f});
    auto box = Add(*root.Get(), u8"box");
    StyleManager styles(CSSParser::Parse(SV(
        u8"box { position: absolute; left: 10px; top: 10px; width: 100px; height: 50px; box-shadow: 4px 6px 10px 2px #000000, inset 0 0 40px #ffffff; }"
        u8"box > far { position: absolute; left: 150px; top: 0px; width: 10px; height: 10px; }"
        u8"blurred { width: 20px; height: 20px; filter: blur(2px); }")));
    styles.ApplyTree(*root.Get());
    root->UpdateLayout();
    // The outer shadow: 2 of spread and 15 of blur (three sigma of a CSS blur of 10) past the
    // offset; the inset one adds nothing.
    const Rect ink = box->GetInkBounds();
    CHECK(ink.x == doctest::Approx(4.0f - 17.0f));
    CHECK(ink.y == doctest::Approx(6.0f - 17.0f));
    CHECK(ink.Right() == doctest::Approx(100.0f + 4.0f + 17.0f));
    CHECK(ink.Bottom() == doctest::Approx(50.0f + 6.0f + 17.0f));

    // A child placed past the box widens the subtree's ink, and moving it changes the ink kept.
    auto far = Add(*box.Get(), u8"far");
    styles.ApplyTree(*root.Get());
    root->UpdateLayout();
    CHECK(box->GetSubtreeInk().Right() == doctest::Approx(160.0f));
    far->SetPosition(core::Float2{300.0f, 0.0f});
    CHECK(box->GetSubtreeInk().Right() == doctest::Approx(310.0f));

    // A filter's blur widens it all by three sigma.
    auto blurred = Add(*root.Get(), u8"blurred");
    styles.ApplyTree(*root.Get());
    root->UpdateLayout();
    CHECK(blurred->GetSubtreeInk().x == doctest::Approx(-6.0f));
    CHECK(blurred->GetSubtreeInk().width == doctest::Approx(32.0f));
}

namespace
{
    // A host's frame: style, tick, lay out, ask, draw; what it asked.
    bool HostFrame(SceneRoot& root, StyleManager& styles)
    {
        styles.ApplyTree(root);
        root.GetContext().Update(core::Duration::FromSeconds(0.016));
        root.UpdateLayout();
        const bool needed = root.NeedsFrame();
        vg::VGContext canvas;
        DrawContext dc{canvas};
        root.DrawFrame(dc);
        return needed;
    }
}

TEST_CASE("culling: a fading box out of view asks for no frames, scrolled into view it does; a turning one always does")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 300.0f});
    auto page = Add(*root.Get(), u8"page");
    auto spacer = Add(*page.Get(), u8"spacer");
    auto fading = Add(*page.Get(), u8"fading");
    StyleManager styles(CSSParser::Parse(SV(
        u8"page { width: 400px; height: 300px; overflow: auto; flex-direction: column; }"
        u8"spacer { height: 2000px; }"
        u8"fading { width: 40px; height: 40px; background-color: #ffffff; animation: fade 1s infinite alternate; }"
        u8"turning { width: 40px; height: 40px; background-color: #ffffff; animation: turn 1s infinite; }"
        u8"@keyframes fade { from { opacity: 0.2; } to { opacity: 1; } }"
        u8"@keyframes turn { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }")));

    CHECK(HostFrame(*root.Get(), styles)); // the first frame
    CHECK(fading->GetDrawnFrame() == 0u);  // culled: below the page's view
    CHECK_FALSE(HostFrame(*root.Get(), styles));
    CHECK(root->GetLastFrameStats().AnimatedNode.IsEmpty());

    page->SetScrollOffset(core::Float2{0.0f, 1900.0f}); // into view: a redraw, which draws it
    CHECK(HostFrame(*root.Get(), styles));
    CHECK(fading->GetDrawnFrame() != 0u);
    CHECK(HostFrame(*root.Get(), styles)); // now its animation asks
    CHECK(root->GetLastFrameStats().Reasons.Animation);

    page->SetScrollOffset(core::Float2{0.0f, 0.0f});
    CHECK(HostFrame(*root.Get(), styles));
    CHECK_FALSE(HostFrame(*root.Get(), styles)); // out of view again: quiet

    // A turning box could swing into view: out of view, it still asks.
    auto turning = Add(*page.Get(), u8"turning");
    CHECK(HostFrame(*root.Get(), styles));
    CHECK(HostFrame(*root.Get(), styles));
    CHECK(turning->GetDrawnFrame() == 0u);
    CHECK(HostFrame(*root.Get(), styles));
    CHECK(root->GetLastFrameStats().Reasons.Animation);
}
