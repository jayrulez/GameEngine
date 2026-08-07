/// Draconic::UIToolkit - the `:code_document` partition.
///
/// The UI-free core of CodeEditView (docs/design/code-editor.md): a line-array text buffer with
/// (line, column) addressing, delta-based undo with typing coalescing, per-line markers
/// (breakpoints, diagnostics, execution line) that track edits, and the identifier harvest that
/// backs the document-word completion provider. Columns are CODEPOINT indices (the widget's
/// monospace fast path multiplies them by one advance); byte offsets appear only at the
/// column<->byte conversion helpers. The document never touches views, fonts, or the clipboard -
/// everything here is headless-testable.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.ui.toolkit:code_document;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{

    // ---- positions ---------------------------------------------------------------------------

    /// 0-based buffer position; `column` counts codepoints, not bytes.
    struct CodePosition
    {
        i32 line = 0;
        i32 column = 0;
    };

    [[nodiscard]] constexpr bool operator==(CodePosition a, CodePosition b) noexcept
    {
        return a.line == b.line && a.column == b.column;
    }
    [[nodiscard]] constexpr bool operator!=(CodePosition a, CodePosition b) noexcept
    {
        return !(a == b);
    }
    [[nodiscard]] constexpr bool operator<(CodePosition a, CodePosition b) noexcept
    {
        return a.line != b.line ? a.line < b.line : a.column < b.column;
    }
    [[nodiscard]] constexpr bool operator<=(CodePosition a, CodePosition b) noexcept
    {
        return !(b < a);
    }

    struct CodeSpan
    {
        CodePosition begin{};
        CodePosition end{};

        [[nodiscard]] constexpr bool IsEmpty() const noexcept { return begin == end; }
        [[nodiscard]] constexpr CodeSpan Normalized() const noexcept
        {
            return begin <= end ? *this : CodeSpan{end, begin};
        }
    };

    // ---- markers -----------------------------------------------------------------------------

    /// Per-line marker flags. Breakpoint/Modified are set directly; Error/Warning derive from the
    /// diagnostic list; ExecutionLine from the single execution-line slot. MarkersOn() returns
    /// the union so the gutter reads one mask.
    enum class CodeMarkers : u8
    {
        None = 0,
        Breakpoint = 1u << 0,
        Error = 1u << 1,
        Warning = 1u << 2,
        ExecutionLine = 1u << 3,
        Modified = 1u << 4,
    };

    [[nodiscard]] constexpr CodeMarkers operator|(CodeMarkers a, CodeMarkers b) noexcept
    {
        return static_cast<CodeMarkers>(static_cast<u8>(a) | static_cast<u8>(b));
    }
    [[nodiscard]] constexpr CodeMarkers operator&(CodeMarkers a, CodeMarkers b) noexcept
    {
        return static_cast<CodeMarkers>(static_cast<u8>(a) & static_cast<u8>(b));
    }
    constexpr CodeMarkers& operator|=(CodeMarkers& a, CodeMarkers b) noexcept
    {
        a = a | b;
        return a;
    }
    [[nodiscard]] constexpr bool HasMarker(CodeMarkers mask, CodeMarkers flag) noexcept
    {
        return (static_cast<u8>(mask) & static_cast<u8>(flag)) != 0u;
    }

    /// One compiler/validator message anchored to a line. Wholesale-replaced per validation run
    /// (SetDiagnostics); Error/Warning gutter bits derive from these.
    struct CodeDiagnostic
    {
        bool isError = true;
        i32 line = 0; // 0-based
        String message;
    };

    // ---- undo --------------------------------------------------------------------------------

    /// Classifies an edit for undo coalescing: consecutive edits of the same kind within
    /// kUndoCoalesceSeconds that are spatially contiguous merge into one undo entry
    /// (the EditText/TextEditingBehavior policy, kept identical on purpose).
    enum class CodeEditKind : u8
    {
        None,
        Typing,
        Backspace,
        Delete,
        Newline,
        Paste,
        Other,
    };

    /// Cursor + anchor snapshot stored with undo entries so undo/redo restores selection.
    struct CodeCursorState
    {
        CodePosition cursor{};
        CodePosition anchor{};
    };

    // ---- the document ------------------------------------------------------------------------

    class CodeDocument
    {
    public:
        static constexpr f64 kUndoCoalesceSeconds = 1.0;
        static constexpr usize kMaxUndoEntries = 256;

        CodeDocument() { m_lines.PushBack(String()); }

        // ---- content access ----

        [[nodiscard]] i32 LineCount() const noexcept { return static_cast<i32>(m_lines.Size()); }

        [[nodiscard]] StringView Line(i32 line) const
        {
            DRACONIC_ASSERT(line >= 0 && line < LineCount());
            return m_lines[static_cast<usize>(line)].AsView();
        }

        /// Codepoint count of a line.
        [[nodiscard]] i32 LineLength(i32 line) const
        {
            return static_cast<i32>(Utf8Length(Line(line)));
        }

        /// Whole buffer joined with '\n'.
        [[nodiscard]] String Text() const
        {
            String out;
            for (usize i = 0; i < m_lines.Size(); ++i)
            {
                if (i > 0)
                {
                    out.PushBack(u8'\n');
                }
                out.Append(m_lines[i].AsView());
            }
            return out;
        }

        /// Replaces the whole buffer (splits on '\n', drops '\r'). Clears undo history and
        /// version-derived caches; markers/diagnostics are cleared too - a full reload is a new
        /// document as far as line anchors are concerned.
        void SetText(StringView text)
        {
            m_lines.Clear();
            String current;
            for (usize i = 0; i < text.Size(); ++i)
            {
                const char8_t c = text[i];
                if (c == u8'\n')
                {
                    m_lines.PushBack(Move(current));
                    current = String();
                }
                else if (c != u8'\r')
                {
                    current.PushBack(c);
                }
            }
            m_lines.PushBack(Move(current));

            m_undo.Clear();
            m_redo.Clear();
            m_lastEditKind = CodeEditKind::None;
            m_lineMarkers.Clear();
            m_diagnostics.Clear();
            m_executionLine = -1;
            BumpVersion();
            if (OnLinesChanged)
            {
                OnLinesChanged(0, -1, LineCount());
            }
        }

        [[nodiscard]] String TextInSpan(const CodeSpan& span) const
        {
            const CodeSpan s = ClampSpan(span.Normalized());
            String out;
            if (s.begin.line == s.end.line)
            {
                AppendColumns(out, s.begin.line, s.begin.column, s.end.column);
                return out;
            }
            AppendColumns(out, s.begin.line, s.begin.column, LineLength(s.begin.line));
            for (i32 line = s.begin.line + 1; line < s.end.line; ++line)
            {
                out.PushBack(u8'\n');
                out.Append(Line(line));
            }
            out.PushBack(u8'\n');
            AppendColumns(out, s.end.line, 0, s.end.column);
            return out;
        }

        /// Monotonic content version; bumped on every mutation (caches key off it).
        [[nodiscard]] u64 Version() const noexcept { return m_version; }

        // ---- positions ----

        [[nodiscard]] CodePosition ClampPosition(CodePosition pos) const
        {
            if (pos.line < 0)
            {
                return CodePosition{0, 0};
            }
            if (pos.line >= LineCount())
            {
                return EndPosition();
            }
            if (pos.column < 0)
            {
                pos.column = 0;
            }
            const i32 length = LineLength(pos.line);
            if (pos.column > length)
            {
                pos.column = length;
            }
            return pos;
        }

        [[nodiscard]] CodePosition EndPosition() const
        {
            const i32 last = LineCount() - 1;
            return CodePosition{last, LineLength(last)};
        }

        /// Byte offset of a codepoint column within a line (clamped).
        [[nodiscard]] usize ColumnToByte(i32 line, i32 column) const
        {
            const StringView text = Line(line);
            usize index = 0;
            for (i32 c = 0; c < column && index < text.Size(); ++c)
            {
                (void)DecodeUtf8(text, index);
            }
            return index;
        }

        /// Codepoint column of a byte offset within a line (clamped; mid-sequence offsets round
        /// down to the containing codepoint).
        [[nodiscard]] i32 ByteToColumn(i32 line, usize byte) const
        {
            const StringView text = Line(line);
            const usize limit = byte < text.Size() ? byte : text.Size();
            usize index = 0;
            i32 column = 0;
            while (index < limit)
            {
                const usize before = index;
                (void)DecodeUtf8(text, index);
                if (index > limit && before < limit)
                {
                    break;
                }
                ++column;
            }
            return column;
        }

        // ---- word boundaries (Ctrl+arrows, double-click, completion prefix) ----

        [[nodiscard]] CodePosition PrevWordBoundary(CodePosition pos) const
        {
            pos = ClampPosition(pos);
            if (pos.column == 0)
            {
                if (pos.line == 0)
                {
                    return pos;
                }
                return CodePosition{pos.line - 1, LineLength(pos.line - 1)};
            }
            const StringView text = Line(pos.line);
            i32 column = pos.column;
            while (column > 0 && CharClassAt(text, column - 1) == CharClass::Space)
            {
                --column;
            }
            if (column > 0)
            {
                const CharClass cls = CharClassAt(text, column - 1);
                while (column > 0 && CharClassAt(text, column - 1) == cls)
                {
                    --column;
                }
            }
            return CodePosition{pos.line, column};
        }

        [[nodiscard]] CodePosition NextWordBoundary(CodePosition pos) const
        {
            pos = ClampPosition(pos);
            const i32 length = LineLength(pos.line);
            if (pos.column >= length)
            {
                if (pos.line >= LineCount() - 1)
                {
                    return pos;
                }
                return CodePosition{pos.line + 1, 0};
            }
            const StringView text = Line(pos.line);
            i32 column = pos.column;
            const CharClass cls = CharClassAt(text, column);
            while (column < length && CharClassAt(text, column) == cls)
            {
                ++column;
            }
            while (column < length && CharClassAt(text, column) == CharClass::Space)
            {
                ++column;
            }
            return CodePosition{pos.line, column};
        }

        /// The identifier-class run containing (or immediately left of) `pos`; empty span at
        /// `pos` when it touches no word character.
        [[nodiscard]] CodeSpan WordAt(CodePosition pos) const
        {
            pos = ClampPosition(pos);
            const StringView text = Line(pos.line);
            const i32 length = LineLength(pos.line);
            i32 probe = pos.column;
            if (probe >= length || CharClassAt(text, probe) != CharClass::Word)
            {
                if (probe == 0 || CharClassAt(text, probe - 1) != CharClass::Word)
                {
                    return CodeSpan{pos, pos};
                }
                --probe;
            }
            i32 begin = probe;
            while (begin > 0 && CharClassAt(text, begin - 1) == CharClass::Word)
            {
                --begin;
            }
            i32 end = probe;
            while (end < length && CharClassAt(text, end) == CharClass::Word)
            {
                ++end;
            }
            return CodeSpan{CodePosition{pos.line, begin}, CodePosition{pos.line, end}};
        }

        // ---- editing ----

        /// The single undoable mutation: replaces `span` with `text`, records/coalesces the undo
        /// entry, and returns the position just past the inserted text (the caller's new cursor;
        /// the entry's after-state is that position with no selection). `time` feeds coalescing -
        /// pass the UI clock, or synthetic times in tests.
        CodePosition Edit(const CodeSpan& span, StringView text, CodeEditKind kind,
                          const CodeCursorState& before, f64 time)
        {
            const CodeSpan target = ClampSpan(span.Normalized());
            EditOp op;
            op.pos = target.begin;
            op.removed = Replace(target, text, op.insertedEnd);
            op.inserted = String(text);

            m_redo.Clear();
            if (m_compoundOpen)
            {
                // Compound bracket: every edit inside lands in ONE undo entry (replace-all,
                // multi-cursor style operations). Coalescing state is untouched.
                if (!m_compoundEntryStarted)
                {
                    UndoEntry entry;
                    entry.before = before;
                    entry.kind = CodeEditKind::Other;
                    m_undo.PushBack(Move(entry));
                    m_compoundEntryStarted = true;
                }
                UndoEntry& last = m_undo[m_undo.Size() - 1];
                last.after = CodeCursorState{op.insertedEnd, op.insertedEnd};
                last.ops.PushBack(Move(op));
                m_lastEditKind = CodeEditKind::None;
                return last.ops[last.ops.Size() - 1].insertedEnd;
            }
            if (CanCoalesce(kind, target, time))
            {
                UndoEntry& last = m_undo[m_undo.Size() - 1];
                last.after = CodeCursorState{op.insertedEnd, op.insertedEnd};
                last.ops.PushBack(Move(op));
            }
            else
            {
                UndoEntry entry;
                entry.before = before;
                entry.after = CodeCursorState{op.insertedEnd, op.insertedEnd};
                entry.kind = kind;
                entry.ops.PushBack(Move(op));
                m_undo.PushBack(Move(entry));
                if (m_undo.Size() > kMaxUndoEntries)
                {
                    m_undo.RemoveAt(0);
                }
            }
            m_lastEditKind = kind;
            m_lastEditTime = time;
            m_lastEditEnd = m_undo[m_undo.Size() - 1].ops[m_undo[m_undo.Size() - 1].ops.Size() - 1]
                                .insertedEnd;
            m_lastEditBegin = target.begin;
            return m_lastEditEnd;
        }

        [[nodiscard]] bool CanUndo() const noexcept { return m_undo.Size() > 0; }
        [[nodiscard]] bool CanRedo() const noexcept { return m_redo.Size() > 0; }

        /// Reverts the newest undo entry; `out` receives the cursor state to restore.
        bool Undo(CodeCursorState& out)
        {
            if (m_undo.Size() == 0)
            {
                return false;
            }
            UndoEntry entry = Move(m_undo[m_undo.Size() - 1]);
            m_undo.PopBack();
            for (usize i = entry.ops.Size(); i > 0; --i)
            {
                EditOp& op = entry.ops[i - 1];
                CodePosition end{};
                (void)Replace(CodeSpan{op.pos, op.insertedEnd}, op.removed.AsView(), end);
            }
            out = entry.before;
            m_redo.PushBack(Move(entry));
            m_lastEditKind = CodeEditKind::None;
            return true;
        }

        bool Redo(CodeCursorState& out)
        {
            if (m_redo.Size() == 0)
            {
                return false;
            }
            UndoEntry entry = Move(m_redo[m_redo.Size() - 1]);
            m_redo.PopBack();
            for (usize i = 0; i < entry.ops.Size(); ++i)
            {
                EditOp& op = entry.ops[i];
                CodePosition removedEnd = AdvancePosition(op.pos, op.removed.AsView());
                (void)Replace(CodeSpan{op.pos, removedEnd}, op.inserted.AsView(), op.insertedEnd);
            }
            out = entry.after;
            m_undo.PushBack(Move(entry));
            m_lastEditKind = CodeEditKind::None;
            return true;
        }

        /// Splits the coalescing chain (call on cursor navigation, focus loss, save).
        void BreakUndoChain() noexcept { m_lastEditKind = CodeEditKind::None; }

        /// Groups every Edit() until EndCompoundEdit into ONE undo entry (replace-all). The
        /// entry's before-state comes from the first edit; no entry is pushed if none happen.
        void BeginCompoundEdit() noexcept
        {
            m_compoundOpen = true;
            m_compoundEntryStarted = false;
        }
        void EndCompoundEdit() noexcept
        {
            m_compoundOpen = false;
            m_compoundEntryStarted = false;
        }

        // ---- search (the find bar's model) ----

        /// Every occurrence of `query` (single-line, no '\n'), in document order. ASCII
        /// case-folding when insensitive; wholeWord bounds matches at word-class boundaries.
        void FindAll(StringView query, bool caseSensitive, bool wholeWord,
                     Array<CodeSpan>& outMatches) const
        {
            outMatches.Clear();
            if (query.IsEmpty())
            {
                return;
            }
            for (i32 line = 0; line < LineCount(); ++line)
            {
                const StringView text = Line(line);
                if (query.Size() > text.Size())
                {
                    continue;
                }
                for (usize i = 0; i + query.Size() <= text.Size(); ++i)
                {
                    if (!MatchesAt(text, i, query, caseSensitive))
                    {
                        continue;
                    }
                    if (wholeWord && !IsWordBoundedMatch(text, i, query.Size()))
                    {
                        continue;
                    }
                    const i32 fromColumn = ByteToColumn(line, i);
                    const i32 toColumn = ByteToColumn(line, i + query.Size());
                    outMatches.PushBack(CodeSpan{CodePosition{line, fromColumn},
                                                 CodePosition{line, toColumn}});
                    i += query.Size() - 1; // non-overlapping matches
                }
            }
        }

        // ---- brackets ----

        /// Codepoint at `pos` (0 at/past the end of the line - callers treat that as none).
        [[nodiscard]] u32 CodepointAt(CodePosition pos) const
        {
            if (pos.line < 0 || pos.line >= LineCount())
            {
                return 0;
            }
            const StringView text = Line(pos.line);
            usize index = ColumnToByte(pos.line, pos.column);
            if (index >= text.Size())
            {
                return 0;
            }
            return DecodeUtf8(text, index);
        }

        /// For a bracket at `bracketPos`, finds its partner (nesting-aware, whole document;
        /// P3 is lexer-blind - brackets inside strings/comments count too). False when the
        /// character is not a bracket or the partner is missing.
        [[nodiscard]] bool FindMatchingBracket(CodePosition bracketPos,
                                               CodePosition& outMatch) const
        {
            static constexpr char8_t kOpen[] = {u8'(', u8'[', u8'{'};
            static constexpr char8_t kClose[] = {u8')', u8']', u8'}'};
            const u32 at = CodepointAt(bracketPos);
            i32 pair = -1;
            bool forward = false;
            for (i32 i = 0; i < 3; ++i)
            {
                if (at == kOpen[i])
                {
                    pair = i;
                    forward = true;
                }
                if (at == kClose[i])
                {
                    pair = i;
                }
            }
            if (pair < 0)
            {
                return false;
            }
            const u32 open = kOpen[pair];
            const u32 close = kClose[pair];
            i32 depth = 0;
            CodePosition pos = bracketPos;
            while (true)
            {
                const u32 codepoint = CodepointAt(pos);
                if (codepoint == open)
                {
                    depth += forward ? 1 : -1;
                }
                else if (codepoint == close)
                {
                    depth += forward ? -1 : 1;
                }
                if (depth == 0 && !(pos == bracketPos))
                {
                    outMatch = pos;
                    return true;
                }
                CodePosition next = forward ? NextOnDocument(pos) : PreviousOnDocument(pos);
                if (next == pos)
                {
                    return false;
                }
                pos = next;
            }
        }

        // ---- markers ----

        void SetMarker(i32 line, CodeMarkers flag)
        {
            if (line < 0 || line >= LineCount())
            {
                return;
            }
            if (u8* mask = m_lineMarkers.Find(line))
            {
                *mask |= static_cast<u8>(flag);
            }
            else
            {
                m_lineMarkers.InsertOrAssign(line, static_cast<u8>(flag));
            }
        }

        void ClearMarker(i32 line, CodeMarkers flag)
        {
            if (u8* mask = m_lineMarkers.Find(line))
            {
                *mask &= static_cast<u8>(~static_cast<u8>(flag));
                if (*mask == 0)
                {
                    m_lineMarkers.Remove(line);
                }
            }
        }

        /// Toggles a direct marker; returns true when the marker is set after the call.
        bool ToggleMarker(i32 line, CodeMarkers flag)
        {
            if (HasMarker(DirectMarkersOn(line), flag))
            {
                ClearMarker(line, flag);
                return false;
            }
            SetMarker(line, flag);
            return true;
        }

        /// Union of direct markers, diagnostic-derived Error/Warning, and the execution line.
        [[nodiscard]] CodeMarkers MarkersOn(i32 line) const
        {
            CodeMarkers mask = DirectMarkersOn(line);
            for (usize i = 0; i < m_diagnostics.Size(); ++i)
            {
                if (m_diagnostics[i].line == line)
                {
                    mask |= m_diagnostics[i].isError ? CodeMarkers::Error : CodeMarkers::Warning;
                }
            }
            if (line == m_executionLine)
            {
                mask |= CodeMarkers::ExecutionLine;
            }
            return mask;
        }

        /// Every line with a direct marker of `flag` (breakpoint harvest for the debugger).
        void CollectMarkerLines(CodeMarkers flag, Array<i32>& outLines) const
        {
            outLines.Clear();
            for (const auto& entry : m_lineMarkers)
            {
                if ((entry.value & static_cast<u8>(flag)) != 0u)
                {
                    outLines.PushBack(entry.key);
                }
            }
            SortLines(outLines);
        }

        /// Wholesale-replaces the diagnostic list (one validation run). Lines are clamped.
        void SetDiagnostics(Array<CodeDiagnostic> diagnostics)
        {
            m_diagnostics = Move(diagnostics);
            for (usize i = 0; i < m_diagnostics.Size(); ++i)
            {
                if (m_diagnostics[i].line < 0)
                {
                    m_diagnostics[i].line = 0;
                }
                if (m_diagnostics[i].line >= LineCount())
                {
                    m_diagnostics[i].line = LineCount() - 1;
                }
            }
        }

        [[nodiscard]] Span<const CodeDiagnostic> Diagnostics() const noexcept
        {
            return Span<const CodeDiagnostic>(m_diagnostics.Data(), m_diagnostics.Size());
        }

        /// First diagnostic on a line, or null (row tooltip content).
        [[nodiscard]] const CodeDiagnostic* DiagnosticOn(i32 line) const
        {
            for (usize i = 0; i < m_diagnostics.Size(); ++i)
            {
                if (m_diagnostics[i].line == line)
                {
                    return &m_diagnostics[i];
                }
            }
            return nullptr;
        }

        /// The single paused-execution line; -1 clears.
        void SetExecutionLine(i32 line) noexcept
        {
            m_executionLine = (line >= 0 && line < LineCount()) ? line : -1;
        }
        [[nodiscard]] i32 ExecutionLine() const noexcept { return m_executionLine; }

        // ---- word harvest (document-word completion provider) ----

        /// Unique identifier-class words in the buffer, cached per content version.
        [[nodiscard]] Span<const String> Words() const
        {
            if (m_wordCacheVersion != m_version)
            {
                RebuildWordCache();
            }
            return Span<const String>(m_wordCache.Data(), m_wordCache.Size());
        }

        /// Fires after any line mutation: (firstLine, removedCount, addedCount). SetText reports
        /// (0, -1, LineCount()) - a full reload.
        Function<void(i32, i32, i32)> OnLinesChanged;

    private:
        enum class CharClass : u8
        {
            Space,
            Word,
            Punct,
        };

        struct EditOp
        {
            CodePosition pos{};         // where removed+inserted both start
            CodePosition insertedEnd{}; // end of `inserted` after apply (undo removes this span)
            String removed;
            String inserted;
        };

        struct UndoEntry
        {
            Array<EditOp> ops;
            CodeCursorState before{};
            CodeCursorState after{};
            CodeEditKind kind = CodeEditKind::None;
        };

        [[nodiscard]] static CharClass Classify(u32 codepoint) noexcept
        {
            if (codepoint == u8' ' || codepoint == u8'\t')
            {
                return CharClass::Space;
            }
            const bool word = (codepoint >= u8'a' && codepoint <= u8'z') ||
                              (codepoint >= u8'A' && codepoint <= u8'Z') ||
                              (codepoint >= u8'0' && codepoint <= u8'9') || codepoint == u8'_' ||
                              codepoint > 127u;
            return word ? CharClass::Word : CharClass::Punct;
        }

        [[nodiscard]] static char8_t FoldAsciiCase(char8_t c) noexcept
        {
            return (c >= u8'A' && c <= u8'Z') ? static_cast<char8_t>(c + 32) : c;
        }

        [[nodiscard]] static bool MatchesAt(StringView text, usize at, StringView query,
                                            bool caseSensitive) noexcept
        {
            for (usize k = 0; k < query.Size(); ++k)
            {
                const char8_t a = caseSensitive ? text[at + k] : FoldAsciiCase(text[at + k]);
                const char8_t b = caseSensitive ? query[k] : FoldAsciiCase(query[k]);
                if (a != b)
                {
                    return false;
                }
            }
            return true;
        }

        /// True when the [at, at+size) byte range does not butt against word-class characters.
        [[nodiscard]] static bool IsWordBoundedMatch(StringView text, usize at,
                                                     usize size) noexcept
        {
            if (at > 0)
            {
                const usize before = Utf8PrevBoundary(text, at);
                usize probe = before;
                if (Classify(DecodeUtf8(text, probe)) == CharClass::Word)
                {
                    return false;
                }
            }
            if (at + size < text.Size())
            {
                usize probe = at + size;
                if (Classify(DecodeUtf8(text, probe)) == CharClass::Word)
                {
                    return false;
                }
            }
            return true;
        }

        /// One codepoint forward/backward across line boundaries; clamps at the document ends.
        [[nodiscard]] CodePosition NextOnDocument(CodePosition pos) const
        {
            if (pos.column < LineLength(pos.line))
            {
                return CodePosition{pos.line, pos.column + 1};
            }
            if (pos.line + 1 < LineCount())
            {
                return CodePosition{pos.line + 1, 0};
            }
            return pos;
        }
        [[nodiscard]] CodePosition PreviousOnDocument(CodePosition pos) const
        {
            if (pos.column > 0)
            {
                return CodePosition{pos.line, pos.column - 1};
            }
            if (pos.line > 0)
            {
                return CodePosition{pos.line - 1, LineLength(pos.line - 1)};
            }
            return pos;
        }

        [[nodiscard]] CharClass CharClassAt(StringView text, i32 column) const
        {
            usize index = 0;
            for (i32 c = 0; c < column && index < text.Size(); ++c)
            {
                (void)DecodeUtf8(text, index);
            }
            if (index >= text.Size())
            {
                return CharClass::Space;
            }
            return Classify(DecodeUtf8(text, index));
        }

        [[nodiscard]] CodeSpan ClampSpan(const CodeSpan& span) const
        {
            return CodeSpan{ClampPosition(span.begin), ClampPosition(span.end)};
        }

        void AppendColumns(String& out, i32 line, i32 fromColumn, i32 toColumn) const
        {
            const StringView text = Line(line);
            const usize fromByte = ColumnToByte(line, fromColumn);
            const usize toByte = ColumnToByte(line, toColumn);
            out.Append(text.SubStr(fromByte, toByte - fromByte));
        }

        /// Position just past `text` when inserted at `pos`.
        [[nodiscard]] static CodePosition AdvancePosition(CodePosition pos, StringView text)
        {
            i32 addedLines = 0;
            usize lastLineStart = 0;
            for (usize i = 0; i < text.Size(); ++i)
            {
                if (text[i] == u8'\n')
                {
                    ++addedLines;
                    lastLineStart = i + 1;
                }
            }
            const StringView tail = text.SubStr(lastLineStart, text.Size() - lastLineStart);
            if (addedLines == 0)
            {
                return CodePosition{pos.line, pos.column + static_cast<i32>(Utf8Length(tail))};
            }
            return CodePosition{pos.line + addedLines, static_cast<i32>(Utf8Length(tail))};
        }

        /// The raw mutation: replaces a normalized clamped span, returns the removed text, sets
        /// `outInsertedEnd`, shifts markers, bumps the version, fires OnLinesChanged. Undo replay
        /// comes through here too (no recording at this level).
        String Replace(const CodeSpan& span, StringView text, CodePosition& outInsertedEnd)
        {
            String removed = TextInSpan(span);

            // Stitch: prefix of the first line + text + suffix of the last line.
            const usize prefixBytes = ColumnToByte(span.begin.line, span.begin.column);
            const usize suffixByte = ColumnToByte(span.end.line, span.end.column);
            const StringView firstLine = Line(span.begin.line);
            const StringView lastLine = Line(span.end.line);
            String prefix = String(firstLine.SubStr(0, prefixBytes));
            String suffix = String(lastLine.SubStr(suffixByte, lastLine.Size() - suffixByte));

            const i32 oldCount = span.end.line - span.begin.line + 1;

            // Build the replacement lines.
            Array<String> newLines;
            String current = Move(prefix);
            for (usize i = 0; i < text.Size(); ++i)
            {
                const char8_t c = text[i];
                if (c == u8'\n')
                {
                    newLines.PushBack(Move(current));
                    current = String();
                }
                else if (c != u8'\r')
                {
                    current.PushBack(c);
                }
            }
            outInsertedEnd = CodePosition{span.begin.line + static_cast<i32>(newLines.Size()),
                                          static_cast<i32>(Utf8Length(current.AsView()))};
            current.Append(suffix.AsView());
            newLines.PushBack(Move(current));

            const i32 newCount = static_cast<i32>(newLines.Size());

            // Splice into the buffer.
            for (i32 i = 0; i < oldCount; ++i)
            {
                m_lines.RemoveAt(static_cast<usize>(span.begin.line));
            }
            for (i32 i = newCount - 1; i >= 0; --i)
            {
                m_lines.Insert(static_cast<usize>(span.begin.line), Move(newLines[static_cast<usize>(i)]));
            }

            ShiftLineAnchors(span, newCount - oldCount);
            BumpVersion();
            if (OnLinesChanged)
            {
                OnLinesChanged(span.begin.line, oldCount, newCount);
            }
            return removed;
        }

        /// Marker policy on a span edit: lines strictly after the edited range shift by the line
        /// delta; markers on lines inside (begin, end] are dropped with their lines; begin.line's
        /// markers stay (the first line always survives a splice).
        void ShiftLineAnchors(const CodeSpan& span, i32 delta)
        {
            if (m_lineMarkers.Size() > 0)
            {
                HashMap<i32, u8> shifted;
                for (const auto& entry : m_lineMarkers)
                {
                    if (entry.key <= span.begin.line)
                    {
                        shifted.InsertOrAssign(entry.key, entry.value);
                    }
                    else if (entry.key > span.end.line)
                    {
                        shifted.InsertOrAssign(entry.key + delta, entry.value);
                    }
                }
                m_lineMarkers = Move(shifted);
            }
            for (usize i = 0; i < m_diagnostics.Size(); ++i)
            {
                if (m_diagnostics[i].line > span.end.line)
                {
                    m_diagnostics[i].line += delta;
                }
            }
            if (m_executionLine > span.end.line)
            {
                m_executionLine += delta;
            }
            else if (m_executionLine > span.begin.line && m_executionLine <= span.end.line)
            {
                m_executionLine = -1;
            }
        }

        [[nodiscard]] CodeMarkers DirectMarkersOn(i32 line) const
        {
            const u8* mask = m_lineMarkers.Find(line);
            return mask != nullptr ? static_cast<CodeMarkers>(*mask) : CodeMarkers::None;
        }

        [[nodiscard]] bool CanCoalesce(CodeEditKind kind, const CodeSpan& target, f64 time) const
        {
            if (m_undo.Size() == 0 || kind != m_lastEditKind)
            {
                return false;
            }
            if (kind != CodeEditKind::Typing && kind != CodeEditKind::Backspace &&
                kind != CodeEditKind::Delete)
            {
                return false;
            }
            if (time - m_lastEditTime > kUndoCoalesceSeconds)
            {
                return false;
            }
            // Spatial contiguity with the previous edit of the same kind.
            switch (kind)
            {
                case CodeEditKind::Typing:
                    return target.begin == m_lastEditEnd && target.IsEmpty();
                case CodeEditKind::Backspace:
                    return target.end == m_lastEditBegin;
                case CodeEditKind::Delete:
                    return target.begin == m_lastEditBegin;
                default:
                    return false;
            }
        }

        void BumpVersion() noexcept { ++m_version; }

        static void SortLines(Array<i32>& lines)
        {
            for (usize i = 1; i < lines.Size(); ++i)
            {
                const i32 value = lines[i];
                usize j = i;
                while (j > 0 && lines[j - 1] > value)
                {
                    lines[j] = lines[j - 1];
                    --j;
                }
                lines[j] = value;
            }
        }

        void RebuildWordCache() const
        {
            m_wordCache.Clear();
            HashSet<u64> seen;
            for (usize lineIndex = 0; lineIndex < m_lines.Size(); ++lineIndex)
            {
                const StringView text = m_lines[lineIndex].AsView();
                usize i = 0;
                while (i < text.Size())
                {
                    usize begin = i;
                    u32 cp = DecodeUtf8(text, i);
                    if (Classify(cp) != CharClass::Word || (cp >= u8'0' && cp <= u8'9'))
                    {
                        continue;
                    }
                    usize end = i;
                    while (end < text.Size())
                    {
                        usize probe = end;
                        if (Classify(DecodeUtf8(text, probe)) != CharClass::Word)
                        {
                            break;
                        }
                        end = probe;
                    }
                    const StringView word = text.SubStr(begin, end - begin);
                    if (seen.Insert(HashBytes(word.Data(), word.Size())))
                    {
                        m_wordCache.PushBack(String(word));
                    }
                    i = end;
                }
            }
            m_wordCacheVersion = m_version;
        }

        Array<String> m_lines; // never empty; one empty line = empty document
        u64 m_version = 0;

        Array<UndoEntry> m_undo;
        Array<UndoEntry> m_redo;
        bool m_compoundOpen = false;
        bool m_compoundEntryStarted = false;
        CodeEditKind m_lastEditKind = CodeEditKind::None;
        f64 m_lastEditTime = 0.0;
        CodePosition m_lastEditBegin{};
        CodePosition m_lastEditEnd{};

        HashMap<i32, u8> m_lineMarkers;
        Array<CodeDiagnostic> m_diagnostics;
        i32 m_executionLine = -1;

        mutable Array<String> m_wordCache;
        mutable u64 m_wordCacheVersion = static_cast<u64>(-1);
    };
}
