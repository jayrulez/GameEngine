// Draconic::ScriptWrenEditorUI - the `draconic.script.wren.editor.ui` module.
//
// The Wren syntax tables + lexer registration. Wren quirks encoded in the spec: block
// comments NEST, and """ delimits raw (multi-line) strings; no preprocessor, no char
// literals.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.script.wren.editor.ui;

import draconic.foundation;
import draconic.ui.toolkit;

using namespace draconic::foundation;

namespace draconic::script::wren
{
    namespace
    {
        namespace toolkit = draconic::ui::toolkit;

        constexpr StringView kWrenKeywords[] = {
            u8"as",    u8"break", u8"class",   u8"construct", u8"continue", u8"else",
            u8"false", u8"for",   u8"foreign", u8"if",        u8"import",   u8"in",
            u8"is",    u8"null",  u8"return",  u8"static",    u8"super",    u8"this",
            u8"true",  u8"var",   u8"while"};
        constexpr StringView kWrenTypes[] = {
            u8"Bool", u8"Class",  u8"Fiber", u8"Fn",       u8"List",   u8"Map", u8"Null",
            u8"Num",  u8"Object", u8"Range", u8"Sequence", u8"String", u8"System"};

        UniquePtr<toolkit::ICodeLexer> MakeWrenLexer()
        {
            toolkit::CLikeLexerSpec spec;
            spec.keywords = Span<const StringView>(kWrenKeywords, ArrayCount(kWrenKeywords));
            spec.types = Span<const StringView>(kWrenTypes, ArrayCount(kWrenTypes));
            spec.nestedBlockComments = true;
            spec.tripleQuotedStrings = true;
            return UniquePtr<toolkit::ICodeLexer>(
                DefaultAllocator().New<toolkit::CLikeLexer>(spec), DefaultAllocator());
        }
    }

    void RegisterWrenEditorUI()
    {
        toolkit::CodeLexerRegistry::Get().Register(u8"wren", [] { return MakeWrenLexer(); });
    }
}
