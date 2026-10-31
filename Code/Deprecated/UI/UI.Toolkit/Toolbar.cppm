// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :toolbar partition
//
// Horizontal toolbar container and its item family (ToolbarItem, ToolbarSeparator, ToolbarButton,
// ToolbarToggle, ToolbarMenuButton). They share one partition because the buttons draw by resolving
// styles off their parent Toolbar, a mutual reference the module-partition graph cannot split. The
// buttons' OnDraw bodies are defined out of line, since they need the complete Toolbar type.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:toolbar;

import foundation.core;
import foundation.vg;
import foundation.fonts;
import foundation.ui;

using namespace foundation::core;
namespace fonts = foundation::fonts;

namespace foundation::ui::toolkit
{
    // Module-linkage helper (not exported, but not TU-local either, so the exported controls' inline
    // draws below may reference it under GCC's stricter modules rules).
    [[nodiscard]] inline Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
    {
        return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }
}

export namespace foundation::ui::toolkit
{
    class
        Toolbar; // forward: items resolve styles off their parent Toolbar (OnDraw defined out of line)

    /// Base class for items in a Toolbar. Any View can be a toolbar item.
    class ToolbarItem : public View
    {
        RTTI_OBJECT(ToolbarItem, View)
    };

    /// Toolbar separator: a vertical divider line.
    class ToolbarSeparator : public ToolbarItem
    {
        RTTI_OBJECT(ToolbarSeparator, ToolbarItem)
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

    /// Toolbar button: text, an icon (via a custom draw delegate), or both.
    class ToolbarButton : public ToolbarItem
    {
        RTTI_OBJECT(ToolbarButton, ToolbarItem)
    public:
        Event<void(ToolbarButton*)> OnClick;

        ToolbarButton()
        {
            IsFocusable = true;
            Cursor = CursorType::Hand;
        }

        /// Invalidates only on a change: a page syncs its toolbar every frame, and an unconditional
        /// invalidate would re-lay-out the whole editor each frame.
        void SetText(StringView text)
        {
            if (m_text.AsView() == text)
            {
                return;
            }
            m_text.Clear();
            m_text.Append(text);
            Invalidate();
        }
        [[nodiscard]] StringView Text() const noexcept { return m_text.AsView(); }

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

    /// Toolbar toggle button: an on/off state with an accent background when active.
    class ToolbarToggle : public ToolbarButton
    {
        RTTI_OBJECT(ToolbarToggle, ToolbarButton)
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

    /// Toolbar menu button: a button that opens a popup (OnClick, the caller shows a
    /// ContextMenu under it) and carries a checked state like a toggle, meaning the mode this
    /// dropdown holds is active. It is drawn with a caret after its text.
    class ToolbarMenuButton : public ToolbarButton
    {
        RTTI_OBJECT(ToolbarMenuButton, ToolbarButton)
    public:
        static constexpr f32 kCaretWidth = 12.0f;

        [[nodiscard]] bool IsChecked() const noexcept { return m_isChecked; }

        void SetIsChecked(bool value)
        {
            if (m_isChecked != value)
            {
                m_isChecked = value;
                Invalidate();
            }
        }

        void OnDraw(UIDrawContext& ctx) override; // defined out-of-line (needs complete Toolbar)

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            ToolbarButton::OnMeasure(constraints);
            MeasuredSize.x = constraints.ConstrainWidth(MeasuredSize.x + kCaretWidth);
        }

    private:
        bool m_isChecked = false;
    };

    /// Horizontal toolbar container. Draws background with bottom border.
    /// Add items via AddItem(), AddButton(), AddSeparator(), AddToggle(), AddMenuButton().
    class Toolbar : public FlexLayout
    {
        RTTI_OBJECT(Toolbar, FlexLayout)
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
            LayoutStyle lp;
            lp.Height = SizeSpec::Match();
            AddView(item, lp);
        }

        /// Add any view with its own layout, such as a slider with a set width or a label centred on the bar.
        void AddItem(View* item, const LayoutStyle& style) { AddView(item, style); }

        /// Add a label centred on the bar (a status line, a caption). Returns it borrowed.
        Label* AddLabel(StringView text)
        {
            RefPtr<Label> label = MakeRef<Label>(MemoryAllocator(), text);
            label->FontSize.SetValue(12.0f);
            LayoutStyle style;
            style.AlignSelf = Align::Center;
            Label* raw = label.Get();
            AddView(label.Get(), style);
            return raw;
        }

        /// Add a text button. Returns the borrowed button for further configuration.
        ToolbarButton* AddButton(StringView text)
        {
            RefPtr<ToolbarButton> btn = MakeRef<ToolbarButton>(MemoryAllocator());
            btn->SetText(text);
            ToolbarButton* raw = btn.Get();
            AddItem(btn.Get());
            return raw;
        }

        /// Add a separator. Returns the borrowed separator.
        ToolbarSeparator* AddSeparator()
        {
            RefPtr<ToolbarSeparator> sep = MakeRef<ToolbarSeparator>(MemoryAllocator());
            ToolbarSeparator* raw = sep.Get();
            AddItem(sep.Get());
            return raw;
        }

        /// Add a toggle button. Returns the borrowed toggle for further configuration.
        ToolbarToggle* AddToggle(StringView text)
        {
            RefPtr<ToolbarToggle> toggle = MakeRef<ToolbarToggle>(MemoryAllocator());
            toggle->SetText(text);
            ToolbarToggle* raw = toggle.Get();
            AddItem(toggle.Get());
            return raw;
        }

        /// Add a menu (dropdown) button. Returns the borrowed button; wire OnClick to show a menu.
        ToolbarMenuButton* AddMenuButton(StringView text)
        {
            RefPtr<ToolbarMenuButton> button = MakeRef<ToolbarMenuButton>(MemoryAllocator());
            button->SetText(text);
            ToolbarMenuButton* raw = button.Get();
            AddItem(button.Get());
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

        // Hover background, derived from the parent toolbar's background.
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
            f32 fontSize = 13.0f;
            if (Toolbar* toolbar = Cast<Toolbar>(Parent))
            {
                fontSize = toolbar->ResolveStyleFloat(StyleProperty::FontSize, fontSize);
            }
            fonts::CachedFont* font = ctx.FontService()->GetFont(fontSize);
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

    RTTI_DEFINE_OBJECT(ToolbarItem, "rtti::ui::toolkit")
    RTTI_DEFINE_OBJECT(ToolbarSeparator, "rtti::ui::toolkit")
    RTTI_DEFINE_OBJECT(ToolbarButton, "rtti::ui::toolkit")
    inline void ToolbarMenuButton::OnDraw(UIDrawContext& ctx)
    {
        const Rectangle bounds{0, 0, Width(), Height()};
        if (m_isChecked)
        {
            const f32 cornerR = ResolveStyleFloat(StyleProperty::CornerRadius, 0.0f);
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

        // The caret: a small down-pointing triangle in the reserved strip at the right.
        const Color caretColor = ResolveStyleColor(StyleProperty::TextColor, Rgb(220, 225, 235, 255));
        const f32 cx = Width() - kCaretWidth * 0.5f - 2.0f;
        const f32 cy = Height() * 0.5f;
        const Float2 points[3] = {Float2{cx - 4.0f, cy - 2.0f}, Float2{cx + 4.0f, cy - 2.0f},
                                  Float2{cx, cy + 3.0f}};
        ctx.VG().FillPolygon(Span<const Float2>(points, 3), caretColor);
    }

    RTTI_DEFINE_OBJECT(ToolbarToggle, "rtti::ui::toolkit")
    RTTI_DEFINE_OBJECT(ToolbarMenuButton, "rtti::ui::toolkit")
    RTTI_DEFINE_OBJECT(Toolbar, "rtti::ui::toolkit")
}
