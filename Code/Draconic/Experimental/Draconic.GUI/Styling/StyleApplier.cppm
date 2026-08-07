// Draconic GUI - :style_applier partition
//
// ApplyStyle: writes a ResolvedStyle's known declarations onto a node's properties - the
// bridge from CSS strings to the widget setters built in earlier phases. This is the small,
// explicit stand-in for eepp's PropertySpecification/PropertyDefinition registry; it grows a
// case per supported property. background-image/font-family (which resolve to Drawable*/
// Font* via a resource provider) arrive with the resource-wiring increment.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:style_applier;

import draconic.foundation;  // Cast, Optional, Color, Float2, MakeRef, DefaultAllocator
import draconic.fonts; // CachedFont, IFontService
import draconic.image; // ImageData
import draconic.vg;    // CornerRadii
import :thickness;
import :text; // TextHAlign / TextVAlign
import :node;
import :ui_node;
import :ui_widget;
import :drawable;
import :rectangle_drawable;
import :border_drawable;
import :image_drawable;
import :style_sheet; // ResolvedStyle
import :css_values;
import :resource_provider;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;
namespace image = draconic::image;

export namespace draconic::gui
{
    // Strip a CSS url(...) wrapper (and any quotes) to the inner resource name; returns the
    // value unchanged if it is not a url() form.
    [[nodiscard]] inline foundation::StringView ParseUrl(foundation::StringView value)
    {
        foundation::StringView v = foundation::Trim(value);
        if (v.Size() >= 5 && v.SubStr(0, 4) == foundation::StringView(u8"url(") &&
            v[v.Size() - 1] == u8')')
            v = foundation::Trim(v.SubStr(4, v.Size() - 5));
        if (v.Size() >= 2 && (v[0] == u8'"' || v[0] == u8'\'') && v[v.Size() - 1] == v[0])
            v = v.SubStr(1, v.Size() - 2);
        return v;
    }

    // Apply the supported declarations of `style` to `node`. `resources` (optional) loads
    // background-image; `fontService` (optional) resolves font-family/size. Missing either
    // just skips that property.
    inline void ApplyStyle(UINode& node, const ResolvedStyle& style,
                           IResourceProvider* resources = nullptr,
                           fonts::IFontService* fontService = nullptr,
                           const LengthContext& lengths = {})
    {
        using foundation::StringView;

        // The containing dimensions percentages resolve against (the parent's size).
        foundation::Float2 percentBase{0.0f, 0.0f};
        if (Node* parent = node.GetParent())
            percentBase = parent->GetSize();

        if (style.Has(StringView(u8"background-color")))
            if (Optional<Color> c = ParseColor(style.Get(StringView(u8"background-color")));
                c.HasValue())
                node.SetBackground(
                    foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), c.Value()));

        if (style.Has(StringView(u8"padding")))
            if (Optional<Thickness> t =
                    ResolveThickness(style.Get(StringView(u8"padding")), lengths);
                t.HasValue())
                node.SetPadding(t.Value());

        if (style.Has(StringView(u8"opacity")))
            if (Optional<f32> o = ParseLength(style.Get(StringView(u8"opacity"))); o.HasValue())
                node.SetAlpha(o.Value());

        // min/max-width/height: set the size constraints before width/height so the applied
        // size is clamped to them.
        {
            foundation::Float2 mn = node.GetMinSize();
            bool changedMin = false;
            if (style.Has(StringView(u8"min-width")))
                if (Optional<f32> v =
                        ResolveLength(style.Get(StringView(u8"min-width")), lengths, percentBase.x);
                    v.HasValue())
                {
                    mn.x = v.Value();
                    changedMin = true;
                }
            if (style.Has(StringView(u8"min-height")))
                if (Optional<f32> v = ResolveLength(style.Get(StringView(u8"min-height")), lengths,
                                                    percentBase.y);
                    v.HasValue())
                {
                    mn.y = v.Value();
                    changedMin = true;
                }
            if (changedMin)
                node.SetMinSize(mn);

            foundation::Float2 mx = node.GetMaxSize();
            bool changedMax = false;
            if (style.Has(StringView(u8"max-width")))
                if (Optional<f32> v =
                        ResolveLength(style.Get(StringView(u8"max-width")), lengths, percentBase.x);
                    v.HasValue())
                {
                    mx.x = v.Value();
                    changedMax = true;
                }
            if (style.Has(StringView(u8"max-height")))
                if (Optional<f32> v = ResolveLength(style.Get(StringView(u8"max-height")), lengths,
                                                    percentBase.y);
                    v.HasValue())
                {
                    mx.y = v.Value();
                    changedMax = true;
                }
            if (changedMax)
                node.SetMaxSize(mx);
        }

        // width / height (combined into one SetSize; clamped by any min/max above).
        {
            foundation::Float2 size = node.GetSize();
            bool changed = false;
            if (style.Has(StringView(u8"width")))
                if (Optional<f32> w =
                        ResolveLength(style.Get(StringView(u8"width")), lengths, percentBase.x);
                    w.HasValue())
                {
                    size.x = w.Value();
                    changed = true;
                }
            if (style.Has(StringView(u8"height")))
                if (Optional<f32> h =
                        ResolveLength(style.Get(StringView(u8"height")), lengths, percentBase.y);
                    h.HasValue())
                {
                    size.y = h.Value();
                    changed = true;
                }
            if (changed)
                node.SetSize(size);
        }

        if (style.Has(StringView(u8"enabled")))
            if (Optional<bool> e = ParseBool(style.Get(StringView(u8"enabled"))); e.HasValue())
                node.SetEnabled(e.Value());

        if (style.Has(StringView(u8"visibility")))
        {
            const StringView v = style.Get(StringView(u8"visibility"));
            if (v == StringView(u8"hidden"))
                node.SetVisible(false);
            else if (v == StringView(u8"visible"))
                node.SetVisible(true);
        }

        if (style.Has(StringView(u8"margin")))
            if (UIWidget* widget = foundation::Cast<UIWidget>(&node))
                if (Optional<Thickness> t =
                        ResolveThickness(style.Get(StringView(u8"margin")), lengths);
                    t.HasValue())
                    widget->SetMargin(t.Value());

        // Text color for text-bearing widgets (Label/TextField/ComboBox/... via the virtual).
        if (style.Has(StringView(u8"color")))
            if (Optional<Color> c = ParseColor(style.Get(StringView(u8"color"))); c.HasValue())
                node.SetThemeTextColor(c.Value());

        // text-align (horizontal). justify falls back to left.
        if (style.Has(StringView(u8"text-align")))
        {
            const StringView v = style.Get(StringView(u8"text-align"));
            if (v == StringView(u8"left") || v == StringView(u8"justify"))
                node.SetThemeTextAlign(TextHAlign::Left);
            else if (v == StringView(u8"center") || v == StringView(u8"centre"))
                node.SetThemeTextAlign(TextHAlign::Center);
            else if (v == StringView(u8"right"))
                node.SetThemeTextAlign(TextHAlign::Right);
        }

        // vertical-align (top / middle / bottom).
        if (style.Has(StringView(u8"vertical-align")))
        {
            const StringView v = style.Get(StringView(u8"vertical-align"));
            if (v == StringView(u8"top"))
                node.SetThemeTextAlignV(TextVAlign::Top);
            else if (v == StringView(u8"middle") || v == StringView(u8"center"))
                node.SetThemeTextAlignV(TextVAlign::Middle);
            else if (v == StringView(u8"bottom"))
                node.SetThemeTextAlignV(TextVAlign::Bottom);
        }

        // background-image: url(path) -> load the image via the resource provider and wrap it
        // in an ImageDrawable (the provider returns the raw asset, the GUI wraps it).
        if (resources != nullptr && style.Has(StringView(u8"background-image")))
        {
            const StringView path = ParseUrl(style.Get(StringView(u8"background-image")));
            if (path.Size() != 0)
                if (const image::ImageData* img = resources->LoadImage(path))
                    node.SetBackground(foundation::MakeRef<ImageDrawable>(foundation::DefaultAllocator(), img));
        }

        // border-radius: round the background rectangle; the border (below) reuses the radii.
        vg::CornerRadii radii{};
        const bool hasRadius = style.Has(StringView(u8"border-radius"));
        if (hasRadius)
            if (Optional<vg::CornerRadii> r =
                    ParseCornerRadii(style.Get(StringView(u8"border-radius")));
                r.HasValue())
            {
                radii = r.Value();
                if (RectangleDrawable* bg = foundation::Cast<RectangleDrawable>(node.GetBackground()))
                    bg->SetCornerRadii(radii);
            }

        // border: shorthand plus border-width / border-color overrides -> a BorderDrawable set
        // as the node's foreground (drawn over the content, rounded to match border-radius).
        {
            Optional<f32> borderWidth;
            Optional<Color> borderColor;
            if (style.Has(StringView(u8"border")))
                if (Optional<BorderShorthand> b = ParseBorder(style.Get(StringView(u8"border")));
                    b.HasValue())
                {
                    borderWidth = b.Value().Width;
                    borderColor = b.Value().LineColor;
                }
            if (style.Has(StringView(u8"border-width")))
                if (Optional<f32> w =
                        ResolveLength(style.Get(StringView(u8"border-width")), lengths);
                    w.HasValue())
                    borderWidth = w.Value();
            if (style.Has(StringView(u8"border-color")))
                if (Optional<Color> c = ParseColor(style.Get(StringView(u8"border-color")));
                    c.HasValue())
                    borderColor = c.Value();

            if (borderWidth.HasValue() || borderColor.HasValue())
            {
                auto border = foundation::MakeRef<BorderDrawable>(
                    foundation::DefaultAllocator(), borderColor.ValueOr(Color{0.0f, 0.0f, 0.0f, 1.0f}),
                    borderWidth.ValueOr(1.0f));
                if (hasRadius)
                    border->SetCornerRadii(radii);
                node.SetForeground(foundation::Move(border));
            }
        }

        // font-family/-size -> resolve through the font service (whatever backs it - VFS, the
        // TrueType service, ...; the GUI stays agnostic).
        if (fontService != nullptr && style.Has(StringView(u8"font-family")))
        {
            const StringView family =
                ParseUrl(style.Get(StringView(u8"font-family"))); // strips quotes
            const f32 size =
                ResolveLength(style.Get(StringView(u8"font-size"), StringView(u8"16")), lengths)
                    .ValueOr(16.0f);
            if (family.Size() != 0 && size > 0.0f)
                if (fonts::CachedFont* font = fontService->GetFont(family, size))
                    node.SetThemeFont(font);
        }
    }

    // Apply a resolved pseudo-element style to a widget part: its background-color becomes the
    // part's color (slider::fill, window::title, scrollbar::thumb, ...).
    inline void ApplyPartStyle(UINode& node, foundation::StringView part, const ResolvedStyle& style)
    {
        if (style.Has(foundation::StringView(u8"background-color")))
            if (Optional<Color> c = ParseColor(style.Get(foundation::StringView(u8"background-color")));
                c.HasValue())
                node.SetThemePartColor(part, c.Value());
    }
}
