// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :rounded_dark_theme partition
//
// Dark theme variant with consistent rounded corners everywhere - demonstrates that the drawable-based
// styling system supports different visual styles from the same control set. Ported from
// Sedulous.UI/src/Styling/RoundedDarkTheme.bf. Same structural pattern as :dark_theme (Beef `static class`
// -> struct of static factories; `new StyleSheet` -> RefPtr<StyleSheet>; byte-literal Color(r,g,b,a) -> the
// local C() helper; `typeof(T)` -> &T::StaticType()). Unlike DarkTheme this builds explicit rounded
// StateListDrawables for the button backgrounds and uses per-corner radii for the spin buttons.

module;
#include "Core/Prelude.h"

export module foundation.ui:rounded_dark_theme;

import foundation.core;
import foundation.vg; // CornerRadii
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
namespace vg = foundation::vg;

export namespace foundation::ui
{
    /// Factory for creating a dark theme with consistent rounded corners everywhere.
    struct RoundedDarkTheme
    {
        [[nodiscard]] static RefPtr<StyleSheet> Create(IAllocator& allocator)
        {
            return Create(allocator, ThemePalette::Dark());
        }
        [[nodiscard]] static RefPtr<StyleSheet> Create(IAllocator& allocator, ThemePalette palette)
        {
            // Authored as Styling/Themes/rounded-dark.sss (embedded at build).
            StyleSheetLoader loader(allocator);
            loader.SetPalette(palette);
            RefPtr<StyleSheet> sheet = loader.Load(EmbeddedThemes::RoundedDark());
            if (!sheet)
            {
                return CreateLegacyForParity(allocator, palette); // never ship unstyled (build defect)
            }
            ThemeRegistry::ApplyExtensions(*sheet, palette);
            return sheet;
        }

        /// The former C++ rule builder - parity-test oracle ONLY.
        [[nodiscard]] static RefPtr<StyleSheet> CreateLegacyForParity(IAllocator& allocator, ThemePalette palette)
        {
            return BuildTheme(allocator, palette);
        }

    private:
        [[nodiscard]] static RefPtr<StyleSheet> BuildTheme(IAllocator& allocator, ThemePalette p)
        {
            RefPtr<StyleSheet> sheetRef = MakeRef<StyleSheet>(allocator);
            StyleSheet& sheet = *sheetRef;
            const f32 R = 6.0f; // consistent corner radius

            // Palette-derived control colors: every surface/border/accent is computed from the palette
            // so the whole theme follows it (no hardcoded cool-grey/blue literals) - a warm palette like
            // GraphiteOrange then applies end-to-end, selection highlights included.
            const auto A = [](Color c, f32 a255) { return Color{c.r, c.g, c.b, a255 / 255.0f}; };
            const Color inputBg = Palette::Darken(p.Surface, 0.25f); // sunken text-field background
            const Color trackBg = Palette::Lighten(p.Surface, 0.12f);   // slider / progress tracks
            const Color ctrlBorder = Palette::Lighten(p.Border, 0.35f); // checkbox / radio outlines
            const Color menuBorder = Palette::Lighten(p.Border, 0.20f);
            const Color dialogBorder = Palette::Lighten(p.Border, 0.30f);
            const Color iconDim = Palette::Lighten(p.TextDim, 0.15f); // arrows / chevrons
            const Color knob = p.Text;                                // slider thumb / switch knob
            const Color selection = A(p.PrimaryAccent, 90.0f);        // text / list selection
            const Color menuHi = A(p.PrimaryAccent, 100.0f);          // menu-item hover / accent

            // === Global defaults ===
            // AccentColor as a global default: controls that ResolveStyleColor(AccentColor, <fallback>)
            // without a type-specific rule (e.g. tree drop-indicators, node-graph links) then pick up the
            // palette accent instead of their hardcoded fallback. Type-specific accent rules still win.
            // Global CornerRadius = the theme's uniform R, so every control that resolves it (focus
            // borders, self-drawing toolkit controls like DockTabGroup/ToastCard) rounds consistently.
            // The flat DarkTheme leaves it at 0, so those same controls stay square there.
            sheet.ForType(&View::StaticType())
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::AccentColor, p.PrimaryAccent)
                .Set(StyleProperty::CornerRadius, R)
                .Set(StyleProperty::FontSize, 16.0f);

            // === Button - rounded state drawables ===
            RefPtr<StateListDrawable> btnBg = MakeRef<StateListDrawable>(allocator);
            btnBg->Set(ControlState::Normal,
                       MakeRef<RoundedRectDrawable>(allocator, p.SurfaceBright, R));
            btnBg->Set(ControlState::Hover,
                       MakeRef<RoundedRectDrawable>(allocator,
                                                    Palette::ComputeHover(p.SurfaceBright), R));
            btnBg->Set(ControlState::Pressed,
                       MakeRef<RoundedRectDrawable>(allocator,
                                                    Palette::ComputePressed(p.SurfaceBright), R));
            btnBg->Set(ControlState::Disabled,
                       MakeRef<RoundedRectDrawable>(allocator,
                                                    Palette::ComputeDisabled(p.SurfaceBright), R));
            btnBg->Set(ControlState::Focused,
                       MakeRef<RoundedRectDrawable>(allocator,
                                                    Palette::ComputeFocused(p.SurfaceBright), R));
            sheet.OwnDrawable(btnBg);

            RefPtr<StateListDrawable> btnChecked = MakeRef<StateListDrawable>(allocator);
            btnChecked->Set(ControlState::Normal,
                            MakeRef<RoundedRectDrawable>(allocator, p.PrimaryAccent, R));
            btnChecked->Set(ControlState::Hover,
                            MakeRef<RoundedRectDrawable>(
                                allocator, Palette::ComputeHover(p.PrimaryAccent), R));
            btnChecked->Set(ControlState::Pressed,
                            MakeRef<RoundedRectDrawable>(
                                allocator, Palette::ComputePressed(p.PrimaryAccent), R));
            btnChecked->Set(ControlState::Disabled,
                            MakeRef<RoundedRectDrawable>(
                                allocator, Palette::ComputeDisabled(p.PrimaryAccent), R));
            btnChecked->Set(ControlState::Focused,
                            MakeRef<RoundedRectDrawable>(
                                allocator, Palette::ComputeFocused(p.PrimaryAccent), R));
            sheet.OwnDrawable(btnChecked);

            sheet.ForType(&ButtonBase::StaticType())
                .Set(StyleProperty::Background, btnBg)
                .Set(StyleProperty::CheckedBackground, btnChecked)
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::FontSize, 12.0f)
                .Set(StyleProperty::Padding, Thickness{12, 8});

            // === Panel ===
            RefPtr<Drawable> panelBg =
                MakeRef<RoundedRectDrawable>(allocator, p.Surface, R, p.Border, 1.0f);
            sheet.OwnDrawable(panelBg);
            sheet.ForClass(u8"panel").Set(StyleProperty::Background, panelBg);

            // === Label ===
            sheet.ForClass(u8"label").Set(StyleProperty::TextColor, p.Text);
            sheet.ForClass(u8"label-dim").Set(StyleProperty::TextColor, p.TextDim);

            // === EditText ===
            RefPtr<Drawable> editBg =
                MakeRef<RoundedRectDrawable>(allocator, inputBg, R, p.Border, 1.0f);
            sheet.OwnDrawable(editBg);
            sheet.ForType(&EditText::StaticType())
                .Set(StyleProperty::Background, editBg)
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::PlaceholderColor, p.TextDim)
                .Set(StyleProperty::FontSize, 14.0f)
                .Set(StyleProperty::Padding, Thickness{6, 4})
                .Set(StyleProperty::CursorColor, p.PrimaryAccent)
                .Set(StyleProperty::SelectionColor, selection);

            // === NumericField (shares EditText styling + rounded spin buttons) ===
            {
                const Color spinColor = p.SurfaceBright;
                RefPtr<Drawable> spinUp =
                    Palette::CreateStateRounded(allocator, spinColor, vg::CornerRadii{0, R, 0, 0});
                RefPtr<Drawable> spinDown =
                    Palette::CreateStateRounded(allocator, spinColor, vg::CornerRadii{0, 0, R, 0});
                sheet.OwnDrawable(spinUp);
                sheet.OwnDrawable(spinDown);
                sheet.ForType(&NumericField::StaticType())
                    .Set(StyleProperty::Background, editBg)
                    .Set(StyleProperty::TextColor, p.Text)
                    .Set(StyleProperty::PlaceholderColor, p.TextDim)
                    .Set(StyleProperty::FontSize, 14.0f)
                    .Set(StyleProperty::Padding, Thickness{6, 4})
                    .Set(StyleProperty::CursorColor, p.PrimaryAccent)
                    .Set(StyleProperty::SelectionColor, selection);
                sheet.ForTypePseudo(&NumericField::StaticType(), u8"spin-up")
                    .Set(StyleProperty::Background, spinUp);
                sheet.ForTypePseudo(&NumericField::StaticType(), u8"spin-down")
                    .Set(StyleProperty::Background, spinDown);
            }

            // === CheckBox - rounded ===
            const Color cbBorder = ctrlBorder;
            RefPtr<Drawable> cbUnchecked =
                MakeRef<RoundedRectDrawable>(allocator, inputBg, 3.0f, cbBorder, 1.0f);
            RefPtr<Drawable> cbChecked = MakeRef<RoundedRectDrawable>(
                allocator, p.PrimaryAccent, 3.0f, cbBorder, 1.0f);
            sheet.OwnDrawable(cbUnchecked);
            sheet.OwnDrawable(cbChecked);
            sheet.ForTypePseudo(&CheckBox::StaticType(), u8"box")
                .Set(StyleProperty::Background, cbUnchecked)
                .Set(StyleProperty::Width, 18.0f);
            sheet.ForTypePseudoState(&CheckBox::StaticType(), u8"box", ControlState::Checked)
                .Set(StyleProperty::Background, cbChecked);
            sheet.ForType(&CheckBox::StaticType()).Set(StyleProperty::Spacing, 6.0f);

            // === RadioButton - circular ===
            const Color rbBorder = ctrlBorder;
            RefPtr<Drawable> rbUnchecked =
                MakeRef<RoundedRectDrawable>(allocator, inputBg, 9.0f, rbBorder, 1.0f);
            RefPtr<Drawable> rbChecked = MakeRef<RoundedRectDrawable>(
                allocator, p.PrimaryAccent, 9.0f, rbBorder, 1.0f);
            sheet.OwnDrawable(rbUnchecked);
            sheet.OwnDrawable(rbChecked);
            sheet.ForTypePseudo(&RadioButton::StaticType(), u8"box")
                .Set(StyleProperty::Background, rbUnchecked);
            sheet.ForTypePseudoState(&RadioButton::StaticType(), u8"box", ControlState::Checked)
                .Set(StyleProperty::Background, rbChecked);

            // === Slider - rounded track and thumb ===
            RefPtr<Drawable> sliderTrack =
                MakeRef<RoundedRectDrawable>(allocator, trackBg, 2.0f);
            RefPtr<Drawable> sliderFill =
                MakeRef<RoundedRectDrawable>(allocator, p.PrimaryAccent, 2.0f);
            RefPtr<Drawable> sliderThumb =
                MakeRef<RoundedRectDrawable>(allocator, knob, 8.0f);
            sheet.OwnDrawable(sliderTrack);
            sheet.OwnDrawable(sliderFill);
            sheet.OwnDrawable(sliderThumb);
            sheet.ForTypePseudo(&Slider::StaticType(), u8"track")
                .Set(StyleProperty::Background, sliderTrack)
                .Set(StyleProperty::Height, 4.0f);
            sheet.ForTypePseudo(&Slider::StaticType(), u8"fill")
                .Set(StyleProperty::Background, sliderFill);
            sheet.ForTypePseudo(&Slider::StaticType(), u8"thumb")
                .Set(StyleProperty::Background, sliderThumb)
                .Set(StyleProperty::Width, 16.0f);

            // === ProgressBar - rounded ===
            RefPtr<Drawable> progTrack =
                MakeRef<RoundedRectDrawable>(allocator, trackBg, 4.0f);
            RefPtr<Drawable> progFill =
                MakeRef<RoundedRectDrawable>(allocator, p.PrimaryAccent, 4.0f);
            sheet.OwnDrawable(progTrack);
            sheet.OwnDrawable(progFill);
            sheet.ForTypePseudo(&ProgressBar::StaticType(), u8"track")
                .Set(StyleProperty::Background, progTrack);
            sheet.ForTypePseudo(&ProgressBar::StaticType(), u8"fill")
                .Set(StyleProperty::Background, progFill);

            // === ToggleSwitch - pill-shaped track (with border) and round knob ===
            RefPtr<Drawable> switchTrackOff =
                MakeRef<RoundedRectDrawable>(allocator, p.Surface, 12.0f, p.Border, 1.0f);
            RefPtr<Drawable> switchTrackOn = MakeRef<RoundedRectDrawable>(
                allocator, p.PrimaryAccent, 12.0f, p.Border, 1.0f);
            RefPtr<Drawable> switchKnob =
                MakeRef<RoundedRectDrawable>(allocator, knob, 10.0f);
            sheet.OwnDrawable(switchTrackOff);
            sheet.OwnDrawable(switchTrackOn);
            sheet.OwnDrawable(switchKnob);
            sheet.ForTypePseudo(&ToggleSwitch::StaticType(), u8"track")
                .Set(StyleProperty::Background, switchTrackOff);
            sheet.ForTypePseudoState(&ToggleSwitch::StaticType(), u8"track", ControlState::Checked)
                .Set(StyleProperty::Background, switchTrackOn);
            sheet.ForTypePseudo(&ToggleSwitch::StaticType(), u8"knob")
                .Set(StyleProperty::Background, switchKnob);
            sheet.ForType(&ToggleSwitch::StaticType()).Set(StyleProperty::BorderColor, p.Border);

            // === ComboBox ===
            RefPtr<Drawable> comboBg = MakeRef<RoundedRectDrawable>(
                allocator, p.SurfaceBright, R, p.Border, 1.0f);
            sheet.OwnDrawable(comboBg);
            sheet.ForType(&ComboBox::StaticType())
                .Set(StyleProperty::Background, comboBg)
                .Set(StyleProperty::FontSize,
                     12.0f); // match the surrounding editor content density
            sheet.ForTypePseudo(&ComboBox::StaticType(), u8"arrow")
                .Set(StyleProperty::TextColor, iconDim);

            // === ScrollBar - rounded ===
            RefPtr<Drawable> scrollTrack = MakeRef<RoundedRectDrawable>(
                allocator, A(Palette::Darken(p.Surface, 0.15f), 150.0f), 5.0f);
            RefPtr<Drawable> scrollThumb = MakeRef<RoundedRectDrawable>(
                allocator, A(Palette::Lighten(p.Border, 0.5f), 200.0f), 5.0f);
            sheet.OwnDrawable(scrollTrack);
            sheet.OwnDrawable(scrollThumb);
            sheet.ForTypePseudo(&ScrollBar::StaticType(), u8"track")
                .Set(StyleProperty::Background, scrollTrack);
            sheet.ForTypePseudo(&ScrollBar::StaticType(), u8"thumb")
                .Set(StyleProperty::Background, scrollThumb);

            // === Separator ===
            sheet.ForType(&Separator::StaticType()).Set(StyleProperty::BorderColor, p.Border);

            // === Expander ===
            RefPtr<Drawable> expanderHeader =
                MakeRef<RoundedRectDrawable>(allocator, p.SurfaceBright, R);
            RefPtr<Drawable> expanderHover = MakeRef<RoundedRectDrawable>(
                allocator, Palette::Lighten(p.SurfaceBright, 0.1f), R);
            sheet.OwnDrawable(expanderHeader);
            sheet.OwnDrawable(expanderHover);
            sheet.ForType(&Expander::StaticType())
                .Set(StyleProperty::FontSize,
                     14.0f); // header text (e.g. property-grid category headers)
            sheet.ForTypePseudo(&Expander::StaticType(), u8"header")
                .Set(StyleProperty::Background, expanderHeader);
            sheet.ForTypePseudoState(&Expander::StaticType(), u8"header", ControlState::Hover)
                .Set(StyleProperty::Background, expanderHover);
            sheet.ForTypePseudo(&Expander::StaticType(), u8"chevron")
                .Set(StyleProperty::TextColor, iconDim);

            // === TabView - rounded tab backgrounds ===
            {
                const f32 tabR = 4.0f;
                RefPtr<Drawable> stripBg = MakeRef<RoundedRectDrawable>(
                    allocator, Palette::Darken(p.Surface, 0.15f), tabR);
                RefPtr<Drawable> contentBg =
                    MakeRef<RoundedRectDrawable>(allocator, p.Surface, tabR);
                RefPtr<Drawable> activeTab =
                    MakeRef<RoundedRectDrawable>(allocator, p.Surface, tabR);
                RefPtr<Drawable> hoverTab = MakeRef<RoundedRectDrawable>(
                    allocator, Palette::Lighten(p.Surface, 0.05f), tabR);
                sheet.OwnDrawable(stripBg);
                sheet.OwnDrawable(contentBg);
                sheet.OwnDrawable(activeTab);
                sheet.OwnDrawable(hoverTab);
                sheet.ForTypePseudo(&TabView::StaticType(), u8"strip")
                    .Set(StyleProperty::Background, stripBg);
                sheet.ForTypePseudo(&TabView::StaticType(), u8"content")
                    .Set(StyleProperty::Background, contentBg);
                sheet.ForTypePseudo(&TabView::StaticType(), u8"tab")
                    .Set(StyleProperty::TextColor, p.TextDim);
                sheet.ForTypePseudoState(&TabView::StaticType(), u8"tab", ControlState::Checked)
                    .Set(StyleProperty::Background, activeTab)
                    .Set(StyleProperty::TextColor, p.Text);
                sheet.ForTypePseudoState(&TabView::StaticType(), u8"tab", ControlState::Hover)
                    .Set(StyleProperty::Background, hoverTab)
                    .Set(StyleProperty::TextColor, Palette::Lighten(p.TextDim, 0.3f));
                sheet.ForTypePseudo(&TabView::StaticType(), u8"close-button")
                    .Set(StyleProperty::TextColor, p.TextDim)
                    .Set(StyleProperty::Width, 12.0f);
                sheet
                    .ForTypePseudoState(&TabView::StaticType(), u8"close-button",
                                        ControlState::Hover)
                    .Set(StyleProperty::TextColor, p.Text);
                sheet.ForType(&TabView::StaticType())
                    .Set(StyleProperty::BorderColor, p.Border)
                    .Set(StyleProperty::AccentColor, p.PrimaryAccent);
            }

            // === ContextMenu ===
            RefPtr<Drawable> menuBg = MakeRef<RoundedRectDrawable>(
                allocator, p.SurfaceBright, R, menuBorder, 1.0f);
            sheet.OwnDrawable(menuBg);
            RefPtr<Drawable> menuHover =
                MakeRef<RoundedRectDrawable>(allocator, menuHi, 3.0f);
            sheet.OwnDrawable(menuHover);
            sheet.ForClass(u8"contextmenu")
                .Set(StyleProperty::Background, menuBg)
                .Set(StyleProperty::MenuItemHoverDrawable, menuHover)
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::BorderColor, menuBorder)
                .Set(StyleProperty::AccentColor, menuHi);

            // === Dialog ===
            // Dialog surface sits BELOW the button surface (SurfaceBright) so the dialog's buttons
            // stand out instead of blending into the background as flat text.
            RefPtr<Drawable> dialogBg =
                MakeRef<RoundedRectDrawable>(allocator, p.Surface, R, dialogBorder, 1.0f);
            sheet.OwnDrawable(dialogBg);
            sheet.ForType(&Dialog::StaticType()).Set(StyleProperty::Background, dialogBg);

            // === Tooltip ===
            RefPtr<Drawable> tooltipBg = MakeRef<RoundedRectDrawable>(
                allocator, A(p.SurfaceBright, 235.0f), R, p.Border, 1.0f);
            sheet.OwnDrawable(tooltipBg);
            sheet.ForType(&TooltipView::StaticType())
                .Set(StyleProperty::Background, tooltipBg)
                .Set(StyleProperty::TextColor, p.Text);

            // === ListView / TreeView / GridView ===
            sheet.ForType(&ListView::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Background))
                .Set(StyleProperty::SelectionColor, selection);
            sheet.ForType(&TreeView::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Background));
            sheet.ForType(&GridView::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Background))
                .Set(StyleProperty::SelectionColor, selection);

            // === Icons ===
            RegisterIcons(allocator, sheet);

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
            if (RefPtr<Drawable> radioMark = ThemeIconSet::Acquire(ThemeIcon::RadioMarkRound))
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
