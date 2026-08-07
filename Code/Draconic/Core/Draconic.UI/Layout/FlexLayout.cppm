// Draconic UI - :flex_layout partition
//
// CSS Flexbox-inspired container: grow distribution, justify-content, cross-axis alignment. Ported
// from Sedulous.UI/src/Layout/FlexLayout.bf. (Beef nested LayoutParams -> FlexLayoutParams; ComputeJustify
// ref params -> f32& out params; the unused-in-layout Gravity field is fully qualified to dodge the
// field/type name clash.)

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:flex_layout;

import draconic.foundation; // Max, Optional, RefPtr
import :view;
import :layout_params;
import :box_constraints;
import :size_spec;
import :unit;
import :thickness;
import :enums; // Orientation
import :gravity;

using namespace draconic::foundation;

export namespace draconic::ui
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
    /// Cross-axis alignment.
    enum class Align
    {
        Start,
        End,
        Center,
        Stretch,
        Baseline
    };

    /// LayoutParams for a FlexLayout child.
    class FlexLayoutParams : public LayoutParams
    {
        DRACONIC_OBJECT(FlexLayoutParams, LayoutParams)
    public:
        f32 Grow = 0.0f;           ///< Extra main-axis space this child absorbs.
        f32 Shrink = 0.0f;         ///< How much this child shrinks when space is insufficient.
        Optional<Align> AlignSelf; ///< Cross-axis override (empty = parent AlignItems).
        ::draconic::ui::Gravity Gravity = ::draconic::ui::Gravity::None; ///< Cross-axis gravity.
        FlexLayoutParams() = default;
    };

    class FlexLayout : public ViewGroup
    {
        DRACONIC_OBJECT(FlexLayout, ViewGroup)
    public:
        Orientation Direction = Orientation::Horizontal;
        Justify JustifyContent = Justify::Start;
        Align AlignItems = Align::Stretch;
        f32 Spacing = 0.0f;

        FlexLayout() = default;

    protected:
        LayoutParamsPtr CreateDefaultLayoutParams() override
        {
            return MakeRef<FlexLayoutParams>(DefaultAllocator());
        }

        void OnMeasure(BoxConstraints constraints) override
        {
            const BoxConstraints inner = constraints.Deflate(Padding);
            if (Direction == Orientation::Horizontal)
            {
                MeasureHorizontal(inner, constraints);
            }
            else
            {
                MeasureVertical(inner, constraints);
            }
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            if (Direction == Orientation::Horizontal)
            {
                LayoutHorizontal(width, height);
            }
            else
            {
                LayoutVertical(width, height);
            }
        }

    private:
        static SizeSpec ChildWidth(View* child)
        {
            return child->LayoutParams ? child->LayoutParams->Width : SizeSpec::Wrap();
        }
        static SizeSpec ChildHeight(View* child)
        {
            return child->LayoutParams ? child->LayoutParams->Height : SizeSpec::Wrap();
        }
        static Thickness ChildMargin(View* child)
        {
            return child->LayoutParams ? child->LayoutParams->Margin : Thickness{};
        }
        static f32 Grow(View* child)
        {
            FlexLayoutParams* flp = Cast<FlexLayoutParams>(child->LayoutParams.Get());
            return flp != nullptr ? flp->Grow : 0.0f;
        }

        /// Like MakeChildConstraints but treats .Match on the cross-axis as .Wrap (loose) so .Match
        /// children don't blow up to infinity during the first measurement pass.
        static BoxConstraints MakeChildConstraintsLooseCross(BoxConstraints parent, View* child,
                                                             bool isHorizontal)
        {
            const Thickness margin = ChildMargin(child);
            RootView* root = child->Root();
            const f32 dpiScale = root != nullptr ? root->DpiScale : 1.0f;

            const f32 availW = Max(0.0f, parent.MaxWidth - margin.TotalHorizontal());
            const f32 availH = Max(0.0f, parent.MaxHeight - margin.TotalVertical());

            SizeSpec widthSpec = ChildWidth(child);
            SizeSpec heightSpec = ChildHeight(child);
            if (isHorizontal && heightSpec.kind == SizeSpec::Kind::Match)
            {
                heightSpec = SizeSpec::Wrap();
            }
            else if (!isHorizontal && widthSpec.kind == SizeSpec::Kind::Match)
            {
                widthSpec = SizeSpec::Wrap();
            }

            f32 minW = 0, maxW = 0, minH = 0, maxH = 0;
            switch (widthSpec.kind)
            {
            case SizeSpec::Kind::Fixed:
            {
                const f32 w = widthSpec.ResolveFixed(dpiScale);
                minW = w;
                maxW = w;
                break;
            }
            case SizeSpec::Kind::Match:
                minW = availW;
                maxW = availW;
                break;
            case SizeSpec::Kind::Wrap:
                minW = 0;
                maxW = availW;
                break;
            }
            switch (heightSpec.kind)
            {
            case SizeSpec::Kind::Fixed:
            {
                const f32 h = heightSpec.ResolveFixed(dpiScale);
                minH = h;
                maxH = h;
                break;
            }
            case SizeSpec::Kind::Match:
                minH = availH;
                maxH = availH;
                break;
            case SizeSpec::Kind::Wrap:
                minH = 0;
                maxH = availH;
                break;
            }
            return BoxConstraints{minW, maxW, minH, maxH};
        }

        void MeasureHorizontal(BoxConstraints inner, BoxConstraints outer)
        {
            f32 totalFixed = 0, maxCross = 0, totalGrow = 0;
            i32 visibleCount = 0;
            bool hasMatchCross = false;
            const BoxConstraints looseInner{inner.MinWidth, inner.MaxWidth, 0, inner.MaxHeight};

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                ++visibleCount;
                const f32 grow = Grow(child);
                if (grow > 0)
                {
                    totalGrow += grow;
                    continue;
                }

                child->Measure(MakeChildConstraintsLooseCross(looseInner, child, true));
                const Thickness margin = ChildMargin(child);
                totalFixed += child->MeasuredSize.x + margin.TotalHorizontal();
                maxCross = Max(maxCross, child->MeasuredSize.y + margin.TotalVertical());
                if (ChildHeight(child).kind == SizeSpec::Kind::Match)
                {
                    hasMatchCross = true;
                }
            }
            if (visibleCount > 1)
            {
                totalFixed += Spacing * static_cast<f32>(visibleCount - 1);
            }

            if (totalGrow > 0)
            {
                const bool isMainAxisDefinite = inner.MaxWidth < 100000;
                const f32 remaining =
                    isMainAxisDefinite ? Max(0.0f, inner.MaxWidth - totalFixed) : 0.0f;
                for (usize i = 0; i < ChildCount(); ++i)
                {
                    View* child = GetChildAt(i);
                    if (child->Visibility == Visibility::Gone)
                    {
                        continue;
                    }
                    const f32 grow = Grow(child);
                    if (grow <= 0)
                    {
                        continue;
                    }
                    const Thickness margin = ChildMargin(child);

                    if (isMainAxisDefinite)
                    {
                        const f32 childMain = remaining * grow / totalGrow;
                        child->Measure(
                            BoxConstraints{childMain - margin.TotalHorizontal(),
                                           Max(0.0f, childMain - margin.TotalHorizontal()), 0,
                                           Max(0.0f, inner.MaxHeight - margin.TotalVertical())});
                        totalFixed += childMain;
                    }
                    else
                    {
                        child->Measure(MakeChildConstraintsLooseCross(looseInner, child, true));
                        totalFixed += child->MeasuredSize.x + margin.TotalHorizontal();
                    }
                    maxCross = Max(maxCross, child->MeasuredSize.y + margin.TotalVertical());
                    if (ChildHeight(child).kind == SizeSpec::Kind::Match)
                    {
                        hasMatchCross = true;
                    }
                }
            }

            if (hasMatchCross && maxCross > 0)
            {
                for (usize i = 0; i < ChildCount(); ++i)
                {
                    View* child = GetChildAt(i);
                    if (child->Visibility == Visibility::Gone)
                    {
                        continue;
                    }
                    if (ChildHeight(child).kind == SizeSpec::Kind::Match)
                    {
                        const Thickness margin = ChildMargin(child);
                        const f32 crossH = Max(0.0f, maxCross - margin.TotalVertical());
                        child->Measure(BoxConstraints{child->MeasuredSize.x, child->MeasuredSize.x,
                                                      crossH, crossH});
                    }
                }
            }

            MeasuredSize = Float2{outer.ConstrainWidth(totalFixed + Padding.TotalHorizontal()),
                                  outer.ConstrainHeight(maxCross + Padding.TotalVertical())};
        }

        void MeasureVertical(BoxConstraints inner, BoxConstraints outer)
        {
            f32 totalFixed = 0, maxCross = 0, totalGrow = 0;
            i32 visibleCount = 0;
            bool hasMatchCross = false;
            const BoxConstraints looseInner{0, inner.MaxWidth, inner.MinHeight, inner.MaxHeight};

            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                ++visibleCount;
                const f32 grow = Grow(child);
                if (grow > 0)
                {
                    totalGrow += grow;
                    continue;
                }

                child->Measure(MakeChildConstraintsLooseCross(looseInner, child, false));
                const Thickness margin = ChildMargin(child);
                totalFixed += child->MeasuredSize.y + margin.TotalVertical();
                maxCross = Max(maxCross, child->MeasuredSize.x + margin.TotalHorizontal());
                if (ChildWidth(child).kind == SizeSpec::Kind::Match)
                {
                    hasMatchCross = true;
                }
            }
            if (visibleCount > 1)
            {
                totalFixed += Spacing * static_cast<f32>(visibleCount - 1);
            }

            if (totalGrow > 0)
            {
                const bool isMainAxisDefinite = inner.MaxHeight < 100000;
                const f32 remaining =
                    isMainAxisDefinite ? Max(0.0f, inner.MaxHeight - totalFixed) : 0.0f;
                for (usize i = 0; i < ChildCount(); ++i)
                {
                    View* child = GetChildAt(i);
                    if (child->Visibility == Visibility::Gone)
                    {
                        continue;
                    }
                    const f32 grow = Grow(child);
                    if (grow <= 0)
                    {
                        continue;
                    }
                    const Thickness margin = ChildMargin(child);

                    if (isMainAxisDefinite)
                    {
                        const f32 childMain = remaining * grow / totalGrow;
                        child->Measure(
                            BoxConstraints{0, Max(0.0f, inner.MaxWidth - margin.TotalHorizontal()),
                                           childMain - margin.TotalVertical(),
                                           Max(0.0f, childMain - margin.TotalVertical())});
                        totalFixed += childMain;
                    }
                    else
                    {
                        child->Measure(MakeChildConstraintsLooseCross(looseInner, child, false));
                        totalFixed += child->MeasuredSize.y + margin.TotalVertical();
                    }
                    maxCross = Max(maxCross, child->MeasuredSize.x + margin.TotalHorizontal());
                    if (ChildWidth(child).kind == SizeSpec::Kind::Match)
                    {
                        hasMatchCross = true;
                    }
                }
            }

            if (hasMatchCross && maxCross > 0)
            {
                for (usize i = 0; i < ChildCount(); ++i)
                {
                    View* child = GetChildAt(i);
                    if (child->Visibility == Visibility::Gone)
                    {
                        continue;
                    }
                    if (ChildWidth(child).kind == SizeSpec::Kind::Match)
                    {
                        const Thickness margin = ChildMargin(child);
                        const f32 crossW = Max(0.0f, maxCross - margin.TotalHorizontal());
                        child->Measure(BoxConstraints{crossW, crossW, child->MeasuredSize.y,
                                                      child->MeasuredSize.y});
                    }
                }
            }

            MeasuredSize = Float2{outer.ConstrainWidth(maxCross + Padding.TotalHorizontal()),
                                  outer.ConstrainHeight(totalFixed + Padding.TotalVertical())};
        }

        void LayoutHorizontal(f32 width, f32 height)
        {
            const f32 contentW = width - Padding.TotalHorizontal();
            const f32 contentH = height - Padding.TotalVertical();

            f32 totalMain = 0;
            i32 visibleCount = 0;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                ++visibleCount;
                totalMain += child->MeasuredSize.x + ChildMargin(child).TotalHorizontal();
            }
            if (visibleCount > 1)
            {
                totalMain += Spacing * static_cast<f32>(visibleCount - 1);
            }

            f32 startOffset = 0, gap = Spacing;
            ComputeJustify(JustifyContent, contentW, totalMain, visibleCount, startOffset, gap);

            f32 xPos = Padding.Left + startOffset;
            bool first = true;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                if (!first)
                {
                    xPos += gap;
                }
                first = false;

                const Thickness margin = ChildMargin(child);
                FlexLayoutParams* flp = Cast<FlexLayoutParams>(child->LayoutParams.Get());
                const Align align = (flp != nullptr && flp->AlignSelf.HasValue())
                                        ? flp->AlignSelf.Value()
                                        : AlignItems;

                const f32 childW = child->MeasuredSize.x;
                const f32 childH = child->MeasuredSize.y;
                const f32 availCross = contentH - margin.TotalVertical();

                f32 yPos = Padding.Top + margin.Top;
                f32 finalH = childH;
                switch (align)
                {
                case Align::Start:
                    yPos = Padding.Top + margin.Top;
                    break;
                case Align::End:
                    yPos = Padding.Top + contentH - margin.Bottom - childH;
                    break;
                case Align::Center:
                    yPos = Padding.Top + margin.Top + (availCross - childH) * 0.5f;
                    break;
                case Align::Stretch:
                    yPos = Padding.Top + margin.Top;
                    finalH = availCross;
                    break;
                case Align::Baseline:
                    yPos = Padding.Top + margin.Top;
                    break;
                }
                child->Layout(xPos + margin.Left, yPos, childW, Max(0.0f, finalH));
                xPos += childW + margin.TotalHorizontal();
            }
        }

        void LayoutVertical(f32 width, f32 height)
        {
            const f32 contentW = width - Padding.TotalHorizontal();
            const f32 contentH = height - Padding.TotalVertical();

            f32 totalMain = 0;
            i32 visibleCount = 0;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                ++visibleCount;
                totalMain += child->MeasuredSize.y + ChildMargin(child).TotalVertical();
            }
            if (visibleCount > 1)
            {
                totalMain += Spacing * static_cast<f32>(visibleCount - 1);
            }

            f32 startOffset = 0, gap = Spacing;
            ComputeJustify(JustifyContent, contentH, totalMain, visibleCount, startOffset, gap);

            f32 yPos = Padding.Top + startOffset;
            bool first = true;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                View* child = GetChildAt(i);
                if (child->Visibility == Visibility::Gone)
                {
                    continue;
                }
                if (!first)
                {
                    yPos += gap;
                }
                first = false;

                const Thickness margin = ChildMargin(child);
                FlexLayoutParams* flp = Cast<FlexLayoutParams>(child->LayoutParams.Get());
                const Align align = (flp != nullptr && flp->AlignSelf.HasValue())
                                        ? flp->AlignSelf.Value()
                                        : AlignItems;

                const f32 childW = child->MeasuredSize.x;
                const f32 childH = child->MeasuredSize.y;
                const f32 availCross = contentW - margin.TotalHorizontal();

                f32 xPos = Padding.Left + margin.Left;
                f32 finalW = childW;
                switch (align)
                {
                case Align::Start:
                    xPos = Padding.Left + margin.Left;
                    break;
                case Align::End:
                    xPos = Padding.Left + contentW - margin.Right - childW;
                    break;
                case Align::Center:
                    xPos = Padding.Left + margin.Left + (availCross - childW) * 0.5f;
                    break;
                case Align::Stretch:
                    xPos = Padding.Left + margin.Left;
                    finalW = availCross;
                    break;
                case Align::Baseline:
                    xPos = Padding.Left + margin.Left;
                    break;
                }
                child->Layout(xPos, yPos + margin.Top, Max(0.0f, finalW), childH);
                yPos += childH + margin.TotalVertical();
            }
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
    };

    DRACONIC_DEFINE_OBJECT(FlexLayoutParams, "draconic::ui")
    DRACONIC_DEFINE_OBJECT(FlexLayout, "draconic::ui")
}
