// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :text_runs partition
//
// Rich text as runs: styled spans of text and inline icons (F2 of Specs/vg-and-gui-lab.md), and
// the small markup that writes them, the same from SML and from script:
//
//   Your mark wears [color=#d04060][b]a rose gown[/b][/color] [icon=Icons/rose.png] [link=hint]?[/link]
//
// Tags: [b] bold, [i] italic, [u] underline, [color=#rrggbb | #rrggbbaa | name],
// [size=<dp>], [font=<family>], [link=<id>] (a span the control reports when clicked), each
// closed by [/name] or [/] (the innermost); [icon=<image path>] stands alone. "[[" is a literal
// "[". A tag that is malformed or unknown, or a close that matches nothing, is kept as text and
// reported in the diagnostics, never dropped.

module;
#include "Core/Prelude.h"

export module foundation.ui:text_runs;

import foundation.core;
import :style_value_parser;

using namespace foundation::core;

export namespace foundation::ui
{
    /// One styled span of a rich text, or an inline icon. A style field left unset takes the
    /// control's (its font family, size and text colour).
    struct TextRun
    {
        String text;
        String icon; ///< An image path: an inline icon sitting on the baseline (text is empty).
        Optional<String> family;
        Optional<f32> size;
        Optional<Color> color;
        bool bold = false;
        bool italic = false;
        bool underline = false;
        String link; ///< A link id the control reports when the run is clicked; empty for none.

        [[nodiscard]] bool IsIcon() const noexcept { return !icon.IsEmpty(); }
        /// Same style (and link), so two runs may merge.
        [[nodiscard]] bool SameStyle(const TextRun& other) const
        {
            return icon.IsEmpty() && other.icon.IsEmpty() && family == other.family && size == other.size &&
                   color == other.color && bold == other.bold && italic == other.italic &&
                   underline == other.underline && link == other.link;
        }
    };

    /// A markup problem: where, in bytes, and what. The offending text stays in the runs.
    struct TextMarkupDiagnostic
    {
        usize offset = 0;
        String message;
    };

    /// A rich text: its runs in order, and what parsing its markup reported.
    struct TextDocument
    {
        Array<TextRun> runs;
        Array<TextMarkupDiagnostic> diagnostics;

        /// The text with the markup taken out (an icon is U+FFFC, the object replacement character).
        [[nodiscard]] String PlainText() const
        {
            String out;
            for (const TextRun& run : runs)
            {
                if (run.IsIcon())
                    out.Append(StringView(u8"￼"));
                else
                    out.Append(run.text.AsView());
            }
            return out;
        }
    };

    /// Resolves a colour name in markup ([color=gold]) beyond the named CSS colours, as a palette
    /// would; null leaves only hex and the named colours.
    using TextMarkupColorResolver = Function<Optional<Color>(StringView)>;

    /// Parse markup into runs.
    [[nodiscard]] TextDocument ParseTextMarkup(StringView markup, const TextMarkupColorResolver* resolver = nullptr);

    namespace detail
    {
        struct MarkupFrame
        {
            String tag; // the tag that opened it (b, i, u, color, size, font, link)
            TextRun style;
        };

        class TextMarkupParser
        {
        public:
            TextMarkupParser(StringView markup, const TextMarkupColorResolver* resolver)
                : m_markup(markup), m_resolver(resolver)
            {
            }

            TextDocument Parse()
            {
                usize i = 0;
                while (i < m_markup.Size())
                {
                    const utf8char c = m_markup[i];
                    if (c != u8'[')
                    {
                        m_text.PushBack(c);
                        ++i;
                        continue;
                    }
                    if (i + 1 < m_markup.Size() && m_markup[i + 1] == u8'[')
                    {
                        m_text.PushBack(u8'['); // "[[": a literal bracket
                        i += 2;
                        continue;
                    }
                    usize close = i + 1;
                    while (close < m_markup.Size() && m_markup[close] != u8']' && m_markup[close] != u8'[')
                        ++close;
                    if (close >= m_markup.Size() || m_markup[close] != u8']')
                    {
                        Diagnose(i, u8"an unclosed '[' (write \"[[\" for a literal bracket)");
                        m_text.PushBack(c);
                        ++i;
                        continue;
                    }
                    const StringView body = m_markup.SubStr(i + 1, close - i - 1);
                    if (!Apply(body, i))
                        m_text.Append(m_markup.SubStr(i, close - i + 1)); // kept as text
                    i = close + 1;
                }
                Flush();
                return Move(m_document);
            }

        private:
            // Apply one tag's body (between the brackets); false when it is not a tag we take.
            bool Apply(StringView body, usize at)
            {
                if (!body.IsEmpty() && body[0] == u8'/')
                    return Close(Trimmed(body.SubStr(1, body.Size() - 1)), at);
                StringView name = body;
                StringView value;
                bool hasValue = false;
                for (usize k = 0; k < body.Size(); ++k)
                {
                    if (body[k] == u8'=')
                    {
                        name = body.SubStr(0, k);
                        value = Trimmed(body.SubStr(k + 1, body.Size() - k - 1));
                        hasValue = true;
                        break;
                    }
                }
                name = Trimmed(name);
                TextRun style = Current();
                if (name == StringView(u8"b") || name == StringView(u8"i") || name == StringView(u8"u"))
                {
                    if (hasValue)
                        return Reject(at, u8"[b], [i] and [u] take no value");
                    if (name == StringView(u8"b"))
                        style.bold = true;
                    else if (name == StringView(u8"i"))
                        style.italic = true;
                    else
                        style.underline = true;
                }
                else if (name == StringView(u8"color"))
                {
                    const Optional<Color> color = ParseColor(value);
                    if (!color.HasValue())
                        return Reject(at, u8"[color=] takes #rrggbb, #rrggbbaa or a colour name");
                    style.color = color;
                }
                else if (name == StringView(u8"size"))
                {
                    const Optional<f64> size = ParseFloat(value);
                    if (!size.HasValue() || size.Value() <= 0.0)
                        return Reject(at, u8"[size=] takes a positive number");
                    style.size = static_cast<f32>(size.Value());
                }
                else if (name == StringView(u8"font"))
                {
                    if (value.IsEmpty())
                        return Reject(at, u8"[font=] takes a family name");
                    style.family = String(value);
                }
                else if (name == StringView(u8"link"))
                {
                    if (value.IsEmpty())
                        return Reject(at, u8"[link=] takes an id");
                    style.link = String(value);
                }
                else if (name == StringView(u8"icon"))
                {
                    if (value.IsEmpty())
                        return Reject(at, u8"[icon=] takes an image path");
                    Flush();
                    TextRun icon = Current();
                    icon.icon = String(value);
                    m_document.runs.PushBack(Move(icon));
                    return true;
                }
                else
                {
                    return Reject(at, u8"an unknown tag");
                }
                Flush();
                MarkupFrame frame;
                frame.tag = String(name);
                frame.style = Move(style);
                m_stack.PushBack(Move(frame));
                return true;
            }

            // [/name] closes back to the innermost frame of that name (inner ones with it); [/]
            // closes the innermost.
            bool Close(StringView name, usize at)
            {
                if (m_stack.IsEmpty())
                    return Reject(at, u8"a close with nothing open");
                usize index = m_stack.Size() - 1;
                if (!name.IsEmpty())
                {
                    while (m_stack[index].tag.AsView() != name)
                    {
                        if (index == 0)
                            return Reject(at, u8"a close that matches no open tag");
                        --index;
                    }
                    if (index != m_stack.Size() - 1)
                        Diagnose(at, u8"closes tags opened inside it");
                }
                Flush();
                m_stack.Resize(index);
                return true;
            }

            bool Reject(usize at, const char8_t* message)
            {
                Diagnose(at, message);
                return false;
            }

            void Diagnose(usize at, const char8_t* message)
            {
                TextMarkupDiagnostic diagnostic;
                diagnostic.offset = at;
                diagnostic.message = String(StringView(message));
                m_document.diagnostics.PushBack(Move(diagnostic));
            }

            [[nodiscard]] TextRun Current() const { return m_stack.IsEmpty() ? TextRun{} : m_stack.Back().style; }

            // The text so far becomes a run in the current style (merged into the last when alike).
            void Flush()
            {
                if (m_text.IsEmpty())
                    return;
                TextRun run = Current();
                if (!m_document.runs.IsEmpty() && m_document.runs.Back().SameStyle(run))
                {
                    m_document.runs.Back().text.Append(m_text.AsView());
                }
                else
                {
                    run.text = Move(m_text);
                    m_document.runs.PushBack(Move(run));
                }
                m_text = String();
            }

            [[nodiscard]] Optional<Color> ParseColor(StringView value) const
            {
                if (value.IsEmpty())
                    return {};
                if (value[0] == u8'#')
                    return StyleValueParser::ParseHexColor(value);
                if (m_resolver != nullptr && *m_resolver)
                {
                    const StringView name =
                        value[0] == u8'$' ? value.SubStr(1, value.Size() - 1) : value;
                    if (Optional<Color> named = (*m_resolver)(name); named.HasValue())
                        return named;
                }
                return StyleValueParser::ParseNamedColor(value);
            }

            StringView m_markup;
            const TextMarkupColorResolver* m_resolver;
            TextDocument m_document;
            Array<MarkupFrame> m_stack;
            String m_text;
        };
    }

    inline TextDocument ParseTextMarkup(StringView markup, const TextMarkupColorResolver* resolver)
    {
        return detail::TextMarkupParser(markup, resolver).Parse();
    }
}
