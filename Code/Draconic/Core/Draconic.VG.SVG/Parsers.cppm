// Draconic::VG::SVG - :parsers partition.
//
// The standalone SVG attribute parsers (no XML dependency): SVGColorParser
// (hex/#rgb/rgb()/named), SVGTransformParser (translate/scale/rotate/skew/matrix
// -> Float4x4), SVGPathParser (the `d` path-data string -> PathBuilder). Ported from
// Sedulous.VG.SVG. Public colors are float Color; transforms are Float4x4.

module;
#include "Draconic.Foundation/Prelude.h"
#include <cstdlib> // std::strtof

export module draconic.vg.svg:parsers;

import draconic.foundation;
import draconic.vg;

using namespace draconic::foundation;

export namespace draconic::vg::svg
{
    namespace detail
    {
        // Parse a (already-scanned) numeric substring to f32 via strtof.
        [[nodiscard]] inline Optional<f32> StrToF32(StringView s)
        {
            if (s.IsEmpty())
                return {};
            char buf[64];
            const usize n = s.Size() < 63 ? s.Size() : 63;
            for (usize i = 0; i < n; ++i)
                buf[i] = static_cast<char>(s[i]);
            buf[n] = '\0';
            char* end = nullptr;
            const f32 v = std::strtof(buf, &end);
            if (end == buf)
                return {};
            return v;
        }

        [[nodiscard]] inline bool IsDigit(utf8char c) { return c >= u8'0' && c <= u8'9'; }
        [[nodiscard]] inline bool IsDigitOrSign(utf8char c)
        {
            return IsDigit(c) || c == u8'-' || c == u8'+' || c == u8'.';
        }
        [[nodiscard]] inline utf8char ToLowerC(utf8char c)
        {
            return (c >= u8'A' && c <= u8'Z') ? static_cast<utf8char>(c - u8'A' + u8'a') : c;
        }

        [[nodiscard]] inline bool EqualsIgnoreCase(StringView a, StringView b)
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); ++i)
                if (ToLowerC(a[i]) != ToLowerC(b[i]))
                    return false;
            return true;
        }
    }

    /// Parses SVG color strings (hex, named, or rgb()) into Color32.
    class SVGColorParser
    {
    public:
        // Parse an SVG color string to the engine's float Color (VG's currency).
        static Result<Color> Parse(StringView colorStr)
        {
            Result<Color32> bytes = ParseBytes(colorStr);
            if (!bytes.HasValue())
                return Err(bytes.Error());
            return ToColor(bytes.Value());
        }

    private:
        // The hex/#rgb/rgb()/named parse, producing packed bytes.
        static Result<Color32> ParseBytes(StringView colorStr)
        {
            StringView s = colorStr;
            while (s.Size() > 0 && (s[0] == u8' ' || s[0] == u8'\t'))
                s = s.SubStr(1, s.Size() - 1);
            while (s.Size() > 0 && (s[s.Size() - 1] == u8' ' || s[s.Size() - 1] == u8'\t'))
                s = s.SubStr(0, s.Size() - 1);
            if (s.IsEmpty())
                return Err(ErrorCode::InvalidArgument);

            if (s[0] == u8'#')
            {
                if (s.Size() == 7) // #rrggbb
                {
                    const Optional<u8> r = ParseHexByte(s, 1), g = ParseHexByte(s, 3),
                                       b = ParseHexByte(s, 5);
                    if (!r || !g || !b)
                        return Err(ErrorCode::InvalidArgument);
                    return Color32(r.Value(), g.Value(), b.Value());
                }
                if (s.Size() == 4) // #rgb
                {
                    const Optional<u8> r = ParseHexNibble(s, 1), g = ParseHexNibble(s, 2),
                                       b = ParseHexNibble(s, 3);
                    if (!r || !g || !b)
                        return Err(ErrorCode::InvalidArgument);
                    return Color32(static_cast<u8>(r.Value() | (r.Value() << 4)),
                                   static_cast<u8>(g.Value() | (g.Value() << 4)),
                                   static_cast<u8>(b.Value() | (b.Value() << 4)));
                }
                if (s.Size() == 9) // #rrggbbaa
                {
                    const Optional<u8> r = ParseHexByte(s, 1), g = ParseHexByte(s, 3),
                                       b = ParseHexByte(s, 5), a = ParseHexByte(s, 7);
                    if (!r || !g || !b || !a)
                        return Err(ErrorCode::InvalidArgument);
                    return Color32(r.Value(), g.Value(), b.Value(), a.Value());
                }
                return Err(ErrorCode::InvalidArgument);
            }

            if (s.Size() >= 5 && s[0] == u8'r' && s[1] == u8'g' && s[2] == u8'b' && s[3] == u8'(')
            {
                usize pos = 4;
                const Optional<i32> r = ParseInt(s, pos);
                SkipComma(s, pos);
                const Optional<i32> g = ParseInt(s, pos);
                SkipComma(s, pos);
                const Optional<i32> b = ParseInt(s, pos);
                if (!r || !g || !b)
                    return Err(ErrorCode::InvalidArgument);
                return Color32(static_cast<u8>(r.Value()), static_cast<u8>(g.Value()),
                               static_cast<u8>(b.Value()));
            }

            return ParseNamedColor(s);
        }

    private:
        static Result<Color32> ParseNamedColor(StringView name)
        {
            using detail::EqualsIgnoreCase;
            if (EqualsIgnoreCase(name, u8"black"))
                return Color32(0, 0, 0);
            if (EqualsIgnoreCase(name, u8"white"))
                return Color32(255, 255, 255);
            if (EqualsIgnoreCase(name, u8"red"))
                return Color32(255, 0, 0);
            if (EqualsIgnoreCase(name, u8"green"))
                return Color32(0, 128, 0);
            if (EqualsIgnoreCase(name, u8"blue"))
                return Color32(0, 0, 255);
            if (EqualsIgnoreCase(name, u8"yellow"))
                return Color32(255, 255, 0);
            if (EqualsIgnoreCase(name, u8"cyan") || EqualsIgnoreCase(name, u8"aqua"))
                return Color32(0, 255, 255);
            if (EqualsIgnoreCase(name, u8"magenta") || EqualsIgnoreCase(name, u8"fuchsia"))
                return Color32(255, 0, 255);
            if (EqualsIgnoreCase(name, u8"gray") || EqualsIgnoreCase(name, u8"grey"))
                return Color32(128, 128, 128);
            if (EqualsIgnoreCase(name, u8"silver"))
                return Color32(192, 192, 192);
            if (EqualsIgnoreCase(name, u8"maroon"))
                return Color32(128, 0, 0);
            if (EqualsIgnoreCase(name, u8"olive"))
                return Color32(128, 128, 0);
            if (EqualsIgnoreCase(name, u8"lime"))
                return Color32(0, 255, 0);
            if (EqualsIgnoreCase(name, u8"teal"))
                return Color32(0, 128, 128);
            if (EqualsIgnoreCase(name, u8"navy"))
                return Color32(0, 0, 128);
            if (EqualsIgnoreCase(name, u8"purple"))
                return Color32(128, 0, 128);
            if (EqualsIgnoreCase(name, u8"orange"))
                return Color32(255, 165, 0);
            if (EqualsIgnoreCase(name, u8"pink"))
                return Color32(255, 192, 203);
            if (EqualsIgnoreCase(name, u8"brown"))
                return Color32(165, 42, 42);
            if (EqualsIgnoreCase(name, u8"coral"))
                return Color32(255, 127, 80);
            if (EqualsIgnoreCase(name, u8"gold"))
                return Color32(255, 215, 0);
            if (EqualsIgnoreCase(name, u8"indigo"))
                return Color32(75, 0, 130);
            if (EqualsIgnoreCase(name, u8"ivory"))
                return Color32(255, 255, 240);
            if (EqualsIgnoreCase(name, u8"khaki"))
                return Color32(240, 230, 140);
            if (EqualsIgnoreCase(name, u8"lavender"))
                return Color32(230, 230, 250);
            if (EqualsIgnoreCase(name, u8"none") || EqualsIgnoreCase(name, u8"transparent"))
                return Color32(0, 0, 0, 0);
            return Err(ErrorCode::InvalidArgument);
        }

        [[nodiscard]] static Optional<u8> HexVal(utf8char c)
        {
            if (c >= u8'0' && c <= u8'9')
                return static_cast<u8>(c - u8'0');
            if (c >= u8'a' && c <= u8'f')
                return static_cast<u8>(c - u8'a' + 10);
            if (c >= u8'A' && c <= u8'F')
                return static_cast<u8>(c - u8'A' + 10);
            return {};
        }

        [[nodiscard]] static Optional<u8> ParseHexByte(StringView s, usize offset)
        {
            if (offset + 1 >= s.Size())
                return {};
            const Optional<u8> high = HexVal(s[offset]);
            const Optional<u8> low = HexVal(s[offset + 1]);
            if (!high || !low)
                return {};
            return static_cast<u8>((high.Value() << 4) | low.Value());
        }

        [[nodiscard]] static Optional<u8> ParseHexNibble(StringView s, usize offset)
        {
            if (offset >= s.Size())
                return {};
            return HexVal(s[offset]);
        }

        [[nodiscard]] static Optional<i32> ParseInt(StringView s, usize& pos)
        {
            SkipWhitespace(s, pos);
            const usize start = pos;
            if (pos < s.Size() && (s[pos] == u8'-' || s[pos] == u8'+'))
                ++pos;
            while (pos < s.Size() && detail::IsDigit(s[pos]))
                ++pos;
            if (pos == start)
                return {};
            const Optional<f32> v = detail::StrToF32(s.SubStr(start, pos - start));
            if (!v)
                return {};
            return static_cast<i32>(v.Value());
        }

        static void SkipWhitespace(StringView s, usize& pos)
        {
            while (pos < s.Size() && (s[pos] == u8' ' || s[pos] == u8'\t'))
                ++pos;
        }
        static void SkipComma(StringView s, usize& pos)
        {
            SkipWhitespace(s, pos);
            if (pos < s.Size() && s[pos] == u8',')
                ++pos;
            SkipWhitespace(s, pos);
        }
    };

    /// Parses SVG transform attribute strings into a Float4x4.
    class SVGTransformParser
    {
    public:
        static Result<Float4x4> Parse(StringView transform)
        {
            Float4x4 result = Float4x4::Identity();
            usize pos = 0;

            while (pos < transform.Size())
            {
                SkipWhitespace(transform, pos);
                if (pos >= transform.Size())
                    break;

                if (StartsWith(transform, pos, u8"translate"))
                {
                    pos += 9;
                    if (!SkipParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);
                    Optional<f32> tx = ParseFloat(transform, pos);
                    if (!tx)
                        return Err(ErrorCode::InvalidArgument);
                    SkipComma(transform, pos);
                    f32 ty = 0.0f;
                    if (pos < transform.Size() && detail::IsDigitOrSign(transform[pos]))
                    {
                        Optional<f32> v = ParseFloat(transform, pos);
                        if (!v)
                            return Err(ErrorCode::InvalidArgument);
                        ty = v.Value();
                    }
                    if (!SkipCloseParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);
                    result = Float4x4::Translation(Float3{tx.Value(), ty, 0.0f}) * result;
                }
                else if (StartsWith(transform, pos, u8"scale"))
                {
                    pos += 5;
                    if (!SkipParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);
                    Optional<f32> sx = ParseFloat(transform, pos);
                    if (!sx)
                        return Err(ErrorCode::InvalidArgument);
                    SkipComma(transform, pos);
                    f32 sy = sx.Value();
                    if (pos < transform.Size() && detail::IsDigitOrSign(transform[pos]))
                    {
                        Optional<f32> v = ParseFloat(transform, pos);
                        if (!v)
                            return Err(ErrorCode::InvalidArgument);
                        sy = v.Value();
                    }
                    if (!SkipCloseParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);
                    result = Float4x4::Scale(Float3{sx.Value(), sy, 1.0f}) * result;
                }
                else if (StartsWith(transform, pos, u8"rotate"))
                {
                    pos += 6;
                    if (!SkipParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);
                    Optional<f32> a = ParseFloat(transform, pos);
                    if (!a)
                        return Err(ErrorCode::InvalidArgument);
                    const f32 angle = a.Value() * kPi / 180.0f;
                    SkipComma(transform, pos);

                    f32 cx = 0.0f, cy = 0.0f;
                    if (pos < transform.Size() && detail::IsDigitOrSign(transform[pos]))
                    {
                        Optional<f32> vcx = ParseFloat(transform, pos);
                        if (!vcx)
                            return Err(ErrorCode::InvalidArgument);
                        SkipComma(transform, pos);
                        Optional<f32> vcy = ParseFloat(transform, pos);
                        if (!vcy)
                            return Err(ErrorCode::InvalidArgument);
                        cx = vcx.Value();
                        cy = vcy.Value();
                    }
                    if (!SkipCloseParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);

                    if (cx != 0.0f || cy != 0.0f)
                    {
                        result = Float4x4::Translation(Float3{-cx, -cy, 0.0f}) * result;
                        result = Float4x4::RotationZ(angle) * result;
                        result = Float4x4::Translation(Float3{cx, cy, 0.0f}) * result;
                    }
                    else
                    {
                        result = Float4x4::RotationZ(angle) * result;
                    }
                }
                else if (StartsWith(transform, pos, u8"skewX"))
                {
                    pos += 5;
                    if (!SkipParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);
                    Optional<f32> a = ParseFloat(transform, pos);
                    if (!a)
                        return Err(ErrorCode::InvalidArgument);
                    if (!SkipCloseParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);
                    Float4x4 skew = Float4x4::Identity();
                    skew.m[1][0] = Tan(a.Value() * kPi / 180.0f); // M21
                    result = skew * result;
                }
                else if (StartsWith(transform, pos, u8"skewY"))
                {
                    pos += 5;
                    if (!SkipParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);
                    Optional<f32> a = ParseFloat(transform, pos);
                    if (!a)
                        return Err(ErrorCode::InvalidArgument);
                    if (!SkipCloseParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);
                    Float4x4 skew = Float4x4::Identity();
                    skew.m[0][1] = Tan(a.Value() * kPi / 180.0f); // M12
                    result = skew * result;
                }
                else if (StartsWith(transform, pos, u8"matrix"))
                {
                    pos += 6;
                    if (!SkipParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);
                    f32 v[6];
                    for (i32 i = 0; i < 6; ++i)
                    {
                        Optional<f32> f = ParseFloat(transform, pos);
                        if (!f)
                            return Err(ErrorCode::InvalidArgument);
                        v[i] = f.Value();
                    }
                    if (!SkipCloseParen(transform, pos))
                        return Err(ErrorCode::InvalidArgument);

                    // SVG matrix(a,b,c,d,e,f) -> M11=a M12=b M21=c M22=d M41=e M42=f.
                    Float4x4 mat = Float4x4::Identity();
                    mat.m[0][0] = v[0];
                    mat.m[0][1] = v[1];
                    mat.m[1][0] = v[2];
                    mat.m[1][1] = v[3];
                    mat.m[3][0] = v[4];
                    mat.m[3][1] = v[5];
                    result = mat * result;
                }
                else
                {
                    return Err(ErrorCode::InvalidArgument); // Unknown transform.
                }

                SkipWhitespace(transform, pos);
            }

            return result;
        }

    private:
        static bool StartsWith(StringView s, usize pos, StringView prefix)
        {
            if (pos + prefix.Size() > s.Size())
                return false;
            for (usize i = 0; i < prefix.Size(); ++i)
                if (s[pos + i] != prefix[i])
                    return false;
            return true;
        }
        static void SkipWhitespace(StringView s, usize& pos)
        {
            while (pos < s.Size() &&
                   (s[pos] == u8' ' || s[pos] == u8'\t' || s[pos] == u8'\n' || s[pos] == u8'\r'))
                ++pos;
        }
        static void SkipComma(StringView s, usize& pos)
        {
            SkipWhitespace(s, pos);
            if (pos < s.Size() && s[pos] == u8',')
                ++pos;
            SkipWhitespace(s, pos);
        }
        [[nodiscard]] static bool SkipParen(StringView s, usize& pos)
        {
            SkipWhitespace(s, pos);
            if (pos >= s.Size() || s[pos] != u8'(')
                return false;
            ++pos;
            SkipWhitespace(s, pos);
            return true;
        }
        [[nodiscard]] static bool SkipCloseParen(StringView s, usize& pos)
        {
            SkipWhitespace(s, pos);
            if (pos >= s.Size() || s[pos] != u8')')
                return false;
            ++pos;
            return true;
        }
        [[nodiscard]] static Optional<f32> ParseFloat(StringView s, usize& pos)
        {
            SkipComma(s, pos);
            if (pos >= s.Size())
                return {};
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
            if (pos < s.Size() && (s[pos] == u8'e' || s[pos] == u8'E'))
            {
                ++pos;
                if (pos < s.Size() && (s[pos] == u8'-' || s[pos] == u8'+'))
                    ++pos;
                while (pos < s.Size() && detail::IsDigit(s[pos]))
                    ++pos;
            }
            if (pos == start)
                return {};
            return detail::StrToF32(s.SubStr(start, pos - start));
        }
    };

    /// Parses SVG path-data strings (the `d` attribute) into a PathBuilder.
    class SVGPathParser
    {
    public:
        static Status Parse(StringView pathData, draconic::vg::PathBuilder& builder)
        {
            usize pos = 0;
            f32 currentX = 0.0f, currentY = 0.0f;
            f32 subPathStartX = 0.0f, subPathStartY = 0.0f;
            f32 lastControlX = 0.0f, lastControlY = 0.0f;
            utf8char lastCommand = 0;

            while (pos < pathData.Size())
            {
                SkipWhitespaceAndCommas(pathData, pos);
                if (pos >= pathData.Size())
                    break;

                utf8char cmd = pathData[pos];
                bool isRelative = cmd >= u8'a' && cmd <= u8'z';

                if (IsCommand(cmd))
                {
                    ++pos;
                }
                else if (detail::IsDigitOrSign(cmd))
                {
                    // Implicit repeat of last command (post-MoveTo becomes LineTo).
                    if (lastCommand == u8'M')
                        cmd = u8'L';
                    else if (lastCommand == u8'm')
                        cmd = u8'l';
                    else
                        cmd = lastCommand;
                    isRelative = cmd >= u8'a' && cmd <= u8'z';
                }
                else
                {
                    return ErrorCode::InvalidArgument;
                }

                const utf8char cmdUpper = isRelative ? static_cast<utf8char>(cmd - 32) : cmd;

                switch (cmdUpper)
                {
                case u8'M':
                {
                    f32 x, y;
                    if (!Float(pathData, pos, x) || !Float(pathData, pos, y))
                        return ErrorCode::InvalidArgument;
                    const f32 absX = isRelative ? currentX + x : x;
                    const f32 absY = isRelative ? currentY + y : y;
                    builder.MoveTo(absX, absY);
                    currentX = absX;
                    currentY = absY;
                    subPathStartX = absX;
                    subPathStartY = absY;
                    break;
                }
                case u8'L':
                {
                    f32 x, y;
                    if (!Float(pathData, pos, x) || !Float(pathData, pos, y))
                        return ErrorCode::InvalidArgument;
                    const f32 absX = isRelative ? currentX + x : x;
                    const f32 absY = isRelative ? currentY + y : y;
                    builder.LineTo(absX, absY);
                    currentX = absX;
                    currentY = absY;
                    break;
                }
                case u8'H':
                {
                    f32 x;
                    if (!Float(pathData, pos, x))
                        return ErrorCode::InvalidArgument;
                    const f32 absX = isRelative ? currentX + x : x;
                    builder.LineTo(absX, currentY);
                    currentX = absX;
                    break;
                }
                case u8'V':
                {
                    f32 y;
                    if (!Float(pathData, pos, y))
                        return ErrorCode::InvalidArgument;
                    const f32 absY = isRelative ? currentY + y : y;
                    builder.LineTo(currentX, absY);
                    currentY = absY;
                    break;
                }
                case u8'C':
                {
                    f32 c1x, c1y, c2x, c2y, x, y;
                    if (!Float(pathData, pos, c1x) || !Float(pathData, pos, c1y) ||
                        !Float(pathData, pos, c2x) || !Float(pathData, pos, c2y) ||
                        !Float(pathData, pos, x) || !Float(pathData, pos, y))
                        return ErrorCode::InvalidArgument;
                    const f32 aC1x = isRelative ? currentX + c1x : c1x;
                    const f32 aC1y = isRelative ? currentY + c1y : c1y;
                    const f32 aC2x = isRelative ? currentX + c2x : c2x;
                    const f32 aC2y = isRelative ? currentY + c2y : c2y;
                    const f32 absX = isRelative ? currentX + x : x;
                    const f32 absY = isRelative ? currentY + y : y;
                    builder.CubicTo(aC1x, aC1y, aC2x, aC2y, absX, absY);
                    lastControlX = aC2x;
                    lastControlY = aC2y;
                    currentX = absX;
                    currentY = absY;
                    break;
                }
                case u8'S':
                {
                    f32 c2x, c2y, x, y;
                    if (!Float(pathData, pos, c2x) || !Float(pathData, pos, c2y) ||
                        !Float(pathData, pos, x) || !Float(pathData, pos, y))
                        return ErrorCode::InvalidArgument;
                    f32 rc1x = currentX, rc1y = currentY;
                    if (lastCommand == u8'C' || lastCommand == u8'c' || lastCommand == u8'S' ||
                        lastCommand == u8's')
                    {
                        rc1x = 2.0f * currentX - lastControlX;
                        rc1y = 2.0f * currentY - lastControlY;
                    }
                    const f32 aC2x = isRelative ? currentX + c2x : c2x;
                    const f32 aC2y = isRelative ? currentY + c2y : c2y;
                    const f32 absX = isRelative ? currentX + x : x;
                    const f32 absY = isRelative ? currentY + y : y;
                    builder.CubicTo(rc1x, rc1y, aC2x, aC2y, absX, absY);
                    lastControlX = aC2x;
                    lastControlY = aC2y;
                    currentX = absX;
                    currentY = absY;
                    break;
                }
                case u8'Q':
                {
                    f32 cx, cy, x, y;
                    if (!Float(pathData, pos, cx) || !Float(pathData, pos, cy) ||
                        !Float(pathData, pos, x) || !Float(pathData, pos, y))
                        return ErrorCode::InvalidArgument;
                    const f32 aCx = isRelative ? currentX + cx : cx;
                    const f32 aCy = isRelative ? currentY + cy : cy;
                    const f32 absX = isRelative ? currentX + x : x;
                    const f32 absY = isRelative ? currentY + y : y;
                    builder.QuadTo(aCx, aCy, absX, absY);
                    lastControlX = aCx;
                    lastControlY = aCy;
                    currentX = absX;
                    currentY = absY;
                    break;
                }
                case u8'T':
                {
                    f32 x, y;
                    if (!Float(pathData, pos, x) || !Float(pathData, pos, y))
                        return ErrorCode::InvalidArgument;
                    f32 rcx = currentX, rcy = currentY;
                    if (lastCommand == u8'Q' || lastCommand == u8'q' || lastCommand == u8'T' ||
                        lastCommand == u8't')
                    {
                        rcx = 2.0f * currentX - lastControlX;
                        rcy = 2.0f * currentY - lastControlY;
                    }
                    const f32 absX = isRelative ? currentX + x : x;
                    const f32 absY = isRelative ? currentY + y : y;
                    builder.QuadTo(rcx, rcy, absX, absY);
                    lastControlX = rcx;
                    lastControlY = rcy;
                    currentX = absX;
                    currentY = absY;
                    break;
                }
                case u8'A':
                {
                    f32 rx, ry, xr, x, y;
                    bool largeArc, sweep;
                    if (!Float(pathData, pos, rx) || !Float(pathData, pos, ry) ||
                        !Float(pathData, pos, xr) || !Flag(pathData, pos, largeArc) ||
                        !Flag(pathData, pos, sweep) || !Float(pathData, pos, x) ||
                        !Float(pathData, pos, y))
                        return ErrorCode::InvalidArgument;
                    const f32 absX = isRelative ? currentX + x : x;
                    const f32 absY = isRelative ? currentY + y : y;
                    builder.ArcTo(rx, ry, xr * kPi / 180.0f, largeArc, sweep, absX, absY);
                    currentX = absX;
                    currentY = absY;
                    break;
                }
                case u8'Z':
                    builder.Close();
                    currentX = subPathStartX;
                    currentY = subPathStartY;
                    break;
                default:
                    return ErrorCode::InvalidArgument;
                }

                lastCommand = cmd;
            }

            return ErrorCode::Ok;
        }

    private:
        [[nodiscard]] static bool IsCommand(utf8char c)
        {
            switch (c)
            {
            case u8'M':
            case u8'm':
            case u8'L':
            case u8'l':
            case u8'H':
            case u8'h':
            case u8'V':
            case u8'v':
            case u8'C':
            case u8'c':
            case u8'S':
            case u8's':
            case u8'Q':
            case u8'q':
            case u8'T':
            case u8't':
            case u8'A':
            case u8'a':
            case u8'Z':
            case u8'z':
                return true;
            default:
                return false;
            }
        }

        static void SkipWhitespaceAndCommas(StringView s, usize& pos)
        {
            while (pos < s.Size() && (s[pos] == u8' ' || s[pos] == u8'\t' || s[pos] == u8'\n' ||
                                      s[pos] == u8'\r' || s[pos] == u8','))
                ++pos;
        }

        // Scan + parse one float, advancing pos; returns false on failure.
        [[nodiscard]] static bool Float(StringView s, usize& pos, f32& out)
        {
            SkipWhitespaceAndCommas(s, pos);
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
            if (pos < s.Size() && (s[pos] == u8'e' || s[pos] == u8'E'))
            {
                ++pos;
                if (pos < s.Size() && (s[pos] == u8'-' || s[pos] == u8'+'))
                    ++pos;
                while (pos < s.Size() && detail::IsDigit(s[pos]))
                    ++pos;
            }
            if (pos == start)
                return false;
            const Optional<f32> v =
                detail::StrToF32(s.SubStr(start, pos - start)); // strtof handles leading dot
            if (!v)
                return false;
            out = v.Value();
            return true;
        }

        [[nodiscard]] static bool Flag(StringView s, usize& pos, bool& out)
        {
            SkipWhitespaceAndCommas(s, pos);
            if (pos >= s.Size())
                return false;
            const utf8char c = s[pos];
            if (c == u8'0')
            {
                ++pos;
                out = false;
                return true;
            }
            if (c == u8'1')
            {
                ++pos;
                out = true;
                return true;
            }
            return false;
        }
    };
}
