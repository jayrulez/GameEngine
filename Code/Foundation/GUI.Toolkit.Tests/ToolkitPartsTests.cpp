// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - the toolkit's sheet reaches the parts its widgets draw: a sheet styles a widget's
// `tag::part` only for the parts its CollectStyleParts names, and the ruler, the dopesheet, the
// curve view, the graph view, the gradient strip and a property's preview named none, so each
// drew its colours from code whatever the theme said.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>
import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    T* FindIn(Node& node)
    {
        if (T* found = core::Cast<T>(&node))
            return found;
        for (core::usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = FindIn<T>(*node.GetChildAt(i)))
                return found;
        return nullptr;
    }

    bool Styled(const UINode& widget, std::initializer_list<const char8_t*> parts)
    {
        bool all = true;
        for (const char8_t* part : parts)
        {
            const bool has = widget.GetPartStyle(core::StringView(part)).HasBackground;
            if (!has)
                MESSAGE("not styled: ", reinterpret_cast<const char*>(part));
            all = all && has;
        }
        return all;
    }
}

TEST_CASE("toolkit parts: the sheet reaches every part the timeline, curve, graph and gradient views draw")
{
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{800.0f, 600.0f});
    auto ruler = core::MakeRef<TimeRuler>(core::DefaultAllocator());
    auto sheet = core::MakeRef<Dopesheet>(core::DefaultAllocator());
    auto curves = core::MakeRef<CurveView>(core::DefaultAllocator());
    auto graph = core::MakeRef<GraphView>(core::DefaultAllocator());
    auto gradient = core::MakeRef<GradientView>(core::DefaultAllocator());
    auto preview = core::MakeRef<PropertyValuePreview>(core::DefaultAllocator());
    for (Node* node : {static_cast<Node*>(ruler.Get()), static_cast<Node*>(sheet.Get()), static_cast<Node*>(curves.Get()),
                       static_cast<Node*>(graph.Get()), static_cast<Node*>(gradient.Get()), static_cast<Node*>(preview.Get())})
        root->AddChild(node);
    const core::String theme = core::Format(u8"{}\n{}", DefaultThemeCSS(), ToolkitSheetCSS()); // its tokens under it, as an application puts them
    StyleManager styles;
    styles.SetUserAgentSheet(CSSParser::Parse(theme.AsView()));
    styles.ApplyTree(*root.Get());

    CHECK(Styled(*ruler, {u8"tick", u8"marker", u8"marker-chosen", u8"playhead"}));
    CHECK(Styled(*sheet, {u8"labels", u8"separator", u8"key", u8"summary", u8"selected", u8"playhead", u8"selection-box"}));
    CHECK(Styled(*curves, {u8"grid", u8"selected", u8"playhead", u8"selection-box"}));
    CHECK(Styled(*graph, {u8"grid", u8"grid-major", u8"node-link", u8"link-selected", u8"link-highlight", u8"link-drag",
                          u8"link-refused", u8"selection-box"}));
    GradientStrip* strip = FindIn<GradientStrip>(*gradient);
    REQUIRE(strip != nullptr);
    CHECK(Styled(*strip, {u8"selected"}));
    CHECK(Styled(*preview, {u8"curve"}));
}
