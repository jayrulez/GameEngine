// CodeLexer headless tests: the C-like scanner's classification and every spec flag (nested
// block comments, triple-quoted strings, preprocessor lines, char literals), the XML lexer's
// stateful constructs, multi-line state carry, the CodeLexerRegistry seam, and
// CodeHighlighter's incremental relex-until-convergence (asserted with a call counter).
//
// Toolkit ships NO language tables (they live with the language-owning modules), so these
// tests drive the machinery with local specs.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui.toolkit;

using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

namespace
{
    constexpr StringView kTestKeywords[] = {u8"class", u8"if", u8"return", u8"var"};
    constexpr StringView kTestTypes[] = {u8"System", u8"float4", u8"int"};

    CLikeLexerSpec TestSpec(bool nestedComments = false, bool tripleStrings = false,
                            bool preprocessorLines = false, bool characterLiterals = false)
    {
        CLikeLexerSpec spec;
        spec.keywords = Span<const StringView>(kTestKeywords, ArrayCount(kTestKeywords));
        spec.types = Span<const StringView>(kTestTypes, ArrayCount(kTestTypes));
        spec.nestedBlockComments = nestedComments;
        spec.tripleQuotedStrings = tripleStrings;
        spec.hashPreprocessorLines = preprocessorLines;
        spec.charLiterals = characterLiterals;
        return spec;
    }

    struct Lexed
    {
        Array<CodeToken> tokens;
        u32 exitState = 0;
    };

    Lexed Lex(ICodeLexer& lexer, const char8_t* line, u32 entry = 0)
    {
        Lexed result;
        result.exitState = lexer.LexLine(StringView(line), entry, result.tokens);
        return result;
    }

    StringView TextOf(const char8_t* line, const CodeToken& token)
    {
        return StringView(line).SubStr(token.byteBegin, token.byteEnd - token.byteBegin);
    }

    const CodeToken* FindToken(const Lexed& lexed, const char8_t* line, const char8_t* text)
    {
        for (usize i = 0; i < lexed.tokens.Size(); ++i)
        {
            if (TextOf(line, lexed.tokens[i]) == StringView(text))
            {
                return &lexed.tokens[i];
            }
        }
        return nullptr;
    }
}

TEST_CASE("toolkit-codelexer: CLikeClassification")
{
    CLikeLexer lexer(TestSpec());

    const char8_t* line = u8"var count = 0x1F + 2.5e-3 // tail";
    const Lexed lexed = Lex(lexer, line);
    REQUIRE(FindToken(lexed, line, u8"var") != nullptr);
    CHECK(FindToken(lexed, line, u8"var")->kind == CodeTokenKind::Keyword);
    CHECK(FindToken(lexed, line, u8"count")->kind == CodeTokenKind::Default);
    CHECK(FindToken(lexed, line, u8"0x1F")->kind == CodeTokenKind::Number);
    CHECK(FindToken(lexed, line, u8"2.5e-3")->kind == CodeTokenKind::Number);
    CHECK(FindToken(lexed, line, u8"// tail")->kind == CodeTokenKind::Comment);
    CHECK(FindToken(lexed, line, u8"=")->kind == CodeTokenKind::Operator);
    CHECK(lexed.exitState == 0);

    const char8_t* typed = u8"System.print(\"hi \\\" there\")";
    const Lexed lexed2 = Lex(lexer, typed);
    CHECK(FindToken(lexed2, typed, u8"System")->kind == CodeTokenKind::Type);
    CHECK(FindToken(lexed2, typed, u8"(")->kind == CodeTokenKind::Punctuation);
    CHECK(FindToken(lexed2, typed, u8"\"hi \\\" there\"")->kind == CodeTokenKind::String);
}

TEST_CASE("toolkit-codelexer: BlockCommentsAcrossLines")
{
    SUBCASE("non-nesting closes on the first terminator")
    {
        CLikeLexer lexer(TestSpec(false));
        const Lexed open = Lex(lexer, u8"a /* outer /* inner");
        CHECK(open.exitState != 0);
        const char8_t* line = u8"done */ var x";
        const Lexed closed = Lex(lexer, line, open.exitState);
        CHECK(closed.exitState == 0);
        CHECK(FindToken(closed, line, u8"var")->kind == CodeTokenKind::Keyword);
    }

    SUBCASE("nesting needs a close per open")
    {
        CLikeLexer lexer(TestSpec(true));
        const Lexed open = Lex(lexer, u8"a /* outer /* inner"); // depth 2
        CHECK(open.exitState != 0);

        const Lexed still = Lex(lexer, u8"still */ inside", open.exitState); // depth 1
        CHECK(still.exitState != 0);
        REQUIRE(still.tokens.Size() == 1);
        CHECK(still.tokens[0].kind == CodeTokenKind::Comment);

        const char8_t* line = u8"done */ var x";
        const Lexed closed = Lex(lexer, line, still.exitState);
        CHECK(closed.exitState == 0);
        CHECK(FindToken(closed, line, u8"var")->kind == CodeTokenKind::Keyword);
    }
}

TEST_CASE("toolkit-codelexer: TripleQuotedStringsSpanLines")
{
    CLikeLexer lexer(TestSpec(false, true));

    const Lexed open = Lex(lexer, u8"var s = \"\"\"first");
    CHECK(open.exitState != 0);

    const Lexed middle = Lex(lexer, u8"raw \"quotes\" fine here", open.exitState);
    CHECK(middle.exitState != 0);
    REQUIRE(middle.tokens.Size() == 1);
    CHECK(middle.tokens[0].kind == CodeTokenKind::String);

    const char8_t* line = u8"end\"\"\" + tail";
    const Lexed closed = Lex(lexer, line, middle.exitState);
    CHECK(closed.exitState == 0);
    CHECK(FindToken(closed, line, u8"tail")->kind == CodeTokenKind::Default);
}

TEST_CASE("toolkit-codelexer: PreprocessorLinesAndCharLiterals")
{
    CLikeLexer lexer(TestSpec(false, false, true, true));

    const char8_t* pre = u8"  #include \"common.hlsli\" // note";
    const Lexed lexedPre = Lex(lexer, pre);
    REQUIRE(lexedPre.tokens.Size() == 1);
    CHECK(lexedPre.tokens[0].kind == CodeTokenKind::Preprocessor);

    const char8_t* line = u8"float4 c = 'x' + 1.0f;";
    const Lexed lexed = Lex(lexer, line);
    CHECK(FindToken(lexed, line, u8"float4")->kind == CodeTokenKind::Type);
    CHECK(FindToken(lexed, line, u8"'x'")->kind == CodeTokenKind::String);
    CHECK(FindToken(lexed, line, u8"1.0f")->kind == CodeTokenKind::Number);
}

TEST_CASE("toolkit-codelexer: XmlConstructs")
{
    XmlLexer lexer;

    const char8_t* line = u8"<Panel width=\"120\">hello</Panel>";
    const Lexed lexed = Lex(lexer, line);
    CHECK(FindToken(lexed, line, u8"Panel")->kind == CodeTokenKind::Tag);
    CHECK(FindToken(lexed, line, u8"width")->kind == CodeTokenKind::Attribute);
    CHECK(FindToken(lexed, line, u8"\"120\"")->kind == CodeTokenKind::String);
    CHECK(FindToken(lexed, line, u8"hello")->kind == CodeTokenKind::Default);
    CHECK(lexed.exitState == 0);

    // Declaration.
    const char8_t* decl = u8"<?xml version=\"1.0\"?>";
    const Lexed lexedDecl = Lex(lexer, decl);
    REQUIRE(lexedDecl.tokens.Size() == 1);
    CHECK(lexedDecl.tokens[0].kind == CodeTokenKind::Preprocessor);

    // Comment spanning lines.
    const Lexed open = Lex(lexer, u8"<!-- start");
    CHECK(open.exitState != 0);
    const char8_t* closeLine = u8"end --><Row/>";
    const Lexed closed = Lex(lexer, closeLine, open.exitState);
    CHECK(closed.exitState == 0);
    CHECK(FindToken(closed, closeLine, u8"end -->")->kind == CodeTokenKind::Comment);
    CHECK(FindToken(closed, closeLine, u8"Row")->kind == CodeTokenKind::Tag);

    // A tag left open at end of line carries its state (attributes continue next line).
    const Lexed openTag = Lex(lexer, u8"<Button");
    CHECK(openTag.exitState != 0);
    const char8_t* attrLine = u8"    label=\"Go\" />";
    const Lexed attrs = Lex(lexer, attrLine, openTag.exitState);
    CHECK(FindToken(attrs, attrLine, u8"label")->kind == CodeTokenKind::Attribute);
    CHECK(attrs.exitState == 0);
}

TEST_CASE("toolkit-codelexer: TokenColumnsAreCodepoints")
{
    CLikeLexer lexer(TestSpec());
    // The e-acute is 2 bytes / 1 column, so the number's column and byte offset diverge.
    const char8_t* line = u8"var héllo=1";
    const Lexed lexed = Lex(lexer, line);
    const CodeToken* number = FindToken(lexed, line, u8"1");
    REQUIRE(number != nullptr);
    CHECK(number->column == 10);
    CHECK(number->byteBegin == 11);
}

TEST_CASE("toolkit-codelexer: RegistrySeam")
{
    auto& registry = CodeLexerRegistry::Get();
    registry.Register(u8"testlang", []
                      {
                          return UniquePtr<ICodeLexer>(
                              foundation::DefaultAllocator().New<CLikeLexer>(TestSpec()),
                              foundation::DefaultAllocator());
                      });

    CHECK(registry.Create(u8"testlang").Get() != nullptr);
    CHECK(registry.Create(u8"TESTLANG").Get() != nullptr); // ids are case-insensitive
    CHECK(registry.Create(u8"nosuchlang").Get() == nullptr);

    // Re-registration replaces (last wins) - the null-factory variant disables it.
    registry.Register(u8"testlang", [] { return UniquePtr<ICodeLexer>(); });
    CHECK(registry.Create(u8"testlang").Get() == nullptr);
}

// ---- CodeHighlighter incrementality ----

namespace
{
    // Wraps a real lexer, counting LexLine calls (proves incrementality end to end).
    class CountingLexer final : public ICodeLexer
    {
    public:
        explicit CountingLexer(ICodeLexer& inner) : m_inner(&inner) {}
        u32 LexLine(StringView line, u32 entry, Array<CodeToken>& out) override
        {
            ++calls;
            return m_inner->LexLine(line, entry, out);
        }
        u64 calls = 0;

    private:
        ICodeLexer* m_inner;
    };
}

TEST_CASE("toolkit-codehighlighter: IncrementalRelexConverges")
{
    CLikeLexer clike(TestSpec());
    CountingLexer counting(clike);

    CodeDocument doc;
    doc.SetText(u8"var a = 1\nvar b = 2\nvar c = 3\nvar d = 4");

    CodeHighlighter highlighter;
    highlighter.SetLexer(&counting);
    highlighter.Reset(doc.LineCount());
    highlighter.EnsureLexed(doc, doc.LineCount() - 1);
    CHECK(counting.calls == 4); // initial full lex

    // Edit line 1 without changing its exit state: exactly ONE relex (convergence at line 2).
    (void)doc.Edit(CodeSpan{{1, 4}, {1, 5}}, StringView(u8"x"), CodeEditKind::Typing,
                   CodeCursorState{}, 0.0);
    highlighter.OnLinesChanged(1, 1, 1);
    highlighter.EnsureLexed(doc, doc.LineCount() - 1);
    CHECK(counting.calls == 5);

    // Open a block comment on line 0: the state cascades - every following line relexes...
    (void)doc.Edit(CodeSpan{{0, 9}, {0, 9}}, StringView(u8" /*"), CodeEditKind::Typing,
                   CodeCursorState{}, 1.0);
    highlighter.OnLinesChanged(0, 1, 1);
    highlighter.EnsureLexed(doc, doc.LineCount() - 1);
    CHECK(counting.calls == 9); // +4 (lines 0..3)
    REQUIRE(highlighter.TokensFor(3).Size() == 1);
    CHECK(highlighter.TokensFor(3)[0].kind == CodeTokenKind::Comment);

    // ...and closing it restores the tail to real tokens.
    (void)doc.Edit(CodeSpan{{0, 12}, {0, 12}}, StringView(u8"*/"), CodeEditKind::Typing,
                   CodeCursorState{}, 2.0);
    highlighter.OnLinesChanged(0, 1, 1);
    highlighter.EnsureLexed(doc, doc.LineCount() - 1);
    CHECK(counting.calls == 13);
    CHECK(highlighter.TokensFor(3)[0].kind == CodeTokenKind::Keyword); // "var" again
}

TEST_CASE("toolkit-codehighlighter: LineSpliceKeepsTailValid")
{
    CLikeLexer clike(TestSpec());
    CountingLexer counting(clike);

    CodeDocument doc;
    doc.SetText(u8"var a = 1\nvar b = 2\nvar c = 3");
    CodeHighlighter highlighter;
    highlighter.SetLexer(&counting);
    highlighter.Reset(doc.LineCount());
    highlighter.EnsureLexed(doc, doc.LineCount() - 1);
    const u64 initial = counting.calls;

    // Insert a line between 0 and 1: only the edited + new lines lex; the shifted tail
    // converges without relexing.
    (void)doc.Edit(CodeSpan{{0, 9}, {0, 9}}, StringView(u8"\nvar n = 9"), CodeEditKind::Paste,
                   CodeCursorState{}, 0.0);
    highlighter.OnLinesChanged(0, 1, 2);
    highlighter.EnsureLexed(doc, doc.LineCount() - 1);
    CHECK(counting.calls == initial + 2);
    CHECK(highlighter.TokensFor(3).Size() > 0); // old line 2's cache survived the shift
}

TEST_CASE("toolkit-codehighlighter: LazyFrontierResumes")
{
    CLikeLexer clike(TestSpec());
    CountingLexer counting(clike);

    CodeDocument doc;
    doc.SetText(u8"/* open\nline1\nline2\nline3 */\nvar tail = 1");
    CodeHighlighter highlighter;
    highlighter.SetLexer(&counting);
    highlighter.Reset(doc.LineCount());

    // Only the first two lines requested: exactly two lex calls (lazy tail).
    highlighter.EnsureLexed(doc, 1);
    CHECK(counting.calls == 2);
    CHECK(highlighter.TokensFor(2).Size() == 0); // not lexed yet

    // Scrolling further resumes from the frontier.
    highlighter.EnsureLexed(doc, 4);
    CHECK(counting.calls == 5);
    CHECK(highlighter.TokensFor(1)[0].kind == CodeTokenKind::Comment);
    REQUIRE(highlighter.TokensFor(4).Size() > 0);
    CHECK(highlighter.TokensFor(4)[0].kind == CodeTokenKind::Keyword);
}

TEST_CASE("toolkit-codeeditview: LexerWiredThroughEdits")
{
    // The view forwards document changes to its highlighter; tokens stay queryable.
    auto view = foundation::MakeRef<CodeEditView>(foundation::DefaultAllocator());
    view->SetLexer(UniquePtr<ICodeLexer>(foundation::DefaultAllocator().New<CLikeLexer>(TestSpec()),
                                         foundation::DefaultAllocator()));
    view->SetText(u8"var a = 1");
    view->Highlighter().EnsureLexed(view->Document(), 0);
    REQUIRE(view->Highlighter().TokensFor(0).Size() > 0);
    CHECK(view->Highlighter().TokensFor(0)[0].kind == CodeTokenKind::Keyword);

    // An edit through the document invalidates and relexes cleanly.
    (void)view->Document().Edit(CodeSpan{{0, 0}, {0, 3}}, StringView(u8"class"),
                                CodeEditKind::Other, CodeCursorState{}, 0.0);
    view->Highlighter().EnsureLexed(view->Document(), 0);
    CHECK(view->Highlighter().TokensFor(0)[0].kind == CodeTokenKind::Keyword); // "class"
}
