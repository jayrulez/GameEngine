// AngelScriptEditorUI: registering the lexer makes it resolvable under both the canonical
// "angelscript" id and the "as" alias, with AngelScript classification.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui.toolkit;
import draconic.script.angelscript.editor.ui;

using namespace draconic::foundation;
using namespace draconic::ui::toolkit;

TEST_CASE("angelscript-editor-ui: LexerRegistration")
{
    draconic::script::angelscript::RegisterAngelScriptEditorUI();
    CHECK(CodeLexerRegistry::Get().Create(u8"as").Get() != nullptr);
    UniquePtr<ICodeLexer> lexer = CodeLexerRegistry::Get().Create(u8"angelscript");
    REQUIRE(lexer.Get() != nullptr);

    Array<CodeToken> tokens;
    (void)lexer->LexLine(StringView(u8"int Update(float dt) override"), 0, tokens);
    REQUIRE(tokens.Size() >= 6);
    CHECK(tokens[0].kind == CodeTokenKind::Type);    // int
    CHECK(tokens[1].kind == CodeTokenKind::Default); // Update

    bool sawOverrideKeyword = false;
    for (usize i = 0; i < tokens.Size(); ++i)
    {
        sawOverrideKeyword = sawOverrideKeyword || tokens[i].kind == CodeTokenKind::Keyword;
    }
    CHECK(sawOverrideKeyword);

    // Block comments do NOT nest: the first terminator closes.
    tokens.Clear();
    CHECK(lexer->LexLine(StringView(u8"/* a /* b */ done"), 0, tokens) == 0);
}
