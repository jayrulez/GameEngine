// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :layout_style partition
//
// What a node asks of layout (E2 of Specs/gui-lab-evolution.md), one record on every node, filled
// from its sheet, its markup or code: how it sizes (width, height, min, max: a length, a
// percentage of its container, or auto), how it lays out its children (display: flex, grid,
// stack, dock or none, with each kind's properties), how its container places it (the flex,
// grid and dock item properties, margin), whether it is out of flow (position and insets), and
// whether it clips or scrolls (overflow). Lengths in em, rem, vw and vh are resolved to pixels
// when the style is applied; percentages and auto wait for layout, which knows the container.

module;
#include "Core/Prelude.h"

export module experimental.gui:layout_style;

import foundation.core; // Array, Float2, Optional
import :thickness;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    /// A length as layout reads it: auto (the content's, or what the container gives), pixels,
    /// a percentage of the container's content box, or one of CSS's content sizes. None is a
    /// max-width or max-height with no bound.
    enum class LayoutLengthKind : u8
    {
        Auto,
        Px,
        Percent,
        FitContent,
        MinContent,
        MaxContent,
        None,
    };
    struct LayoutLength
    {
        LayoutLengthKind Kind = LayoutLengthKind::Auto;
        f32 Value = 0.0f;

        [[nodiscard]] static constexpr LayoutLength Px(f32 v) noexcept { return {LayoutLengthKind::Px, v}; }
        [[nodiscard]] static constexpr LayoutLength Percent(f32 v) noexcept { return {LayoutLengthKind::Percent, v}; }
        [[nodiscard]] static constexpr LayoutLength None() noexcept { return {LayoutLengthKind::None, 0.0f}; }
        [[nodiscard]] constexpr bool IsAuto() const noexcept { return Kind == LayoutLengthKind::Auto; }
        /// The length in pixels against `base` (the container's content length, negative when it
        /// is not known yet); none for auto, a content size, or a percentage of an unknown base.
        [[nodiscard]] Optional<f32> Resolve(f32 base) const noexcept
        {
            switch (Kind)
            {
            case LayoutLengthKind::Px: return Value;
            case LayoutLengthKind::Percent:
                if (base >= 0.0f)
                    return Value * base / 100.0f;
                return {};
            default: return {};
            }
        }
        [[nodiscard]] constexpr bool operator==(const LayoutLength&) const noexcept = default;
    };

    /// A bar's or a track's direction (a scrollbar, a tab strip).
    enum class Orientation : u8
    {
        Horizontal,
        Vertical
    };

    /// How a node lays out its children (CSS `display`). A plain container is a flex column.
    enum class Display : u8
    {
        Flex,
        Grid,
        Stack, ///< children over each other in the content box, each aligned on its own
        Dock,  ///< children docked to the sides in order, the last filling what remains
        None,  ///< the node and its subtree take no space and draw nothing
    };
    /// In its container's flow, offset after it, or out of it (placed by its insets).
    enum class Position : u8
    {
        Static,
        Relative,
        Absolute,
    };
    enum class FlexDirection : u8
    {
        Row,
        Column,
        RowReverse,
        ColumnReverse,
    };
    enum class FlexWrap : u8
    {
        NoWrap,
        Wrap,
        WrapReverse,
    };
    /// Distribution along an axis (justify-content, align-content).
    enum class Justify : u8
    {
        Start,
        End,
        Center,
        SpaceBetween,
        SpaceAround,
        SpaceEvenly,
        Stretch, ///< align-content's normal: lines share the free space
    };
    /// Placement across an axis (align-items, align-self, justify-items, justify-self). Auto on
    /// an item takes the container's.
    enum class Align : u8
    {
        Auto,
        Start,
        End,
        Center,
        Stretch,
        Baseline,
    };
    enum class Dock : u8
    {
        Fill,
        Left,
        Top,
        Right,
        Bottom,
    };
    /// Visible draws children outside the box; hidden clips them; scroll and auto clip and
    /// scroll (scroll always shows its bar, auto when the content overflows).
    enum class Overflow : u8
    {
        Visible,
        Hidden,
        Scroll,
        Auto,
    };
    /// Whether width and height include the padding and border (the lab's default, what a UI
    /// author means by a 120 pixel button) or are the content's alone, CSS's initial value.
    enum class BoxSizing : u8
    {
        BorderBox,
        ContentBox,
    };
    enum class GridAutoFlow : u8
    {
        Row,
        Column,
    };

    /// A grid track's size: from `Min` to `Max`, or a share of the free space (`Fr` > 0, its
    /// minimum `Min`). `120px` is Min = Max = 120; `1fr` is Fr 1; `auto` is Min = Max = auto (the
    /// content's); `minmax(100px, 1fr)` is Min 100 and Fr 1.
    struct GridTrack
    {
        LayoutLength Min{};
        LayoutLength Max{};
        f32 Fr = 0.0f;

        [[nodiscard]] static constexpr GridTrack Fixed(f32 px) noexcept { return {LayoutLength::Px(px), LayoutLength::Px(px), 0.0f}; }
        [[nodiscard]] static constexpr GridTrack Flex(f32 fr) noexcept { return {LayoutLength{}, LayoutLength{}, fr}; }
        [[nodiscard]] constexpr bool operator==(const GridTrack&) const noexcept = default;
    };
    /// Where an item sits on one grid axis: a 1-based start line (0: placed automatically) and
    /// how many tracks it spans.
    struct GridPlacement
    {
        i32 Start = 0;
        i32 Span = 1;
        [[nodiscard]] constexpr bool operator==(const GridPlacement&) const noexcept = default;
    };

    struct LayoutStyle
    {
        // --- The node's own box ---
        LayoutLength Width{};
        LayoutLength Height{};
        LayoutLength MinWidth{}; ///< auto: none, but a flex item's content minimum
        LayoutLength MinHeight{};
        LayoutLength MaxWidth = LayoutLength::None();
        LayoutLength MaxHeight = LayoutLength::None();
        Thickness Margin{};
        BoxSizing Sizing = BoxSizing::BorderBox;
        Overflow OverflowX = Overflow::Visible;
        Overflow OverflowY = Overflow::Visible;

        // --- Out of flow ---
        Position Placement = Position::Static;
        LayoutLength Left{};
        LayoutLength Top{};
        LayoutLength Right{};
        LayoutLength Bottom{};
        i32 ZIndex = 0;

        // --- How it lays out its children ---
        Display Kind = Display::Flex;
        FlexDirection Direction = FlexDirection::Column;
        FlexWrap Wrap = FlexWrap::NoWrap;
        Justify JustifyContent = Justify::Start;
        Align AlignItems = Align::Stretch;
        Justify AlignContent = Justify::Stretch;
        Align JustifyItems = Align::Stretch; ///< grid and stack
        f32 RowGap = 0.0f;
        f32 ColumnGap = 0.0f;
        Array<GridTrack> Columns;
        Array<GridTrack> Rows;
        GridTrack AutoRows{};
        GridTrack AutoColumns{};
        GridAutoFlow AutoFlow = GridAutoFlow::Row;

        // --- How its container places it ---
        f32 Grow = 0.0f;
        f32 Shrink = 1.0f;
        LayoutLength Basis{};
        Align AlignSelf = Align::Auto;
        Align JustifySelf = Align::Auto;
        i32 Order = 0;
        GridPlacement Column{};
        GridPlacement Row{};
        experimental::gui::Dock DockSide = experimental::gui::Dock::Fill;
    };

    /// The properties a sheet may set through a node's setters, one bit each (E3): code that sets
    /// one makes it its own, as CSS's inline style, and the sheet leaves it be (Node pins it).
    enum StyleSlot : u32
    {
        SlotOpacity,
        SlotPaddingTop, SlotPaddingRight, SlotPaddingBottom, SlotPaddingLeft,
        SlotMarginTop, SlotMarginRight, SlotMarginBottom, SlotMarginLeft,
        SlotWidth, SlotHeight, SlotMinWidth, SlotMinHeight, SlotMaxWidth, SlotMaxHeight,
        SlotTop, SlotRight, SlotBottom, SlotLeft,
        SlotFlexBasis, SlotFlexGrow, SlotFlexShrink, SlotOrder, SlotZIndex, SlotRowGap, SlotColumnGap,
        SlotDisplay, SlotPosition, SlotBoxSizing, SlotOverflowX, SlotOverflowY,
        SlotFlexDirection, SlotFlexWrap, SlotJustifyContent, SlotAlignContent, SlotAlignItems,
        SlotJustifyItems, SlotAlignSelf, SlotJustifySelf, SlotGridAutoFlow, SlotDock,
        SlotGridTemplateColumns, SlotGridTemplateRows, SlotGridAutoRows, SlotGridAutoColumns,
        SlotGridColumn, SlotGridRow,
        SlotEnabled, SlotVisibility,
        SlotColor, SlotFontFamily, SlotFontSize, SlotTextAlign, SlotVerticalAlign,
        SlotWhiteSpace, SlotLineHeight, SlotLetterSpacing,
        SlotCount,
    };
    static_assert(SlotCount <= 64, "the pins are one u64");
    [[nodiscard]] constexpr u64 SlotBit(StyleSlot slot) noexcept { return u64{1} << slot; }

    /// Calls `f(slot, member)` for each LayoutStyle field a sheet sets as one property (the margin,
    /// four properties in one field, is apart).
    template <typename F>
    constexpr void ForEachLayoutField(F&& f)
    {
        f(SlotWidth, &LayoutStyle::Width);
        f(SlotHeight, &LayoutStyle::Height);
        f(SlotMinWidth, &LayoutStyle::MinWidth);
        f(SlotMinHeight, &LayoutStyle::MinHeight);
        f(SlotMaxWidth, &LayoutStyle::MaxWidth);
        f(SlotMaxHeight, &LayoutStyle::MaxHeight);
        f(SlotTop, &LayoutStyle::Top);
        f(SlotRight, &LayoutStyle::Right);
        f(SlotBottom, &LayoutStyle::Bottom);
        f(SlotLeft, &LayoutStyle::Left);
        f(SlotFlexBasis, &LayoutStyle::Basis);
        f(SlotFlexGrow, &LayoutStyle::Grow);
        f(SlotFlexShrink, &LayoutStyle::Shrink);
        f(SlotOrder, &LayoutStyle::Order);
        f(SlotZIndex, &LayoutStyle::ZIndex);
        f(SlotRowGap, &LayoutStyle::RowGap);
        f(SlotColumnGap, &LayoutStyle::ColumnGap);
        f(SlotDisplay, &LayoutStyle::Kind);
        f(SlotPosition, &LayoutStyle::Placement);
        f(SlotBoxSizing, &LayoutStyle::Sizing);
        f(SlotOverflowX, &LayoutStyle::OverflowX);
        f(SlotOverflowY, &LayoutStyle::OverflowY);
        f(SlotFlexDirection, &LayoutStyle::Direction);
        f(SlotFlexWrap, &LayoutStyle::Wrap);
        f(SlotJustifyContent, &LayoutStyle::JustifyContent);
        f(SlotAlignContent, &LayoutStyle::AlignContent);
        f(SlotAlignItems, &LayoutStyle::AlignItems);
        f(SlotJustifyItems, &LayoutStyle::JustifyItems);
        f(SlotAlignSelf, &LayoutStyle::AlignSelf);
        f(SlotJustifySelf, &LayoutStyle::JustifySelf);
        f(SlotGridAutoFlow, &LayoutStyle::AutoFlow);
        f(SlotDock, &LayoutStyle::DockSide);
        f(SlotGridTemplateColumns, &LayoutStyle::Columns);
        f(SlotGridTemplateRows, &LayoutStyle::Rows);
        f(SlotGridAutoRows, &LayoutStyle::AutoRows);
        f(SlotGridAutoColumns, &LayoutStyle::AutoColumns);
        f(SlotGridColumn, &LayoutStyle::Column);
        f(SlotGridRow, &LayoutStyle::Row);
    }
    namespace detail
    {
        template <typename T>
        [[nodiscard]] constexpr bool SameField(const T& a, const T& b)
        {
            if constexpr (requires { a.Size(); a[0]; })
            {
                if (a.Size() != b.Size())
                    return false;
                for (usize i = 0; i < a.Size(); ++i)
                    if (!(a[i] == b[i]))
                        return false;
                return true;
            }
            else
                return a == b;
        }
    }
    /// The slots whose values differ between two layout styles.
    [[nodiscard]] inline u64 ChangedLayoutSlots(const LayoutStyle& a, const LayoutStyle& b)
    {
        u64 bits = 0;
        ForEachLayoutField([&](StyleSlot slot, auto member)
        {
            if (!detail::SameField(a.*member, b.*member))
                bits |= SlotBit(slot);
        });
        if (a.Margin.Top != b.Margin.Top) bits |= SlotBit(SlotMarginTop);
        if (a.Margin.Right != b.Margin.Right) bits |= SlotBit(SlotMarginRight);
        if (a.Margin.Bottom != b.Margin.Bottom) bits |= SlotBit(SlotMarginBottom);
        if (a.Margin.Left != b.Margin.Left) bits |= SlotBit(SlotMarginLeft);
        return bits;
    }

    /// What a GUI's layout overlay shows over its tree, for working on a layout: each node's border
    /// box (red outline), its padding (green) and margin (orange), and where it reports a baseline
    /// of its own, the baseline (magenta).
    struct LayoutOverlay
    {
        bool Bounds = false;
        bool Padding = false;
        bool Margin = false;
        bool Baselines = false;

        [[nodiscard]] static constexpr LayoutOverlay All() noexcept { return {true, true, true, true}; }
        [[nodiscard]] constexpr bool Any() const noexcept { return Bounds || Padding || Margin || Baselines; }
        [[nodiscard]] constexpr bool operator==(const LayoutOverlay&) const noexcept = default;
    };

    /// A length layout leaves unbounded (an available width with no limit).
    inline constexpr f32 kUnbounded = 1.0e30f;
    [[nodiscard]] constexpr bool IsUnbounded(f32 length) noexcept { return length >= 1.0e29f; }

    /// What a container allows a child when it measures it: the border-box size from `Min` to
    /// `Max` (kUnbounded for none), and the lengths its percentages resolve against (negative when
    /// the container's own size is not known yet).
    struct LayoutConstraints
    {
        core::Float2 Min{0.0f, 0.0f};
        core::Float2 Max{kUnbounded, kUnbounded};
        core::Float2 PercentBase{-1.0f, -1.0f};

        [[nodiscard]] static LayoutConstraints Tight(core::Float2 size, core::Float2 percentBase) noexcept
        {
            return LayoutConstraints{size, size, percentBase};
        }
        [[nodiscard]] bool operator==(const LayoutConstraints& o) const noexcept
        {
            return Min.x == o.Min.x && Min.y == o.Min.y && Max.x == o.Max.x && Max.y == o.Max.y &&
                   PercentBase.x == o.PercentBase.x && PercentBase.y == o.PercentBase.y;
        }
    };
}
