// WrenEditorUI: registering the Wren lexer makes it resolvable by language id, and the spec
// encodes the Wren quirks (nested block comments, """ raw strings).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui.toolkit;
import draconic.script.wren.editor.ui;

using namespace draconic::foundation;
using namespace draconic::ui::toolkit;

TEST_CASE("wren-editor-ui: LexerRegistration")
{
    draconic::script::wren::RegisterWrenEditorUI();
    UniquePtr<ICodeLexer> lexer = CodeLexerRegistry::Get().Create(u8"wren");
    REQUIRE(lexer.Get() != nullptr);

    // Keyword + builtin-class classification.
    Array<CodeToken> tokens;
    (void)lexer->LexLine(StringView(u8"var x = System"), 0, tokens);
    REQUIRE(tokens.Size() == 4);
    CHECK(tokens[0].kind == CodeTokenKind::Keyword); // var
    CHECK(tokens[3].kind == CodeTokenKind::Type);    // System

    // Nested block comments: one close leaves the comment open.
    tokens.Clear();
    const u32 open = lexer->LexLine(StringView(u8"/* a /* b */ still"), 0, tokens);
    CHECK(open != 0);
}
