// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :style_applier partition
//
// ApplyStyle: writes a resolved style's typed values onto a node (G2 of Specs/vg-and-gui-lab.md).
// Node and text properties go to their setters; the box properties (background colour and layers,
// a border per side, radii, box shadows, filters, the backdrop filter, the blend mode) become the node's NodeBox and `transform` its style
// transform, both owned by the sheet; a property the table (StyleProperties) does not name goes to
// the widget's reflected property declaring it as its CSS name, so a library's widget is styled
// with no applier code of its own.

module;
#include "Core/Prelude.h"

export module experimental.gui:style_applier;

import foundation.core;  // Cast, Optional, Color, Float2, MakeRef, DefaultAllocator
import foundation.fonts; // CachedFont, IFontService
import foundation.image; // ImageData
import foundation.vg;    // CornerRadii
import :thickness;
import :text; // TextHAlign / TextVAlign
import :node;
import :ui_node;
import :ui_widget;
import :drawable;
import :node_box;
import :style_sheet; // ResolvedStyle
import :style_properties;
import :style_value;
import :css_values;
import :resource_provider;

using namespace foundation::core;
namespace core = foundation::core;
// Declared here, not inherited: a namespace alias is not an exported entity, so one written in
// a sibling partition is not visible in this one. This file uses fonts::, image:: and vg:: but
// declared none of them; clang happened to resolve them anyway, while MSVC (correctly) reported
// "C2653: '<name>': is not a class or namespace name". Every other partition using these
// declares its own aliases the same way. One per module imported above and referenced qualified.
namespace fonts = foundation::fonts;
namespace image = foundation::image;
namespace vg = foundation::vg;

export namespace experimental::gui
{
    // Strip a CSS url(...) wrapper (and any quotes) to the inner resource name; returns the
    // value unchanged if it is not a url() form.
    [[nodiscard]] inline core::StringView ParseUrl(core::StringView value)
    {
        core::StringView v = core::Trim(value);
        if (v.Size() >= 5 && v.SubStr(0, 4) == core::StringView(u8"url(") &&
            v[v.Size() - 1] == u8')')
            v = core::Trim(v.SubStr(4, v.Size() - 5));
        if (v.Size() >= 2 && (v[0] == u8'"' || v[0] == u8'\'') && v[v.Size() - 1] == v[0])
            v = v.SubStr(1, v.Size() - 2);
        return v;
    }

    namespace detail
    {
        [[nodiscard]] inline f32 PixelsOf(const StyleValue* v, const LengthContext& lengths, f32 percentBase, f32 fallback)
        {
            return v != nullptr && v->Kind == StyleValueKind::Length ? ResolveLengthValue(v->Length, lengths, percentBase)
                                                                    : fallback;
        }

        [[nodiscard]] inline BoxGradient ToBoxGradient(const GradientValue& g)
        {
            BoxGradient out;
            out.Kind = g.Kind == GradientKind::Linear   ? BoxGradientKind::Linear
                       : g.Kind == GradientKind::Radial ? BoxGradientKind::Radial
                                                        : BoxGradientKind::Conic;
            out.Angle = g.Angle;
            out.Repeating = g.Repeating;
            for (const GradientStopValue& stop : SettleStops(g.Stops))
                out.Stops.PushBack(vg::GradientStop{stop.Position, stop.StopColor});
            return out;
        }

        // mix-blend-mode: the blends VG composites (plus-lighter is additive); another is normal.
        [[nodiscard]] inline vg::VGBlendMode BlendModeOf(core::StringView keyword)
        {
            if (keyword == core::StringView(u8"multiply"))
                return vg::VGBlendMode::Multiply;
            if (keyword == core::StringView(u8"screen"))
                return vg::VGBlendMode::Screen;
            if (keyword == core::StringView(u8"plus-lighter"))
                return vg::VGBlendMode::Additive;
            return vg::VGBlendMode::Normal;
        }

        [[nodiscard]] inline bool AnyBoxProperty(const ResolvedStyle& style)
        {
            for (const ResolvedProperty& p : style.Properties())
            {
                const core::StringView n = p.Name.AsView();
                if (n == core::StringView(u8"background-color") || n == core::StringView(u8"background-image") ||
                    n == core::StringView(u8"box-shadow") || n == core::StringView(u8"filter") ||
                    n == core::StringView(u8"backdrop-filter") || n == core::StringView(u8"mix-blend-mode") ||
                    (n.Size() > 7 && n.SubStr(0, 7) == core::StringView(u8"border-")))
                    return true;
            }
            return false;
        }

        // The node's box from the sheet's box properties, in pixels.
        [[nodiscard]] inline NodeBox BuildBox(const UINode& node, const ResolvedStyle& style, IResourceProvider* resources,
                                              const LengthContext& lengths)
        {
            NodeBox box;
            if (const StyleValue* c = style.Value(u8"background-color"))
                box.BackgroundColor = c->ColorValue;
            if (const StyleValue* layers = style.Value(u8"background-image"))
            {
                for (const BackgroundLayer& layer : layers->Layers)
                {
                    BoxLayer out;
                    if (layer.Kind == BackgroundKind::Image)
                    {
                        out.IsImage = true;
                        out.Image = resources != nullptr ? resources->LoadImage(layer.Image.AsView()) : nullptr;
                        if (out.Image == nullptr)
                            continue; // not loaded (no provider, or no such image)
                    }
                    else if (layer.Kind == BackgroundKind::Gradient)
                    {
                        out.Gradient = ToBoxGradient(layer.Gradient);
                    }
                    else
                    {
                        continue; // a colour layer above the bottom is not CSS
                    }
                    box.Layers.PushBack(Move(out));
                }
            }
            // A border per side: its style none or hidden draws nothing, whatever its width.
            const auto side = [&](const char8_t* name, f32& width, Color& color)
            {
                core::String w(u8"border-");
                w.Append(core::StringView(name));
                core::String styleName = w;
                core::String colorName = w;
                w.Append(core::StringView(u8"-width"));
                styleName.Append(core::StringView(u8"-style"));
                colorName.Append(core::StringView(u8"-color"));
                const StyleValue* styleValue = style.Value(styleName.AsView());
                const core::StringView lineStyle = styleValue != nullptr ? styleValue->Keyword.AsView() : core::StringView(u8"none");
                const bool drawn = lineStyle != core::StringView(u8"none") && lineStyle != core::StringView(u8"hidden");
                width = drawn ? Max(0.0f, PixelsOf(style.Value(w.AsView()), lengths, 0.0f, 3.0f)) : 0.0f;
                if (const StyleValue* c = style.Value(colorName.AsView()))
                    color = c->ColorValue;
                else
                    color = Color{0.0f, 0.0f, 0.0f, 1.0f};
            };
            side(u8"top", box.BorderWidths.top, box.BorderColors.top);
            side(u8"right", box.BorderWidths.right, box.BorderColors.right);
            side(u8"bottom", box.BorderWidths.bottom, box.BorderColors.bottom);
            side(u8"left", box.BorderWidths.left, box.BorderColors.left);
            // Radii: a percentage of the box's smaller side.
            const core::Float2 size = node.GetSize();
            const f32 percentBase = Min(size.x, size.y);
            box.Radii = vg::CornerRadii(PixelsOf(style.Value(u8"border-top-left-radius"), lengths, percentBase, 0.0f),
                                        PixelsOf(style.Value(u8"border-top-right-radius"), lengths, percentBase, 0.0f),
                                        PixelsOf(style.Value(u8"border-bottom-right-radius"), lengths, percentBase, 0.0f),
                                        PixelsOf(style.Value(u8"border-bottom-left-radius"), lengths, percentBase, 0.0f));
            if (const StyleValue* shadows = style.Value(u8"box-shadow"))
            {
                for (const ShadowValue& s : shadows->Shadows)
                {
                    BoxShadow shadow;
                    shadow.Offset = core::Float2{ResolveLengthValue(s.OffsetX, lengths), ResolveLengthValue(s.OffsetY, lengths)};
                    shadow.Blur = ResolveLengthValue(s.Blur, lengths);
                    shadow.Spread = ResolveLengthValue(s.Spread, lengths);
                    shadow.ShadowColor = s.ShadowColor;
                    shadow.Inset = s.Inset;
                    box.Shadows.PushBack(shadow);
                }
            }
            // Effects: blurs in sequence add as variances (a blur of a blur is one of the root of the
            // squares' sum).
            const auto blurOf = [&](const char8_t* name)
            {
                f32 variance = 0.0f;
                if (const StyleValue* filters = style.Value(name))
                    for (const FilterFunction& f : filters->Filters)
                        if (f.Kind == FilterKind::Blur)
                        {
                            const f32 sigma = ResolveLengthValue(f.Amount, lengths);
                            variance += sigma * sigma;
                        }
                return Sqrt(variance);
            };
            box.FilterBlur = blurOf(u8"filter");
            box.BackdropBlur = blurOf(u8"backdrop-filter");
            if (const StyleValue* blend = style.Value(u8"mix-blend-mode"))
                box.BlendMode = BlendModeOf(blend->Keyword.AsView());
            return box;
        }

        // A property the table does not name, given to the reflected property of the node's type
        // (or a base's) that declares it as its CSS name (the `css` property attribute), parsed by
        // that property's type.
        inline void ApplyReflected(UINode& node, const ResolvedProperty& p, const LengthContext& lengths)
        {
            for (const TypeInfo* type = node.GetType(); type != nullptr; type = type->base)
            {
                for (usize i = 0; i < PropertyCount(*type); ++i)
                {
                    const PropertyInfo& property = PropertyAt(*type, i);
                    const Attribute* css = FindAttribute(property, u8"css");
                    const core::String* cssName = css != nullptr ? css->value.TryGet<core::String>() : nullptr;
                    if (cssName == nullptr || cssName->AsView() != p.Name.AsView())
                        continue;
                    Instance instance = Instance::From(&node);
                    const core::StringView text = Trim(p.Value.AsView());
                    Variant value;
                    if (property.type == &TypeOf<f32>())
                    {
                        if (Optional<f32> px = ResolveLength(text, lengths); px.HasValue())
                            value = Variant::From<f32>(px.Value());
                    }
                    else if (property.type == &TypeOf<Color>())
                    {
                        if (Optional<Color> c = ParseColor(text); c.HasValue())
                            value = Variant::From<Color>(c.Value());
                    }
                    else if (property.type == &TypeOf<bool>())
                    {
                        if (Optional<bool> b = ParseBool(text); b.HasValue())
                            value = Variant::From<bool>(b.Value());
                    }
                    else if (property.type == &TypeOf<core::String>())
                    {
                        value = Variant::From<core::String>(core::String(text));
                    }
                    if (!value.IsEmpty())
                        (void)SetProperty(property, instance, value);
                    return;
                }
            }
        }
    }

    // Apply the supported declarations of `style` to `node`. `resources` (optional) loads
    // background-image; `fontService` (optional) resolves font-family/size. Missing either
    // just skips that property. The box properties (background, border, radius, box-shadow)
    // become the node's box, owned by the sheet: a restyle without any replaces it with none.
    inline void ApplyStyle(UINode& node, const ResolvedStyle& input,
                           IResourceProvider* resources = nullptr,
                           fonts::IFontService* fontService = nullptr,
                           const LengthContext& lengths = {})
    {
        using core::StringView;
        ResolvedStyle settledCopy;
        const ResolvedStyle* settled = &input;
        if (!input.IsFinalized())
        {
            settledCopy = input;
            settledCopy.Finalize();
            settled = &settledCopy;
        }
        const ResolvedStyle& style = *settled;

        // The containing dimensions percentages resolve against (the parent's size).
        core::Float2 percentBase{0.0f, 0.0f};
        if (Node* parent = node.GetParent())
            percentBase = parent->GetSize();
        const auto length = [&](const char8_t* name, f32 base) -> Optional<f32>
        {
            const StyleValue* v = style.Value(name);
            if (v == nullptr || v->Kind != StyleValueKind::Length)
                return {};
            return ResolveLengthValue(v->Length, lengths, base);
        };
        const auto keyword = [&](const char8_t* name) -> StringView
        {
            const StyleValue* v = style.Value(name);
            return v != nullptr ? v->Keyword.AsView() : StringView{};
        };

        if (const StyleValue* o = style.Value(u8"opacity"))
            node.SetAlpha(o->Number);

        // Padding (and margin, a widget's) per side; the sides not declared keep their value.
        {
            Thickness padding = node.GetPadding();
            bool changed = false;
            if (Optional<f32> v = length(u8"padding-top", 0.0f); v.HasValue()) { padding.Top = v.Value(); changed = true; }
            if (Optional<f32> v = length(u8"padding-right", 0.0f); v.HasValue()) { padding.Right = v.Value(); changed = true; }
            if (Optional<f32> v = length(u8"padding-bottom", 0.0f); v.HasValue()) { padding.Bottom = v.Value(); changed = true; }
            if (Optional<f32> v = length(u8"padding-left", 0.0f); v.HasValue()) { padding.Left = v.Value(); changed = true; }
            if (changed)
                node.SetPadding(padding);
        }
        if (UIWidget* widget = core::Cast<UIWidget>(&node))
        {
            Thickness margin = widget->GetMargin();
            bool changed = false;
            if (Optional<f32> v = length(u8"margin-top", 0.0f); v.HasValue()) { margin.Top = v.Value(); changed = true; }
            if (Optional<f32> v = length(u8"margin-right", 0.0f); v.HasValue()) { margin.Right = v.Value(); changed = true; }
            if (Optional<f32> v = length(u8"margin-bottom", 0.0f); v.HasValue()) { margin.Bottom = v.Value(); changed = true; }
            if (Optional<f32> v = length(u8"margin-left", 0.0f); v.HasValue()) { margin.Left = v.Value(); changed = true; }
            if (changed)
                widget->SetMargin(margin);
        }

        // min/max-width/height: set the size constraints before width/height so the applied
        // size is clamped to them.
        {
            core::Float2 mn = node.GetMinSize();
            core::Float2 mx = node.GetMaxSize();
            bool changedMin = false, changedMax = false;
            if (Optional<f32> v = length(u8"min-width", percentBase.x); v.HasValue()) { mn.x = v.Value(); changedMin = true; }
            if (Optional<f32> v = length(u8"min-height", percentBase.y); v.HasValue()) { mn.y = v.Value(); changedMin = true; }
            if (Optional<f32> v = length(u8"max-width", percentBase.x); v.HasValue()) { mx.x = v.Value(); changedMax = true; }
            if (Optional<f32> v = length(u8"max-height", percentBase.y); v.HasValue()) { mx.y = v.Value(); changedMax = true; }
            if (changedMin)
                node.SetMinSize(mn);
            if (changedMax)
                node.SetMaxSize(mx);
        }
        {
            core::Float2 size = node.GetSize();
            bool changed = false;
            if (Optional<f32> w = length(u8"width", percentBase.x); w.HasValue()) { size.x = w.Value(); changed = true; }
            if (Optional<f32> h = length(u8"height", percentBase.y); h.HasValue()) { size.y = h.Value(); changed = true; }
            if (changed)
                node.SetSize(size);
        }

        // overflow: any value but visible clips the children to the box (the lab does not scroll
        // a box by it; ScrollView does).
        if (const StringView v = keyword(u8"overflow"); v.Size() != 0)
            node.SetClipChildren(v != StringView(u8"visible"));

        if (const StringView e = keyword(u8"enabled"); e.Size() != 0)
            if (Optional<bool> b = ParseBool(e); b.HasValue())
                node.SetEnabled(b.Value());
        if (const StringView v = keyword(u8"visibility"); v == StringView(u8"hidden"))
            node.SetVisible(false);
        else if (v == StringView(u8"visible"))
            node.SetVisible(true);

        // Text: colour, alignment, face.
        if (const StyleValue* c = style.Value(u8"color"))
            node.SetThemeTextColor(c->ColorValue);
        if (const StringView v = keyword(u8"text-align"); v.Size() != 0)
        {
            if (v == StringView(u8"left") || v == StringView(u8"justify"))
                node.SetThemeTextAlign(TextHAlign::Left);
            else if (v == StringView(u8"center") || v == StringView(u8"centre"))
                node.SetThemeTextAlign(TextHAlign::Center);
            else if (v == StringView(u8"right"))
                node.SetThemeTextAlign(TextHAlign::Right);
        }
        if (const StringView v = keyword(u8"vertical-align"); v.Size() != 0)
        {
            if (v == StringView(u8"top"))
                node.SetThemeTextAlignV(TextVAlign::Top);
            else if (v == StringView(u8"middle") || v == StringView(u8"center"))
                node.SetThemeTextAlignV(TextVAlign::Middle);
            else if (v == StringView(u8"bottom"))
                node.SetThemeTextAlignV(TextVAlign::Bottom);
        }
        if (fontService != nullptr)
        {
            if (const StringView family = ParseUrl(keyword(u8"font-family")); family.Size() != 0) // strips quotes
            {
                const f32 size = length(u8"font-size", 0.0f).ValueOr(16.0f);
                if (size > 0.0f)
                    if (fonts::CachedFont* font = fontService->GetFont(family, size))
                        node.SetThemeFont(font);
            }
        }

        // The box: the sheet's, or none when a restyle drops every box property it had set.
        if (detail::AnyBoxProperty(style))
            node.SetBox(detail::BuildBox(node, style, resources, lengths), true);
        else if (node.IsBoxFromStyle())
            node.SetBox(NodeBox{}, true);

        // The transform about its origin; likewise owned by the sheet once it sets one.
        const StyleValue* transform = style.Value(u8"transform");
        const StyleValue* origin = style.Value(u8"transform-origin");
        if (transform != nullptr || origin != nullptr)
        {
            node.SetStyleTransform(transform != nullptr ? transform->Transforms : Array<TransformFunction>{},
                                   origin != nullptr ? origin->OriginX : LengthValue{50.0f, LengthUnit::Percent},
                                   origin != nullptr ? origin->OriginY : LengthValue{50.0f, LengthUnit::Percent});
        }
        else if (node.IsTransformFromStyle())
        {
            node.SetStyleTransform(Array<TransformFunction>{}, LengthValue{50.0f, LengthUnit::Percent},
                                   LengthValue{50.0f, LengthUnit::Percent});
        }

        // What the table does not name: a reflected property that takes it by its CSS name.
        for (const ResolvedProperty& p : style.Properties())
            if (!ResolvedStyle::IsCustomProperty(p.Name.AsView()) && FindStyleProperty(p.Name.AsView()) == nullptr)
                detail::ApplyReflected(node, p, lengths);
    }

    // Apply a resolved pseudo-element style to a widget part: its background-color becomes the
    // part's color (slider::fill, window::title, scrollbar::thumb, ...).
    inline void ApplyPartStyle(UINode& node, core::StringView part, const ResolvedStyle& style)
    {
        if (style.Has(core::StringView(u8"background-color")))
            if (Optional<Color> c = ParseColor(style.Get(core::StringView(u8"background-color")));
                c.HasValue())
                node.SetThemePartColor(part, c.Value());
    }
}
