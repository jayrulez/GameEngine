// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG.SVG::SVG - :loader partition.
//
// SVGLoader: parses a subset of SVG (path, rect, circle, ellipse, line, polygon,
// polyline, g, text) from a string into an SVGDocument. Has its own minimal XML
// tag and attribute scanner (no XML library dependency).
//
// The paint attributes (fill, stroke, stroke-width, stroke-linecap, stroke-linejoin) are
// inherited as SVG's are: from the <svg> element and each <g> down to the shapes in them, a
// shape's own attribute winning. An icon set written once on its root (fill="none" stroke=...)
// draws as written.

module;
#include "Core/Prelude.h"

export module foundation.vg.svg:loader;

import foundation.core;
import foundation.vg;
import :types;
import :parsers;

using namespace foundation::core;

export namespace foundation::vg::svg
{
    /// Loads SVG documents from string content (icon-systems subset).
    class SVGLoader
    {
    public:
        static Result<SVGDocument> Load(StringView svgContent)
        {
            usize pos = 0;
            SVGDocument doc;

            if (!FindTag(svgContent, pos, u8"svg"))
                return Err(ErrorCode::InvalidArgument);

            HashMap<String, String> svgAttrs;
            ParseAttributes(svgContent, pos, svgAttrs);
            const PaintStyle rootPaint = Cascade(PaintStyle{}, svgAttrs);

            if (const String* wStr = svgAttrs.Find(String(u8"width")))
                if (const Optional<f32> w = ParseFloatValue(wStr->AsView()))
                    doc.width = w.Value();
            if (const String* hStr = svgAttrs.Find(String(u8"height")))
                if (const Optional<f32> h = ParseFloatValue(hStr->AsView()))
                    doc.height = h.Value();

            // viewBox if no explicit width/height.
            if (doc.width == 0.0f && doc.height == 0.0f)
            {
                if (const String* viewBox = svgAttrs.Find(String(u8"viewBox")))
                {
                    const StringView vb = viewBox->AsView();
                    usize vbPos = 0;
                    f32 tmp;
                    (void)ParseFloatFromView(vb, vbPos, tmp); // min-x
                    (void)ParseFloatFromView(vb, vbPos, tmp); // min-y
                    if (ParseFloatFromView(vb, vbPos, tmp))
                        doc.width = tmp;
                    if (ParseFloatFromView(vb, vbPos, tmp))
                        doc.height = tmp;
                }
            }

            if (!ParseChildren(svgContent, pos, doc.elements, doc, rootPaint).IsOk())
                return Err(ErrorCode::InvalidArgument);

            return doc;
        }

    private:
        /// The paint a shape inherits from the elements around it (SVG's initial values at the top).
        struct PaintStyle
        {
            Optional<Color> fillColor = Color::Black; ///< empty: fill="none"
            String fillGradientId;
            Optional<Color> strokeColor; ///< empty: no stroke
            f32 strokeWidth = 1.0f;
            foundation::vg::VGLineCap cap = foundation::vg::VGLineCap::Butt;
            foundation::vg::VGLineJoin join = foundation::vg::VGLineJoin::Miter;
        };

        /// `inherited` with the paint attributes `attrs` sets over it.
        [[nodiscard]] static PaintStyle Cascade(const PaintStyle& inherited, const HashMap<String, String>& attrs)
        {
            using detail::EqualsIgnoreCase;
            PaintStyle style = inherited;
            if (const String* fillStr = attrs.Find(String(u8"fill")))
            {
                if (EqualsIgnoreCase(fillStr->AsView(), u8"none"))
                {
                    style.fillColor = {};
                    style.fillGradientId = String();
                }
                else if (const Optional<String> id = ParseUrlReference(fillStr->AsView()))
                {
                    style.fillGradientId = id.Value();
                    style.fillColor = Color::Black; // fallback if the id never resolves
                }
                else if (Result<Color> c = SVGColorParser::Parse(fillStr->AsView()); c.HasValue())
                {
                    style.fillColor = c.Value();
                    style.fillGradientId = String();
                }
            }
            if (const String* strokeStr = attrs.Find(String(u8"stroke")))
            {
                if (EqualsIgnoreCase(strokeStr->AsView(), u8"none"))
                    style.strokeColor = {};
                else if (Result<Color> c = SVGColorParser::Parse(strokeStr->AsView()); c.HasValue())
                    style.strokeColor = c.Value();
            }
            if (const String* swStr = attrs.Find(String(u8"stroke-width")))
                if (const Optional<f32> v = ParseFloatValue(swStr->AsView()))
                    style.strokeWidth = v.Value();
            if (const String* capStr = attrs.Find(String(u8"stroke-linecap")))
            {
                if (EqualsIgnoreCase(capStr->AsView(), u8"round"))
                    style.cap = foundation::vg::VGLineCap::Round;
                else if (EqualsIgnoreCase(capStr->AsView(), u8"square"))
                    style.cap = foundation::vg::VGLineCap::Square;
                else if (EqualsIgnoreCase(capStr->AsView(), u8"butt"))
                    style.cap = foundation::vg::VGLineCap::Butt;
            }
            if (const String* joinStr = attrs.Find(String(u8"stroke-linejoin")))
            {
                if (EqualsIgnoreCase(joinStr->AsView(), u8"round"))
                    style.join = foundation::vg::VGLineJoin::Round;
                else if (EqualsIgnoreCase(joinStr->AsView(), u8"bevel"))
                    style.join = foundation::vg::VGLineJoin::Bevel;
                else if (EqualsIgnoreCase(joinStr->AsView(), u8"miter"))
                    style.join = foundation::vg::VGLineJoin::Miter;
            }
            return style;
        }

        static Status ParseChildren(StringView content, usize& pos, Array<SVGElement>& elements,
                                    SVGDocument& doc, const PaintStyle& inherited)
        {
            while (pos < content.Size())
            {
                SkipWhitespace(content, pos);
                if (pos >= content.Size())
                    break;

                if (pos + 1 < content.Size() && content[pos] == u8'<' && content[pos + 1] == u8'/')
                    break; // closing tag

                if (content[pos] != u8'<')
                {
                    ++pos;
                    continue;
                }

                const usize savedPos = pos;
                if (!TryParseElement(content, pos, elements, doc, inherited).IsOk())
                {
                    pos = savedPos;
                    SkipTag(content, pos);
                }
            }
            return ErrorCode::Ok;
        }

        static Status TryParseElement(StringView content, usize& pos, Array<SVGElement>& elements,
                                      SVGDocument& doc, const PaintStyle& inherited)
        {
            if (pos >= content.Size() || content[pos] != u8'<')
                return ErrorCode::InvalidArgument;

            ++pos; // skip '<'
            SkipWhitespace(content, pos);

            const usize tagStart = pos;
            while (pos < content.Size() && content[pos] != u8' ' && content[pos] != u8'>' &&
                   content[pos] != u8'/' && content[pos] != u8'\t' && content[pos] != u8'\n')
                ++pos;
            const String tagName(content.SubStr(tagStart, pos - tagStart));

            HashMap<String, String> attrs;
            ParseAttributes(content, pos, attrs);

            const bool isSelfClosing =
                (pos > 0 && pos <= content.Size() && content[pos - 1] == u8'/');

            if (pos < content.Size() && content[pos] == u8'>')
                ++pos;
            const PaintStyle paint = Cascade(inherited, attrs);

            using detail::EqualsIgnoreCase;
            const StringView tag = tagName.AsView();
            SVGElement element;
            bool recognized = true;
            bool emit = true; // defs and gradients register on the document, not the tree

            if (EqualsIgnoreCase(tag, u8"path"))
            {
                element.type = SVGElementType::Path;
                if (const String* d = attrs.Find(String(u8"d")))
                {
                    PathBuilder pb;
                    if (SVGPathParser::Parse(d->AsView(), pb).IsOk())
                        element.path = pb.ToPath();
                }
            }
            else if (EqualsIgnoreCase(tag, u8"rect"))
            {
                element.type = SVGElementType::Rectangle;
                f32 x = Attr(attrs, u8"x"), y = Attr(attrs, u8"y"), w = Attr(attrs, u8"width"),
                    h = Attr(attrs, u8"height");
                f32 rx = Attr(attrs, u8"rx"), ry = Attr(attrs, u8"ry");
                if (ry == 0.0f)
                    ry = rx;
                if (rx == 0.0f)
                    rx = ry;

                PathBuilder pb;
                if (rx > 0.0f || ry > 0.0f)
                    ShapeBuilder::BuildRoundedRect(Rectangle{x, y, w, h}, CornerRadii(rx), pb);
                else
                {
                    pb.MoveTo(x, y);
                    pb.LineTo(x + w, y);
                    pb.LineTo(x + w, y + h);
                    pb.LineTo(x, y + h);
                    pb.Close();
                }
                element.path = pb.ToPath();
            }
            else if (EqualsIgnoreCase(tag, u8"circle"))
            {
                element.type = SVGElementType::Circle;
                PathBuilder pb;
                ShapeBuilder::BuildCircle(Float2{Attr(attrs, u8"cx"), Attr(attrs, u8"cy")},
                                          Attr(attrs, u8"r"), pb);
                element.path = pb.ToPath();
            }
            else if (EqualsIgnoreCase(tag, u8"ellipse"))
            {
                element.type = SVGElementType::Ellipse;
                PathBuilder pb;
                ShapeBuilder::BuildEllipse(Float2{Attr(attrs, u8"cx"), Attr(attrs, u8"cy")},
                                           Attr(attrs, u8"rx"), Attr(attrs, u8"ry"), pb);
                element.path = pb.ToPath();
            }
            else if (EqualsIgnoreCase(tag, u8"line"))
            {
                element.type = SVGElementType::Line;
                PathBuilder pb;
                pb.MoveTo(Attr(attrs, u8"x1"), Attr(attrs, u8"y1"));
                pb.LineTo(Attr(attrs, u8"x2"), Attr(attrs, u8"y2"));
                element.path = pb.ToPath();
            }
            else if (EqualsIgnoreCase(tag, u8"polygon") || EqualsIgnoreCase(tag, u8"polyline"))
            {
                const bool isPolygon = EqualsIgnoreCase(tag, u8"polygon");
                element.type = isPolygon ? SVGElementType::Polygon : SVGElementType::Polyline;
                if (const String* pointsStr = attrs.Find(String(u8"points")))
                {
                    PathBuilder pb;
                    const StringView pv = pointsStr->AsView();
                    usize pPos = 0;
                    bool first = true;
                    f32 x, y;
                    while (pPos < pv.Size())
                    {
                        if (ParseFloatFromView(pv, pPos, x) && ParseFloatFromView(pv, pPos, y))
                        {
                            if (first)
                            {
                                pb.MoveTo(x, y);
                                first = false;
                            }
                            else
                                pb.LineTo(x, y);
                        }
                        else
                            break;
                    }
                    if (isPolygon)
                        pb.Close();
                    element.path = pb.ToPath();
                }
            }
            else if (EqualsIgnoreCase(tag, u8"g"))
            {
                element.type = SVGElementType::Group;
                if (!isSelfClosing)
                    ParseChildren(content, pos, element.children, doc, paint);
                SkipClosingTag(content, pos, u8"g");
            }
            else if (EqualsIgnoreCase(tag, u8"defs"))
            {
                // A definitions container: parse children so gradients register on the
                // document, then drop everything (defs content is never rendered).
                emit = false;
                if (!isSelfClosing)
                {
                    Array<SVGElement> discarded;
                    ParseChildren(content, pos, discarded, doc, PaintStyle{});
                }
                SkipClosingTag(content, pos, u8"defs");
            }
            else if (EqualsIgnoreCase(tag, u8"linearGradient") ||
                     EqualsIgnoreCase(tag, u8"radialGradient"))
            {
                emit = false;
                SVGGradient grad;
                grad.radial = EqualsIgnoreCase(tag, u8"radialGradient");
                if (grad.radial)
                {
                    grad.cx = CoordAttr(attrs, u8"cx", 0.5f);
                    grad.cy = CoordAttr(attrs, u8"cy", 0.5f);
                    grad.r = CoordAttr(attrs, u8"r", 0.5f);
                }
                else
                {
                    grad.x1 = CoordAttr(attrs, u8"x1", 0.0f);
                    grad.y1 = CoordAttr(attrs, u8"y1", 0.0f);
                    grad.x2 = CoordAttr(attrs, u8"x2", 1.0f);
                    grad.y2 = CoordAttr(attrs, u8"y2", 0.0f);
                }
                if (const String* units = attrs.Find(String(u8"gradientUnits")))
                    grad.userSpace = EqualsIgnoreCase(units->AsView(), u8"userSpaceOnUse");
                if (const String* spread = attrs.Find(String(u8"spreadMethod")))
                {
                    if (EqualsIgnoreCase(spread->AsView(), u8"repeat"))
                        grad.spread = foundation::vg::VGGradientSpread::Repeat;
                    else if (EqualsIgnoreCase(spread->AsView(), u8"reflect"))
                        grad.spread = foundation::vg::VGGradientSpread::Reflect;
                }
                if (!isSelfClosing)
                    ParseGradientStops(content, pos, grad);
                if (const String* id = attrs.Find(String(u8"id")))
                    doc.gradients.InsertOrAssign(*id, Move(grad));
            }
            else if (EqualsIgnoreCase(tag, u8"text"))
            {
                element.type = SVGElementType::Text;
                element.textX = Attr(attrs, u8"x");
                element.textY = Attr(attrs, u8"y");
                if (const String* fs = attrs.Find(String(u8"font-size")))
                    if (const Optional<f32> v = ParseFloatValue(fs->AsView()))
                        element.fontSize = v.Value();
                if (const String* ta = attrs.Find(String(u8"text-anchor")))
                {
                    if (EqualsIgnoreCase(ta->AsView(), u8"middle"))
                        element.textAnchor = SVGTextAnchor::Middle;
                    else if (EqualsIgnoreCase(ta->AsView(), u8"end"))
                        element.textAnchor = SVGTextAnchor::End;
                }
                if (const String* fw = attrs.Find(String(u8"font-weight")))
                    element.fontBold = EqualsIgnoreCase(fw->AsView(), u8"bold");

                if (!isSelfClosing)
                {
                    const usize textStart = pos;
                    while (pos + 1 < content.Size())
                    {
                        if (content[pos] == u8'<' && content[pos + 1] == u8'/')
                            break;
                        ++pos;
                    }
                    const StringView innerText = content.SubStr(textStart, pos - textStart);
                    AppendTrimmedText(innerText, element.textContent);
                    SkipClosingTag(content, pos, u8"text");
                }
            }
            else
            {
                recognized = false;
            }

            if (!recognized)
                return ErrorCode::InvalidArgument;

            // The paint: inherited, with the element's own attributes over it.
            element.fillColor = paint.fillColor;
            element.fillGradientId = paint.fillGradientId;
            element.strokeColor = paint.strokeColor;
            element.strokeWidth = paint.strokeWidth;
            element.strokeCap = paint.cap;
            element.strokeJoin = paint.join;

            if (const String* opStr = attrs.Find(String(u8"opacity")))
                if (const Optional<f32> v = ParseFloatValue(opStr->AsView()))
                    element.opacity = v.Value();

            if (const String* trStr = attrs.Find(String(u8"transform")))
                if (Result<Float4x4> m = SVGTransformParser::Parse(trStr->AsView()); m.HasValue())
                    element.transform = m.Value();

            if (emit)
                elements.PushBack(Move(element));
            return ErrorCode::Ok;
        }

        /// Extract the id from a `url(#id)` paint reference (empty when not one).
        [[nodiscard]] static Optional<String> ParseUrlReference(StringView v)
        {
            usize i = 0;
            SkipWhitespace(v, i);
            if (i + 4 > v.Size() || v[i] != u8'u' || v[i + 1] != u8'r' || v[i + 2] != u8'l' ||
                v[i + 3] != u8'(')
                return {};
            i += 4;
            SkipWhitespace(v, i);
            if (i >= v.Size() || v[i] != u8'#')
                return {};
            ++i;
            const usize start = i;
            while (i < v.Size() && v[i] != u8')' && v[i] != u8' ')
                ++i;
            if (i == start)
                return {};
            return String(v.SubStr(start, i - start));
        }

        /// Parse <stop> children until the gradient's closing tag. Handles the attribute
        /// form (offset, stop-color, stop-opacity) and the common style="stop-color:
        /// ...;stop-opacity:..." form Inkscape and friends export.
        static void ParseGradientStops(StringView content, usize& pos, SVGGradient& grad)
        {
            while (pos < content.Size())
            {
                SkipWhitespace(content, pos);
                if (pos + 1 < content.Size() && content[pos] == u8'<' &&
                    content[pos + 1] == u8'/')
                    break; // the gradient's closing tag
                if (pos >= content.Size() || content[pos] != u8'<')
                {
                    ++pos;
                    continue;
                }
                const usize savedPos = pos;
                ++pos;
                SkipWhitespace(content, pos);
                const usize tagStart = pos;
                while (pos < content.Size() && content[pos] != u8' ' && content[pos] != u8'>' &&
                       content[pos] != u8'/' && content[pos] != u8'\t' && content[pos] != u8'\n')
                    ++pos;
                const String stopTag(content.SubStr(tagStart, pos - tagStart));
                if (!detail::EqualsIgnoreCase(stopTag.AsView(), u8"stop"))
                {
                    pos = savedPos;
                    SkipTag(content, pos);
                    continue;
                }
                HashMap<String, String> stopAttrs;
                ParseAttributes(content, pos, stopAttrs);
                if (pos < content.Size() && content[pos] == u8'>')
                    ++pos;

                f32 offset = 0.0f;
                if (const String* o = stopAttrs.Find(String(u8"offset")))
                    offset = ParsePercentAware(o->AsView(), 0.0f);
                Color color = Color::Black;
                f32 alpha = 1.0f;
                if (const String* c = stopAttrs.Find(String(u8"stop-color")))
                    if (Result<Color> parsed = SVGColorParser::Parse(c->AsView());
                        parsed.HasValue())
                        color = parsed.Value();
                if (const String* a = stopAttrs.Find(String(u8"stop-opacity")))
                    if (const Optional<f32> v = ParseFloatValue(a->AsView()))
                        alpha = v.Value();
                if (const String* style = stopAttrs.Find(String(u8"style")))
                    ParseStopStyle(style->AsView(), color, alpha);
                color.a *= alpha;
                grad.stops.PushBack(foundation::vg::GradientStop(offset, color));
            }
            SkipClosingTag(content, pos, u8"gradient");
        }

        /// Minimal `style` splitter for the two stop properties.
        static void ParseStopStyle(StringView style, Color& color, f32& alpha)
        {
            usize i = 0;
            while (i < style.Size())
            {
                usize end = i;
                while (end < style.Size() && style[end] != u8';')
                    ++end;
                StringView decl = style.SubStr(i, end - i);
                usize colon = 0;
                while (colon < decl.Size() && decl[colon] != u8':')
                    ++colon;
                if (colon < decl.Size())
                {
                    StringView name = Trim(decl.SubStr(0, colon));
                    StringView value = Trim(decl.SubStr(colon + 1, decl.Size() - colon - 1));
                    if (detail::EqualsIgnoreCase(name, u8"stop-color"))
                    {
                        if (Result<Color> parsed = SVGColorParser::Parse(value); parsed.HasValue())
                            color = parsed.Value();
                    }
                    else if (detail::EqualsIgnoreCase(name, u8"stop-opacity"))
                    {
                        if (const Optional<f32> v = ParseFloatValue(value))
                            alpha = v.Value();
                    }
                }
                i = end + 1;
            }
        }

        [[nodiscard]] static StringView Trim(StringView s)
        {
            usize start = 0, end = s.Size();
            while (start < end && (s[start] == u8' ' || s[start] == u8'\t'))
                ++start;
            while (end > start && (s[end - 1] == u8' ' || s[end - 1] == u8'\t'))
                --end;
            return s.SubStr(start, end - start);
        }

        /// A gradient coordinate or offset: "50%" is 0.5, anything else the plain number.
        [[nodiscard]] static f32 ParsePercentAware(StringView s, f32 fallback)
        {
            const Optional<f32> v = ParseFloatValue(s);
            if (!v)
                return fallback;
            StringView trimmed = Trim(s);
            const bool percent = !trimmed.IsEmpty() && trimmed[trimmed.Size() - 1] == u8'%';
            return percent ? v.Value() / 100.0f : v.Value();
        }

        [[nodiscard]] static f32 CoordAttr(const HashMap<String, String>& attrs, StringView name,
                                           f32 fallback)
        {
            if (const String* v = attrs.Find(String(name)))
                return ParsePercentAware(v->AsView(), fallback);
            return fallback;
        }

        // --- attribute lookup helper: parse an attr value as a float, default 0 ---
        [[nodiscard]] static f32 Attr(const HashMap<String, String>& attrs, StringView name)
        {
            if (const String* v = attrs.Find(String(name)))
                if (const Optional<f32> f = ParseFloatValue(v->AsView()))
                    return f.Value();
            return 0.0f;
        }

        // --- XML helpers ---

        static bool FindTag(StringView content, usize& pos, StringView tagName)
        {
            while (pos < content.Size())
            {
                if (content[pos] == u8'<')
                {
                    const usize start = pos + 1;
                    usize end = start;
                    while (end < content.Size() && content[end] != u8' ' && content[end] != u8'>' &&
                           content[end] != u8'/')
                        ++end;
                    if (detail::EqualsIgnoreCase(content.SubStr(start, end - start), tagName))
                    {
                        pos = end;
                        return true;
                    }
                }
                ++pos;
            }
            return false;
        }

        static void ParseAttributes(StringView content, usize& pos, HashMap<String, String>& attrs)
        {
            while (pos < content.Size())
            {
                SkipWhitespace(content, pos);

                if (pos >= content.Size() || content[pos] == u8'>' || content[pos] == u8'/')
                {
                    if (pos < content.Size() && content[pos] == u8'/')
                    {
                        ++pos;
                        if (pos < content.Size() && content[pos] == u8'>')
                            ++pos;
                    }
                    else if (pos < content.Size() && content[pos] == u8'>')
                        ++pos;
                    break;
                }

                const usize nameStart = pos;
                while (pos < content.Size() && content[pos] != u8'=' && content[pos] != u8' ' &&
                       content[pos] != u8'>')
                    ++pos;
                String attrName(content.SubStr(nameStart, pos - nameStart));

                SkipWhitespace(content, pos);
                if (pos < content.Size() && content[pos] == u8'=')
                {
                    ++pos;
                    SkipWhitespace(content, pos);
                    if (pos < content.Size() && (content[pos] == u8'"' || content[pos] == u8'\''))
                    {
                        const utf8char quote = content[pos];
                        ++pos;
                        const usize valStart = pos;
                        while (pos < content.Size() && content[pos] != quote)
                            ++pos;
                        String attrValue(content.SubStr(valStart, pos - valStart));
                        if (pos < content.Size())
                            ++pos; // closing quote
                        attrs.InsertOrAssign(Move(attrName), Move(attrValue));
                    }
                }
            }
        }

        // Step over a comment, CDATA or processing instruction as one unit when `pos` sits on
        // one; returns false when it does not. A '<' or '>' inside one must not touch the
        // depth count below.
        static bool SkipMarkupUnit(StringView content, usize& pos)
        {
            const auto startsWith = [&](StringView prefix)
            {
                return pos + prefix.Size() <= content.Size() &&
                       content.SubStr(pos, prefix.Size()) == prefix;
            };
            const auto skipPast = [&](StringView terminator)
            {
                while (pos < content.Size())
                {
                    if (pos + terminator.Size() <= content.Size() &&
                        content.SubStr(pos, terminator.Size()) == terminator)
                    {
                        pos += terminator.Size();
                        return;
                    }
                    ++pos;
                }
            };
            if (startsWith(u8"<!--"))
            {
                skipPast(u8"-->");
                return true;
            }
            if (startsWith(u8"<![CDATA["))
            {
                skipPast(u8"]]>");
                return true;
            }
            if (startsWith(u8"<?"))
            {
                skipPast(u8"?>");
                return true;
            }
            return false;
        }

        // Advance past the '>' that closes the tag starting at `pos`; quotes are honoured so a
        // '>' inside an attribute value cannot end it. Returns true for a self-closing tag.
        static bool SkipOneTag(StringView content, usize& pos)
        {
            bool selfClosing = false;
            utf8char quote = 0;
            while (pos < content.Size())
            {
                const utf8char c = content[pos];
                if (quote != 0)
                {
                    if (c == quote)
                        quote = 0;
                }
                else if (c == u8'"' || c == u8'\'')
                {
                    quote = c;
                }
                else if (c == u8'>')
                {
                    selfClosing = pos > 0 && content[pos - 1] == u8'/';
                    ++pos;
                    return selfClosing;
                }
                ++pos;
            }
            return selfClosing;
        }

        // Skip an unrecognised element whole: its opening tag, every nested child (by depth, so
        // an inner element cannot end it early), and its closing tag; a self-closing tag ends at
        // once. Skipping only the opening tag would leave an unrecognised container's children at
        // the current level and let its closing tag end that level, dropping every sibling after a
        // <metadata>, <style> or <desc>, which is most of an editor-exported file.
        static void SkipTag(StringView content, usize& pos)
        {
            if (SkipMarkupUnit(content, pos))
                return;
            if (pos >= content.Size() || content[pos] != u8'<')
            {
                ++pos;
                return;
            }
            if (pos + 1 < content.Size() && content[pos + 1] == u8'/')
            {
                (void)SkipOneTag(content, pos); // a stray closing tag: step over it alone
                return;
            }
            if (SkipOneTag(content, pos))
                return; // self-closing
            usize depth = 1;
            while (pos < content.Size() && depth > 0)
            {
                if (content[pos] != u8'<')
                {
                    ++pos;
                    continue;
                }
                if (SkipMarkupUnit(content, pos))
                    continue;
                if (pos + 1 < content.Size() && content[pos + 1] == u8'/')
                {
                    (void)SkipOneTag(content, pos);
                    --depth;
                    continue;
                }
                if (!SkipOneTag(content, pos))
                    ++depth; // an opening tag with children of its own
            }
        }

        static void SkipClosingTag(StringView content, usize& pos, StringView /*tagName*/)
        {
            SkipWhitespace(content, pos);
            if (pos + 1 < content.Size() && content[pos] == u8'<' && content[pos + 1] == u8'/')
            {
                pos += 2;
                while (pos < content.Size() && content[pos] != u8'>')
                    ++pos;
                if (pos < content.Size())
                    ++pos;
            }
        }

        static void SkipWhitespace(StringView s, usize& pos)
        {
            while (pos < s.Size() &&
                   (s[pos] == u8' ' || s[pos] == u8'\t' || s[pos] == u8'\n' || s[pos] == u8'\r'))
                ++pos;
        }

        // Strips a trailing unit (e.g. "px") and parses the leading number.
        [[nodiscard]] static Optional<f32> ParseFloatValue(StringView s)
        {
            usize end = 0;
            while (end < s.Size() && (detail::IsDigit(s[end]) || s[end] == u8'.' ||
                                      s[end] == u8'-' || s[end] == u8'+'))
                ++end;
            if (end == 0)
                return {};
            return detail::StrToF32(s.SubStr(0, end));
        }

        // Parses one float from a list (points or viewBox), advancing pos.
        [[nodiscard]] static bool ParseFloatFromView(StringView s, usize& pos, f32& out)
        {
            while (pos < s.Size() &&
                   (s[pos] == u8' ' || s[pos] == u8'\t' || s[pos] == u8',' || s[pos] == u8'\n'))
                ++pos;
            if (pos >= s.Size())
                return false;
            const usize start = pos;
            if (pos < s.Size() && (s[pos] == u8'-' || s[pos] == u8'+'))
                ++pos;
            while (pos < s.Size() && detail::IsDigit(s[pos]))
                ++pos;
            if (pos < s.Size() && s[pos] == u8'.')
            {
                ++pos;
                while (pos < s.Size() && detail::IsDigit(s[pos]))
                    ++pos;
            }
            if (pos == start)
                return false;
            const Optional<f32> v = detail::StrToF32(s.SubStr(start, pos - start));
            if (!v)
                return false;
            out = v.Value();
            return true;
        }

        // Append inner text, collapsing runs of whitespace to single spaces, then trim.
        static void AppendTrimmedText(StringView innerText, String& out)
        {
            for (usize i = 0; i < innerText.Size(); ++i)
            {
                const utf8char c = innerText[i];
                if (c == u8'\n' || c == u8'\r' || c == u8'\t')
                {
                    if (!out.AsView().IsEmpty() && out.AsView()[out.AsView().Size() - 1] != u8' ')
                        out.Append(u8' ');
                }
                else
                {
                    out.Append(c);
                }
            }
            // Trim leading and trailing spaces.
            StringView v = out.AsView();
            usize start = 0, end = v.Size();
            while (start < end && v[start] == u8' ')
                ++start;
            while (end > start && v[end - 1] == u8' ')
                --end;
            if (start != 0 || end != v.Size())
            {
                String trimmed(v.SubStr(start, end - start));
                out = Move(trimmed);
            }
        }
    };
}
