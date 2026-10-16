// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :markup partition
//
// MarkupLoader: inflate a widget tree from XML markup. Modeled on eepp's layout loading (role
// only). An element name is a tag the loader's ElementSet resolves to a registered element type
// (ElementTypes: the lab's widgets and any library's), made from the loader's allocator; nested
// elements become children;
// attributes are applied in three tiers:
//   - `id` / `class` set the widget's CSS identity,
//   - an attribute a widget claims (via UINode::SetMarkupAttribute - e.g. Label "text",
//     LinearLayout "orientation") is consumed structurally,
//   - everything else is treated as an inline CSS property and run through the StyleApplier,
// so the whole CSS property vocabulary (width/height/padding/margin/background-color/border/
// text-align/units/...) works in markup for free.
//
// Example:
//   <LinearLayout orientation="vertical" spacing="8" padding="12" class="panel">
//     <Label text="Hello" text-align="center"/>
//     <Button text="OK" width="120" class="accent"/>
//   </LinearLayout>

module;
#include "Core/Prelude.h"

export module experimental.gui:markup;

import foundation.core; // RefPtr, MakeRef, Array, String, StringView, Function, Move, Cast, IsWhiteSpace
import foundation.fonts; // IFontService
import foundation.xml;   // XmlDocument, XmlElement, XmlAttribute, XmlResult
import :node;
import :ui_node;
import :ui_widget;
import :style_sheet;   // ResolvedStyle
import :style_applier; // ApplyStyle
import :css_values;    // LengthContext
import :resource_provider;
import :element_types; // ElementSet

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
namespace xml = foundation::xml;

export namespace experimental::gui
{
    class MarkupLoader
    {
    public:
        MarkupLoader(const ElementSet& elements, IAllocator& allocator, IResourceProvider* resources = nullptr,
                     fonts::IFontService* fontService = nullptr, LengthContext lengths = {}) noexcept
            : m_elements(&elements), m_allocator(&allocator), m_resources(resources), m_fontService(fontService),
              m_lengths(lengths)
        {
        }

        // Inflate the root element of `markup` into a widget tree; null on a parse error or an
        // unknown root element.
        [[nodiscard]] RefPtr<Node> LoadFromString(core::StringView markup)
        {
            xml::XmlDocument document(*m_allocator);
            if (document.Parse(markup) != xml::XmlResult::Ok)
                return {};
            xml::XmlElement* root = FirstElementChild(document);
            return root != nullptr ? Inflate(*root) : RefPtr<Node>{};
        }

    private:
        [[nodiscard]] RefPtr<Node> Inflate(xml::XmlElement& element)
        {
            RefPtr<Node> widget = m_elements->Create(element.TagName(), *m_allocator);
            if (!widget)
                return {}; // unknown element name

            UINode* uinode = core::Cast<UINode>(widget.Get());
            UIWidget* uiwidget = core::Cast<UIWidget>(widget.Get());

            ResolvedStyle inlineStyle;
            for (xml::XmlAttribute* attr : element.Attributes())
            {
                const core::StringView name = attr->Name();
                const core::StringView value = attr->Value();
                if (name == core::StringView(u8"id"))
                {
                    if (uiwidget != nullptr)
                        uiwidget->SetId(value);
                    continue;
                }
                if (name == core::StringView(u8"class"))
                {
                    AddClasses(uiwidget, value);
                    continue;
                }
                if (uinode != nullptr && uinode->SetMarkupAttribute(name, value))
                    continue;
                inlineStyle.Set(name, value); // an inline CSS property
            }
            if (uinode != nullptr)
                ApplyStyle(*uinode, inlineStyle, m_resources, m_fontService, m_lengths);

            for (xml::XmlElement* child = element.FirstChildElement(); child != nullptr;
                 child = child->NextSiblingElement())
                if (RefPtr<Node> childWidget = Inflate(*child))
                    widget->AddChild(childWidget.Get());

            return widget;
        }

        static void AddClasses(UIWidget* widget, core::StringView classes)
        {
            if (widget == nullptr)
                return;
            usize start = 0;
            for (usize i = 0; i <= classes.Size(); ++i)
            {
                const bool boundary = (i == classes.Size()) || core::IsWhiteSpace(classes[i]);
                if (boundary)
                {
                    if (i > start)
                        widget->AddClass(classes.SubStr(start, i - start));
                    start = i + 1;
                }
            }
        }

        [[nodiscard]] static xml::XmlElement* FirstElementChild(xml::XmlNode& node)
        {
            for (xml::XmlNode* c = node.FirstChild(); c != nullptr; c = c->NextSibling())
                if (c->NodeType() == xml::XmlNodeType::Element)
                    return static_cast<xml::XmlElement*>(c);
            return nullptr;
        }

        const ElementSet* m_elements;
        IAllocator* m_allocator;
        IResourceProvider* m_resources;
        fonts::IFontService* m_fontService;
        LengthContext m_lengths;
    };
}
