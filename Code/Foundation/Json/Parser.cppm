// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Json - :parser partition
//
// Hand-rolled recursive-descent JSON parser (RFC 8259), UTF-8 in / JsonValue out. No exceptions: a
// failure returns a ParseResult with ok=false, a human-readable message, and the byte offset. A
// nesting-depth guard bounds recursion so hostile wire input cannot blow the stack.

module;
#include "Core/Prelude.h"

export module foundation.json:parser;

import foundation.core;
import :value;

using namespace foundation::core;

export namespace foundation::json
{
    struct ParseResult
    {
        bool ok = false;
        JsonValue value;
        String error;     // empty on success
        i64 position = 0; // byte offset of the error (0 on success)
    };
}

namespace foundation::json::detail
{
    inline constexpr i32 kMaxDepth = 256;

    inline void AppendCodepoint(String& out, u32 cp)
    {
        if (cp <= 0x7F)
        {
            out.PushBack(static_cast<char8_t>(cp));
        }
        else if (cp <= 0x7FF)
        {
            out.PushBack(static_cast<char8_t>(0xC0 | (cp >> 6)));
            out.PushBack(static_cast<char8_t>(0x80 | (cp & 0x3F)));
        }
        else if (cp <= 0xFFFF)
        {
            out.PushBack(static_cast<char8_t>(0xE0 | (cp >> 12)));
            out.PushBack(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F)));
            out.PushBack(static_cast<char8_t>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.PushBack(static_cast<char8_t>(0xF0 | (cp >> 18)));
            out.PushBack(static_cast<char8_t>(0x80 | ((cp >> 12) & 0x3F)));
            out.PushBack(static_cast<char8_t>(0x80 | ((cp >> 6) & 0x3F)));
            out.PushBack(static_cast<char8_t>(0x80 | (cp & 0x3F)));
        }
    }

    struct Parser
    {
        const char8_t* d = nullptr;
        usize n = 0;
        usize pos = 0;
        String error;
        usize errorPos = 0;

        [[nodiscard]] bool Fail(const char8_t* msg, usize at)
        {
            if (error.Size() == 0) // keep the first (innermost) error
            {
                error = String(msg);
                errorPos = at;
            }
            return false;
        }

        [[nodiscard]] bool AtEnd() const noexcept { return pos >= n; }
        [[nodiscard]] char8_t Peek() const noexcept { return pos < n ? d[pos] : u8'\0'; }

        void SkipWs() noexcept
        {
            while (pos < n)
            {
                const char8_t c = d[pos];
                if (c == u8' ' || c == u8'\t' || c == u8'\n' || c == u8'\r')
                {
                    ++pos;
                }
                else
                {
                    break;
                }
            }
        }

        [[nodiscard]] bool ParseHex4(u32& out)
        {
            if (pos + 4 > n)
            {
                return Fail(u8"truncated \\u escape", pos);
            }
            u32 value = 0;
            for (i32 i = 0; i < 4; ++i)
            {
                const char8_t c = d[pos++];
                value <<= 4;
                if (c >= u8'0' && c <= u8'9')
                {
                    value |= static_cast<u32>(c - u8'0');
                }
                else if (c >= u8'a' && c <= u8'f')
                {
                    value |= static_cast<u32>(c - u8'a' + 10);
                }
                else if (c >= u8'A' && c <= u8'F')
                {
                    value |= static_cast<u32>(c - u8'A' + 10);
                }
                else
                {
                    return Fail(u8"invalid hex digit in \\u escape", pos - 1);
                }
            }
            out = value;
            return true;
        }

        [[nodiscard]] bool ParseString(String& out)
        {
            // Caller has consumed the opening quote.
            while (pos < n)
            {
                const char8_t c = d[pos++];
                if (c == u8'"')
                {
                    return true;
                }
                if (c == u8'\\')
                {
                    if (pos >= n)
                    {
                        return Fail(u8"unterminated escape in string", pos);
                    }
                    const char8_t e = d[pos++];
                    switch (e)
                    {
                    case u8'"': out.PushBack(u8'"'); break;
                    case u8'\\': out.PushBack(u8'\\'); break;
                    case u8'/': out.PushBack(u8'/'); break;
                    case u8'b': out.PushBack(u8'\b'); break;
                    case u8'f': out.PushBack(u8'\f'); break;
                    case u8'n': out.PushBack(u8'\n'); break;
                    case u8'r': out.PushBack(u8'\r'); break;
                    case u8't': out.PushBack(u8'\t'); break;
                    case u8'u':
                    {
                        u32 cp = 0;
                        if (!ParseHex4(cp))
                        {
                            return false;
                        }
                        // Surrogate pair: high surrogate must be followed by \uDC00-\uDFFF.
                        if (cp >= 0xD800 && cp <= 0xDBFF)
                        {
                            if (pos + 2 > n || d[pos] != u8'\\' || d[pos + 1] != u8'u')
                            {
                                return Fail(u8"unpaired high surrogate", pos);
                            }
                            pos += 2;
                            u32 lo = 0;
                            if (!ParseHex4(lo))
                            {
                                return false;
                            }
                            if (lo < 0xDC00 || lo > 0xDFFF)
                            {
                                return Fail(u8"invalid low surrogate", pos);
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        else if (cp >= 0xDC00 && cp <= 0xDFFF)
                        {
                            return Fail(u8"unexpected low surrogate", pos);
                        }
                        AppendCodepoint(out, cp);
                        break;
                    }
                    default: return Fail(u8"invalid escape character", pos - 1);
                    }
                }
                else if (c < 0x20)
                {
                    return Fail(u8"control character in string (must be escaped)", pos - 1);
                }
                else
                {
                    out.PushBack(c); // UTF-8 continuation bytes pass through verbatim
                }
            }
            return Fail(u8"unterminated string", pos);
        }

        [[nodiscard]] bool ParseNumber(JsonValue& out)
        {
            const usize start = pos;
            if (Peek() == u8'-')
            {
                ++pos;
            }
            // RFC 8259: an integer part is `0` or a non-zero digit followed by digits; `01`
            // is not a number (it would parse as 1 and hide a typo or a smuggled octal).
            if (pos + 1 < n && d[pos] == u8'0' && d[pos + 1] >= u8'0' && d[pos + 1] <= u8'9')
            {
                return Fail(u8"leading zero", start);
            }
            while (pos < n && d[pos] >= u8'0' && d[pos] <= u8'9')
            {
                ++pos;
            }
            if (pos < n && d[pos] == u8'.')
            {
                ++pos;
                while (pos < n && d[pos] >= u8'0' && d[pos] <= u8'9')
                {
                    ++pos;
                }
            }
            if (pos < n && (d[pos] == u8'e' || d[pos] == u8'E'))
            {
                ++pos;
                if (pos < n && (d[pos] == u8'+' || d[pos] == u8'-'))
                {
                    ++pos;
                }
                while (pos < n && d[pos] >= u8'0' && d[pos] <= u8'9')
                {
                    ++pos;
                }
            }
            const StringView token(d + start, pos - start);
            const Optional<f64> parsed = ParseFloat(token);
            if (!parsed.HasValue())
            {
                return Fail(u8"invalid number", start);
            }
            out = JsonValue::MakeNumber(parsed.Value());
            return true;
        }

        [[nodiscard]] bool Literal(const char8_t* word, usize len)
        {
            if (pos + len > n)
            {
                return false;
            }
            for (usize i = 0; i < len; ++i)
            {
                if (d[pos + i] != word[i])
                {
                    return false;
                }
            }
            pos += len;
            return true;
        }

        [[nodiscard]] bool ParseValue(JsonValue& out, i32 depth)
        {
            if (depth > kMaxDepth)
            {
                return Fail(u8"maximum nesting depth exceeded", pos);
            }
            SkipWs();
            if (AtEnd())
            {
                return Fail(u8"unexpected end of input", pos);
            }
            const char8_t c = d[pos];
            switch (c)
            {
            case u8'{': return ParseObject(out, depth);
            case u8'[': return ParseArray(out, depth);
            case u8'"':
            {
                ++pos;
                String s;
                if (!ParseString(s))
                {
                    return false;
                }
                out = JsonValue::MakeString(Move(s));
                return true;
            }
            case u8't':
                if (Literal(u8"true", 4))
                {
                    out = JsonValue::MakeBool(true);
                    return true;
                }
                return Fail(u8"invalid literal", pos);
            case u8'f':
                if (Literal(u8"false", 5))
                {
                    out = JsonValue::MakeBool(false);
                    return true;
                }
                return Fail(u8"invalid literal", pos);
            case u8'n':
                if (Literal(u8"null", 4))
                {
                    out = JsonValue::MakeNull();
                    return true;
                }
                return Fail(u8"invalid literal", pos);
            default:
                if (c == u8'-' || (c >= u8'0' && c <= u8'9'))
                {
                    return ParseNumber(out);
                }
                return Fail(u8"unexpected character", pos);
            }
        }

        [[nodiscard]] bool ParseArray(JsonValue& out, i32 depth)
        {
            ++pos; // '['
            out = JsonValue::MakeArray();
            SkipWs();
            if (Peek() == u8']')
            {
                ++pos;
                return true;
            }
            for (;;)
            {
                JsonValue element;
                if (!ParseValue(element, depth + 1))
                {
                    return false;
                }
                out.Add(Move(element));
                SkipWs();
                const char8_t c = Peek();
                if (c == u8',')
                {
                    ++pos;
                    continue;
                }
                if (c == u8']')
                {
                    ++pos;
                    return true;
                }
                return Fail(u8"expected ',' or ']' in array", pos);
            }
        }

        [[nodiscard]] bool ParseObject(JsonValue& out, i32 depth)
        {
            ++pos; // '{'
            out = JsonValue::MakeObject();
            SkipWs();
            if (Peek() == u8'}')
            {
                ++pos;
                return true;
            }
            for (;;)
            {
                SkipWs();
                if (Peek() != u8'"')
                {
                    return Fail(u8"expected string key in object", pos);
                }
                ++pos;
                String key;
                if (!ParseString(key))
                {
                    return false;
                }
                SkipWs();
                if (Peek() != u8':')
                {
                    return Fail(u8"expected ':' after object key", pos);
                }
                ++pos;
                JsonValue value;
                if (!ParseValue(value, depth + 1))
                {
                    return false;
                }
                out.Set(Move(key), Move(value));
                SkipWs();
                const char8_t c = Peek();
                if (c == u8',')
                {
                    ++pos;
                    continue;
                }
                if (c == u8'}')
                {
                    ++pos;
                    return true;
                }
                return Fail(u8"expected ',' or '}' in object", pos);
            }
        }
    };
}

export namespace foundation::json
{
    // Parse JSON text into a JsonValue. On failure, result.ok is false and result.error/position
    // describe the first problem (result.value is left Null).
    [[nodiscard]] inline ParseResult Parse(StringView text)
    {
        detail::Parser p;
        p.d = text.Data();
        p.n = text.Size();

        ParseResult result;
        if (!p.ParseValue(result.value, 0))
        {
            result.value = JsonValue::MakeNull();
            result.error = Move(p.error);
            result.position = static_cast<i64>(p.errorPos);
            return result;
        }
        p.SkipWs();
        if (!p.AtEnd())
        {
            result.value = JsonValue::MakeNull();
            result.error = String(u8"trailing characters after JSON value");
            result.position = static_cast<i64>(p.pos);
            return result;
        }
        result.ok = true;
        return result;
    }
}
