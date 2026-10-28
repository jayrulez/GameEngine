// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :toolkit_theme_extension partition
//
// Registers default theme styles for every foundation.ui.toolkit control (dock manager / panels / tab
// groups / splits / floating windows + the menu/tool/status bars + split view + breadcrumb + color
// picker + property grid). A faithful port of Sedulous.UI.Toolkit/src/ToolkitThemeExtension.bf.
//
// Because it names EVERY toolkit control's StaticType(), this partition imports all of them and MUST be
// the last toolkit partition. Register before building a theme:
//   ThemeRegistry::RegisterExtension(MakeUnique<ToolkitThemeExtension>(...));
//
// Port taxes: Beef `typeof(X)` -> `&X::StaticType()`; byte `Color(r,g,b,a)` -> float core::Color (the
// palette is float, so `p.Background.R < 128` -> `p.Background.r < 0.5f`); `new RoundedRectDrawable(...)`
// -> `MakeRef<RoundedRectDrawable>(sheet.MemoryAllocator(), ...)` handed to sheet.OwnDrawable.

module;
#include "Core/Prelude.h"

export module foundation.ui.toolkit:toolkit_theme_extension;

import foundation.core;
import foundation.vg;
import foundation.ui;

import :embedded_toolkit_themes;
import :docking; // DockManager / DockablePanel / DockTabGroup / DockSplit / DockableWindow
import :menu_bar;
import :toolbar;
import :status_bar;
import :split_view;
import :breadcrumb_bar;
import :color_picker;
import :property_grid;
import :toast_host;
import :curve_canvas;
import :gradient_editor;
import :floating_panel;
import :timeline;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui::toolkit
{
    /// Make the toolkit controls selectable from .sss - the
    /// prerequisite for expressing this extension's rules as a toolkit.sss fragment. Idempotent;
    /// called by ToolkitThemeExtension construction, and callable directly by hosts that parse
    /// toolkit-styling sheets without the extension.
    inline void RegisterToolkitTypes()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }
        registered = true;
        UITypeRegistry::Register(u8"DockManager", &DockManager::StaticType());
        UITypeRegistry::Register(u8"DockablePanel", &DockablePanel::StaticType());
        UITypeRegistry::Register(u8"DockTabGroup", &DockTabGroup::StaticType());
        UITypeRegistry::Register(u8"DockSplit", &DockSplit::StaticType());
        UITypeRegistry::Register(u8"DockableWindow", &DockableWindow::StaticType());
        UITypeRegistry::Register(u8"MenuBar", &MenuBar::StaticType());
        UITypeRegistry::Register(u8"Toolbar", &Toolbar::StaticType());
        UITypeRegistry::Register(u8"StatusBar", &StatusBar::StaticType());
        UITypeRegistry::Register(u8"SplitView", &SplitView::StaticType());
        UITypeRegistry::Register(u8"BreadcrumbBar", &BreadcrumbBar::StaticType());
        UITypeRegistry::Register(u8"ColorPicker", &ColorPicker::StaticType());
        UITypeRegistry::Register(u8"PropertyGrid", &PropertyGrid::StaticType());
        UITypeRegistry::Register(u8"ToastCard", &ToastCard::StaticType());
        UITypeRegistry::Register(u8"CurveCanvas", &CurveCanvas::StaticType());
        UITypeRegistry::Register(u8"GradientEditor", &GradientEditor::StaticType());
        UITypeRegistry::Register(u8"DockDragPreview", &DockDragPreview::StaticType());
        UITypeRegistry::Register(u8"FloatingPanel", &FloatingPanel::StaticType());
        UITypeRegistry::Register(u8"Timeline", &Timeline::StaticType());
        // No hand-kept count here: a .sss selector for an unregistered type parses as
        // UnknownType and matches nothing, silently - the toolkit sheet test asserts that no
        // rule of either fragment is unknown, which is what catches a missing registration.
    }

    /// Registers default theme styles for all foundation.ui.toolkit controls (Pattern-B injected
    /// IThemeExtension - hand it to ThemeRegistry before building a theme).
    class ToolkitThemeExtension : public IThemeExtension
    {
    public:
        ToolkitThemeExtension() { RegisterToolkitTypes(); }

        void Apply(StyleSheet& sheet, ThemePalette p) override
        {
            // The toolkit styling is authored as .sss: two fragments
            // because dark and light chrome derive in DIFFERENT directions per control (a variable
            // swap cannot express "darken here on dark, lighten here on light").
            const bool isDark = p.Background.r < 0.5f;
            StyleSheetLoader loader(sheet.MemoryAllocator());
            loader.SetPalette(p);
            RefPtr<StyleSheet> parsed = loader.Load(isDark ? EmbeddedToolkitThemes::Dark()
                                                           : EmbeddedToolkitThemes::Light());
            if (!parsed || parsed->IsEmpty())
            {
                // A parse failure of the EMBEDDED fragment is a build defect; never ship unstyled
                // toolkit chrome - the legacy builder is the belt while it still exists.
                ApplyLegacyForParity(sheet, p);
                return;
            }
            sheet.MergeFrom(*parsed);
        }

        /// The former C++ rule builder - parity-test oracle ONLY (ToolkitThemeParityTests proves
        /// the embedded fragments declare the same styling rule-for-rule), plus the parse-failure
        /// belt above. Dies after the consistency pass is visually signed off.
        static void ApplyLegacyForParity(StyleSheet& sheet, ThemePalette p)
        {
            const bool isDark = p.Background.r < 0.5f;

            // === DockManager ===
            sheet.ForType(&DockManager::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Background));

            // === DockablePanel ===
            {
                const core::Color headerBg =
                    isDark ? Palette::Darken(p.Surface, 0.1f) : Palette::Darken(p.Surface, 0.05f);
                RefPtr<RoundedRectDrawable> headerDrawable =
                    MakeRef<RoundedRectDrawable>(sheet.MemoryAllocator(), headerBg, 0.0f);
                sheet.OwnDrawable(headerDrawable);
                sheet.ForType(&DockablePanel::StaticType())
                    .Set(StyleProperty::TextColor, p.Text)
                    .Set(StyleProperty::Background, sheet.OwnColor(p.Surface));
                sheet.ForTypePseudo(&DockablePanel::StaticType(), u8"header")
                    .Set(StyleProperty::Background, headerDrawable);
                sheet.ForTypePseudo(&DockablePanel::StaticType(), u8"content")
                    .Set(StyleProperty::Background, sheet.OwnColor(p.Surface));
                sheet.ForTypePseudo(&DockablePanel::StaticType(), u8"close-button")
                    .Set(StyleProperty::TextColor, WithAlpha(p.Text, 150));
                sheet
                    .ForTypePseudoState(&DockablePanel::StaticType(), u8"close-button",
                                        ControlState::Hover)
                    .Set(StyleProperty::TextColor, p.Error);
            }

            // === DockTabGroup ===
            {
                const core::Color tabBg =
                    isDark ? Palette::Darken(p.Surface, 0.15f) : Palette::Darken(p.Surface, 0.08f);
                const core::Color activeTab =
                    isDark ? p.Surface : Palette::Lighten(p.Surface, 0.03f);
                const core::Color hoverTab =
                    isDark ? Palette::Lighten(tabBg, 0.05f) : Palette::Darken(p.Surface, 0.04f);
                const core::Color inactiveText = WithAlpha(p.Text, 153);

                sheet.ForType(&DockTabGroup::StaticType())
                    .Set(StyleProperty::BorderColor, p.Border)
                    .Set(StyleProperty::AccentColor, p.PrimaryAccent)
                    // Compact tab-strip text (a specific size so the 24px strip doesn't inherit the
                    // global 16px default); wins over the View default via CSS last-declared tie-break.
                    .Set(StyleProperty::FontSize, 12.0f);
                sheet.ForTypePseudo(&DockTabGroup::StaticType(), u8"strip")
                    .Set(StyleProperty::Background, sheet.OwnColor(tabBg));
                sheet.ForTypePseudo(&DockTabGroup::StaticType(), u8"content")
                    .Set(StyleProperty::Background, sheet.OwnColor(p.Surface));
                sheet.ForTypePseudo(&DockTabGroup::StaticType(), u8"tab")
                    .Set(StyleProperty::TextColor, inactiveText);
                // RoundedRectDrawables so DockTabGroup can mask their top corners to the theme's
                // resolved CornerRadius (rounded in the rounded theme, square/0 in the flat one).
                RefPtr<RoundedRectDrawable> activeTabD =
                    MakeRef<RoundedRectDrawable>(sheet.MemoryAllocator(), activeTab, 0.0f);
                RefPtr<RoundedRectDrawable> hoverTabD =
                    MakeRef<RoundedRectDrawable>(sheet.MemoryAllocator(), hoverTab, 0.0f);
                sheet.OwnDrawable(activeTabD);
                sheet.OwnDrawable(hoverTabD);
                sheet
                    .ForTypePseudoState(&DockTabGroup::StaticType(), u8"tab", ControlState::Checked)
                    .Set(StyleProperty::Background, activeTabD)
                    .Set(StyleProperty::TextColor, p.Text);
                sheet.ForTypePseudoState(&DockTabGroup::StaticType(), u8"tab", ControlState::Hover)
                    .Set(StyleProperty::Background, hoverTabD)
                    .Set(StyleProperty::TextColor, Palette::Lighten(inactiveText, 0.3f));
                sheet.ForTypePseudo(&DockTabGroup::StaticType(), u8"close-button")
                    .Set(StyleProperty::TextColor, inactiveText);
                sheet
                    .ForTypePseudoState(&DockTabGroup::StaticType(), u8"close-button",
                                        ControlState::Hover)
                    .Set(StyleProperty::TextColor, p.Error);
            }

            // === DockSplit ===
            {
                const core::Color divColor =
                    isDark ? Palette::Lighten(p.Surface, 0.1f) : Palette::Darken(p.Surface, 0.1f);
                const core::Color divHover =
                    isDark ? Palette::Lighten(p.Surface, 0.25f) : Palette::Darken(p.Surface, 0.2f);
                sheet.ForType(&DockSplit::StaticType())
                    .Set(StyleProperty::BorderColor, divColor)
                    .Set(StyleProperty::AccentColor, divHover);
            }

            // === DockableWindow ===
            {
                RefPtr<RoundedRectDrawable> dwBg = MakeRef<RoundedRectDrawable>(
                    sheet.MemoryAllocator(), p.Surface, 0.0f, p.Border, 1.0f);
                sheet.OwnDrawable(dwBg);
                sheet.ForType(&DockableWindow::StaticType()).Set(StyleProperty::Background, dwBg);
            }

            // === MenuBar ===
            {
                const core::Color menuBg = isDark ? Palette::Darken(p.Surface, 0.15f) : p.Surface;
                sheet.ForType(&MenuBar::StaticType())
                    .Set(StyleProperty::Background, sheet.OwnColor(menuBg))
                    .Set(StyleProperty::TextColor, p.Text)
                    .Set(StyleProperty::BorderColor, p.Border)
                    // Menu-item hover fills the whole item rect, so use a MUTED translucent accent (not the
                    // full accent, which would paint solid blocks behind the titles).
                    .Set(StyleProperty::AccentColor, WithAlpha(p.PrimaryAccent, 55));
            }

            // === Toolbar ===
            {
                const core::Color toolbarBg =
                    isDark ? Palette::Darken(p.Surface, 0.15f) : Palette::Darken(p.Surface, 0.05f);
                const core::Color toggleOn = isDark ? Palette::Darken(p.PrimaryAccent, 0.3f)
                                                    : Palette::Lighten(p.PrimaryAccent, 0.3f);
                sheet.ForType(&Toolbar::StaticType())
                    .Set(StyleProperty::Background, sheet.OwnColor(toolbarBg))
                    .Set(StyleProperty::BorderColor, p.Border)
                    .Set(StyleProperty::SelectionColor, toggleOn);
            }

            // === StatusBar ===
            {
                const core::Color statusBg =
                    isDark ? Palette::Darken(p.Surface, 0.2f) : Palette::Darken(p.Surface, 0.05f);
                sheet.ForType(&StatusBar::StaticType())
                    .Set(StyleProperty::Background, sheet.OwnColor(statusBg))
                    .Set(StyleProperty::BorderColor, p.Border)
                    .Set(StyleProperty::TextColor, isDark ? WithAlpha(p.Text, 200) : p.Text);
            }

            // === SplitView ===
            {
                const core::Color divColor =
                    isDark ? Palette::Lighten(p.Surface, 0.1f) : Palette::Darken(p.Surface, 0.1f);
                const core::Color divHover =
                    isDark ? Palette::Lighten(p.Surface, 0.25f) : Palette::Darken(p.Surface, 0.2f);
                sheet.ForType(&SplitView::StaticType())
                    .Set(StyleProperty::BorderColor, divColor)
                    .Set(StyleProperty::AccentColor, divHover)
                    .Set(StyleProperty::TextDimColor, WithAlpha(p.TextDim, 180));
            }

            // === BreadcrumbBar === (RoundedRectDrawable so it can round to the theme CornerRadius)
            RefPtr<RoundedRectDrawable> breadcrumbBg = MakeRef<RoundedRectDrawable>(
                sheet.MemoryAllocator(), isDark ? Palette::Darken(p.Surface, 0.1f) : p.Surface, 0.0f);
            sheet.OwnDrawable(breadcrumbBg);
            sheet.ForType(&BreadcrumbBar::StaticType())
                .Set(StyleProperty::Background, breadcrumbBg)
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::AccentColor, p.PrimaryAccent);

            // === ColorPicker ===
            sheet.ForType(&ColorPicker::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Surface))
                .Set(StyleProperty::BorderColor, p.Border);

            // === PropertyGrid ===
            sheet.ForType(&PropertyGrid::StaticType())
                .Set(StyleProperty::Background, sheet.OwnColor(p.Surface))
                .Set(StyleProperty::BorderColor, p.Border);

            // === ToastCard === (floating notification cards). ToastCard resolves Background as a COLOR
            // (ResolveStyleColor), so this must be a raw color - a themed *drawable* (OwnColor) is ignored
            // and the card falls back to its hardcoded value. Fully opaque so it reads over any content.
            sheet.ForType(&ToastCard::StaticType())
                .Set(StyleProperty::Background, p.SurfaceBright)
                .Set(StyleProperty::TextColor, p.Text);
        }

    private:
        [[nodiscard]] static core::Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return core::Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        /// Beef `Color(c.R, c.G, c.B, byteAlpha)` - keep RGB, override alpha with a 0..255 byte.
        [[nodiscard]] static core::Color WithAlpha(core::Color c, u8 a) noexcept
        {
            return core::Color{c.r, c.g, c.b, a / 255.0f};
        }
    };
}
