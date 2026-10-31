// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Code - :code_document partition
//
// A code editor's document: an array of lines, each holding its text and a hash, which a
// tokenizer caches its tokens by. Positions are (line, byte column), with the column on a
// character's boundary. Anchors are positions the document moves through each edit, biased left
// or right, so selections, diagnostics, markers and search hits keep their places while the text
// around them changes. Undo works in transactions of edits. Each edit records the range replaced,
// the text it held and the text put there, and each transaction records the selections before and
// after, so a multi-cursor undo puts every cursor back. Transactions of typing (or of deleting)
// that come within 300 ms of each other merge into one undo step. Time is measured on the
// document's clock, which its host advances.

module;
#include "Core/Prelude.h"

export module foundation.gui.code:code_document;

import foundation.core;
import foundation.gui; // NextGrapheme, PrevGrapheme

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// A place in a document: a line, and a byte column in it on a character's boundary.
    struct CodePosition
    {
        u32 Line = 0;
        u32 Column = 0;

        [[nodiscard]] constexpr bool operator==(const CodePosition&) const noexcept = default;
        [[nodiscard]] constexpr bool operator<(const CodePosition& o) const noexcept
        {
            return Line < o.Line || (Line == o.Line && Column < o.Column);
        }
        [[nodiscard]] constexpr bool operator<=(const CodePosition& o) const noexcept { return !(o < *this); }
    };

    /// A span of a document from start to end, with the end excluded. It is empty when the two
    /// are equal.
    struct CodeRange
    {
        CodePosition Start;
        CodePosition End;

        [[nodiscard]] constexpr bool IsEmpty() const noexcept { return Start == End; }
        [[nodiscard]] constexpr bool operator==(const CodeRange&) const noexcept = default;
        /// Returns the range with start before end, whichever order `a` and `b` came in.
        [[nodiscard]] static constexpr CodeRange Ordered(CodePosition a, CodePosition b) noexcept
        {
            return b < a ? CodeRange{b, a} : CodeRange{a, b};
        }
    };

    /// A selection: where it began (its anchor) and where its caret is (its head).
    struct CodeSelection
    {
        CodePosition Anchor;
        CodePosition Head;

        [[nodiscard]] constexpr bool IsEmpty() const noexcept { return Anchor == Head; }
        [[nodiscard]] constexpr CodeRange Range() const noexcept { return CodeRange::Ordered(Anchor, Head); }
        [[nodiscard]] constexpr bool operator==(const CodeSelection&) const noexcept = default;
    };

    /// What happens to an anchor when text is inserted exactly where it stands: the anchor either
    /// stays before the new text or moves past it.
    enum class CodeBias : u8
    {
        Left,  ///< the anchor stays and the text goes after it (a selection's start, a diagnostic's start)
        Right, ///< the text goes before the anchor, which moves past it (a caret after typing)
    };

    /// The kind of an edit, used to merge undo steps: typing merges with typing, and deleting with
    /// deleting.
    enum class CodeEditKind : u8
    {
        Other,
        Typing,
        Deleting,
    };

    /// An edit. Starting at `FirstLine`, `RemovedLines` lines were replaced by `InsertedLines`
    /// lines. More exactly, the range `Replaced` (in the text as it was) was replaced by text that
    /// now ends at `End`; another view sharing the document moves its own positions through that.
    struct CodeChange
    {
        u32 FirstLine = 0;
        u32 RemovedLines = 0;
        u32 InsertedLines = 0;
        CodeRange Replaced;
        CodePosition End;
    };

    using CodeAnchorId = u32;

    class CodeDocument
    {
    public:
        /// A transaction of typing (or of deleting) merges with the previous one when it comes within
        /// this many seconds.
        static constexpr f32 kMergeSeconds = 0.3f;

        explicit CodeDocument(IAllocator& allocator)
            : m_allocator(&allocator), m_lines(allocator), m_anchors(allocator), m_undo(allocator), m_redo(allocator)
        {
            m_lines.PushBack(Line{core::String{}, Hash(core::StringView{})});
        }

        // === The text ===
        /// Replaces the whole text, splitting lines at line feeds and dropping a carriage return
        /// before one. The undo history is cleared, and every anchor moves to the start.
        void SetText(core::StringView text)
        {
            const u32 removed = static_cast<u32>(m_lines.Size());
            m_lines.Clear();
            SplitInto(text, m_lines);
            for (Anchor& anchor : m_anchors)
                anchor.Position = CodePosition{};
            m_undo.Clear();
            m_redo.Clear();
            m_open = false;
            ++m_version;
            Changed(CodeChange{0, removed, static_cast<u32>(m_lines.Size()), CodeRange{}, End()});
        }
        /// Returns the whole text, with its lines joined by line feeds.
        [[nodiscard]] core::String GetText() const
        {
            core::String out;
            for (usize i = 0; i < m_lines.Size(); ++i)
            {
                if (i > 0)
                    out.Append(core::StringView(u8"\n"));
                out.Append(m_lines[i].Text.AsView());
            }
            return out;
        }
        [[nodiscard]] u32 LineCount() const noexcept { return static_cast<u32>(m_lines.Size()); }
        [[nodiscard]] core::StringView LineText(u32 line) const noexcept { return m_lines[line].Text.AsView(); }
        /// Returns the hash of a line's text, which a tokenizer keys its tokens by.
        [[nodiscard]] u64 LineHash(u32 line) const noexcept { return m_lines[line].Hash; }
        /// Increases with every change; a view compares it with the version it last drew.
        [[nodiscard]] u64 GetVersion() const noexcept { return m_version; }
        /// Returns the text in `range`, with lines joined by line feeds.
        [[nodiscard]] core::String TextIn(CodeRange range) const
        {
            range = CodeRange{Clamp(range.Start), Clamp(range.End)};
            if (range.End < range.Start)
                range = CodeRange{range.End, range.Start};
            core::String out;
            for (u32 line = range.Start.Line; line <= range.End.Line; ++line)
            {
                const core::StringView text = LineText(line);
                const u32 from = line == range.Start.Line ? range.Start.Column : 0u;
                const u32 to = line == range.End.Line ? range.End.Column : static_cast<u32>(text.Size());
                if (line > range.Start.Line)
                    out.Append(core::StringView(u8"\n"));
                out.Append(text.SubStr(from, to - from));
            }
            return out;
        }
        /// Returns the end of the text.
        [[nodiscard]] CodePosition End() const noexcept
        {
            return CodePosition{LineCount() - 1, static_cast<u32>(m_lines.Back().Text.Size())};
        }

        // === Search ===
        /// Finds the first match of `needle` at or after `from`; line feeds in `needle` span lines.
        /// With `wrap`, the search continues from the start of the text up to `from`. ASCII letters
        /// match either case unless `matchCase` is set. With `wholeWord`, a match counts only where
        /// no name character adjoins it on either side. Returns false when there is no match.
        bool Find(core::StringView needle, CodePosition from, bool matchCase, bool wholeWord, bool wrap, CodeRange& out) const
        {
            if (needle.Size() == 0)
                return false;
            from = Clamp(from);
            for (u32 line = from.Line; line < LineCount(); ++line)
                if (FindOnLine(needle, line, line == from.Line ? from.Column : 0u, ~u32(0), matchCase, wholeWord, out))
                    return true;
            if (!wrap)
                return false;
            for (u32 line = 0; line <= from.Line && line < LineCount(); ++line)
                if (FindOnLine(needle, line, 0, line == from.Line ? from.Column : ~u32(0), matchCase, wholeWord, out))
                    return true;
            return false;
        }

        // === Positions ===
        /// Returns `position` clamped into the text, with its column moved back to a character's
        /// boundary.
        [[nodiscard]] CodePosition Clamp(CodePosition position) const noexcept
        {
            if (position.Line >= LineCount())
                return End();
            const core::StringView text = LineText(position.Line);
            u32 column = core::Min(position.Column, static_cast<u32>(text.Size()));
            while (column > 0 && column < text.Size() && (static_cast<u8>(text[column]) & 0xC0u) == 0x80u)
                --column; // inside a character's bytes, so step back to its start
            return CodePosition{position.Line, column};
        }
        /// Returns the position after the next character. A character is a grapheme, so "é" written
        /// as e and its accent, or an emoji's parts, count as one. From a line's end it returns the
        /// next line's start; at the end of the text it stays.
        [[nodiscard]] CodePosition NextCharacter(CodePosition position) const noexcept
        {
            position = Clamp(position);
            const core::StringView text = LineText(position.Line);
            if (position.Column < text.Size())
                return CodePosition{position.Line, static_cast<u32>(NextGrapheme(text, position.Column))};
            return position.Line + 1 < LineCount() ? CodePosition{position.Line + 1, 0} : position;
        }
        /// Returns the position before the previous character. From a line's start it returns the
        /// previous line's end; at the start of the text it stays.
        [[nodiscard]] CodePosition PreviousCharacter(CodePosition position) const noexcept
        {
            position = Clamp(position);
            if (position.Column > 0)
                return CodePosition{position.Line, static_cast<u32>(PrevGrapheme(LineText(position.Line), position.Column))};
            return position.Line > 0 ? CodePosition{position.Line - 1, static_cast<u32>(LineText(position.Line - 1).Size())} : position;
        }

        // === Edits ===
        /// Replaces `range` with `text`, where line feeds in `text` make new lines. The edit joins
        /// the open transaction, or forms a transaction of its own when none is open. Returns the
        /// range `text` now occupies.
        CodeRange Replace(CodeRange range, core::StringView text, CodeEditKind kind = CodeEditKind::Other)
        {
            range = CodeRange::Ordered(Clamp(range.Start), Clamp(range.End));
            const bool own = !m_open;
            if (own)
                Begin(core::Span<const CodeSelection>{}, kind);
            Edit edit;
            edit.Before = range;
            edit.Removed = TextIn(range);
            edit.Inserted = Normalized(text);
            edit.After = Apply(range, edit.Inserted.AsView());
            m_undo.Back().Edits.PushBack(edit);
            m_redo.Clear();
            if (own)
                End(core::Span<const CodeSelection>{});
            return edit.After;
        }
        CodeRange Insert(CodePosition at, core::StringView text, CodeEditKind kind = CodeEditKind::Other)
        {
            return Replace(CodeRange{at, at}, text, kind);
        }
        void Erase(CodeRange range, CodeEditKind kind = CodeEditKind::Deleting) { (void)Replace(range, core::StringView{}, kind); }

        // === Undo ===
        /// Opens a transaction: the edits until End form one undo step, and undoing it restores the
        /// selections `before`. When `kind` is typing or deleting and the transaction comes within
        /// kMergeSeconds of the last one of the same kind, it continues that step instead.
        void Begin(core::Span<const CodeSelection> before, CodeEditKind kind = CodeEditKind::Other)
        {
            if (m_open)
            {
                ++m_depth;
                return;
            }
            m_open = true;
            m_depth = 0;
            const bool merges = kind != CodeEditKind::Other && !m_undo.IsEmpty() && m_undo.Back().Kind == kind && !m_breakNext &&
                                m_clock - m_undo.Back().At <= kMergeSeconds && m_redo.IsEmpty();
            m_breakNext = false;
            if (!merges)
            {
                Transaction transaction(*m_allocator);
                transaction.Kind = kind;
                for (const CodeSelection& selection : before)
                    transaction.Before.PushBack(selection);
                m_undo.PushBack(core::Move(transaction));
                if (m_undo.Size() > m_undoLimit)
                    m_undo.RemoveAt(0);
            }
        }
        /// Closes the transaction. Redoing it after an undo restores the selections `after`.
        void End(core::Span<const CodeSelection> after)
        {
            if (!m_open)
                return;
            if (m_depth > 0)
            {
                --m_depth;
                return;
            }
            m_open = false;
            Transaction& transaction = m_undo.Back();
            transaction.At = m_clock;
            transaction.After.Clear();
            for (const CodeSelection& selection : after)
                transaction.After.PushBack(selection);
            if (transaction.Edits.IsEmpty())
                m_undo.RemoveAt(m_undo.Size() - 1); // nothing changed, so there is no step
        }
        /// Makes the next transaction start a step of its own (for example, when the caret moved
        /// between two runs of typing).
        void BreakMerge() noexcept { m_breakNext = true; }
        /// Advances the clock that undo merging measures time on; the host calls it each frame.
        void AdvanceClock(f32 seconds) noexcept { m_clock += seconds; }

        [[nodiscard]] bool CanUndo() const noexcept { return !m_undo.IsEmpty() && !m_open; }
        [[nodiscard]] bool CanRedo() const noexcept { return !m_redo.IsEmpty() && !m_open; }
        /// Undoes one step and gives the selections as they were before it (empty when the step
        /// kept none). Returns false when there is nothing to undo.
        bool Undo(core::Array<CodeSelection>& selections)
        {
            if (!CanUndo())
                return false;
            Transaction transaction = core::Move(m_undo.Back());
            m_undo.RemoveAt(m_undo.Size() - 1);
            for (usize i = transaction.Edits.Size(); i-- > 0;)
            {
                const Edit& edit = transaction.Edits[i];
                (void)Apply(edit.After, edit.Removed.AsView());
            }
            selections = transaction.Before;
            m_redo.PushBack(core::Move(transaction));
            m_breakNext = true;
            return true;
        }
        /// Redoes one step and gives the selections as they were after it.
        bool Redo(core::Array<CodeSelection>& selections)
        {
            if (!CanRedo())
                return false;
            Transaction transaction = core::Move(m_redo.Back());
            m_redo.RemoveAt(m_redo.Size() - 1);
            for (const Edit& edit : transaction.Edits)
                (void)Apply(edit.Before, edit.Inserted.AsView());
            selections = transaction.After;
            m_undo.PushBack(core::Move(transaction));
            m_breakNext = true;
            return true;
        }
        /// Sets how many steps undo keeps (1000 by default).
        void SetUndoLimit(usize steps) noexcept { m_undoLimit = core::Max<usize>(1, steps); }
        [[nodiscard]] usize UndoCount() const noexcept { return m_undo.Size(); }

        // === Anchors ===
        /// Adds a position the document keeps up to date through edits.
        CodeAnchorId AddAnchor(CodePosition position, CodeBias bias = CodeBias::Left)
        {
            const Anchor anchor{Clamp(position), bias, true};
            for (usize i = 0; i < m_anchors.Size(); ++i)
                if (!m_anchors[i].Live)
                {
                    m_anchors[i] = anchor;
                    return static_cast<CodeAnchorId>(i);
                }
            m_anchors.PushBack(anchor);
            return static_cast<CodeAnchorId>(m_anchors.Size() - 1);
        }
        void RemoveAnchor(CodeAnchorId id) noexcept
        {
            if (id < m_anchors.Size())
                m_anchors[id].Live = false;
        }
        [[nodiscard]] CodePosition GetAnchor(CodeAnchorId id) const noexcept
        {
            return id < m_anchors.Size() ? m_anchors[id].Position : CodePosition{};
        }
        void SetAnchor(CodeAnchorId id, CodePosition position) noexcept
        {
            if (id < m_anchors.Size())
                m_anchors[id].Position = Clamp(position);
        }
        /// Returns where `position` moves to through an edit that replaced `before` with text ending
        /// at `after`. Anchors move this way, and a view moves positions it keeps outside the
        /// document the same way.
        [[nodiscard]] static CodePosition Moved(CodePosition position, CodeBias bias, CodeRange before, CodePosition after) noexcept
        {
            if (position < before.Start)
                return position; // before the edit
            if (position == before.Start && bias == CodeBias::Left)
                return position; // at the start with a left bias: inserted text goes after it
            if (position < before.End || (position == before.Start && before.IsEmpty()))
                return bias == CodeBias::Left ? before.Start : after; // inside what was replaced, or text inserted exactly at it
            // At or after the edit's end: carried along with the text that follows it.
            if (position.Line == before.End.Line)
                return CodePosition{after.Line, after.Column + (position.Column - before.End.Column)};
            return CodePosition{position.Line - before.End.Line + after.Line, position.Column};
        }

        /// Registers `callback` to hear about each change's lines (for a tokenizer's cache or a
        /// view's layouts; each editor sharing the document adds its own). Returns a token for
        /// RemoveChangeListener.
        u32 AddChangeListener(core::Function<void(const CodeChange&)> callback)
        {
            m_listeners.PushBack(Listener{++m_lastListener, core::Move(callback)});
            return m_lastListener;
        }
        void RemoveChangeListener(u32 token)
        {
            for (usize i = 0; i < m_listeners.Size(); ++i)
                if (m_listeners[i].Token == token)
                {
                    m_listeners.RemoveAt(i);
                    return;
                }
        }

    private:
        [[nodiscard]] static char8_t Folded(char8_t c, bool matchCase) noexcept
        {
            return !matchCase && c >= u8'A' && c <= u8'Z' ? static_cast<char8_t>(c - u8'A' + u8'a') : c;
        }
        [[nodiscard]] static bool IsNameChar(char8_t c) noexcept
        {
            return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') || (c >= u8'0' && c <= u8'9') || c == u8'_' ||
                   static_cast<u8>(c) >= 0x80u;
        }
        [[nodiscard]] static bool SameText(core::StringView a, core::StringView b, bool matchCase) noexcept
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); ++i)
                if (Folded(a[i], matchCase) != Folded(b[i], matchCase))
                    return false;
            return true;
        }
        // Finds a match beginning on `line` at a column in [first, last).
        bool FindOnLine(core::StringView needle, u32 line, u32 first, u32 last, bool matchCase, bool wholeWord, CodeRange& out) const
        {
            usize split = 0;
            while (split < needle.Size() && needle[split] != u8'\n')
                ++split;
            const core::StringView head = needle.SubStr(0, split);
            const core::StringView text = LineText(line);
            const bool spans = split < needle.Size();
            for (usize column = first; column + head.Size() <= text.Size() && column < last; ++column)
            {
                if ((static_cast<u8>(text[column]) & 0xC0u) == 0x80u)
                    continue; // inside a character
                if (spans ? column + head.Size() != text.Size() : false)
                    continue; // a match that spans lines must run to the end of its first line
                if (!SameText(text.SubStr(column, head.Size()), head, matchCase))
                    continue;
                CodePosition end{line, static_cast<u32>(column + head.Size())};
                if (spans)
                {
                    // Match each whole line in between, then the start of the last line.
                    usize at = split + 1;
                    u32 next = line + 1;
                    bool fits = true;
                    while (fits)
                    {
                        usize stop = at;
                        while (stop < needle.Size() && needle[stop] != u8'\n')
                            ++stop;
                        const core::StringView part = needle.SubStr(at, stop - at);
                        if (next >= LineCount())
                        {
                            fits = false;
                            break;
                        }
                        const core::StringView there = LineText(next);
                        if (stop < needle.Size())
                        {
                            fits = SameText(there, part, matchCase);
                            at = stop + 1;
                            ++next;
                            continue;
                        }
                        fits = part.Size() <= there.Size() && SameText(there.SubStr(0, part.Size()), part, matchCase);
                        end = CodePosition{next, static_cast<u32>(part.Size())};
                        break;
                    }
                    if (!fits)
                        continue;
                }
                if (wholeWord)
                {
                    const core::StringView endLine = LineText(end.Line);
                    if ((column > 0 && IsNameChar(text[column - 1])) || (end.Column < endLine.Size() && IsNameChar(endLine[end.Column])))
                        continue;
                }
                out = CodeRange{CodePosition{line, static_cast<u32>(column)}, end};
                return true;
            }
            return false;
        }
        struct Line
        {
            core::String Text;
            u64 Hash = 0;
        };
        struct Anchor
        {
            CodePosition Position;
            CodeBias Bias = CodeBias::Left;
            bool Live = false;
        };
        struct Edit
        {
            CodeRange Before;      // what was replaced
            core::String Removed;  // the text it held
            core::String Inserted; // the text put there
            CodeRange After;       // where that text lies
        };
        struct Transaction
        {
            explicit Transaction(IAllocator& allocator) : Edits(allocator), Before(allocator), After(allocator) {}
            core::Array<Edit> Edits;
            core::Array<CodeSelection> Before;
            core::Array<CodeSelection> After;
            CodeEditKind Kind = CodeEditKind::Other;
            f32 At = 0.0f; // the clock when the transaction last closed
        };

        [[nodiscard]] static u64 Hash(core::StringView text) noexcept { return core::HashBytes(text.Data(), text.Size()); }
        // Returns `text` with its line endings normalized to bare line feeds.
        [[nodiscard]] static core::String Normalized(core::StringView text)
        {
            core::String out;
            for (usize i = 0; i < text.Size(); ++i)
                if (!(text[i] == u8'\r' && i + 1 < text.Size() && text[i + 1] == u8'\n'))
                    out.Append(text.SubStr(i, 1));
            return out;
        }
        static void SplitInto(core::StringView text, core::Array<Line>& out)
        {
            usize start = 0;
            for (usize i = 0; i <= text.Size(); ++i)
                if (i == text.Size() || text[i] == u8'\n')
                {
                    usize end = i;
                    if (end > start && text[end - 1] == u8'\r')
                        --end;
                    const core::StringView line = text.SubStr(start, end - start);
                    out.PushBack(Line{core::String(line), Hash(line)});
                    start = i + 1;
                }
        }
        // Applies the edit: replaces the lines, moves the anchors and tells the listeners. Returns where
        // the new text lies.
        CodeRange Apply(CodeRange range, core::StringView text)
        {
            const core::String prefix(LineText(range.Start.Line).SubStr(0, range.Start.Column));
            const core::StringView endLine = LineText(range.End.Line);
            const core::String suffix(endLine.SubStr(range.End.Column, endLine.Size() - range.End.Column));
            core::Array<Line> made(*m_allocator);
            SplitInto(text, made);
            const CodePosition after{range.Start.Line + static_cast<u32>(made.Size()) - 1,
                                     static_cast<u32>((made.Size() == 1 ? prefix.Size() : 0u) + made.Back().Text.Size())};
            core::String first(prefix);
            first.Append(made[0].Text.AsView());
            made[0].Text = core::Move(first);
            made.Back().Text.Append(suffix.AsView());
            for (Line& line : made)
                line.Hash = Hash(line.Text.AsView());
            const u32 removed = range.End.Line - range.Start.Line + 1;
            for (u32 i = 0; i < removed; ++i)
                m_lines.RemoveAt(range.Start.Line);
            for (usize i = 0; i < made.Size(); ++i)
                m_lines.Insert(range.Start.Line + i, core::Move(made[i]));
            for (Anchor& anchor : m_anchors)
                if (anchor.Live)
                    anchor.Position = Moved(anchor.Position, anchor.Bias, range, after);
            ++m_version;
            Changed(CodeChange{range.Start.Line, removed, static_cast<u32>(made.Size()), range, after});
            return CodeRange{range.Start, after};
        }
        void Changed(const CodeChange& change)
        {
            for (usize i = 0; i < m_listeners.Size(); ++i)
                if (m_listeners[i].Callback)
                    m_listeners[i].Callback(change);
        }
        struct Listener
        {
            u32 Token = 0;
            core::Function<void(const CodeChange&)> Callback;
        };

        IAllocator* m_allocator;
        core::Array<Line> m_lines; // never empty: an empty document is one empty line
        core::Array<Anchor> m_anchors;
        core::Array<Transaction> m_undo;
        core::Array<Transaction> m_redo;
        bool m_open = false;
        u32 m_depth = 0;        // Begin calls inside the open transaction
        bool m_breakNext = false;
        f32 m_clock = 0.0f;
        usize m_undoLimit = 1000;
        u64 m_version = 0;
        core::Array<Listener> m_listeners;
        u32 m_lastListener = 0;
    };
}
