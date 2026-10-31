// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:search`: finding models in the library. A query is a list of terms
// that must all match. A plain word matches a member's name, the file's name, the note or a tag,
// ignoring case; `tag:boss` matches a tag; `collection:act1` matches membership of that collection
// or one under it; `rigged`, `animated` and `missing` match the entry's state. A term with spaces
// is quoted (`tag:"night sky"`). A search runs over a collection and its sub-collections (members
// in their order), or over the whole library.

module;
#include "Core/Prelude.h"

export module modelviewer:search;

import foundation.core;
import :library;
import :store;

using namespace foundation::core;

export namespace modelviewer
{
    struct SearchQuery
    {
        Array<String> words;
        Array<String> tags;
        Array<String> collections;
        bool rigged = false;
        bool animated = false;
        bool missing = false;
    };

    /// Splits `text` into terms; quotes keep spaces inside one term.
    [[nodiscard]] inline SearchQuery ParseQuery(StringView text)
    {
        SearchQuery query;
        usize i = 0;
        while (i < text.Size())
        {
            while (i < text.Size() && (text[i] == u8' ' || text[i] == u8'\t'))
                ++i;
            if (i >= text.Size())
                break;
            String term;
            bool quoted = false;
            while (i < text.Size() && (quoted || (text[i] != u8' ' && text[i] != u8'\t')))
            {
                if (text[i] == u8'"')
                    quoted = !quoted;
                else
                    term += text.SubStr(i, 1);
                ++i;
            }
            const StringView t = term.AsView();
            if (t.StartsWith(u8"tag:"))
                query.tags.PushBack(String(t.SubStr(4, t.Size() - 4)));
            else if (t.StartsWith(u8"collection:"))
                query.collections.PushBack(String(t.SubStr(11, t.Size() - 11)));
            else if (t == StringView(u8"rigged"))
                query.rigged = true;
            else if (t == StringView(u8"animated"))
                query.animated = true;
            else if (t == StringView(u8"missing"))
                query.missing = true;
            else if (!t.IsEmpty())
                query.words.PushBack(term);
        }
        return query;
    }

    namespace search_detail
    {
        [[nodiscard]] inline bool WordMatches(const Library& library, const Entry& e, StringView word)
        {
            if (FileTitle(e.path.AsView()).AsView().ContainsIgnoreCase(word) || e.note.AsView().ContainsIgnoreCase(word))
                return true;
            for (const String& tag : e.tags)
                if (tag.AsView().ContainsIgnoreCase(word))
                    return true;
            for (const Collection& c : library.Collections())
                for (const Member& m : c.members)
                    if (m.entry == e.id && m.name.AsView().ContainsIgnoreCase(word))
                        return true;
            return false;
        }

        [[nodiscard]] inline bool InCollectionNamed(const Library& library, const Entry& e, StringView name)
        {
            for (const Collection& named : library.Collections())
            {
                if (!store_detail::SameIgnoreCase(named.name.AsView(), name))
                    continue;
                Array<Guid> under;
                library.Subtree(named.id, under);
                for (const Guid& g : under)
                    for (const Member& m : library.FindCollection(g)->members)
                        if (m.entry == e.id)
                            return true;
            }
            return false;
        }
    }

    /// Whether `e` meets every term of `query`.
    [[nodiscard]] inline bool MatchesQuery(const Library& library, const Entry& e, const SearchQuery& query)
    {
        if (query.rigged && !(e.stats.known && e.stats.rigged))
            return false;
        if (query.animated && !(e.stats.known && e.stats.clips > 0))
            return false;
        if (query.missing && !Library::IsMissing(e))
            return false;
        for (const String& tag : query.tags)
            if (!Library::HasTag(e, tag.AsView()))
                return false;
        for (const String& name : query.collections)
            if (!search_detail::InCollectionNamed(library, e, name.AsView()))
                return false;
        for (const String& word : query.words)
            if (!search_detail::WordMatches(library, e, word.AsView()))
                return false;
        return true;
    }

    /// The entries matching `query` within `scope` and its sub-collections (each entry once, in
    /// member order), or within the whole library in its order when `scope` is nil. At most
    /// `limit` are returned; 0 means no limit.
    [[nodiscard]] inline Array<Guid> Search(const Library& library, const SearchQuery& query, const Guid& scope = Guid{}, usize limit = 0)
    {
        Array<Guid> found;
        auto consider = [&](const Entry& e)
        {
            if (limit != 0 && found.Size() >= limit)
                return;
            for (const Guid& g : found)
                if (g == e.id)
                    return;
            if (MatchesQuery(library, e, query))
                found.PushBack(e.id);
        };
        if (scope.IsNil())
        {
            for (const Entry& e : library.Entries())
                consider(e);
            return found;
        }
        Array<Guid> under;
        library.Subtree(scope, under);
        for (const Guid& g : under)
            for (const Member& m : library.FindCollection(g)->members)
                if (const Entry* e = library.FindEntry(m.entry))
                    consider(*e);
        return found;
    }

    /// The models of `ids` that carry any of `tags` (ignoring case), in their order; all of them
    /// when `tags` is empty. This backs the Library tab's tag chips, where each active tag adds
    /// its models, unlike a query's `tag:` terms, which must all match.
    [[nodiscard]] inline Array<Guid> WithAnyTag(const Library& library, Span<const Guid> ids, Span<const String> tags)
    {
        Array<Guid> kept;
        for (const Guid& id : ids)
        {
            const Entry* e = library.FindEntry(id);
            bool tagged = tags.IsEmpty();
            for (usize i = 0; e != nullptr && !tagged && i < e->tags.Size(); ++i)
                for (const String& tag : tags)
                    tagged = tagged || (e->tags[i].Size() == tag.Size() && e->tags[i].AsView().ContainsIgnoreCase(tag.AsView()));
            if (e != nullptr && tagged)
                kept.PushBack(id);
        }
        return kept;
    }
}
