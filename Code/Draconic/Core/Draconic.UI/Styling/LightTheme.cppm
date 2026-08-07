// Draconic UI - :light_theme partition
//
// Factory building a light theme as a StyleSheet (flat ColorDrawable regions + tinted SVG icons). Ported
// from Sedulous.UI/src/Styling/LightTheme.bf - structurally identical to DarkTheme.cppm with different
// literal colours and dark-tinted icons; see that file's header for the conventions.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:light_theme;

import draconic.foundation;
import :style_sheet;
import :style_rule;
import :style_property;
import :control_state;
import :thickness;
import :palette;
import :theme_palette;
import :theme_registry;
import :theme_icons;
import :rounded_rect_drawable;
import :state_list_drawable;
import :color_drawable;
import :svg_drawable;
import :drawable;
import :view;
import :button_base;
import :edit_text;
import :numeric_field;
import :checkbox;
import :radio_button;
import :slider;
import :progress_bar;
import :toggle_switch;
import :combo_box;
import :scroll_bar;
import :separator;
import :expander;
import :tab_view;
import :dialog;
import :tooltip_view;
import :list_view;
import :tree_view;
import :grid_view;

using namespace draconic::foundation;

export namespace draconic::ui
{
    /// Factory for creating a light theme as a StyleSheet.
    struct LightTheme
    {
        [[nodiscard]] static RefPtr<StyleSheet> Create()
        {
            return BuildTheme(ThemePalette::Light());
        }
        [[nodiscard]] static RefPtr<StyleSheet> Create(ThemePalette palette)
        {
            return BuildTheme(palette);
        }

    private:
        [[nodiscard]] static Color C(f32 r, f32 g, f32 b, f32 a)
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        [[nodiscard]] static RefPtr<StyleSheet> BuildTheme(ThemePalette p)
        {
            RefPtr<StyleSheet> sheetRef = MakeRef<StyleSheet>(DefaultAllocator());
            StyleSheet& sheet = *sheetRef;

            sheet.ForType(&View::StaticType())
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::FontSize, 16.0f);

            // Button
            RefPtr<Drawable> btnBg = Palette::CreateStateColors(C(220, 222, 230, 255));
            RefPtr<Drawable> btnChecked = Palette::CreateStateColors(p.PrimaryAccent);
            sheet.OwnDrawable(btnBg);
            sheet.OwnDrawable(btnChecked);
            sheet.ForType(&ButtonBase::StaticType())
                .Set(StyleProperty::Background, btnBg)
                .Set(StyleProperty::CheckedBackground, btnChecked)
                .Set(StyleProperty::TextColor, C(30, 30, 40, 255))
                .Set(StyleProperty::Padding, Thickness{12, 8})
                .Set(StyleProperty::CornerRadius, 0.0f);

            // Panel
            RefPtr<Drawable> panelBg =
                MakeRef<RoundedRectDrawable>(DefaultAllocator(), p.Surface, 0.0f, p.Border, 1.0f);
            sheet.OwnDrawable(panelBg);
            sheet.ForClass(u8"panel").Set(StyleProperty::Background, panelBg);

            // Label
            sheet.ForClass(u8"label").Set(StyleProperty::TextColor, p.Text);
            sheet.ForClass(u8"label-dim").Set(StyleProperty::TextColor, p.TextDim);

            // EditText
            RefPtr<Drawable> editBg =
                MakeRef<RoundedRectDrawable>(DefaultAllocator(), p.Surface, 0.0f, p.Border, 1.0f);
            sheet.OwnDrawable(editBg);
            sheet.ForType(&EditText::StaticType())
                .Set(StyleProperty::Background, editBg)
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::PlaceholderColor, p.TextDim)
                .Set(StyleProperty::FontSize, 14.0f)
                .Set(StyleProperty::Padding, Thickness{6, 4})
                .Set(StyleProperty::CursorColor, p.PrimaryAccent)
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 60));

            // NumericField
            RefPtr<Drawable> spinBg = Palette::CreateStateColors(Palette::Darken(p.Surface, 0.08f));
            sheet.OwnDrawable(spinBg);
            sheet.ForType(&NumericField::StaticType())
                .Set(StyleProperty::Background, editBg)
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::PlaceholderColor, p.TextDim)
                .Set(StyleProperty::FontSize, 14.0f)
                .Set(StyleProperty::Padding, Thickness{6, 4})
                .Set(StyleProperty::CursorColor, p.PrimaryAccent)
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 60));
            sheet.ForTypePseudo(&NumericField::StaticType(), u8"spin-up")
                .Set(StyleProperty::Background, spinBg);
            sheet.ForTypePseudo(&NumericField::StaticType(), u8"spin-down")
                .Set(StyleProperty::Background, spinBg);

            // CheckBox
            RefPtr<Drawable> cbUnchecked =
                MakeRef<RoundedRectDrawable>(DefaultAllocator(), p.Surface, 0.0f, p.Border, 1.0f);
            RefPtr<Drawable> cbChecked = MakeRef<RoundedRectDrawable>(
                DefaultAllocator(), p.PrimaryAccent, 0.0f, p.Border, 1.0f);
            sheet.OwnDrawable(cbUnchecked);
            sheet.OwnDrawable(cbChecked);
            sheet.ForTypePseudo(&CheckBox::StaticType(), u8"box")
                .Set(StyleProperty::Background, cbUnchecked)
                .Set(StyleProperty::Width, 18.0f);
            sheet.ForTypePseudoState(&CheckBox::StaticType(), u8"box", ControlState::Checked)
                .Set(StyleProperty::Background, cbChecked);
            sheet.ForType(&CheckBox::StaticType()).Set(StyleProperty::Spacing, 6.0f);

            // RadioButton
            RefPtr<Drawable> rbUnchecked =
                MakeRef<RoundedRectDrawable>(DefaultAllocator(), p.Surface, 0.0f, p.Border, 1.0f);
            RefPtr<Drawable> rbChecked = MakeRef<RoundedRectDrawable>(
                DefaultAllocator(), p.PrimaryAccent, 0.0f, p.Border, 1.0f);
            sheet.OwnDrawable(rbUnchecked);
            sheet.OwnDrawable(rbChecked);
            sheet.ForTypePseudo(&RadioButton::StaticType(), u8"box")
                .Set(StyleProperty::Background, rbUnchecked);
            sheet.ForTypePseudoState(&RadioButton::StaticType(), u8"box", ControlState::Checked)
                .Set(StyleProperty::Background, rbChecked);

            // Slider
            sheet.ForTypePseudo(&Slider::StaticType(), u8"track")
                .Set(StyleProperty::Background, sheet.OwnColor(C(210, 215, 225, 255)))
                .Set(StyleProperty::Height, 4.0f);
            sheet.ForTypePseudo(&Slider::StaticType(), u8"fill")
                .Set(StyleProperty::Background, sheet.OwnColor(p.PrimaryAccent));
            sheet.ForTypePseudo(&Slider::StaticType(), u8"thumb")
                .Set(StyleProperty::Background, sheet.OwnColor(p.PrimaryAccent))
                .Set(StyleProperty::Width, 16.0f);

            // ProgressBar
            sheet.ForTypePseudo(&ProgressBar::StaticType(), u8"track")
                .Set(StyleProperty::Background, sheet.OwnColor(C(210, 215, 225, 255)));
            sheet.ForTypePseudo(&ProgressBar::StaticType(), u8"fill")
                .Set(StyleProperty::Background, sheet.OwnColor(p.PrimaryAccent));

            // ToggleSwitch
            {
                RefPtr<Drawable> swOff = MakeRef<RoundedRectDrawable>(
                    DefaultAllocator(), C(200, 205, 215, 255), 0.0f, p.Border, 1.0f);
                RefPtr<Drawable> swOn = MakeRef<RoundedRectDrawable>(
                    DefaultAllocator(), p.PrimaryAccent, 0.0f, p.Border, 1.0f);
                sheet.OwnDrawable(swOff);
                sheet.OwnDrawable(swOn);
                sheet.ForTypePseudo(&ToggleSwitch::StaticType(), u8"track")
                    .Set(StyleProperty::Background, swOff);
                sheet
                    .ForTypePseudoState(&ToggleSwitch::StaticType(), u8"track",
                                        ControlState::Checked)
                    .Set(StyleProperty::Background, swOn);
                sheet.ForTypePseudo(&ToggleSwitch::StaticType(), u8"knob")
                    .Set(StyleProperty::Background, sheet.OwnColor(p.Surface));
            }

            // ComboBox
            RefPtr<Drawable> comboBg =
                MakeRef<RoundedRectDrawable>(DefaultAllocator(), p.Surface, 0.0f, p.Border, 1.0f);
            sheet.OwnDrawable(comboBg);
            sheet.ForType(&ComboBox::StaticType()).Set(StyleProperty::Background, comboBg);
            sheet.ForTypePseudo(&ComboBox::StaticType(), u8"arrow")
                .Set(StyleProperty::TextColor, C(80, 85, 100, 255));

            // ScrollBar
            sheet.ForTypePseudo(&ScrollBar::StaticType(), u8"track")
                .Set(StyleProperty::Background, sheet.OwnColor(C(230, 232, 240, 150)));
            sheet.ForTypePseudo(&ScrollBar::StaticType(), u8"thumb")
                .Set(StyleProperty::Background, sheet.OwnColor(C(160, 165, 180, 200)));

            // Separator
            sheet.ForType(&Separator::StaticType()).Set(StyleProperty::BorderColor, p.Border);

            // Expander
            sheet.ForTypePseudo(&Expander::StaticType(), u8"header")
                .Set(StyleProperty::Background, sheet.OwnColor(C(235, 238, 245, 255)));
            sheet.ForTypePseudoState(&Expander::StaticType(), u8"header", ControlState::Hover)
                .Set(StyleProperty::Background,
                     sheet.OwnColor(Palette::Darken(C(235, 238, 245, 255), 0.05f)));
            sheet.ForTypePseudo(&Expander::StaticType(), u8"chevron")
                .Set(StyleProperty::TextColor, C(80, 85, 100, 255));

            // TabView
            sheet.ForTypePseudo(&TabView::StaticType(), u8"strip")
                .Set(StyleProperty::Background, sheet.OwnColor(Palette::Darken(p.Surface, 0.05f)));
            sheet.ForTypePseudo(&TabView::StaticType(), u8"content")
                .Set(StyleProperty::Background, sheet.OwnColor(p.Surface));
            sheet.ForTypePseudoState(&TabView::StaticType(), u8"tab", ControlState::Checked)
                .Set(StyleProperty::Background, sheet.OwnColor(p.Surface));
            sheet.ForTypePseudoState(&TabView::StaticType(), u8"tab", ControlState::Hover)
                .Set(StyleProperty::Background, sheet.OwnColor(Palette::Darken(p.Surface, 0.03f)));
            sheet.ForTypePseudo(&TabView::StaticType(), u8"tab")
                .Set(StyleProperty::TextColor, p.TextDim);
            sheet.ForTypePseudoState(&TabView::StaticType(), u8"tab", ControlState::Checked)
                .Set(StyleProperty::TextColor, p.Text);
            sheet.ForTypePseudoState(&TabView::StaticType(), u8"tab", ControlState::Hover)
                .Set(StyleProperty::TextColor, Palette::Darken(p.TextDim, 0.2f));
            sheet.ForTypePseudo(&TabView::StaticType(), u8"close-button")
                .Set(StyleProperty::TextColor, p.TextDim)
                .Set(StyleProperty::Width, 12.0f);
            sheet.ForTypePseudoState(&TabView::StaticType(), u8"close-button", ControlState::Hover)
                .Set(StyleProperty::TextColor, p.Text);
            sheet.ForType(&TabView::StaticType())
                .Set(StyleProperty::BorderColor, p.Border)
                .Set(StyleProperty::AccentColor, p.PrimaryAccent);

            // ContextMenu
            RefPtr<Drawable> menuBg =
                MakeRef<RoundedRectDrawable>(DefaultAllocator(), p.Surface, 0.0f, p.Border, 1.0f);
            sheet.OwnDrawable(menuBg);
            RefPtr<Drawable> menuHover =
                MakeRef<RoundedRectDrawable>(DefaultAllocator(), C(60, 120, 200, 60), 0.0f);
            sheet.OwnDrawable(menuHover);
            sheet.ForClass(u8"contextmenu")
                .Set(StyleProperty::Background, menuBg)
                .Set(StyleProperty::MenuItemHoverDrawable, menuHover)
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::BorderColor, p.Border)
                .Set(StyleProperty::AccentColor, C(60, 120, 200, 60));

            // Dialog
            RefPtr<Drawable> dialogBg =
                MakeRef<RoundedRectDrawable>(DefaultAllocator(), p.Surface, 0.0f, p.Border, 1.0f);
            sheet.OwnDrawable(dialogBg);
            sheet.ForType(&Dialog::StaticType()).Set(StyleProperty::Background, dialogBg);

            // Tooltip
            RefPtr<Drawable> tooltipBg = MakeRef<RoundedRectDrawable>(
                DefaultAllocator(), C(255, 255, 225, 245), 0.0f, C(180, 175, 140, 255), 1.0f);
            sheet.OwnDrawable(tooltipBg);
            sheet.ForType(&TooltipView::StaticType())
                .Set(StyleProperty::Background, tooltipBg)
                .Set(StyleProperty::TextColor, p.Text);

            // List/Tree/GridView
            sheet.ForType(&ListView::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Background))
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 60));
            sheet.ForType(&TreeView::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Background));
            sheet.ForType(&GridView::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Background))
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 60));

            RegisterIcons(sheet);
            ThemeRegistry::ApplyExtensions(sheet, p);
            return sheetRef;
        }

        static void RegisterIcons(StyleSheet& sheet)
        {
            const Color tint = C(60, 60, 70, 255); // dark icon tint for the light theme
            if (RefPtr<Drawable> checkmark = SVGDrawable::FromString(ThemeIcons::Checkmark(), tint))
            {
                sheet.OwnDrawable(checkmark);
                sheet.ForTypePseudo(&CheckBox::StaticType(), u8"checkmark")
                    .Set(StyleProperty::Background, checkmark);
            }
            if (RefPtr<Drawable> radioMark =
                    SVGDrawable::FromString(ThemeIcons::RadioMarkSquare(), tint))
            {
                sheet.OwnDrawable(radioMark);
                sheet.ForTypePseudo(&RadioButton::StaticType(), u8"mark")
                    .Set(StyleProperty::Background, radioMark);
            }
            if (RefPtr<Drawable> closeIcon = SVGDrawable::FromString(ThemeIcons::Close(), tint))
            {
                sheet.OwnDrawable(closeIcon);
                sheet.ForTypePseudo(&TabView::StaticType(), u8"close-button")
                    .Set(StyleProperty::Background, closeIcon);
            }
            if (RefPtr<Drawable> chevExpanded =
                    SVGDrawable::FromString(ThemeIcons::ChevronDown(), tint))
            {
                sheet.OwnDrawable(chevExpanded);
                sheet
                    .ForTypePseudoState(&Expander::StaticType(), u8"chevron", ControlState::Checked)
                    .Set(StyleProperty::Background, chevExpanded);
            }
            if (RefPtr<Drawable> chevCollapsed =
                    SVGDrawable::FromString(ThemeIcons::ChevronRight(), tint))
            {
                sheet.OwnDrawable(chevCollapsed);
                sheet.ForTypePseudo(&Expander::StaticType(), u8"chevron")
                    .Set(StyleProperty::Background, chevCollapsed);
            }
            if (RefPtr<Drawable> tvChevExpanded =
                    SVGDrawable::FromString(ThemeIcons::ChevronDown(), tint))
            {
                sheet.OwnDrawable(tvChevExpanded);
                sheet
                    .ForTypePseudoState(&TreeView::StaticType(), u8"chevron", ControlState::Checked)
                    .Set(StyleProperty::Background, tvChevExpanded);
            }
            if (RefPtr<Drawable> tvChevCollapsed =
                    SVGDrawable::FromString(ThemeIcons::ChevronRight(), tint))
            {
                sheet.OwnDrawable(tvChevCollapsed);
                sheet.ForTypePseudo(&TreeView::StaticType(), u8"chevron")
                    .Set(StyleProperty::Background, tvChevCollapsed);
            }
            if (RefPtr<Drawable> subArrow =
                    SVGDrawable::FromString(ThemeIcons::ChevronRight(), tint))
            {
                sheet.OwnDrawable(subArrow);
                RefPtr<StyleRule> rule = MakeRef<StyleRule>(DefaultAllocator());
                rule->Selector.AddClass(u8"contextmenu");
                rule->Selector.SetPseudoElement(u8"submenu-arrow");
                rule->Set(StyleProperty::Background, subArrow);
                sheet.AddRule(Move(rule));
            }
            if (RefPtr<Drawable> arrowDown = SVGDrawable::FromString(ThemeIcons::ArrowDown(), tint))
            {
                sheet.OwnDrawable(arrowDown);
                sheet.ForTypePseudo(&ComboBox::StaticType(), u8"arrow")
                    .Set(StyleProperty::Background, arrowDown);
            }
            if (RefPtr<Drawable> arrowUp = SVGDrawable::FromString(ThemeIcons::ArrowUp(), tint))
            {
                sheet.OwnDrawable(arrowUp);
                sheet.ForTypePseudo(&NumericField::StaticType(), u8"arrow-up")
                    .Set(StyleProperty::Background, arrowUp);
            }
            if (RefPtr<Drawable> arrowDn2 = SVGDrawable::FromString(ThemeIcons::ArrowDown(), tint))
            {
                sheet.OwnDrawable(arrowDn2);
                sheet.ForTypePseudo(&NumericField::StaticType(), u8"arrow-down")
                    .Set(StyleProperty::Background, arrowDn2);
            }
        }
    };
}
