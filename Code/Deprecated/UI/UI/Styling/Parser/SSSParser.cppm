// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :sss_parser partition
//
// Parses .sss stylesheet text into a StyleSheet (used internally by StyleSheetLoader).
// DrawableFactoryRegistry shares this partition: the factories use SSSParser and
// SSSParser::ParseDrawableValue calls the registry, a mutual dependency that two module partitions
// cannot express.
//
// Factories are plain function pointers, since the built-in ones capture nothing. Drawables are
// RefPtr, so the sheet's owned list and a container share them without manual reference counting. An
// @import merges into the sheet through StyleSheet::MergeFrom. ParseProperty handles drawable and
// string values itself, because a StyleValue cannot hand back an owning RefPtr<Drawable>.
// ApplyInlineStyle (for `style="..."` markup attributes) is defined in the implementation unit
// Styling/Parser/SSSParserImpl.cpp, since its body touches the View cluster.

module;
#include "Core/Prelude.h"

export module foundation.ui:sss_parser;

import foundation.core;
import foundation.image;
import foundation.vg;
import :style_property;
import :style_value;
import :style_rule;
import :style_sheet;
import :style_selector;
import :control_state;
import :thickness;
import :drawable;
import :color_drawable;
import :rounded_rect_drawable;
import :gradient_drawable;
import :state_list_drawable;
import :layer_drawable;
import :inset_drawable;
import :image_drawable;
import :nine_slice_drawable;
import :svg_drawable;
import :theme_icon_set;
import :palette;
import :sss_token;
import :sss_tokenizer;
import :style_value_parser;
import :color_functions;
import :ui_type_registry;
import :iresource_provider;

using namespace foundation::core;
namespace image = foundation::image;
namespace vg = foundation::vg;

export namespace foundation::ui
{
    class SSSParser;
    class View; // for SSSParser::ApplyInlineStyle (body in Styling/Parser/SSSParserImpl.cpp)

    /// Registry of drawable factory functions invocable from .sss stylesheets. User-extensible via
    /// Register(). Factories capture nothing, so each is a plain function pointer.
    struct DrawableFactoryRegistry
    {
        using FactoryFn = RefPtr<Drawable> (*)(SSSParser&, StyleSheet&);

        static void Register(StringView name, FactoryFn factory);
        [[nodiscard]] static FactoryFn Get(StringView name);
        static void RegisterBuiltins();
    };

    /// Parses .sss stylesheet text into a StyleSheet.
    class SSSParser
    {
    public:
        SSSParser(IAllocator& allocator, Array<Token> tokens, HashMap<String, Color>* palette,
                  HashMap<String, String>* svgRegistry,
                  HashMap<String, const image::ImageData*>* imageRegistry,
                  IResourceProvider* resourceProvider, String basePath)
            : m_allocator(&allocator), m_tokens(Move(tokens)), m_palette(palette),
              m_svgRegistry(svgRegistry),
              m_imageRegistry(imageRegistry), m_resourceProvider(resourceProvider),
              m_basePath(Move(basePath))
        {
        }

        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }

        RefPtr<StyleSheet> Parse()
        {
            m_sheetOwner = MakeRef<StyleSheet>((*m_allocator));
            m_sheet = m_sheetOwner.Get();
            m_pos = 0;

            while (!IsAtEnd())
            {
                if (Peek().Kind == TokenKind::Directive)
                    ParseDirective();
                else
                    ParseRule();
            }

            return m_sheetOwner;
        }

        /// Parse a bare declaration body (no selectors/braces) into an existing rule on `ownerSheet`.
        /// Used by ApplyInlineStyle for `style="..."` markup attributes.
        void ParseDeclarations(StyleSheet* ownerSheet, StyleRule& targetRule)
        {
            m_sheet = ownerSheet;
            m_pos = 0;
            while (!IsAtEnd())
            {
                ParseProperty(targetRule);
            }
        }

        /// Parse a `style="..."` markup attribute and apply its declared properties to `view`'s inline
        /// StyleSheet. Drawable values are owned by the inline sheet (released when the view dies). Theme
        /// variables ($name) and @-rules are not supported: inline-style values must be literal. The body
        /// is in the implementation unit, since it touches the View cluster.
        static void ApplyInlineStyle(View* view, StringView body);

        // === Value parsing (public for DrawableFactoryRegistry) ===

        /// Parse a color value: hex, named, rgb(), rgba(), $variable, or color function.
        Color ParseColorValue()
        {
            if (Peek().Kind == TokenKind::HexColor)
            {
                const Token tok = Consume();
                if (Optional<Color> c = StyleValueParser::ParseHexColor(tok.Text); c.HasValue())
                {
                    return c.Value();
                }
                return Color::White;
            }
            if (Peek().Kind == TokenKind::Variable)
            {
                const Token tok = Consume();
                return ResolveVariable(tok.Text);
            }
            if (Peek().Kind == TokenKind::Ident)
            {
                const StringView name = Peek().Text;
                if (Optional<Color> c = StyleValueParser::ParseNamedColor(name); c.HasValue())
                {
                    Consume();
                    return c.Value();
                }
                if (name == StringView(u8"rgb") || name == StringView(u8"rgba"))
                {
                    return ParseRgbFunction();
                }
                if (name == StringView(u8"lighten") || name == StringView(u8"darken") ||
                    name == StringView(u8"alpha") || name == StringView(u8"mix") ||
                    name == StringView(u8"hover") || name == StringView(u8"pressed") ||
                    name == StringView(u8"disabled") || name == StringView(u8"focused"))
                {
                    return ParseColorFunction();
                }
            }
            return Color::White;
        }

        /// Parse a color argument inside a function call (for drawable factories).
        Color ParseColorArg() { return ParseColorValue(); }

        /// A drawable's colour argument that may be `var(--name[, fallback])`: the custom property's
        /// name goes in `variable` (empty for a written colour), resolved where the drawable is
        /// drawn, and the fallback (transparent without one) or the written colour is returned.
        Color ParseColorArgOrVar(String& variable)
        {
            variable = String();
            if (Peek().Kind == TokenKind::Ident && Peek().Text == StringView(u8"var") &&
                m_pos + 1 < static_cast<i32>(m_tokens.Size()) &&
                m_tokens[static_cast<usize>(m_pos + 1)].Kind == TokenKind::LParen)
            {
                Consume(); // var
                Consume(); // (
                variable = String(ConsumeIdent());
                Color fallback = Color::Transparent;
                if (MatchComma())
                    fallback = ParseColorValue();
                Expect(TokenKind::RParen);
                return fallback;
            }
            return ParseColorValue();
        }

        /// Parse 1-4 numbers into per-corner radii: one value =
        /// uniform; four = top-left, top-right, bottom-right, bottom-left (CSS order).
        vg::CornerRadii ParseCornerRadiiValue()
        {
            f32 values[4] = {};
            i32 count = 0;
            while (count < 4 && Peek().Kind == TokenKind::Number)
                values[count++] = ParseFloatValue();
            if (count >= 4)
                return vg::CornerRadii(values[0], values[1], values[2], values[3]);
            return vg::CornerRadii(count > 0 ? values[0] : 0.0f);
        }

        /// 1, 2 or 4 numbers as CSS writes a box's sides (padding, a drawable's border widths).
        Thickness ParseThicknessValue()
        {
            f32 values[4] = {};
            i32 count = 0;
            while (count < 4 && Peek().Kind == TokenKind::Number)
                values[count++] = ParseFloatValue();
            return StyleValueParser::ParseThickness(values, count);
        }


        /// Parse a float value (number, possibly followed by %).
        f32 ParseFloatValue()
        {
            if (Peek().Kind == TokenKind::Number)
            {
                const Token tok = Consume();
                f32 val = tok.NumericValue;
                if (Peek().Kind == TokenKind::Percent)
                {
                    Consume();
                    val /= 100.0f;
                }
                return val;
            }
            if (Peek().Kind == TokenKind::Variable)
            {
                Consume(); // variable as float not supported yet
                return 0.0f;
            }
            return 0.0f;
        }

        /// Parse a drawable value: a factory call, or a color literal that becomes a ColorDrawable.
        RefPtr<Drawable> ParseDrawableValue(StyleSheet& sheet)
        {
            if (Peek().Kind == TokenKind::Ident)
            {
                if (DrawableFactoryRegistry::FactoryFn factory =
                        DrawableFactoryRegistry::Get(Peek().Text))
                {
                    Consume(); // function name
                    Expect(TokenKind::LParen);
                    RefPtr<Drawable> d = factory(*this, sheet);
                    Expect(TokenKind::RParen);
                    return d;
                }
            }
            // Color literal as ColorDrawable
            const Color color = ParseColorValue();
            RefPtr<ColorDrawable> d = MakeRef<ColorDrawable>((*m_allocator), color);
            sheet.OwnDrawable(d);
            return d;
        }

        /// Check if next token is a comma and consume it.
        bool MatchComma()
        {
            if (Peek().Kind == TokenKind::Comma)
            {
                Consume();
                return true;
            }
            return false;
        }

        /// Check if we're at a closing paren.
        [[nodiscard]] bool IsAtRParen() { return Peek().Kind == TokenKind::RParen; }

        /// Peek at keyword arg name (e.g., "radius" in "radius=6").
        StringView PeekKeywordArg()
        {
            if (Peek().Kind == TokenKind::Ident && m_pos + 1 < static_cast<i32>(m_tokens.Size()) &&
                m_tokens[static_cast<usize>(m_pos + 1)].Kind == TokenKind::Equals)
                return Peek().Text;
            return u8"";
        }

        /// Consume keyword arg name and equals sign.
        void ConsumeKeywordArg()
        {
            Consume(); // name
            Consume(); // =
        }

        /// Peek at current ident text without consuming.
        StringView PeekIdent()
        {
            if (Peek().Kind == TokenKind::Ident)
                return Peek().Text;
            return u8"";
        }

        /// Check if next token is a number.
        [[nodiscard]] bool PeekIsNumber() { return Peek().Kind == TokenKind::Number; }

        /// An angle in degrees: a number with deg (or none), turn or rad.
        f32 ParseAngleDegrees()
        {
            const Token tok = Consume();
            if (tok.UnitSuffix == StringView(u8"turn"))
                return tok.NumericValue * 360.0f;
            if (tok.UnitSuffix == StringView(u8"rad"))
                return tok.NumericValue * kRadToDeg;
            return tok.NumericValue;
        }

        /// A gradient's colour stops, each `<color> [<offset>%]` (a colour may be a var()), separated
        /// by commas, up to a keyword argument or the closing parenthesis; `variables` gets each
        /// stop's custom property name, or nothing when every colour is written. Offsets left out are spread evenly
        /// between their neighbours, the first at 0 and the last at 1, as CSS spreads them.
        Array<vg::GradientStop> ParseGradientStops(Array<String>& variables)
        {
            Array<vg::GradientStop> stops;
            Array<bool> placed;
            variables.Clear();
            bool anyVariable = false;
            while (!IsAtRParen() && PeekKeywordArg().IsEmpty())
            {
                vg::GradientStop stop;
                String variable;
                stop.color = ParseColorArgOrVar(variable);
                anyVariable = anyVariable || !variable.IsEmpty();
                variables.PushBack(Move(variable));
                const bool hasOffset = Peek().Kind == TokenKind::Number;
                if (hasOffset)
                    stop.offset = ParseFloatValue();
                stops.PushBack(stop);
                placed.PushBack(hasOffset);
                if (!MatchComma())
                    break;
            }
            if (!anyVariable)
                variables.Clear(); // written colours only: no names to resolve
            if (stops.IsEmpty())
                return stops;
            if (!placed[0])
            {
                stops[0].offset = 0.0f;
                placed[0] = true;
            }
            if (!placed.Back())
            {
                stops.Back().offset = 1.0f;
                placed.Back() = true;
            }
            for (usize i = 1; i < stops.Size(); ++i)
            {
                if (placed[i])
                    continue;
                usize next = i;
                while (!placed[next])
                    ++next;
                const f32 from = stops[i - 1].offset;
                const f32 step = (stops[next].offset - from) / static_cast<f32>(next - i + 1);
                for (usize j = i; j < next; ++j)
                {
                    stops[j].offset = from + step * static_cast<f32>(j - i + 1);
                    placed[j] = true;
                }
            }
            return stops;
        }

        /// Resolve a registered SVG by name. Returns the SVG text or empty.
        Optional<StringView> ResolveSvg(StringView name)
        {
            if (const String* s = m_svgRegistry->Find(String(name)))
            {
                return s->AsView();
            }
            return {};
        }

        /// Resolve a registered image by name. Returns the image data or null.
        const image::ImageData* ResolveImage(StringView name)
        {
            if (const image::ImageData* const* img = m_imageRegistry->Find(String(name)))
            {
                return *img;
            }
            return nullptr;
        }

        StringView ConsumeIdent()
        {
            if (Peek().Kind == TokenKind::Ident)
                return Consume().Text;
            return u8"";
        }

    private:
        // === Directives ===

        void ParseDirective()
        {
            const Token dir = Consume();
            if (dir.Text == StringView(u8"@palette"))
                ParsePaletteDirective();
            else if (dir.Text == StringView(u8"@icon"))
                ParseIconDirective();
            else if (dir.Text == StringView(u8"@image"))
                ParseImageDirective();
            else if (dir.Text == StringView(u8"@import"))
                ParseImportDirective();
            else if (dir.Text == StringView(u8"@keyframes"))
                ParseKeyframesDirective();
            else
                SkipUntilSemicolon();
        }

        void ParseKeyframesDirective()
        {
            // @keyframes name { from { ... } 50% { ... } to { ... } }: a frame's selector is from
            // (0%), to (100%) or a percentage, several separated by commas sharing its block.
            RefPtr<KeyframesRule> keyframes = MakeRef<KeyframesRule>(*m_allocator);
            keyframes->Name = String(ConsumeIdent());
            Expect(TokenKind::LBrace);
            while (!IsAtEnd() && Peek().Kind != TokenKind::RBrace)
            {
                Array<f32> offsets;
                for (;;)
                {
                    if (Peek().Kind == TokenKind::Ident)
                    {
                        const StringView word = Consume().Text;
                        if (word == StringView(u8"from"))
                            offsets.PushBack(0.0f);
                        else if (word == StringView(u8"to"))
                            offsets.PushBack(1.0f);
                    }
                    else if (Peek().Kind == TokenKind::Number)
                    {
                        offsets.PushBack(Clamp(ParseFloatValue(), 0.0f, 1.0f)); // 50% reads as 0.5
                    }
                    else
                    {
                        break;
                    }
                    if (!MatchComma())
                        break;
                }
                RefPtr<StyleRule> declarations = MakeRef<StyleRule>(*m_allocator);
                Expect(TokenKind::LBrace);
                while (!IsAtEnd() && Peek().Kind != TokenKind::RBrace)
                    ParseProperty(*declarations);
                Expect(TokenKind::RBrace);
                if (offsets.IsEmpty())
                    continue; // a frame with no selector this reads: dropped
                for (const f32 offset : offsets)
                {
                    usize at = keyframes->Frames.Size();
                    while (at > 0 && keyframes->Frames[at - 1].Offset > offset)
                        --at;
                    keyframes->Frames.Insert(at, KeyframesRule::Frame{offset, declarations});
                }
            }
            Expect(TokenKind::RBrace);
            m_sheet->AddKeyframes(Move(keyframes));
        }

        void ParsePaletteDirective()
        {
            // @palette name { ... }  /  @palette name extends parent { ... }
            ConsumeIdent(); // palette name (read but unused: there is one palette)

            if (Peek().Kind == TokenKind::Extends)
            {
                Consume();      // eat "extends"
                ConsumeIdent(); // parent name (base palette already loaded)
            }

            Expect(TokenKind::LBrace);
            while (!IsAtEnd() && Peek().Kind != TokenKind::RBrace)
            {
                const StringView varName = ConsumeIdent();
                Expect(TokenKind::Colon);
                const Color color = ParseColorValue();
                m_palette->InsertOrAssign(String(varName), color);
                MatchSemicolon();
            }
            Expect(TokenKind::RBrace);
        }

        void ParseIconDirective()
        {
            // @icon name "path"; registers an SVG loaded from a file through the resource provider.
            const StringView name = ConsumeIdent();
            const StringView path = ConsumeString();
            MatchSemicolon();

            if (m_resourceProvider != nullptr && path.Size() > 0)
            {
                const String resolvedPath = ResolvePath(path);
                String svgText;
                if (m_resourceProvider->LoadText(resolvedPath.AsView(), svgText))
                    m_svgRegistry->InsertOrAssign(String(name), Move(svgText));
            }
        }

        void ParseImageDirective()
        {
            // @image name "path";
            const StringView name = ConsumeIdent();
            const StringView path = ConsumeString();
            MatchSemicolon();

            if (m_resourceProvider != nullptr && path.Size() > 0)
            {
                const String resolvedPath = ResolvePath(path);
                if (const image::ImageData* imageData =
                        m_resourceProvider->LoadImage(resolvedPath.AsView()))
                    m_imageRegistry->InsertOrAssign(String(name), imageData);
            }
        }

        void ParseImportDirective()
        {
            // @import "path.sss";
            const StringView path = ConsumeString();
            MatchSemicolon();

            if (m_resourceProvider != nullptr && path.Size() > 0)
            {
                const String resolvedPath = ResolvePath(path);
                String importText;
                if (m_resourceProvider->LoadText(resolvedPath.AsView(), importText))
                {
                    // Tokenize and parse the imported file, sharing our palette/registries.
                    Tokenizer importTokenizer(importText.AsView());
                    Array<Token> importTokens;
                    importTokenizer.TokenizeAll(importTokens);

                    // Compute base path for the import (prefix up to and including the last slash).
                    String importBase;
                    const StringView rp = resolvedPath.AsView();
                    bool found = false;
                    usize lastSlash = 0;
                    for (usize i = 0; i < rp.Size(); ++i)
                    {
                        if (rp[i] == u8'/' || rp[i] == u8'\\')
                        {
                            lastSlash = i;
                            found = true;
                        }
                    }
                    if (found)
                        importBase = String(rp.SubStr(0, lastSlash + 1));

                    SSSParser importParser(*m_allocator, Move(importTokens), m_palette, m_svgRegistry,
                                           m_imageRegistry, m_resourceProvider, Move(importBase));
                    RefPtr<StyleSheet> importSheet = importParser.Parse();
                    m_sheet->MergeFrom(*importSheet);
                }
            }
        }

        // === Rules ===

        /// True when the current token touches the previous one (no whitespace between): the
        /// tokenizer drops whitespace, so the descendant combinator is recovered from positions.
        [[nodiscard]] bool TouchesPrevious() const
        {
            if (m_pos <= 0 || m_pos >= static_cast<i32>(m_tokens.Size()))
                return false;
            const Token& prev = m_tokens[static_cast<usize>(m_pos - 1)];
            const Token& cur = m_tokens[static_cast<usize>(m_pos)];
            return prev.Line == cur.Line &&
                   cur.Column == prev.Column + static_cast<i32>(prev.Text.Size());
        }

        [[nodiscard]] bool PeekStartsCompound() const
        {
            const TokenKind k = Peek().Kind;
            return k == TokenKind::Ident || k == TokenKind::ClassSelector ||
                   k == TokenKind::HexColor || k == TokenKind::PseudoState;
        }

        /// One compound of a selector chain: `[Type][#id][.class]*[:pseudo-class]*`. The
        /// pseudo-element (`::part[:state]*`) is only read for the subject (`allowPseudoElement`).
        void ParseCompound(SelectorCompound& compound, Optional<String>* pseudoElement,
                           ControlState& pseudoElementState, bool& hasPseudoElementState)
        {
            // Type selector. An unknown type name is consumed and marks the compound as
            // matching nothing (rather than being mistaken for a property name).
            if (Peek().Kind == TokenKind::Ident)
            {
                const StringView typeName = Consume().Text;
                if (const TypeInfo* type = UITypeRegistry::Resolve(typeName))
                    compound.ViewType = type;
                else
                    compound.UnknownType = true;
            }

            bool consumedAny = compound.ViewType != nullptr || compound.UnknownType;
            ControlState state = ControlState::Normal;
            bool hasState = false;
            for (;;)
            {
                // A part joins this compound when it touches the previous token (no
                // whitespace) or starts the compound; whitespace before it is the descendant
                // combinator, handled by the caller.
                const bool joins = !consumedAny || TouchesPrevious();
                if (Peek().Kind == TokenKind::HexColor && joins)
                {
                    const Token id = Consume();
                    compound.Id = String(id.Text.SubStr(1, id.Text.Size() - 1)); // skip #
                    consumedAny = true;
                    continue;
                }
                if (Peek().Kind == TokenKind::ClassSelector && joins)
                {
                    const Token cls = Consume();
                    compound.StyleClasses.PushBack(String(cls.Text.SubStr(1, cls.Text.Size() - 1)));
                    consumedAny = true;
                    continue;
                }
                if (Peek().Kind == TokenKind::PseudoState && joins)
                {
                    consumedAny = true;
                    const Token ps = Consume();
                    ApplyPseudoClass(ps.Text.SubStr(1, ps.Text.Size() - 1), state, hasState,
                                     compound.Structural);
                    continue;
                }
                break;
            }

            // Pseudo-element (::thumb, ::track, ...). The tokenizer produces :: as Colon +
            // PseudoState(:name), since the second : followed by a letter reads as a PseudoState.
            if (pseudoElement != nullptr && Peek().Kind == TokenKind::Colon &&
                m_pos + 1 < static_cast<i32>(m_tokens.Size()) &&
                m_tokens[static_cast<usize>(m_pos + 1)].Kind == TokenKind::PseudoState)
            {
                Consume();                  // first : (Colon)
                const Token ps = Consume(); // :name (PseudoState)
                *pseudoElement = String(ps.Text.SubStr(1, ps.Text.Size() - 1));
                // Allow :state after ::pseudo (e.g., ::tab:hover)
                while (Peek().Kind == TokenKind::PseudoState)
                {
                    const Token st = Consume();
                    ApplyPseudoClass(st.Text.SubStr(1, st.Text.Size() - 1), pseudoElementState,
                                     hasPseudoElementState, compound.Structural);
                }
            }
            if (hasState)
                compound.State = state;
        }

        void ParseRule()
        {
            // Selector chain: compound (` ` | `>`) compound ... { ... }
            RefPtr<StyleRule> rule = MakeRef<StyleRule>((*m_allocator));

            Array<SelectorCompound> compounds;
            Array<bool> childCombinator; // [i]: compound i is the direct parent of i+1
            Optional<String> pseudoElement;
            ControlState partState = ControlState::Normal;
            bool hasPartState = false;
            for (;;)
            {
                SelectorCompound compound;
                ParseCompound(compound, &pseudoElement, partState, hasPartState);
                compounds.PushBack(Move(compound));
                if (pseudoElement.HasValue())
                    break; // a pseudo-element ends the chain (it is the subject's)
                if (Peek().Kind == TokenKind::Greater)
                {
                    Consume();
                    childCombinator.PushBack(true);
                    continue;
                }
                if (PeekStartsCompound() && !TouchesPrevious())
                {
                    childCombinator.PushBack(false);
                    continue;
                }
                break;
            }

            // The last compound is the subject; the rest are ancestors, nearest first.
            const usize subjectIndex = compounds.Size() - 1;
            SelectorCompound& subject = compounds[subjectIndex];
            rule->Selector.ViewType = subject.ViewType;
            rule->Selector.UnknownType = subject.UnknownType;
            rule->Selector.StyleClasses = Move(subject.StyleClasses);
            rule->Selector.Id = subject.Id;
            rule->Selector.Structural = subject.Structural;
            if (subject.State.HasValue() || hasPartState)
            {
                ControlState state = subject.State.HasValue() ? subject.State.Value()
                                                              : ControlState::Normal;
                if (hasPartState)
                    state |= partState;
                rule->Selector.State = state;
            }
            if (pseudoElement.HasValue())
                rule->Selector.PseudoElement = pseudoElement;
            for (usize i = 0; i < subjectIndex; ++i)
            {
                // AddAncestor prepends: feed outermost first so [0] ends up nearest.
                rule->Selector.AddAncestor(Move(compounds[i]), childCombinator[i]);
            }

            // Property block
            Expect(TokenKind::LBrace);
            while (!IsAtEnd() && Peek().Kind != TokenKind::RBrace)
                ParseProperty(*rule);
            Expect(TokenKind::RBrace);

            m_sheet->AddRule(Move(rule));
        }

        void ParseProperty(StyleRule& rule)
        {
            const StringView propName = ConsumeIdent();
            Expect(TokenKind::Colon);

            // `--name: value`: a custom property, typed by its literal.
            if (propName.Size() > 2 && propName[0] == u8'-' && propName[1] == u8'-')
            {
                rule.SetCustom(propName, ParseCustomValue());
                MatchSemicolon();
                return;
            }

            // `background-color` stores Background as a raw color (as CSS does), for controls that
            // resolve it through ResolveStyleColor (ToastCard); `background:` always builds a drawable.
            if (propName == StringView(u8"background-color"))
            {
                rule.Set(StyleProperty::Background, ParseColorValue());
                MatchSemicolon();
                return;
            }

            const Optional<StyleProperty> prop = ResolvePropertyName(propName);
            if (!prop.HasValue())
            {
                // Unknown property: skip to the semicolon
                SkipUntilSemicolonOrBrace();
                return;
            }
            const StyleProperty p = prop.Value();

            // The cascade keywords and variable references apply to every property.
            if (Peek().Kind == TokenKind::Ident)
            {
                const StringView word = Peek().Text;
                if (word == StringView(u8"inherit"))
                {
                    Consume();
                    rule.SetValue(p, StyleValue::Inherit());
                    MatchSemicolon();
                    return;
                }
                if (word == StringView(u8"initial"))
                {
                    Consume();
                    rule.SetValue(p, StyleValue::Initial());
                    MatchSemicolon();
                    return;
                }
                if (word == StringView(u8"var"))
                {
                    rule.SetValue(p, ParseVarReference(p));
                    MatchSemicolon();
                    return;
                }
            }

            // String-valued properties (font-family, etc) skip the StyleValue wrapper.
            if (IsStringProperty(p))
            {
                const StringView s = ParseStringOrIdent();
                if (s.Size() > 0)
                    rule.Set(p, s);
                MatchSemicolon();
                return;
            }

            // Drawable properties own a RefPtr directly, so they are handled here rather than in
            // ParseStyleValue.
            if (IsDrawableProperty(p))
            {
                RefPtr<Drawable> d = ParseDrawableValue(*m_sheet);
                if (d)
                    rule.Set(p, Move(d));
                MatchSemicolon();
                return;
            }

            const StyleValue value = ParseStyleValue(p);
            if (!value.IsNone())
                rule.SetValue(p, value);

            MatchSemicolon();
        }

        /// The typed value of a property, or a var()/keyword. Used for values and for a
        /// var() fallback (which is written in the property's own syntax).
        StyleValue ParseTypedValue(StyleProperty p)
        {
            if (p == StyleProperty::COUNT) // a custom property's fallback: typed by its literal
                return ParseCustomValue();
            if (Peek().Kind == TokenKind::Ident)
            {
                const StringView word = Peek().Text;
                if (word == StringView(u8"inherit"))
                {
                    Consume();
                    return StyleValue::Inherit();
                }
                if (word == StringView(u8"initial"))
                {
                    Consume();
                    return StyleValue::Initial();
                }
                if (word == StringView(u8"var"))
                    return ParseVarReference(p);
            }
            if (IsStringProperty(p))
            {
                const StringView str = ParseStringOrIdent();
                return str.Size() > 0 ? StyleValue::StringRef(str) : StyleValue::None();
            }
            if (IsDrawableProperty(p))
            {
                RefPtr<Drawable> d = ParseDrawableValue(*m_sheet);
                return d ? StyleValue::DrawableRef(Move(d)) : StyleValue::None();
            }
            return ParseStyleValue(p);
        }

        /// `var(--name[, fallback])` where the fallback is in the property's own syntax.
        StyleValue ParseVarReference(StyleProperty p)
        {
            Consume(); // var
            Expect(TokenKind::LParen);
            const StringView name = ConsumeIdent();
            StyleValue fallback;
            if (MatchComma())
                fallback = ParseTypedValue(p);
            Expect(TokenKind::RParen);
            return StyleValue::VariableRef(*m_allocator, name, fallback);
        }

        /// A custom property's value, typed by its literal: a color (hex, $palette, name,
        /// function), a drawable factory call, a length/number, a string, a bool, or another
        /// var() reference.
        StyleValue ParseCustomValue()
        {
            switch (Peek().Kind)
            {
            case TokenKind::HexColor:
            case TokenKind::Variable:
                return StyleValue::ColorVal(ParseColorValue());
            case TokenKind::Number:
                return ParseLengthOrFloat();
            case TokenKind::StringLit:
                return StyleValue::StringRef(Consume().Text);
            case TokenKind::BoolLit:
                return StyleValue::BoolVal(Consume().Text == StringView(u8"true"));
            case TokenKind::Ident:
            {
                const StringView name = Peek().Text;
                if (name == StringView(u8"var"))
                    return ParseVarReference(StyleProperty::COUNT);
                if (name == StringView(u8"calc"))
                    return ParseLengthOrFloat();
                if (DrawableFactoryRegistry::Get(name) != nullptr)
                {
                    RefPtr<Drawable> d = ParseDrawableValue(*m_sheet);
                    return d ? StyleValue::DrawableRef(Move(d)) : StyleValue::None();
                }
                if (StyleValueParser::ParseNamedColor(name).HasValue() ||
                    name == StringView(u8"rgb") || name == StringView(u8"rgba") ||
                    name == StringView(u8"lighten") || name == StringView(u8"darken") ||
                    name == StringView(u8"alpha") || name == StringView(u8"mix") ||
                    name == StringView(u8"hover") || name == StringView(u8"pressed") ||
                    name == StringView(u8"disabled") || name == StringView(u8"focused"))
                    return StyleValue::ColorVal(ParseColorValue());
                return StyleValue::StringRef(Consume().Text);
            }
            default:
                SkipUntilSemicolonOrBrace();
                return StyleValue::None();
            }
        }

        /// One length term: a number with an optional unit suffix (px/dp/pt/em) or a trailing
        /// `%`. A bare, px, dp or pt number is an absolute length; em and % are relative.
        Unit ParseLengthTerm()
        {
            if (Peek().Kind != TokenKind::Number)
                return Unit::Dp(0.0f);
            const Token tok = Consume();
            if (Peek().Kind == TokenKind::Percent)
            {
                Consume();
                return Unit::Percent(tok.NumericValue);
            }
            return StyleValueParser::ParseUnit(tok.NumericValue, tok.UnitSuffix);
        }

        /// A number stays a Float (dp; px and pt convert at resolve time) unless it needs a
        /// reference (em, %, or a calc()), in which case it becomes a Length.
        StyleValue ParseLengthOrFloat()
        {
            if (Peek().Kind == TokenKind::Ident && Peek().Text == StringView(u8"calc"))
            {
                Consume();
                Expect(TokenKind::LParen);
                Unit result = ParseLengthTerm();
                // One nesting level: a chain of `+ term` / `- term` (a `-20dp` written without
                // the space arrives as a negative Number and adds).
                for (;;)
                {
                    if (Peek().Kind == TokenKind::Plus)
                    {
                        Consume();
                        result = result + ParseLengthTerm();
                    }
                    else if (Peek().Kind == TokenKind::Minus)
                    {
                        Consume();
                        result = result - ParseLengthTerm();
                    }
                    else if (Peek().Kind == TokenKind::Number)
                    {
                        result = result + ParseLengthTerm();
                    }
                    else
                    {
                        break;
                    }
                }
                Expect(TokenKind::RParen);
                return StyleValue::LengthVal(result);
            }
            if (Peek().Kind != TokenKind::Number)
                return StyleValue::None();
            const Token tok = Peek();
            const bool percent = m_pos + 1 < static_cast<i32>(m_tokens.Size()) &&
                                 m_tokens[static_cast<usize>(m_pos + 1)].Kind == TokenKind::Percent;
            if (percent || tok.UnitSuffix == StringView(u8"em"))
                return StyleValue::LengthVal(ParseLengthTerm());
            return StyleValue::FloatVal(ParseFloatValue());
        }

        // === Private value parsing ===

        StyleValue ParseStyleValue(StyleProperty prop)
        {
            if (IsColorProperty(prop))
                return StyleValue::ColorVal(ParseColorValue());
            if (prop == StyleProperty::BoxShadow)
                return ParseBoxShadowValue();
            if (prop == StyleProperty::Transition)
                return ParseTransitionValue();
            if (prop == StyleProperty::Filter || prop == StyleProperty::BackdropFilter)
                return ParseFilterValue();
            if (prop == StyleProperty::Transform)
                return ParseTransformValue();
            if (prop == StyleProperty::TransformOrigin)
                return ParseTransformOriginValue();
            if (prop == StyleProperty::Animation)
                return ParseAnimationValue();
            if (prop == StyleProperty::TextShadow || prop == StyleProperty::TextOutline ||
                prop == StyleProperty::TextGlow)
                return ParseTextEffectValue(prop);
            if (IsSizeSpecProperty(prop) && Peek().Kind == TokenKind::Ident &&
                Peek().Text != StringView(u8"calc"))
            {
                // `match` / `wrap` (and the CSS spellings) travel as a String; the view's
                // effective-layout refresh maps them to the SizeSpec kind.
                return StyleValue::StringRef(Consume().Text);
            }
            if (IsThicknessProperty(prop))
                return StyleValue::ThicknessVal(ParseThicknessValue());
            if (IsBoolProperty(prop))
            {
                if (Peek().Kind == TokenKind::BoolLit)
                    return StyleValue::BoolVal(Consume().Text == StringView(u8"true"));
                return StyleValue::None();
            }
            // Default: a number (Float), or a relative length (%, em or calc, which become a Length).
            return ParseLengthOrFloat();
        }

        /// `transition: none | <entry> {, <entry>}` with `<entry> = <property> | all,
        /// <duration> [<easing>] [<delay>]`. Times take `ms` or `s` (a bare number is ms);
        /// easings: linear, ease, ease-in, ease-out, ease-in-out. An unknown property name
        /// drops its entry; `none` yields an empty list, which still overrides a UA `all`.
        StyleValue ParseTransitionValue()
        {
            RefPtr<TransitionList> list = MakeRef<TransitionList>(*m_allocator);
            if (Peek().Kind == TokenKind::Ident && Peek().Text == StringView(u8"none"))
            {
                Consume();
                return StyleValue::TransitionsRef(Move(list));
            }
            while (Peek().Kind == TokenKind::Ident)
            {
                const StringView name = Consume().Text;
                TransitionSpec spec;
                bool known = true;
                if (name == StringView(u8"all"))
                {
                    spec.Property = StyleProperty::COUNT;
                }
                else if (Optional<StyleProperty> p = ResolvePropertyName(name); p.HasValue())
                {
                    spec.Property = p.Value();
                }
                else
                {
                    known = false;
                }
                if (Peek().Kind == TokenKind::Number)
                {
                    spec.Duration = ParseTimeSeconds();
                }
                if (Peek().Kind == TokenKind::Ident)
                {
                    spec.Easing = ParseTimingFunction();
                }
                if (Peek().Kind == TokenKind::Number)
                {
                    spec.Delay = ParseTimeSeconds();
                }
                if (known)
                {
                    list->Specs.PushBack(spec);
                }
                if (!MatchComma())
                {
                    break;
                }
            }
            return StyleValue::TransitionsRef(Move(list));
        }

        /// `none` or a list of filter functions: blur(<sigma>) in dp, grayscale(<amount>),
        /// saturate(<amount>) and brightness(<amount>), each amount a number or a percentage
        /// (grayscale(1) is saturate(0)). They combine: blurs add, the colour factors multiply. A
        /// backdrop-filter draws only the blur. An unknown function leaves the property unset.
        StyleValue ParseFilterValue()
        {
            StyleFilter filter;
            if (Peek().Kind == TokenKind::Ident && Peek().Text == StringView(u8"none"))
            {
                Consume();
                return StyleValue::FilterVal(filter);
            }
            bool any = false;
            while (Peek().Kind == TokenKind::Ident)
            {
                const StringView name = Consume().Text;
                if (Peek().Kind != TokenKind::LParen)
                    return StyleValue::None();
                Consume();
                const bool hasAmount = Peek().Kind == TokenKind::Number;
                const f32 amount = hasAmount ? Max(0.0f, ParseFloatValue()) : 0.0f;
                if (name == StringView(u8"blur"))
                    filter.Blur += amount;
                else if (name == StringView(u8"grayscale"))
                    filter.Saturation *= 1.0f - Min(hasAmount ? amount : 1.0f, 1.0f);
                else if (name == StringView(u8"saturate"))
                    filter.Saturation *= hasAmount ? amount : 1.0f;
                else if (name == StringView(u8"brightness"))
                    filter.Brightness *= hasAmount ? amount : 1.0f;
                else
                    return StyleValue::None();
                if (Peek().Kind != TokenKind::RParen)
                    return StyleValue::None();
                Consume();
                any = true;
            }
            return any ? StyleValue::FilterVal(filter) : StyleValue::None();
        }

        /// `text-shadow: <x> <y> [blur] [color]`, `text-outline: <width> [color]`, `text-glow:
        /// <radius> [color]`, or `none` (a zero Shadow, so it turns off what a parent set). The
        /// outline's width is the shadow's spread and the glow's radius its blur.
        StyleValue ParseTextEffectValue(StyleProperty prop)
        {
            BoxShadow effect;
            effect.Color = prop == StyleProperty::TextGlow ? Color{1.0f, 1.0f, 1.0f, 1.0f}
                           : prop == StyleProperty::TextOutline ? Color{0.0f, 0.0f, 0.0f, 1.0f}
                                                                : Color{0.0f, 0.0f, 0.0f, 0.5f};
            if (Peek().Kind == TokenKind::Ident && Peek().Text == StringView(u8"none"))
            {
                Consume();
                effect.Color = Color{0.0f, 0.0f, 0.0f, 0.0f};
                return StyleValue::ShadowVal(effect);
            }
            f32 lengths[3] = {};
            i32 count = 0;
            while (count < 3 && Peek().Kind == TokenKind::Number)
                lengths[count++] = ParseFloatValue();
            const i32 wanted = prop == StyleProperty::TextShadow ? 2 : 1;
            if (count < wanted || (prop != StyleProperty::TextShadow && count > 1))
                return StyleValue::None();
            if (prop == StyleProperty::TextShadow)
            {
                effect.OffsetX = lengths[0];
                effect.OffsetY = lengths[1];
                effect.Blur = count > 2 ? Max(0.0f, lengths[2]) : 0.0f;
            }
            else if (prop == StyleProperty::TextOutline)
                effect.Spread = Max(0.0f, lengths[0]);
            else
                effect.Blur = Max(0.0f, lengths[0]);
            if (Peek().Kind != TokenKind::Semicolon && Peek().Kind != TokenKind::RBrace)
                effect.Color = ParseColorValue();
            return StyleValue::ShadowVal(effect);
        }

        /// `transform: none` or a list of transform functions, composed in the order written as
        /// CSS composes them: translate(x[, y]), translateX, translateY (lengths: dp, px, or a percent
        /// of the view), scale(s[, t]), scaleX, scaleY, rotate(<angle>), skew(<ax>[, <ay>]),
        /// skewX, skewY. A list keeps its first StyleTransform::kMaxFunctions; an unknown function
        /// leaves the property unset.
        StyleValue ParseTransformValue()
        {
            StyleTransform transform;
            if (Peek().Kind == TokenKind::Ident && Peek().Text == StringView(u8"none"))
            {
                Consume();
                return StyleValue::TransformVal(transform);
            }
            const auto factor = [this]() { return Peek().Kind == TokenKind::Number ? Consume().NumericValue : 1.0f; };
            const auto angle = [this]() { return PeekIsNumber() ? ParseAngleDegrees() : 0.0f; };
            bool any = false;
            while (Peek().Kind == TokenKind::Ident)
            {
                const StringView name = Consume().Text;
                if (Peek().Kind != TokenKind::LParen)
                    return StyleValue::None();
                Consume();
                TransformFunction f;
                if (name == StringView(u8"translate"))
                {
                    f.Kind = TransformFunctionKind::Translate;
                    f.X = ParseLengthTerm();
                    if (MatchComma())
                        f.Y = ParseLengthTerm();
                }
                else if (name == StringView(u8"translateX"))
                {
                    f.Kind = TransformFunctionKind::Translate;
                    f.X = ParseLengthTerm();
                }
                else if (name == StringView(u8"translateY"))
                {
                    f.Kind = TransformFunctionKind::Translate;
                    f.Y = ParseLengthTerm();
                }
                else if (name == StringView(u8"scale"))
                {
                    f.Kind = TransformFunctionKind::Scale;
                    f.A = factor();
                    f.B = MatchComma() ? factor() : f.A;
                }
                else if (name == StringView(u8"scaleX") || name == StringView(u8"scaleY"))
                {
                    f.Kind = TransformFunctionKind::Scale;
                    const f32 s = factor();
                    f.A = name == StringView(u8"scaleX") ? s : 1.0f;
                    f.B = name == StringView(u8"scaleY") ? s : 1.0f;
                }
                else if (name == StringView(u8"rotate"))
                {
                    f.Kind = TransformFunctionKind::Rotate;
                    f.A = angle();
                }
                else if (name == StringView(u8"skew"))
                {
                    f.Kind = TransformFunctionKind::Skew;
                    f.A = angle();
                    f.B = MatchComma() ? angle() : 0.0f;
                }
                else if (name == StringView(u8"skewX") || name == StringView(u8"skewY"))
                {
                    f.Kind = TransformFunctionKind::Skew;
                    const f32 a = angle();
                    f.A = name == StringView(u8"skewX") ? a : 0.0f;
                    f.B = name == StringView(u8"skewY") ? a : 0.0f;
                }
                else
                {
                    return StyleValue::None();
                }
                if (Peek().Kind != TokenKind::RParen)
                    return StyleValue::None();
                Consume();
                transform.Push(f);
                any = true;
            }
            return any ? StyleValue::TransformVal(transform) : StyleValue::None();
        }

        /// `transform-origin: <x> [<y>]`: each left, center, right (top, center, bottom for y), a
        /// length or a percentage; one keyword of the other axis sets that axis, the rest centred.
        StyleValue ParseTransformOriginValue()
        {
            StylePoint point;
            bool xSet = false;
            for (i32 part = 0; part < 2; ++part)
            {
                if (Peek().Kind == TokenKind::Ident)
                {
                    const StringView word = Peek().Text;
                    if (word == StringView(u8"left") || word == StringView(u8"right"))
                    {
                        point.X = Unit::Percent(word == StringView(u8"left") ? 0.0f : 100.0f);
                        xSet = true;
                    }
                    else if (word == StringView(u8"top") || word == StringView(u8"bottom"))
                        point.Y = Unit::Percent(word == StringView(u8"top") ? 0.0f : 100.0f);
                    else if (word != StringView(u8"center"))
                        break;
                    Consume();
                }
                else if (Peek().Kind == TokenKind::Number)
                {
                    const Unit length = ParseLengthTerm();
                    (part == 0 && !xSet ? point.X : point.Y) = length;
                    xSet = xSet || part == 0;
                }
                else
                {
                    break;
                }
            }
            return StyleValue::PointVal(point);
        }

        /// `animation: none` or a comma list of `<name> <duration> [timing] [delay] [count |
        /// infinite] [direction] [fill]`, in any order as CSS allows: the first time is the
        /// duration and the second the delay (each with s or ms), a bare number the iteration
        /// count, and the first word no keyword claims the @keyframes' name.
        StyleValue ParseAnimationValue()
        {
            RefPtr<AnimationList> list = MakeRef<AnimationList>(*m_allocator);
            if (Peek().Kind == TokenKind::Ident && Peek().Text == StringView(u8"none"))
            {
                Consume();
                return StyleValue::AnimationsRef(Move(list));
            }
            while (!IsAtEnd() && Peek().Kind != TokenKind::Semicolon && Peek().Kind != TokenKind::RBrace)
            {
                AnimationSpec spec;
                bool haveDuration = false;
                while (Peek().Kind == TokenKind::Ident || Peek().Kind == TokenKind::Number)
                {
                    if (Peek().Kind == TokenKind::Number)
                    {
                        const Token& tok = Peek();
                        if (tok.UnitSuffix == StringView(u8"s") || tok.UnitSuffix == StringView(u8"ms"))
                        {
                            (haveDuration ? spec.Delay : spec.Duration) = ParseTimeSeconds();
                            haveDuration = true;
                        }
                        else
                        {
                            spec.Iterations = Max(0.0f, Consume().NumericValue);
                        }
                        continue;
                    }
                    const StringView word = Peek().Text;
                    if (word == StringView(u8"infinite"))
                        spec.Iterations = -1.0f;
                    else if (word == StringView(u8"normal"))
                        spec.Direction = AnimationDirection::Normal;
                    else if (word == StringView(u8"reverse"))
                        spec.Direction = AnimationDirection::Reverse;
                    else if (word == StringView(u8"alternate"))
                        spec.Direction = AnimationDirection::Alternate;
                    else if (word == StringView(u8"alternate-reverse"))
                        spec.Direction = AnimationDirection::AlternateReverse;
                    else if (word == StringView(u8"forwards"))
                        spec.Fill = AnimationFill::Forwards;
                    else if (word == StringView(u8"backwards"))
                        spec.Fill = AnimationFill::Backwards;
                    else if (word == StringView(u8"both"))
                        spec.Fill = AnimationFill::Both;
                    else if (IsTimingFunctionName(word))
                    {
                        spec.Easing = ParseTimingFunction();
                        continue;
                    }
                    else if (spec.Name.IsEmpty())
                        spec.Name = String(word);
                    Consume();
                }
                if (!spec.Name.IsEmpty())
                    list->Specs.PushBack(Move(spec));
                if (!MatchComma())
                    break;
            }
            return StyleValue::AnimationsRef(Move(list));
        }

        [[nodiscard]] static bool IsTimingFunctionName(StringView word)
        {
            return word == StringView(u8"linear") || word == StringView(u8"ease") || word == StringView(u8"ease-in") ||
                   word == StringView(u8"ease-out") || word == StringView(u8"ease-in-out") ||
                   word == StringView(u8"step-start") || word == StringView(u8"step-end") ||
                   word == StringView(u8"cubic-bezier") || word == StringView(u8"steps");
        }

        /// A `<time>`: `120ms`, `0.2s`, or a bare number of milliseconds. Seconds out.
        f32 ParseTimeSeconds()
        {
            const Token tok = Consume();
            const f32 value = Max(0.0f, tok.NumericValue);
            if (tok.UnitSuffix == StringView(u8"s"))
            {
                return value;
            }
            return value / 1000.0f;
        }

        /// A CSS timing function: linear, ease, ease-in, ease-out, ease-in-out, step-start,
        /// step-end, cubic-bezier(x1, y1, x2, y2) (x clamped to 0 to 1, as CSS requires) or
        /// steps(n[, start | jump-start | end | jump-end]). Anything else is ease.
        TimingFunction ParseTimingFunction()
        {
            const StringView name = Consume().Text;
            if (name == StringView(u8"linear"))
                return TimingFunction::Linear();
            if (name == StringView(u8"ease-in"))
                return TimingFunction::EaseIn();
            if (name == StringView(u8"ease-out"))
                return TimingFunction::EaseOut();
            if (name == StringView(u8"ease-in-out"))
                return TimingFunction::EaseInOut();
            if (name == StringView(u8"step-start"))
                return TimingFunction::StepsOf(1, true);
            if (name == StringView(u8"step-end"))
                return TimingFunction::StepsOf(1, false);
            if (name == StringView(u8"cubic-bezier") && Peek().Kind == TokenKind::LParen)
            {
                Consume();
                f32 v[4] = {};
                for (i32 i = 0; i < 4; ++i)
                {
                    v[i] = Peek().Kind == TokenKind::Number ? Consume().NumericValue : 0.0f;
                    MatchComma();
                }
                Expect(TokenKind::RParen);
                return TimingFunction::CubicBezier(Clamp(v[0], 0.0f, 1.0f), v[1], Clamp(v[2], 0.0f, 1.0f), v[3]);
            }
            if (name == StringView(u8"steps") && Peek().Kind == TokenKind::LParen)
            {
                Consume();
                const f32 count = Peek().Kind == TokenKind::Number ? Consume().NumericValue : 1.0f;
                bool jumpStart = false;
                if (MatchComma() && Peek().Kind == TokenKind::Ident)
                {
                    const StringView position = Consume().Text;
                    jumpStart = position == StringView(u8"start") || position == StringView(u8"jump-start");
                }
                Expect(TokenKind::RParen);
                return TimingFunction::StepsOf(static_cast<u16>(Clamp(count, 1.0f, 1000.0f)), jumpStart);
            }
            return TimingFunction::Ease();
        }

        /// `box-shadow: none`, or one or more shadows separated by commas, each `[inset] <x> <y>
        /// [blur] [spread] [color] [inset]`: lengths are plain numbers (dp), the colour any colour
        /// syntax, and `inset` may lead or trail. The first is painted on top. A list keeps its
        /// first StyleValue::kMaxShadows; one that does not parse makes the value none.
        StyleValue ParseBoxShadowValue()
        {
            if (Peek().Kind == TokenKind::Ident && Peek().Text == StringView(u8"none"))
            {
                Consume();
                return StyleValue::None();
            }
            BoxShadow shadows[StyleValue::kMaxShadows];
            usize count = 0;
            do
            {
                BoxShadow shadow;
                if (!ParseOneBoxShadow(shadow))
                {
                    return StyleValue::None();
                }
                if (count < StyleValue::kMaxShadows)
                {
                    shadows[count++] = shadow;
                }
            } while (MatchComma());
            return StyleValue::ShadowsVal(Span<const BoxShadow>(shadows, count));
        }

        bool ParseOneBoxShadow(BoxShadow& shadow)
        {
            if (Peek().Kind == TokenKind::Ident && Peek().Text == StringView(u8"inset"))
            {
                Consume();
                shadow.Inset = true;
            }
            f32 lengths[4] = {};
            i32 count = 0;
            while (count < 4 && Peek().Kind == TokenKind::Number)
            {
                lengths[count++] = ParseFloatValue();
            }
            if (count < 2)
            {
                return false;
            }
            shadow.OffsetX = lengths[0];
            shadow.OffsetY = lengths[1];
            shadow.Blur = count > 2 ? Max(0.0f, lengths[2]) : 0.0f;
            shadow.Spread = count > 3 ? lengths[3] : 0.0f;
            if (Peek().Kind != TokenKind::Semicolon && Peek().Kind != TokenKind::RBrace &&
                Peek().Kind != TokenKind::Comma &&
                !(Peek().Kind == TokenKind::Ident && Peek().Text == StringView(u8"inset")))
            {
                shadow.Color = ParseColorValue();
            }
            if (Peek().Kind == TokenKind::Ident && Peek().Text == StringView(u8"inset"))
            {
                Consume();
                shadow.Inset = true;
            }
            return true;
        }

        Color ParseRgbFunction()
        {
            Consume(); // rgb/rgba
            Expect(TokenKind::LParen);
            const i32 r = static_cast<i32>(ParseFloatValue());
            MatchComma();
            const i32 g = static_cast<i32>(ParseFloatValue());
            MatchComma();
            const i32 b = static_cast<i32>(ParseFloatValue());
            // rgba() alpha is 0..1 per CSS, R/G/B are 0..255.
            if (MatchComma())
            {
                const f32 a = ParseFloatValue();
                Expect(TokenKind::RParen);
                return Color{r / 255.0f, g / 255.0f, b / 255.0f, a};
            }
            Expect(TokenKind::RParen);
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
        }

        Color ParseColorFunction()
        {
            const StringView name = ConsumeIdent();
            Expect(TokenKind::LParen);
            Color result = Color::White;

            if (name == StringView(u8"lighten"))
            {
                const Color c = ParseColorValue();
                MatchComma();
                result = ColorFunctions::Lighten(c, ParseFloatValue());
            }
            else if (name == StringView(u8"darken"))
            {
                const Color c = ParseColorValue();
                MatchComma();
                result = ColorFunctions::Darken(c, ParseFloatValue());
            }
            else if (name == StringView(u8"alpha"))
            {
                const Color c = ParseColorValue();
                MatchComma();
                result = ColorFunctions::Alpha(c, ParseFloatValue());
            }
            else if (name == StringView(u8"mix"))
            {
                const Color a = ParseColorValue();
                MatchComma();
                const Color b = ParseColorValue();
                MatchComma();
                result = ColorFunctions::Mix(a, b, ParseFloatValue());
            }
            // State derivations use the same Palette::Compute* math as the C++ themes, so sheets
            // keep identical state colors, including disabled()'s luminance desaturation, which
            // lighten and darken cannot express.
            else if (name == StringView(u8"hover"))
            {
                result = Palette::ComputeHover(ParseColorValue());
            }
            else if (name == StringView(u8"pressed"))
            {
                result = Palette::ComputePressed(ParseColorValue());
            }
            else if (name == StringView(u8"disabled"))
            {
                result = Palette::ComputeDisabled(ParseColorValue());
            }
            else if (name == StringView(u8"focused"))
            {
                const Color base = ParseColorValue();
                MatchComma();
                result = Palette::ComputeFocused(base, ParseColorValue());
            }

            Expect(TokenKind::RParen);
            return result;
        }


        // === Property name resolution ===

        [[nodiscard]] static Optional<StyleProperty> ResolvePropertyName(StringView name)
        {
            // Drawable properties
            if (name == StringView(u8"background"))
                return StyleProperty::Background;
            if (name == StringView(u8"checked-background"))
                return StyleProperty::CheckedBackground;
            if (name == StringView(u8"menu-item-hover-drawable"))
                return StyleProperty::MenuItemHoverDrawable;

            // Color properties
            if (name == StringView(u8"text-color"))
                return StyleProperty::TextColor;
            if (name == StringView(u8"text-dim-color"))
                return StyleProperty::TextDimColor;
            if (name == StringView(u8"placeholder-color"))
                return StyleProperty::PlaceholderColor;
            if (name == StringView(u8"border-color"))
                return StyleProperty::BorderColor;
            if (name == StringView(u8"cursor-color"))
                return StyleProperty::CursorColor;
            if (name == StringView(u8"selection-color"))
                return StyleProperty::SelectionColor;
            if (name == StringView(u8"accent-color"))
                return StyleProperty::AccentColor;
            if (name == StringView(u8"success-color"))
                return StyleProperty::SuccessColor;
            if (name == StringView(u8"warning-color"))
                return StyleProperty::WarningColor;
            if (name == StringView(u8"error-color"))
                return StyleProperty::ErrorColor;

            // Float properties
            if (name == StringView(u8"font-size"))
                return StyleProperty::FontSize;
            if (name == StringView(u8"corner-radius"))
                return StyleProperty::CornerRadius;

            // String properties
            if (name == StringView(u8"font-family"))
                return StyleProperty::FontFamily;
            if (name == StringView(u8"border-width"))
                return StyleProperty::BorderWidth;
            if (name == StringView(u8"spacing"))
                return StyleProperty::Spacing;
            if (name == StringView(u8"opacity"))
                return StyleProperty::Opacity;
            if (name == StringView(u8"width"))
                return StyleProperty::Width;
            if (name == StringView(u8"height"))
                return StyleProperty::Height;

            // Thickness properties
            if (name == StringView(u8"padding"))
                return StyleProperty::Padding;
            if (name == StringView(u8"margin"))
                return StyleProperty::Margin;

            // Bool properties
            if (name == StringView(u8"word-wrap"))
                return StyleProperty::WordWrap;

            // Box model
            if (name == StringView(u8"box-shadow"))
                return StyleProperty::BoxShadow;
            if (name == StringView(u8"min-width"))
                return StyleProperty::MinWidth;
            if (name == StringView(u8"min-height"))
                return StyleProperty::MinHeight;
            if (name == StringView(u8"max-width"))
                return StyleProperty::MaxWidth;
            if (name == StringView(u8"max-height"))
                return StyleProperty::MaxHeight;
            if (name == StringView(u8"top"))
                return StyleProperty::Top;
            if (name == StringView(u8"right"))
                return StyleProperty::Right;
            if (name == StringView(u8"bottom"))
                return StyleProperty::Bottom;
            if (name == StringView(u8"left"))
                return StyleProperty::Left;
            if (name == StringView(u8"z-index"))
                return StyleProperty::ZIndex;
            if (name == StringView(u8"flex-grow"))
                return StyleProperty::FlexGrow;
            if (name == StringView(u8"flex-shrink"))
                return StyleProperty::FlexShrink;
            if (name == StringView(u8"position"))
                return StyleProperty::Position;
            if (name == StringView(u8"overflow"))
                return StyleProperty::Overflow;
            if (name == StringView(u8"align-self"))
                return StyleProperty::AlignSelf;

            // Transitions
            if (name == StringView(u8"transition"))
                return StyleProperty::Transition;

            // Wrap, gap and ellipsis
            if (name == StringView(u8"flex-basis"))
                return StyleProperty::FlexBasis;
            if (name == StringView(u8"text-overflow"))
                return StyleProperty::TextOverflow;

            // Filters
            if (name == StringView(u8"filter"))
                return StyleProperty::Filter;
            if (name == StringView(u8"transform"))
                return StyleProperty::Transform;
            if (name == StringView(u8"transform-origin"))
                return StyleProperty::TransformOrigin;
            if (name == StringView(u8"animation"))
                return StyleProperty::Animation;
            if (name == StringView(u8"backdrop-filter"))
                return StyleProperty::BackdropFilter;

            // Text effects
            if (name == StringView(u8"text-shadow"))
                return StyleProperty::TextShadow;
            if (name == StringView(u8"text-outline"))
                return StyleProperty::TextOutline;
            if (name == StringView(u8"text-glow"))
                return StyleProperty::TextGlow;

            return {};
        }

        /// A pseudo-class: a control state (with the CSS aliases `:active` = pressed, `:focus`
        /// and `:focus-visible` = focused) or a structural test. Unknown names are ignored.
        static void ApplyPseudoClass(StringView name, ControlState& state, bool& hasState,
                                     StructuralMatch& structural)
        {
            if (name == StringView(u8"first-child"))
            {
                structural = structural | StructuralMatch::FirstChild;
                return;
            }
            if (name == StringView(u8"last-child"))
            {
                structural = structural | StructuralMatch::LastChild;
                return;
            }
            if (name == StringView(u8"empty"))
            {
                structural = structural | StructuralMatch::Empty;
                return;
            }
            hasState = true;
            if (name == StringView(u8"hover"))
                state |= ControlState::Hover;
            else if (name == StringView(u8"pressed") || name == StringView(u8"active"))
                state |= ControlState::Pressed;
            else if (name == StringView(u8"focused") || name == StringView(u8"focus") ||
                     name == StringView(u8"focus-visible"))
                state |= ControlState::Focused;
            else if (name == StringView(u8"disabled"))
                state |= ControlState::Disabled;
            else if (name == StringView(u8"checked"))
                state |= ControlState::Checked;
            else if (name == StringView(u8"indeterminate"))
                state |= ControlState::Indeterminate;
            // `normal` (and anything unknown) adds no flag: matches the normal state.
        }

        [[nodiscard]] static bool IsDrawableProperty(StyleProperty prop)
        {
            return prop <= StyleProperty::MenuItemHoverDrawable;
        }
        [[nodiscard]] static bool IsColorProperty(StyleProperty prop)
        {
            return prop >= StyleProperty::TextColor && prop <= StyleProperty::ErrorColor;
        }
        [[nodiscard]] static bool IsThicknessProperty(StyleProperty prop)
        {
            return prop == StyleProperty::Padding || prop == StyleProperty::Margin;
        }
        [[nodiscard]] static bool IsBoolProperty(StyleProperty prop)
        {
            return prop == StyleProperty::WordWrap;
        }
        [[nodiscard]] static bool IsStringProperty(StyleProperty prop)
        {
            return prop == StyleProperty::FontFamily || prop == StyleProperty::Position ||
                   prop == StyleProperty::Overflow || prop == StyleProperty::AlignSelf ||
                   prop == StyleProperty::TextOverflow;
        }
        /// width/height accept the SizeSpec keywords (`match`, `wrap`) as well as a length.
        [[nodiscard]] static bool IsSizeSpecProperty(StyleProperty prop)
        {
            return prop == StyleProperty::Width || prop == StyleProperty::Height;
        }

        /// Read a quoted string literal or a bare identifier.
        StringView ParseStringOrIdent()
        {
            if (Peek().Kind == TokenKind::StringLit)
                return Consume().Text;
            if (Peek().Kind == TokenKind::Ident)
                return Consume().Text;
            return u8"";
        }

        // === Variable resolution ===

        Color ResolveVariable(StringView varText)
        {
            const StringView name = (varText.Size() > 0 && varText[0] == u8'$')
                                        ? varText.SubStr(1, varText.Size() - 1)
                                        : varText;
            if (const Color* c = m_palette->Find(String(name)))
            {
                return *c;
            }
            return Color::White; // unresolved variable
        }

        // === Path resolution ===

        [[nodiscard]] String ResolvePath(StringView path)
        {
            if (m_basePath.Size() > 0)
            {
                String resolved;
                resolved += m_basePath.AsView();
                resolved += path;
                return resolved;
            }
            return String(path);
        }

        // === Token helpers ===

        [[nodiscard]] Token Peek() const
        {
            return (m_pos < static_cast<i32>(m_tokens.Size())) ? m_tokens[static_cast<usize>(m_pos)]
                                                               : Token(TokenKind::EndOfInput, u8"", 0, 0);
        }
        Token Consume()
        {
            return (m_pos < static_cast<i32>(m_tokens.Size()))
                       ? m_tokens[static_cast<usize>(m_pos++)]
                       : Token(TokenKind::EndOfInput, u8"", 0, 0);
        }
        [[nodiscard]] bool IsAtEnd() const
        {
            return m_pos >= static_cast<i32>(m_tokens.Size()) ||
                   m_tokens[static_cast<usize>(m_pos)].Kind == TokenKind::EndOfInput;
        }

        StringView ConsumeString()
        {
            if (Peek().Kind == TokenKind::StringLit)
                return Consume().Text;
            return u8"";
        }

        void Expect(TokenKind kind)
        {
            if (Peek().Kind == kind)
                Consume();
        }

        bool MatchSemicolon()
        {
            if (Peek().Kind == TokenKind::Semicolon)
            {
                Consume();
                return true;
            }
            return false;
        }

        void SkipUntilSemicolon()
        {
            while (!IsAtEnd() && Peek().Kind != TokenKind::Semicolon)
                Consume();
            MatchSemicolon();
        }

        void SkipUntilSemicolonOrBrace()
        {
            while (!IsAtEnd() && Peek().Kind != TokenKind::Semicolon &&
                   Peek().Kind != TokenKind::RBrace)
                Consume();
            MatchSemicolon();
        }

        IAllocator* m_allocator;
        Array<Token> m_tokens;
        i32 m_pos = 0;
        HashMap<String, Color>* m_palette = nullptr;
        HashMap<String, String>* m_svgRegistry = nullptr;
        HashMap<String, const image::ImageData*>* m_imageRegistry = nullptr;
        IResourceProvider* m_resourceProvider = nullptr;
        RefPtr<StyleSheet> m_sheetOwner;
        StyleSheet* m_sheet = nullptr;
        String m_basePath;
    };

    // === DrawableFactoryRegistry (defined after SSSParser is complete) ===

    namespace detail
    {
        // Defined out of line in UiRegistryStateImpl.cpp: an inline function-local static would
        // give each shared library its own map.
        [[nodiscard]] HashMap<String, DrawableFactoryRegistry::FactoryFn>& FactoryMap();

        // The RegisterBuiltins run-once flag, out of line for the same reason: an inline
        // function-local flag would let one library's registration satisfy only its own guard.
        // The lock guards it because the UI document cook registers on job workers, and two
        // workers past an unguarded flag would rehash the factory map under each other.
        [[nodiscard]] bool& DrawableBuiltinsRegisteredFlag();
        [[nodiscard]] Mutex& DrawableRegistrationLock();

        /// A gradient's trailing `radius=` (1 or 4 values, as rounded-rect takes them).
        inline void ParseGradientRadius(SSSParser& parser, GradientDrawable& drawable)
        {
            while (parser.MatchComma() || !parser.PeekKeywordArg().IsEmpty())
            {
                if (parser.PeekKeywordArg() == StringView(u8"radius"))
                {
                    parser.ConsumeKeywordArg();
                    drawable.Radii = parser.ParseCornerRadiiValue();
                }
                else
                {
                    break;
                }
            }
        }

        [[nodiscard]] inline ControlState ParseStateName(StringView name)
        {
            if (name == StringView(u8"normal"))
                return ControlState::Normal;
            if (name == StringView(u8"hover"))
                return ControlState::Hover;
            if (name == StringView(u8"pressed"))
                return ControlState::Pressed;
            if (name == StringView(u8"focused"))
                return ControlState::Focused;
            if (name == StringView(u8"disabled"))
                return ControlState::Disabled;
            if (name == StringView(u8"checked"))
                return ControlState::Checked;
            if (name == StringView(u8"indeterminate"))
                return ControlState::Indeterminate;
            return ControlState::Normal;
        }
    }

    inline void DrawableFactoryRegistry::Register(StringView name, FactoryFn factory)
    {
        detail::FactoryMap().InsertOrAssign(String(name), factory);
    }

    inline DrawableFactoryRegistry::FactoryFn DrawableFactoryRegistry::Get(StringView name)
    {
        if (FactoryFn* f = detail::FactoryMap().Find(String(name)))
        {
            return *f;
        }
        return nullptr;
    }

    inline void DrawableFactoryRegistry::RegisterBuiltins()
    {
        // Idempotent and thread-safe: the factory map is a global static, so register once even if
        // called from several entry points (StyleSheetLoader::Load and SSSParser::ApplyInlineStyle
        // both call this) or from several cook workers at once. A second caller waits for the first
        // registration to complete rather than reading a half-built map.
        ScopedLock lock(detail::DrawableRegistrationLock());
        bool& registered = detail::DrawableBuiltinsRegisteredFlag();
        if (registered)
        {
            return;
        }
        registered = true;

        // color($color) makes a ColorDrawable
        Register(u8"color",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     String variable;
                     const Color color = parser.ParseColorArgOrVar(variable);
                     RefPtr<ColorDrawable> d = MakeRef<ColorDrawable>(parser.Allocator(), color);
                     d->ColorVariable = Move(variable);
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // rounded-rect($color, radius=6, border=$color, border-width=1) makes a RoundedRectDrawable.
        // border-width takes 1, 2 or 4 values in CSS's order (top, right, bottom, left), and
        // border-top=, border-right=, border-bottom= and border-left= colour one side over border=.
        Register(u8"rounded-rect",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     String fillVariable;
                     const Color fillColor = parser.ParseColorArgOrVar(fillVariable);
                     vg::CornerRadii radii{};
                     Color borderColor = Color::Transparent;
                     String borderVariable;
                     Thickness widths{};
                     bool sideWidths = false;
                     Optional<Color> sides[4]; // top, right, bottom, left

                     while (parser.MatchComma())
                     {
                         const StringView kw = parser.PeekKeywordArg();
                         if (kw == StringView(u8"radius"))
                         {
                             parser.ConsumeKeywordArg();
                             radii = parser.ParseCornerRadiiValue(); // 1 or 4 values
                         }
                         else if (kw == StringView(u8"border-width"))
                         {
                             parser.ConsumeKeywordArg();
                             widths = parser.ParseThicknessValue();
                             sideWidths = widths.Top != widths.Right || widths.Top != widths.Bottom ||
                                          widths.Top != widths.Left;
                         }
                         else if (kw == StringView(u8"border"))
                         {
                             parser.ConsumeKeywordArg();
                             borderColor = parser.ParseColorArgOrVar(borderVariable);
                         }
                         else if (kw == StringView(u8"border-top") || kw == StringView(u8"border-right") ||
                                  kw == StringView(u8"border-bottom") || kw == StringView(u8"border-left"))
                         {
                             const usize side = kw == StringView(u8"border-top")     ? 0
                                                : kw == StringView(u8"border-right") ? 1
                                                : kw == StringView(u8"border-bottom") ? 2
                                                                                       : 3;
                             parser.ConsumeKeywordArg();
                             sides[side] = parser.ParseColorArg();
                         }
                         else
                             radii = parser.ParseCornerRadiiValue();
                     }

                     RefPtr<RoundedRectDrawable> d = MakeRef<RoundedRectDrawable>(
                         parser.Allocator(), fillColor, radii, borderColor, widths.Top);
                     d->FillVariable = Move(fillVariable);
                     d->BorderVariable = Move(borderVariable);
                     if (sideWidths)
                         d->SideWidths = vg::BorderWidths(widths.Top, widths.Right, widths.Bottom, widths.Left);
                     if (sides[0].HasValue() || sides[1].HasValue() || sides[2].HasValue() || sides[3].HasValue())
                     {
                         const auto side = [&](usize i) { return sides[i].HasValue() ? sides[i].Value() : borderColor; };
                         d->SideColors = vg::BorderColors(side(0), side(1), side(2), side(3));
                     }
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // gradient(direction, color1, color2[, radius=r]) makes a GradientDrawable
        Register(u8"gradient",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     GradientDirection dir = GradientDirection::TopToBottom;
                     if (parser.PeekIdent() == StringView(u8"top-to-bottom"))
                     {
                         parser.ConsumeIdent();
                         dir = GradientDirection::TopToBottom;
                         parser.MatchComma();
                     }
                     else if (parser.PeekIdent() == StringView(u8"left-to-right"))
                     {
                         parser.ConsumeIdent();
                         dir = GradientDirection::LeftToRight;
                         parser.MatchComma();
                     }
                     else if (parser.PeekIdent() == StringView(u8"top-left-to-bottom-right"))
                     {
                         parser.ConsumeIdent();
                         dir = GradientDirection::TopLeftToBottomRight;
                         parser.MatchComma();
                     }
                     else if (parser.PeekIdent() == StringView(u8"top-right-to-bottom-left"))
                     {
                         parser.ConsumeIdent();
                         dir = GradientDirection::TopRightToBottomLeft;
                         parser.MatchComma();
                     }

                     String startVariable;
                     const Color c1 = parser.ParseColorArgOrVar(startVariable);
                     parser.MatchComma();
                     String endVariable;
                     const Color c2 = parser.ParseColorArgOrVar(endVariable);
                     RefPtr<GradientDrawable> d =
                         MakeRef<GradientDrawable>(parser.Allocator(), c1, c2, dir);
                     d->StartVariable = Move(startVariable);
                     d->EndVariable = Move(endVariable);
                     detail::ParseGradientRadius(parser, *d);
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // linear-gradient([<angle> | to <side> [<side>],] <color> [<offset>%], ...[, radius=r]),
        // as CSS writes it (180deg, down, when no direction is given), makes a GradientDrawable
        Register(u8"linear-gradient",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     RefPtr<GradientDrawable> d = MakeRef<GradientDrawable>(parser.Allocator());
                     f32 angle = 180.0f;
                     if (parser.PeekIsNumber())
                     {
                         angle = parser.ParseAngleDegrees();
                         parser.MatchComma();
                     }
                     else if (parser.PeekIdent() == StringView(u8"to"))
                     {
                         parser.ConsumeIdent();
                         // A side or a corner, as two sides in either order.
                         f32 dx = 0.0f;
                         f32 dy = 0.0f;
                         for (i32 word = 0; word < 2; ++word)
                         {
                             const StringView side = parser.PeekIdent();
                             if (side == StringView(u8"top"))
                                 dy = -1.0f;
                             else if (side == StringView(u8"bottom"))
                                 dy = 1.0f;
                             else if (side == StringView(u8"left"))
                                 dx = -1.0f;
                             else if (side == StringView(u8"right"))
                                 dx = 1.0f;
                             else
                                 break;
                             parser.ConsumeIdent();
                         }
                         // A corner points the line at it through a square's diagonal: CSS's
                         // corner follows the box's shape, kept to 45 degrees here.
                         angle = Atan2(dx, -dy) * kRadToDeg;
                         parser.MatchComma();
                     }
                     d->AngleDegrees = angle < 0.0f ? angle + 360.0f : angle;
                     d->Stops = parser.ParseGradientStops(d->StopVariables);
                     detail::ParseGradientRadius(parser, *d);
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // radial-gradient(<color> [<offset>%], ...[, radius=r]): from the centre out to the farthest
        // corner, a circle, makes a GradientDrawable
        Register(u8"radial-gradient",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     RefPtr<GradientDrawable> d = MakeRef<GradientDrawable>(parser.Allocator());
                     d->Radial = true;
                     d->Stops = parser.ParseGradientStops(d->StopVariables);
                     detail::ParseGradientRadius(parser, *d);
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // state-list(normal=d, hover=d, pressed=d, ...) makes a StateListDrawable
        Register(u8"state-list",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     RefPtr<StateListDrawable> sl = MakeRef<StateListDrawable>(parser.Allocator());
                     sheet.OwnDrawable(sl);

                     while (!parser.IsAtRParen())
                     {
                         const StringView stateName = parser.PeekKeywordArg();
                         if (stateName.Size() > 0)
                         {
                             parser.ConsumeKeywordArg();
                             const ControlState state = detail::ParseStateName(stateName);
                             RefPtr<Drawable> drawable = parser.ParseDrawableValue(sheet);
                             if (drawable)
                                 sl->Set(state, drawable);
                         }
                         if (!parser.MatchComma())
                             break;
                     }

                     return sl;
                 });

        // state-colors($base) makes a StateListDrawable through Palette::CreateStateColors
        Register(u8"state-colors",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     const Color baseColor = parser.ParseColorArg();
                     RefPtr<StateListDrawable> sl = Palette::CreateStateColors(parser.Allocator(), baseColor);
                     sheet.OwnDrawable(sl);
                     return sl;
                 });

        // state-rounded($base, radius=6) makes a StateListDrawable through Palette::CreateStateRounded
        Register(u8"state-rounded",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     const Color baseColor = parser.ParseColorArg();
                     vg::CornerRadii radii{};
                     if (parser.MatchComma())
                     {
                         const StringView kw = parser.PeekKeywordArg();
                         if (kw == StringView(u8"radius"))
                         {
                             parser.ConsumeKeywordArg();
                             radii = parser.ParseCornerRadiiValue(); // 1 or 4 values
                         }
                         else
                             radii = parser.ParseCornerRadiiValue();
                     }
                     RefPtr<StateListDrawable> sl =
                         Palette::CreateStateRounded(parser.Allocator(), baseColor, radii);
                     sheet.OwnDrawable(sl);
                     return sl;
                 });

        // layer(d1, d2, ...) makes a LayerDrawable
        Register(u8"layer",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     RefPtr<LayerDrawable> ld = MakeRef<LayerDrawable>(parser.Allocator());
                     sheet.OwnDrawable(ld);

                     while (!parser.IsAtRParen())
                     {
                         RefPtr<Drawable> drawable = parser.ParseDrawableValue(sheet);
                         if (drawable)
                             ld->AddLayer(drawable);
                         if (!parser.MatchComma())
                             break;
                     }

                     return ld;
                 });

        // inset(drawable, top, right, bottom, left) makes an InsetDrawable
        Register(u8"inset",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     RefPtr<Drawable> inner = parser.ParseDrawableValue(sheet);
                     f32 t = 0.0f, r = 0.0f, b = 0.0f, l = 0.0f;
                     if (parser.MatchComma())
                         t = parser.ParseFloatValue();
                     if (parser.MatchComma())
                         r = parser.ParseFloatValue();
                     if (parser.MatchComma())
                         b = parser.ParseFloatValue();
                     if (parser.MatchComma())
                         l = parser.ParseFloatValue();

                     RefPtr<InsetDrawable> d = MakeRef<InsetDrawable>(
                         parser.Allocator(), Move(inner), Thickness{l, t, r, b});
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // svg(name, tint=$color) makes an SVGDrawable
        Register(u8"svg",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     const StringView name = parser.ConsumeIdent();
                     Optional<Color> tint;
                     if (parser.MatchComma())
                     {
                         const StringView kw = parser.PeekKeywordArg();
                         if (kw == StringView(u8"tint"))
                         {
                             parser.ConsumeKeywordArg();
                             tint = parser.ParseColorArg();
                         }
                         else
                             tint = parser.ParseColorArg();
                     }

                     const Optional<StringView> svgText = parser.ResolveSvg(name);
                     if (!svgText.HasValue())
                     {
                         // Built-in fallback: the chrome glyph names resolve through
                         // ThemeIconSet, so cooked and runtime themes work without the host
                         // registering the names (the lookup would otherwise yield null here),
                         // and sheets share the pixel-snapped baked instances instead of
                         // parsing the vectors again.
                         if (Optional<ThemeIcon> builtin = ThemeIconFromName(name);
                             builtin.HasValue())
                         {
                             RefPtr<Drawable> shared =
                                 tint.HasValue()
                                     ? ThemeIconSet::Acquire(builtin.Value(), tint.Value())
                                     : ThemeIconSet::Acquire(builtin.Value());
                             if (shared)
                                 sheet.OwnDrawable(shared);
                             return shared;
                         }
                         return nullptr;
                     }

                     RefPtr<SVGDrawable> d;
                     if (tint.HasValue())
                         d = SVGDrawable::FromString(parser.Allocator(), svgText.Value(), tint.Value());
                     else
                         d = SVGDrawable::FromString(parser.Allocator(), svgText.Value());

                     if (d)
                         sheet.OwnDrawable(d);
                     return d;
                 });

        // image(name, tint=$color, radius=6) makes an ImageDrawable (radius takes 1 or 4 values
        // and rounds the image's corners)
        Register(u8"image",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     const StringView name = parser.ConsumeIdent();
                     Color tint = Color::White;
                     vg::CornerRadii radii{};
                     while (parser.MatchComma())
                     {
                         const StringView kw = parser.PeekKeywordArg();
                         if (kw == StringView(u8"radius"))
                         {
                             parser.ConsumeKeywordArg();
                             radii = parser.ParseCornerRadiiValue();
                         }
                         else if (kw == StringView(u8"tint"))
                         {
                             parser.ConsumeKeywordArg();
                             tint = parser.ParseColorArg();
                         }
                         else
                             tint = parser.ParseColorArg();
                     }

                     const image::ImageData* imageData = parser.ResolveImage(name);
                     if (imageData == nullptr)
                         return nullptr;

                     RefPtr<ImageDrawable> d =
                         MakeRef<ImageDrawable>(parser.Allocator(), imageData, tint);
                     d->Radii = radii;
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // nine-slice(name, slices, tint=$color) makes a NineSliceDrawable
        Register(u8"nine-slice",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     const StringView name = parser.ConsumeIdent();
                     parser.MatchComma();

                     // Parse slices as 1 or 4 values
                     f32 sliceVals[4] = {};
                     i32 sliceCount = 0;
                     while (sliceCount < 4 && parser.PeekIsNumber())
                         sliceVals[sliceCount++] = parser.ParseFloatValue();

                     image::NineSlice slices{};
                     if (sliceCount == 1)
                         slices = image::NineSlice(sliceVals[0], sliceVals[0], sliceVals[0],
                                                   sliceVals[0]);
                     else if (sliceCount == 4)
                         slices = image::NineSlice(sliceVals[0], sliceVals[1], sliceVals[2],
                                                   sliceVals[3]);

                     Color tint = Color::White;
                     if (parser.MatchComma())
                     {
                         const StringView kw = parser.PeekKeywordArg();
                         if (kw == StringView(u8"tint"))
                         {
                             parser.ConsumeKeywordArg();
                             tint = parser.ParseColorArg();
                         }
                         else
                             tint = parser.ParseColorArg();
                     }

                     const image::ImageData* imageData = parser.ResolveImage(name);
                     if (imageData == nullptr)
                         return nullptr;

                     RefPtr<NineSliceDrawable> d =
                         MakeRef<NineSliceDrawable>(parser.Allocator(), imageData, slices, tint);
                     sheet.OwnDrawable(d);
                     return d;
                 });
    }
}
