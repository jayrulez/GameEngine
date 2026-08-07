// Draconic GUI - :relative_layout partition
//
// RelativeLayout: positions each child by an anchor relative to the parent's padding-inset
// content box - pin to an edge, a corner, or center, horizontally and/or vertically. Modeled
// on eepp's UIRelativeLayout (role, not a line-for-line port); this v1 covers parent-relative
// anchoring only (sibling-relative rules like toRightOf/below are deferred). Children keep
// their own sizes. Anchors are stored per child (non-owning Node* keys); an un-anchored child
// defaults to the top-left. Re-runs on size change and child add/remove.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:relative_layout;

import draconic.foundation; // Float2, HashMap
import :rect;
import :node;
import :ui_widget;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    // Anchor bitmask: one horizontal bit and one vertical bit. Combine with '|' (e.g.
    // AnchorRight | AnchorBottom = bottom-right corner). Missing horizontal/vertical bits
    // default to Left / Top respectively.
    enum Anchor : u32
    {
        AnchorNone = 0,
        AnchorLeft = 1u << 0,
        AnchorRight = 1u << 1,
        AnchorCenterH = 1u << 2,
        AnchorTop = 1u << 3,
        AnchorBottom = 1u << 4,
        AnchorCenterV = 1u << 5,
        AnchorCenter = AnchorCenterH | AnchorCenterV,
    };

    class RelativeLayout : public UIWidget
    {
        DRACONIC_OBJECT(RelativeLayout, UIWidget)
    public:
        RelativeLayout() = default;

        // Anchor a child within the content box. Passing AnchorNone clears it (back to top-left).
        void SetAnchor(Node* child, u32 anchor)
        {
            if (child == nullptr)
                return;
            if (anchor == AnchorNone)
                m_anchors.Remove(child);
            else
                m_anchors.InsertOrAssign(child, anchor);
            PerformLayout();
        }
        [[nodiscard]] u32 GetAnchor(Node* child) const
        {
            const u32* found = child != nullptr ? m_anchors.Find(child) : nullptr;
            return found != nullptr ? *found : static_cast<u32>(AnchorNone);
        }

        void PerformLayout()
        {
            const Rect content = GetContentBounds();
            for (usize i = 0; i < ChildCount(); ++i)
            {
                Node* child = GetChildAt(i);
                if (child == nullptr || !child->IsVisible())
                    continue;

                const u32 anchor = GetAnchor(child);
                const foundation::Float2 size = child->GetSize();

                f32 x = content.x; // default: left
                if (anchor & AnchorCenterH)
                    x = content.x + (content.width - size.x) * 0.5f;
                else if (anchor & AnchorRight)
                    x = content.x + content.width - size.x;

                f32 y = content.y; // default: top
                if (anchor & AnchorCenterV)
                    y = content.y + (content.height - size.y) * 0.5f;
                else if (anchor & AnchorBottom)
                    y = content.y + content.height - size.y;

                child->SetPosition(foundation::Float2{x, y});
            }
        }

    protected:
        void OnSizeChange() override { PerformLayout(); }
        void OnChildrenChanged() override { PerformLayout(); }

        HashMap<Node*, u32> m_anchors; // per-child anchor flags (non-owning keys)
    };

    DRACONIC_DEFINE_OBJECT(RelativeLayout, "draconic::gui")
}
