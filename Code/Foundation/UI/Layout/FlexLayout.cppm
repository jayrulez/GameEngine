// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :flex_layout partition
//
// CSS Flexbox-inspired container: grow and shrink distribution, justify-content, cross-axis
// alignment, and (P4) line wrapping with align-content, gaps on both axes and flex-basis. Ported from
// Sedulous.UI/src/Layout/FlexLayout.bf, then restructured around a LINE model: both passes
// collect the in-flow children into items, break them into lines (one line when not wrapping),
// and work per line. Per-child FlexGrow/FlexBasis/AlignSelf come from the child's LayoutStyle.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:flex_layout;

import foundation.core; // Max, Optional, RefPtr
import :view;
import :layout_style; // Align, LayoutStyle
import :box_constraints;
import :size_spec;
import :unit;
import :thickness;
import :enums; // Orientation
import :gravity;
import :style_property; // FontSize for em flex-basis

using namespace foundation::core;

export namespace foundation::ui
{
    /// Main-axis content distribution.
    enum class Justify
    {
        Start,
        End,
        Center,
        SpaceBetween,
        SpaceAround,
        SpaceEvenly
    };
    /// Cross-axis packing of the LINES of a wrapping container (no effect on a single line).
    enum class AlignContent
    {
        Start,
        End,
        Center,
        SpaceBetween,
        SpaceAround,
        /// Lines share the free cross space (CSS `normal`).
        Stretch
    };

    class FlexLayout : public ViewGroup
    {
        RTTI_OBJECT(FlexLayout, ViewGroup)
    public:
        Orientation Direction = Orientation::Horizontal;
        Justify JustifyContent = Justify::Start;
        Align AlignItems = Align::Stretch;
        /// Main-axis gap between items (CSS column-gap of a row, row-gap of a column).
        f32 Spacing = 0.0f;
        /// Cross-axis gap between wrapped lines.
        f32 LineSpacing = 0.0f;
        /// CSS-named gaps by AXIS (rows/columns), mapped onto Spacing/LineSpacing by Direction
        /// when set - what markup `gap` / `row-gap` / `column-gap` write.
        Optional<f32> RowGap;
        Optional<f32> ColumnGap;
        /// Break items into lines when the main axis is definite and they do not fit.
        bool Wrap = false;
        ::foundation::ui::AlignContent AlignContent = ::foundation::ui::AlignContent::Stretch;

        FlexLayout() = default;

        /// The gap between items on the main axis after the RowGap/ColumnGap overrides.
        [[nodiscard]] f32 MainGap() const noexcept
        {
            const Optional<f32>& named = Direction == Orientation::Horizontal ? ColumnGap : RowGap;
            return named.HasValue() ? named.Value() : Spacing;
        }
        /// The gap between lines on the cross axis after the overrides.
        [[nodiscard]] f32 CrossGap() const noexcept
        {
            const Optional<f32>& named = Direction == Orientation::Horizontal ? RowGap : ColumnGap;
            return named.HasValue() ? named.Value() : LineSpacing;
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            // Padding and border, from the sheet as well as the Padding field.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            MeasureLines(constraints.Deflate(chrome), constraints, Direction == Orientation::Horizontal);
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            LayoutLines(width, height, Direction == Orientation::Horizontal);
        }

    private:
        struct FlexItem
        {
            View* child = nullptr;
            f32 grow = 0.0f;
            f32 shrink = 0.0f;
            /// The size shrinking starts from (the main size before any shrink).
            f32 base = 0.0f;
            bool frozen = false; ///< shrinking stopped at its minimum
            /// Main-axis size at line-breaking time: the basis (a growing child's, before its
            /// share) or the measured margin box.
            f32 main = 0.0f;
            f32 cross = 0.0f;
            bool matchCross = false;
            /// Baseline-aligned in a row: its baseline from the top of its margin box.
            Optional<f32> baseline;
        };
        struct FlexLine
        {
            usize start = 0;
            usize count = 0;
            f32 main = 0.0f;
            f32 cross = 0.0f;
            f32 baseline = 0.0f; ///< from the line's top, where baseline-aligned items line up
        };

        static SizeSpec ChildWidth(View* child) { return child->Layout().Width; }
        static SizeSpec ChildHeight(View* child) { return child->Layout().Height; }
        static f32 Grow(View* child) { return child->Layout().FlexGrow; }
        static f32 MainOf(Float2 size, bool horizontal) { return horizontal ? size.x : size.y; }
        static f32 CrossOf(Float2 size, bool horizontal) { return horizontal ? size.y : size.x; }
        static BoxConstraints MainCross(f32 minMain, f32 maxMain, f32 minCross, f32 maxCross, bool horizontal)
        {
            return horizontal ? BoxConstraints{minMain, maxMain, minCross, maxCross}
                              : BoxConstraints{minCross, maxCross, minMain, maxMain};
        }

        /// The SEMANTIC part of Flex's first measurement pass, kept parent-side by design:
        /// Match fills the axis, but Match on the CROSS axis is demoted
        /// to loose so it wraps naturally first (a base-side Match would defeat this). Fixed,
        /// margin, and DPI are base-handled.
        static BoxConstraints MakeChildConstraintsLooseCross(BoxConstraints parent, View* child,
                                                             bool isHorizontal)
        {
            const bool fillW = ChildWidth(child).kind == SizeSpec::Kind::Match && !isHorizontal
                                   ? false // cross-axis Match demoted to loose
                                   : ChildWidth(child).kind == SizeSpec::Kind::Match;
            const bool fillH = ChildHeight(child).kind == SizeSpec::Kind::Match && isHorizontal
                                   ? false
                                   : ChildHeight(child).kind == SizeSpec::Kind::Match;
            const f32 availW = Max(0.0f, parent.MaxWidth);
            const f32 availH = Max(0.0f, parent.MaxHeight);
            return BoxConstraints{fillW ? availW : 0.0f, availW, fillH ? availH : 0.0f, availH};
        }

        /// A child's declared flex-basis in logical units (negative = auto). % resolves
        /// against the available main size (0 when unbounded), em against the child's font.
        [[nodiscard]] static f32 DeclaredBasis(View* child, f32 availMain)
        {
            const Unit basis = child->Layout().FlexBasis.Value();
            if (basis == Unit{})
            {
                return -1.0f;
            }
            const f32 reference = BoxConstraints::IsBounded(availMain) ? availMain : 0.0f;
            const f32 fontSize = basis.em != 0.0f ? child->ResolveStyleLength(StyleProperty::FontSize, 0.0f, 16.0f) : 0.0f;
            RootView* root = child->Root();
            const f32 dpiScale = (root != nullptr) ? Max(root->DpiScale, 0.01f) : 1.0f;
            return Max(0.0f, basis.Resolve(dpiScale, reference, fontSize));
        }

        /// Break m_items into m_lines against `limit` (unbounded = one line). The first item of
        /// a line always fits; a half-pixel tolerance keeps a line that grow filled exactly from
        /// spilling on re-break.
        void BreakLines(f32 limit, f32 gap)
        {
            m_lines.Clear();
            FlexLine line;
            for (usize i = 0; i < m_items.Size(); ++i)
            {
                const f32 itemMain = m_items[i].main;
                const f32 needed = line.count == 0 ? itemMain : line.main + gap + itemMain;
                if (line.count > 0 && BoxConstraints::IsBounded(limit) && needed > limit + 0.5f)
                {
                    m_lines.PushBack(line);
                    line = FlexLine{};
                    line.start = i;
                }
                line.main = line.count == 0 ? itemMain : needed;
                ++line.count;
            }
            if (line.count > 0)
            {
                m_lines.PushBack(line);
            }
        }

        void MeasureLines(BoxConstraints inner, BoxConstraints outer, bool horizontal)
        {
            // Padding and border, from the sheet as well as the Padding field.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            const f32 availMain = horizontal ? inner.MaxWidth : inner.MaxHeight;
            const f32 availCross = horizontal ? inner.MaxHeight : inner.MaxWidth;
            const bool mainDefinite = BoxConstraints::IsBounded(availMain);
            const f32 gap = MainGap();
            const BoxConstraints looseInner = horizontal
                                                  ? BoxConstraints{inner.MinWidth, inner.MaxWidth, 0, inner.MaxHeight}
                                                  : BoxConstraints{0, inner.MaxWidth, inner.MinHeight, inner.MaxHeight};

            // 1. Items at their hypothetical main size: the basis (declared, or 0 for a growing
            //    child - the `flex: <grow>` shorthand) or the content size.
            m_items.Clear();
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }
                FlexItem item;
                item.child = child;
                item.grow = Grow(child);
                item.shrink = Max(0.0f, child->Layout().FlexShrink.Value());
                item.matchCross = (horizontal ? ChildHeight(child) : ChildWidth(child)).kind == SizeSpec::Kind::Match;
                const f32 basis = DeclaredBasis(child, availMain);
                if (basis >= 0.0f && item.grow <= 0.0f)
                {
                    // A declared basis without grow IS the main size: tight main, loose cross.
                    child->Measure(MainCross(basis, basis, 0.0f, Max(0.0f, availCross), horizontal));
                    item.main = MainOf(child->MarginBoxSize(), horizontal);
                    item.cross = CrossOf(child->MarginBoxSize(), horizontal);
                }
                else if (item.grow > 0.0f)
                {
                    item.main = Max(0.0f, basis); // auto = 0 for a growing child; measured below
                }
                else
                {
                    child->Measure(MakeChildConstraintsLooseCross(looseInner, child, horizontal));
                    item.main = MainOf(child->MarginBoxSize(), horizontal);
                    item.cross = CrossOf(child->MarginBoxSize(), horizontal);
                }
                m_items.PushBack(item);
            }

            // 2. Lines.
            BreakLines(Wrap && mainDefinite ? availMain : kFloatMax, gap);

            // 3. Per line: an overflowing line shrinks its shrinkable children; growing children
            //    take their share of the line's free space and get measured; the line's cross
            //    size is its tallest item.
            for (FlexLine& line : m_lines)
            {
                if (mainDefinite)
                {
                    ShrinkLine(line, availMain, availCross, gap, horizontal);
                }
                f32 totalGrow = 0.0f;
                f32 used = gap * static_cast<f32>(line.count > 0 ? line.count - 1 : 0);
                for (usize i = line.start; i < line.start + line.count; ++i)
                {
                    totalGrow += m_items[i].grow > 0.0f ? m_items[i].grow : 0.0f;
                    used += m_items[i].main;
                }
                const f32 remaining = mainDefinite ? Max(0.0f, availMain - used) : 0.0f;
                line.main = gap * static_cast<f32>(line.count > 0 ? line.count - 1 : 0);
                line.cross = 0.0f;
                for (usize i = line.start; i < line.start + line.count; ++i)
                {
                    FlexItem& item = m_items[i];
                    if (item.grow > 0.0f)
                    {
                        if (mainDefinite)
                        {
                            // The grow share is the child's MARGIN-BOX main length; the base
                            // deflates its own margin from it.
                            const f32 childMain = item.main + remaining * item.grow / totalGrow;
                            item.child->Measure(MainCross(childMain, Max(0.0f, childMain), 0.0f,
                                                          Max(0.0f, availCross), horizontal));
                        }
                        else
                        {
                            item.child->Measure(MakeChildConstraintsLooseCross(looseInner, item.child, horizontal));
                        }
                        item.main = MainOf(item.child->MarginBoxSize(), horizontal);
                        item.cross = CrossOf(item.child->MarginBoxSize(), horizontal);
                    }
                    line.main += item.main;
                    line.cross = Max(line.cross, item.cross);
                }
                // Match on the cross axis: re-measure at the settled line cross size: tight
                // MARGIN-BOX targets (the base insets to the border box).
                if (line.cross > 0.0f)
                {
                    for (usize i = line.start; i < line.start + line.count; ++i)
                    {
                        FlexItem& item = m_items[i];
                        if (item.matchCross)
                        {
                            item.child->Measure(MainCross(item.main, item.main, line.cross, line.cross, horizontal));
                            item.cross = CrossOf(item.child->MarginBoxSize(), horizontal);
                        }
                    }
                }
            }

            // 4. The container: the widest line (the one line, when not wrapping) by the stacked
            //    line cross sizes plus the cross gaps.
            f32 totalMain = 0.0f;
            f32 totalCross = 0.0f;
            for (const FlexLine& line : m_lines)
            {
                totalMain = Max(totalMain, line.main);
                totalCross += line.cross;
            }
            if (m_lines.Size() > 1)
            {
                totalCross += CrossGap() * static_cast<f32>(m_lines.Size() - 1);
            }
            const f32 width = horizontal ? totalMain : totalCross;
            const f32 height = horizontal ? totalCross : totalMain;
            MeasuredSize = Float2{outer.ConstrainWidth(width + chrome.TotalHorizontal()),
                                  outer.ConstrainHeight(height + chrome.TotalVertical())};
        }

        /// CSS flex-shrink: when a line's items overflow the main size, each shrinkable item
        /// gives up a part of the overflow in proportion to its shrink factor times its base
        /// size. An item measured larger than its target (its minimum held) freezes there, and
        /// the rest share what it could not give, until none freezes or nothing shrinks.
        /// Unlike CSS, an undeclared flex-shrink is 0, so a container's children keep their
        /// sizes unless the sheet or markup asks.
        void ShrinkLine(const FlexLine& line, f32 availMain, f32 availCross, f32 gap, bool horizontal)
        {
            for (usize i = line.start; i < line.start + line.count; ++i)
            {
                m_items[i].base = m_items[i].main;
                m_items[i].frozen = m_items[i].shrink <= 0.0f;
            }
            const f32 gaps = gap * static_cast<f32>(line.count > 0 ? line.count - 1 : 0);
            for (usize round = 0; round < line.count; ++round)
            {
                f32 fixed = gaps;
                f32 unfrozenBase = 0.0f;
                f32 scaledTotal = 0.0f;
                for (usize i = line.start; i < line.start + line.count; ++i)
                {
                    const FlexItem& item = m_items[i];
                    if (item.frozen)
                    {
                        fixed += item.main;
                    }
                    else
                    {
                        unfrozenBase += item.base;
                        scaledTotal += item.shrink * item.base;
                    }
                }
                const f32 overflow = fixed + unfrozenBase - availMain;
                if (overflow <= 0.0f || scaledTotal <= 0.0f)
                {
                    return;
                }
                bool froze = false;
                for (usize i = line.start; i < line.start + line.count; ++i)
                {
                    FlexItem& item = m_items[i];
                    if (item.frozen)
                    {
                        continue;
                    }
                    const f32 target = Max(0.0f, item.base - overflow * item.shrink * item.base / scaledTotal);
                    item.child->Measure(MainCross(target, target, 0.0f, Max(0.0f, availCross), horizontal));
                    item.main = MainOf(item.child->MarginBoxSize(), horizontal);
                    item.cross = CrossOf(item.child->MarginBoxSize(), horizontal);
                    if (item.main > target + 0.5f)
                    {
                        item.frozen = true;
                        froze = true;
                    }
                }
                if (!froze)
                {
                    return;
                }
            }
        }

        void LayoutLines(f32 width, f32 height, bool horizontal)
        {
            // Padding and border, from the sheet as well as the Padding field.
            const Thickness chrome = ResolveBoxMetrics().Chrome();
            const f32 contentW = width - chrome.TotalHorizontal();
            const f32 contentH = height - chrome.TotalVertical();
            const f32 contentMain = horizontal ? contentW : contentH;
            const f32 contentCross = horizontal ? contentH : contentW;
            const f32 gap = MainGap();
            const f32 crossGap = CrossGap();

            // Items at their MEASURED sizes, re-broken against the arranged main size.
            m_items.Clear();
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (!IsInFlow(child))
                {
                    continue;
                }
                FlexItem item;
                item.child = child;
                item.main = MainOf(child->MarginBoxSize(), horizontal);
                item.cross = CrossOf(child->MarginBoxSize(), horizontal);
                m_items.PushBack(item);
            }
            BreakLines(Wrap ? contentMain : kFloatMax, gap);
            for (FlexLine& line : m_lines)
            {
                line.cross = 0.0f;
                line.baseline = 0.0f;
                f32 belowBaseline = 0.0f;
                for (usize i = line.start; i < line.start + line.count; ++i)
                {
                    FlexItem& item = m_items[i];
                    line.cross = Max(line.cross, item.cross);
                    // A row's baseline-aligned items share the line's baseline, the deepest
                    // ascent above it and the deepest descent below: the line grows to hold both.
                    // A column has no baselines to line up (CSS): those items sit at the start.
                    const Optional<Align>& alignSelf = item.child->Layout().AlignSelf;
                    if (horizontal && (alignSelf.HasValue() ? alignSelf.Value() : AlignItems) == Align::Baseline)
                    {
                        item.baseline = BaselineOf(*item.child);
                        line.baseline = Max(line.baseline, item.baseline.Value());
                        belowBaseline = Max(belowBaseline, item.cross - item.baseline.Value());
                    }
                }
                line.cross = Max(line.cross, line.baseline + belowBaseline);
            }

            // Line packing on the cross axis. A single line IS the container (align-content
            // has no effect - CSS); wrapped lines distribute the free cross space.
            f32 crossStart = 0.0f;
            f32 crossBetween = crossGap;
            if (!Wrap && m_lines.Size() == 1)
            {
                m_lines[0].cross = contentCross;
            }
            else if (!m_lines.IsEmpty())
            {
                f32 stacked = crossGap * static_cast<f32>(m_lines.Size() - 1);
                for (const FlexLine& line : m_lines)
                {
                    stacked += line.cross;
                }
                const f32 free = Max(0.0f, contentCross - stacked);
                const f32 lineCount = static_cast<f32>(m_lines.Size());
                switch (AlignContent)
                {
                case AlignContent::Start:
                    break;
                case AlignContent::End:
                    crossStart = free;
                    break;
                case AlignContent::Center:
                    crossStart = free * 0.5f;
                    break;
                case AlignContent::SpaceBetween:
                    if (m_lines.Size() > 1)
                    {
                        crossBetween += free / (lineCount - 1.0f);
                    }
                    break;
                case AlignContent::SpaceAround:
                    crossStart = free / lineCount * 0.5f;
                    crossBetween += free / lineCount;
                    break;
                case AlignContent::Stretch:
                    for (FlexLine& line : m_lines)
                    {
                        line.cross += free / lineCount;
                    }
                    break;
                }
            }

            const f32 padMain = horizontal ? chrome.Left : chrome.Top;
            const f32 padCross = horizontal ? chrome.Top : chrome.Left;
            f32 crossPos = padCross + crossStart;
            for (const FlexLine& line : m_lines)
            {
                f32 startOffset = 0.0f;
                f32 between = gap;
                f32 lineMain = gap * static_cast<f32>(line.count > 0 ? line.count - 1 : 0);
                for (usize i = line.start; i < line.start + line.count; ++i)
                {
                    lineMain += m_items[i].main;
                }
                ComputeJustify(JustifyContent, contentMain, lineMain, static_cast<i32>(line.count), startOffset, between);

                f32 mainPos = padMain + startOffset;
                for (usize i = line.start; i < line.start + line.count; ++i)
                {
                    const FlexItem& item = m_items[i];
                    if (i > line.start)
                    {
                        mainPos += between;
                    }
                    const Optional<Align>& alignSelf = item.child->Layout().AlignSelf;
                    const Align align = alignSelf.HasValue() ? alignSelf.Value() : AlignItems;
                    // MARGIN-box placement (base insets by margin once).
                    f32 itemCrossPos = crossPos;
                    f32 finalCross = item.cross;
                    switch (align)
                    {
                    case Align::Start:
                        break;
                    case Align::Baseline:
                        if (item.baseline.HasValue())
                        {
                            itemCrossPos = crossPos + line.baseline - item.baseline.Value();
                        }
                        break;
                    case Align::End:
                        itemCrossPos = crossPos + line.cross - item.cross;
                        break;
                    case Align::Center:
                        itemCrossPos = crossPos + (line.cross - item.cross) * 0.5f;
                        break;
                    case Align::Stretch:
                        // CSS: stretch fills the cross axis only when the cross size is auto; an
                        // explicit (Fixed) cross size keeps its measured length.
                        finalCross = (horizontal ? item.child->Layout().Height : item.child->Layout().Width)->IsFixed()
                                         ? item.cross
                                         : line.cross;
                        break;
                    }
                    finalCross = Max(0.0f, finalCross);
                    if (horizontal)
                    {
                        item.child->Layout(mainPos, itemCrossPos, item.main, finalCross);
                    }
                    else
                    {
                        item.child->Layout(itemCrossPos, mainPos, finalCross, item.main);
                    }
                    mainPos += item.main;
                }
                crossPos += line.cross + crossBetween;
            }
        }

        /// A child's baseline from the top of its margin box: the one it reports (a text view's
        /// first line), or without one the bottom of its border box, as CSS synthesizes it.
        [[nodiscard]] static f32 BaselineOf(const View& child)
        {
            const f32 top = child.Layout().Margin->Top;
            const f32 own = child.GetBaseline();
            return top + (own >= 0.0f ? own : child.MeasuredSize.y);
        }

        static void ComputeJustify(Justify justify, f32 containerSize, f32 totalChildSize,
                                   i32 childCount, f32& startOffset, f32& gap)
        {
            const f32 freeSpace = Max(0.0f, containerSize - totalChildSize);
            switch (justify)
            {
            case Justify::Start:
                break;
            case Justify::End:
                startOffset = freeSpace;
                break;
            case Justify::Center:
                startOffset = freeSpace * 0.5f;
                break;
            case Justify::SpaceBetween:
                if (childCount > 1)
                {
                    gap += freeSpace / static_cast<f32>(childCount - 1);
                }
                break;
            case Justify::SpaceAround:
                if (childCount > 0)
                {
                    const f32 around = freeSpace / static_cast<f32>(childCount);
                    startOffset = around * 0.5f;
                    gap += around;
                }
                break;
            case Justify::SpaceEvenly:
                if (childCount > 0)
                {
                    const f32 even = freeSpace / static_cast<f32>(childCount + 1);
                    startOffset = even;
                    gap += even;
                }
                break;
            }
        }

        // Per-pass scratch (members: no allocation per frame once warm).
        Array<FlexItem> m_items;
        Array<FlexLine> m_lines;
    };

    RTTI_DEFINE_OBJECT(FlexLayout, "rtti::ui")
}
