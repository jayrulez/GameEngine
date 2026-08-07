// Draconic UI - :combo_box partition
//
// Drop-down selector: shows the selected item + a dropdown arrow, and opens a dedicated ComboBoxDropdown
// panel (not ContextMenu) via the RootView's PopupLayer. Ported from Sedulous.UI/src/Controls/ComboBox.bf.
// ComboBox is a View + IPopupOwner (multiple inheritance; IPopupOwner is a non-View pattern-B base).
// Beef List<String> + manual delete -> Array<String> (RAII); get/set props -> methods; the dropdown is
// RefPtr-owned by PopupLayer (ownsView) and closed via a deferred MutationQueue action.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:combo_box;

import draconic.foundation;
import draconic.vg;
import draconic.fonts;
import :view;
import :event;
import :box_constraints;
import :thickness;
import :draw_context;
import :drawable;
import :rounded_rect_drawable;
import :control_state;
import :style_property;
import :event_args;
import :input_enums;
import :enums;
import :palette;
import :ipopup_owner;
import :popup_layer; // ShowPopup/ClosePopup on the RootView's PopupLayer

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::ui
{
    class ComboBox : public View, public IPopupOwner
    {
        DRACONIC_OBJECT(ComboBox, View)
    public:
        Event<void(ComboBox*, i32)> OnSelectionChanged;

        ComboBox()
        {
            IsFocusable = true;
            IsTabStop = true;
            WantsArrowKeys = true;
            Cursor = CursorType::Hand;
        }

        [[nodiscard]] i32 SelectedIndex() const noexcept { return m_selectedIndex; }
        void SetSelectedIndex(i32 value)
        {
            const i32 clamped = Clamp(value, -1, static_cast<i32>(m_items.Size()) - 1);
            if (m_selectedIndex != clamped)
            {
                m_selectedIndex = clamped;
                Invalidate();
                OnSelectionChanged.Invoke(this, clamped);
            }
        }
        [[nodiscard]] StringView SelectedText() const
        {
            return (m_selectedIndex >= 0 && m_selectedIndex < static_cast<i32>(m_items.Size()))
                       ? m_items[static_cast<usize>(m_selectedIndex)].AsView()
                       : StringView{};
        }
        [[nodiscard]] i32 ItemCount() const noexcept { return static_cast<i32>(m_items.Size()); }
        [[nodiscard]] bool IsOpen() const noexcept { return m_isOpen; }
        [[nodiscard]] StringView ItemAt(i32 index) const
        {
            return m_items[static_cast<usize>(index)].AsView();
        }

        i32 AddItem(StringView text)
        {
            const i32 index = static_cast<i32>(m_items.Size());
            m_items.PushBack(String(text));
            Invalidate();
            return index;
        }
        void RemoveItem(i32 index)
        {
            if (index < 0 || index >= static_cast<i32>(m_items.Size()))
            {
                return;
            }
            m_items.RemoveAt(static_cast<usize>(index));
            if (m_selectedIndex >= static_cast<i32>(m_items.Size()))
            {
                m_selectedIndex = static_cast<i32>(m_items.Size()) - 1;
            }
            Invalidate();
        }
        void ClearItems()
        {
            m_items.Clear();
            m_selectedIndex = -1;
            Invalidate();
        }

        void OpenDropdown();
        void CloseDropdown()
        {
            m_isOpen = false;
            Invalidate();
        }

        // === IPopupOwner ===
        void OnPopupClosed(View*) override
        {
            m_isOpen = false;
            Invalidate();
        }
        [[nodiscard]] View* OwnerView() override { return this; }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled() || e.Button != MouseButton::Left)
            {
                return;
            }
            if (m_isOpen)
            {
                CloseDropdown();
            }
            else
            {
                OpenDropdown();
            }
            e.Handled = true;
        }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            if (e.Key == KeyCode::Space || e.Key == KeyCode::Return)
            {
                if (!m_isOpen)
                {
                    OpenDropdown();
                }
                e.Handled = true;
            }
            else if (e.Key == KeyCode::Up)
            {
                if (m_selectedIndex > 0)
                {
                    SetSelectedIndex(m_selectedIndex - 1);
                }
                e.Handled = true;
            }
            else if (e.Key == KeyCode::Down)
            {
                if (m_selectedIndex < static_cast<i32>(m_items.Size()) - 1)
                {
                    SetSelectedIndex(m_selectedIndex + 1);
                }
                e.Handled = true;
            }
            else if (e.Key == KeyCode::Escape && m_isOpen)
            {
                CloseDropdown();
                e.Handled = true;
            }
        }
        void OnActivate() override
        {
            if (IsEffectivelyEnabled())
            {
                OpenDropdown();
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            f32 maxTextW = 0, textH = fontSize;
            if (Context != nullptr && Context->FontService() != nullptr)
            {
                if (fonts::CachedFont* font =
                        Context->FontService()->GetFont(ResolveStyleFontFamily(), fontSize))
                {
                    textH = font->font->Metrics().lineHeight;
                    for (const String& item : m_items)
                    {
                        maxTextW = Max(maxTextW, font->font->MeasureString(item));
                    }
                }
            }
            const Thickness padding{8, 6};
            MeasuredSize = Float2{
                constraints.ConstrainWidth(padding.TotalHorizontal() + maxTextW + m_arrowAreaWidth),
                constraints.ConstrainHeight(padding.TotalVertical() + textH)};
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            const ControlState state = GetControlState();

            Drawable* bg = ResolveStyleDrawable(StyleProperty::Background);
            if (bg != nullptr)
            {
                bg->Draw(ctx, bounds, state);
            }
            else
            {
                Color bgColor{40.0f / 255.0f, 42.0f / 255.0f, 52.0f / 255.0f, 1.0f};
                if (IsHovered())
                {
                    bgColor = Palette::ComputeHover(bgColor);
                }
                ctx.VG().FillRect(bounds, bgColor);
            }

            if (m_isOpen)
            {
                const Color accent = ResolveStyleColor(
                    StyleProperty::AccentColor, Color{80.0f / 255.0f, 160.0f / 255.0f, 1.0f, 1.0f});
                if (RoundedRectDrawable* rrd = Cast<RoundedRectDrawable>(bg))
                {
                    if (!rrd->Radii.IsZero())
                    {
                        ctx.VG().StrokeRoundedRect(bounds, rrd->Radii, accent, 2.0f);
                    }
                    else
                    {
                        ctx.VG().StrokeRect(bounds, accent, 2.0f);
                    }
                }
                else
                {
                    const f32 cr = ResolveStyleFloat(StyleProperty::CornerRadius);
                    if (cr > 0)
                    {
                        ctx.VG().StrokeRoundedRect(bounds, cr, accent, 2.0f);
                    }
                    else
                    {
                        ctx.VG().StrokeRect(bounds, accent, 2.0f);
                    }
                }
            }

            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            if (m_selectedIndex >= 0 && m_selectedIndex < static_cast<i32>(m_items.Size()) &&
                ctx.FontService() != nullptr)
            {
                if (fonts::CachedFont* font =
                        ctx.FontService()->GetFont(ResolveStyleFontFamily(), fontSize))
                {
                    const Color textColor = ResolveStyleColor(
                        StyleProperty::TextColor,
                        Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
                    ctx.VG().DrawText(m_items[static_cast<usize>(m_selectedIndex)], font,
                                      Rectangle{8, 0, Width() - 16 - m_arrowAreaWidth, Height()},
                                      fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle,
                                      textColor);
                }
            }

            Drawable* arrowDrawable =
                ResolvePartDrawable(u8"arrow", StyleProperty::Background, state);
            const f32 arrowX = Width() - m_arrowAreaWidth * 0.5f;
            const f32 arrowY = Height() * 0.5f;
            if (arrowDrawable != nullptr)
            {
                arrowDrawable->Draw(ctx, Rectangle{arrowX - 4.0f, arrowY - 4.0f, 8.0f, 8.0f});
            }
            else
            {
                const Color arrowColor = ResolvePartColor(
                    u8"arrow", StyleProperty::TextColor, state,
                    Color{180.0f / 255.0f, 185.0f / 255.0f, 200.0f / 255.0f, 1.0f});
                const f32 a = 4.0f;
                ctx.VG().BeginPath();
                ctx.VG().MoveTo(arrowX - a, arrowY - a * 0.5f);
                ctx.VG().LineTo(arrowX + a, arrowY - a * 0.5f);
                ctx.VG().LineTo(arrowX, arrowY + a * 0.5f);
                ctx.VG().ClosePath();
                ctx.VG().Fill(arrowColor);
            }
        }

    private:
        Array<String> m_items;
        i32 m_selectedIndex = -1;
        bool m_isOpen = false;
        f32 m_arrowAreaWidth = 24.0f;
    };

    /// Dedicated dropdown panel for ComboBox - matches the parent width, highlights selected/hovered items.
    class ComboBoxDropdown : public View
    {
        DRACONIC_OBJECT(ComboBoxDropdown, View)
    public:
        explicit ComboBoxDropdown(ComboBox* owner) : m_owner(owner)
        {
            IsFocusable = true;
            AddClass(u8"contextmenu"); // reuse context-menu styling for the background
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            Drawable* bg = ResolveStyleDrawable(StyleProperty::Background);
            if (bg != nullptr)
            {
                bg->Draw(ctx, bounds, GetControlState());
            }
            else
            {
                ctx.VG().FillRect(bounds,
                                  Color{45.0f / 255.0f, 48.0f / 255.0f, 58.0f / 255.0f, 1.0f});
                ctx.VG().StrokeRect(
                    bounds, Color{70.0f / 255.0f, 75.0f / 255.0f, 90.0f / 255.0f, 1.0f}, 1.0f);
            }

            const Color textColor =
                ResolveStyleColor(StyleProperty::TextColor,
                                  Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
            // Dropdown hover/selection fill the item rect, so use a TRANSLUCENT tint of the theme accent
            // (the accent itself is opaque - filling with it would paint solid blocks behind the items).
            const Color accentBase =
                ResolveStyleColor(StyleProperty::AccentColor,
                                  Color{60.0f / 255.0f, 120.0f / 255.0f, 200.0f / 255.0f, 1.0f});
            const Color hoverColor{accentBase.r, accentBase.g, accentBase.b, 100.0f / 255.0f};
            const Color selectedColor{accentBase.r, accentBase.g, accentBase.b, 50.0f / 255.0f};
            Drawable* hoverDrawable = ResolveStyleDrawable(StyleProperty::MenuItemHoverDrawable);
            const f32 fontSize = m_owner->ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            fonts::CachedFont* font =
                ctx.FontService() != nullptr
                    ? ctx.FontService()->GetFont(ResolveStyleFontFamily(), fontSize)
                    : nullptr;

            f32 y = 4;
            for (i32 i = 0; i < m_owner->ItemCount(); ++i)
            {
                const Rectangle itemRect{4, y, Width() - 8, m_itemHeight};
                if (i == m_owner->SelectedIndex())
                {
                    if (RoundedRectDrawable* rrd = Cast<RoundedRectDrawable>(hoverDrawable))
                    {
                        if (!rrd->Radii.IsZero())
                        {
                            ctx.VG().FillRoundedRect(itemRect, rrd->Radii, selectedColor);
                        }
                        else
                        {
                            ctx.VG().FillRect(itemRect, selectedColor);
                        }
                    }
                    else
                    {
                        ctx.VG().FillRect(itemRect, selectedColor);
                    }
                }
                if (i == m_hoveredIndex)
                {
                    if (hoverDrawable != nullptr)
                    {
                        hoverDrawable->Draw(ctx, itemRect);
                    }
                    else
                    {
                        ctx.VG().FillRect(itemRect, hoverColor);
                    }
                }
                if (font != nullptr)
                {
                    ctx.VG().DrawText(
                        m_owner->ItemAt(i), font, Rectangle{12, y, Width() - 24, m_itemHeight},
                        fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle, textColor);
                }
                y += m_itemHeight;
            }
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            const i32 newIndex = GetIndexAt(e.Y);
            if (newIndex != m_hoveredIndex)
            {
                m_hoveredIndex = newIndex;
                Invalidate();
            }
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (e.Button != MouseButton::Left)
            {
                return;
            }
            const i32 index = GetIndexAt(e.Y);
            if (index >= 0 && index < m_owner->ItemCount())
            {
                m_owner->SetSelectedIndex(index);
                m_owner->CloseDropdown();
                QueueSelfClose();
                e.Handled = true;
            }
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            switch (e.Key)
            {
            case KeyCode::Up:
                if (m_hoveredIndex > 0)
                {
                    m_hoveredIndex--;
                }
                else if (m_hoveredIndex < 0 && m_owner->ItemCount() > 0)
                {
                    m_hoveredIndex = m_owner->ItemCount() - 1;
                }
                Invalidate();
                e.Handled = true;
                break;
            case KeyCode::Down:
                if (m_hoveredIndex < m_owner->ItemCount() - 1)
                {
                    m_hoveredIndex++;
                }
                else if (m_hoveredIndex < 0 && m_owner->ItemCount() > 0)
                {
                    m_hoveredIndex = 0;
                }
                Invalidate();
                e.Handled = true;
                break;
            case KeyCode::Return:
                if (m_hoveredIndex >= 0 && m_hoveredIndex < m_owner->ItemCount())
                {
                    m_owner->SetSelectedIndex(m_hoveredIndex);
                    m_owner->CloseDropdown();
                    QueueSelfClose();
                }
                e.Handled = true;
                break;
            case KeyCode::Escape:
                m_owner->CloseDropdown();
                QueueSelfClose();
                e.Handled = true;
                break;
            default:
                break;
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 totalH = 4 + m_itemHeight * m_owner->ItemCount() + 4;
            const f32 w = Max(m_owner->Width(), 100.0f);
            MeasuredSize =
                Float2{constraints.ConstrainWidth(w), constraints.ConstrainHeight(totalH)};
        }

    private:
        [[nodiscard]] i32 GetIndexAt(f32 localY) const
        {
            const f32 y = localY - 4;
            if (y < 0)
            {
                return -1;
            }
            const i32 index = static_cast<i32>(y / m_itemHeight);
            return (index >= m_owner->ItemCount()) ? -1 : index;
        }

        void QueueSelfClose()
        {
            // Defer closing so we don't tear the popup down mid input-dispatch.
            if (Context == nullptr)
            {
                return;
            }
            UIContext* ctx = Context;
            View* self = this;
            ctx->MutationQueueRef().QueueAction(
                Function<void()>{[ctx, self]()
                                 {
                                     if (RootView* root = ctx->ActiveInputRoot())
                                     {
                                         root->GetPopupLayer()->ClosePopup(self);
                                     }
                                 }});
        }

        ComboBox* m_owner;
        i32 m_hoveredIndex = -1;
        f32 m_itemHeight = 28.0f;
    };

    DRACONIC_DEFINE_OBJECT(ComboBox, "draconic::ui")
    DRACONIC_DEFINE_OBJECT(ComboBoxDropdown, "draconic::ui")

    // Defined out-of-line: OpenDropdown needs ComboBoxDropdown complete.
    inline void ComboBox::OpenDropdown()
    {
        if (m_isOpen || m_items.Size() == 0 || Context == nullptr)
        {
            return;
        }
        RootView* root = Context->ActiveInputRoot();
        if (root == nullptr)
        {
            return;
        }

        RefPtr<ComboBoxDropdown> dropdown = MakeRef<ComboBoxDropdown>(DefaultAllocator(), this);
        const Float2 screenPos = LocalToScreen(Float2{0, Height()});
        const Float2 logical = root->LogicalSize();
        const Rectangle screen{0, 0, logical.x, logical.y};

        dropdown->Measure(BoxConstraints::Loose(screen.width, screen.height));
        const f32 gap = 2.0f; // sit just below the box's bottom border instead of overlapping it
        f32 sy = screenPos.y + gap;
        if (sy + dropdown->MeasuredSize.y > screen.height)
        {
            sy = screenPos.y - Height() - gap - dropdown->MeasuredSize.y;
        } // flip above

        root->GetPopupLayer()->ShowPopup(dropdown.Get(), this, screenPos.x, sy, true, false, true);
        m_isOpen = true;
        Invalidate();
    }
}
