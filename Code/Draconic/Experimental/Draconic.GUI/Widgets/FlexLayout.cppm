// Draconic GUI - :flex_layout partition
//
// FlexLayout: a CSS-flexbox-style layout. Modeled on the CSS flex model (role, not a port). It
// lays its children along a main axis (row or column) with:
//   - flex-grow per child (children expand to share leftover main-axis space),
//   - justify-content on the main axis (start/end/center/space-between/around/evenly),
//   - align-items on the cross axis (start/end/center/stretch),
//   - a gap between items.
// This is our first measure/distribute layouter (LinearLayout/Grid/Relative just place children
// at their own sizes). Wrapping (flex-wrap), flex-shrink, and per-child align-self are follow-ups.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:flex_layout;

import draconic.foundation; // Float2, HashMap, Array, Max, Min
import :rect;
import :node;
import :ui_widget;
import :css_values; // ParseLength (markup)

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    enum class FlexDirection
    {
        Row,
        Column
    };
    enum class JustifyContent
    {
        Start,
        End,
        Center,
        SpaceBetween,
        SpaceAround,
        SpaceEvenly
    };
    enum class AlignItems
    {
        Start,
        End,
        Center,
        Stretch
    };

    class FlexLayout : public UIWidget
    {
        DRACONIC_OBJECT(FlexLayout, UIWidget)
    public:
        FlexLayout() { SetTag(foundation::StringView(u8"flexlayout")); }

        void SetDirection(FlexDirection direction)
        {
            m_direction = direction;
            m_basis = {};
            PerformLayout();
        }
        [[nodiscard]] FlexDirection GetDirection() const noexcept { return m_direction; }
        void SetJustifyContent(JustifyContent justify)
        {
            m_justify = justify;
            PerformLayout();
        }
        [[nodiscard]] JustifyContent GetJustifyContent() const noexcept { return m_justify; }
        void SetAlignItems(AlignItems align)
        {
            m_align = align;
            PerformLayout();
        }
        [[nodiscard]] AlignItems GetAlignItems() const noexcept { return m_align; }
        void SetGap(f32 gap)
        {
            m_gap = foundation::Max(0.0f, gap);
            PerformLayout();
        }
        [[nodiscard]] f32 GetGap() const noexcept { return m_gap; }

        // The grow factor for a child (0 = fixed; >0 = shares leftover main-axis space in
        // proportion to the total grow).
        void SetChildGrow(Node* child, f32 grow)
        {
            if (child == nullptr)
                return;
            if (grow <= 0.0f)
                m_grow.Remove(child);
            else
                m_grow.InsertOrAssign(child, grow);
            PerformLayout();
        }
        [[nodiscard]] f32 GetChildGrow(Node* child) const
        {
            const f32* g = child != nullptr ? m_grow.Find(child) : nullptr;
            return g != nullptr ? *g : 0.0f;
        }

        bool SetMarkupAttribute(foundation::StringView name, foundation::StringView value) override
        {
            if (name == foundation::StringView(u8"direction") ||
                name == foundation::StringView(u8"flex-direction"))
            {
                SetDirection(value == foundation::StringView(u8"column") ? FlexDirection::Column
                                                                   : FlexDirection::Row);
                return true;
            }
            if (name == foundation::StringView(u8"justify-content"))
            {
                SetJustifyContent(ParseJustify(value));
                return true;
            }
            if (name == foundation::StringView(u8"align-items"))
            {
                SetAlignItems(ParseAlign(value));
                return true;
            }
            if (name == foundation::StringView(u8"gap"))
            {
                if (Optional<f32> g = ParseLength(value); g.HasValue())
                    SetGap(g.Value());
                return true;
            }
            return UIWidget::SetMarkupAttribute(name, value);
        }

        void PerformLayout()
        {
            const Rect content = GetContentBounds();
            const bool row = (m_direction == FlexDirection::Row);
            const f32 mainSize = row ? content.width : content.height;
            const f32 crossSize = row ? content.height : content.width;

            // Collect visible children + their base main sizes and grow factors.
            Array<Node*> items;
            f32 baseTotal = 0.0f;
            f32 growTotal = 0.0f;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                Node* child = GetChildAt(i);
                if (child == nullptr || !child->IsVisible())
                    continue;
                items.PushBack(child);
                baseTotal += BasisMainOf(child, row);
                growTotal += GetChildGrow(child);
            }
            const usize n = items.Size();
            if (n == 0)
                return;

            const f32 gapTotal = m_gap * static_cast<f32>(n - 1);
            f32 freeSpace = mainSize - baseTotal - gapTotal;

            // Distribute grow along the main axis.
            Array<f32> mainSizes;
            for (Node* child : items)
            {
                f32 main = BasisMainOf(child, row);
                if (growTotal > 0.0f && freeSpace > 0.0f)
                    main += (GetChildGrow(child) / growTotal) * freeSpace;
                mainSizes.PushBack(main);
            }

            // Remaining free space after grow drives justify-content.
            f32 usedMain = gapTotal;
            for (f32 m : mainSizes)
                usedMain += m;
            const f32 remaining = foundation::Max(0.0f, mainSize - usedMain);

            f32 mainStart = row ? content.x : content.y;
            f32 between = m_gap;
            ComputeJustify(remaining, n, mainStart, between);
            f32 mainPos = mainStart;

            for (usize i = 0; i < n; ++i)
            {
                Node* child = items[i];
                const f32 mainLen = mainSizes[i];
                const f32 childCrossBase = CrossOf(child, row);
                const f32 crossLen = (m_align == AlignItems::Stretch) ? crossSize : childCrossBase;
                const f32 crossPos =
                    (row ? content.y : content.x) + CrossOffset(crossSize, crossLen);

                if (row)
                {
                    child->SetPosition(foundation::Float2{mainPos, crossPos});
                    child->SetSize(foundation::Float2{mainLen, crossLen});
                }
                else
                {
                    child->SetPosition(foundation::Float2{crossPos, mainPos});
                    child->SetSize(foundation::Float2{crossLen, mainLen});
                }
                mainPos += mainLen + between;
            }
        }

    protected:
        void OnSizeChange() override { PerformLayout(); }
        void OnChildrenChanged() override { PerformLayout(); }

    private:
        [[nodiscard]] static f32 CrossOf(Node* child, bool row)
        {
            return row ? child->GetSize().y : child->GetSize().x;
        }

        // The child's flex-basis (main-axis size), captured once from its initial size so that
        // growing it does not corrupt the basis on the next layout. Cleared on direction change.
        [[nodiscard]] f32 BasisMainOf(Node* child, bool row)
        {
            if (const f32* stored = m_basis.Find(child))
                return *stored;
            const f32 basis = row ? child->GetSize().x : child->GetSize().y;
            m_basis.InsertOrAssign(child, basis);
            return basis;
        }

        [[nodiscard]] f32 CrossOffset(f32 crossSize, f32 childCross) const
        {
            switch (m_align)
            {
            case AlignItems::Start:
                return 0.0f;
            case AlignItems::End:
                return foundation::Max(0.0f, crossSize - childCross);
            case AlignItems::Center:
                return foundation::Max(0.0f, (crossSize - childCross) * 0.5f);
            case AlignItems::Stretch:
                return 0.0f;
            }
            return 0.0f;
        }

        // Set the main-axis start position and the between-items spacing for the current
        // justify-content, given the leftover space and item count.
        void ComputeJustify(f32 remaining, usize n, f32& start, f32& between) const
        {
            between = m_gap;
            switch (m_justify)
            {
            case JustifyContent::Start:
                break;
            case JustifyContent::End:
                start += remaining;
                break;
            case JustifyContent::Center:
                start += remaining * 0.5f;
                break;
            case JustifyContent::SpaceBetween:
                if (n > 1)
                    between += remaining / static_cast<f32>(n - 1);
                break;
            case JustifyContent::SpaceAround:
                if (n > 0)
                {
                    const f32 unit = remaining / static_cast<f32>(n);
                    start += unit * 0.5f;
                    between += unit;
                }
                break;
            case JustifyContent::SpaceEvenly:
                if (n > 0)
                {
                    const f32 unit = remaining / static_cast<f32>(n + 1);
                    start += unit;
                    between += unit;
                }
                break;
            }
        }

        [[nodiscard]] static JustifyContent ParseJustify(foundation::StringView v)
        {
            if (v == foundation::StringView(u8"end") || v == foundation::StringView(u8"flex-end"))
                return JustifyContent::End;
            if (v == foundation::StringView(u8"center"))
                return JustifyContent::Center;
            if (v == foundation::StringView(u8"space-between"))
                return JustifyContent::SpaceBetween;
            if (v == foundation::StringView(u8"space-around"))
                return JustifyContent::SpaceAround;
            if (v == foundation::StringView(u8"space-evenly"))
                return JustifyContent::SpaceEvenly;
            return JustifyContent::Start;
        }
        [[nodiscard]] static AlignItems ParseAlign(foundation::StringView v)
        {
            if (v == foundation::StringView(u8"end") || v == foundation::StringView(u8"flex-end"))
                return AlignItems::End;
            if (v == foundation::StringView(u8"center"))
                return AlignItems::Center;
            if (v == foundation::StringView(u8"stretch"))
                return AlignItems::Stretch;
            return AlignItems::Start;
        }

        FlexDirection m_direction = FlexDirection::Row;
        JustifyContent m_justify = JustifyContent::Start;
        AlignItems m_align = AlignItems::Start;
        f32 m_gap = 0.0f;
        HashMap<Node*, f32> m_grow;  // per-child grow factor (non-owning keys)
        HashMap<Node*, f32> m_basis; // per-child flex-basis (main-axis size, captured once)
    };

    DRACONIC_DEFINE_OBJECT(FlexLayout, "draconic::gui")
}
