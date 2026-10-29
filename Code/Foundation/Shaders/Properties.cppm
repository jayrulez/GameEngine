// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Shader properties - the `properties { ... }` block a custom shader declares its material
// parameters in, and the HLSL the engine generates from it (Specs/custom-shaders.md, decided
// 2026-10-12). The block is the ONE declaration of a shader's material layout, in both kinds of
// custom shader: the engine replaces it with the set-2 declarations (the `Material` cbuffer, the
// textures and the samplers), so the layout a material is cooked with and the one the shader
// reads cannot disagree.
//
//     properties
//     {
//         Color   BaseColor = (1, 1, 1, 1);
//         float   Bands     = 3   [1, 8];
//         Texture AlbedoMap;
//         Sampler MainSampler;
//     }
//
// One declaration per line: a type, a name, an optional default and, for a float or an int, an
// optional [min, max] range (the editor's slider). The cbuffer follows HLSL's packing rules,
// through PackCbufferMember, which MaterialBuilder uses too.
//
// An effect block (Specs/wind-for-every-shader.md, ruled 2026-10-29) is the same grammar in an
// include, for a vertex effect's own parameters rather than a shader's:
//
//     effect Wind : WIND
//     {
//         float Strength = 0 [0, 2] (gate);
//         float Speed    = 1 [0, 20];
//         float Height   = 1 [0.01, 100];
//     }
//
// The header names the effect and the variant flag its code is compiled in (one of the engine's
// flags). The body takes uniforms only, and exactly one float marked (gate): the effect is on
// while it is above zero. The block becomes `struct WindEffect { ... };` and
// `ConstantBuffer<WindEffect> Wind : register(b1, space2);`, so its code reads `Wind.Strength`;
// the compiler's include handler expands it wherever the include is served.

module;
#include "Core/Prelude.h"

export module foundation.shaders:properties;

import foundation.core;
import :flags;

using namespace foundation::core;

export namespace foundation::shaders
{
    enum class ShaderPropertyType : u8
    {
        Float,
        Float2,
        Float3,
        Float4,
        Int,
        Color,    // authored sRGB rgba; the shader reads it linear (a float4)
        ColorHdr, // authored sRGB rgb, linear intensity in w (a float4)
        Texture,  // Texture2D
        TextureCube,
        Sampler,
    };

    // HLSL's cbuffer packing, the one rule every material block is laid out by: a member is
    // aligned to its own size and never crosses a 16-byte row (a float3 then a float share a row;
    // a float2 at 12 moves to 16). Returns the member's offset and advances `cursor` past it.
    [[nodiscard]] inline u32 PackCbufferMember(u32& cursor, u32 size) noexcept
    {
        u32 offset = cursor;
        if (size > 0u && offset / 16u != (offset + size - 1u) / 16u)
        {
            offset = (offset + 15u) & ~15u;
        }
        cursor = offset + size;
        return offset;
    }

    // A cbuffer's size: its members' extent rounded up to a whole row.
    [[nodiscard]] inline u32 CbufferSize(u32 cursor) noexcept { return (cursor + 15u) & ~15u; }

    struct ShaderProperty
    {
        String name;
        ShaderPropertyType type = ShaderPropertyType::Float;
        Float4 value{0.0f, 0.0f, 0.0f, 0.0f}; // the default (an int's in x)
        bool hasRange = false;
        f32 rangeMin = 0.0f;
        f32 rangeMax = 0.0f;
        u32 line = 0;    // where it is declared (1-based)
        u32 offset = 0;  // a uniform's byte offset in the cbuffer
        u32 size = 0;    // a uniform's byte size; 0 for a texture or sampler
        u32 binding = 0; // a texture's tN or a sampler's sN
    };

    struct ShaderPropertyError
    {
        u32 line = 0; // 1-based
        String message;
    };

    // What ParseShaderProperties found. `present` false = the source declares no block (a valid
    // shader with no material parameters).
    struct ShaderPropertiesBlock
    {
        bool present = false;
        usize begin = 0;    // byte offset of the `properties` line's start
        usize end = 0;      // byte offset just past the closing brace's line
        u32 firstLine = 0;  // the `properties` line (1-based)
        u32 lastLine = 0;   // the closing brace's line
        Array<ShaderProperty> properties; // in declaration order
        Array<ShaderPropertyError> errors;
        u32 uniformSize = 0; // the cbuffer's size (0 = no uniforms)

        [[nodiscard]] bool IsOk() const noexcept { return errors.IsEmpty(); }
    };

    // Where every effect's cbuffer binds: binding 1 of the material's set (the material's own
    // buffer is b0, its textures and samplers sit at the +100 and +300 shifts), ruled in
    // Specs/wind-for-every-shader.md, "Where the block binds".
    inline constexpr StringView kShaderEffectRegister = u8"register(b1, space2)";
    inline constexpr u32 kNoShaderEffectGate = ~0u;

    // An effect's table: what its block declares, laid out as its cbuffer is. A material whose
    // shader declares the effect's flag carries these values as a group of their own; the effect
    // is active while its gate is above zero, and an active effect turns its flag on.
    struct ShaderEffect
    {
        String name;                             // the cbuffer's name, and `Name.Property`
        ShaderFlags flag = ShaderFlags::None;    // the variant the effect's code is compiled in
        Array<ShaderProperty> properties;        // uniforms only, in declaration order
        u32 gateIndex = kNoShaderEffectGate;     // the property marked (gate)
        u32 uniformSize = 0;                     // the cbuffer's size

        [[nodiscard]] const ShaderProperty* Gate() const noexcept
        {
            return gateIndex < properties.Size() ? &properties[gateIndex] : nullptr;
        }
        [[nodiscard]] const ShaderProperty* FindProperty(StringView propertyName) const noexcept
        {
            for (const ShaderProperty& p : properties)
            {
                if (p.name.AsView() == propertyName)
                {
                    return &p;
                }
            }
            return nullptr;
        }
    };

    // What ParseShaderEffect found: the block's place in its source (what ExpandShaderEffects
    // replaces), its table and its errors. `present` false = no effect block.
    struct ShaderEffectBlock
    {
        bool present = false;
        usize begin = 0;   // byte offset of the `effect` line's start
        usize end = 0;     // byte offset just past the closing brace's line
        u32 firstLine = 0; // the `effect` line (1-based)
        u32 lastLine = 0;  // the closing brace's line
        ShaderEffect effect;
        Array<ShaderPropertyError> errors;

        [[nodiscard]] bool IsOk() const noexcept { return errors.IsEmpty(); }
    };

    namespace detail
    {
        [[nodiscard]] inline bool IsSpace(char8_t c) noexcept
        {
            return c == u8' ' || c == u8'\t' || c == u8'\r';
        }
        [[nodiscard]] inline bool IsIdentStart(char8_t c) noexcept
        {
            return (c >= u8'A' && c <= u8'Z') || (c >= u8'a' && c <= u8'z') || c == u8'_';
        }
        [[nodiscard]] inline bool IsIdent(char8_t c) noexcept
        {
            return IsIdentStart(c) || (c >= u8'0' && c <= u8'9');
        }

        // A cursor over one line of the block.
        struct LineReader
        {
            StringView text;
            usize at = 0;

            void SkipSpace()
            {
                while (at < text.Size() && IsSpace(text.Data()[at]))
                {
                    ++at;
                }
            }
            [[nodiscard]] bool AtEnd() const noexcept { return at >= text.Size(); }
            [[nodiscard]] char8_t Peek() const noexcept
            {
                return AtEnd() ? u8'\0' : text.Data()[at];
            }
            bool Take(char8_t c)
            {
                SkipSpace();
                if (Peek() == c)
                {
                    ++at;
                    return true;
                }
                return false;
            }
            [[nodiscard]] StringView Identifier()
            {
                SkipSpace();
                const usize start = at;
                if (!AtEnd() && IsIdentStart(Peek()))
                {
                    while (!AtEnd() && IsIdent(Peek()))
                    {
                        ++at;
                    }
                }
                return text.SubStr(start, at - start);
            }
            // A decimal number (sign, digits, optional fraction and exponent).
            bool Number(f32& out)
            {
                SkipSpace();
                const usize start = at;
                if (Peek() == u8'-' || Peek() == u8'+')
                {
                    ++at;
                }
                bool digits = false;
                while (!AtEnd() && ((Peek() >= u8'0' && Peek() <= u8'9') || Peek() == u8'.'))
                {
                    digits = digits || Peek() != u8'.';
                    ++at;
                }
                if (digits && (Peek() == u8'e' || Peek() == u8'E'))
                {
                    ++at;
                    if (Peek() == u8'-' || Peek() == u8'+')
                    {
                        ++at;
                    }
                    while (!AtEnd() && Peek() >= u8'0' && Peek() <= u8'9')
                    {
                        ++at;
                    }
                }
                if (!digits)
                {
                    at = start;
                    return false;
                }
                const Optional<f64> parsed = ParseFloat(text.SubStr(start, at - start));
                if (!parsed.HasValue())
                {
                    at = start;
                    return false;
                }
                out = static_cast<f32>(parsed.Value());
                return true;
            }
        };

        struct TypeInfoRow
        {
            StringView keyword;
            ShaderPropertyType type;
            u32 components; // 0 = a resource (no default)
            StringView hlsl;
        };
        inline constexpr TypeInfoRow kTypes[] = {
            {u8"float", ShaderPropertyType::Float, 1, u8"float"},
            {u8"float2", ShaderPropertyType::Float2, 2, u8"float2"},
            {u8"float3", ShaderPropertyType::Float3, 3, u8"float3"},
            {u8"float4", ShaderPropertyType::Float4, 4, u8"float4"},
            {u8"int", ShaderPropertyType::Int, 1, u8"int"},
            {u8"Color", ShaderPropertyType::Color, 4, u8"float4"},
            {u8"ColorHdr", ShaderPropertyType::ColorHdr, 4, u8"float4"},
            {u8"Texture", ShaderPropertyType::Texture, 0, u8"Texture2D"},
            {u8"TextureCube", ShaderPropertyType::TextureCube, 0, u8"TextureCube"},
            {u8"Sampler", ShaderPropertyType::Sampler, 0, u8"SamplerState"},
        };

        [[nodiscard]] inline const TypeInfoRow* FindType(StringView keyword) noexcept
        {
            for (const TypeInfoRow& row : kTypes)
            {
                if (row.keyword == keyword)
                {
                    return &row;
                }
            }
            return nullptr;
        }

        [[nodiscard]] inline const TypeInfoRow& RowOf(ShaderPropertyType type) noexcept
        {
            for (const TypeInfoRow& row : kTypes)
            {
                if (row.type == type)
                {
                    return row;
                }
            }
            return kTypes[0];
        }

        inline void AddError(Array<ShaderPropertyError>& errors, u32 line, StringView message)
        {
            errors.PushBack(ShaderPropertyError{line, String(message)});
        }
        inline void AddError(ShaderPropertiesBlock& block, u32 line, StringView message)
        {
            AddError(block.errors, line, message);
        }

        // What a kind of block allows. `properties` and `effect` share one parser; they differ
        // in their keyword, whether textures and samplers may be declared (an effect's values
        // live in a cbuffer of their own, so it takes uniforms only), and the `(gate)` marker.
        struct BlockGrammar
        {
            StringView keyword;
            bool resources = true;
            bool gates = false;
        };
        inline constexpr BlockGrammar kPropertiesGrammar{u8"properties", true, false};
        inline constexpr BlockGrammar kEffectGrammar{u8"effect", false, true};

        // What the shared parse leaves for the block's own rules: the text between the keyword
        // and the opening brace on the keyword's line (an effect's name and flag), and the
        // properties marked (gate).
        struct BlockParse
        {
            StringView header;
            Array<usize> gates;    // indices into the block's properties, valid gates only
            u32 gateMarks = 0;     // every marker written, valid or not
            bool closed = false;   // the block reached its closing brace
        };

        // A block's first line: a line whose first token is the keyword, outside a `//` comment,
        // at or after byte `from` (a line start). Returns its start offset, or `source.Size()`
        // when absent; `outLine` is its 1-based line.
        [[nodiscard]] inline usize FindBlockStart(StringView source, StringView keyword,
                                                  usize from, u32& outLine) noexcept
        {
            usize lineStart = 0;
            u32 line = 1;
            while (lineStart < source.Size())
            {
                usize lineEnd = lineStart;
                while (lineEnd < source.Size() && source.Data()[lineEnd] != u8'\n')
                {
                    ++lineEnd;
                }
                if (lineStart >= from)
                {
                    usize k = lineStart;
                    while (k < lineEnd && IsSpace(source.Data()[k]))
                    {
                        ++k;
                    }
                    const usize after = k + keyword.Size();
                    if (after <= lineEnd && source.SubStr(k, keyword.Size()) == keyword &&
                        (after == lineEnd || !IsIdent(source.Data()[after])))
                    {
                        outLine = line;
                        return lineStart;
                    }
                }
                lineStart = lineEnd + 1;
                ++line;
            }
            return source.Size();
        }

        // One declaration line (already stripped of a trailing `//` comment).
        inline void ParseDeclaration(ShaderPropertiesBlock& block, StringView text, u32 line,
                                     const BlockGrammar& grammar, BlockParse& parse)
        {
            LineReader r{text};
            const StringView typeWord = r.Identifier();
            const TypeInfoRow* row = FindType(typeWord);
            if (row == nullptr)
            {
                String message(u8"unknown property type '");
                message += typeWord;
                message += u8"' (float, float2, float3, float4, int, Color, ColorHdr, Texture, "
                           u8"TextureCube, Sampler)";
                AddError(block, line, message.AsView());
                return;
            }
            if (!grammar.resources && row->components == 0)
            {
                String message(u8"'");
                message += typeWord;
                message += u8"' is not a uniform: an effect takes uniforms only (float, float2, "
                           u8"float3, float4, int, Color, ColorHdr)";
                AddError(block, line, message.AsView());
                return;
            }
            const StringView name = r.Identifier();
            if (name.IsEmpty())
            {
                AddError(block, line, u8"a property needs a name after its type");
                return;
            }
            for (const ShaderProperty& existing : block.properties)
            {
                if (existing.name.AsView() == name)
                {
                    String message(u8"'");
                    message += name;
                    message += u8"' is declared twice";
                    AddError(block, line, message.AsView());
                    return;
                }
            }

            ShaderProperty p;
            p.name = String(name);
            p.type = row->type;
            p.line = line;
            // Defaults when none is written: white for a colour, black at intensity 1 for an HDR
            // colour (no glow), zero otherwise.
            if (p.type == ShaderPropertyType::Color)
            {
                p.value = Float4{1.0f, 1.0f, 1.0f, 1.0f};
            }
            else if (p.type == ShaderPropertyType::ColorHdr)
            {
                p.value = Float4{0.0f, 0.0f, 0.0f, 1.0f};
            }

            if (r.Take(u8'='))
            {
                if (row->components == 0)
                {
                    AddError(block, line, u8"a texture or sampler takes no default");
                    return;
                }
                f32 values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                u32 count = 0;
                if (r.Take(u8'('))
                {
                    do
                    {
                        if (count == 4 || !r.Number(values[count]))
                        {
                            AddError(block, line, u8"a default is a number or (a, b, ...)");
                            return;
                        }
                        ++count;
                    } while (r.Take(u8','));
                    if (!r.Take(u8')'))
                    {
                        AddError(block, line, u8"a default's ( has no )");
                        return;
                    }
                }
                else if (r.Number(values[0]))
                {
                    count = 1;
                }
                else
                {
                    AddError(block, line, u8"a default is a number or (a, b, ...)");
                    return;
                }
                // A colour may leave its alpha out (opaque).
                const bool colour = p.type == ShaderPropertyType::Color ||
                                    p.type == ShaderPropertyType::ColorHdr;
                const bool colourWithoutAlpha = colour && count == 3;
                if (count != row->components && !colourWithoutAlpha)
                {
                    String message(u8"'");
                    message += name;
                    message += u8"' is a ";
                    message += row->keyword;
                    message += u8": its default needs ";
                    message += Format(u8"{}", row->components);
                    message += row->components == 1 ? u8" value" : u8" values";
                    AddError(block, line, message.AsView());
                    return;
                }
                p.value = Float4{values[0], values[1], values[2],
                                 colourWithoutAlpha ? 1.0f : values[3]};
            }

            if (r.Take(u8'['))
            {
                if (p.type != ShaderPropertyType::Float && p.type != ShaderPropertyType::Int)
                {
                    AddError(block, line, u8"a [min, max] range is for a float or an int");
                    return;
                }
                if (!r.Number(p.rangeMin) || !r.Take(u8',') || !r.Number(p.rangeMax) ||
                    !r.Take(u8']'))
                {
                    AddError(block, line, u8"a range is [min, max]");
                    return;
                }
                if (p.rangeMin > p.rangeMax)
                {
                    AddError(block, line, u8"a range's min is above its max");
                    return;
                }
                p.hasRange = true;
            }

            // An effect's switch: `(gate)` after the default and range. Only an effect's grammar
            // reads it; in a properties block the `(` stays the missing-`;` error it always was.
            bool gate = false;
            if (grammar.gates && r.Take(u8'('))
            {
                ++parse.gateMarks; // a mistyped marker is one error, not also a missing gate
                if (r.Identifier() != StringView(u8"gate") || !r.Take(u8')'))
                {
                    AddError(block, line, u8"the only marker is (gate)");
                    return;
                }
                if (p.type != ShaderPropertyType::Float)
                {
                    AddError(block, line,
                             u8"the (gate) is a float: the effect is on while it is above zero");
                    return;
                }
                gate = true;
            }

            if (!r.Take(u8';'))
            {
                AddError(block, line, u8"a declaration ends with ;");
                return;
            }
            r.SkipSpace();
            if (!r.AtEnd())
            {
                AddError(block, line, u8"one declaration per line");
                return;
            }
            if (gate)
            {
                parse.gates.PushBack(block.properties.Size());
            }
            block.properties.PushBack(Move(p));
        }

        // Parse the block whose first line starts at `start` (line `line`): the opening brace,
        // the declarations a line at a time to the closing brace, then the layout. Fills the
        // block's place, table and errors; the header and gates go to `parse`.
        inline void ParseBlockAt(StringView source, usize start, u32 line,
                                 const BlockGrammar& grammar, ShaderPropertiesBlock& block,
                                 BlockParse& parse)
        {
            block.present = true;
            block.begin = start;
            block.firstLine = line;

            usize keywordEnd = start;
            while (keywordEnd < source.Size() && IsSpace(source.Data()[keywordEnd]))
            {
                ++keywordEnd;
            }
            keywordEnd += grammar.keyword.Size();

            // The opening brace: after the keyword on its line, or alone on a following line.
            usize at = keywordEnd;
            usize headerEnd = source.Size();
            while (at < source.Size() && source.Data()[at] != u8'{')
            {
                if (source.Data()[at] == u8'\n')
                {
                    headerEnd = headerEnd < at ? headerEnd : at;
                    ++line;
                }
                ++at;
            }
            headerEnd = headerEnd < at ? headerEnd : at;
            parse.header = source.SubStr(keywordEnd, headerEnd - keywordEnd);
            if (at >= source.Size())
            {
                String message(grammar.keyword);
                message += u8" needs a { ... } block";
                AddError(block, block.firstLine, message.AsView());
                block.end = source.Size();
                block.lastLine = line;
                return;
            }
            ++at; // past {

            // Declarations, a line at a time, to the closing brace.
            bool closed = false;
            while (at < source.Size())
            {
                usize lineEnd = at;
                while (lineEnd < source.Size() && source.Data()[lineEnd] != u8'\n')
                {
                    ++lineEnd;
                }
                StringView text = source.SubStr(at, lineEnd - at);
                // A trailing `//` comment is not part of the declaration.
                for (usize i = 0; i + 1 < text.Size(); ++i)
                {
                    if (text.Data()[i] == u8'/' && text.Data()[i + 1] == u8'/')
                    {
                        text = text.SubStr(0, i);
                        break;
                    }
                }
                // Trim.
                usize b = 0;
                usize e = text.Size();
                while (b < e && IsSpace(text.Data()[b]))
                {
                    ++b;
                }
                while (e > b && IsSpace(text.Data()[e - 1]))
                {
                    --e;
                }
                text = text.SubStr(b, e - b);
                if (!text.IsEmpty() && text.Data()[0] == u8'}')
                {
                    closed = true;
                    block.lastLine = line;
                    block.end = lineEnd < source.Size() ? lineEnd + 1 : lineEnd;
                    if (text.Size() > 1)
                    {
                        AddError(block, line, u8"nothing may follow the block's }");
                    }
                    break;
                }
                if (!text.IsEmpty())
                {
                    ParseDeclaration(block, text, line, grammar, parse);
                }
                if (lineEnd >= source.Size())
                {
                    break;
                }
                at = lineEnd + 1;
                ++line;
            }
            if (!closed)
            {
                String message(u8"the ");
                message += grammar.keyword;
                message += u8" block has no closing }";
                AddError(block, block.firstLine, message.AsView());
                block.end = source.Size();
                block.lastLine = line;
                return;
            }

            parse.closed = true;

            // Lay out: uniforms by HLSL's packing, textures t0.., samplers s0.. in declaration
            // order (what MaterialSystem assigns a material with this property list).
            u32 cursor = 0;
            u32 textures = 0;
            u32 samplers = 0;
            bool anyUniform = false;
            for (ShaderProperty& p : block.properties)
            {
                switch (p.type)
                {
                case ShaderPropertyType::Texture:
                case ShaderPropertyType::TextureCube:
                    p.binding = textures++;
                    break;
                case ShaderPropertyType::Sampler:
                    p.binding = samplers++;
                    break;
                default:
                    p.size = RowOf(p.type).components * 4u;
                    p.offset = PackCbufferMember(cursor, p.size);
                    anyUniform = true;
                    break;
                }
            }
            block.uniformSize = anyUniform ? CbufferSize(cursor) : 0u;
        }

        // An effect's header, `<Name> : <FLAG>`, from the text after the keyword on its line.
        inline void ParseEffectHeader(StringView header, u32 line, ShaderEffect& effect,
                                      Array<ShaderPropertyError>& errors)
        {
            for (usize i = 0; i + 1 < header.Size(); ++i)
            {
                if (header.Data()[i] == u8'/' && header.Data()[i + 1] == u8'/')
                {
                    header = header.SubStr(0, i);
                    break;
                }
            }
            LineReader r{header};
            const StringView name = r.Identifier();
            if (name.IsEmpty())
            {
                AddError(errors, line, u8"an effect needs a name: effect <Name> : <FLAG>");
                return;
            }
            effect.name = String(name);
            if (!r.Take(u8':'))
            {
                AddError(errors, line,
                         u8"an effect names its variant flag after a colon: "
                         u8"effect <Name> : <FLAG>");
                return;
            }
            const StringView flagWord = r.Identifier();
            bool found = false;
            for (const ShaderFlagName& entry : kShaderFlagNames)
            {
                if (entry.define == flagWord)
                {
                    effect.flag = entry.flag;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                String message(u8"unknown variant flag '");
                message += flagWord;
                message += u8"' (";
                bool first = true;
                for (const ShaderFlagName& entry : kShaderFlagNames)
                {
                    message += first ? u8"" : u8", ";
                    message += entry.define;
                    first = false;
                }
                message += u8")";
                AddError(errors, line, message.AsView());
                return;
            }
            r.SkipSpace();
            if (!r.AtEnd())
            {
                AddError(errors, line, u8"nothing may follow an effect's flag but its {");
            }
        }

        // A block's uniforms as members, in declaration order, one per line with its offset.
        inline void AppendUniformMembers(String& out, const Array<ShaderProperty>& properties)
        {
            for (const ShaderProperty& p : properties)
            {
                if (p.size == 0u)
                {
                    continue;
                }
                out += u8"    ";
                out += RowOf(p.type).hlsl;
                out += u8" ";
                out += p.name;
                out += u8"; // offset ";
                out += Format(u8"{}", p.offset);
                out += u8"\n";
            }
        }

        // A cbuffer of a block's uniforms in declaration order, which HLSL packs exactly as
        // PackCbufferMember did. Nothing when the block has none.
        inline void AppendCbuffer(String& out, StringView name, StringView registerText,
                                  const Array<ShaderProperty>& properties)
        {
            bool any = false;
            for (const ShaderProperty& p : properties)
            {
                any = any || p.size != 0u;
            }
            if (!any)
            {
                return;
            }
            out += u8"cbuffer ";
            out += name;
            out += u8" : ";
            out += registerText;
            out += u8" {\n";
            AppendUniformMembers(out, properties);
            out += u8"};\n";
        }

        // `#line` so the lines after a replaced block keep the number (and file name) their
        // author sees.
        inline void AppendLineDirective(String& out, u32 line, StringView fileName)
        {
            out += u8"#line ";
            out += Format(u8"{}", line);
            if (!fileName.IsEmpty())
            {
                out += u8" \"";
                out += fileName;
                out += u8"\"";
            }
            out += u8"\n";
        }
    }

    // Find and parse the source's properties block. Errors carry their line; the block's place
    // (begin/end, first/last line) is what ExpandShaderProperties replaces.
    [[nodiscard]] inline ShaderPropertiesBlock ParseShaderProperties(StringView source)
    {
        ShaderPropertiesBlock block;
        u32 line = 0;
        const usize start =
            detail::FindBlockStart(source, detail::kPropertiesGrammar.keyword, 0, line);
        if (start >= source.Size())
        {
            return block; // no block: a shader without material parameters
        }
        detail::BlockParse parse;
        detail::ParseBlockAt(source, start, line, detail::kPropertiesGrammar, block, parse);
        return block;
    }

    // The set-2 declarations a block stands for: the `Material` cbuffer (members in order, which
    // HLSL packs exactly as PackCbufferMember did), then each texture and sampler at its slot.
    [[nodiscard]] inline String GenerateMaterialDeclarations(const ShaderPropertiesBlock& block)
    {
        String out;
        detail::AppendCbuffer(out, u8"Material", u8"register(b0, space2)", block.properties);
        for (const ShaderProperty& p : block.properties)
        {
            if (p.size != 0u)
            {
                continue;
            }
            const bool sampler = p.type == ShaderPropertyType::Sampler;
            out += detail::RowOf(p.type).hlsl;
            out += u8" ";
            out += p.name;
            out += sampler ? u8" : register(s" : u8" : register(t";
            out += Format(u8"{}", p.binding);
            out += u8", space2);\n";
        }
        return out;
    }

    // The source as the compiler sees it: the properties block replaced by its declarations, then
    // `#line` so every line after it keeps the number (and file name) its author sees. A source
    // with no block is returned as it is. The parse is returned for its table and its errors.
    [[nodiscard]] inline ShaderPropertiesBlock ExpandShaderProperties(StringView source,
                                                                      StringView fileName,
                                                                      String& out)
    {
        ShaderPropertiesBlock block = ParseShaderProperties(source);
        if (!block.present)
        {
            out = String(source);
            return block;
        }
        out = String(source.SubStr(0, block.begin));
        out += GenerateMaterialDeclarations(block);
        detail::AppendLineDirective(out, block.lastLine + 1u, fileName);
        out += source.SubStr(block.end, source.Size() - block.end);
        return block;
    }

    // --- Effect blocks ------------------------------------------------------------------------

    // Whether the source declares an effect block at all: the cheap test the compiler makes on
    // every include before it parses one.
    [[nodiscard]] inline bool HasShaderEffect(StringView source) noexcept
    {
        u32 line = 0;
        return detail::FindBlockStart(source, detail::kEffectGrammar.keyword, 0, line) <
               source.Size();
    }

    // Find and parse the first effect block at or after byte `from` (a line start). Errors carry
    // their line, header errors first; a block with errors still reports its place.
    [[nodiscard]] inline ShaderEffectBlock ParseShaderEffect(StringView source, usize from = 0)
    {
        ShaderEffectBlock result;
        u32 line = 0;
        const usize start =
            detail::FindBlockStart(source, detail::kEffectGrammar.keyword, from, line);
        if (start >= source.Size())
        {
            return result;
        }
        ShaderPropertiesBlock body;
        detail::BlockParse parse;
        detail::ParseBlockAt(source, start, line, detail::kEffectGrammar, body, parse);

        result.present = true;
        result.begin = body.begin;
        result.end = body.end;
        result.firstLine = body.firstLine;
        result.lastLine = body.lastLine;
        detail::ParseEffectHeader(parse.header, body.firstLine, result.effect, result.errors);
        for (ShaderPropertyError& error : body.errors)
        {
            result.errors.PushBack(Move(error));
        }

        // One switch per effect: the renderer turns the variant on while it is above zero, so
        // none leaves the effect unreachable and two leave it ambiguous. (A block without its
        // braces has said what is wrong with it already.)
        if (parse.closed && parse.gateMarks == 0u)
        {
            detail::AddError(result.errors, result.firstLine,
                             u8"an effect needs one float property marked (gate): the value "
                             u8"that turns it on");
        }
        else if (parse.gates.Size() > 1u)
        {
            const ShaderProperty& second = body.properties[parse.gates[1]];
            String message(u8"an effect has one (gate); '");
            message += body.properties[parse.gates[0]].name;
            message += u8"' is it already";
            detail::AddError(result.errors, second.line, message.AsView());
        }
        if (!parse.gates.IsEmpty())
        {
            result.effect.gateIndex = static_cast<u32>(parse.gates[0]);
        }
        result.effect.properties = Move(body.properties);
        result.effect.uniformSize = body.uniformSize;
        return result;
    }

    // Every effect block in a source, in order.
    [[nodiscard]] inline Array<ShaderEffectBlock> ParseShaderEffects(StringView source)
    {
        Array<ShaderEffectBlock> blocks;
        usize from = 0;
        while (from < source.Size())
        {
            ShaderEffectBlock block = ParseShaderEffect(source, from);
            if (!block.present)
            {
                break;
            }
            from = block.end > block.begin ? block.end : source.Size();
            blocks.PushBack(Move(block));
        }
        return blocks;
    }

    // The declaration an effect stands for: a struct of its parameters (`<Name>Effect`) and a
    // ConstantBuffer of it named for the effect, at the effect binding, so the effect's code reads
    // `Wind.Strength`, as a material and a script address the value. Not a plain cbuffer: its
    // members would be global names, and a shader that declares the effect's variant would then
    // refuse a property of its own named `Speed` or `Height` (ruled in 2b-2: every custom shader
    // may opt in, rule 4). A ConstantBuffer<T> packs its struct by the cbuffer rules, so the
    // offsets are PackCbufferMember's.
    [[nodiscard]] inline String EffectStructName(StringView effectName)
    {
        String out(effectName);
        out += u8"Effect";
        return out;
    }
    [[nodiscard]] inline String GenerateEffectDeclaration(const ShaderEffect& effect)
    {
        String out;
        bool any = false;
        for (const ShaderProperty& p : effect.properties)
        {
            any = any || p.size != 0u;
        }
        if (!any)
        {
            return out;
        }
        const String structName = EffectStructName(effect.name.AsView());
        out += u8"struct ";
        out += structName;
        out += u8" {\n";
        detail::AppendUniformMembers(out, effect.properties);
        out += u8"};\n";
        out += u8"ConstantBuffer<";
        out += structName;
        out += u8"> ";
        out += effect.name;
        out += u8" : ";
        out += kShaderEffectRegister;
        out += u8";\n";
        return out;
    }

    // The source as the compiler sees it: each effect block replaced by its declaration, then
    // `#line` so every later line keeps its number and file name. A block with errors becomes an
    // `#error` per error at the error's line instead, so a compile that reaches it fails naming
    // the file and line even where the caller (an include handler) cannot report otherwise. A
    // source without a block is returned as it is. The parses are returned for their tables.
    [[nodiscard]] inline Array<ShaderEffectBlock> ExpandShaderEffects(StringView source,
                                                                      StringView fileName,
                                                                      String& out)
    {
        Array<ShaderEffectBlock> blocks = ParseShaderEffects(source);
        if (blocks.IsEmpty())
        {
            out = String(source);
            return blocks;
        }
        out = String();
        usize cursor = 0;
        for (const ShaderEffectBlock& block : blocks)
        {
            out += source.SubStr(cursor, block.begin - cursor);
            if (block.IsOk())
            {
                out += GenerateEffectDeclaration(block.effect);
            }
            else
            {
                for (const ShaderPropertyError& error : block.errors)
                {
                    detail::AppendLineDirective(out, error.line, fileName);
                    out += u8"#error effect: ";
                    out += error.message;
                    out += u8"\n";
                }
            }
            detail::AppendLineDirective(out, block.lastLine + 1u, fileName);
            cursor = block.end;
        }
        out += source.SubStr(cursor, source.Size() - cursor);
        return blocks;
    }

    // The effects a corpus declares, each once: by name, by its variant flag, or all of them.
    // The cook fills one into the pack; the shader system serves one.
    class ShaderEffectLibrary
    {
    public:
        // Adds an effect. False, with the reason in `outError`, when its name or its flag is
        // taken: a flag turns on one effect, and a name addresses one (`Wind.Strength`).
        bool Add(ShaderEffect effect, String& outError)
        {
            for (const ShaderEffect& existing : m_effects)
            {
                if (existing.name.AsView() == effect.name.AsView())
                {
                    outError = String(u8"effect '");
                    outError += effect.name;
                    outError += u8"' is declared twice";
                    return false;
                }
                if (existing.flag == effect.flag)
                {
                    outError = String(u8"effects '");
                    outError += existing.name;
                    outError += u8"' and '";
                    outError += effect.name;
                    outError += u8"' share a variant flag";
                    return false;
                }
            }
            m_effects.PushBack(Move(effect));
            return true;
        }

        [[nodiscard]] const ShaderEffect* Find(StringView name) const noexcept
        {
            for (const ShaderEffect& effect : m_effects)
            {
                if (effect.name.AsView() == name)
                {
                    return &effect;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const ShaderEffect* ForFlag(ShaderFlags flag) const noexcept
        {
            for (const ShaderEffect& effect : m_effects)
            {
                if (effect.flag == flag)
                {
                    return &effect;
                }
            }
            return nullptr;
        }

        [[nodiscard]] Span<const ShaderEffect> All() const noexcept
        {
            return Span<const ShaderEffect>(m_effects.Data(), m_effects.Size());
        }
        [[nodiscard]] usize Size() const noexcept { return m_effects.Size(); }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_effects.IsEmpty(); }
        void Clear() { m_effects.Clear(); }

    private:
        Array<ShaderEffect> m_effects;
    };
}
