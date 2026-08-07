// Draconic UI - module implementation unit for TooltipManager.
//
// Holds the View-touching bodies of TooltipManager (it reaches across the whole View cluster: UIContext,
// RootView, PopupLayer, TooltipView, ITooltipProvider, Label). Like InputImpl.cpp / UIClusterImpl.cpp it
// lives in an implementation unit outside the interface-partition dependency graph, so :tooltip_manager
// only forward-declares the view types. Ported from Sedulous.UI/src/Overlay/TooltipManager.bf.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.ui;

using namespace draconic::foundation;

namespace draconic::ui
{
    TooltipManager::TooltipManager(UIContext* context) : m_context(context)
    {
        m_tooltipView = MakeRef<TooltipView>(DefaultAllocator());
    }

    TooltipManager::~TooltipManager()
    {
        // During destruction the PopupLayer may already be freed. The TooltipView was shown with
        // ownsView:false, so nothing else deletes it; null Parent/Context so the RefPtr drop below does
        // not touch freed objects (matches the Beef destructor's manual detach).
        if (m_tooltipView)
        {
            m_tooltipView->Parent = nullptr;
            m_tooltipView->Context = nullptr;
        }
    }

    // The tooltip OWNER for a hovered view: the view itself or its nearest ancestor with
    // tooltip content (provider or text). Hit-testing returns the LEAF under the cursor, so
    // a container carrying one tooltip for all its children (e.g. an inspector property row)
    // would otherwise never show it; resolution also keeps the timer alive while hover moves
    // between children of the same owner.
    static View* ResolveTooltipOwner(View* view)
    {
        for (View* v = view; v != nullptr; v = v->Parent)
        {
            if (v->AsTooltipProvider() != nullptr || v->TooltipText.Size() > 0)
            {
                return v;
            }
        }
        return nullptr;
    }

    void TooltipManager::OnHoverChanged(View* newTarget)
    {
        // Hover onto the tooltip itself never dismisses (checked on the RAW view - the
        // tooltip's own content carries no TooltipText).
        if (m_showing && newTarget != nullptr && IsTooltipOrDescendant(newTarget))
        {
            return;
        }

        View* owner = ResolveTooltipOwner(newTarget);
        const ViewId newId = (owner != nullptr) ? owner->Id : ViewId::Invalid;
        if (newId != m_hoverTarget)
        {
            Hide();
            m_hoverTarget = newId;
            m_hoverTime = 0;
        }
    }

    void TooltipManager::OnMouseDown()
    {
        if (m_showing && m_interactive)
        {
            const ViewId hoveredId = m_context->GetInputManager()->HoveredId();
            View* hovered = m_context->GetViewById(hoveredId);
            if (hovered != nullptr && IsTooltipOrDescendant(hovered))
            {
                return;
            }
        }
        Hide();
    }

    void TooltipManager::Update(f32 deltaTime)
    {
        if (!m_hoverTarget.IsValid())
        {
            return;
        }

        if (!m_showing)
        {
            m_hoverTime += deltaTime;
            if (m_hoverTime >= ShowDelay)
            {
                View* target = m_context->GetViewById(m_hoverTarget);
                if (target != nullptr)
                {
                    Show(target);
                }
                else
                {
                    m_hoverTarget = ViewId::Invalid;
                } // View was deleted
            }
        }
        else
        {
            m_showTime += deltaTime;
            if (m_showTime >= AutoHideDelay)
            {
                Hide();
            }
        }
    }

    void TooltipManager::OnViewDeleted(View* view)
    {
        if (m_hoverTarget == view->Id)
        {
            m_hoverTarget = ViewId::Invalid;
        }
    }

    void TooltipManager::Show(View* target)
    {
        // Check for a custom tooltip content provider first.
        if (ITooltipProvider* provider = target->AsTooltipProvider())
        {
            RefPtr<View> content = provider->CreateTooltipContent();
            if (!content)
            {
                return;
            }
            m_tooltipView->SetContent(content.Get());
        }
        else
        {
            // Fall back to a plain text label.
            const StringView text = target->TooltipText.AsView();
            if (text.Size() == 0)
            {
                return;
            }

            RefPtr<Label> label = MakeRef<Label>(DefaultAllocator(), text);
            m_tooltipView->SetContent(label.Get());
        }

        m_interactive = target->IsTooltipInteractive;
        m_tooltipView->IsHitTestVisible = m_interactive;

        // Get the popup layer from the active root.
        RootView* root = m_context->ActiveInputRoot();
        if (root == nullptr)
        {
            return;
        }
        PopupLayer* popupLayer = root->GetPopupLayer();

        // Show at (0,0) first so the tooltip gets context-attached (needed for measurement).
        // closeOnClickOutside=false, modal=false, ownsView=false, takesFocus=false -
        // a tooltip must NEVER disturb the focused view (typing, completion popups).
        popupLayer->ShowPopup(m_tooltipView.Get(), nullptr, 0, 0, false, false, false,
                              false);
        m_showing = true;
        m_showTime = 0;

        // Measure then reposition.
        const Float2 logical = root->LogicalSize();
        const BoxConstraints layerConstraints = BoxConstraints::Loose(logical.x, logical.y);
        m_tooltipView->Measure(layerConstraints);

        const Rectangle screen{0, 0, logical.x, logical.y};
        const Float2 popupSize = m_tooltipView->MeasuredSize;

        // Compute the screen-space position of the target. Pointer placement anchors a
        // 1x1 "target" at the mouse instead (per-region tooltips on large views).
        Float2 targetScreen = target->LocalToScreen(Float2{0, 0});
        f32 targetW = target->Width();
        f32 targetH = target->Height();
        TooltipPlacement placement = target->TooltipPlacement;
        if (placement == TooltipPlacement::Pointer)
        {
            const InputManager* input = m_context->GetInputManager();
            targetScreen = Float2{input->MouseX() + 12.0f, input->MouseY() + 6.0f};
            targetW = 1.0f;
            targetH = 1.0f;
            placement = TooltipPlacement::Bottom; // below-right of the pointer, screen-clamped
        }
        const Float2 pos = PositionTooltip(placement, targetScreen.x, targetScreen.y, targetW,
                                           targetH, popupSize, screen);

        popupLayer->UpdatePopupPosition(m_tooltipView.Get(), pos.x, pos.y);
    }

    Float2 TooltipManager::PositionTooltip(TooltipPlacement placement, f32 targetX, f32 targetY,
                                           f32 targetW, f32 targetH, Float2 popupSize,
                                           Rectangle screen)
    {
        f32 x = 0, y = 0;
        switch (placement)
        {
        case TooltipPlacement::Pointer: // resolved to Bottom-at-mouse by the caller
        case TooltipPlacement::Bottom:
            x = targetX;
            y = targetY + targetH;
            if (y + popupSize.y > screen.height)
            {
                y = targetY - popupSize.y;
            }
            break;
        case TooltipPlacement::Top:
            x = targetX;
            y = targetY - popupSize.y;
            if (y < screen.y)
            {
                y = targetY + targetH;
            }
            break;
        case TooltipPlacement::Right:
            x = targetX + targetW;
            y = targetY;
            if (x + popupSize.x > screen.width)
            {
                x = targetX - popupSize.x;
            }
            break;
        case TooltipPlacement::Left:
            x = targetX - popupSize.x;
            y = targetY;
            if (x < screen.x)
            {
                x = targetX + targetW;
            }
            break;
        }

        // Final clamp to screen.
        x = Clamp(x, screen.x, Max(screen.x, screen.x + screen.width - popupSize.x));
        y = Clamp(y, screen.y, Max(screen.y, screen.y + screen.height - popupSize.y));
        return Float2{x, y};
    }

    void TooltipManager::Hide()
    {
        if (m_showing)
        {
            RootView* root = m_context->ActiveInputRoot();
            if (root != nullptr)
            {
                root->GetPopupLayer()->ClosePopup(m_tooltipView.Get());
            }
            m_showing = false;
        }
        m_hoverTime = 0;
    }

    bool TooltipManager::IsTooltipOrDescendant(View* view) const
    {
        View* v = view;
        while (v != nullptr)
        {
            if (v == m_tooltipView.Get())
            {
                return true;
            }
            v = v->Parent;
        }
        return false;
    }
}
