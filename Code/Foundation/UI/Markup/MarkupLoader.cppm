// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :markup_loader partition
//
// Loads a View tree from an XML (.sml) string using MarkupRegistry for element/property resolution.
// Ported from Sedulous.UI/src/Markup/MarkupLoader.bf, on foundation.xml (Code/Foundation/Xml). Ownership:
// Beef raw `View` returns + AddView -> RefPtr<View> (RAII); the returned root owns the whole subtree.
// XML downcasts use XmlNode::NodeType() + static_cast (foundation.xml nodes are not RTTI_OBJECTs).

module;
#include "Core/Prelude.h"

export module foundation.ui:markup_loader;

import foundation.core;
import foundation.xml;
import :view;
import :layout_style;
import :size_spec;
import :thickness;
import :enums;
import :style_property;
import :markup_registry;
import :sss_parser; // SSSParser::ApplyInlineStyle for style="..."

using namespace foundation::core;
namespace xml = foundation::xml;

export namespace foundation::ui
{
    /// Loads a View tree from an XML (.sml) string. Uses MarkupRegistry for element type resolution and
    /// property binding.
    struct MarkupLoader
    {
        /// Parse an XML string and return the root View (null on failure). The context is used for name
        /// registration (id attributes) via the tree it is attached to. `warnings`, when given, collects
        /// diagnostics for constructs the loader would otherwise drop SILENTLY - unknown child elements
        /// and unrecognized attributes (the cook surfaces these; authoring typos die loud, not quiet).
        [[nodiscard]] static RefPtr<View> LoadFromString(IAllocator& allocator,
                                                         StringView xmlText,
                                                         UIContext* context = nullptr,
                                                         Array<String>* warnings = nullptr)
        {
            xml::XmlDocument doc(allocator);
            if (xml::IsError(doc.Parse(xmlText)))
            {
                return {};
            }

            xml::XmlElement* rootElem = doc.RootElement();
            if (rootElem == nullptr)
            {
                return {};
            }

            return BuildView(allocator, rootElem, context, warnings);
        }

        /// Initialize the markup system. Call once at startup.
        static void Initialize() { MarkupRegistry::RegisterBuiltins(); }

    private:
        /// Build a View from an XML element, recursing into children.
        [[nodiscard]] static RefPtr<View> BuildView(IAllocator& allocator,
                                                    xml::XmlElement* element, UIContext* context,
                                                    Array<String>* warnings = nullptr)
        {
            const StringView tagName = element->TagName();

            // <Include> directive is not implemented (requires IResourceProvider).
            if (tagName == u8"Include")
            {
                return {};
            }

            RefPtr<View> view = MarkupRegistry::CreateView(tagName, allocator);
            if (!view)
            {
                return {};
            }

            ApplyAttributes(element, tagName, view.Get(), context, warnings);

            // A view that draws one view as its content (a ContentButton) takes its one child
            // element as that content.
            if (!Cast<ViewGroup>(view.Get()))
            {
                bool taken = false;
                for (xml::XmlNode* childNode : element->Children())
                {
                    if (childNode->NodeType() != xml::XmlNodeType::Element)
                    {
                        continue;
                    }
                    xml::XmlElement* childElem = static_cast<xml::XmlElement*>(childNode);
                    if (taken)
                    {
                        if (warnings != nullptr)
                        {
                            String w(u8"<");
                            w.Append(tagName);
                            w.Append(u8"> holds one element as its content: <");
                            w.Append(childElem->TagName());
                            w.Append(u8"> dropped (wrap several in a Flex)");
                            warnings->PushBack(Move(w));
                        }
                        continue;
                    }
                    RefPtr<View> childView = BuildView(allocator, childElem, context, warnings);
                    if (childView && view->SetContentChild(childView))
                    {
                        taken = true;
                    }
                    else if (warnings != nullptr)
                    {
                        String w(childView ? u8"<" : u8"unknown element <");
                        w.Append(childElem->TagName());
                        w.Append(childView ? u8"> dropped: <" : u8"> dropped (inside <");
                        w.Append(tagName);
                        w.Append(childView ? u8"> holds no children" : u8">)");
                        warnings->PushBack(Move(w));
                    }
                }
            }

            // Recurse into children.
            if (ViewGroup* viewGroup = Cast<ViewGroup>(view.Get()))
            {
                for (xml::XmlNode* childNode : element->Children())
                {
                    if (childNode->NodeType() == xml::XmlNodeType::Element)
                    {
                        xml::XmlElement* childElem = static_cast<xml::XmlElement*>(childNode);
                        RefPtr<View> childView =
                            BuildView(allocator, childElem, context, warnings);
                        if (childView)
                        {
                            viewGroup->AddView(childView.Get());
                        }
                        else if (warnings != nullptr)
                        {
                            String w(u8"unknown element <");
                            w.Append(childElem->TagName());
                            w.Append(u8"> dropped (inside <");
                            w.Append(tagName);
                            w.Append(u8">)");
                            warnings->PushBack(Move(w));
                        }
                    }
                }
            }

            // Text content: <Button>Click Me</Button> -> set as "text" property if the view has one.
            String textContent;
            GetTextContent(element, textContent);
            if (textContent.Size() > 0)
            {
                MarkupRegistry::SetProperty(tagName, view.Get(), u8"text", textContent.AsView());
            }

            return view;
        }

        /// Apply XML attributes, routing to special attributes, the LayoutStyle vocabulary
        /// (the same on every element, whatever the parent), or registered properties.
        static void ApplyAttributes(xml::XmlElement* element, StringView tagName, View* view,
                                    UIContext* context, Array<String>* warnings = nullptr)
        {
            (void)context;
            LayoutStyle layout = view->Layout();
            for (xml::XmlAttribute* attr : element->Attributes())
            {
                const StringView name = attr->Name();
                const StringView value = attr->Value();

                // === Special attributes ===

                if (name == u8"id" || name == u8"name")
                {
                    view->Name = String(value);
                    continue;
                }

                if (name == u8"class")
                {
                    ForEachSplit(value, u8' ',
                                 [view](StringView cls)
                                 {
                                     const StringView t = Trimmed(cls);
                                     if (t.Size() > 0)
                                     {
                                         view->AddClass(t);
                                     }
                                 });
                    continue;
                }

                if (name == u8"style")
                {
                    SSSParser::ApplyInlineStyle(view, value);
                    continue;
                }

                // === Common View properties ===

                if (name == u8"visibility")
                {
                    if (value == u8"visible")
                    {
                        view->Visibility = Visibility::Visible;
                    }
                    else if (value == u8"hidden")
                    {
                        view->Visibility = Visibility::Hidden;
                    }
                    else if (value == u8"gone")
                    {
                        view->Visibility = Visibility::Gone;
                    }
                    continue;
                }

                if (name == u8"is-enabled")
                {
                    view->IsEnabled = (value == u8"true");
                    continue;
                }

                if (name == u8"opacity")
                {
                    if (Optional<f64> f = ParseFloat(value); f.HasValue())
                    {
                        view->Opacity = static_cast<f32>(f.Value());
                    }
                    continue;
                }

                if (name == u8"cursor")
                {
                    if (value == u8"hand")
                    {
                        view->Cursor = CursorType::Hand;
                    }
                    else if (value == u8"ibeam")
                    {
                        view->Cursor = CursorType::IBeam;
                    }
                    else if (value == u8"crosshair")
                    {
                        view->Cursor = CursorType::Crosshair;
                    }
                    else if (value == u8"arrow")
                    {
                        view->Cursor = CursorType::Arrow;
                    }
                    else if (value == u8"move")
                    {
                        view->Cursor = CursorType::Move;
                    }
                    continue;
                }

                if (name == u8"tooltip")
                {
                    view->TooltipText = String(value);
                    continue;
                }
                if (name == u8"is-focusable")
                {
                    view->IsFocusable = (value == u8"true");
                    continue;
                }
                if (name == u8"is-tab-stop")
                {
                    view->IsTabStop = (value == u8"true");
                    continue;
                }
                if (name == u8"tab-index")
                {
                    if (Optional<i64> idx = ParseInt(value); idx.HasValue())
                    {
                        view->TabIndex = static_cast<i32>(idx.Value());
                    }
                    continue;
                }

                if (name == u8"padding")
                {
                    if (ViewGroup* vg = Cast<ViewGroup>(view))
                    {
                        vg->Padding = MarkupRegistry::ParseThickness(value);
                    }
                    else
                    {
                        // A control's padding is a style (its DefaultStylePadding when none is
                        // set), so the attribute sets it inline rather than being dropped.
                        view->SetStyle(StyleProperty::Padding, MarkupRegistry::ParseThickness(value));
                    }
                    continue;
                }

                if (name == u8"clips-content")
                {
                    view->ClipsContent = (value == u8"true");
                    continue;
                }

                // === LayoutStyle (width/height/margin/flex-grow/dock/grid-row/...) ===

                // A gravity name that matches none is dropped like an unknown attribute, so it is
                // surfaced the same way (the names are PascalCase: Top|Right, not top|right).
                if (name == u8"gravity" && warnings != nullptr)
                {
                    Array<String> unknown;
                    (void)MarkupRegistry::ParseGravity(value, &unknown);
                    for (const String& gravity : unknown)
                    {
                        String w(u8"unknown gravity '");
                        w.Append(gravity.AsView());
                        w.Append(u8"' on <");
                        w.Append(tagName);
                        w.Append(u8"> (the names are PascalCase: Left, Right, Top, Bottom, Center, "
                                 u8"Fill, TopRight, ...)");
                        warnings->PushBack(Move(w));
                    }
                }

                if (MarkupRegistry::ApplyLayoutAttribute(layout, name, value))
                {
                    continue;
                }

                // === Control-specific properties via registry ===
                if (MarkupRegistry::SetProperty(tagName, view, name, value))
                {
                    continue;
                }

                // Unknown attribute: ignored at runtime, but SURFACED to the cook - a
                // camelCase typo (fontSize vs font-size) can silently produce an invisible HUD.
                if (warnings != nullptr)
                {
                    String w(u8"unknown attribute '");
                    w.Append(name);
                    w.Append(u8"' on <");
                    w.Append(tagName);
                    w.Append(u8">");
                    warnings->PushBack(Move(w));
                }
            }
            view->SetLayout(layout);
        }

        /// Extract direct text content from an element (not child elements).
        static void GetTextContent(xml::XmlElement* element, String& outText)
        {
            for (xml::XmlNode* child : element->Children())
            {
                if (child->NodeType() == xml::XmlNodeType::Text)
                {
                    const StringView t = Trimmed(static_cast<xml::XmlText*>(child)->Text());
                    if (t.Size() > 0)
                    {
                        if (outText.Size() > 0)
                        {
                            outText.Append(u8" ");
                        }
                        outText.Append(t);
                    }
                }
            }
        }

        template <typename F>
        static void ForEachSplit(StringView s, char8_t delim, F&& fn)
        {
            const char8_t* data = s.Data();
            const usize n = s.Size();
            usize start = 0;
            for (usize i = 0; i <= n; ++i)
            {
                if (i == n || data[i] == delim)
                {
                    if (i > start)
                    {
                        fn(StringView{data + start, i - start});
                    }
                    start = i + 1;
                }
            }
        }
    };
}
