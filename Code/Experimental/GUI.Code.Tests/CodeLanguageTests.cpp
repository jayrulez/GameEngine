// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Code - a language's definition and its tokenizer (C2 of Specs/gui-lab-evolution-3.md): rules in
// contexts give runs their tokens, words name identifiers, a state carries a block comment and a
// long bracket from line to line; a definition read back from the engine's XML tokenizes alike; the
// editor keeps each line's tokens, tokenizing again only what changed or what a change reaches.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.xml;
import foundation.xml.serialization;
import experimental.gui;
import experimental.gui.code;

using namespace experimental::gui;
namespace core = foundation::core;
namespace xml = foundation::xml;
using core::u32;
using core::usize;

#define CHECK_RUNS(actual, expected)                                                      \
    do                                                                                    \
    {                                                                                     \
        const core::String runs_ = (actual);                                              \
        INFO(doctest::String(reinterpret_cast<const char*>(runs_.CStr())));               \
        CHECK(runs_ == core::String(expected));                                           \
    } while (false)

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    CodeRule Rule(CodeMatch match, const char8_t* text, CodeToken token, const char8_t* enter = u8"", core::u8 leave = 0)
    {
        CodeRule rule;
        rule.Match = match;
        rule.Text = core::String(text);
        rule.Token = token;
        rule.Enter = core::String(enter);
        rule.Leave = leave;
        return rule;
    }
    CodeContext Context(const char8_t* name, CodeToken token)
    {
        CodeContext context;
        context.Name = core::String(name);
        context.Token = token;
        return context;
    }

    // A small C-like language: comments, strings with escapes, `#` lines, keywords and types.
    CodeLanguage CLike()
    {
        CodeLanguage lang;
        lang.Id = core::String(u8"clike");
        lang.Words.PushBack(CodeWords{CodeToken::Keyword, core::String(u8"if else return for")});
        lang.Words.PushBack(CodeWords{CodeToken::Type, core::String(u8"int float")});
        CodeContext root = Context(u8"code", CodeToken::Text);
        CodeRule hash = Rule(CodeMatch::LineRest, u8"", CodeToken::Preprocessor);
        hash.LineStart = true;
        CodeRule pound = Rule(CodeMatch::Literal, u8"#", CodeToken::Preprocessor, u8"directive");
        pound.LineStart = true;
        root.Rules.PushBack(pound);
        root.Rules.PushBack(Rule(CodeMatch::Literal, u8"//", CodeToken::Comment, u8"line-comment"));
        root.Rules.PushBack(Rule(CodeMatch::Literal, u8"/*", CodeToken::Comment, u8"block-comment"));
        root.Rules.PushBack(Rule(CodeMatch::Literal, u8"\"", CodeToken::String, u8"string"));
        root.Rules.PushBack(Rule(CodeMatch::Identifier, u8"", CodeToken::Text));
        root.Rules.PushBack(Rule(CodeMatch::Number, u8"", CodeToken::Number));
        root.Rules.PushBack(Rule(CodeMatch::AnyOf, u8"+-*/=<>!&|", CodeToken::Operator));
        root.Rules.PushBack(Rule(CodeMatch::AnyOf, u8"(){}[];,.", CodeToken::Punctuation));
        lang.Contexts.PushBack(root);
        CodeContext line = Context(u8"line-comment", CodeToken::Comment);
        line.EndsAtLineEnd = true;
        lang.Contexts.PushBack(line);
        CodeContext block = Context(u8"block-comment", CodeToken::Comment);
        block.Rules.PushBack(Rule(CodeMatch::Literal, u8"*/", CodeToken::Comment, u8"", 1));
        lang.Contexts.PushBack(block);
        CodeContext string = Context(u8"string", CodeToken::String);
        string.Escape = core::String(u8"\\");
        string.EndsAtLineEnd = true;
        string.Rules.PushBack(Rule(CodeMatch::Literal, u8"\"", CodeToken::String, u8"", 1));
        lang.Contexts.PushBack(string);
        CodeContext directive = Context(u8"directive", CodeToken::Preprocessor);
        directive.EndsAtLineEnd = true;
        lang.Contexts.PushBack(directive);
        lang.Prepare();
        return lang;
    }

    // "token:text" for each run, space-separated: what a line tokenized to.
    core::String Runs(const CodeLanguage& lang, core::StringView line, CodeLexState& state)
    {
        core::Array<CodeSpan> spans{core::DefaultAllocator()};
        lang.Tokenize(line, state, spans);
        core::String out;
        const auto add = [&out](core::StringView token, core::StringView text)
        {
            if (!out.IsEmpty())
                out.Append(SV(u8" "));
            out.Append(token);
            out.Append(SV(u8":"));
            out.Append(text);
        };
        for (const CodeSpan& span : spans)
        {
            const core::StringView text = line.SubStr(span.Start, span.End - span.Start);
            if (span.Token != CodeToken::Text)
            {
                add(CodeTokenName(span.Token), text);
                continue;
            }
            // Plain text's runs join (the spaces between names too): each word apart here.
            usize at = 0;
            while (at < text.Size())
            {
                while (at < text.Size() && text[at] == u8' ')
                    ++at;
                usize end = at;
                while (end < text.Size() && text[end] != u8' ')
                    ++end;
                if (end > at)
                    add(SV(u8"text"), text.SubStr(at, end - at));
                at = end;
            }
        }
        return out;
    }
}

TEST_CASE("code language: rules give runs their tokens; words name identifiers; numbers, strings and escapes")
{
    const CodeLanguage lang = CLike();
    CodeLexState state;
    CHECK(Runs(lang, SV(u8"int x = 0x1F + 2.5f;"), state) ==
          core::String(u8"type:int text:x operator:= number:0x1F operator:+ number:2.5f punctuation:;"));
    CHECK(state.Depth == 0);
    CHECK(Runs(lang, SV(u8"return \"a\\\"b\"; // done"), state) ==
          core::String(u8"keyword:return string:\"a escape:\\\" string:b\" punctuation:; comment:// done"));
    CHECK(state.Depth == 0); // the line comment ends with its line
    CHECK_RUNS(Runs(lang, SV(u8"  #include <x>"), state), u8"preprocessor:#include <x>");
    CHECK_RUNS(Runs(lang, SV(u8"a # b"), state), u8"text:a text:# text:b"); // not first on its line
    CHECK_RUNS(Runs(lang, SV(u8"x2 = élan"), state), u8"text:x2 operator:= text:élan");
}

TEST_CASE("code language: a block comment carries to the next lines, the state saying so")
{
    const CodeLanguage lang = CLike();
    CodeLexState state;
    CHECK(Runs(lang, SV(u8"int a; /* start"), state) == core::String(u8"type:int text:a punctuation:; comment:/* start"));
    CHECK(state.Depth == 1);
    const CodeLexState inside = state;
    CHECK_RUNS(Runs(lang, SV(u8"still"), state), u8"comment:still");
    CHECK(state == inside);
    CHECK_RUNS(Runs(lang, SV(u8"end */ if"), state), u8"comment:end */ keyword:if");
    CHECK(state.Depth == 0);
}

TEST_CASE("code language: a long bracket closes at its own level (Luau's [==[ ]==])")
{
    CodeLanguage lang;
    CodeContext root = Context(u8"code", CodeToken::Text);
    root.Rules.PushBack(Rule(CodeMatch::LongOpen, u8"[", CodeToken::String, u8"long"));
    root.Rules.PushBack(Rule(CodeMatch::Identifier, u8"", CodeToken::Text));
    lang.Contexts.PushBack(root);
    CodeContext longString = Context(u8"long", CodeToken::String);
    longString.Rules.PushBack(Rule(CodeMatch::LongClose, u8"]", CodeToken::String, u8"", 1));
    lang.Contexts.PushBack(longString);
    lang.Prepare();
    CodeLexState state;
    CHECK_RUNS(Runs(lang, SV(u8"x [==[ a ]=] b"), state), u8"text:x string:[==[ a ]=] b");
    CHECK(state.Depth == 1);
    CHECK(state.Level == 2);
    CHECK_RUNS(Runs(lang, SV(u8"c ]==] y"), state), u8"string:c ]==] text:y");
    CHECK(state.Depth == 0);
}

TEST_CASE("code language: a definition written by the engine's XML and read back tokenizes alike")
{
    CodeLanguage lang = CLike();
    xml::XmlSerializer writer(core::DefaultAllocator());
    Serialize(writer, lang);
    core::String text;
    writer.GetOutput(text);
    REQUIRE(writer.IsOk());
    MESSAGE(doctest::String(reinterpret_cast<const char*>(text.CStr()))); // the file a definition's owner keeps

    xml::XmlDocument doc(core::DefaultAllocator());
    REQUIRE(doc.Parse(text.AsView()) == xml::XmlResult::Ok);
    xml::XmlSerializer reader(doc);
    CodeLanguage read;
    Serialize(reader, read);
    REQUIRE(reader.IsOk());
    CHECK(read.Id == core::String(u8"clike"));
    CHECK(read.Contexts.Size() == lang.Contexts.Size());
    CodeLexState a, b;
    const core::StringView line = SV(u8"float y = 1; /* c */ \"s\\n\"");
    CHECK(Runs(read, line, a) == Runs(lang, line, b));
    CHECK(a == b);
}

TEST_CASE("code editor: keeps each line's tokens, tokenizing again only what changed or what a change reaches")
{
    CodeLanguage lang = CLike();
    core::RefPtr<CodeEditor> held = core::MakeRef<CodeEditor>(core::DefaultAllocator());
    CodeEditor& editor = *held;
    core::String text;
    for (int i = 0; i < 50; ++i)
        text.Append(SV(u8"int a = 1;\n"));
    editor.SetText(text.AsView());
    editor.SetLanguage(&lang);
    REQUIRE(editor.TokensOf(10).Size() > 0);
    CHECK(editor.TokensOf(10)[0].Token == CodeToken::Type);
    const core::u64 first = editor.TokenizedCount();
    CHECK(first == 11u); // lines 0 to 10, as far as asked
    // An edit that leaves the state as it was: that line alone.
    editor.GetDocument().Replace(CodeRange{CodePosition{3, 8}, CodePosition{3, 9}}, SV(u8"2"));
    (void)editor.TokensOf(10);
    CHECK(editor.TokenizedCount() == first + 1);
    // Opening a block comment: the lines after it, as they are asked for, change their tokens.
    editor.GetDocument().Insert(CodePosition{5, 0}, SV(u8"/*"));
    CHECK(editor.TokensOf(8)[0].Token == CodeToken::Comment);
    CHECK(editor.TokenizedCount() == first + 1 + 4); // 5 to 8
    // Closing it: back as they were.
    editor.GetDocument().Insert(CodePosition{5, 2}, SV(u8"*/"));
    CHECK(editor.TokensOf(8)[0].Token == CodeToken::Type);
}
