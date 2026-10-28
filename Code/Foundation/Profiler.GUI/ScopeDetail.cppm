// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Profiler.GUI - :scope_detail partition
//
// The name chosen, apart (X7 of Specs/editor-gui.md): its calls, total, mean and longest over the
// frames chosen (the span's, when one is marked) and the lanes it ran in; what it was called from;
// its children by name with their share of it, its own time last; and how long its calls take
// over every frame the source holds, as a histogram whose bars holding the frames chosen's calls
// are lit. Its longest call in the frames chosen is the one a page zooms the timeline to.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.profiler.gui:scope_detail;

import foundation.core;
import foundation.vg;
import foundation.gui;
import foundation.profiler;
import :focus;
import :widget;
import :scope_table;

using namespace foundation::core;
using namespace foundation::gui;

export namespace foundation::profiler::gui
{
    class ScopeDetail final : public ProfilerWidget
    {
        RTTI_OBJECT(ScopeDetail, ProfilerWidget)
    public:
        static constexpr usize kBins = 24;
        static constexpr f32 kLineHeight = 18.0f;
        static constexpr f32 kHistogramHeight = 56.0f;

        /// What it says about the name chosen.
        struct Summary
        {
            bool any = false;
            u64 calls = 0;
            u64 totalTicks = 0;
            u64 maxTicks = 0;
            String lanes;               // the lanes it ran in, by name
            Array<String> callers;      // what it was called from: "Physics.Step (Main)"
            struct Child
            {
                String name;
                NameId id = 0;
                u64 ticks = 0;
                bool self = false;
            };
            Array<Child> children;      // by their time, the longest first; its own time last
            Array<u32> bins;            // kBins: its calls' durations over every frame held
            Array<u8> lit;              // kBins: 1 where a call of the frames chosen falls
            u64 binLeast = 0;
            u64 binMost = 0;
            u64 heldCalls = 0;
            u32 heldFrames = 0;
            u64 longestBegin = 0;       // its longest call in the frames chosen
            u64 longestEnd = 0;
        };

        ScopeDetail()
        {
            SetTag(StringView(u8"scopedetail"));
            SetTabFocusable(false);
        }

        [[nodiscard]] const Summary& Describe() const
        {
            Refresh();
            return m_summary;
        }
        /// Its longest call in the frames chosen (false: none chosen, or it did not run there).
        [[nodiscard]] bool LongestCall(u64& begin, u64& end) const
        {
            const Summary& summary = Describe();
            if (!summary.any || summary.longestEnd <= summary.longestBegin)
                return false;
            begin = summary.longestBegin;
            end = summary.longestEnd;
            return true;
        }

        /// The parts it draws, which the sheet styles as `tag::part`.
        void CollectStyleParts(Array<StringView>& out) const override
        {
            ProfilerWidget::CollectStyleParts(out);
            out.PushBack(StringView(u8"title"));
            out.PushBack(StringView(u8"heading"));
            out.PushBack(StringView(u8"note"));
            out.PushBack(StringView(u8"track"));
            out.PushBack(StringView(u8"self"));
            out.PushBack(StringView(u8"plot"));
            out.PushBack(StringView(u8"bar"));
            out.PushBack(StringView(u8"lit"));
        }

        void OnProfileSourceChanged() override { Dirty(); }
        void OnProfileFramesChanged() override { Dirty(); }
        void OnProfileChoiceChanged() override { Dirty(); }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            UIWidget::OnDraw(ctx, localBounds);
            const Summary& summary = Describe();
            const Rect box = GetContentBounds();
            const IProfileSource* source = Source();
            const Color text = GetTextColor();
            const Color dim = PartTextColor(u8"note");
            const Color heading = PartTextColor(u8"heading");
            ctx.PushClip(box);
            f32 y = box.y;
            if (!summary.any || source == nullptr)
            {
                DrawLabel(ctx, StringView(u8"Choose a scope on the timeline or in the table"), Rect{box.x, y, box.width, kLineHeight}, dim);
                ctx.PopClip();
                return;
            }
            const ChosenName chosen = m_focus->Name();
            const StringView name = source->NameOf(chosen.name);
            const u64 frequency = source->TickFrequency();
            ctx.VG().FillRoundedRect(Rectangle{box.x, y + kLineHeight * 0.5f - 5.0f, 10.0f, 10.0f}, 2.0f, NameColor(name));
            DrawLabel(ctx, name, Rect{box.x + 16.0f, y, box.width - 16.0f, kLineHeight + 4.0f}, PartTextColor(u8"title"));
            y += kLineHeight + 6.0f;
            const String stats = Format(u8"{} {} on {}   total {}   mean {}   max {} ms", summary.calls,
                                        summary.calls == 1 ? StringView(u8"call") : StringView(u8"calls"), summary.lanes.AsView(),
                                        FormatFixed(TicksToMs(summary.totalTicks, frequency), 2).AsView(),
                                        FormatFixed(summary.calls > 0 ? TicksToMs(summary.totalTicks, frequency) / static_cast<f32>(summary.calls) : 0.0f, 2).AsView(),
                                        FormatFixed(TicksToMs(summary.maxTicks, frequency), 2).AsView());
            DrawLabel(ctx, stats.AsView(), Rect{box.x, y, box.width, kLineHeight}, dim);
            y += kLineHeight + 8.0f;

            DrawLabel(ctx, StringView(u8"Called from"), Rect{box.x, y, box.width, kLineHeight}, heading);
            y += kLineHeight;
            for (const String& caller : summary.callers)
            {
                DrawLabel(ctx, caller.AsView(), Rect{box.x, y, box.width, kLineHeight}, dim);
                y += kLineHeight;
            }
            y += 8.0f;

            if (!summary.children.IsEmpty())
            {
                DrawLabel(ctx, StringView(u8"Children"), Rect{box.x, y, box.width, kLineHeight}, heading);
                y += kLineHeight;
                const Color track = PartColor(u8"track", Color{0.0f, 0.0f, 0.0f, 0.18f});
                for (const Summary::Child& child : summary.children)
                {
                    const f32 share = summary.totalTicks > 0 ? static_cast<f32>(child.ticks) / static_cast<f32>(summary.totalTicks) : 0.0f;
                    const Color swatch = child.self ? PartColor(u8"self", Color{0.6f, 0.6f, 0.6f, 1.0f}) : NameColor(child.name.AsView());
                    ctx.VG().FillRoundedRect(Rectangle{box.x, y + kLineHeight * 0.5f - 5.0f, 10.0f, 10.0f}, 2.0f, swatch);
                    DrawLabel(ctx, child.name.AsView(), Rect{box.x + 16.0f, y, box.width * 0.55f, kLineHeight}, text);
                    const String figures = Format(u8"{} ms  {}%", FormatFixed(TicksToMs(child.ticks, frequency), 2).AsView(),
                                                  FormatFixed(share * 100.0f, 0).AsView());
                    DrawLabel(ctx, figures.AsView(), Rect{box.x, y, box.width, kLineHeight}, dim, TextHAlign::End);
                    y += kLineHeight;
                    ctx.VG().FillRoundedRect(Rectangle{box.x, y, box.width, 5.0f}, 2.5f, track);
                    ctx.VG().FillRoundedRect(Rectangle{box.x, y, Max(box.width * share, 2.0f), 5.0f}, 2.5f, swatch);
                    y += 8.0f;
                }
                y += 6.0f;
            }

            const String spread = Format(u8"Durations over {} frames ({} calls)", summary.heldFrames, summary.heldCalls);
            DrawLabel(ctx, spread.AsView(), Rect{box.x, y, box.width, kLineHeight}, heading);
            y += kLineHeight + 2.0f;
            const Rect plot{box.x, y, box.width, kHistogramHeight};
            ctx.VG().FillRect(plot.ToRectangle(), PartColor(u8"plot", Color{0.0f, 0.0f, 0.0f, 0.18f}));
            u32 most = 1;
            for (const u32 count : summary.bins)
                most = Max(most, count);
            const Color bar = PartColor(u8"bar", Color{0.44f, 0.53f, 0.72f, 1.0f});
            const Color lit = PartColor(u8"lit", Color{0.18f, 0.43f, 0.87f, 1.0f});
            const f32 binWidth = plot.width / static_cast<f32>(kBins);
            for (usize b = 0; b < summary.bins.Size(); ++b)
            {
                if (summary.bins[b] == 0)
                    continue;
                const f32 h = 2.0f + static_cast<f32>(summary.bins[b]) / static_cast<f32>(most) * (plot.height - 4.0f);
                ctx.VG().FillRect(Rectangle{plot.x + static_cast<f32>(b) * binWidth + binWidth * 0.1f, plot.Bottom() - h, binWidth * 0.8f, h},
                                  summary.lit[b] != 0 ? lit : bar);
            }
            y += kHistogramHeight + 2.0f;
            DrawLabel(ctx, Format(u8"{} ms", FormatFixed(TicksToMs(summary.binLeast, frequency), 2).AsView()).AsView(),
                      Rect{box.x, y, box.width, kLineHeight}, dim);
            DrawLabel(ctx, Format(u8"{} ms", FormatFixed(TicksToMs(summary.binMost, frequency), 2).AsView()).AsView(),
                      Rect{box.x, y, box.width, kLineHeight}, dim, TextHAlign::End);
            ctx.PopClip();
        }

    private:
        void Dirty()
        {
            m_dirty = true;
            Invalidate();
        }

        void Refresh() const
        {
            if (!m_dirty)
                return;
            m_dirty = false;
            m_summary = Summary{};
            const IProfileSource* source = Source();
            if (source == nullptr || !m_focus->Name().any)
                return;
            const ChosenName chosen = m_focus->Name();
            ScopeTree tree;
            tree.Build(*source, m_focus->Frames(), m_focus->Span());
            Summary& out = m_summary;
            Array<u32> lanes;
            for (const ScopeNode& node : tree.nodes)
            {
                if (node.group || node.name != chosen.name || node.gpu != chosen.gpu || node.calls == 0)
                    continue;
                out.any = true;
                out.calls += node.calls;
                out.totalTicks += node.totalTicks;
                out.maxTicks = Max(out.maxTicks, node.maxTicks);
                // The lanes it ran in, each once.
                const u32 group = Root(tree, node);
                bool seen = false;
                for (const u32 at : lanes)
                    seen = seen || at == group;
                if (!seen)
                {
                    lanes.PushBack(group);
                    if (!out.lanes.IsEmpty())
                        out.lanes += StringView(u8", ");
                    out.lanes += tree.nodes[group].groupName.AsView();
                }
                // What called it, each once.
                const ScopeNode& parent = tree.nodes[static_cast<usize>(node.parent)];
                const String caller = parent.group ? Format(u8"The top of {}", parent.groupName.AsView())
                                                   : Format(u8"{} ({})", source->NameOf(parent.name), tree.nodes[group].groupName.AsView());
                bool known = false;
                for (const String& at : out.callers)
                    known = known || at.AsView() == caller.AsView();
                if (!known)
                    out.callers.PushBack(caller);
                // Its children by name, and its own time.
                u64 own = node.selfTicks;
                for (const u32 child : node.children)
                    AddChildTime(out, source->NameOf(tree.nodes[child].name), tree.nodes[child].name, tree.nodes[child].totalTicks, false);
                AddChildTime(out, StringView(u8"Self"), 0, own, true);
            }
            if (!out.any)
                return;
            out.children.Sort([](const Summary::Child& a, const Summary::Child& b)
            {
                if (a.self != b.self)
                    return !a.self;
                return a.ticks > b.ticks;
            });
            Spread(*source, chosen, out);
        }

        static u32 Root(const ScopeTree& tree, const ScopeNode& node)
        {
            const ScopeNode* at = &node;
            while (at->parent >= 0)
                at = &tree.nodes[static_cast<usize>(at->parent)];
            return static_cast<u32>(at - tree.nodes.Data());
        }
        static void AddChildTime(Summary& out, StringView name, NameId id, u64 ticks, bool self)
        {
            for (Summary::Child& at : out.children)
                if (at.self == self && (self || at.id == id))
                {
                    at.ticks += ticks;
                    return;
                }
            Summary::Child child;
            child.name = String(name);
            child.id = id;
            child.ticks = ticks;
            child.self = self;
            out.children.PushBack(core::Move(child));
        }

        // Its calls' durations over every frame held; the frames chosen's calls lit; its longest
        // call in the frames chosen.
        void Spread(const IProfileSource& source, ChosenName chosen, Summary& out) const
        {
            struct Call
            {
                u64 ticks;
                bool inChosen;
            };
            Array<Call> calls;
            const FrameRange frames = m_focus->Frames();
            const FrameRange all = AllFrames(&source);
            u64 longest = 0;
            for (u64 n = all.first; !all.empty && n <= all.last; ++n)
            {
                const FrameRecord* frame = source.Frame(n);
                if (frame == nullptr)
                    continue;
                ++out.heldFrames;
                const bool inChosen = !frames.empty && n >= frames.first && n <= frames.last;
                auto take = [&](u64 begin, u64 end)
                {
                    calls.PushBack(Call{end - begin, inChosen});
                    if (inChosen && end - begin > longest)
                    {
                        longest = end - begin;
                        out.longestBegin = begin;
                        out.longestEnd = end;
                    }
                };
                if (chosen.gpu)
                {
                    for (const GpuPassEvent& pass : frame->gpu)
                        if (pass.name == chosen.name)
                            take(pass.beginTick, pass.endTick);
                }
                else
                    for (const ThreadFrame& thread : frame->threads)
                        for (const ScopeEvent& scope : thread.scopes)
                            if (scope.name == chosen.name)
                                take(scope.beginTick, scope.endTick);
            }
            out.bins.Resize(kBins, 0);
            out.lit.Resize(kBins, 0);
            out.heldCalls = calls.Size();
            if (calls.IsEmpty())
                return;
            out.binLeast = calls[0].ticks;
            out.binMost = calls[0].ticks;
            for (const Call& call : calls)
            {
                out.binLeast = Min(out.binLeast, call.ticks);
                out.binMost = Max(out.binMost, call.ticks);
            }
            const f64 width = static_cast<f64>(Max<u64>(out.binMost - out.binLeast, 1));
            for (const Call& call : calls)
            {
                const usize bin = Min(static_cast<usize>(static_cast<f64>(call.ticks - out.binLeast) / width * static_cast<f64>(kBins)), kBins - 1);
                ++out.bins[bin];
                if (call.inChosen)
                    out.lit[bin] = 1;
            }
        }

        mutable Summary m_summary;
        mutable bool m_dirty = true;
    };
}
