// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Checks on the built-in theme sheets. The sheets are written against the shared design system;
// the older C++ builders remain only as a fallback when a sheet fails to parse.
// These checks hold whatever the look: every sheet parses non-empty under its palettes, the
// design-system type ramp holds (16, 14, 12), and the icon vocabulary resolves to real drawables
// (svg() never silently returns null).
#include <doctest/doctest.h>
#include <cstdio>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::core;
namespace core = foundation::core;

namespace
{
    RefPtr<StyleSheet> LoadTheme(StringView sss, ThemePalette palette)
    {
        StyleSheetLoader loader(DefaultAllocator());
        loader.SetPalette(palette);
        return loader.Load(sss);
    }

    /// Merged lookup: last rule that targets exactly (type, no class, no state, pseudo) wins.
    const StyleValue* FindValue(const StyleSheet& sheet, const TypeInfo* type, StringView pseudo,
                                StyleProperty prop)
    {
        const StyleValue* found = nullptr;
        for (usize r = 0; r < sheet.RuleCount(); ++r)
        {
            const StyleRule& rule = sheet.GetRule(r);
            const StyleSelector& sel = rule.Selector;
            if (sel.ViewType != type || sel.StyleClasses.Size() > 0 || sel.State.HasValue())
            {
                continue;
            }
            const StringView rulePseudo =
                sel.PseudoElement.HasValue() ? sel.PseudoElement.Value().AsView() : StringView{};
            if (rulePseudo != pseudo)
            {
                continue;
            }
            for (usize i = 0; i < rule.PropertyCount(); ++i)
            {
                if (rule.GetProperty(i).Prop == prop)
                {
                    found = &rule.GetProperty(i).Value;
                }
            }
        }
        return found;
    }

    void CheckFontSize(const StyleSheet& sheet, const TypeInfo* type, f32 expected)
    {
        const StyleValue* v = FindValue(sheet, type, {}, StyleProperty::FontSize);
        REQUIRE(v != nullptr);
        REQUIRE(v->AsFloat().HasValue());
        CHECK(v->AsFloat().Value() == doctest::Approx(expected));
    }

    usize CountSvgIconRules(const StyleSheet& sheet)
    {
        usize count = 0;
        for (usize r = 0; r < sheet.RuleCount(); ++r)
        {
            const StyleRule& rule = sheet.GetRule(r);
            for (usize i = 0; i < rule.PropertyCount(); ++i)
            {
                if (Drawable* d = rule.GetProperty(i).Value.AsDrawable();
                    d != nullptr && core::Cast<SVGDrawable>(d) != nullptr)
                {
                    ++count;
                }
            }
        }
        return count;
    }

    /// The full design-system contract shared by all built-in sheets.
    void CheckDesignSystem(const StyleSheet& sheet)
    {
        // Type ramp: 16 by default, 14 for inputs, 12 for compact chrome.
        CheckFontSize(sheet, &View::StaticType(), 16.0f);
        CheckFontSize(sheet, &ButtonBase::StaticType(), 12.0f);
        CheckFontSize(sheet, &ComboBox::StaticType(), 12.0f);
        CheckFontSize(sheet, &EditText::StaticType(), 14.0f);
        CheckFontSize(sheet, &NumericField::StaticType(), 14.0f);
        CheckFontSize(sheet, &Expander::StaticType(), 14.0f);

        // Global defaults: themed text, accent and semantic status colors on the View base type
        // (toasts and validation resolve success/warning/error through the theme).
        CHECK(FindValue(sheet, &View::StaticType(), {}, StyleProperty::TextColor) != nullptr);
        CHECK(FindValue(sheet, &View::StaticType(), {}, StyleProperty::AccentColor) != nullptr);
        CHECK(FindValue(sheet, &View::StaticType(), {}, StyleProperty::SuccessColor) != nullptr);
        CHECK(FindValue(sheet, &View::StaticType(), {}, StyleProperty::WarningColor) != nullptr);
        CHECK(FindValue(sheet, &View::StaticType(), {}, StyleProperty::ErrorColor) != nullptr);

        // Spacing scale: button padding 8 12 (CSS vertical then horizontal, so Thickness l12 t8 r12 b8),
        // input padding 4 6.
        {
            const StyleValue* v =
                FindValue(sheet, &ButtonBase::StaticType(), {}, StyleProperty::Padding);
            REQUIRE(v != nullptr);
            REQUIRE(v->AsThickness().HasValue());
            const Thickness t = v->AsThickness().Value();
            CHECK(t.Left == 12.0f);
            CHECK(t.Top == 8.0f);
            CHECK(t.Right == 12.0f);
            CHECK(t.Bottom == 8.0f);
        }
        {
            const StyleValue* v =
                FindValue(sheet, &EditText::StaticType(), {}, StyleProperty::Padding);
            REQUIRE(v != nullptr);
            REQUIRE(v->AsThickness().HasValue());
            const Thickness t = v->AsThickness().Value();
            CHECK(t.Left == 6.0f);
            CHECK(t.Top == 4.0f);
        }
        {
            // Icon chrome has padding 2. Without an explicit rule, IconButton would inherit
            // ButtonBase's button-bar padding (8 12) through subclass matching and overflow tight
            // header bands.
            const StyleValue* v =
                FindValue(sheet, &IconButton::StaticType(), {}, StyleProperty::Padding);
            REQUIRE(v != nullptr);
            REQUIRE(v->AsThickness().HasValue());
            CHECK(v->AsThickness().Value().Left == 2.0f);
            CHECK(v->AsThickness().Value().Top == 2.0f);
        }

        // Icon vocabulary: 11 icon rules (the check and radio marks, close, 4 chevrons, the
        // submenu arrow, the combo arrow, 2 spin arrows), each resolved to a real SVGDrawable.
        CHECK(CountSvgIconRules(sheet) >= 11);

        // Sunken inputs carry a background drawable.
        CHECK(FindValue(sheet, &EditText::StaticType(), {}, StyleProperty::Background) !=
              nullptr);
    }
}

TEST_CASE("theme-sheets: dark.sss parses and holds the design system (Dark + GraphiteOrange)")
{
    RefPtr<StyleSheet> dark = LoadTheme(EmbeddedThemes::Dark(), ThemePalette::Dark());
    REQUIRE(dark.Get() != nullptr);
    REQUIRE(dark->RuleCount() > 0);
    CheckDesignSystem(*dark);

    RefPtr<StyleSheet> warm = LoadTheme(EmbeddedThemes::Dark(), ThemePalette::GraphiteOrange());
    REQUIRE(warm.Get() != nullptr);
    CheckDesignSystem(*warm);
}

TEST_CASE("theme-sheets: light.sss parses and holds the design system (tinted icons live)")
{
    RefPtr<StyleSheet> light = LoadTheme(EmbeddedThemes::Light(), ThemePalette::Light());
    REQUIRE(light.Get() != nullptr);
    REQUIRE(light->RuleCount() > 0);
    CheckDesignSystem(*light);
}

TEST_CASE("theme-sheets: rounded-dark.sss parses and holds the design system (editor palette)")
{
    RefPtr<StyleSheet> sheet =
        LoadTheme(EmbeddedThemes::RoundedDark(), ThemePalette::GraphiteOrange());
    REQUIRE(sheet.Get() != nullptr);
    REQUIRE(sheet->RuleCount() > 0);
    CheckDesignSystem(*sheet);

    // The rounded theme's identity on top of the shared logic: a global corner radius.
    const StyleValue* radius =
        FindValue(*sheet, &View::StaticType(), {}, StyleProperty::CornerRadius);
    REQUIRE(radius != nullptr);
    CHECK(radius->AsFloat().Value() == doctest::Approx(6.0f));
}

TEST_CASE("theme-sheets: Create() serves the parsed sheet (belt does not engage)")
{
    // DarkTheme::Create falls back to the C++ builder only when the embedded sheet fails to
    // parse, so a healthy build serves the parsed sheet. The C++ builder sets no AccentColor on
    // the View rule, so its presence shows the parsed path was taken.
    RefPtr<StyleSheet> sheet = DarkTheme::Create(DefaultAllocator(), ThemePalette::Dark());
    REQUIRE(sheet.Get() != nullptr);
    CHECK(FindValue(*sheet, &View::StaticType(), {}, StyleProperty::AccentColor) != nullptr);
}
