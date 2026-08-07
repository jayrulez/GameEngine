// Draconic UI Toolkit - CodeHighlighter implementation (declared in :code_lexer).
//
// The incremental per-line cache: relex from the first invalid line, converging as soon as a
// cached line's entry state matches the incoming chain, lazily up to the requested line.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.ui.toolkit;

import draconic.foundation;

using namespace draconic::foundation;

namespace draconic::ui::toolkit
{
    void CodeHighlighter::SetLexer(ICodeLexer* lexer)
    {
        m_lexer = lexer;
        Reset(static_cast<i32>(m_lines.Size()));
    }

    void CodeHighlighter::Reset(i32 lineCount)
    {
        m_lines.Clear();
        for (i32 i = 0; i < lineCount; ++i)
        {
            m_lines.PushBack(LineCache{});
        }
        m_firstInvalid = 0;
    }

    void CodeHighlighter::OnLinesChanged(i32 first, i32 removed, i32 added)
    {
        if (removed < 0)
        {
            Reset(added);
            return;
        }
        for (i32 i = 0; i < removed && static_cast<usize>(first) < m_lines.Size(); ++i)
        {
            m_lines.RemoveAt(static_cast<usize>(first));
        }
        for (i32 i = 0; i < added; ++i)
        {
            m_lines.Insert(static_cast<usize>(first), LineCache{});
        }
        m_firstInvalid = Min(m_firstInvalid, first);
    }

    void CodeHighlighter::EnsureLexed(const CodeDocument& document, i32 upToLine)
    {
        if (m_lexer == nullptr)
        {
            return;
        }
        // Defensive resync (the owner normally keeps counts aligned via OnLinesChanged).
        while (m_lines.Size() < static_cast<usize>(document.LineCount()))
        {
            m_lines.PushBack(LineCache{});
        }
        while (m_lines.Size() > static_cast<usize>(document.LineCount()))
        {
            m_lines.PopBack();
        }

        const i32 count = static_cast<i32>(m_lines.Size());
        upToLine = Min(upToLine, count - 1);
        i32 line = m_firstInvalid;
        while (line < count)
        {
            const u32 entry = line > 0 ? m_lines[static_cast<usize>(line - 1)].exit : 0u;
            LineCache& cache = m_lines[static_cast<usize>(line)];
            if (cache.valid && cache.entry == entry)
            {
                // Chain is consistent here - skip ahead to the next explicitly invalidated
                // line (if any) and continue from there.
                i32 next = line + 1;
                while (next < count && m_lines[static_cast<usize>(next)].valid)
                {
                    ++next;
                }
                m_firstInvalid = next;
                if (next > upToLine)
                {
                    return;
                }
                line = next;
                continue;
            }
            cache.tokens.Clear();
            cache.entry = entry;
            cache.exit = m_lexer->LexLine(document.Line(line), entry, cache.tokens);
            cache.valid = true;
            ++m_lexLineCalls;
            ++line;
            if (line > upToLine)
            {
                // Lazy frontier: the cascade (if any) resumes from here next call.
                m_firstInvalid = line;
                return;
            }
        }
        m_firstInvalid = count;
    }

    Span<const CodeToken> CodeHighlighter::TokensFor(i32 line) const
    {
        if (line < 0 || static_cast<usize>(line) >= m_lines.Size() ||
            !m_lines[static_cast<usize>(line)].valid)
        {
            return Span<const CodeToken>();
        }
        const LineCache& cache = m_lines[static_cast<usize>(line)];
        return Span<const CodeToken>(cache.tokens.Data(), cache.tokens.Size());
    }
}
