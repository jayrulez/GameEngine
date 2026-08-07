// Draconic UI - :sss_parser partition
//
// Parses .sss stylesheet text into a StyleSheet (used internally by StyleSheetLoader). Ported from
// Sedulous.UI/src/Styling/Parser/SSSParser.bf, with DrawableFactoryRegistry
// (Sedulous.UI/src/Styling/Parser/DrawableFactoryRegistry.bf) MERGED into this partition: the
// factories intimately use SSSParser and SSSParser::ParseDrawableValue calls the registry, a mutual
// dependency C++ module partitions cannot express as two files.
//
// Divergences (language):
//  - Beef delegate `FactoryFn` -> a plain function pointer (the built-in factories capture nothing);
//    the static Dictionary -> a function-local static HashMap.
//  - Manual Beef refcounting (AddRef before adding a drawable to a container) disappears: drawables
//    are RefPtr, so sharing between the sheet's owned list and a container is automatic.
//  - The @import merge (Beef [Friend] mRules/mOwnedDrawables move) -> StyleSheet::MergeFrom.
//  - ParseProperty hoists the drawable/string cases out of ParseStyleValue (our StyleValue cannot
//    hand back an owning RefPtr<Drawable>); behavior is identical to Sedulous.
//  - ApplyInlineStyle (for `style="..."` markup attributes) is declared here and defined in the impl
//    unit Styling/Parser/SSSParserImpl.cpp (its body touches the View cluster). ParseDeclarations parses
//    a bare declaration body into an existing rule.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:sss_parser;

import draconic.foundation;
import draconic.image;
import draconic.vg;
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
import :palette;
import :sss_token;
import :sss_tokenizer;
import :style_value_parser;
import :color_functions;
import :ui_type_registry;
import :iresource_provider;

using namespace draconic::foundation;
namespace image = draconic::image;
namespace vg = draconic::vg;

export namespace draconic::ui
{
    class SSSParser;
    class View; // for SSSParser::ApplyInlineStyle (body in Styling/Parser/SSSParserImpl.cpp)

    /// Registry of drawable factory functions invocable from .sss stylesheets. User-extensible via
    /// Register(). Factories are non-capturing, so a plain function pointer replaces Beef's delegate.
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
        SSSParser(Array<Token> tokens, HashMap<String, Color>* palette,
                  HashMap<String, String>* svgRegistry,
                  HashMap<String, const image::ImageData*>* imageRegistry,
                  IResourceProvider* resourceProvider, String basePath)
            : m_tokens(Move(tokens)), m_palette(palette), m_svgRegistry(svgRegistry),
              m_imageRegistry(imageRegistry), m_resourceProvider(resourceProvider),
              m_basePath(Move(basePath))
        {
        }

        RefPtr<StyleSheet> Parse()
        {
            m_sheetOwner = MakeRef<StyleSheet>(DefaultAllocator());
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
        /// variables ($name) and @-rules are not supported - inline-style values must be literal. Body in
        /// the impl unit (touches the View cluster).
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
                    name == StringView(u8"alpha") || name == StringView(u8"mix"))
                {
                    return ParseColorFunction();
                }
            }
            return Color::White;
        }

        /// Parse a color argument inside a function call (for drawable factories).
        Color ParseColorArg() { return ParseColorValue(); }

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

        /// Parse a drawable value: factory call or color literal -> ColorDrawable.
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
            RefPtr<ColorDrawable> d = MakeRef<ColorDrawable>(DefaultAllocator(), color);
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
            else
                SkipUntilSemicolon();
        }

        void ParsePaletteDirective()
        {
            // @palette name { ... }  /  @palette name extends parent { ... }
            ConsumeIdent(); // palette name (for future multi-palette support)

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
            // @icon name "path"; - registers SVG by loading from file via resource provider.
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

                    SSSParser importParser(Move(importTokens), m_palette, m_svgRegistry,
                                           m_imageRegistry, m_resourceProvider, Move(importBase));
                    RefPtr<StyleSheet> importSheet = importParser.Parse();
                    m_sheet->MergeFrom(*importSheet);
                }
            }
        }

        // === Rules ===

        void ParseRule()
        {
            // Selector: Type.class.class:state:state { ... }
            RefPtr<StyleRule> rule = MakeRef<StyleRule>(DefaultAllocator());

            // Type selector
            if (Peek().Kind == TokenKind::Ident)
            {
                const StringView typeName = Peek().Text;
                if (const TypeInfo* type = UITypeRegistry::Resolve(typeName))
                {
                    Consume();
                    rule->Selector.ViewType = type;
                }
            }

            // Class selectors
            while (Peek().Kind == TokenKind::ClassSelector)
            {
                const Token cls = Consume();
                rule->Selector.AddClass(cls.Text.SubStr(1, cls.Text.Size() - 1)); // skip leading .
            }

            // Pseudo-states (:hover, :checked, ...) - may be multiple for compound states
            ControlState state = ControlState::Normal;
            bool hasState = false;
            while (Peek().Kind == TokenKind::PseudoState)
            {
                const Token ps = Consume();
                state |=
                    ParsePseudoStateName(ps.Text.SubStr(1, ps.Text.Size() - 1)); // skip leading :
                hasState = true;
            }
            if (hasState)
                rule->Selector.State = state;

            // Pseudo-element (::thumb, ::track, ...). The tokenizer produces :: as Colon +
            // PseudoState(:name), since the second : followed by a letter reads as a PseudoState.
            if (Peek().Kind == TokenKind::Colon && m_pos + 1 < static_cast<i32>(m_tokens.Size()) &&
                m_tokens[static_cast<usize>(m_pos + 1)].Kind == TokenKind::PseudoState)
            {
                Consume();                  // first : (Colon)
                const Token ps = Consume(); // :name (PseudoState)
                rule->Selector.SetPseudoElement(
                    ps.Text.SubStr(1, ps.Text.Size() - 1)); // skip leading :
            }

            // Allow :state after ::pseudo (e.g., ::tab:hover)
            while (Peek().Kind == TokenKind::PseudoState)
            {
                const Token ps = Consume();
                state |= ParsePseudoStateName(ps.Text.SubStr(1, ps.Text.Size() - 1));
                hasState = true;
            }
            if (hasState)
                rule->Selector.State = state;

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

            const Optional<StyleProperty> prop = ResolvePropertyName(propName);
            if (!prop.HasValue())
            {
                // Unknown property - skip to semicolon
                SkipUntilSemicolonOrBrace();
                return;
            }
            const StyleProperty p = prop.Value();

            // String-valued properties (font-family, etc) skip the StyleValue wrapper.
            if (IsStringProperty(p))
            {
                const StringView s = ParseStringOrIdent();
                if (s.Size() > 0)
                    rule.Set(p, s);
                MatchSemicolon();
                return;
            }

            // Drawable properties own a RefPtr directly (hoisted out of ParseStyleValue).
            if (IsDrawableProperty(p))
            {
                RefPtr<Drawable> d = ParseDrawableValue(*m_sheet);
                if (d)
                    rule.Set(p, Move(d));
                MatchSemicolon();
                return;
            }

            const StyleValue value = ParseStyleValue(p);
            switch (value.GetKind())
            {
            case StyleValue::Kind::Color:
                rule.Set(p, value.AsColor().Value());
                break;
            case StyleValue::Kind::Float:
                rule.Set(p, value.AsFloat().Value());
                break;
            case StyleValue::Kind::Thickness:
                rule.Set(p, value.AsThickness().Value());
                break;
            case StyleValue::Kind::Bool:
                rule.Set(p, value.AsBool().Value());
                break;
            default:
                break;
            }

            MatchSemicolon();
        }

        // === Private value parsing ===

        StyleValue ParseStyleValue(StyleProperty prop)
        {
            if (IsColorProperty(prop))
                return StyleValue::ColorVal(ParseColorValue());
            if (IsThicknessProperty(prop))
                return StyleValue::ThicknessVal(ParseThicknessValue());
            if (IsBoolProperty(prop))
            {
                if (Peek().Kind == TokenKind::BoolLit)
                    return StyleValue::BoolVal(Consume().Text == StringView(u8"true"));
                return StyleValue::None();
            }
            // Default: float
            return StyleValue::FloatVal(ParseFloatValue());
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

            Expect(TokenKind::RParen);
            return result;
        }

        Thickness ParseThicknessValue()
        {
            f32 values[4] = {};
            i32 count = 0;
            while (count < 4 && Peek().Kind == TokenKind::Number)
                values[count++] = ParseFloatValue();
            return StyleValueParser::ParseThickness(values, count);
        }

        // === Property name resolution (all ~48 properties) ===

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

            return {};
        }

        [[nodiscard]] static ControlState ParsePseudoStateName(StringView name)
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

        [[nodiscard]] static bool IsDrawableProperty(StyleProperty prop)
        {
            return prop <= StyleProperty::MenuItemHoverDrawable;
        }
        [[nodiscard]] static bool IsColorProperty(StyleProperty prop)
        {
            return prop >= StyleProperty::TextColor && prop <= StyleProperty::AccentColor;
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
            return prop == StyleProperty::FontFamily;
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
        inline HashMap<String, DrawableFactoryRegistry::FactoryFn>& FactoryMap()
        {
            static HashMap<String, DrawableFactoryRegistry::FactoryFn> factories;
            return factories;
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
        // Idempotent: the factory map is a global static, so register once even if called from several
        // entry points (StyleSheetLoader::Load and SSSParser::ApplyInlineStyle both ensure this).
        static bool registered = false;
        if (registered)
        {
            return;
        }
        registered = true;

        // color($color) -> ColorDrawable
        Register(u8"color",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     const Color color = parser.ParseColorArg();
                     RefPtr<ColorDrawable> d = MakeRef<ColorDrawable>(DefaultAllocator(), color);
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // rounded-rect($color, radius=6, border=$color, border-width=1) -> RoundedRectDrawable
        Register(u8"rounded-rect",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     const Color fillColor = parser.ParseColorArg();
                     f32 radius = 0.0f;
                     Color borderColor = Color::Transparent;
                     f32 borderWidth = 0.0f;

                     while (parser.MatchComma())
                     {
                         const StringView kw = parser.PeekKeywordArg();
                         if (kw == StringView(u8"radius"))
                         {
                             parser.ConsumeKeywordArg();
                             radius = parser.ParseFloatValue();
                         }
                         else if (kw == StringView(u8"border-width"))
                         {
                             parser.ConsumeKeywordArg();
                             borderWidth = parser.ParseFloatValue();
                         }
                         else if (kw == StringView(u8"border"))
                         {
                             parser.ConsumeKeywordArg();
                             borderColor = parser.ParseColorArg();
                         }
                         else
                             radius = parser.ParseFloatValue();
                     }

                     RefPtr<RoundedRectDrawable> d = MakeRef<RoundedRectDrawable>(
                         DefaultAllocator(), fillColor, radius, borderColor, borderWidth);
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // gradient(direction, color1, color2) -> GradientDrawable
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

                     const Color c1 = parser.ParseColorArg();
                     parser.MatchComma();
                     const Color c2 = parser.ParseColorArg();
                     RefPtr<GradientDrawable> d =
                         MakeRef<GradientDrawable>(DefaultAllocator(), c1, c2, dir);
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // state-list(normal=d, hover=d, pressed=d, ...) -> StateListDrawable
        Register(u8"state-list",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     RefPtr<StateListDrawable> sl = MakeRef<StateListDrawable>(DefaultAllocator());
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

        // state-colors($base) -> StateListDrawable via Palette.CreateStateColors
        Register(u8"state-colors",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     const Color baseColor = parser.ParseColorArg();
                     RefPtr<StateListDrawable> sl = Palette::CreateStateColors(baseColor);
                     sheet.OwnDrawable(sl);
                     return sl;
                 });

        // state-rounded($base, radius=6) -> StateListDrawable via Palette.CreateStateRounded
        Register(u8"state-rounded",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     const Color baseColor = parser.ParseColorArg();
                     f32 radius = 0.0f;
                     if (parser.MatchComma())
                     {
                         const StringView kw = parser.PeekKeywordArg();
                         if (kw == StringView(u8"radius"))
                         {
                             parser.ConsumeKeywordArg();
                             radius = parser.ParseFloatValue();
                         }
                         else
                             radius = parser.ParseFloatValue();
                     }
                     RefPtr<StateListDrawable> sl =
                         Palette::CreateStateRounded(baseColor, vg::CornerRadii(radius));
                     sheet.OwnDrawable(sl);
                     return sl;
                 });

        // layer(d1, d2, ...) -> LayerDrawable
        Register(u8"layer",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     RefPtr<LayerDrawable> ld = MakeRef<LayerDrawable>(DefaultAllocator());
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

        // inset(drawable, top, right, bottom, left) -> InsetDrawable
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
                         DefaultAllocator(), Move(inner), Thickness{l, t, r, b});
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // svg(name, tint=$color) -> SVGDrawable
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
                         return nullptr;

                     RefPtr<SVGDrawable> d;
                     if (tint.HasValue())
                         d = SVGDrawable::FromString(svgText.Value(), tint.Value());
                     else
                         d = SVGDrawable::FromString(svgText.Value());

                     if (d)
                         sheet.OwnDrawable(d);
                     return d;
                 });

        // image(name, tint=$color) -> ImageDrawable
        Register(u8"image",
                 [](SSSParser& parser, StyleSheet& sheet) -> RefPtr<Drawable>
                 {
                     const StringView name = parser.ConsumeIdent();
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

                     RefPtr<ImageDrawable> d =
                         MakeRef<ImageDrawable>(DefaultAllocator(), imageData, tint);
                     sheet.OwnDrawable(d);
                     return d;
                 });

        // nine-slice(name, slices, tint=$color) -> NineSliceDrawable
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
                         MakeRef<NineSliceDrawable>(DefaultAllocator(), imageData, slices, tint);
                     sheet.OwnDrawable(d);
                     return d;
                 });
    }
}
