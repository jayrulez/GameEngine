// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Toolkit fragment sheet gates. The fragments are authored against the shared design system;
// ApplyLegacyForParity remains only as a parse-failure belt. These gates check the invariants:
// both fragments parse non-empty, Apply merges the parsed rules (not the belt), the ramp holds
// (dock tabs + status bar 12, menu bar 14), and ToastCard's background stays a raw COLOR (the
// ResolveStyleColor contract behind the background-color property).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;

namespace
{
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

    void CheckFragment(StringView fragment, ThemePalette palette)
    {
        // The fragment itself must parse non-empty - if it didn't, Apply would fall back to
        // the legacy belt and the checks below would test the wrong thing.
        RegisterToolkitTypes();
        {
            StyleSheetLoader loader(DefaultAllocator());
            loader.SetPalette(palette);
            RefPtr<StyleSheet> parsed = loader.Load(fragment);
            REQUIRE(parsed.Get() != nullptr);
            REQUIRE(parsed->RuleCount() > 0);
        }

        ToolkitThemeExtension ext;
        RefPtr<StyleSheet> sheetRef = MakeRef<StyleSheet>(DefaultAllocator());
        StyleSheet& sheet = *sheetRef;
        ext.Apply(sheet, palette); // the REAL path: parse + MergeFrom
        REQUIRE(sheet.RuleCount() > 0);

        // Ramp: compact chrome 12, menu bar 14. The explicit 12s are REGRESSION GATES: these
        // controls resolve FontSize now, and without a rule the View base 16 wins (type rules
        // match subclasses). (StatusBar text is child Labels - a container rule cannot reach
        // them, so the sheet deliberately sets none; audit remainder.)
        CheckFontSize(sheet, &DockTabGroup::StaticType(), 12.0f);
        CheckFontSize(sheet, &DockablePanel::StaticType(), 12.0f);
        CheckFontSize(sheet, &Toolbar::StaticType(), 12.0f);
        CheckFontSize(sheet, &BreadcrumbBar::StaticType(), 12.0f);
        CheckFontSize(sheet, &MenuBar::StaticType(), 14.0f);

        // The canvases + drag preview resolve Background as a raw COLOR (background-color).
        const TypeInfo* rawColorTypes[] = {&CurveCanvas::StaticType(),
                                           &GradientEditor::StaticType(),
                                           &DockDragPreview::StaticType()};
        for (const TypeInfo* t : rawColorTypes)
        {
            const StyleValue* bg = FindValue(sheet, t, {}, StyleProperty::Background);
            REQUIRE(bg != nullptr);
            CHECK(bg->AsColor().HasValue());
        }

        // ToastCard resolves Background via ResolveStyleColor: the value must be a raw COLOR
        // (background-color), never a drawable (which that path ignores).
        const StyleValue* toastBg =
            FindValue(sheet, &ToastCard::StaticType(), {}, StyleProperty::Background);
        REQUIRE(toastBg != nullptr);
        CHECK(toastBg->AsColor().HasValue());
        CHECK(toastBg->AsDrawable() == nullptr);

        // Dim text convention: inactive dock tabs use the palette's dim text.
        CHECK(FindValue(sheet, &DockTabGroup::StaticType(), u8"tab", StyleProperty::TextColor) !=
              nullptr);
    }
}

TEST_CASE("toolkit-sheets: toolkit-dark.sss holds the design system (Dark + GraphiteOrange)")
{
    CheckFragment(EmbeddedToolkitThemes::Dark(), ThemePalette::Dark());
    CheckFragment(EmbeddedToolkitThemes::Dark(), ThemePalette::GraphiteOrange());
}

TEST_CASE("toolkit-sheets: toolkit-light.sss holds the design system")
{
    CheckFragment(EmbeddedToolkitThemes::Light(), ThemePalette::Light());
}

TEST_CASE("toolkit-sheets: every type selector in both fragments resolves to a registered type")
{
    // A selector naming an unregistered type parses as UnknownType and styles nothing,
    // silently. The hand-kept type COUNT that was meant to catch this agreed with the
    // registrations rather than with the sheets, which is how Timeline went unstyled.
    struct Case
    {
        StringView fragment;
        ThemePalette palette;
    };
    const Case cases[] = {{EmbeddedToolkitThemes::Dark(), ThemePalette::Dark()},
                          {EmbeddedToolkitThemes::Light(), ThemePalette::Light()}};
    for (const Case& c : cases)
    {
        ToolkitThemeExtension ext;
        RefPtr<StyleSheet> sheetRef = MakeRef<StyleSheet>(DefaultAllocator());
        ext.Apply(*sheetRef, c.palette);
        REQUIRE(sheetRef->RuleCount() > 0);
        bool timelineStyled = false;
        for (usize r = 0; r < sheetRef->RuleCount(); ++r)
        {
            const StyleSelector& sel = sheetRef->GetRule(r).Selector;
            CHECK_FALSE(sel.UnknownType);
            timelineStyled = timelineStyled || sel.ViewType == &Timeline::StaticType();
        }
        CHECK(timelineStyled);
    }
}
