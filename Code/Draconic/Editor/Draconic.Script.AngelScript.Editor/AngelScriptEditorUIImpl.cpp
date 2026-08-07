// Draconic::ScriptAngelScriptEditorUI - the `draconic.script.angelscript.editor.ui` module.
//
// The AngelScript syntax tables + lexer registration. Spec quirks: """ heredoc strings and
// char literals; block comments do not nest, no preprocessor lines.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.script.angelscript.editor.ui;

import draconic.foundation;
import draconic.ui.toolkit;

using namespace draconic::foundation;

namespace draconic::script::angelscript
{
    namespace
    {
        namespace toolkit = draconic::ui::toolkit;

        constexpr StringView kAngelScriptKeywords[] = {
            u8"abstract",  u8"and",      u8"auto",      u8"break",     u8"case",    u8"cast",
            u8"class",     u8"const",    u8"continue",  u8"default",   u8"delete",  u8"do",
            u8"else",      u8"enum",     u8"explicit",  u8"external",  u8"false",   u8"final",
            u8"for",       u8"from",     u8"funcdef",   u8"function",  u8"get",     u8"if",
            u8"import",    u8"in",       u8"inout",     u8"interface", u8"is",      u8"mixin",
            u8"namespace", u8"not",      u8"null",      u8"or",        u8"out",     u8"override",
            u8"private",   u8"property", u8"protected", u8"return",    u8"set",     u8"shared",
            u8"super",     u8"switch",   u8"this",      u8"true",      u8"typedef", u8"while",
            u8"xor"};
        constexpr StringView kAngelScriptTypes[] = {
            u8"any",    u8"array", u8"bool",   u8"dictionary", u8"double", u8"float",
            u8"int",    u8"int16", u8"int32",  u8"int64",      u8"int8",   u8"ref",
            u8"string", u8"uint",  u8"uint16", u8"uint32",     u8"uint64", u8"uint8",
            u8"void"};

        UniquePtr<toolkit::ICodeLexer> MakeAngelScriptLexer()
        {
            toolkit::CLikeLexerSpec spec;
            spec.keywords =
                Span<const StringView>(kAngelScriptKeywords, ArrayCount(kAngelScriptKeywords));
            spec.types = Span<const StringView>(kAngelScriptTypes, ArrayCount(kAngelScriptTypes));
            spec.tripleQuotedStrings = true;
            spec.charLiterals = true;
            return UniquePtr<toolkit::ICodeLexer>(
                DefaultAllocator().New<toolkit::CLikeLexer>(spec), DefaultAllocator());
        }
    }

    void RegisterAngelScriptEditorUI()
    {
        auto& registry = toolkit::CodeLexerRegistry::Get();
        registry.Register(u8"angelscript", [] { return MakeAngelScriptLexer(); });
        registry.Register(u8"as", [] { return MakeAngelScriptLexer(); });
    }
}
