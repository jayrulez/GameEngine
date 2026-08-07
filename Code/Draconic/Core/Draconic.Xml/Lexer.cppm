// Draconic::Xml - :lexer partition
//
// Low-level UTF-8 scanning primitives: name/value/content reading, entity and
// character-reference decoding, CDATA/comment/PI bodies. Operates on byte
// (UTF-8) StringViews; bytes >= 0x80 count as name characters so multi-byte
// names pass through. Ported from Sedulous.Xml/XmlLexer.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.xml:lexer;

import draconic.foundation;
import :result;

using namespace draconic::foundation;

export namespace draconic::xml
{
    class XmlLexer
    {
    public:
        // XML whitespace: space, tab, CR, LF.
        [[nodiscard]] static usize GetWhitespaceLength(StringView text)
        {
            usize index = 0;
            while (index < text.Size())
            {
                const utf8char c = text[index];
                if (c == u8' ' || c == u8'\t' || c == u8'\r' || c == u8'\n')
                {
                    ++index;
                }
                else
                {
                    break;
                }
            }
            return index;
        }

        [[nodiscard]] static bool IsWhitespace(utf8char c)
        {
            return c == u8' ' || c == u8'\t' || c == u8'\r' || c == u8'\n';
        }

        [[nodiscard]] static bool IsNameStartChar(utf8char c)
        {
            return sNameStartCharState[static_cast<u8>(c)] != 0;
        }
        [[nodiscard]] static bool IsNameChar(utf8char c)
        {
            return sNameCharState[static_cast<u8>(c)] != 0;
        }

        [[nodiscard]] static bool IsHexDigit(utf8char c)
        {
            return (c >= u8'0' && c <= u8'9') || (c >= u8'A' && c <= u8'F') ||
                   (c >= u8'a' && c <= u8'f');
        }

        [[nodiscard]] static int HexDigitValue(utf8char c)
        {
            if (c >= u8'0' && c <= u8'9')
            {
                return c - u8'0';
            }
            if (c >= u8'A' && c <= u8'F')
            {
                return c - u8'A' + 10;
            }
            if (c >= u8'a' && c <= u8'f')
            {
                return c - u8'a' + 10;
            }
            return -1;
        }

        // Reads an XML name: NameStartChar (NameChar)*. Sets `length`.
        [[nodiscard]] static XmlResult ReadName(StringView text, usize& length)
        {
            length = 0;
            if (text.IsEmpty())
            {
                return XmlResult::NameEmpty;
            }
            if (!IsNameStartChar(text[0]))
            {
                return XmlResult::NameEmpty;
            }

            usize index = 1;
            while (index < text.Size() && IsNameChar(text[index]))
            {
                ++index;
            }
            length = index;
            return XmlResult::Ok;
        }

        [[nodiscard]] static XmlResult ReadName(StringView text, usize& length, String& output)
        {
            const XmlResult result = ReadName(text, length);
            if (result == XmlResult::Ok)
            {
                output.Clear();
                output.Append(text.SubStr(0, length));
            }
            return result;
        }

        // Reads a quoted attribute value (' or "), decoding references.
        [[nodiscard]] static XmlResult ReadAttributeValue(StringView text, usize& length,
                                                          String& output)
        {
            length = 0;
            output.Clear();
            if (text.IsEmpty())
            {
                return XmlResult::AttributeMissingQuote;
            }

            const utf8char quote = text[0];
            if (quote != u8'"' && quote != u8'\'')
            {
                return XmlResult::AttributeMissingQuote;
            }

            usize index = 1;
            while (index < text.Size())
            {
                const utf8char c = text[index];
                if (c == quote)
                {
                    length = index + 1;
                    return XmlResult::Ok;
                }
                if (c == u8'<')
                {
                    return XmlResult::AttributeValueInvalid;
                }
                if (c == u8'&')
                {
                    usize refLen = 0;
                    const XmlResult refResult = DecodeReference(Rest(text, index), refLen, output);
                    if (refResult != XmlResult::Ok)
                    {
                        return refResult;
                    }
                    index += refLen;
                }
                else
                {
                    output.PushBack(c);
                    ++index;
                }
            }
            return XmlResult::AttributeMissingQuote; // unclosed
        }

        // Reads text content until a stop char (default '<'), decoding references.
        [[nodiscard]] static XmlResult ReadTextContent(StringView text, usize& length,
                                                       String& output,
                                                       StringView stopChars = StringView(u8"<"))
        {
            length = 0;
            output.Clear();
            usize index = 0;
            while (index < text.Size())
            {
                const utf8char c = text[index];
                bool shouldStop = false;
                for (usize i = 0; i < stopChars.Size(); ++i)
                {
                    if (c == stopChars[i])
                    {
                        shouldStop = true;
                        break;
                    }
                }
                if (shouldStop)
                {
                    break;
                }

                if (c == u8'&')
                {
                    usize refLen = 0;
                    const XmlResult refResult = DecodeReference(Rest(text, index), refLen, output);
                    if (refResult != XmlResult::Ok)
                    {
                        return refResult;
                    }
                    index += refLen;
                }
                else
                {
                    output.PushBack(c);
                    ++index;
                }
            }
            length = index;
            return XmlResult::Ok;
        }

        // Decodes an entity or character reference (text starts with '&').
        [[nodiscard]] static XmlResult DecodeReference(StringView text, usize& length,
                                                       String& output)
        {
            length = 0;
            if (text.IsEmpty() || text[0] != u8'&')
            {
                return XmlResult::EntityMalformed;
            }
            if (text.Size() > 2 && text[1] == u8'#')
            {
                return DecodeCharacterReference(text, length, output);
            }
            return DecodeEntityReference(text, length, output);
        }

        // Reads a CDATA body (text after "<![CDATA["), consuming through "]]>".
        [[nodiscard]] static XmlResult ReadCDataContent(StringView text, usize& length,
                                                        String& output)
        {
            length = 0;
            output.Clear();
            usize index = 0;
            while (index < text.Size())
            {
                if (index + 2 < text.Size() && text[index] == u8']' && text[index + 1] == u8']' &&
                    text[index + 2] == u8'>')
                {
                    length = index + 3;
                    return XmlResult::Ok;
                }
                output.PushBack(text[index]);
                ++index;
            }
            return XmlResult::CDataUnclosed;
        }

        // Reads a comment body (text after "<!--"), consuming through "-->".
        [[nodiscard]] static XmlResult ReadCommentContent(StringView text, usize& length,
                                                          String& output)
        {
            length = 0;
            output.Clear();
            usize index = 0;
            while (index < text.Size())
            {
                if (index + 2 < text.Size() && text[index] == u8'-' && text[index + 1] == u8'-')
                {
                    if (text[index + 2] == u8'>')
                    {
                        length = index + 3;
                        return XmlResult::Ok;
                    }
                    return XmlResult::CommentIllegalSequence; // "--" not followed by ">"
                }
                output.PushBack(text[index]);
                ++index;
            }
            return XmlResult::CommentUnclosed;
        }

        // Reads a processing instruction (text after "<?"), through "?>".
        [[nodiscard]] static XmlResult ReadProcessingInstruction(StringView text, usize& length,
                                                                 String& target, String& data)
        {
            length = 0;
            target.Clear();
            data.Clear();

            usize nameLen = 0;
            if (ReadName(text, nameLen, target) != XmlResult::Ok)
            {
                return XmlResult::PIInvalid;
            }

            usize index = nameLen;
            while (index < text.Size() && IsWhitespace(text[index]))
            {
                ++index;
            }

            if (index + 1 < text.Size() && text[index] == u8'?' && text[index + 1] == u8'>')
            {
                length = index + 2;
                return XmlResult::Ok;
            }

            while (index < text.Size())
            {
                if (index + 1 < text.Size() && text[index] == u8'?' && text[index + 1] == u8'>')
                {
                    length = index + 2;
                    return XmlResult::Ok;
                }
                data.PushBack(text[index]);
                ++index;
            }
            return XmlResult::PIUnclosed;
        }

        // XML 1.0 valid character ranges.
        [[nodiscard]] static bool IsValidXmlChar(u32 code)
        {
            return code == 0x09 || code == 0x0A || code == 0x0D ||
                   (code >= 0x20 && code <= 0xD7FF) || (code >= 0xE000 && code <= 0xFFFD) ||
                   (code >= 0x10000 && code <= 0x10FFFF);
        }

        [[nodiscard]] static bool IsValidName(StringView name)
        {
            if (name.IsEmpty())
            {
                return false;
            }
            if (!IsNameStartChar(name[0]))
            {
                return false;
            }
            for (usize i = 1; i < name.Size(); ++i)
            {
                if (!IsNameChar(name[i]))
                {
                    return false;
                }
            }
            return true;
        }

        // Splits "prefix:local" into prefix + local (prefix empty if no colon).
        static void SplitQualifiedName(StringView qualifiedName, String& prefix, String& localName)
        {
            prefix.Clear();
            localName.Clear();

            usize colon = qualifiedName.Size();
            for (usize i = 0; i < qualifiedName.Size(); ++i)
            {
                if (qualifiedName[i] == u8':')
                {
                    colon = i;
                    break;
                }
            }
            if (colon < qualifiedName.Size())
            {
                prefix.Append(qualifiedName.SubStr(0, colon));
                localName.Append(qualifiedName.SubStr(colon + 1, qualifiedName.Size() - colon - 1));
            }
            else
            {
                localName.Append(qualifiedName);
            }
        }

    private:
        // Returns the view from `index` to the end.
        [[nodiscard]] static StringView Rest(StringView text, usize index)
        {
            return text.SubStr(index, text.Size() - index);
        }

        static void AppendUtf8(String& out, u32 cp)
        {
            if (cp < 0x80u)
            {
                out.PushBack(static_cast<utf8char>(cp));
            }
            else if (cp < 0x800u)
            {
                out.PushBack(static_cast<utf8char>(0xC0u | (cp >> 6)));
                out.PushBack(static_cast<utf8char>(0x80u | (cp & 0x3Fu)));
            }
            else if (cp < 0x10000u)
            {
                out.PushBack(static_cast<utf8char>(0xE0u | (cp >> 12)));
                out.PushBack(static_cast<utf8char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.PushBack(static_cast<utf8char>(0x80u | (cp & 0x3Fu)));
            }
            else
            {
                out.PushBack(static_cast<utf8char>(0xF0u | (cp >> 18)));
                out.PushBack(static_cast<utf8char>(0x80u | ((cp >> 12) & 0x3Fu)));
                out.PushBack(static_cast<utf8char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.PushBack(static_cast<utf8char>(0x80u | (cp & 0x3Fu)));
            }
        }

        [[nodiscard]] static XmlResult DecodeCharacterReference(StringView text, usize& length,
                                                                String& output)
        {
            length = 0;
            if (text.Size() < 4 || text[0] != u8'&' || text[1] != u8'#')
            {
                return XmlResult::CharRefInvalid;
            }

            usize index = 2;
            u32 codepoint = 0;
            bool isHex = false;
            if (text[index] == u8'x' || text[index] == u8'X')
            {
                isHex = true;
                ++index;
            }
            if (index >= text.Size())
            {
                return XmlResult::CharRefInvalid;
            }

            bool hasDigits = false;
            while (index < text.Size())
            {
                const utf8char c = text[index];
                if (c == u8';')
                {
                    if (!hasDigits)
                    {
                        return XmlResult::CharRefInvalid;
                    }
                    if (codepoint == 0 || codepoint > 0x10FFFF)
                    {
                        return XmlResult::CharRefOutOfRange;
                    }
                    if (!IsValidXmlChar(codepoint))
                    {
                        return XmlResult::CharRefOutOfRange;
                    }
                    AppendUtf8(output, codepoint);
                    length = index + 1;
                    return XmlResult::Ok;
                }
                if (isHex)
                {
                    if (!IsHexDigit(c))
                    {
                        return XmlResult::CharRefInvalid;
                    }
                    const u32 digit = static_cast<u32>(HexDigitValue(c));
                    if (codepoint > (0x10FFFFu - digit) / 16)
                    {
                        return XmlResult::CharRefOutOfRange;
                    }
                    codepoint = codepoint * 16 + digit;
                }
                else
                {
                    if (c < u8'0' || c > u8'9')
                    {
                        return XmlResult::CharRefInvalid;
                    }
                    const u32 digit = static_cast<u32>(c - u8'0');
                    if (codepoint > (0x10FFFFu - digit) / 10)
                    {
                        return XmlResult::CharRefOutOfRange;
                    }
                    codepoint = codepoint * 10 + digit;
                }
                hasDigits = true;
                ++index;
            }
            return XmlResult::CharRefInvalid; // missing ';'
        }

        [[nodiscard]] static XmlResult DecodeEntityReference(StringView text, usize& length,
                                                             String& output)
        {
            length = 0;
            if (text.IsEmpty() || text[0] != u8'&')
            {
                return XmlResult::EntityMalformed;
            }

            usize index = 1;
            const usize nameStart = index;
            while (index < text.Size() && text[index] != u8';')
            {
                const utf8char c = text[index];
                if (index == nameStart)
                {
                    if (!IsNameStartChar(c))
                    {
                        return XmlResult::EntityMalformed;
                    }
                }
                else if (!IsNameChar(c))
                {
                    return XmlResult::EntityMalformed;
                }
                ++index;
            }
            if (index >= text.Size() || text[index] != u8';')
            {
                return XmlResult::EntityMalformed;
            }

            const StringView name = text.SubStr(nameStart, index - nameStart);
            length = index + 1;

            if (name == StringView(u8"amp"))
            {
                output.PushBack(u8'&');
                return XmlResult::Ok;
            }
            if (name == StringView(u8"lt"))
            {
                output.PushBack(u8'<');
                return XmlResult::Ok;
            }
            if (name == StringView(u8"gt"))
            {
                output.PushBack(u8'>');
                return XmlResult::Ok;
            }
            if (name == StringView(u8"apos"))
            {
                output.PushBack(u8'\'');
                return XmlResult::Ok;
            }
            if (name == StringView(u8"quot"))
            {
                output.PushBack(u8'"');
                return XmlResult::Ok;
            }
            return XmlResult::EntityUnknown;
        }

        // 1 = valid XML name start char (letter, '_', ':'); bytes >= 0x80 allowed.
        static constexpr u8 sNameStartCharState[256] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 1, 0, 0, 0, 0, 0, // ':' (0x3A)
            0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            0, 0, 0, 0, 1, // '_' (0x5F)
            0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        };

        // 1 = valid XML name char (above + digits, '-', '.').
        static constexpr u8 sNameCharState[256] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, // '-' (0x2D), '.' (0x2E)
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,                // '0'-'9', ':' (0x3A)
            0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            0, 0, 0, 0, 1, // '_' (0x5F)
            0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        };
    };
}
