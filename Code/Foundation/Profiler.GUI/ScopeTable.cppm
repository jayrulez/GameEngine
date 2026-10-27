// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Profiler.GUI - :scope_table partition
//
// The frames chosen's scopes as a table (X7 of Specs/editor-gui.md): by default the call tree,
// each thread a group and the GPU's passes one, a scope under the one it ran in (the frames
// merged: a scope's every run in the same place is one row); sorted by any other heading, a flat
// list of names, the longest first (again: the other way). Columns: calls, total and self time
// (its children's taken out), the mean and the longest call, and its share of the frames' time.
// Over a span marked on the timeline, the span's: scopes cut to it.
//
// Mouse first: a click on a row chooses its name (the page's other views told), a click on a
// row's arrow folds or opens it, a click on a heading sorts by it, the wheel scrolls the rows. A
// filter shows the names holding its text (and in the tree, the rows they hang from).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.profiler.gui:scope_table;

import foundation.core;
import foundation.vg;
import foundation.gui;
import foundation.profiler;
import :focus;
import :widget;

using namespace foundation::core;
using namespace foundation::gui;

export namespace foundation::profiler::gui
{
    /// A table's column, its heading's order.
    enum class ScopeColumn : u8
    {
        Name, // the call tree; the others sort a flat list
        Calls,
        Total,
        Self,
        Mean,
        Max,
        Share,
    };

    /// A row of the call tree: a thread's group, or a scope in the place it ran.
    struct ScopeNode
    {
        NameId name = 0;
        bool gpu = false;
        bool group = false; // a thread's (or the GPU's) group: a name, no numbers
        String groupName;
        u32 thread = 0;
        i32 parent = -1;
        u32 depth = 0; // 0 for a group
        u64 calls = 0;
        u64 totalTicks = 0;
        u64 selfTicks = 0;
        u64 maxTicks = 0;
        u64 key = 0; // its place, kept across rebuilds: the fold remembers it by this
        Array<u32> children;
    };

    /// What the call tree is built from: the frames chosen and the span marked, if any.
    struct ScopeTree
    {
        Array<ScopeNode> nodes; // groups first in their order, then scopes as met
        Array<u32> roots;       // the groups
        u64 frameTicks = 0;     // the frames' (or the span's) length, for a scope's share

        /// Builds it from `source` over `frames`, the scopes cut to `span` when it is marked.
        void Build(const IProfileSource& source, FrameRange frames, MarkedSpan span)
        {
            nodes.Clear();
            roots.Clear();
            frameTicks = 0;
            if (frames.empty)
                return;
            Array<u32> order;
            Array<u32> stack;
            for (u64 n = frames.first; n <= frames.last; ++n)
            {
                const FrameRecord* frame = source.Frame(n);
                if (frame == nullptr)
                    continue;
                frameTicks += Cut(frame->beginTick, frame->endTick, span);
                for (const ThreadFrame& thread : frame->threads)
                {
                    if (thread.scopes.IsEmpty())
                        continue;
                    const u32 group = Group(thread.thread, false, source);
                    AddRuns(thread.scopes.AsSpan(), group, false, span, order, stack);
                }
                if (!frame->gpu.IsEmpty())
                {
                    const u32 group = Group(~0u, true, source);
                    Array<ScopeEvent> passes;
                    for (const GpuPassEvent& pass : frame->gpu)
                        passes.PushBack(ScopeEvent{pass.name, pass.depth, pass.beginTick, pass.endTick});
                    AddRuns(passes.AsSpan(), group, true, span, order, stack);
                }
            }
            // Self: a node's total less its children's.
            for (ScopeNode& node : nodes)
            {
                u64 children = 0;
                for (const u32 child : node.children)
                    children += nodes[child].totalTicks;
                node.selfTicks = node.totalTicks - Min(children, node.totalTicks);
            }
            // The GPU's group last; the threads' in their order.
            roots.Sort([this](u32 a, u32 b)
            {
                const ScopeNode& x = nodes[a];
                const ScopeNode& y = nodes[b];
                if (x.gpu != y.gpu)
                    return !x.gpu;
                return x.thread < y.thread;
            });
        }

    private:
        static u64 Cut(u64 begin, u64 end, MarkedSpan span) noexcept
        {
            if (!span.any)
                return end - begin;
            const u64 from = Max(begin, span.beginTick);
            const u64 to = Min(end, span.endTick);
            return to > from ? to - from : 0;
        }

        u32 Group(u32 thread, bool gpu, const IProfileSource& source)
        {
            for (const u32 root : roots)
                if (nodes[root].gpu == gpu && nodes[root].thread == thread)
                    return root;
            ScopeNode group;
            group.group = true;
            group.gpu = gpu;
            group.thread = thread;
            group.key = Mix(0x9E3779B97F4A7C15ull, gpu ? 0xFFFFFFFFull : static_cast<u64>(thread) + 1);
            if (gpu)
                group.groupName = String(u8"GPU");
            else
            {
                for (const ThreadInfo& info : source.Threads())
                    if (info.index == thread)
                        group.groupName = String(info.name.AsView());
                if (group.groupName.IsEmpty())
                    group.groupName = Format(u8"Thread {}", thread);
            }
            nodes.PushBack(core::Move(group));
            roots.PushBack(static_cast<u32>(nodes.Size() - 1));
            return static_cast<u32>(nodes.Size() - 1);
        }

        // A thread's runs of one frame into the tree: in order of their start (the outer first),
        // each under the run it lies in.
        void AddRuns(Span<const ScopeEvent> runs, u32 group, bool gpu, MarkedSpan span, Array<u32>& order, Array<u32>& stack)
        {
            order.Clear();
            for (u32 i = 0; i < runs.Size(); ++i)
                order.PushBack(i);
            order.Sort([runs](u32 a, u32 b)
            {
                return runs[a].beginTick < runs[b].beginTick || (runs[a].beginTick == runs[b].beginTick && runs[a].depth < runs[b].depth);
            });
            stack.Clear(); // the open runs' nodes, by depth
            Array<u32> depths;
            for (const u32 i : order)
            {
                const ScopeEvent& run = runs[i];
                while (!depths.IsEmpty() && depths[depths.Size() - 1] >= run.depth + 1)
                {
                    depths.PopBack();
                    stack.PopBack();
                }
                const u32 parent = stack.IsEmpty() ? group : stack[stack.Size() - 1];
                const u32 node = Child(parent, run.name, gpu);
                const u64 ticks = Cut(run.beginTick, run.endTick, span);
                if (ticks > 0 || !span.any)
                {
                    ScopeNode& at = nodes[node];
                    ++at.calls;
                    at.totalTicks += ticks;
                    at.maxTicks = Max(at.maxTicks, ticks);
                }
                stack.PushBack(node);
                depths.PushBack(run.depth + 1);
            }
        }

        u32 Child(u32 parent, NameId name, bool gpu)
        {
            for (const u32 child : nodes[parent].children)
                if (nodes[child].name == name)
                    return child;
            ScopeNode node;
            node.name = name;
            node.gpu = gpu;
            node.parent = static_cast<i32>(parent);
            node.thread = nodes[parent].thread;
            node.depth = nodes[parent].depth + 1;
            node.key = Mix(nodes[parent].key, static_cast<u64>(name) + 1);
            nodes.PushBack(core::Move(node));
            const u32 at = static_cast<u32>(nodes.Size() - 1);
            nodes[parent].children.PushBack(at);
            return at;
        }

        static u64 Mix(u64 a, u64 b) noexcept
        {
            u64 h = a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2));
            h ^= h >> 31;
            h *= 0xBF58476D1CE4E5B9ull;
            return h ^ (h >> 29);
        }
    };

    class ScopeTable final : public ProfilerWidget
    {
        RTTI_OBJECT(ScopeTable, ProfilerWidget)
    public:
        static constexpr f32 kRowHeight = 22.0f;
        static constexpr f32 kHeadingHeight = 24.0f;
        static constexpr f32 kNumberWidth = 72.0f;
        static constexpr f32 kIndent = 16.0f;
        static constexpr f32 kArrowWidth = 16.0f;
        static constexpr u32 kOpenDepth = 3; // tree rows open to this depth until folded
        static constexpr usize kColumnCount = 7;

        /// A row shown: a tree node, or in the flat list a name's totals.
        struct Row
        {
            i32 node = -1; // the tree's node (the tree), or -1 (the flat list)
            NameId name = 0;
            bool gpu = false;
            bool group = false;
            String label;
            u32 depth = 0;
            bool hasChildren = false;
            bool open = false;
            u64 calls = 0;
            u64 totalTicks = 0;
            u64 selfTicks = 0;
            u64 maxTicks = 0;
        };

        ScopeTable()
        {
            SetTag(StringView(u8"scopetable"));
            SetTabFocusable(false);
        }

        // === What it shows ===
        [[nodiscard]] ScopeColumn SortColumn() const noexcept { return m_sort; }
        [[nodiscard]] bool SortsDescending() const noexcept { return m_descending; }
        /// Sorts by `column` (Name: the tree), longest first unless `descending` is false.
        void SortBy(ScopeColumn column, bool descending = true)
        {
            m_sort = column;
            m_descending = descending;
            m_scroll = 0.0f;
            m_rowsDirty = true;
            Invalidate();
        }
        /// Shows only the names holding `text` (any case); empty: all.
        void SetFilter(StringView text)
        {
            m_filter = String(text);
            m_scroll = 0.0f;
            m_rowsDirty = true;
            Invalidate();
        }
        [[nodiscard]] StringView Filter() const noexcept { return m_filter.AsView(); }

        /// Folds or opens a tree row (by its place).
        void SetOpen(const Row& row, bool open)
        {
            if (row.node < 0)
                return;
            const ScopeNode& node = Tree().nodes[static_cast<usize>(row.node)];
            const bool byDefault = node.group || node.depth < kOpenDepth;
            for (usize i = 0; i < m_toggled.Size(); ++i)
                if (m_toggled[i] == node.key)
                {
                    m_toggled.RemoveAt(i);
                    break;
                }
            if (open != byDefault)
                m_toggled.PushBack(node.key);
            m_rowsDirty = true;
            Invalidate();
        }

        // === The model (the draw and the tests read it) ===
        [[nodiscard]] const ScopeTree& Tree() const
        {
            Refresh();
            return m_tree;
        }
        [[nodiscard]] Span<const Row> Rows() const
        {
            Refresh();
            return m_rows.AsSpan();
        }
        /// The row at a point's y (its own space), or -1.
        [[nodiscard]] i32 RowAt(f32 y) const
        {
            const Rect box = GetContentBounds();
            const f32 at = y - box.y - kHeadingHeight + m_scroll;
            if (y < box.y + kHeadingHeight || at < 0.0f)
                return -1;
            const i32 row = static_cast<i32>(at / kRowHeight);
            return row < static_cast<i32>(Rows().Size()) ? row : -1;
        }
        /// The heading at a point's x (its own space).
        [[nodiscard]] ScopeColumn ColumnAt(f32 x) const
        {
            const Rect box = GetContentBounds();
            const f32 fromEnd = box.Right() - x;
            if (fromEnd < 0.0f || fromEnd >= kNumberWidth * static_cast<f32>(kColumnCount - 1))
                return ScopeColumn::Name;
            return static_cast<ScopeColumn>(kColumnCount - 1 - static_cast<usize>(fromEnd / kNumberWidth));
        }
        /// A row's value in a column (ms, or calls), as the table writes it.
        [[nodiscard]] String CellText(const Row& row, ScopeColumn column) const
        {
            const IProfileSource* source = Source();
            if (row.group || source == nullptr)
                return String();
            const u64 frequency = source->TickFrequency();
            switch (column)
            {
            case ScopeColumn::Name:
                return String(row.label.AsView());
            case ScopeColumn::Calls:
                return Format(u8"{}", row.calls);
            case ScopeColumn::Total:
                return FormatFixed(TicksToMs(row.totalTicks, frequency), 2);
            case ScopeColumn::Self:
                return FormatFixed(TicksToMs(row.selfTicks, frequency), 2);
            case ScopeColumn::Mean:
                return FormatFixed(row.calls > 0 ? TicksToMs(row.totalTicks, frequency) / static_cast<f32>(row.calls) : 0.0f, 2);
            case ScopeColumn::Max:
                return FormatFixed(TicksToMs(row.maxTicks, frequency), 2);
            case ScopeColumn::Share:
            default:
                return Format(u8"{}%", FormatFixed(m_tree.frameTicks > 0 ? 100.0 * static_cast<f64>(row.totalTicks) / static_cast<f64>(m_tree.frameTicks) : 0.0, 1).AsView());
            }
        }

        /// The parts it draws, which the sheet styles as `tag::part`.
        void CollectStyleParts(Array<StringView>& out) const override
        {
            ProfilerWidget::CollectStyleParts(out);
            out.PushBack(StringView(u8"heading"));
            out.PushBack(StringView(u8"sorted"));
            out.PushBack(StringView(u8"separator"));
            out.PushBack(StringView(u8"group"));
            out.PushBack(StringView(u8"fold"));
            out.PushBack(StringView(u8"chosen"));
            out.PushBack(StringView(u8"hover"));
        }

        void OnProfileSourceChanged() override { Dirty(); }
        void OnProfileFramesChanged() override { Dirty(); }
        void OnProfileChoiceChanged() override
        {
            // The span cuts the scopes; the name only lights its rows.
            const MarkedSpan span = m_focus ? m_focus->Span() : MarkedSpan{};
            if (!(span == m_builtSpan))
                Dirty();
            Invalidate();
        }

    protected:
        void OnMouseDown(const MouseEvent& event) override
        {
            if (event.Button != MouseButton::Left)
                return;
            event.Handled = true;
            const core::Float2 at = Local(event.Position);
            const Rect box = GetContentBounds();
            if (at.y < box.y + kHeadingHeight)
            {
                const ScopeColumn column = ColumnAt(at.x);
                SortBy(column, column == m_sort ? !m_descending : true);
                return;
            }
            const i32 index = RowAt(at.y);
            if (index < 0)
            {
                if (m_focus)
                    m_focus->ClearName();
                return;
            }
            const Row& row = m_rows[static_cast<usize>(index)];
            const f32 arrowX = box.x + 6.0f + static_cast<f32>(row.depth) * kIndent;
            if (row.hasChildren && at.x >= arrowX && at.x < arrowX + kArrowWidth)
            {
                SetOpen(row, !row.open);
                return;
            }
            if (row.group)
            {
                SetOpen(row, !row.open);
                return;
            }
            if (m_focus)
                m_focus->ChooseName(row.name, row.gpu);
        }
        void OnMouseMove(const MouseEvent& event) override
        {
            const i32 hovered = RowAt(Local(event.Position).y);
            if (hovered != m_hovered)
            {
                m_hovered = hovered;
                Invalidate();
            }
        }
        void OnMouseLeave(const MouseEvent& event) override
        {
            UIWidget::OnMouseLeave(event);
            m_hovered = -1;
            Invalidate();
        }
        void OnMouseWheel(const WheelEvent& event) override
        {
            event.Handled = true;
            const Rect box = GetContentBounds();
            const f32 most = Max(static_cast<f32>(Rows().Size()) * kRowHeight - (box.height - kHeadingHeight), 0.0f);
            m_scroll = Clamp(m_scroll - event.Delta.y * kRowHeight * 3.0f, 0.0f, most);
            Invalidate();
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            UIWidget::OnDraw(ctx, localBounds);
            Refresh();
            const Rect box = GetContentBounds();
            const Color text = GetTextColor();
            const Color dim = PartTextColor(u8"group");
            const Color heading = PartTextColor(u8"heading");
            const Color separator = PartColor(u8"separator", Color{0.5f, 0.5f, 0.5f, 0.3f});
            const IProfileSource* source = Source();

            // The headings.
            ctx.VG().FillRect(Rectangle{box.x, box.y, box.width, kHeadingHeight}, PartColor(u8"heading", Color{0.0f, 0.0f, 0.0f, 0.12f}));
            static constexpr const char8_t* kHeadings[kColumnCount] = {u8"Name", u8"Calls", u8"Total ms", u8"Self ms", u8"Mean", u8"Max", u8"Frame"};
            for (usize c = 0; c < kColumnCount; ++c)
            {
                const ScopeColumn column = static_cast<ScopeColumn>(c);
                const Rect cell = CellBounds(column, box.y, kHeadingHeight);
                Color color = heading;
                if (column == m_sort)
                    color = PartTextColor(u8"sorted");
                String label(kHeadings[c]);
                if (column == m_sort && column != ScopeColumn::Name)
                    label += StringView(m_descending ? u8" ▾" : u8" ▴");
                DrawLabel(ctx, label.AsView(), cell, color, column == ScopeColumn::Name ? TextHAlign::Start : TextHAlign::End);
            }
            ctx.VG().FillRect(Rectangle{box.x, box.y + kHeadingHeight - 1.0f, box.width, 1.0f}, separator);

            // The rows.
            const Rect rowsClip{box.x, box.y + kHeadingHeight, box.width, Max(box.height - kHeadingHeight, 0.0f)};
            ctx.PushClip(rowsClip);
            const ChosenName chosen = m_focus ? m_focus->Name() : ChosenName{};
            const usize first = static_cast<usize>(m_scroll / kRowHeight);
            for (usize i = first; i < m_rows.Size(); ++i)
            {
                const f32 y = box.y + kHeadingHeight + static_cast<f32>(i) * kRowHeight - m_scroll;
                if (y > box.Bottom())
                    break;
                const Row& row = m_rows[i];
                const Rect line{box.x, y, box.width, kRowHeight};
                if (!row.group && chosen.Is(row.name, row.gpu))
                    ctx.VG().FillRect(line.ToRectangle(), PartColor(u8"chosen", Color{0.18f, 0.43f, 0.87f, 0.35f}));
                else if (static_cast<i32>(i) == m_hovered)
                    ctx.VG().FillRect(line.ToRectangle(), PartColor(u8"hover", Color{1.0f, 1.0f, 1.0f, 0.05f}));
                f32 x = box.x + 6.0f + static_cast<f32>(row.depth) * kIndent;
                if (row.hasChildren)
                    DrawArrow(ctx, core::Float2{x + kArrowWidth * 0.5f, y + kRowHeight * 0.5f}, !row.open, PartColor(u8"fold", dim));
                x += kArrowWidth;
                if (!row.group && source != nullptr)
                {
                    ctx.VG().FillRoundedRect(Rectangle{x, y + kRowHeight * 0.5f - 5.0f, 10.0f, 10.0f}, 2.0f, NameColor(source->NameOf(row.name)));
                    x += 14.0f;
                }
                const Rect name = CellBounds(ScopeColumn::Name, y, kRowHeight);
                DrawLabel(ctx, row.label.AsView(), Rect{x, y, Max(name.Right() - x, 0.0f), kRowHeight}, row.group ? dim : text);
                for (usize c = 1; c < kColumnCount; ++c)
                {
                    const ScopeColumn column = static_cast<ScopeColumn>(c);
                    DrawLabel(ctx, CellText(row, column).AsView(), CellBounds(column, y, kRowHeight), text, TextHAlign::End);
                }
            }
            ctx.PopClip();
        }

    private:
        void Dirty()
        {
            m_treeDirty = true;
            m_rowsDirty = true;
            Invalidate();
        }

        // A column's cell on a line at `y`: the numbers right to left from the end, the name the rest.
        [[nodiscard]] Rect CellBounds(ScopeColumn column, f32 y, f32 height) const
        {
            const Rect box = GetContentBounds();
            const f32 numbers = kNumberWidth * static_cast<f32>(kColumnCount - 1);
            if (column == ScopeColumn::Name)
                return Rect{box.x + 6.0f, y, Max(box.width - numbers - 12.0f, 0.0f), height};
            const usize fromEnd = kColumnCount - 1 - static_cast<usize>(column);
            return Rect{box.Right() - static_cast<f32>(fromEnd + 1) * kNumberWidth, y, kNumberWidth - 8.0f, height};
        }

        void Refresh() const
        {
            const IProfileSource* source = Source();
            if (m_treeDirty)
            {
                m_treeDirty = false;
                m_rowsDirty = true;
                m_builtSpan = m_focus ? m_focus->Span() : MarkedSpan{};
                if (source != nullptr)
                    m_tree.Build(*source, m_focus->Frames(), m_builtSpan);
                else
                    m_tree = ScopeTree{};
            }
            if (!m_rowsDirty)
                return;
            m_rowsDirty = false;
            m_rows.Clear();
            if (source == nullptr)
                return;
            if (m_sort == ScopeColumn::Name)
            {
                for (const u32 root : m_tree.roots)
                    AddTreeRows(root, *source);
                return;
            }
            AddFlatRows(*source);
        }

        [[nodiscard]] bool Matches(StringView name) const
        {
            if (m_filter.IsEmpty())
                return true;
            const StringView filter = m_filter.AsView();
            for (usize i = 0; i + filter.Size() <= name.Size(); ++i)
            {
                bool same = true;
                for (usize j = 0; j < filter.Size() && same; ++j)
                    same = ToLower(name[i + j]) == ToLower(filter[j]);
                if (same)
                    return true;
            }
            return false;
        }
        [[nodiscard]] static char8_t ToLower(char8_t c) noexcept { return c >= u8'A' && c <= u8'Z' ? static_cast<char8_t>(c + 32) : c; }

        // Whether a node, or one under it, holds the filter's text.
        [[nodiscard]] bool Shows(u32 index, const IProfileSource& source) const
        {
            const ScopeNode& node = m_tree.nodes[index];
            if (!node.group && node.calls > 0 && Matches(source.NameOf(node.name)))
                return true; // a node with no call ran only outside the span: shown for its children
            for (const u32 child : node.children)
                if (Shows(child, source))
                    return true;
            return false;
        }

        [[nodiscard]] bool IsOpen(const ScopeNode& node) const
        {
            bool open = node.group || node.depth < kOpenDepth;
            for (const u64 key : m_toggled)
                if (key == node.key)
                    open = !open;
            return open || !m_filter.IsEmpty(); // a filter opens what it shows
        }

        void AddTreeRows(u32 index, const IProfileSource& source) const
        {
            const ScopeNode& node = m_tree.nodes[index];
            if (!Shows(index, source))
                return;
            Row row;
            row.node = static_cast<i32>(index);
            row.name = node.name;
            row.gpu = node.gpu;
            row.group = node.group;
            row.label = node.group ? String(node.groupName.AsView()) : String(source.NameOf(node.name));
            row.depth = node.depth;
            row.hasChildren = !node.children.IsEmpty();
            row.open = IsOpen(node);
            row.calls = node.calls;
            row.totalTicks = node.totalTicks;
            row.selfTicks = node.selfTicks;
            row.maxTicks = node.maxTicks;
            m_rows.PushBack(core::Move(row));
            if (!IsOpen(node))
                return;
            // Children by their total, the longest first.
            Array<u32> children;
            for (const u32 child : node.children)
                children.PushBack(child);
            children.Sort([this](u32 a, u32 b) { return m_tree.nodes[a].totalTicks > m_tree.nodes[b].totalTicks; });
            for (const u32 child : children)
                AddTreeRows(child, source);
        }

        void AddFlatRows(const IProfileSource& source) const
        {
            for (const ScopeNode& node : m_tree.nodes)
            {
                if (node.group || node.calls == 0 || !Matches(source.NameOf(node.name)))
                    continue;
                Row* row = nullptr;
                for (Row& at : m_rows)
                    if (at.name == node.name && at.gpu == node.gpu)
                        row = &at;
                if (row == nullptr)
                {
                    Row added;
                    added.name = node.name;
                    added.gpu = node.gpu;
                    added.label = String(source.NameOf(node.name));
                    m_rows.PushBack(core::Move(added));
                    row = &m_rows[m_rows.Size() - 1];
                }
                row->calls += node.calls;
                row->totalTicks += node.totalTicks;
                row->selfTicks += node.selfTicks;
                row->maxTicks = Max(row->maxTicks, node.maxTicks);
            }
            const ScopeColumn column = m_sort;
            const bool descending = m_descending;
            m_rows.Sort([column, descending](const Row& a, const Row& b)
            {
                const f64 x = Value(a, column);
                const f64 y = Value(b, column);
                if (x != y)
                    return descending ? x > y : x < y;
                return a.label.AsView().Compare(b.label.AsView()) < 0;
            });
        }
        [[nodiscard]] static f64 Value(const Row& row, ScopeColumn column) noexcept
        {
            switch (column)
            {
            case ScopeColumn::Calls:
                return static_cast<f64>(row.calls);
            case ScopeColumn::Self:
                return static_cast<f64>(row.selfTicks);
            case ScopeColumn::Mean:
                return row.calls > 0 ? static_cast<f64>(row.totalTicks) / static_cast<f64>(row.calls) : 0.0;
            case ScopeColumn::Max:
                return static_cast<f64>(row.maxTicks);
            case ScopeColumn::Total:
            case ScopeColumn::Share:
            default:
                return static_cast<f64>(row.totalTicks);
            }
        }

        static void DrawArrow(DrawContext& ctx, core::Float2 at, bool folded, Color color)
        {
            vg::PathBuilder arrow;
            if (folded)
            {
                arrow.MoveTo(at.x - 2.0f, at.y - 4.0f);
                arrow.LineTo(at.x + 3.0f, at.y);
                arrow.LineTo(at.x - 2.0f, at.y + 4.0f);
            }
            else
            {
                arrow.MoveTo(at.x - 4.0f, at.y - 2.0f);
                arrow.LineTo(at.x + 4.0f, at.y - 2.0f);
                arrow.LineTo(at.x, at.y + 3.0f);
            }
            arrow.Close();
            ctx.VG().FillPath(arrow.ToPath(), color);
        }

        ScopeColumn m_sort = ScopeColumn::Name;
        bool m_descending = true;
        String m_filter;
        Array<u64> m_toggled; // nodes folded or opened against their default, by their place
        f32 m_scroll = 0.0f;
        i32 m_hovered = -1;

        mutable ScopeTree m_tree;
        mutable Array<Row> m_rows;
        mutable MarkedSpan m_builtSpan;
        mutable bool m_treeDirty = true;
        mutable bool m_rowsDirty = true;
    };
}
