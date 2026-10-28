// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :dark_theme partition
//
// Factory building the default dark theme as a StyleSheet (all visual regions use Drawable properties;
// ColorDrawable for the flat theme). Ported from Sedulous.UI/src/Styling/DarkTheme.bf. Beef `static class`
// -> a struct of static factories; `new StyleSheet` (refcount 1, caller-managed) -> RefPtr<StyleSheet>;
// Beef `Color(r,g,b,a)` byte literals -> the local C() helper (/255); `typeof(T)` -> &T::StaticType();
// OwnDrawable shares the RefPtr (kept-alive list) - the owning StyleValue keeps it regardless, but the
// calls are retained for faithfulness. Drawables are RefPtr (no manual AddRef).

module;
#include "Core/Prelude.h"

export module foundation.ui:dark_theme;

import foundation.core;
import :style_sheet;
import :style_rule;
import :style_property;
import :control_state;
import :thickness;
import :palette;
import :theme_palette;
import :theme_registry;
import :style_sheet_loader;
import :embedded_themes;
import :theme_icons;
import :theme_icon_set;
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

using namespace foundation::core;

export namespace foundation::ui
{
    /// Factory for creating the default dark theme as a StyleSheet.
    ///
    /// The theme is AUTHORED as Styling/Themes/dark.sss (embedded at build - see
    /// EmbeddedThemes) and parsed here. The legacy C++ rule
    /// builder is kept ONLY as the parity oracle (ThemeParityTests diffs the two sheets
    /// rule-for-rule).
    struct DarkTheme
    {
        [[nodiscard]] static RefPtr<StyleSheet> Create(IAllocator& allocator)
        {
            return Create(allocator, ThemePalette::Dark());
        }
        [[nodiscard]] static RefPtr<StyleSheet> Create(IAllocator& allocator, ThemePalette palette)
        {
            StyleSheetLoader loader(allocator);
            loader.SetPalette(palette);
            RefPtr<StyleSheet> sheet = loader.Load(EmbeddedThemes::Dark());
            if (!sheet)
            {
                // A parse failure of the EMBEDDED sheet is a build defect; never ship an
                // unstyled UI - the legacy builder is the belt while it still exists.
                return CreateLegacyForParity(allocator, palette);
            }
            ThemeRegistry::ApplyExtensions(*sheet, palette);
            return sheet;
        }

        /// The former C++ rule builder - parity-test oracle ONLY (see the struct comment).
        [[nodiscard]] static RefPtr<StyleSheet> CreateLegacyForParity(IAllocator& allocator, ThemePalette palette)
        {
            return BuildTheme(allocator, palette);
        }

    private:
        [[nodiscard]] static Color C(f32 r, f32 g, f32 b, f32 a)
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        [[nodiscard]] static RefPtr<StyleSheet> BuildTheme(IAllocator& allocator, ThemePalette p)
        {
            RefPtr<StyleSheet> sheetRef = MakeRef<StyleSheet>(allocator);
            StyleSheet& sheet = *sheetRef;

            // === Global defaults (View base type) ===
            sheet.ForType(&View::StaticType())
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::FontSize, 16.0f);

            // === Button ===
            RefPtr<Drawable> btnBg = Palette::CreateStateColors(allocator, p.SurfaceBright);
            RefPtr<Drawable> btnChecked = Palette::CreateStateColors(allocator, p.PrimaryAccent);
            sheet.OwnDrawable(btnBg);
            sheet.OwnDrawable(btnChecked);
            sheet.ForType(&ButtonBase::StaticType())
                .Set(StyleProperty::Background, btnBg)
                .Set(StyleProperty::CheckedBackground, btnChecked)
                .Set(StyleProperty::TextColor, C(240, 240, 245, 255))
                .Set(StyleProperty::Padding, Thickness{12, 8})
                .Set(StyleProperty::CornerRadius, 0.0f);

            // === Panel ===
            RefPtr<Drawable> panelBg =
                MakeRef<RoundedRectDrawable>(allocator, p.Surface, 0.0f, p.Border, 1.0f);
            sheet.OwnDrawable(panelBg);
            sheet.ForClass(u8"panel").Set(StyleProperty::Background, panelBg);

            // === Label ===
            sheet.ForClass(u8"label").Set(StyleProperty::TextColor, p.Text);
            sheet.ForClass(u8"label-dim").Set(StyleProperty::TextColor, p.TextDim);

            // === EditText ===
            RefPtr<Drawable> editBg = MakeRef<RoundedRectDrawable>(
                allocator, C(30, 32, 42, 255), 0.0f, p.Border, 1.0f);
            sheet.OwnDrawable(editBg);
            sheet.ForType(&EditText::StaticType())
                .Set(StyleProperty::Background, editBg)
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::PlaceholderColor, p.TextDim)
                .Set(StyleProperty::FontSize, 14.0f)
                .Set(StyleProperty::Padding, Thickness{6, 4})
                .Set(StyleProperty::CursorColor, p.PrimaryAccent)
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 80));

            // === NumericField (shares EditText styling + spin buttons) ===
            RefPtr<Drawable> spinBg = Palette::CreateStateColors(allocator, C(50, 55, 68, 255));
            sheet.OwnDrawable(spinBg);
            sheet.ForType(&NumericField::StaticType())
                .Set(StyleProperty::Background, editBg)
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::PlaceholderColor, p.TextDim)
                .Set(StyleProperty::FontSize, 14.0f)
                .Set(StyleProperty::Padding, Thickness{6, 4})
                .Set(StyleProperty::CursorColor, p.PrimaryAccent)
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 80));
            sheet.ForTypePseudo(&NumericField::StaticType(), u8"spin-up")
                .Set(StyleProperty::Background, spinBg);
            sheet.ForTypePseudo(&NumericField::StaticType(), u8"spin-down")
                .Set(StyleProperty::Background, spinBg);

            // === CheckBox ===
            const Color cbBorder = C(100, 105, 120, 255);
            RefPtr<Drawable> cbUnchecked = MakeRef<RoundedRectDrawable>(
                allocator, C(30, 32, 42, 255), 0.0f, cbBorder, 1.0f);
            RefPtr<Drawable> cbChecked = MakeRef<RoundedRectDrawable>(
                allocator, p.PrimaryAccent, 0.0f, cbBorder, 1.0f);
            sheet.OwnDrawable(cbUnchecked);
            sheet.OwnDrawable(cbChecked);
            sheet.ForTypePseudo(&CheckBox::StaticType(), u8"box")
                .Set(StyleProperty::Background, cbUnchecked)
                .Set(StyleProperty::Width, 18.0f);
            sheet.ForTypePseudoState(&CheckBox::StaticType(), u8"box", ControlState::Checked)
                .Set(StyleProperty::Background, cbChecked);
            sheet.ForType(&CheckBox::StaticType()).Set(StyleProperty::Spacing, 6.0f);

            // === RadioButton ===
            const Color rbBorder = C(100, 105, 120, 255);
            RefPtr<Drawable> rbUnchecked = MakeRef<RoundedRectDrawable>(
                allocator, C(30, 32, 42, 255), 0.0f, rbBorder, 1.0f);
            RefPtr<Drawable> rbChecked = MakeRef<RoundedRectDrawable>(
                allocator, p.PrimaryAccent, 0.0f, rbBorder, 1.0f);
            sheet.OwnDrawable(rbUnchecked);
            sheet.OwnDrawable(rbChecked);
            sheet.ForTypePseudo(&RadioButton::StaticType(), u8"box")
                .Set(StyleProperty::Background, rbUnchecked);
            sheet.ForTypePseudoState(&RadioButton::StaticType(), u8"box", ControlState::Checked)
                .Set(StyleProperty::Background, rbChecked);

            // === Slider ===
            sheet.ForTypePseudo(&Slider::StaticType(), u8"track")
                .Set(StyleProperty::Background, sheet.OwnColor(C(50, 52, 62, 255)))
                .Set(StyleProperty::Height, 4.0f);
            sheet.ForTypePseudo(&Slider::StaticType(), u8"fill")
                .Set(StyleProperty::Background, sheet.OwnColor(p.PrimaryAccent));
            sheet.ForTypePseudo(&Slider::StaticType(), u8"thumb")
                .Set(StyleProperty::Background, sheet.OwnColor(C(220, 220, 230, 255)))
                .Set(StyleProperty::Width, 16.0f);

            // === ProgressBar ===
            sheet.ForTypePseudo(&ProgressBar::StaticType(), u8"track")
                .Set(StyleProperty::Background, sheet.OwnColor(C(50, 52, 62, 255)));
            sheet.ForTypePseudo(&ProgressBar::StaticType(), u8"fill")
                .Set(StyleProperty::Background, sheet.OwnColor(p.PrimaryAccent));

            // === ToggleSwitch ===
            {
                RefPtr<Drawable> swOff = MakeRef<RoundedRectDrawable>(allocator, p.Surface,
                                                                      0.0f, p.Border, 1.0f);
                RefPtr<Drawable> swOn = MakeRef<RoundedRectDrawable>(
                    allocator, p.PrimaryAccent, 0.0f, p.Border, 1.0f);
                sheet.OwnDrawable(swOff);
                sheet.OwnDrawable(swOn);
                sheet.ForTypePseudo(&ToggleSwitch::StaticType(), u8"track")
                    .Set(StyleProperty::Background, swOff);
                sheet
                    .ForTypePseudoState(&ToggleSwitch::StaticType(), u8"track",
                                        ControlState::Checked)
                    .Set(StyleProperty::Background, swOn);
                sheet.ForTypePseudo(&ToggleSwitch::StaticType(), u8"knob")
                    .Set(StyleProperty::Background, sheet.OwnColor(C(230, 230, 235, 255)));
            }

            // === ComboBox ===
            RefPtr<Drawable> comboBg = MakeRef<RoundedRectDrawable>(
                allocator, C(40, 42, 52, 255), 0.0f, p.Border, 1.0f);
            sheet.OwnDrawable(comboBg);
            sheet.ForType(&ComboBox::StaticType()).Set(StyleProperty::Background, comboBg);
            sheet.ForTypePseudo(&ComboBox::StaticType(), u8"arrow")
                .Set(StyleProperty::TextColor, C(180, 185, 200, 255));

            // === ScrollBar ===
            sheet.ForTypePseudo(&ScrollBar::StaticType(), u8"track")
                .Set(StyleProperty::Background, sheet.OwnColor(C(40, 42, 50, 150)));
            sheet.ForTypePseudo(&ScrollBar::StaticType(), u8"thumb")
                .Set(StyleProperty::Background, sheet.OwnColor(C(100, 110, 130, 200)));

            // === Separator ===
            sheet.ForType(&Separator::StaticType()).Set(StyleProperty::BorderColor, p.Border);

            // === Expander ===
            sheet.ForTypePseudo(&Expander::StaticType(), u8"header")
                .Set(StyleProperty::Background, sheet.OwnColor(C(50, 55, 68, 255)));
            sheet.ForTypePseudoState(&Expander::StaticType(), u8"header", ControlState::Hover)
                .Set(StyleProperty::Background,
                     sheet.OwnColor(Palette::Lighten(C(50, 55, 68, 255), 0.1f)));
            sheet.ForTypePseudo(&Expander::StaticType(), u8"chevron")
                .Set(StyleProperty::TextColor, C(180, 185, 200, 255));

            // === TabView ===
            sheet.ForTypePseudo(&TabView::StaticType(), u8"strip")
                .Set(StyleProperty::Background, sheet.OwnColor(Palette::Darken(p.Surface, 0.15f)));
            sheet.ForTypePseudo(&TabView::StaticType(), u8"content")
                .Set(StyleProperty::Background, sheet.OwnColor(p.Surface));
            sheet.ForTypePseudoState(&TabView::StaticType(), u8"tab", ControlState::Checked)
                .Set(StyleProperty::Background, sheet.OwnColor(p.Surface));
            sheet.ForTypePseudoState(&TabView::StaticType(), u8"tab", ControlState::Hover)
                .Set(StyleProperty::Background, sheet.OwnColor(Palette::Lighten(p.Surface, 0.05f)));
            sheet.ForTypePseudo(&TabView::StaticType(), u8"tab")
                .Set(StyleProperty::TextColor, p.TextDim);
            sheet.ForTypePseudoState(&TabView::StaticType(), u8"tab", ControlState::Checked)
                .Set(StyleProperty::TextColor, p.Text);
            sheet.ForTypePseudoState(&TabView::StaticType(), u8"tab", ControlState::Hover)
                .Set(StyleProperty::TextColor, Palette::Lighten(p.TextDim, 0.3f));
            sheet.ForTypePseudo(&TabView::StaticType(), u8"close-button")
                .Set(StyleProperty::TextColor, p.TextDim)
                .Set(StyleProperty::Width, 12.0f);
            sheet.ForTypePseudoState(&TabView::StaticType(), u8"close-button", ControlState::Hover)
                .Set(StyleProperty::TextColor, p.Text);
            sheet.ForType(&TabView::StaticType())
                .Set(StyleProperty::BorderColor, p.Border)
                .Set(StyleProperty::AccentColor, p.PrimaryAccent);

            // === ContextMenu ===
            RefPtr<Drawable> menuBg = MakeRef<RoundedRectDrawable>(
                allocator, C(45, 48, 58, 255), 0.0f, C(70, 75, 90, 255), 1.0f);
            sheet.OwnDrawable(menuBg);
            RefPtr<Drawable> menuHover =
                MakeRef<RoundedRectDrawable>(allocator, C(60, 120, 200, 100), 0.0f);
            sheet.OwnDrawable(menuHover);
            sheet.ForClass(u8"contextmenu")
                .Set(StyleProperty::Background, menuBg)
                .Set(StyleProperty::MenuItemHoverDrawable, menuHover)
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::BorderColor, C(70, 75, 90, 255))
                .Set(StyleProperty::AccentColor, C(60, 120, 200, 100));

            // === Dialog ===
            RefPtr<Drawable> dialogBg = MakeRef<RoundedRectDrawable>(
                allocator, C(50, 52, 62, 255), 0.0f, C(80, 85, 100, 255), 1.0f);
            sheet.OwnDrawable(dialogBg);
            sheet.ForType(&Dialog::StaticType()).Set(StyleProperty::Background, dialogBg);

            // === Tooltip ===
            RefPtr<Drawable> tooltipBg = MakeRef<RoundedRectDrawable>(
                allocator, C(40, 42, 50, 230), 0.0f, C(70, 75, 85, 255), 1.0f);
            sheet.OwnDrawable(tooltipBg);
            sheet.ForType(&TooltipView::StaticType())
                .Set(StyleProperty::Background, tooltipBg)
                .Set(StyleProperty::TextColor, p.Text);

            // === ListView / TreeView / GridView ===
            sheet.ForType(&ListView::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Background))
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 80));
            sheet.ForType(&TreeView::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Background));
            sheet.ForType(&GridView::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Background))
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 80));

            // === Icons ===
            RegisterIcons(allocator, sheet);

            // Apply registered extensions.
            ThemeRegistry::ApplyExtensions(sheet, p);

            return sheetRef;
        }

        static void RegisterIcons(IAllocator& allocator, StyleSheet& sheet)
        {
            if (RefPtr<Drawable> checkmark = ThemeIconSet::Acquire(ThemeIcon::Checkmark))
            {
                sheet.OwnDrawable(checkmark);
                sheet.ForTypePseudo(&CheckBox::StaticType(), u8"checkmark")
                    .Set(StyleProperty::Background, checkmark);
            }
            if (RefPtr<Drawable> radioMark = ThemeIconSet::Acquire(ThemeIcon::RadioMarkSquare))
            {
                sheet.OwnDrawable(radioMark);
                sheet.ForTypePseudo(&RadioButton::StaticType(), u8"mark")
                    .Set(StyleProperty::Background, radioMark);
            }
            if (RefPtr<Drawable> closeIcon = ThemeIconSet::Acquire(ThemeIcon::Close))
            {
                sheet.OwnDrawable(closeIcon);
                sheet.ForTypePseudo(&TabView::StaticType(), u8"close-button")
                    .Set(StyleProperty::Background, closeIcon);
            }
            if (RefPtr<Drawable> chevExpanded = ThemeIconSet::Acquire(ThemeIcon::ChevronDown))
            {
                sheet.OwnDrawable(chevExpanded);
                sheet
                    .ForTypePseudoState(&Expander::StaticType(), u8"chevron", ControlState::Checked)
                    .Set(StyleProperty::Background, chevExpanded);
            }
            if (RefPtr<Drawable> chevCollapsed =
                    ThemeIconSet::Acquire(ThemeIcon::ChevronRight))
            {
                sheet.OwnDrawable(chevCollapsed);
                sheet.ForTypePseudo(&Expander::StaticType(), u8"chevron")
                    .Set(StyleProperty::Background, chevCollapsed);
            }
            if (RefPtr<Drawable> tvChevExpanded =
                    ThemeIconSet::Acquire(ThemeIcon::ChevronDown))
            {
                sheet.OwnDrawable(tvChevExpanded);
                sheet
                    .ForTypePseudoState(&TreeView::StaticType(), u8"chevron", ControlState::Checked)
                    .Set(StyleProperty::Background, tvChevExpanded);
            }
            if (RefPtr<Drawable> tvChevCollapsed =
                    ThemeIconSet::Acquire(ThemeIcon::ChevronRight))
            {
                sheet.OwnDrawable(tvChevCollapsed);
                sheet.ForTypePseudo(&TreeView::StaticType(), u8"chevron")
                    .Set(StyleProperty::Background, tvChevCollapsed);
            }
            if (RefPtr<Drawable> subArrow = ThemeIconSet::Acquire(ThemeIcon::ChevronRight))
            {
                sheet.OwnDrawable(subArrow);
                RefPtr<StyleRule> rule = MakeRef<StyleRule>(allocator);
                rule->Selector.AddClass(u8"contextmenu");
                rule->Selector.SetPseudoElement(u8"submenu-arrow");
                rule->Set(StyleProperty::Background, subArrow);
                sheet.AddRule(Move(rule));
            }
            if (RefPtr<Drawable> arrowDown = ThemeIconSet::Acquire(ThemeIcon::ArrowDown))
            {
                sheet.OwnDrawable(arrowDown);
                sheet.ForTypePseudo(&ComboBox::StaticType(), u8"arrow")
                    .Set(StyleProperty::Background, arrowDown);
            }
            if (RefPtr<Drawable> arrowUp = ThemeIconSet::Acquire(ThemeIcon::ArrowUp))
            {
                sheet.OwnDrawable(arrowUp);
                sheet.ForTypePseudo(&NumericField::StaticType(), u8"arrow-up")
                    .Set(StyleProperty::Background, arrowUp);
            }
            if (RefPtr<Drawable> arrowDn2 = ThemeIconSet::Acquire(ThemeIcon::ArrowDown))
            {
                sheet.OwnDrawable(arrowDn2);
                sheet.ForTypePseudo(&NumericField::StaticType(), u8"arrow-down")
                    .Set(StyleProperty::Background, arrowDn2);
            }
        }
    };
}
