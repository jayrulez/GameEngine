// Draconic UI - :textured_theme partition
//
// Creates a fully image-skinned StyleSheet from a ThemeImageSet. All provided images are packed into a
// single atlas for optimal GPU batching (zero texture switches during UI rendering). Ported from
// Sedulous.UI/src/Styling/TexturedTheme.bf. Starts from a palette's colors (text/padding/sizes only) as a
// base, then overlays atlas-backed drawables. Language divergences: Beef `new ThemeAtlas()` + manual
// delete / OwnResource(atlas) -> RefPtr<ThemeAtlas> held by the sheet via OwnResource(RefPtr<Object>) (so
// ThemeAtlas derives Object); the key-parsing IndexOf uses local helpers (StringView has none); Beef
// `Type` -> const TypeInfo*; `StyleProperty?`/`ControlState?` -> Optional<...>.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:textured_theme;

import draconic.foundation;
import draconic.image; // ImageData, NineSlice
import :style_sheet;
import :style_rule;
import :style_property;
import :control_state;
import :thickness;
import :palette;
import :theme_palette;
import :theme_registry;
import :theme_icons;
import :theme_image_set;
import :theme_atlas;
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
namespace image = draconic::image;

export namespace draconic::ui
{
    /// Factory for creating a fully image-skinned StyleSheet from a ThemeImageSet.
    struct TexturedTheme
    {
        /// Create a textured theme with dark base colors.
        [[nodiscard]] static RefPtr<StyleSheet> Create(const ThemeImageSet& images)
        {
            return Create(images, ThemePalette::Dark());
        }

        /// Create a textured theme with a specific palette for base (non-drawable) colors.
        [[nodiscard]] static RefPtr<StyleSheet> Create(const ThemeImageSet& images, ThemePalette p)
        {
            RefPtr<StyleSheet> sheetRef = MakeRef<StyleSheet>(DefaultAllocator());
            StyleSheet& sheet = *sheetRef;

            // Global text defaults.
            sheet.ForType(&View::StaticType())
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::FontSize, 16.0f);

            // Per-control non-drawable properties (colors, padding, sizes).
            sheet.ForType(&ButtonBase::StaticType())
                .Set(StyleProperty::TextColor, C(30, 30, 40, 255))
                .Set(StyleProperty::Padding, Thickness{12, 8});

            sheet.ForClass(u8"label").Set(StyleProperty::TextColor, p.Text);
            sheet.ForClass(u8"label-dim").Set(StyleProperty::TextColor, p.TextDim);

            sheet.ForType(&EditText::StaticType())
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::PlaceholderColor, p.TextDim)
                .Set(StyleProperty::FontSize, 14.0f)
                .Set(StyleProperty::Padding, Thickness{6, 4})
                .Set(StyleProperty::CursorColor, p.PrimaryAccent)
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 80))
                .Set(StyleProperty::CornerRadius, 4.0f);

            sheet.ForType(&NumericField::StaticType())
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::PlaceholderColor, p.TextDim)
                .Set(StyleProperty::FontSize, 14.0f)
                .Set(StyleProperty::Padding, Thickness{6, 4})
                .Set(StyleProperty::CursorColor, p.PrimaryAccent)
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 80))
                .Set(StyleProperty::CornerRadius, 4.0f);

            sheet.ForTypePseudo(&CheckBox::StaticType(), u8"box").Set(StyleProperty::Width, 18.0f);
            sheet.ForType(&CheckBox::StaticType()).Set(StyleProperty::Spacing, 6.0f);

            sheet.ForTypePseudo(&Slider::StaticType(), u8"track").Set(StyleProperty::Height, 4.0f);
            sheet.ForTypePseudo(&Slider::StaticType(), u8"thumb").Set(StyleProperty::Width, 16.0f);

            sheet.ForType(&Separator::StaticType()).Set(StyleProperty::BorderColor, p.Border);

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

            sheet.ForClass(u8"contextmenu")
                .Set(StyleProperty::TextColor, p.Text)
                .Set(StyleProperty::BorderColor, p.Border)
                .Set(StyleProperty::AccentColor, C(60, 120, 200, 80));

            sheet.ForTypePseudo(&Expander::StaticType(), u8"chevron")
                .Set(StyleProperty::TextColor, C(80, 85, 100, 255));

            sheet.ForType(&ComboBox::StaticType()).Set(StyleProperty::CornerRadius, 4.0f);
            sheet.ForTypePseudo(&ComboBox::StaticType(), u8"arrow")
                .Set(StyleProperty::TextColor, C(80, 85, 100, 255));

            sheet.ForType(&TooltipView::StaticType()).Set(StyleProperty::TextColor, p.Text);

            sheet.ForType(&ListView::StaticType())
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 80));
            sheet.ForType(&GridView::StaticType())
                .Set(StyleProperty::SelectionColor, C(60, 120, 200, 80));

            // Register icons with appropriate tint for the palette.
            RegisterIcons(sheet, p);

            ThemeRegistry::ApplyExtensions(sheet, p);

            RefPtr<ThemeAtlas> atlas = MakeRef<ThemeAtlas>(DefaultAllocator());

            // Add all images to atlas.
            for (const auto& kv : images.GetImages())
                atlas->AddImage(kv.key.AsView(), kv.value.Image);

            if (!atlas->Build())
                return sheetRef;

            // Create drawables for state groups (StateListDrawable).
            for (const auto& kv : images.GetStateGroups())
            {
                StringView drawableKey = kv.key.AsView();
                const Array<ThemeStateEntry>& states = kv.value;
                RefPtr<StateListDrawable> stateList =
                    MakeRef<StateListDrawable>(DefaultAllocator());

                for (const ThemeStateEntry& e : states)
                {
                    Optional<ThemeImageEntry> entry = images.GetEntry(e.Key.AsView());
                    if (!entry.HasValue())
                    {
                        continue;
                    }

                    RefPtr<Drawable> drawable;
                    if (entry.Value().IsNineSlice)
                        drawable =
                            atlas->CreateNineSliceDrawable(e.Key.AsView(), entry.Value().Slices);
                    else
                        drawable = atlas->CreateImageDrawable(e.Key.AsView());

                    if (drawable)
                        stateList->Set(e.State, drawable);
                }

                sheet.OwnDrawable(stateList);
                SetDrawableByKey(sheet, drawableKey, stateList);
            }

            // Create drawables for non-grouped images.
            for (const auto& kv : images.GetImages())
            {
                StringView key = kv.key.AsView();

                // Skip internal state images.
                bool isStateImage = false;
                for (const auto& sg : images.GetStateGroups())
                {
                    for (const ThemeStateEntry& e : sg.value)
                    {
                        if (key == e.Key.AsView())
                        {
                            isStateImage = true;
                            break;
                        }
                    }
                    if (isStateImage)
                    {
                        break;
                    }
                }
                if (isStateImage)
                {
                    continue;
                }

                const ThemeImageEntry& entry = kv.value;
                RefPtr<Drawable> drawable;
                if (entry.IsNineSlice)
                    drawable = atlas->CreateNineSliceDrawable(key, entry.Slices);
                else
                    drawable = atlas->CreateImageDrawable(key);

                if (drawable)
                {
                    sheet.OwnDrawable(drawable);
                    SetDrawableByKey(sheet, key, drawable);
                }
            }

            // Theme owns the atlas so it lives as long as the drawables that reference it.
            sheet.OwnResource(atlas);

            return sheetRef;
        }

    private:
        [[nodiscard]] static Color C(f32 r, f32 g, f32 b, f32 a)
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        /// Find the first index of a substring (-1 if not present). Local helper (StringView has no IndexOf).
        [[nodiscard]] static i64 IndexOf(StringView s, StringView needle)
        {
            if (needle.Size() == 0 || needle.Size() > s.Size())
            {
                return needle.Size() == 0 ? 0 : -1;
            }
            const usize last = s.Size() - needle.Size();
            for (usize i = 0; i <= last; ++i)
            {
                bool match = true;
                for (usize j = 0; j < needle.Size(); ++j)
                {
                    if (s[i + j] != needle[j])
                    {
                        match = false;
                        break;
                    }
                }
                if (match)
                {
                    return static_cast<i64>(i);
                }
            }
            return -1;
        }

        /// Find the first index of a character (-1 if not present).
        [[nodiscard]] static i64 IndexOf(StringView s, char8_t c)
        {
            for (usize i = 0; i < s.Size(); ++i)
            {
                if (s[i] == c)
                {
                    return static_cast<i64>(i);
                }
            }
            return -1;
        }

        /// Map a string key to a StyleSheet rule.
        /// Supports:  "typeName::pseudo"  /  "typeName::pseudo:state"  (pseudo-element)
        ///     and    "typeName:propertyName"  (legacy element-level).
        static void SetDrawableByKey(StyleSheet& sheet, StringView key, RefPtr<Drawable> drawable)
        {
            // Check for "::" pseudo-element separator first.
            const i64 pseudoIdx = IndexOf(key, StringView(u8"::"));
            if (pseudoIdx >= 0)
            {
                StringView typeName = key.SubStr(0, static_cast<usize>(pseudoIdx));
                StringView remainder = key.SubStr(static_cast<usize>(pseudoIdx) + 2,
                                                  key.Size() - static_cast<usize>(pseudoIdx) - 2);
                const TypeInfo* type = ResolveTypeName(typeName);
                if (type == nullptr)
                {
                    return;
                }

                // Check for ":state" suffix on the pseudo name.
                const i64 stateIdx = IndexOf(remainder, static_cast<char8_t>(':'));
                if (stateIdx >= 0)
                {
                    StringView pseudo = remainder.SubStr(0, static_cast<usize>(stateIdx));
                    StringView stateName =
                        remainder.SubStr(static_cast<usize>(stateIdx) + 1,
                                         remainder.Size() - static_cast<usize>(stateIdx) - 1);
                    Optional<ControlState> state = ParseStateName(stateName);
                    if (state.HasValue())
                        sheet.ForTypePseudoState(type, pseudo, state.Value())
                            .Set(StyleProperty::Background, drawable);
                }
                else
                {
                    sheet.ForTypePseudo(type, remainder).Set(StyleProperty::Background, drawable);
                }
                return;
            }

            // Legacy format: "typeName:propertyName".
            const i64 colonIdx = IndexOf(key, static_cast<char8_t>(':'));
            if (colonIdx < 0)
            {
                return;
            }

            StringView typeName = key.SubStr(0, static_cast<usize>(colonIdx));
            StringView propName = key.SubStr(static_cast<usize>(colonIdx) + 1,
                                             key.Size() - static_cast<usize>(colonIdx) - 1);

            Optional<StyleProperty> prop = ParsePropertyName(propName);
            if (!prop.HasValue())
            {
                return;
            }

            const TypeInfo* type = ResolveTypeName(typeName);
            if (type != nullptr)
                sheet.ForType(type).Set(prop.Value(), drawable);
            else if (typeName.IsEmpty())
                sheet.ForType(&View::StaticType()).Set(prop.Value(), drawable);
            else
                sheet.ForClass(typeName).Set(prop.Value(), drawable);
        }

        /// Parse a state name to ControlState.
        [[nodiscard]] static Optional<ControlState> ParseStateName(StringView name)
        {
            if (name == StringView(u8"hover"))
            {
                return ControlState::Hover;
            }
            if (name == StringView(u8"pressed"))
            {
                return ControlState::Pressed;
            }
            if (name == StringView(u8"checked"))
            {
                return ControlState::Checked;
            }
            if (name == StringView(u8"disabled"))
            {
                return ControlState::Disabled;
            }
            if (name == StringView(u8"focused"))
            {
                return ControlState::Focused;
            }
            return {};
        }

        /// Map a property name string to a StyleProperty enum value.
        [[nodiscard]] static Optional<StyleProperty> ParsePropertyName(StringView name)
        {
            if (name == StringView(u8"Background"))
            {
                return StyleProperty::Background;
            }
            if (name == StringView(u8"MenuItemHoverDrawable"))
            {
                return StyleProperty::MenuItemHoverDrawable;
            }
            return {};
        }

        /// Map an old style class name to a concrete type for type-based selectors.
        [[nodiscard]] static const TypeInfo* ResolveTypeName(StringView name)
        {
            if (name == StringView(u8"button"))
            {
                return &ButtonBase::StaticType();
            }
            if (name == StringView(u8"edittext"))
            {
                return &EditText::StaticType();
            }
            if (name == StringView(u8"checkbox"))
            {
                return &CheckBox::StaticType();
            }
            if (name == StringView(u8"radiobutton"))
            {
                return &RadioButton::StaticType();
            }
            if (name == StringView(u8"slider"))
            {
                return &Slider::StaticType();
            }
            if (name == StringView(u8"progressbar"))
            {
                return &ProgressBar::StaticType();
            }
            if (name == StringView(u8"toggleswitch"))
            {
                return &ToggleSwitch::StaticType();
            }
            if (name == StringView(u8"combobox"))
            {
                return &ComboBox::StaticType();
            }
            if (name == StringView(u8"scrollbar"))
            {
                return &ScrollBar::StaticType();
            }
            if (name == StringView(u8"separator"))
            {
                return &Separator::StaticType();
            }
            if (name == StringView(u8"expander"))
            {
                return &Expander::StaticType();
            }
            if (name == StringView(u8"tabview"))
            {
                return &TabView::StaticType();
            }
            // "contextmenu" is class-based (shared by ContextMenu and ComboBoxDropdown).
            if (name == StringView(u8"dialog"))
            {
                return &Dialog::StaticType();
            }
            if (name == StringView(u8"tooltip"))
            {
                return &TooltipView::StaticType();
            }
            if (name == StringView(u8"listview"))
            {
                return &ListView::StaticType();
            }
            if (name == StringView(u8"treeview"))
            {
                return &TreeView::StaticType();
            }
            if (name == StringView(u8"gridview"))
            {
                return &GridView::StaticType();
            }
            if (name == StringView(u8"numericfield"))
            {
                return &NumericField::StaticType();
            }
            return nullptr;
        }

        /// Register SVG icons with an appropriate tint for the palette.
        static void RegisterIcons(StyleSheet& sheet, ThemePalette p)
        {
            // Use dark tint for light palettes, no tint for dark.
            const bool isLight = p.Background.r > 0.5f;
            Optional<Color> tint;
            if (isLight)
            {
                tint = C(60, 60, 70, 255);
            }

            auto MakeSVG = [&](StringView svg) -> RefPtr<SVGDrawable>
            {
                if (tint.HasValue())
                {
                    return SVGDrawable::FromString(svg, tint.Value());
                }
                return SVGDrawable::FromString(svg);
            };

            if (RefPtr<Drawable> checkmark = MakeSVG(ThemeIcons::Checkmark()))
            {
                sheet.OwnDrawable(checkmark);
                sheet.ForTypePseudo(&CheckBox::StaticType(), u8"checkmark")
                    .Set(StyleProperty::Background, checkmark);
            }
            if (RefPtr<Drawable> radioMark = MakeSVG(ThemeIcons::RadioMarkRound()))
            {
                sheet.OwnDrawable(radioMark);
                sheet.ForTypePseudo(&RadioButton::StaticType(), u8"mark")
                    .Set(StyleProperty::Background, radioMark);
            }
            if (RefPtr<Drawable> closeIcon = MakeSVG(ThemeIcons::Close()))
            {
                sheet.OwnDrawable(closeIcon);
                sheet.ForTypePseudo(&TabView::StaticType(), u8"close-button")
                    .Set(StyleProperty::Background, closeIcon);
            }
            if (RefPtr<Drawable> chevExpanded = MakeSVG(ThemeIcons::ChevronDown()))
            {
                sheet.OwnDrawable(chevExpanded);
                sheet
                    .ForTypePseudoState(&Expander::StaticType(), u8"chevron", ControlState::Checked)
                    .Set(StyleProperty::Background, chevExpanded);
            }
            if (RefPtr<Drawable> chevCollapsed = MakeSVG(ThemeIcons::ChevronRight()))
            {
                sheet.OwnDrawable(chevCollapsed);
                sheet.ForTypePseudo(&Expander::StaticType(), u8"chevron")
                    .Set(StyleProperty::Background, chevCollapsed);
            }
            if (RefPtr<Drawable> tvChevExpanded = MakeSVG(ThemeIcons::ChevronDown()))
            {
                sheet.OwnDrawable(tvChevExpanded);
                sheet
                    .ForTypePseudoState(&TreeView::StaticType(), u8"chevron", ControlState::Checked)
                    .Set(StyleProperty::Background, tvChevExpanded);
            }
            if (RefPtr<Drawable> tvChevCollapsed = MakeSVG(ThemeIcons::ChevronRight()))
            {
                sheet.OwnDrawable(tvChevCollapsed);
                sheet.ForTypePseudo(&TreeView::StaticType(), u8"chevron")
                    .Set(StyleProperty::Background, tvChevCollapsed);
            }
            if (RefPtr<Drawable> subArrow = MakeSVG(ThemeIcons::ChevronRight()))
            {
                sheet.OwnDrawable(subArrow);
                RefPtr<StyleRule> rule = MakeRef<StyleRule>(DefaultAllocator());
                rule->Selector.AddClass(u8"contextmenu");
                rule->Selector.SetPseudoElement(u8"submenu-arrow");
                rule->Set(StyleProperty::Background, subArrow);
                sheet.AddRule(Move(rule));
            }
            if (RefPtr<Drawable> arrowDown = MakeSVG(ThemeIcons::ArrowDown()))
            {
                sheet.OwnDrawable(arrowDown);
                sheet.ForTypePseudo(&ComboBox::StaticType(), u8"arrow")
                    .Set(StyleProperty::Background, arrowDown);
            }
            if (RefPtr<Drawable> arrowUp = MakeSVG(ThemeIcons::ArrowUp()))
            {
                sheet.OwnDrawable(arrowUp);
                sheet.ForTypePseudo(&NumericField::StaticType(), u8"arrow-up")
                    .Set(StyleProperty::Background, arrowUp);
            }
            if (RefPtr<Drawable> arrowDn2 = MakeSVG(ThemeIcons::ArrowDown()))
            {
                sheet.OwnDrawable(arrowDn2);
                sheet.ForTypePseudo(&NumericField::StaticType(), u8"arrow-down")
                    .Set(StyleProperty::Background, arrowDn2);
            }
        }
    };
}
