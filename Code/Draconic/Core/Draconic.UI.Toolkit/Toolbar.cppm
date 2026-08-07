// Draconic UI Toolkit - :toolbar partition
//
// Horizontal toolbar container + its item family (ToolbarItem / ToolbarSeparator / ToolbarButton /
// ToolbarToggle). Ported from Sedulous.UI.Toolkit/src/Toolbar.bf. All five classes live in one partition
// because ToolbarButton/ToolbarToggle draw by resolving styles off their parent Toolbar (a mutual
// reference the module-partition DAG cannot split). Beef `String mText` -> String m_text (empty == null);
// `delegate void(UIDrawContext, RectangleF) mIconDraw` -> Function<void(UIDrawContext&, Rectangle)>;
// `Event<delegate void(ToolbarButton)>` -> Event<void(ToolbarButton*)>; `x as Toolbar` -> Cast<Toolbar>(x).
// ToolbarButton/ToolbarToggle OnDraw are defined out-of-line (need the complete Toolbar type).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:toolbar;

import draconic.foundation;
import draconic.vg;
import draconic.fonts;
import draconic.ui;

using namespace draconic::foundation;

namespace draconic::ui::toolkit
{
    // Module-linkage helper (not exported, but not TU-local either, so the exported controls' inline
    // draws below may reference it under GCC's stricter modules rules).
    [[nodiscard]] inline Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
    {
        return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }
}

export namespace draconic::ui::toolkit
{
    class
        Toolbar; // forward - items resolve styles off their parent Toolbar (OnDraw defined out-of-line)

    /// Base class for items in a Toolbar. Any View can be a toolbar item.
    class ToolbarItem : public View
    {
        DRACONIC_OBJECT(ToolbarItem, View)
    };

    /// Toolbar separator - vertical divider line.
    class ToolbarSeparator : public ToolbarItem
    {
        DRACONIC_OBJECT(ToolbarSeparator, ToolbarItem)
    public:
        void OnDraw(UIDrawContext& ctx) override
        {
            const Color color =
                ResolveStyleColor(StyleProperty::BorderColor, Rgb(80, 85, 100, 255));
            const f32 cx = Width() * 0.5f;
            const f32 margin = Height() * 0.2f;
            ctx.VG().FillRect(Rectangle{cx, margin, 1.0f, Height() - margin * 2.0f}, color);
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize =
                Float2{constraints.ConstrainWidth(8.0f), constraints.ConstrainHeight(0.0f)};
        }
    };

    /// Toolbar button - supports text, icon (via custom draw delegate), or both.
    class ToolbarButton : public ToolbarItem
    {
        DRACONIC_OBJECT(ToolbarButton, ToolbarItem)
    public:
        Event<void(ToolbarButton*)> OnClick;

        ToolbarButton()
        {
            IsFocusable = true;
            Cursor = CursorType::Hand;
        }

        void SetText(StringView text)
        {
            m_text.Clear();
            m_text.Append(text);
            Invalidate();
        }

        /// Set a custom icon draw delegate. Drawn in the icon area before text.
        void SetIcon(Function<void(UIDrawContext&, Rectangle)> iconDraw)
        {
            m_iconDraw = Move(iconDraw);
        }

        void OnDraw(UIDrawContext& ctx) override; // defined out-of-line (needs complete Toolbar)

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled() || e.Button != MouseButton::Left)
            {
                return;
            }
            OnClick.Invoke(this);
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
                OnClick.Invoke(this);
                e.Handled = true;
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            f32 w = 8.0f; // padding
            f32 textH = 14.0f;

            if (m_iconDraw)
            {
                w += 16.0f;
            } // icon area

            if (!m_text.IsEmpty())
            {
                if (m_iconDraw)
                {
                    w += 4.0f;
                } // gap between icon and text
                if (Context != nullptr && Context->FontService() != nullptr)
                {
                    fonts::CachedFont* font = Context->FontService()->GetFont(13.0f);
                    if (font != nullptr)
                    {
                        w += font->font->MeasureString(m_text);
                        textH = font->font->Metrics().lineHeight;
                    }
                }
            }

            w += 8.0f; // right padding
            MeasuredSize =
                Float2{constraints.ConstrainWidth(w), constraints.ConstrainHeight(textH + 8.0f)};
        }

        String m_text;                                        // empty == "no text"
        Function<void(UIDrawContext&, Rectangle)> m_iconDraw; // null == "no icon"
    };

    /// Toolbar toggle button - on/off state with accent background when active.
    class ToolbarToggle : public ToolbarButton
    {
        DRACONIC_OBJECT(ToolbarToggle, ToolbarButton)
    public:
        Event<void(ToolbarToggle*, bool)> OnCheckedChanged;

        [[nodiscard]] bool IsChecked() const noexcept { return m_isChecked; }

        void SetIsChecked(bool value)
        {
            if (m_isChecked != value)
            {
                m_isChecked = value;
                Invalidate();
                OnCheckedChanged.Invoke(this, value);
            }
        }

        void OnDraw(UIDrawContext& ctx) override; // defined out-of-line (needs complete Toolbar)

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled() || e.Button != MouseButton::Left)
            {
                return;
            }
            SetIsChecked(!m_isChecked);
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
                SetIsChecked(!m_isChecked);
                e.Handled = true;
            }
        }

    private:
        bool m_isChecked = false;
    };

    /// Horizontal toolbar container. Draws background with bottom border.
    /// Add items via AddItem(), AddButton(), AddSeparator(), AddToggle().
    class Toolbar : public FlexLayout
    {
        DRACONIC_OBJECT(Toolbar, FlexLayout)
    public:
        Toolbar()
        {
            Direction = Orientation::Horizontal;
            Spacing = 2.0f;
            Padding = Thickness(4.0f);
        }

        /// Add any ToolbarItem (or View). The child tree takes a ref.
        void AddItem(View* item)
        {
            RefPtr<FlexLayoutParams> lp = MakeRef<FlexLayoutParams>(DefaultAllocator());
            lp->Height = SizeSpec::Match();
            AddView(item, lp);
        }

        /// Add a text button. Returns the borrowed button for further configuration.
        ToolbarButton* AddButton(StringView text)
        {
            RefPtr<ToolbarButton> btn = MakeRef<ToolbarButton>(DefaultAllocator());
            btn->SetText(text);
            ToolbarButton* raw = btn.Get();
            AddItem(btn.Get());
            return raw;
        }

        /// Add a separator. Returns the borrowed separator.
        ToolbarSeparator* AddSeparator()
        {
            RefPtr<ToolbarSeparator> sep = MakeRef<ToolbarSeparator>(DefaultAllocator());
            ToolbarSeparator* raw = sep.Get();
            AddItem(sep.Get());
            return raw;
        }

        /// Add a toggle button. Returns the borrowed toggle for further configuration.
        ToolbarToggle* AddToggle(StringView text)
        {
            RefPtr<ToolbarToggle> toggle = MakeRef<ToolbarToggle>(DefaultAllocator());
            toggle->SetText(text);
            ToolbarToggle* raw = toggle.Get();
            AddItem(toggle.Get());
            return raw;
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            // Background.
            if (Drawable* bgDrawable = ResolveStyleDrawable(StyleProperty::Background))
            {
                bgDrawable->Draw(ctx, Rectangle{0, 0, Width(), Height()});
            }
            else
            {
                ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()}, Rgb(35, 37, 46, 255));
            }

            // Bottom border.
            const Color borderColor =
                ResolveStyleColor(StyleProperty::BorderColor, Rgb(65, 70, 85, 255));
            ctx.VG().FillRect(Rectangle{0, Height() - 1.0f, Width(), 1.0f}, borderColor);

            DrawChildren(ctx);
        }
    };

    // === Out-of-line item draws (need the complete Toolbar type) ===

    inline void ToolbarButton::OnDraw(UIDrawContext& ctx)
    {
        const Rectangle bounds{0, 0, Width(), Height()};

        // Hover background - derive from parent toolbar's background.
        if (IsHovered())
        {
            Color bgColor = Rgb(50, 52, 62, 255);
            const f32 cornerR =
                ResolveStyleFloat(StyleProperty::CornerRadius, 0.0f); // theme radius
            if (Toolbar* toolbar = Cast<Toolbar>(Parent))
            {
                Drawable* bg = toolbar->ResolveStyleDrawable(StyleProperty::Background);
                if (RoundedRectDrawable* rrd = Cast<RoundedRectDrawable>(bg))
                {
                    bgColor = rrd->FillColor;
                }
                else if (ColorDrawable* cd = Cast<ColorDrawable>(bg))
                {
                    bgColor = cd->Color;
                }
            }
            const Color hoverBg = Palette::ComputeHover(bgColor);
            if (cornerR > 0.0f)
            {
                ctx.VG().FillRoundedRect(bounds, cornerR, hoverBg);
            }
            else
            {
                ctx.VG().FillRect(bounds, hoverBg);
            }
        }

        f32 x = 8.0f;

        // Icon.
        if (m_iconDraw)
        {
            const Rectangle iconRect{x, (Height() - 16.0f) * 0.5f, 16.0f, 16.0f};
            m_iconDraw(ctx, iconRect);
            x += 16.0f;
        }

        // Text.
        if (!m_text.IsEmpty() && ctx.FontService() != nullptr)
        {
            if (m_iconDraw)
            {
                x += 4.0f;
            }
            fonts::CachedFont* font = ctx.FontService()->GetFont(13.0f);
            if (font != nullptr)
            {
                const Color textColor =
                    ResolveStyleColor(StyleProperty::TextColor, Rgb(220, 225, 235, 255));
                ctx.VG().DrawText(m_text, font, Rectangle{x, 0, Width() - x - 8.0f, Height()},
                                  fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle,
                                  textColor);
            }
        }
    }

    inline void ToolbarToggle::OnDraw(UIDrawContext& ctx)
    {
        const Rectangle bounds{0, 0, Width(), Height()};

        // Active toggle: muted accent background from toolbar's SelectionColor.
        if (m_isChecked)
        {
            const f32 cornerR =
                ResolveStyleFloat(StyleProperty::CornerRadius, 0.0f); // theme radius
            Color onColor = Rgb(40, 80, 160, 255);
            if (Toolbar* toolbar = Cast<Toolbar>(Parent))
            {
                onColor = toolbar->ResolveStyleColor(StyleProperty::SelectionColor, onColor);
            }
            if (cornerR > 0.0f)
            {
                ctx.VG().FillRoundedRect(bounds, cornerR, onColor);
            }
            else
            {
                ctx.VG().FillRect(bounds, onColor);
            }
        }

        ToolbarButton::OnDraw(ctx);
    }

    DRACONIC_DEFINE_OBJECT(ToolbarItem, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(ToolbarSeparator, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(ToolbarButton, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(ToolbarToggle, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(Toolbar, "draconic::ui::toolkit")
}
