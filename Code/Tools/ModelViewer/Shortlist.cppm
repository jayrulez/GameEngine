// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:shortlist`: the models picked from a collection
// (Documentation/Specs/model-viewer.md, P4). A model is starred, and may carry a note; the list is
// kept in a text file beside the viewer's settings (a line a model: the path, a tab, the note,
// with tabs, line breaks and backslashes escaped), and exports as CSV (name, path, note) for a
// spreadsheet or a purchase list.

module;
#include "Core/Prelude.h"

export module modelviewer:shortlist;

import foundation.core;
import :library;

using namespace foundation::core;

export namespace modelviewer
{
    struct ShortlistEntry
    {
        String path;
        String note;
    };

    class Shortlist
    {
    public:
        explicit Shortlist(IAllocator& allocator) : m_entries(allocator) {}

        [[nodiscard]] bool IsStarred(StringView path) const { return Find(path) >= 0; }
        [[nodiscard]] usize Count() const noexcept { return m_entries.Size(); }
        [[nodiscard]] const ShortlistEntry& At(usize index) const { return m_entries[index]; }

        /// Stars a model (at the end of the list) or takes it off (its note with it).
        void SetStarred(StringView path, bool starred)
        {
            const i32 at = Find(path);
            if (starred && at < 0)
                m_entries.PushBack(ShortlistEntry{String(path), {}});
            else if (!starred && at >= 0)
                m_entries.RemoveAt(static_cast<usize>(at));
        }

        [[nodiscard]] StringView Note(StringView path) const
        {
            const i32 at = Find(path);
            return at >= 0 ? m_entries[static_cast<usize>(at)].note.AsView() : StringView{};
        }
        /// A note on a model; writing one stars it.
        void SetNote(StringView path, StringView note)
        {
            if (Find(path) < 0)
                SetStarred(path, true);
            m_entries[static_cast<usize>(Find(path))].note = String(note);
        }

        /// The file's text: a line a model, its path, a tab, its note.
        [[nodiscard]] String Write() const
        {
            String text;
            for (const ShortlistEntry& entry : m_entries)
            {
                text += Escape(entry.path.AsView()).AsView();
                text += u8"\t";
                text += Escape(entry.note.AsView()).AsView();
                text += u8"\n";
            }
            return text;
        }

        /// Reads Write's text back, in place of what was there.
        void Read(StringView text)
        {
            m_entries.Clear();
            usize start = 0;
            while (start < text.Size())
            {
                usize end = start;
                while (end < text.Size() && text[end] != u8'\n')
                    ++end;
                const StringView line = text.SubStr(start, end - start);
                start = end + 1;
                if (line.IsEmpty())
                    continue;
                usize tab = 0;
                while (tab < line.Size() && line[tab] != u8'\t')
                    ++tab;
                const String path = Unescape(line.SubStr(0, tab));
                const String note = tab < line.Size() ? Unescape(line.SubStr(tab + 1, line.Size() - tab - 1)) : String{};
                if (!path.IsEmpty() && Find(path.AsView()) < 0)
                    m_entries.PushBack(ShortlistEntry{path, note});
            }
        }

        /// The list as CSV: a header, then a row a model (its file's name, its path, its note), a
        /// field quoted when it holds a comma, a quote or a line break.
        [[nodiscard]] String ToCsv() const
        {
            String csv(u8"name,path,note\n");
            for (const ShortlistEntry& entry : m_entries)
            {
                csv += CsvField(FileTitle(entry.path.AsView()).AsView()).AsView();
                csv += u8",";
                csv += CsvField(entry.path.AsView()).AsView();
                csv += u8",";
                csv += CsvField(entry.note.AsView()).AsView();
                csv += u8"\n";
            }
            return csv;
        }

        [[nodiscard]] static String CsvField(StringView value)
        {
            bool quote = false;
            for (const utf8char c : value)
                quote = quote || c == u8',' || c == u8'"' || c == u8'\n' || c == u8'\r';
            if (!quote)
                return String(value);
            String field(u8"\"");
            for (const utf8char c : value)
            {
                if (c == u8'"')
                    field += u8"\"\"";
                else
                    field += StringView(&c, 1);
            }
            field += u8"\"";
            return field;
        }

    private:
        [[nodiscard]] i32 Find(StringView path) const
        {
            for (usize i = 0; i < m_entries.Size(); ++i)
                if (m_entries[i].path.AsView() == path)
                    return static_cast<i32>(i);
            return -1;
        }

        [[nodiscard]] static String Escape(StringView value)
        {
            String out;
            for (const utf8char c : value)
            {
                if (c == u8'\\')
                    out += u8"\\\\";
                else if (c == u8'\t')
                    out += u8"\\t";
                else if (c == u8'\n')
                    out += u8"\\n";
                else if (c != u8'\r')
                    out += StringView(&c, 1);
            }
            return out;
        }
        [[nodiscard]] static String Unescape(StringView value)
        {
            String out;
            for (usize i = 0; i < value.Size(); ++i)
            {
                if (value[i] == u8'\\' && i + 1 < value.Size())
                {
                    ++i;
                    out += value[i] == u8't' ? StringView(u8"\t") : value[i] == u8'n' ? StringView(u8"\n") : value.SubStr(i, 1);
                    continue;
                }
                if (value[i] != u8'\r')
                    out += value.SubStr(i, 1);
            }
            return out;
        }

        Array<ShortlistEntry> m_entries;
    };
}
