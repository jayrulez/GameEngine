// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :code_lexer partition (interface).
//
// The lexing seam of CodeEditView, built on the line-state
// model: a lexer receives ONE line plus the entry state (e.g. "inside a block comment") and
// returns styled token spans plus the exit state. CodeHighlighter caches per-line entry/exit
// states and tokens; an edit re-lexes from the edited line downward only until exit states
// converge (usually one line), lazily up to the last visible line.
//
// Implementations live beside this file: CodeHighlighter.cpp (the incremental cache),
// CLikeLexer.cpp (the configurable scanner behind AngelScript/Luau/HLSL), XmlLexer.cpp, and
// CodeLexerFactory.cpp (language-id mapping + the keyword tables).

module;
#include "Core/Prelude.h"

export module foundation.ui.toolkit:code_lexer;

import foundation.core;
import :code_document;

using namespace foundation::core;

export namespace foundation::ui::toolkit
{

    // ---- tokens ------------------------------------------------------------------------------

    enum class CodeTokenKind : u8
    {
        Default,      // identifiers, plain text - drawn in the theme text color
        Keyword,
        Type,         // builtin/primitive type names
        Number,
        String,
        Comment,
        Operator,
        Punctuation,
        Preprocessor, // HLSL # lines, XML <?...?> declarations
        Tag,          // XML element names
        Attribute,    // XML attribute names
    };

    /// One styled span within a line. Byte range indexes the line's UTF-8 text (for SubStr);
    /// `column` is the codepoint column of `byteBegin` (monospace x = column * advance).
    struct CodeToken
    {
        u32 byteBegin = 0;
        u32 byteEnd = 0;
        i32 column = 0;
        CodeTokenKind kind = CodeTokenKind::Default;
    };

    /// Lexes one line given the entry state; appends tokens and returns the exit state.
    /// State 0 is "default"; everything else is lexer-private. Contract: all VISIBLE content
    /// is covered by a token (the renderer draws only tokens); whitespace may be skipped.
    class ICodeLexer
    {
    public:
        virtual ~ICodeLexer() = default;
        virtual u32 LexLine(StringView line, u32 entryState, Array<CodeToken>& outTokens) = 0;

        /// The language's line-comment marker (comment-toggle uses it). Empty disables the
        /// toggle (XML has only block comments).
        [[nodiscard]] virtual StringView LineCommentPrefix() const { return u8"//"; }
    };

    // ---- the incremental per-line cache ------------------------------------------------------

    /// Per-line token/state cache over a CodeDocument. The owner forwards the document's
    /// OnLinesChanged events here; EnsureLexed then re-lexes lazily from the first invalid
    /// line, stopping as soon as a cached line's entry state matches the incoming chain
    /// (convergence) or the requested line is covered (the tail resumes when scrolled to).
    class CodeHighlighter
    {
    public:
        /// Borrowed; null disables highlighting and clears the cache.
        void SetLexer(ICodeLexer* lexer);
        [[nodiscard]] bool HasLexer() const noexcept { return m_lexer != nullptr; }

        /// Full invalidation with a new line count (SetText / SetLexer).
        void Reset(i32 lineCount);

        /// Mirror of CodeDocument::OnLinesChanged: `removed` lines at `first` were replaced by
        /// `added` lines (removed == -1 signals a full reload).
        void OnLinesChanged(i32 first, i32 removed, i32 added);

        /// Brings lines [0, upToLine] up to date (plus whatever upstream cascade they need).
        void EnsureLexed(const CodeDocument& document, i32 upToLine);

        /// Valid only after EnsureLexed covered `line`; empty otherwise.
        [[nodiscard]] Span<const CodeToken> TokensFor(i32 line) const;

        /// Total LexLine invocations (tests assert incrementality with this).
        [[nodiscard]] u64 LexLineCallCount() const noexcept { return m_lexLineCalls; }

    private:
        struct LineCache
        {
            u32 entry = 0;
            u32 exit = 0;
            Array<CodeToken> tokens;
            bool valid = false;
        };

        ICodeLexer* m_lexer = nullptr; // borrowed
        Array<LineCache> m_lines;
        i32 m_firstInvalid = 0;
        u64 m_lexLineCalls = 0;
    };

    // ---- the configurable C-like lexer (AngelScript / HLSL) --------------------------

    struct CLikeLexerSpec
    {
        Span<const StringView> keywords;
        Span<const StringView> types;
        bool nestedBlockComments = false; // nested block comments: /* */ nests
        bool tripleQuotedStrings = false; // AngelScript heredocs (""" ... """)
        bool hashPreprocessorLines = false; // HLSL: a line whose first glyph is '#'
        bool charLiterals = false;          // 'x' (AngelScript, HLSL)
    };

    class CLikeLexer final : public ICodeLexer
    {
    public:
        explicit CLikeLexer(const CLikeLexerSpec& spec);
        u32 LexLine(StringView line, u32 entryState, Array<CodeToken>& out) override;

    private:
        // Scan helpers are free functions in CLikeLexer.cpp (shared cursor machinery in the
        // internal :code_lexer_scan partition); the class carries only its configuration.
        CLikeLexerSpec m_spec;
        HashSet<u64> m_keywords;
        HashSet<u64> m_types;
    };

    // ---- the configurable Lua-like lexer (Luau) ---------------------------------------------

    // Lua-family syntax the C-like scanner cannot express: `--` line comments, `--[[ ]]` /
    // `--[=[ ]=]` long (level-bracketed) block comments, `[[ ]]` / `[=[ ]=]` long strings, and
    // backtick interpolated strings. Keyword/type tables come from the OWNER, like CLikeLexer -
    // the toolkit ships no language tables. Comment-toggle (Ctrl+/) uses `--`.
    struct LuaLikeLexerSpec
    {
        Span<const StringView> keywords;
        Span<const StringView> types;
    };

    class LuaLikeLexer final : public ICodeLexer
    {
    public:
        explicit LuaLikeLexer(const LuaLikeLexerSpec& spec);
        u32 LexLine(StringView line, u32 entryState, Array<CodeToken>& out) override;
        [[nodiscard]] StringView LineCommentPrefix() const override { return u8"--"; }

    private:
        HashSet<u64> m_keywords;
        HashSet<u64> m_types;
    };

    // ---- XML lexer (UI documents, scene XML) ------------------------------------------------

    class XmlLexer final : public ICodeLexer
    {
    public:
        u32 LexLine(StringView line, u32 entryState, Array<CodeToken>& out) override;
        [[nodiscard]] StringView LineCommentPrefix() const override { return StringView(); }

    private:
        static constexpr u32 kModeText = 0;
        static constexpr u32 kModeInTag = 1;
        static constexpr u32 kModeComment = 2;
        static constexpr u32 kModeCData = 3;
        static constexpr u32 kModeDeclaration = 4;
        static constexpr u32 kModeDoubleQuote = 5;
        static constexpr u32 kModeSingleQuote = 6;
    };

    // ---- language registry -------------------------------------------------------------------

    /// Language-id -> lexer-factory registry. TOOLKIT SHIPS NO LANGUAGE TABLES: the modules
    /// that OWN a language register here (editor.script registers "angelscript" and
    /// "luau" beside the page; the shader page will bring "hlsl") - the same layering
    /// as ICompletionProvider, where reflection-fed providers plug in from outside. Pages
    /// with static knowledge (the XML document page) construct their lexer directly instead.
    /// Ids are ASCII case-insensitive; Create returns null for unknown ids (unstyled text).
    class CodeLexerRegistry
    {
    public:
        [[nodiscard]] static CodeLexerRegistry& Get();

        /// Last registration for an id wins.
        void Register(StringView languageId, Function<UniquePtr<ICodeLexer>()> factory);
        [[nodiscard]] UniquePtr<ICodeLexer> Create(StringView languageId) const;

    private:
        struct Entry
        {
            String id;
            Function<UniquePtr<ICodeLexer>()> factory;
        };
        Array<Entry> m_entries;
    };
}
