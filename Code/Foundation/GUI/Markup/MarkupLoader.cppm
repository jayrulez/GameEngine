// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :markup partition
//
// MarkupLoader: inflates a widget tree from XML markup. The loader's ElementSet resolves each
// element name to a registered element type (ElementTypes holds the GUI's widgets and those of
// any other library), and the loader makes it from its own allocator. Nested elements become
// children. An element the set does not know is left out with a warning.
// Attributes are applied in tiers:
//   - `id` and `class` set the widget's CSS identity.
//   - A value starting with `@` on a translatable property is a translation key; its count and
//     arguments go in `<prop>-count` and `<prop>-args`. `@@` is a literal @.
//   - A value containing `{{name}}` binds the reflected property to the field `name` of the
//     loader's data model. `value="{{volume}}"` follows `volume` and writes it back when the
//     user can change the property (a slider's value, a checkbox, a text field's text).
//     `text="Score: {{score}}"` follows the text built from `score`.
//   - An attribute the widget claims through UINode::SetMarkupAttribute (such as Label's "text"
//     and "wrap") is consumed by the widget.
//   - Every other attribute is treated as an inline CSS property and run through the
//     StyleApplier. The loader warns about one that names no style property, shorthand or
//     reflected property.
//   - A `<template type="...">` child is kept as markup by its parent
//     (UINode::SetMarkupTemplate), which makes nodes from it later (an item view's rows). When
//     the loader has item bindings, a value containing `{{item}}`, `{{item.Column}}` or
//     `{{index}}` is recorded for the row to fill.
// As a result, the whole CSS property vocabulary (width, height, padding, margin,
// background-color, border, text-align, units and so on) works in markup. When the loader has a
// diagnostics list, dropped elements and attributes and parse errors go there for a cook to
// report; without one they are logged.
//
// Example:
//   <Panel flex-direction="row" gap="8" padding="12" class="panel">
//     <Label text="Hello" text-align="center"/>
//     <Button text="OK" width="120" class="accent"/>
//   </Panel>

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h" // LOG_WARNING, for markup names that nothing takes

export module foundation.gui:markup;

import foundation.core;
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
import :data_model;    // DataModel, FormatVariant
import :diagnostic;    // GuiDiagnostic

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
namespace xml = foundation::xml;

export namespace foundation::gui
{
    /// A run of a `{{...}}` template: literal text, or a name.
    struct MarkupTemplatePart
    {
        core::String Text;
        bool IsName = false;
    };

    /// An attribute an item view's row fills from its item (`text="{{item.Name}}"`).
    struct ItemBinding
    {
        Node* Target = nullptr; // in the row's content, which owns it
        core::String Attribute;
        Array<MarkupTemplatePart> Parts;
    };

    class MarkupLoader
    {
    public:
        MarkupLoader(const ElementSet& elements, IAllocator& allocator, IResourceProvider* resources = nullptr,
                     fonts::IFontService* fontService = nullptr, LengthContext lengths = {}) noexcept
            : m_elements(&elements), m_allocator(&allocator), m_resources(resources), m_fontService(fontService),
              m_lengths(lengths)
        {
        }

        /// Sends dropped elements (no type makes them), dropped attributes (nothing takes them)
        /// and parse errors to `diagnostics`, where a cook reports them, instead of the log. Null
        /// sends them to the log.
        void SetDiagnostics(Array<GuiDiagnostic>* diagnostics) noexcept { m_diagnostics = diagnostics; }

        /// Sets the data model that `{{name}}` values bind to (usually a SceneRoot's, from
        /// GetDataRef). Without a model those values stay as written.
        void SetDataModel(RefPtr<DataModel> model) { m_data = core::Move(model); }
        /// Records `{{item...}}` and `{{index}}` attributes in `bindings` instead of applying
        /// them, so that a row made from an item view's template can fill them.
        void SetItemBindings(Array<ItemBinding>* bindings) noexcept { m_itemBindings = bindings; }

        /// Applies one attribute the way markup does: the widget takes it if it claims it, and
        /// otherwise it becomes inline CSS for the style pass to apply. A row's item bindings
        /// write their values through this.
        static void ApplyAttribute(Node& node, core::StringView name, core::StringView value)
        {
            UINode* uinode = core::Cast<UINode>(&node);
            if (uinode == nullptr)
                return;
            if (!uinode->SetMarkupAttribute(name, value))
                uinode->SetInlineStyle(name, value);
        }


        // Inflates the root element of `markup` into a widget tree; null on a parse error or an
        // unknown root element.
        [[nodiscard]] RefPtr<Node> LoadFromString(core::StringView markup)
        {
            xml::XmlDocument document(*m_allocator);
            if (const xml::XmlResult result = document.Parse(markup); result != xml::XmlResult::Ok)
            {
                Report(GuiDiagnostic::Severity::Error, static_cast<u32>(core::Max(document.ErrorLine(), 0)),
                       core::Format(u8"the markup does not parse: {}", xml::Describe(result)));
                return {};
            }
            xml::XmlElement* root = FirstElementChild(document);
            if (root == nullptr)
            {
                Report(GuiDiagnostic::Severity::Error, 0, core::String(u8"the markup has no element"));
                return {};
            }
            return Inflate(*root);
        }

    private:
        [[nodiscard]] RefPtr<Node> Inflate(xml::XmlElement& element)
        {
            RefPtr<Node> widget = m_elements->Create(element.TagName(), *m_allocator);
            if (!widget)
            {
                // This GUI cannot make the element (a typo, or a type its element set leaves out).
                // Report it, so the subtree does not vanish silently.
                Report(GuiDiagnostic::Severity::Warning, element.Line(),
                       core::Format(u8"no element <{}>; it and what it holds are left out", element.TagName()));
                return {};
            }

            UINode* uinode = core::Cast<UINode>(widget.Get());
            UIWidget* uiwidget = core::Cast<UIWidget>(widget.Get());

            ResolvedStyle inlineStyle;
            for (xml::XmlAttribute* attr : element.Attributes())
            {
                const core::StringView name = attr->Name();
                core::StringView value = attr->Value();
                // A translated text such as `text="@menu.play"`. Its count and arguments are in
                // `text-count` and `text-args`, each a value or a {{binding}}. `@@` is a literal @.
                if (IsTranslationExtra(element, name))
                    continue;
                if (value.Size() > 1 && value[0] == u8'@')
                {
                    if (value[1] == u8'@')
                        value = value.SubStr(1, value.Size() - 1);
                    else if (BindTranslated(element, *widget, name, value.SubStr(1, value.Size() - 1)))
                        continue;
                }
                if (name == core::StringView(u8"id"))
                {
                    if (uiwidget != nullptr)
                        uiwidget->SetId(value);
                    continue;
                }
                if (name == core::StringView(u8"class"))
                {
                    if (uiwidget != nullptr)
                        uiwidget->AddClasses(value);
                    continue;
                }
                if (m_itemBindings != nullptr && IsItemTemplate(value))
                {
                    ItemBinding binding{widget.Get(), core::String(name), {}};
                    if (SplitTemplate(value, binding.Parts))
                    {
                        m_itemBindings->PushBack(core::Move(binding));
                        continue;
                    }
                }
                if (m_data && FindFrom(value, core::StringView(u8"{{"), 0) < value.Size() && Bind(*widget, name, value))
                    continue;
                if (uinode != nullptr && uinode->SetMarkupAttribute(name, value))
                    continue;
                if (!IsStyleName(uinode, name, value))
                    Report(GuiDiagnostic::Severity::Warning, attr->Line(),
                           core::Format(u8"<{}> has no attribute or style property '{}'", element.TagName(), name));
                inlineStyle.Set(name, value); // an inline CSS property
                if (uinode != nullptr)
                    uinode->SetInlineStyle(name, value); // kept so it outranks the sheets at every restyle
            }
            if (uinode != nullptr)
                ApplyStyle(*uinode, inlineStyle, m_resources, m_fontService, m_lengths);

            for (xml::XmlElement* child = element.FirstChildElement(); child != nullptr;
                 child = child->NextSiblingElement())
            {
                if (child->TagName() == core::StringView(u8"template"))
                {
                    // The parent keeps the template as markup and makes nodes from it later.
                    xml::XmlElement* body = child->FirstChildElement();
                    if (uinode != nullptr && body != nullptr)
                    {
                        xml::XmlWriter writer;
                        writer.WriteNode(*body);
                        (void)uinode->SetMarkupTemplate(child->GetAttribute(core::StringView(u8"type")), writer.Output());
                    }
                    continue;
                }
                if (RefPtr<Node> childWidget = Inflate(*child))
                    widget->GetChildHost()->AddChild(childWidget.Get()); // e.g. a scroller's or a window's content
            }

            return widget;
        }

        // Reports a message into the diagnostics list when there is one, else to the log.
        void Report(GuiDiagnostic::Severity level, u32 line, core::String message)
        {
            if (m_diagnostics != nullptr)
            {
                m_diagnostics->PushBack(GuiDiagnostic{level, line, core::Move(message)});
                return;
            }
            LOG_WARNING(u8"GUI", u8"markup: {}", message.AsView());
        }

        // Whether an attribute names something a style sets: a property the sheets know, a shorthand
        // for such properties, a custom property, or a property the widget's type takes by its CSS
        // name.
        [[nodiscard]] static bool IsStyleName(const UINode* node, core::StringView name, core::StringView value)
        {
            if (FindStyleProperty(name) != nullptr || (name.Size() > 2 && name[0] == u8'-' && name[1] == u8'-'))
                return true;
            Array<StyleLonghand> longhands;
            if (ExpandStyleShorthand(name, value, longhands) && !(longhands.Size() == 1 && longhands[0].Name.AsView() == name))
                return true;
            return node != nullptr && detail::FindCssProperty(*node, name) != nullptr;
        }

    public:
        /// Where `needle` starts in `text` at or after `from`; text.Size() when it does not.
        [[nodiscard]] static usize FindFrom(core::StringView text, core::StringView needle, usize from)
        {
            for (usize i = from; i + needle.Size() <= text.Size(); ++i)
                if (text.SubStr(i, needle.Size()) == needle)
                    return i;
            return text.Size();
        }
        /// Splits `text` into its literal runs and `{{name}}` parts, trimming the names. Returns
        /// false when a `{{` is not closed or a name is empty.
        [[nodiscard]] static bool SplitTemplate(core::StringView text, Array<MarkupTemplatePart>& parts)
        {
            usize at = 0;
            while (at < text.Size())
            {
                const usize open = FindFrom(text, core::StringView(u8"{{"), at);
                if (open > at)
                    parts.PushBack(MarkupTemplatePart{core::String(text.SubStr(at, open - at)), false});
                if (open == text.Size())
                    break;
                const usize start = open + 2;
                const usize close = FindFrom(text, core::StringView(u8"}}"), start);
                if (close == text.Size())
                    return false;
                const core::StringView name = core::Trim(text.SubStr(start, close - start));
                if (name.Size() == 0)
                    return false;
                parts.PushBack(MarkupTemplatePart{core::String(name), true});
                at = close + 2;
            }
            return true;
        }

        // Whether `name` is `<prop>-count` or `<prop>-args` on an element that also has
        // `<prop>="@key"`. Those hold the translation's count and arguments, which BindTranslated
        // reads. Any other attribute with such a name belongs to the widget
        // (`animation-iteration-count`).
        [[nodiscard]] static bool IsTranslationExtra(const xml::XmlElement& element, core::StringView name)
        {
            core::StringView base;
            const core::StringView suffixes[] = {core::StringView(u8"-count"), core::StringView(u8"-args")};
            for (const core::StringView suffix : suffixes)
                if (name.Size() > suffix.Size() && name.SubStr(name.Size() - suffix.Size(), suffix.Size()) == suffix)
                    base = name.SubStr(0, name.Size() - suffix.Size());
            if (base.Size() == 0)
                return false;
            const core::StringView value = element.GetAttribute(base);
            return value.Size() > 1 && value[0] == u8'@' && value[1] != u8'@';
        }
        // Parses one translation argument: {{name}} reads the GUI's data model, and anything else
        // is a literal value.
        [[nodiscard]] static Node::TranslationArgument TranslationArgumentOf(core::StringView text)
        {
            text = core::Trim(text);
            if (text.Size() > 4 && text.SubStr(0, 2) == core::StringView(u8"{{") && text.SubStr(text.Size() - 2, 2) == core::StringView(u8"}}"))
                return Node::TranslationArgument{core::String(core::Trim(text.SubStr(2, text.Size() - 4))), true};
            return Node::TranslationArgument{core::String(text), false};
        }
        bool BindTranslated(xml::XmlElement& element, Node& node, core::StringView property, core::StringView key)
        {
            core::String countName(property);
            countName.Append(core::StringView(u8"-count"));
            core::String argsName(property);
            argsName.Append(core::StringView(u8"-args"));
            Optional<Node::TranslationArgument> count;
            Array<Node::TranslationArgument> args;
            for (xml::XmlAttribute* attr : element.Attributes())
            {
                if (attr->Name() == countName.AsView())
                    count = TranslationArgumentOf(attr->Value());
                else if (attr->Name() == argsName.AsView())
                {
                    const core::StringView list = attr->Value();
                    usize start = 0;
                    for (usize i = 0; i <= list.Size(); ++i)
                        if (i == list.Size() || list[i] == u8',')
                        {
                            args.PushBack(TranslationArgumentOf(list.SubStr(start, i - start)));
                            start = i + 1;
                        }
                }
            }
            return node.BindTranslation(property, key, m_data, core::Move(count), core::Move(args));
        }

        // Binds `property` of `node` to the template `value`. A lone `{{name}}` follows the data
        // value as it is, and writes it back where the property allows. A template with text
        // around the names follows the text built from them. Returns false when nothing was bound
        // (no such property, or a malformed template), so the caller handles the attribute as
        // written.
        bool Bind(Node& node, core::StringView property, core::StringView value)
        {
            Array<MarkupTemplatePart> parts;
            if (!SplitTemplate(value, parts) || parts.IsEmpty())
                return false;
            RefPtr<DataModel> model = m_data;
            if (parts.Size() == 1 && parts[0].IsName)
            {
                return node.BindData(property, model, parts[0].Text.AsView());
            }
            return node.BindProperty(property,
                                     [model, templateParts = core::Move(parts)]
                                     {
                                         core::String text;
                                         for (const MarkupTemplatePart& part : templateParts)
                                             text.Append(part.IsName ? FormatVariant(model->Get(part.Text.AsView())).AsView()
                                                                     : part.Text.AsView());
                                         return Variant::From<core::String>(core::Move(text));
                                     });
        }

        [[nodiscard]] static xml::XmlElement* FirstElementChild(xml::XmlNode& node)
        {
            for (xml::XmlNode* c = node.FirstChild(); c != nullptr; c = c->NextSibling())
                if (c->NodeType() == xml::XmlNodeType::Element)
                    return static_cast<xml::XmlElement*>(c);
            return nullptr;
        }

        // Whether a value names the item a row shows ({{item}}, {{item.X}}, {{index}}).
        [[nodiscard]] static bool IsItemTemplate(core::StringView value)
        {
            return FindFrom(value, core::StringView(u8"{{item"), 0) < value.Size() ||
                   FindFrom(value, core::StringView(u8"{{index"), 0) < value.Size() ||
                   FindFrom(value, core::StringView(u8"{{ item"), 0) < value.Size() ||
                   FindFrom(value, core::StringView(u8"{{ index"), 0) < value.Size();
        }

        const ElementSet* m_elements;
        IAllocator* m_allocator;
        IResourceProvider* m_resources;
        fonts::IFontService* m_fontService;
        LengthContext m_lengths;
        RefPtr<DataModel> m_data;
        Array<ItemBinding>* m_itemBindings = nullptr;
        Array<GuiDiagnostic>* m_diagnostics = nullptr; // receives dropped items and errors (null: the log)
    };

}
