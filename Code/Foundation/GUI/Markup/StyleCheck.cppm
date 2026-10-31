// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :style_check partition
//
// Finds the declarations in a sheet whose names nothing takes, so a cook can warn about a typo
// (`colour: red`) that the style pass would otherwise drop silently. A name is known when the
// style table has it (as a property or a shorthand), when it is a custom property (`--accent`), or
// when a currently registered element type takes it by its CSS name (`nav-up`, a gamekit screen's
// `covers`). The check therefore asks the element types the tools register rather than a fixed
// list.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:style_check;

import foundation.core;
import :style_properties; // FindStyleProperty, ExpandStyleShorthand
import :style_rule;
import :style_sheet;
import :element_types;    // ElementTypes::ForEach
import :diagnostic;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// Returns the CSS names that the registered element types, and their base types, take as
    /// reflected properties.
    [[nodiscard]] inline Array<core::String> RegisteredCssNames()
    {
        Array<core::String> names;
        const auto add = [&names](core::StringView name)
        {
            for (const core::String& known : names)
                if (known.AsView() == name)
                    return;
            names.PushBack(core::String(name));
        };
        ElementTypes::ForEach([&](core::StringView, const TypeInfo& element)
        {
            for (const TypeInfo* type = &element; type != nullptr; type = type->base)
                for (usize i = 0; i < PropertyCount(*type); ++i)
                    if (const Attribute* css = FindAttribute(PropertyAt(*type, i), u8"css"))
                        if (const core::String* name = css->value.TryGet<core::String>())
                            add(name->AsView());
        });
        return names;
    }

    /// Whether a declaration's `name` is one a style takes, by the rules at the top of this file.
    [[nodiscard]] inline bool IsKnownStyleName(core::StringView name, core::StringView value, const Array<core::String>& cssNames)
    {
        if (name.Size() > 2 && name[0] == u8'-' && name[1] == u8'-')
            return true; // a custom property
        if (FindStyleProperty(name) != nullptr)
            return true;
        Array<StyleLonghand> longhands;
        if (ExpandStyleShorthand(name, value, longhands) && !(longhands.Size() == 1 && longhands[0].Name.AsView() == name))
            return true;
        for (const core::String& known : cssNames)
            if (known.AsView() == name)
                return true;
        return false;
    }

    /// Adds a diagnostic to `out`, with the rule's line, for each declaration in `sheet`'s rules
    /// and keyframe stops whose name nothing takes.
    inline void CheckStyleSheetNames(const StyleSheet& sheet, Array<GuiDiagnostic>& out)
    {
        const Array<core::String> cssNames = RegisteredCssNames();
        for (const StyleRule& rule : sheet.Rules())
            for (const StyleProperty& property : rule.Properties())
                if (!IsKnownStyleName(property.Name.AsView(), property.Value.AsView(), cssNames))
                    out.PushBack(GuiDiagnostic{GuiDiagnostic::Severity::Warning, rule.Line(),
                                               core::Format(u8"{} sets '{}', which no style or element takes", rule.SelectorText(),
                                                            property.Name.AsView())});
        for (const Keyframes& keyframes : sheet.AllKeyframes())
        {
            for (const KeyframeStop& stop : keyframes.Stops)
                for (const StyleProperty& property : stop.Properties)
                    if (property.Name.AsView() != core::StringView(u8"animation-timing-function") &&
                        !IsKnownStyleName(property.Name.AsView(), property.Value.AsView(), cssNames))
                        out.PushBack(GuiDiagnostic{GuiDiagnostic::Severity::Warning, 0,
                                                   core::Format(u8"@keyframes {} sets '{}', which no style takes", keyframes.Name.AsView(),
                                                                property.Name.AsView())});
        }
    }
}
