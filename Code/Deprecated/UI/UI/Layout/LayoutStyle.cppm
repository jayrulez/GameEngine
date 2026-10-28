// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :layout_style partition
//
// The ONE per-child placement record. Every View carries a LayoutStyle value (View::Layout /
// View::SetLayout); every container reads the fields it understands and ignores the rest, so a
// child keeps its intent across reparenting and a custom container needs no parameter subclass.
// Container-owned data (Grid tracks, Flex direction/justify/align-items, Dock LastChildFill) stays
// on the container. Spec: Documentation/Specs/ui-layout-and-style-model.md, section 1.

module;
#include "Core/Prelude.h"
#include <type_traits>

export module foundation.ui:layout_style;

import foundation.core;
import :thickness;
import :size_spec;
import :gravity;
import :unit;

using namespace foundation::core;

export namespace foundation::ui
{
    /// Dock side for a DockLayout child.
    enum class Dock
    {
        Left,
        Top,
        Right,
        Bottom,
        Fill
    };

    /// Cross-axis alignment (FlexLayout AlignItems and the per-child AlignSelf override).
    enum class Align
    {
        Start,
        End,
        Center,
        Stretch,
        Baseline
    };

    /// Where a view sits in its container's flow.
    enum class Position
    {
        /// Laid out by the container (Flex/Flow/Grid/Dock/Frame/Absolute rules).
        Static,
        /// Taken OUT of the container's flow and placed by the base ViewGroup against the
        /// container's content box from the Left/Top/Right/Bottom insets (CSS absolute).
        Absolute
    };

    /// A LayoutStyle field that remembers whether it was SET. Assignment declares; the
    /// default-constructed field is undeclared and the cascade (`width:`, `margin:` ... in a
    /// sheet) may fill it. An inline (declared) value always wins over the sheet, even when
    /// it equals the default - the CSS inline-style rule. Reads convert implicitly to T;
    /// member access goes through `->`.
    template <typename T>
    struct Declared
    {
        T value{};
        bool declared = false;

        constexpr Declared() noexcept = default;
        constexpr Declared(const T& v) noexcept : value(v), declared(true) {}
        constexpr Declared(const T& v, bool isDeclared) noexcept : value(v), declared(isDeclared) {}

        constexpr Declared& operator=(const T& v) noexcept
        {
            value = v;
            declared = true;
            return *this;
        }
        [[nodiscard]] constexpr operator const T&() const noexcept { return value; }
        [[nodiscard]] constexpr const T* operator->() const noexcept { return &value; }
        [[nodiscard]] constexpr const T& Value() const noexcept { return value; }
        [[nodiscard]] constexpr bool IsDeclared() const noexcept { return declared; }
        /// Back to undeclared (the sheet may fill the field again); `defaultValue` is what
        /// an undeclared field reads as.
        constexpr void Clear(const T& defaultValue = T{}) noexcept
        {
            value = defaultValue;
            declared = false;
        }

        [[nodiscard]] constexpr bool operator==(const Declared& other) const noexcept
        {
            return declared == other.declared && value == other.value;
        }
        /// Compare against a plain value (or anything the value compares with, e.g. an int
        /// literal against a Declared<f32>); a template so it never competes with the
        /// Declared-vs-Declared overload through the converting constructor.
        template <typename U>
            requires(!std::is_same_v<std::remove_cvref_t<U>, Declared>)
        [[nodiscard]] constexpr bool operator==(const U& other) const noexcept
        {
            return value == other;
        }
    };

    struct LayoutStyle
    {
        // --- every container ---
        /// Desired width. Default: Wrap (fit to content).
        Declared<SizeSpec> Width{SizeSpec::Wrap(), false};
        /// Desired height. Default: Wrap (fit to content).
        Declared<SizeSpec> Height{SizeSpec::Wrap(), false};
        /// Space between this view and its siblings/parent.
        Declared<Thickness> Margin{};
        /// Size clamps applied after the Width/Height spec (a zero unit = no clamp).
        Declared<Unit> MinWidth{};
        Declared<Unit> MinHeight{};
        Declared<Unit> MaxWidth{};
        Declared<Unit> MaxHeight{};
        /// Static (in the container's flow) or Absolute (placed by the insets below).
        Declared<::foundation::ui::Position> Position{::foundation::ui::Position::Static, false};
        /// Draw order among siblings (ascending; ties keep child order). Hit testing walks
        /// the same order back to front.
        Declared<i32> ZIndex{};

        // --- FlexLayout ---
        /// Extra main-axis space this child absorbs.
        Declared<f32> FlexGrow{};
        /// How much this child shrinks when space is insufficient.
        Declared<f32> FlexShrink{};
        /// Main-axis starting size before grow (a zero unit = auto: the content size, or 0 for
        /// a growing child - the `flex: <grow>` shorthand). %, em and calc resolve like a size.
        Declared<Unit> FlexBasis{};
        /// Cross-axis override (empty = the parent's AlignItems, or the sheet's align-self).
        Optional<Align> AlignSelf;

        // --- FrameLayout / FlowLayout / FlexLayout cross-axis ---
        /// Placement anchor within the cell the container hands this child.
        ::foundation::ui::Gravity Gravity = ::foundation::ui::Gravity::None;

        // --- DockLayout ---
        ::foundation::ui::Dock Dock = ::foundation::ui::Dock::Left;

        // --- AbsoluteLayout, and Position::Absolute in any container ---
        /// Offset from the container's content box. AbsoluteLayout reads Left/Top; an
        /// absolute child in any container honors all four (Right/Bottom anchor the far
        /// edges; Left+Right both declared = the width is the remainder).
        Declared<f32> Left{};
        Declared<f32> Top{};
        Declared<f32> Right{};
        Declared<f32> Bottom{};

        // --- GridLayout ---
        /// -1 = auto-flow (the grid assigns the next free cell; the intent stays -1).
        i32 GridRow = -1;
        i32 GridColumn = -1;
        i32 GridRowSpan = 1;
        i32 GridColumnSpan = 1;

        [[nodiscard]] bool operator==(const LayoutStyle& other) const noexcept
        {
            return Width == other.Width && Height == other.Height && Margin == other.Margin &&
                   MinWidth == other.MinWidth && MinHeight == other.MinHeight &&
                   MaxWidth == other.MaxWidth && MaxHeight == other.MaxHeight &&
                   Position == other.Position && ZIndex == other.ZIndex &&
                   FlexGrow == other.FlexGrow && FlexShrink == other.FlexShrink &&
                   FlexBasis == other.FlexBasis &&
                   AlignSelf == other.AlignSelf && Gravity == other.Gravity &&
                   Dock == other.Dock && Left == other.Left && Top == other.Top &&
                   Right == other.Right && Bottom == other.Bottom &&
                   GridRow == other.GridRow && GridColumn == other.GridColumn &&
                   GridRowSpan == other.GridRowSpan && GridColumnSpan == other.GridColumnSpan;
        }
    };
}
