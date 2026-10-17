// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - module implementation unit: layout (E2 of Specs/gui-lab-evolution.md).
//
// Node's measure and arrange, and the display kinds they run: flex (CSS's algorithm: base sizes,
// lines, grow and shrink with minimums freezing, stretch, justify and align), grid (explicit and
// implicit tracks of lengths, percentages, auto and fr, items placed by line or flowed, spans),
// stack (children over each other, each aligned on its own), dock (children to the sides in order),
// then the out-of-flow children by their insets. One algorithm per kind serves both measuring (the
// content's size within the space available) and arranging (placing the children in the content
// box), so the two can never disagree.

module;
#include "Core/Prelude.h"

module experimental.gui;

namespace core = foundation::core;

namespace experimental::gui
{
    namespace
    {
        [[nodiscard]] bool Scrolls(Overflow o) noexcept { return o == Overflow::Scroll || o == Overflow::Auto; }
        [[nodiscard]] bool InFlow(const Node& node) noexcept
        {
            return node.IsDisplayed() && node.GetLayoutStyle().Placement != Position::Absolute;
        }
        // Both axes sized by the style, not by what is inside: the node's size does not follow its
        // content, so a change inside it stops at it.
        [[nodiscard]] bool HasDefiniteSize(const Node& node) noexcept
        {
            const LayoutStyle& s = node.GetLayoutStyle();
            const auto definite = [](LayoutLength l) { return l.Kind == LayoutLengthKind::Px || l.Kind == LayoutLengthKind::Percent; };
            return definite(s.Width) && definite(s.Height);
        }
        [[nodiscard]] f32 Less(f32 available, f32 by) noexcept
        {
            return IsUnbounded(available) ? kUnbounded : core::Max(0.0f, available - by);
        }
        [[nodiscard]] f32 BaseOf(f32 available) noexcept { return IsUnbounded(available) ? -1.0f : available; }

        // One layout run over a container's children: its content box (W and H, kUnbounded on an
        // axis with no limit), where that box starts, and whether to place the children or only
        // measure them.
        struct Flow
        {
            Node& container;
            const LayoutStyle& style;
            f32 width;
            f32 height;
            core::Float2 origin;
            bool arrange;
        };

        // A child's margin-box size within the space given (the size its own style and content
        // want, kept within the space less its margins).
        [[nodiscard]] core::Float2 MeasureItem(Node& child, f32 availW, f32 availH, core::Float2 percentBase)
        {
            const Thickness m = child.GetMargin();
            LayoutConstraints c;
            c.Max = core::Float2{Less(availW, m.TotalHorizontal()), Less(availH, m.TotalVertical())};
            c.PercentBase = percentBase;
            const core::Float2 border = child.Measure(c);
            return core::Float2{border.x + m.TotalHorizontal(), border.y + m.TotalVertical()};
        }
        // The border-box size of a child given its size on some axes (tight) and a limit on the
        // others.
        [[nodiscard]] core::Float2 MeasureBorder(Node& child, Optional<f32> w, Optional<f32> h, f32 maxW, f32 maxH,
                                                 core::Float2 percentBase)
        {
            LayoutConstraints c;
            c.Min = core::Float2{w.HasValue() ? w.Value() : 0.0f, h.HasValue() ? h.Value() : 0.0f};
            c.Max = core::Float2{w.HasValue() ? w.Value() : maxW, h.HasValue() ? h.Value() : maxH};
            c.PercentBase = percentBase;
            return child.Measure(c);
        }
        void Place(Flow& f, Node& child, f32 x, f32 y, f32 w, f32 h)
        {
            if (f.arrange)
                child.Arrange(Rect{f.origin.x + x, f.origin.y + y, core::Max(0.0f, w), core::Max(0.0f, h)});
        }
        [[nodiscard]] Align SelfAlign(Align self, Align container) noexcept { return self == Align::Auto ? container : self; }

        // The in-flow children in their `order` (stable).
        [[nodiscard]] Array<Node*> FlowChildren(Node& container)
        {
            Array<Node*> items;
            for (usize i = 0; i < container.ChildCount(); ++i)
                if (Node* child = container.GetChildAt(i); child != nullptr && InFlow(*child))
                    items.PushBack(child);
            for (usize i = 1; i < items.Size(); ++i) // insertion sort: stable, and lists are short
                for (usize j = i; j > 0 && items[j]->GetLayoutStyle().Order < items[j - 1]->GetLayoutStyle().Order; --j)
                    core::Swap(items[j], items[j - 1]);
            return items;
        }

        // Where `count` items of total `used` along `free` space start and how far apart they are.
        void Distribute(Justify justify, f32 free, usize count, f32& start, f32& between)
        {
            start = 0.0f;
            between = 0.0f;
            if (free <= 0.0f || count == 0)
            {
                if (justify == Justify::End)
                    start = free;
                else if (justify == Justify::Center)
                    start = free * 0.5f;
                return;
            }
            switch (justify)
            {
            case Justify::End: start = free; break;
            case Justify::Center: start = free * 0.5f; break;
            case Justify::SpaceBetween: between = count > 1 ? free / static_cast<f32>(count - 1) : 0.0f; break;
            case Justify::SpaceAround:
                between = free / static_cast<f32>(count);
                start = between * 0.5f;
                break;
            case Justify::SpaceEvenly:
                between = free / static_cast<f32>(count + 1);
                start = between;
                break;
            default: break;
            }
        }

        // === Flex ===

        struct FlexItem
        {
            Node* node = nullptr;
            Thickness margin{};
            f32 marginMain = 0.0f;
            f32 marginCross = 0.0f;
            f32 base = 0.0f;  // the flex base size (border box)
            f32 main = 0.0f;  // resolved
            f32 cross = 0.0f; // border box
            f32 baseline = 0.0f; // down from its margin box's top, when it aligns by baseline
            f32 minMain = 0.0f;
            f32 maxMain = kUnbounded;
            bool minMainKnown = false;
            bool frozen = false;
            bool stretch = false;
            Align align = Align::Start;
        };
        struct FlexLine
        {
            usize start = 0;
            usize count = 0;
            f32 cross = 0.0f;
            f32 baseline = 0.0f; // where its baseline-aligned items' baselines meet, down from its top
        };

        [[nodiscard]] core::Float2 Flex(Flow& f)
        {
            const LayoutStyle& s = f.style;
            const bool row = s.Direction == FlexDirection::Row || s.Direction == FlexDirection::RowReverse;
            const bool reverse = s.Direction == FlexDirection::RowReverse || s.Direction == FlexDirection::ColumnReverse;
            const f32 mainAvail = row ? f.width : f.height;
            const f32 crossAvail = row ? f.height : f.width;
            const bool mainDefinite = !IsUnbounded(mainAvail);
            const bool crossDefinite = !IsUnbounded(crossAvail);
            const f32 mainGap = row ? s.ColumnGap : s.RowGap;
            const f32 crossGap = row ? s.RowGap : s.ColumnGap;
            const core::Float2 percentBase{BaseOf(f.width), BaseOf(f.height)};
            const auto mainOf = [row](core::Float2 v) { return row ? v.x : v.y; };
            const auto crossOf = [row](core::Float2 v) { return row ? v.y : v.x; };

            Array<FlexItem> items;
            for (Node* child : FlowChildren(f.container))
            {
                const LayoutStyle& cs = child->GetLayoutStyle();
                FlexItem item;
                item.node = child;
                item.margin = child->GetMargin();
                item.marginMain = row ? item.margin.TotalHorizontal() : item.margin.TotalVertical();
                item.marginCross = row ? item.margin.TotalVertical() : item.margin.TotalHorizontal();
                item.align = SelfAlign(cs.AlignSelf, s.AlignItems);
                const LayoutLength crossSize = row ? cs.Height : cs.Width;
                item.stretch = item.align == Align::Stretch && crossSize.IsAuto();
                // The base size: flex-basis, else the main size its style gives, else its content's
                // (max-content along the main axis, at the cross size it will have when stretched).
                const Optional<f32> basis = cs.Basis.Resolve(BaseOf(mainAvail));
                if (basis.HasValue())
                    item.base = basis.Value();
                else
                {
                    const f32 crossLimit = Less(crossAvail, item.marginCross);
                    const Optional<f32> crossTight = item.stretch && crossDefinite ? Optional<f32>(crossLimit) : Optional<f32>{};
                    const core::Float2 b = row ? MeasureBorder(*child, {}, crossTight, kUnbounded, crossLimit, percentBase)
                                               : MeasureBorder(*child, crossTight, {}, crossLimit, kUnbounded, percentBase);
                    item.base = mainOf(b);
                }
                const LayoutLength minL = row ? cs.MinWidth : cs.MinHeight;
                const LayoutLength maxL = row ? cs.MaxWidth : cs.MaxHeight;
                if (const Optional<f32> mn = minL.Resolve(BaseOf(mainAvail)); mn.HasValue())
                {
                    item.minMain = mn.Value();
                    item.minMainKnown = true;
                }
                if (const Optional<f32> mx = maxL.Resolve(BaseOf(mainAvail)); mx.HasValue())
                    item.maxMain = mx.Value();
                items.PushBack(item);
            }

            // Lines: break where the next item does not fit, when wrapping into a known length.
            Array<FlexLine> lines;
            {
                FlexLine line;
                f32 used = 0.0f;
                for (usize i = 0; i < items.Size(); ++i)
                {
                    const f32 size = items[i].base + items[i].marginMain;
                    const f32 needed = line.count == 0 ? size : used + mainGap + size;
                    if (s.Wrap != FlexWrap::NoWrap && mainDefinite && line.count > 0 && needed > mainAvail + 0.5f)
                    {
                        lines.PushBack(line);
                        line = FlexLine{i, 0, 0.0f};
                        used = 0.0f;
                    }
                    used = line.count == 0 ? size : used + mainGap + size;
                    ++line.count;
                }
                if (line.count > 0 || lines.IsEmpty())
                    lines.PushBack(line);
            }

            f32 contentMain = 0.0f;
            for (FlexLine& line : lines)
            {
                // Grow into free space, or shrink out of the overflow (each in proportion to its
                // shrink times its base size, frozen at its minimum: its content's when auto).
                f32 used = mainGap * static_cast<f32>(line.count > 0 ? line.count - 1 : 0);
                for (usize i = line.start; i < line.start + line.count; ++i)
                {
                    items[i].main = items[i].base;
                    items[i].frozen = false;
                    used += items[i].base + items[i].marginMain;
                }
                if (mainDefinite)
                {
                    const f32 free = mainAvail - used;
                    if (free > 0.0f)
                    {
                        f32 grow = 0.0f;
                        for (usize i = line.start; i < line.start + line.count; ++i)
                            grow += items[i].node->GetLayoutStyle().Grow;
                        if (grow > 0.0f)
                            for (usize i = line.start; i < line.start + line.count; ++i)
                                items[i].main += free * items[i].node->GetLayoutStyle().Grow / grow;
                    }
                    else if (free < 0.0f)
                    {
                        for (usize round = 0; round <= line.count; ++round)
                        {
                            f32 fixed = mainGap * static_cast<f32>(line.count > 0 ? line.count - 1 : 0);
                            f32 scaled = 0.0f;
                            for (usize i = line.start; i < line.start + line.count; ++i)
                            {
                                FlexItem& item = items[i];
                                const f32 shrink = item.node->GetLayoutStyle().Shrink;
                                if (item.frozen || shrink <= 0.0f)
                                    fixed += item.main + item.marginMain;
                                else
                                {
                                    fixed += item.base + item.marginMain;
                                    scaled += shrink * item.base;
                                }
                            }
                            const f32 overflow = fixed - mainAvail;
                            if (overflow <= 0.0f || scaled <= 0.0f)
                                break;
                            bool froze = false;
                            for (usize i = line.start; i < line.start + line.count; ++i)
                            {
                                FlexItem& item = items[i];
                                const f32 shrink = item.node->GetLayoutStyle().Shrink;
                                if (item.frozen || shrink <= 0.0f)
                                    continue;
                                if (!item.minMainKnown) // min auto: the content's minimum, no more than its specified size
                                {
                                    Node& n = *item.node;
                                    const Thickness chrome = n.GetChrome();
                                    const core::Float2 content = n.MeasureContent(row ? core::Float2{0.0f, Less(crossAvail, item.marginCross + chrome.TotalVertical())}
                                                                                      : core::Float2{Less(crossAvail, item.marginCross + chrome.TotalHorizontal()), 0.0f});
                                    f32 minContent = row ? content.x + chrome.TotalHorizontal() : content.y + chrome.TotalVertical();
                                    const LayoutStyle& ns = n.GetLayoutStyle();
                                    if (const Optional<f32> specified = (row ? ns.Width : ns.Height).Resolve(BaseOf(mainAvail)); specified.HasValue())
                                        minContent = core::Min(minContent, specified.Value());
                                    item.minMain = minContent;
                                    item.minMainKnown = true;
                                }
                                item.main = item.base - overflow * shrink * item.base / scaled;
                                if (item.main <= item.minMain)
                                {
                                    item.main = item.minMain;
                                    item.frozen = true;
                                    froze = true;
                                }
                            }
                            if (!froze)
                                break;
                        }
                    }
                }
                f32 lineMain = mainGap * static_cast<f32>(line.count > 0 ? line.count - 1 : 0);
                line.cross = 0.0f;
                f32 aboveBaseline = 0.0f; // the baseline-aligned items' reach above and below it
                f32 belowBaseline = 0.0f;
                for (usize i = line.start; i < line.start + line.count; ++i)
                {
                    FlexItem& item = items[i];
                    item.main = core::Clamp(item.main, item.minMainKnown ? core::Min(item.minMain, item.main) : 0.0f, item.maxMain);
                    if (item.node->GetLayoutStyle().MinWidth.Kind == LayoutLengthKind::Px && row)
                        item.main = core::Max(item.main, item.node->GetLayoutStyle().MinWidth.Value);
                    if (item.node->GetLayoutStyle().MinHeight.Kind == LayoutLengthKind::Px && !row)
                        item.main = core::Max(item.main, item.node->GetLayoutStyle().MinHeight.Value);
                    // Its cross size at that main size (text wraps to the width it was given).
                    const f32 crossLimit = Less(crossAvail, item.marginCross);
                    const core::Float2 b = row ? MeasureBorder(*item.node, item.main, {}, item.main, crossLimit, percentBase)
                                               : MeasureBorder(*item.node, {}, item.main, crossLimit, item.main, percentBase);
                    item.cross = crossOf(b);
                    line.cross = core::Max(line.cross, item.cross + item.marginCross);
                    lineMain += item.main + item.marginMain;
                    // Baselines line up across a row; in a column, baseline is start (as in CSS).
                    if (row && item.align == Align::Baseline)
                    {
                        item.baseline = item.margin.Top + item.node->BaselineAt(core::Float2{item.main, item.cross});
                        aboveBaseline = core::Max(aboveBaseline, item.baseline);
                        belowBaseline = core::Max(belowBaseline, item.cross + item.marginCross - item.baseline);
                    }
                }
                line.baseline = aboveBaseline;
                line.cross = core::Max(line.cross, aboveBaseline + belowBaseline);
                contentMain = core::Max(contentMain, lineMain);
            }
            if (lines.Size() == 1 && crossDefinite)
                lines[0].cross = crossAvail; // one line is as deep as the container
            f32 contentCross = crossGap * static_cast<f32>(lines.Size() > 0 ? lines.Size() - 1 : 0);
            for (const FlexLine& line : lines)
                contentCross += line.cross;

            if (f.arrange)
            {
                // Lines across: align-content shares the free cross space (stretch: each line
                // grows by the same).
                f32 crossStart = 0.0f;
                f32 crossBetween = 0.0f;
                if (crossDefinite && lines.Size() > 1)
                {
                    const f32 free = crossAvail - contentCross;
                    if (s.AlignContent == Justify::Stretch && free > 0.0f)
                        for (FlexLine& line : lines)
                            line.cross += free / static_cast<f32>(lines.Size());
                    else
                        Distribute(s.AlignContent, free, lines.Size(), crossStart, crossBetween);
                }
                f32 crossPos = crossStart;
                const f32 mainExtent = mainDefinite ? mainAvail : contentMain;
                for (usize li = 0; li < lines.Size(); ++li)
                {
                    const FlexLine& line = lines[s.Wrap == FlexWrap::WrapReverse ? lines.Size() - 1 - li : li];
                    f32 used = mainGap * static_cast<f32>(line.count > 0 ? line.count - 1 : 0);
                    for (usize i = line.start; i < line.start + line.count; ++i)
                        used += items[i].main + items[i].marginMain;
                    f32 start = 0.0f;
                    f32 between = 0.0f;
                    Distribute(s.JustifyContent, mainDefinite ? mainAvail - used : 0.0f, line.count, start, between);
                    f32 mainPos = start;
                    for (usize k = 0; k < line.count; ++k)
                    {
                        FlexItem& item = items[line.start + k];
                        f32 cross = item.cross;
                        f32 crossOffset = 0.0f;
                        const f32 room = line.cross - item.marginCross;
                        switch (item.align)
                        {
                        case Align::End: crossOffset = room - cross; break;
                        case Align::Center: crossOffset = (room - cross) * 0.5f; break;
                        case Align::Stretch:
                            if (item.stretch)
                            {
                                const LayoutStyle& cs = item.node->GetLayoutStyle();
                                const Optional<f32> mx = (row ? cs.MaxHeight : cs.MaxWidth).Resolve(BaseOf(crossAvail));
                                cross = mx.HasValue() ? core::Min(room, mx.Value()) : room;
                            }
                            break;
                        case Align::Baseline:
                            if (row)
                                crossOffset = line.baseline - item.baseline;
                            break;
                        default: break;
                        }
                        const f32 mainEdge = reverse ? mainExtent - mainPos - item.main - item.marginMain : mainPos;
                        const f32 mainAt = mainEdge + (row ? item.margin.Left : item.margin.Top);
                        const f32 crossAt = crossPos + crossOffset + (row ? item.margin.Top : item.margin.Left);
                        if (row)
                            Place(f, *item.node, mainAt, crossAt, item.main, cross);
                        else
                            Place(f, *item.node, crossAt, mainAt, cross, item.main);
                        mainPos += item.main + item.marginMain + mainGap + between;
                    }
                    crossPos += line.cross + crossGap + crossBetween;
                }
            }
            return row ? core::Float2{contentMain, contentCross} : core::Float2{contentCross, contentMain};
        }

        // === Grid ===

        struct GridItem
        {
            Node* node = nullptr;
            i32 column = 0;
            i32 row = 0;
            i32 columnSpan = 1;
            i32 rowSpan = 1;
        };

        // Places items on the grid: those with both lines where they say, the rest flowed into the
        // first cells free (row by row, or column by column). Returns the track counts.
        void PlaceGridItems(const LayoutStyle& s, Array<GridItem>& items, i32& columns, i32& rows)
        {
            const bool byRow = s.AutoFlow == GridAutoFlow::Row;
            columns = core::Max(1, static_cast<i32>(s.Columns.Size()));
            rows = core::Max(0, static_cast<i32>(s.Rows.Size()));
            for (const GridItem& item : items)
            {
                if (item.column > 0)
                    columns = core::Max(columns, item.column + item.columnSpan - 1);
                if (byRow)
                    columns = core::Max(columns, item.columnSpan);
            }
            Array<bool> taken; // rows x columns (byRow) or columns x rows
            const i32 across = byRow ? columns : core::Max(1, rows);
            i32 along = 0;
            const auto cell = [&](i32 a, i32 b) -> bool& { return taken[static_cast<usize>(a * across + b)]; };
            const auto grow = [&](i32 to)
            {
                while (along < to)
                {
                    for (i32 b = 0; b < across; ++b)
                        taken.PushBack(false);
                    ++along;
                }
            };
            const auto fits = [&](i32 a, i32 b, i32 spanA, i32 spanB) -> bool
            {
                if (b + spanB > across)
                    return false;
                grow(a + spanA);
                for (i32 i = a; i < a + spanA; ++i)
                    for (i32 j = b; j < b + spanB; ++j)
                        if (cell(i, j))
                            return false;
                return true;
            };
            const auto take = [&](i32 a, i32 b, i32 spanA, i32 spanB)
            {
                grow(a + spanA);
                for (i32 i = a; i < a + spanA; ++i)
                    for (i32 j = b; j < b + spanB; ++j)
                        cell(i, j) = true;
            };
            i32 cursorA = 0, cursorB = 0;
            for (GridItem& item : items)
            {
                i32& a = byRow ? item.row : item.column;    // along the flow
                i32& b = byRow ? item.column : item.row;    // across it
                const i32 spanA = byRow ? item.rowSpan : item.columnSpan;
                const i32 spanB = core::Min(byRow ? item.columnSpan : item.rowSpan, across);
                if (a > 0 && b > 0)
                {
                    --a;
                    --b;
                    take(a, b, spanA, spanB);
                }
                else if (b > 0) // its cross line given: the first free place along it
                {
                    --b;
                    i32 at = 0;
                    while (!fits(at, b, spanA, spanB))
                        ++at;
                    a = at;
                    take(a, b, spanA, spanB);
                }
                else if (a > 0) // its line along the flow given: the first free cell in it
                {
                    --a;
                    i32 at = 0;
                    while (at < across && !fits(a, at, spanA, spanB))
                        ++at;
                    b = core::Min(at, across - spanB);
                    take(a, b, spanA, spanB);
                }
                else
                {
                    while (!fits(cursorA, cursorB, spanA, spanB))
                    {
                        if (++cursorB + spanB > across)
                        {
                            cursorB = 0;
                            ++cursorA;
                        }
                    }
                    a = cursorA;
                    b = cursorB;
                    take(a, b, spanA, spanB);
                    cursorB += spanB;
                }
            }
            if (byRow)
                rows = core::Max(rows, along);
            else
                columns = core::Max(columns, along);
        }

        // Sizes one axis's tracks: fixed and percentage tracks at their length, auto tracks at their
        // single-span items' largest size, fr tracks sharing what is left (or at their content when
        // the axis has no limit); items spanning auto tracks widen them to fit.
        [[nodiscard]] Array<f32> SizeTracks(const Array<GridTrack>& explicitTracks, const GridTrack& implicit, i32 count,
                                            f32 available, f32 gap, const Array<GridItem>& items, bool columns,
                                            const core::Function<f32(const GridItem&)>& contentOf)
        {
            Array<f32> sizes;
            Array<GridTrack> tracks;
            for (i32 i = 0; i < count; ++i)
                tracks.PushBack(i < static_cast<i32>(explicitTracks.Size()) ? explicitTracks[static_cast<usize>(i)] : implicit);
            const f32 base = BaseOf(available);
            f32 fixedTotal = gap * static_cast<f32>(count > 0 ? count - 1 : 0);
            f32 frTotal = 0.0f;
            for (const GridTrack& t : tracks)
            {
                f32 size = 0.0f;
                if (t.Fr <= 0.0f)
                {
                    const Optional<f32> lo = t.Min.Resolve(base);
                    const Optional<f32> hi = t.Max.Resolve(base);
                    if (lo.HasValue() && hi.HasValue() && lo.Value() == hi.Value())
                        size = lo.Value();
                    else
                        size = lo.HasValue() ? lo.Value() : 0.0f;
                }
                sizes.PushBack(size);
            }
            const auto spanOf = [columns](const GridItem& item) { return columns ? item.columnSpan : item.rowSpan; };
            const auto startOf = [columns](const GridItem& item) { return columns ? item.column : item.row; };
            const auto contentSized = [&](usize i)
            {
                const GridTrack& t = tracks[i];
                if (t.Fr > 0.0f)
                    return IsUnbounded(available);
                const Optional<f32> lo = t.Min.Resolve(base);
                const Optional<f32> hi = t.Max.Resolve(base);
                return !(lo.HasValue() && hi.HasValue() && lo.Value() == hi.Value());
            };
            // Content-sized tracks from their single-span items.
            for (const GridItem& item : items)
                if (spanOf(item) == 1)
                {
                    const usize i = static_cast<usize>(startOf(item));
                    if (i < sizes.Size() && contentSized(i))
                        sizes[i] = core::Max(sizes[i], contentOf(item));
                }
            for (usize i = 0; i < tracks.Size(); ++i)
            {
                if (tracks[i].Fr > 0.0f && !IsUnbounded(available))
                    frTotal += tracks[i].Fr;
                else
                    fixedTotal += sizes[i];
            }
            if (frTotal > 0.0f)
            {
                const f32 free = core::Max(0.0f, available - fixedTotal);
                for (usize i = 0; i < tracks.Size(); ++i)
                    if (tracks[i].Fr > 0.0f)
                    {
                        const Optional<f32> lo = tracks[i].Min.Resolve(base);
                        sizes[i] = core::Max(lo.HasValue() ? lo.Value() : 0.0f, free * tracks[i].Fr / frTotal);
                    }
            }
            // Items spanning several tracks widen the content-sized ones among them to fit.
            for (const GridItem& item : items)
                if (spanOf(item) > 1)
                {
                    const usize first = static_cast<usize>(startOf(item));
                    const usize last = core::Min(sizes.Size(), first + static_cast<usize>(spanOf(item)));
                    f32 have = gap * static_cast<f32>(last - first - 1);
                    usize flexible = 0;
                    for (usize i = first; i < last; ++i)
                    {
                        have += sizes[i];
                        flexible += contentSized(i) ? 1u : 0u;
                    }
                    const f32 need = contentOf(item) - have;
                    if (need > 0.0f && flexible > 0)
                        for (usize i = first; i < last; ++i)
                            if (contentSized(i))
                                sizes[i] += need / static_cast<f32>(flexible);
                }
            return sizes;
        }

        [[nodiscard]] core::Float2 Grid(Flow& f)
        {
            const LayoutStyle& s = f.style;
            const core::Float2 percentBase{BaseOf(f.width), BaseOf(f.height)};
            Array<GridItem> items;
            for (Node* child : FlowChildren(f.container))
            {
                const LayoutStyle& cs = child->GetLayoutStyle();
                items.PushBack(GridItem{child, cs.Column.Start, cs.Row.Start, core::Max(1, cs.Column.Span), core::Max(1, cs.Row.Span)});
            }
            i32 columns = 0, rows = 0;
            PlaceGridItems(s, items, columns, rows);
            const Array<f32> widths = SizeTracks(s.Columns, s.AutoColumns, columns, f.width, s.ColumnGap, items, true,
                                                 [&](const GridItem& item) { return MeasureItem(*item.node, kUnbounded, kUnbounded, percentBase).x; });
            const auto areaWidth = [&](const GridItem& item)
            {
                f32 w = s.ColumnGap * static_cast<f32>(item.columnSpan - 1);
                for (i32 i = item.column; i < item.column + item.columnSpan && i < columns; ++i)
                    w += widths[static_cast<usize>(i)];
                return w;
            };
            const Array<f32> heights = SizeTracks(s.Rows, s.AutoRows, rows, f.height, s.RowGap, items, false,
                                                  [&](const GridItem& item) { return MeasureItem(*item.node, areaWidth(item), kUnbounded, percentBase).y; });
            f32 contentW = s.ColumnGap * static_cast<f32>(columns > 0 ? columns - 1 : 0);
            f32 contentH = s.RowGap * static_cast<f32>(rows > 0 ? rows - 1 : 0);
            for (f32 w : widths)
                contentW += w;
            for (f32 h : heights)
                contentH += h;
            if (f.arrange)
            {
                Array<f32> xs, ys;
                f32 at = 0.0f;
                for (f32 w : widths)
                {
                    xs.PushBack(at);
                    at += w + s.ColumnGap;
                }
                at = 0.0f;
                for (f32 h : heights)
                {
                    ys.PushBack(at);
                    at += h + s.RowGap;
                }
                for (const GridItem& item : items)
                {
                    const LayoutStyle& cs = item.node->GetLayoutStyle();
                    const Thickness m = item.node->GetMargin();
                    const f32 areaW = areaWidth(item);
                    f32 areaH = s.RowGap * static_cast<f32>(item.rowSpan - 1);
                    for (i32 i = item.row; i < item.row + item.rowSpan && i < rows; ++i)
                        areaH += heights[static_cast<usize>(i)];
                    const Align jx = SelfAlign(cs.JustifySelf, s.JustifyItems);
                    const Align ay = SelfAlign(cs.AlignSelf, s.AlignItems);
                    const f32 roomW = core::Max(0.0f, areaW - m.TotalHorizontal());
                    const f32 roomH = core::Max(0.0f, areaH - m.TotalVertical());
                    const bool fillW = jx == Align::Stretch && cs.Width.IsAuto();
                    const bool fillH = ay == Align::Stretch && cs.Height.IsAuto();
                    const core::Float2 b = MeasureBorder(*item.node, fillW ? Optional<f32>(roomW) : Optional<f32>{},
                                                         fillH ? Optional<f32>(roomH) : Optional<f32>{}, roomW, roomH, percentBase);
                    const auto offset = [](Align a, f32 room, f32 size)
                    {
                        return a == Align::End ? room - size : a == Align::Center ? (room - size) * 0.5f : 0.0f;
                    };
                    const usize ci = static_cast<usize>(item.column < columns ? item.column : 0);
                    const usize ri = static_cast<usize>(item.row < rows ? item.row : 0);
                    Place(f, *item.node, xs[ci] + m.Left + offset(jx, roomW, b.x), ys[ri] + m.Top + offset(ay, roomH, b.y), b.x, b.y);
                }
            }
            return core::Float2{contentW, contentH};
        }

        // === Stack ===

        [[nodiscard]] core::Float2 Stack(Flow& f)
        {
            const LayoutStyle& s = f.style;
            const core::Float2 percentBase{BaseOf(f.width), BaseOf(f.height)};
            core::Float2 content{0.0f, 0.0f};
            const Array<Node*> children = FlowChildren(f.container);
            for (Node* child : children)
            {
                const core::Float2 size = MeasureItem(*child, f.width, f.height, percentBase);
                content.x = core::Max(content.x, size.x);
                content.y = core::Max(content.y, size.y);
            }
            if (f.arrange)
            {
                const f32 w = IsUnbounded(f.width) ? content.x : f.width;
                const f32 h = IsUnbounded(f.height) ? content.y : f.height;
                for (Node* child : children)
                {
                    const LayoutStyle& cs = child->GetLayoutStyle();
                    const Thickness m = child->GetMargin();
                    const Align jx = SelfAlign(cs.JustifySelf, s.JustifyItems);
                    const Align ay = SelfAlign(cs.AlignSelf, s.AlignItems);
                    const f32 roomW = core::Max(0.0f, w - m.TotalHorizontal());
                    const f32 roomH = core::Max(0.0f, h - m.TotalVertical());
                    const bool fillW = jx == Align::Stretch && cs.Width.IsAuto();
                    const bool fillH = ay == Align::Stretch && cs.Height.IsAuto();
                    const core::Float2 b = MeasureBorder(*child, fillW ? Optional<f32>(roomW) : Optional<f32>{},
                                                         fillH ? Optional<f32>(roomH) : Optional<f32>{}, roomW, roomH, percentBase);
                    const auto offset = [](Align a, f32 room, f32 size)
                    {
                        return a == Align::End ? room - size : a == Align::Center ? (room - size) * 0.5f : 0.0f;
                    };
                    Place(f, *child, m.Left + offset(jx, roomW, b.x), m.Top + offset(ay, roomH, b.y), b.x, b.y);
                }
            }
            return content;
        }

        // === Dock ===

        [[nodiscard]] core::Float2 DockChildren(Flow& f)
        {
            const core::Float2 percentBase{BaseOf(f.width), BaseOf(f.height)};
            f32 left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f; // what the docked children took
            f32 needW = 0.0f, needH = 0.0f;
            for (Node* child : FlowChildren(f.container))
            {
                const Dock side = child->GetLayoutStyle().DockSide;
                const f32 availW = Less(f.width, left + right);
                const f32 availH = Less(f.height, top + bottom);
                const Thickness m = child->GetMargin();
                const bool horizontal = side == Dock::Left || side == Dock::Right;
                const bool vertical = side == Dock::Top || side == Dock::Bottom;
                // Docked to a side, it fills that side's length; filling, it takes what is left.
                const Optional<f32> tightW = (vertical || side == Dock::Fill) && !IsUnbounded(availW) ? Optional<f32>(Less(availW, m.TotalHorizontal())) : Optional<f32>{};
                const Optional<f32> tightH = (horizontal || side == Dock::Fill) && !IsUnbounded(availH) ? Optional<f32>(Less(availH, m.TotalVertical())) : Optional<f32>{};
                const core::Float2 b = MeasureBorder(*child, child->GetLayoutStyle().Width.IsAuto() ? tightW : Optional<f32>{},
                                                     child->GetLayoutStyle().Height.IsAuto() ? tightH : Optional<f32>{},
                                                     Less(availW, m.TotalHorizontal()), Less(availH, m.TotalVertical()), percentBase);
                const f32 w = b.x + m.TotalHorizontal();
                const f32 h = b.y + m.TotalVertical();
                switch (side)
                {
                case Dock::Left:
                    Place(f, *child, left + m.Left, top + m.Top, b.x, b.y);
                    needH = core::Max(needH, top + bottom + h);
                    left += w;
                    break;
                case Dock::Right:
                    Place(f, *child, (IsUnbounded(f.width) ? left : f.width - right - w) + m.Left, top + m.Top, b.x, b.y);
                    needH = core::Max(needH, top + bottom + h);
                    right += w;
                    break;
                case Dock::Top:
                    Place(f, *child, left + m.Left, top + m.Top, b.x, b.y);
                    needW = core::Max(needW, left + right + w);
                    top += h;
                    break;
                case Dock::Bottom:
                    Place(f, *child, left + m.Left, (IsUnbounded(f.height) ? top : f.height - bottom - h) + m.Top, b.x, b.y);
                    needW = core::Max(needW, left + right + w);
                    bottom += h;
                    break;
                case Dock::Fill:
                    Place(f, *child, left + m.Left, top + m.Top, b.x, b.y);
                    needW = core::Max(needW, left + right + w);
                    needH = core::Max(needH, top + bottom + h);
                    break;
                }
            }
            return core::Float2{core::Max(needW, left + right), core::Max(needH, top + bottom)};
        }

        [[nodiscard]] core::Float2 RunFlow(Flow& f)
        {
            switch (f.style.Kind)
            {
            case Display::Grid: return Grid(f);
            case Display::Stack: return Stack(f);
            case Display::Dock: return DockChildren(f);
            case Display::None: return core::Float2{0.0f, 0.0f};
            default: return Flex(f);
            }
        }
    }

    u32& Node::LayoutRunCounter() noexcept
    {
        thread_local u32 counter = 0;
        return counter;
    }

    core::Float2 Node::Measure(const LayoutConstraints& constraints)
    {
        if (m_measureValid && constraints == m_measureKey)
            return m_measured;
        const LayoutStyle& s = m_layoutStyle;
        core::Float2 size{0.0f, 0.0f};
        if (s.Kind != Display::None)
        {
            const Thickness chrome = GetChrome();
            const f32 ch = chrome.TotalHorizontal();
            const f32 cv = chrome.TotalVertical();
            const bool contentBox = s.Sizing == BoxSizing::ContentBox;
            const auto box = [contentBox](LayoutLength l, f32 base, f32 chromeAxis) -> Optional<f32>
            {
                Optional<f32> v = l.Resolve(base);
                if (v.HasValue() && contentBox)
                    return v.Value() + chromeAxis;
                return v;
            };
            const Optional<f32> w = box(s.Width, constraints.PercentBase.x, ch);
            const Optional<f32> h = box(s.Height, constraints.PercentBase.y, cv);
            const f32 minW = box(s.MinWidth, constraints.PercentBase.x, ch).ValueOr(0.0f);
            const f32 minH = box(s.MinHeight, constraints.PercentBase.y, cv).ValueOr(0.0f);
            const f32 maxW = core::Max(minW, box(s.MaxWidth, constraints.PercentBase.x, ch).ValueOr(kUnbounded));
            const f32 maxH = core::Max(minH, box(s.MaxHeight, constraints.PercentBase.y, cv).ValueOr(kUnbounded));
            if (w.HasValue() && h.HasValue())
                size = core::Float2{w.Value(), h.Value()};
            else
            {
                f32 availW = w.HasValue() ? w.Value() : core::Min(constraints.Max.x, maxW);
                f32 availH = h.HasValue() ? h.Value() : core::Min(constraints.Max.y, maxH);
                if (s.Width.Kind == LayoutLengthKind::MaxContent)
                    availW = kUnbounded;
                else if (s.Width.Kind == LayoutLengthKind::MinContent)
                    availW = ch;
                if (s.Height.Kind == LayoutLengthKind::MaxContent)
                    availH = kUnbounded;
                else if (s.Height.Kind == LayoutLengthKind::MinContent)
                    availH = cv;
                const core::Float2 content = MeasureContent(core::Float2{Less(availW, ch), Less(availH, cv)});
                size.x = w.HasValue() ? w.Value() : content.x + ch;
                size.y = h.HasValue() ? h.Value() : content.y + cv;
            }
            size.x = core::Clamp(size.x, minW, maxW);
            size.y = core::Clamp(size.y, minH, maxH);
            // The container's limit holds an auto size; a size the style gives overflows it, as in
            // CSS, unless the container fixes the size exactly (flex's resolved size, a stretch).
            const auto fit = [](f32 v, bool given, f32 lo, f32 hi)
            {
                if (lo == hi)
                    return lo;
                return core::Max(lo, given ? v : core::Min(v, hi));
            };
            size.x = fit(size.x, w.HasValue(), constraints.Min.x, constraints.Max.x);
            size.y = fit(size.y, h.HasValue(), constraints.Min.y, constraints.Max.y);
        }
        m_measured = size;
        m_measureKey = constraints;
        m_measureValid = true;
        return size;
    }

    core::Float2 Node::MeasureContent(core::Float2 available)
    {
        if (m_children.IsEmpty())
            return core::Float2{0.0f, 0.0f};
        Flow flow{*this, m_layoutStyle, available.x, available.y, core::Float2{0.0f, 0.0f}, false};
        return RunFlow(flow);
    }

    void Node::Arrange(const Rect& rect)
    {
        // Each edge rounds to the device grid on its own, so neighbours that share an edge still
        // share it and a box's size is what its edges make (a third of 400 is 133 or 134 wide).
        // The container's own origin is on the grid already, so its space's grid is the device's.
        m_pixelScale = m_parent != nullptr ? m_parent->m_pixelScale : TopPixelScale();
        const f32 s = m_pixelScale;
        const auto snap = [s](f32 v) { return core::Round(v * s) / s; };
        const f32 left = snap(rect.x);
        const f32 top = snap(rect.y);
        m_placing = true;
        SetPosition(core::Float2{left, top});
        const core::Float2 size{core::Max(0.0f, snap(rect.x + rect.width) - left), core::Max(0.0f, snap(rect.y + rect.height) - top)};
        const bool resized = size != m_size;
        if (resized)
        {
            m_size = size;
            HandleSizeChange();
        }
        m_placing = false;
        if (resized || m_layoutDirty)
        {
            m_layoutDirty = false;
            m_childLayoutDirty = false;
            m_layingOut = true;
            OnLayout();
            m_layingOut = false;
            if (!m_children.IsEmpty()) // a container laid out (a leaf has nothing to place)
                ++LayoutRunCounter();
        }
        else if (m_childLayoutDirty)
        {
            m_childLayoutDirty = false;
            for (usize i = 0; i < m_children.Size(); ++i)
                m_children[i]->ArrangeAgain();
        }
    }

    void Node::LayoutChildren()
    {
        const Rect box = GetContentBounds();
        // A scroll container's content is laid out with no limit on the axes it scrolls.
        Flow flow{*this, m_layoutStyle, Scrolls(m_layoutStyle.OverflowX) ? kUnbounded : box.width,
                  Scrolls(m_layoutStyle.OverflowY) ? kUnbounded : box.height, core::Float2{box.x, box.y}, true};
        (void)RunFlow(flow);

        // Out of flow: placed by the insets against this node's box, sized by their style, by both
        // insets of an axis, or by their content.
        for (usize i = 0; i < m_children.Size(); ++i)
        {
            Node& child = *m_children[i];
            const LayoutStyle& cs = child.GetLayoutStyle();
            if (!child.IsDisplayed() || cs.Placement != Position::Absolute)
                continue;
            const Thickness m = child.GetMargin();
            const Optional<f32> left = cs.Left.Resolve(m_size.x);
            const Optional<f32> right = cs.Right.Resolve(m_size.x);
            const Optional<f32> top = cs.Top.Resolve(m_size.y);
            const Optional<f32> bottom = cs.Bottom.Resolve(m_size.y);
            Optional<f32> w, h;
            if (cs.Width.IsAuto() && left.HasValue() && right.HasValue())
                w = core::Max(0.0f, m_size.x - left.Value() - right.Value() - m.TotalHorizontal());
            if (cs.Height.IsAuto() && top.HasValue() && bottom.HasValue())
                h = core::Max(0.0f, m_size.y - top.Value() - bottom.Value() - m.TotalVertical());
            const core::Float2 b = MeasureBorder(child, w, h, kUnbounded, kUnbounded, m_size);
            const f32 x = left.HasValue() ? left.Value() + m.Left
                          : right.HasValue() ? m_size.x - right.Value() - b.x - m.Right
                                             : box.x + m.Left;
            const f32 y = top.HasValue() ? top.Value() + m.Top
                          : bottom.HasValue() ? m_size.y - bottom.Value() - b.y - m.Bottom
                                              : box.y + m.Top;
            child.Arrange(Rect{x, y, b.x, b.y});
        }

        // Relative: moved after the flow, its siblings where they were.
        core::Float2 extent{0.0f, 0.0f};
        for (usize i = 0; i < m_children.Size(); ++i)
        {
            Node& child = *m_children[i];
            if (!child.IsDisplayed())
                continue;
            const LayoutStyle& cs = child.GetLayoutStyle();
            if (cs.Placement == Position::Relative)
            {
                const f32 dx = cs.Left.Resolve(box.width).ValueOr(-cs.Right.Resolve(box.width).ValueOr(0.0f));
                const f32 dy = cs.Top.Resolve(box.height).ValueOr(-cs.Bottom.Resolve(box.height).ValueOr(0.0f));
                const f32 s = m_pixelScale; // moved by whole device pixels, keeping it on the grid
                child.SetPosition(core::Float2{child.GetPosition().x + core::Round(dx * s) / s,
                                               child.GetPosition().y + core::Round(dy * s) / s});
            }
            const Thickness m = child.GetMargin();
            extent.x = core::Max(extent.x, child.GetPosition().x + child.GetSize().x + m.Right - box.x);
            extent.y = core::Max(extent.y, child.GetPosition().y + child.GetSize().y + m.Bottom - box.y);
        }
        m_contentExtent = extent;
        SetScrollOffset(m_scrollOffset); // kept within the new range
    }

    void Node::RequestLayout()
    {
        m_measureValid = false;
        m_layoutDirty = true;
        // Up through the containers whose size follows what is inside them: each measures and lays
        // out again. One of definite size stops it (its size does not change); one out of flow
        // makes its container place it again, nothing more.
        Node* n = this;
        while (n->m_parent != nullptr && !HasDefiniteSize(*n))
        {
            const bool outOfFlow = n->m_layoutStyle.Placement == Position::Absolute;
            n = n->m_parent;
            n->m_layoutDirty = true;
            if (outOfFlow)
                break;
            n->m_measureValid = false;
        }
        for (Node* p = n->m_parent; p != nullptr && !p->m_childLayoutDirty; p = p->m_parent)
            p->m_childLayoutDirty = true;
        Invalidate();
    }

    void Node::RequestLayoutTree()
    {
        const auto mark = [](auto& self, Node& node) -> void
        {
            node.m_measureValid = false;
            node.m_layoutDirty = true;
            for (const core::RefPtr<Node>& child : node.m_children)
                self(self, *child);
        };
        mark(mark, *this);
        RequestLayout();
    }

    u32 Node::UpdateLayout()
    {
        const u32 before = LayoutRunCounter();
        for (i32 round = 0; round < 8 && (m_layoutDirty || m_childLayoutDirty); ++round)
        {
            // The top node's size is its style's, or what it holds where that is auto.
            const core::Float2 size = Measure(LayoutConstraints{});
            Arrange(Rect{GetPosition().x, GetPosition().y, size.x, size.y});
        }
        return LayoutRunCounter() - before;
    }

    // === Scrolling ===

    namespace
    {
        constexpr f32 kBarWidth = 8.0f;      // logical units
        constexpr f32 kMinThumb = 20.0f;
        [[nodiscard]] bool ShowsBar(Overflow o, f32 range) noexcept { return o == Overflow::Scroll || (o == Overflow::Auto && range > 0.0f); }
    }

    Rect Node::ScrollBarTrack(u32 axis) const
    {
        if (!IsScrollContainer())
            return Rect{};
        const core::Float2 range = GetScrollRange();
        const bool showX = ShowsBar(m_layoutStyle.OverflowX, range.x);
        const bool showY = ShowsBar(m_layoutStyle.OverflowY, range.y);
        const Thickness b = GetBorderThickness();
        const f32 w = core::Max(0.0f, m_size.x - b.TotalHorizontal());
        const f32 h = core::Max(0.0f, m_size.y - b.TotalVertical());
        if (axis == 1 && showY)
            return Rect{m_size.x - b.Right - kBarWidth, b.Top, kBarWidth, showX ? core::Max(0.0f, h - kBarWidth) : h};
        if (axis == 0 && showX)
            return Rect{b.Left, m_size.y - b.Bottom - kBarWidth, showY ? core::Max(0.0f, w - kBarWidth) : w, kBarWidth};
        return Rect{};
    }

    Rect Node::ScrollBarThumb(u32 axis) const
    {
        const Rect track = ScrollBarTrack(axis);
        if (track.width <= 0.0f || track.height <= 0.0f)
            return Rect{};
        const Rect box = GetContentBounds();
        const f32 view = axis == 1 ? box.height : box.width;
        const f32 extent = core::Max(view, axis == 1 ? m_contentExtent.y : m_contentExtent.x);
        const f32 range = axis == 1 ? GetScrollRange().y : GetScrollRange().x;
        const f32 offset = axis == 1 ? m_scrollOffset.y : m_scrollOffset.x;
        const f32 length = axis == 1 ? track.height : track.width;
        const f32 thumb = core::Min(length, core::Max(kMinThumb, length * view / core::Max(1.0f, extent)));
        const f32 at = range > 0.0f ? (length - thumb) * offset / range : 0.0f;
        return axis == 1 ? Rect{track.x, track.y + at, track.width, thumb} : Rect{track.x + at, track.y, thumb, track.height};
    }

    void Node::DrawScrollBars(DrawContext& ctx) const
    {
        for (u32 axis = 0; axis < 2; ++axis)
        {
            const Rect thumb = ScrollBarThumb(axis);
            if (thumb.width <= 0.0f || thumb.height <= 0.0f)
                continue;
            const bool active = m_scrollDragAxis == axis;
            const Rect inset{thumb.x + 1.5f, thumb.y + 1.5f, core::Max(0.0f, thumb.width - 3.0f), core::Max(0.0f, thumb.height - 3.0f)};
            ctx.VG().FillRoundedRect(inset.ToRectangle(), core::Min(inset.width, inset.height) * 0.5f,
                                     Color{0.62f, 0.66f, 0.72f, active ? 0.85f : 0.55f});
        }
    }

    void Node::ScrollIntoView(Node& descendant)
    {
        if (!IsScrollContainer() || &descendant == this)
            return;
        const Rect world = descendant.GetScreenBounds();
        const core::Float2 lo = ConvertToNodeSpace(core::Float2{world.x, world.y});
        const core::Float2 hi = ConvertToNodeSpace(core::Float2{world.x + world.width, world.y + world.height});
        const Rect box = GetContentBounds();
        core::Float2 offset = m_scrollOffset;
        if (lo.y < box.y)
            offset.y -= box.y - lo.y;
        else if (hi.y > box.y + box.height)
            offset.y += hi.y - (box.y + box.height);
        if (lo.x < box.x)
            offset.x -= box.x - lo.x;
        else if (hi.x > box.x + box.width)
            offset.x += hi.x - (box.x + box.width);
        SetScrollOffset(offset);
    }

    void Node::ScrollDefaultAction(const Event& event)
    {
        const core::Float2 before = m_scrollOffset;
        switch (event.Type)
        {
        case EventType::MouseWheel:
        {
            // Wheel up (positive y) shows what is above. A scroller at its end leaves the wheel to
            // the one around it.
            const WheelEvent& wheel = static_cast<const WheelEvent&>(event);
            ScrollBy(core::Float2{-wheel.Delta.x * m_scrollStep, -wheel.Delta.y * m_scrollStep});
            event.Handled = m_scrollOffset != before;
            return;
        }
        case EventType::KeyDown:
        {
            if (event.Target != this)
                return; // keys a focused widget inside left go on to navigation, not to scrolling
            const Rect box = GetContentBounds();
            const KeyEvent& key = static_cast<const KeyEvent&>(event);
            bool arrow = false;
            switch (static_cast<KeyCode>(key.KeyCode))
            {
            case KeyCode::Down: ScrollBy(core::Float2{0.0f, m_scrollStep}); arrow = true; break;
            case KeyCode::Up: ScrollBy(core::Float2{0.0f, -m_scrollStep}); arrow = true; break;
            case KeyCode::Right: ScrollBy(core::Float2{m_scrollStep, 0.0f}); arrow = true; break;
            case KeyCode::Left: ScrollBy(core::Float2{-m_scrollStep, 0.0f}); arrow = true; break;
            case KeyCode::PageDown: ScrollBy(core::Float2{0.0f, box.height}); break;
            case KeyCode::PageUp: ScrollBy(core::Float2{0.0f, -box.height}); break;
            case KeyCode::Home: SetScrollOffset(core::Float2{m_scrollOffset.x, 0.0f}); break;
            case KeyCode::End: SetScrollOffset(core::Float2{m_scrollOffset.x, GetScrollRange().y}); break;
            default: return;
            }
            // An arrow that could not scroll further is given back (focus moves on).
            event.Handled = !arrow || m_scrollOffset != before;
            return;
        }
        case EventType::MouseDown:
        {
            const MouseEvent& mouse = static_cast<const MouseEvent&>(event);
            if (mouse.Button != MouseButton::Left)
                return;
            const core::Float2 p = ConvertToNodeSpace(mouse.Position);
            for (u32 axis = 0; axis < 2; ++axis)
            {
                const Rect track = ScrollBarTrack(axis);
                if (!track.Contains(p))
                    continue;
                const Rect thumb = ScrollBarThumb(axis);
                if (thumb.Contains(p)) // grab the thumb where it was taken
                {
                    m_scrollDragAxis = axis;
                    m_scrollDragGrab = axis == 1 ? p.y - thumb.y : p.x - thumb.x;
                    if (EventDispatcher* dispatcher = GetEventDispatcher())
                        dispatcher->CapturePointer(*this);
                }
                else // a press on the track pages toward it
                {
                    const Rect box = GetContentBounds();
                    const bool back = axis == 1 ? p.y < thumb.y : p.x < thumb.x;
                    const f32 page = axis == 1 ? box.height : box.width;
                    ScrollBy(axis == 1 ? core::Float2{0.0f, back ? -page : page} : core::Float2{back ? -page : page, 0.0f});
                }
                event.Handled = true;
                Invalidate();
                return;
            }
            return;
        }
        case EventType::MouseMove:
        {
            if (m_scrollDragAxis > 1)
                return;
            const MouseEvent& mouse = static_cast<const MouseEvent&>(event);
            const core::Float2 p = ConvertToNodeSpace(mouse.Position);
            const u32 axis = m_scrollDragAxis;
            const Rect track = ScrollBarTrack(axis);
            const Rect thumb = ScrollBarThumb(axis);
            const f32 travel = axis == 1 ? track.height - thumb.height : track.width - thumb.width;
            const f32 at = (axis == 1 ? p.y - track.y : p.x - track.x) - m_scrollDragGrab;
            const f32 range = axis == 1 ? GetScrollRange().y : GetScrollRange().x;
            const f32 offset = travel > 0.0f ? core::Clamp(at / travel, 0.0f, 1.0f) * range : 0.0f;
            SetScrollOffset(axis == 1 ? core::Float2{m_scrollOffset.x, offset} : core::Float2{offset, m_scrollOffset.y});
            event.Handled = true;
            return;
        }
        case EventType::MouseUp:
        case EventType::PointerCancel:
        case EventType::PointerCaptureLost:
            if (m_scrollDragAxis <= 1)
            {
                m_scrollDragAxis = 2;
                Invalidate();
            }
            return;
        default: return;
        }
    }
}
