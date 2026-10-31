// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:store`: the model library. An entry is a kept model: a stable id, its
// path (with the file's size and time when it was added or last relinked, so a moved or changed
// file is noticed), its tags, a note, and the stats its last cook counted. A collection has a name,
// a parent (nil at the top level) and an ordered list of members. A member is an entry plus the
// name it has in that collection; the name is unique, ignoring case, across the collection's
// top-level collection and everything under it, so a game's ids never clash. Deleting a collection
// deletes its sub-collections and their memberships, never an entry. The library reads and writes
// itself as XML with the engine's serializer, version first. LibraryFile keeps it in the user data
// folder, writing through a temporary file and a rename, and holds the system's file lock so only
// one viewer at a time writes it.

module;
#include "Core/Prelude.h"

export module modelviewer:store;

import foundation.core;
import foundation.xml;
import foundation.xml.serialization;
import :library;

using namespace foundation::core;

export namespace modelviewer
{
    /// The stats an entry's last cook counted, so search works without cooking.
    struct EntryStats
    {
        bool known = false;
        bool rigged = false;
        u32 clips = 0;
        u32 triangles = 0;
        Float3 size{};
    };

    struct Entry
    {
        Guid id;
        String path;
        u64 size = 0;     // the file's size and time when added or relinked
        i64 modified = 0;
        Array<String> tags;
        String note;
        EntryStats stats;
    };

    struct Member
    {
        Guid entry;
        String name; // the id a game knows it by in this collection (empty: the file's name is shown)
    };

    struct Collection
    {
        Guid id;
        String name;
        Guid parent; // nil for a top-level collection
        Array<Member> members;
    };

    inline void Serialize(ISerializer& ar, EntryStats& s)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "known", s.known);
        foundation::core::Serialize(ar, "rigged", s.rigged);
        foundation::core::Serialize(ar, "clips", s.clips);
        foundation::core::Serialize(ar, "triangles", s.triangles);
        foundation::core::Serialize(ar, "size", s.size);
        ar.EndObject();
    }
    inline void Serialize(ISerializer& ar, Entry& e)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "id", e.id);
        foundation::core::Serialize(ar, "path", e.path);
        foundation::core::Serialize(ar, "size", e.size);
        foundation::core::Serialize(ar, "modified", e.modified);
        foundation::core::Serialize(ar, "tags", e.tags);
        foundation::core::Serialize(ar, "note", e.note);
        foundation::core::Serialize(ar, "stats", e.stats);
        ar.EndObject();
    }
    inline void Serialize(ISerializer& ar, Member& m)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "entry", m.entry);
        foundation::core::Serialize(ar, "name", m.name);
        ar.EndObject();
    }
    inline void Serialize(ISerializer& ar, Collection& c)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "id", c.id);
        foundation::core::Serialize(ar, "name", c.name);
        foundation::core::Serialize(ar, "parent", c.parent);
        foundation::core::Serialize(ar, "members", c.members);
        ar.EndObject();
    }

    namespace store_detail
    {
        [[nodiscard]] inline bool SameIgnoreCase(StringView a, StringView b)
        {
            if (a.Size() != b.Size())
                return false;
            for (usize i = 0; i < a.Size(); ++i)
            {
                utf8char x = a[i];
                utf8char y = b[i];
                if (x >= u8'A' && x <= u8'Z')
                    x = static_cast<utf8char>(x - u8'A' + u8'a');
                if (y >= u8'A' && y <= u8'Z')
                    y = static_cast<utf8char>(y - u8'A' + u8'a');
                if (x != y)
                    return false;
            }
            return true;
        }
    }

    class Library
    {
    public:
        /// The file format's version, raised when the format changes; older files still read.
        static constexpr i32 kVersion = 1;

        explicit Library(IAllocator& allocator) : m_entries(allocator), m_collections(allocator)
        {
            // The ids need to be unique, not reproducible, so the generator is seeded from the system.
            u64 seed = 0;
            if (FillEntropy(Span<byte>(reinterpret_cast<byte*>(&seed), sizeof(seed))))
                m_random = Random(seed);
        }

        // === Reading ===
        [[nodiscard]] Span<const Entry> Entries() const { return m_entries.AsSpan(); }
        [[nodiscard]] Span<const Collection> Collections() const { return m_collections.AsSpan(); }
        [[nodiscard]] const Entry* FindEntry(const Guid& id) const
        {
            const i32 at = EntryIndex(id);
            return at >= 0 ? &m_entries[static_cast<usize>(at)] : nullptr;
        }
        [[nodiscard]] const Entry* FindEntryByPath(StringView path) const
        {
            for (const Entry& e : m_entries)
                if (e.path.AsView() == path)
                    return &e;
            return nullptr;
        }
        [[nodiscard]] const Collection* FindCollection(const Guid& id) const
        {
            const i32 at = CollectionIndex(id);
            return at >= 0 ? &m_collections[static_cast<usize>(at)] : nullptr;
        }
        /// The first collection in tree order with this name, ignoring case.
        [[nodiscard]] const Collection* FindCollectionByName(StringView name) const
        {
            for (const Collection& c : m_collections)
                if (store_detail::SameIgnoreCase(c.name.AsView(), name))
                    return &c;
            return nullptr;
        }

        /// The top-level collection that contains `id` (itself when it is top-level); nil when
        /// there is none.
        [[nodiscard]] Guid TopOf(const Guid& id) const
        {
            Guid at = id;
            for (usize guard = 0; guard <= m_collections.Size(); ++guard)
            {
                const Collection* c = FindCollection(at);
                if (c == nullptr)
                    return Guid{};
                if (c->parent.IsNil())
                    return c->id;
                at = c->parent;
            }
            return Guid{};
        }
        /// `id` and every collection under it.
        void Subtree(const Guid& id, Array<Guid>& out) const
        {
            if (FindCollection(id) == nullptr)
                return;
            out.PushBack(id);
            for (usize i = out.Size() - 1; i < out.Size(); ++i)
                for (const Collection& c : m_collections)
                    if (c.parent == out[i])
                        out.PushBack(c.id);
        }
        [[nodiscard]] bool IsUnder(const Guid& collection, const Guid& ancestor) const
        {
            Array<Guid> under;
            Subtree(ancestor, under);
            for (const Guid& g : under)
                if (g == collection)
                    return true;
            return false;
        }

        /// Whether the entry's file is missing from its path.
        [[nodiscard]] static bool IsMissing(const Entry& e) { return !FileExists(e.path.AsView()); }
        /// Whether the file at the entry's path differs from the one added (its size or time differ).
        [[nodiscard]] static bool IsChanged(const Entry& e)
        {
            u64 size = 0;
            i64 modified = 0;
            return FileStat(e.path.AsView(), size, modified) && (size != e.size || modified != e.modified);
        }

        /// An entry's name in a collection; empty when it is not a member or has no name there.
        [[nodiscard]] StringView MemberName(const Guid& collection, const Guid& entry) const
        {
            const Collection* c = FindCollection(collection);
            if (c != nullptr)
                for (const Member& m : c->members)
                    if (m.entry == entry)
                        return m.name.AsView();
            return {};
        }
        /// The collections an entry is a member of.
        void CollectionsOf(const Guid& entry, Array<Guid>& out) const
        {
            for (const Collection& c : m_collections)
                for (const Member& m : c.members)
                    if (m.entry == entry)
                    {
                        out.PushBack(c.id);
                        break;
                    }
        }
        /// Every tag with the number of entries carrying it, in first-seen order. Tags that differ
        /// only in case count as one.
        void TagCounts(Array<String>& tags, Array<u32>& counts) const
        {
            for (const Entry& e : m_entries)
                for (const String& tag : e.tags)
                {
                    usize i = 0;
                    while (i < tags.Size() && !store_detail::SameIgnoreCase(tags[i].AsView(), tag.AsView()))
                        ++i;
                    if (i == tags.Size())
                    {
                        tags.PushBack(tag);
                        counts.PushBack(0);
                    }
                    ++counts[i];
                }
        }

        // === Entries ===

        /// Adds the model at `path` as an entry, or returns its existing entry. False when the file
        /// does not exist.
        bool AddEntry(StringView path, Guid& outId)
        {
            if (const Entry* existing = FindEntryByPath(path))
            {
                outId = existing->id;
                return true;
            }
            Entry e;
            if (!FileStat(path, e.size, e.modified))
                return false;
            e.id = Guid::Generate(m_random);
            e.path = String(path);
            m_entries.PushBack(foundation::core::Move(e));
            outId = m_entries.Back().id;
            return true;
        }
        /// Removes an entry and its memberships.
        bool RemoveEntry(const Guid& id)
        {
            const i32 at = EntryIndex(id);
            if (at < 0)
                return false;
            for (Collection& c : m_collections)
                for (usize i = c.members.Size(); i-- > 0;)
                    if (c.members[i].entry == id)
                        c.members.RemoveAt(i);
            m_entries.RemoveAt(static_cast<usize>(at));
            return true;
        }
        /// Points an entry at `path` after its file moved or changed, recording the file's size and
        /// time again.
        bool Relink(const Guid& id, StringView path, String& error)
        {
            const i32 at = EntryIndex(id);
            if (at < 0)
                return Fail(error, u8"no such model");
            const Entry* other = FindEntryByPath(path);
            if (other != nullptr && other->id != id)
                return Fail(error, Format(u8"{} is another model in the library already", path).AsView());
            Entry& e = m_entries[static_cast<usize>(at)];
            if (!FileStat(path, e.size, e.modified))
                return Fail(error, Format(u8"{} is not there", path).AsView());
            e.path = String(path);
            return true;
        }
        bool SetNote(const Guid& id, StringView note)
        {
            Entry* e = MutableEntry(id);
            if (e == nullptr)
                return false;
            e->note = String(note);
            return true;
        }
        bool SetStats(const Guid& id, const EntryStats& stats)
        {
            Entry* e = MutableEntry(id);
            if (e == nullptr)
                return false;
            e->stats = stats;
            return true;
        }

        // === Collections ===

        bool CreateCollection(StringView name, const Guid& parent, Guid& outId, String& error)
        {
            if (name.IsEmpty())
                return Fail(error, u8"a collection needs a name");
            if (!parent.IsNil() && FindCollection(parent) == nullptr)
                return Fail(error, u8"no such parent collection");
            Collection c;
            c.id = Guid::Generate(m_random);
            c.name = String(name);
            c.parent = parent;
            m_collections.PushBack(foundation::core::Move(c));
            outId = m_collections.Back().id;
            return true;
        }
        bool RenameCollection(const Guid& id, StringView name, String& error)
        {
            Collection* c = MutableCollection(id);
            if (c == nullptr)
                return Fail(error, u8"no such collection");
            if (name.IsEmpty())
                return Fail(error, u8"a collection needs a name");
            c->name = String(name);
            return true;
        }
        /// Moves a collection under `parent` (nil for the top level). Refused when `parent` is inside
        /// the collection, or when a member's name would clash under the new top-level collection.
        bool MoveCollection(const Guid& id, const Guid& parent, String& error)
        {
            Collection* c = MutableCollection(id);
            if (c == nullptr)
                return Fail(error, u8"no such collection");
            if (!parent.IsNil() && (FindCollection(parent) == nullptr || IsUnder(parent, id)))
                return Fail(error, u8"a collection cannot go inside itself");
            // The member names it brings must not clash with those of the top-level collection it joins.
            const Guid newTop = parent.IsNil() ? id : TopOf(parent);
            if (newTop != TopOf(id))
            {
                Array<Guid> moving;
                Subtree(id, moving);
                for (const Guid& g : moving)
                    for (const Member& m : FindCollection(g)->members)
                        if (!m.name.IsEmpty())
                            if (const Member* holder = NameHolder(newTop, m.name.AsView(), &moving))
                                return Fail(error, Format(u8"the name {} is taken there (by {})", m.name.AsView(), Describe(*holder)).AsView());
            }
            MutableCollection(id)->parent = parent;
            return true;
        }
        /// Deletes a collection, its sub-collections and their memberships; the entries stay.
        bool DeleteCollection(const Guid& id)
        {
            Array<Guid> doomed;
            Subtree(id, doomed);
            if (doomed.IsEmpty())
                return false;
            for (usize i = m_collections.Size(); i-- > 0;)
                for (const Guid& g : doomed)
                    if (m_collections[i].id == g)
                    {
                        m_collections.RemoveAt(i);
                        break;
                    }
            return true;
        }

        // === Members ===

        /// Inserts entries into a collection at position `at` (past the end appends). An entry that
        /// is already a member keeps its place.
        bool AddMembers(const Guid& collection, Span<const Guid> entries, i32 at, String& error)
        {
            Collection* c = MutableCollection(collection);
            if (c == nullptr)
                return Fail(error, u8"no such collection");
            usize place = at < 0 ? c->members.Size() : Min(static_cast<usize>(at), c->members.Size());
            for (const Guid& entry : entries)
            {
                if (FindEntry(entry) == nullptr)
                    return Fail(error, Format(u8"no such model {}", entry).AsView());
                bool present = false;
                for (const Member& m : c->members)
                    present = present || m.entry == entry;
                if (present)
                    continue;
                c->members.Insert(place++, Member{entry, {}});
            }
            return true;
        }
        bool RemoveMembers(const Guid& collection, Span<const Guid> entries)
        {
            Collection* c = MutableCollection(collection);
            if (c == nullptr)
                return false;
            for (const Guid& entry : entries)
                for (usize i = c->members.Size(); i-- > 0;)
                    if (c->members[i].entry == entry)
                        c->members.RemoveAt(i);
            return true;
        }
        /// Sets the name an entry has in a collection. The name must be unique, ignoring case, across
        /// the collection's top-level collection; an empty name clears it.
        bool RenameMember(const Guid& collection, const Guid& entry, StringView name, String& error)
        {
            Collection* c = MutableCollection(collection);
            if (c == nullptr)
                return Fail(error, u8"no such collection");
            Member* member = nullptr;
            for (Member& m : c->members)
                if (m.entry == entry)
                    member = &m;
            if (member == nullptr)
                return Fail(error, u8"that model is not in this collection");
            if (!name.IsEmpty())
                if (const Member* holder = NameHolder(TopOf(collection), name, nullptr); holder != nullptr && holder != member)
                    return Fail(error, Format(u8"the name {} is taken (by {})", name, Describe(*holder)).AsView());
            member->name = String(name);
            return true;
        }
        bool MoveMember(const Guid& collection, const Guid& entry, i32 at)
        {
            Collection* c = MutableCollection(collection);
            if (c == nullptr)
                return false;
            for (usize i = 0; i < c->members.Size(); ++i)
                if (c->members[i].entry == entry)
                {
                    Member moved = foundation::core::Move(c->members[i]);
                    c->members.RemoveAt(i);
                    c->members.Insert(at < 0 ? c->members.Size() : Min(static_cast<usize>(at), c->members.Size()), foundation::core::Move(moved));
                    return true;
                }
            return false;
        }

        // === Tags ===

        void AddTag(Span<const Guid> entries, StringView tag)
        {
            if (tag.IsEmpty())
                return;
            for (const Guid& id : entries)
                if (Entry* e = MutableEntry(id); e != nullptr && !HasTag(*e, tag))
                    e->tags.PushBack(String(tag));
        }
        void RemoveTag(Span<const Guid> entries, StringView tag)
        {
            for (const Guid& id : entries)
                if (Entry* e = MutableEntry(id))
                    for (usize i = e->tags.Size(); i-- > 0;)
                        if (store_detail::SameIgnoreCase(e->tags[i].AsView(), tag))
                            e->tags.RemoveAt(i);
        }
        /// Renames a tag on every entry, merging it into `to` where an entry has both.
        void RenameTag(StringView from, StringView to)
        {
            for (Entry& e : m_entries)
            {
                if (!HasTag(e, from))
                    continue;
                for (usize i = e.tags.Size(); i-- > 0;)
                    if (store_detail::SameIgnoreCase(e.tags[i].AsView(), from))
                        e.tags.RemoveAt(i);
                if (!to.IsEmpty() && !HasTag(e, to))
                    e.tags.PushBack(String(to));
            }
        }
        void DeleteTag(StringView tag) { RenameTag(tag, {}); }
        [[nodiscard]] static bool HasTag(const Entry& e, StringView tag)
        {
            for (const String& t : e.tags)
                if (store_detail::SameIgnoreCase(t.AsView(), tag))
                    return true;
            return false;
        }

        // === Persistence ===

        /// The library as XML: its version, then its entries and collections.
        [[nodiscard]] String Write() const
        {
            foundation::xml::XmlSerializer ar(m_entries.Allocator());
            i32 version = kVersion;
            foundation::core::Serialize(ar, "version", version);
            foundation::core::Serialize(ar, "entries", const_cast<Array<Entry>&>(m_entries));
            foundation::core::Serialize(ar, "collections", const_cast<Array<Collection>&>(m_collections));
            String text;
            ar.GetOutput(text);
            return text;
        }
        /// Replaces the library with XML produced by Write. Returns false, leaving the library
        /// unchanged, when the text is not a library or is newer than this viewer can read.
        bool Read(StringView text, String& error)
        {
            foundation::xml::XmlDocument document(m_entries.Allocator());
            if (document.Parse(text) != foundation::xml::XmlResult::Ok || document.RootElement() == nullptr)
                return Fail(error, Format(u8"the library is not XML (line {})", document.ErrorLine()).AsView());
            foundation::xml::XmlSerializer ar(document);
            i32 version = 0;
            foundation::core::Serialize(ar, "version", version);
            if (version < 1 || version > kVersion)
                return Fail(error, Format(u8"the library is version {}, this viewer reads 1 to {}", version, kVersion).AsView());
            Array<Entry> entries(m_entries.Allocator());
            Array<Collection> collections(m_collections.Allocator());
            foundation::core::Serialize(ar, "entries", entries);
            foundation::core::Serialize(ar, "collections", collections);
            m_entries = foundation::core::Move(entries);
            m_collections = foundation::core::Move(collections);
            return true;
        }

    private:
        static bool Fail(String& error, StringView message)
        {
            error = String(message);
            return false;
        }

        [[nodiscard]] i32 EntryIndex(const Guid& id) const
        {
            for (usize i = 0; i < m_entries.Size(); ++i)
                if (m_entries[i].id == id)
                    return static_cast<i32>(i);
            return -1;
        }
        [[nodiscard]] i32 CollectionIndex(const Guid& id) const
        {
            for (usize i = 0; i < m_collections.Size(); ++i)
                if (m_collections[i].id == id)
                    return static_cast<i32>(i);
            return -1;
        }
        [[nodiscard]] Entry* MutableEntry(const Guid& id)
        {
            const i32 at = EntryIndex(id);
            return at >= 0 ? &m_entries[static_cast<usize>(at)] : nullptr;
        }
        [[nodiscard]] Collection* MutableCollection(const Guid& id)
        {
            const i32 at = CollectionIndex(id);
            return at >= 0 ? &m_collections[static_cast<usize>(at)] : nullptr;
        }

        /// The member that holds `name` (ignoring case) anywhere under `top`, skipping the
        /// collections in `skip`.
        [[nodiscard]] const Member* NameHolder(const Guid& top, StringView name, const Array<Guid>* skip) const
        {
            Array<Guid> under;
            Subtree(top, under);
            for (const Guid& g : under)
            {
                bool skipped = false;
                if (skip != nullptr)
                    for (const Guid& s : *skip)
                        skipped = skipped || s == g;
                if (skipped)
                    continue;
                for (const Member& m : FindCollection(g)->members)
                    if (store_detail::SameIgnoreCase(m.name.AsView(), name))
                        return &m;
            }
            return nullptr;
        }
        [[nodiscard]] String Describe(const Member& m) const
        {
            const Entry* e = FindEntry(m.entry);
            return e != nullptr ? FileTitle(e->path.AsView()) : String(u8"a model");
        }

        Array<Entry> m_entries;
        Array<Collection> m_collections;
        Random m_random;
    };

    /// The library file in its folder (model-viewer in the user data folder). `library.xml` is
    /// written through a temporary file and a rename, so a crash never leaves it half written.
    /// While the library is open the viewer holds the system's lock on `library.lock`
    /// (FileTryLock); the system releases it when the viewer ends, even by crashing, so it is
    /// never left behind. A second viewer finds the lock held and opens the library read-only.
    /// The lock file records the holder's process id for the message.
    class LibraryFile
    {
    public:
        static constexpr StringView kFileName = u8"library.xml";
        static constexpr StringView kLockName = u8"library.lock";

        LibraryFile() = default;
        LibraryFile(const LibraryFile&) = delete;
        LibraryFile& operator=(const LibraryFile&) = delete;
        ~LibraryFile() { Close(); }

        /// Opens the library in `folder` into `library`, which stays empty when there is no file
        /// yet. Returns false only when the file exists and cannot be read (`error` says why).
        bool Open(StringView folder, Library& library, String& error)
        {
            Close();
            m_folder = String(folder);
            (void)CreateDirectories(folder);
            m_readOnly = !TakeLock();
            const String path = PathJoin(folder, kFileName);
            Result<Array<byte>> bytes = ReadFile(path.AsView());
            if (!bytes.HasValue())
                return true; // a first run
            return library.Read(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()), error);
        }

        /// Writes `library`; refused while read-only.
        bool Save(const Library& library)
        {
            if (m_readOnly || m_folder.IsEmpty())
                return false;
            const String text = library.Write();
            const String path = PathJoin(m_folder.AsView(), kFileName);
            const String temporary = Format(u8"{}.tmp", path.AsView());
            if (!WriteFile(temporary.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk())
                return false;
            (void)FileDelete(path.AsView());
            return FileMove(temporary.AsView(), path.AsView());
        }

        void Close()
        {
            if (FileIsValid(m_lock))
            {
                FileUnlock(m_lock);
                FileClose(m_lock);
            }
            m_lock = kInvalidFile;
            m_folder = {};
            m_readOnly = true;
            m_holder = 0;
        }

        [[nodiscard]] bool ReadOnly() const noexcept { return m_readOnly; }
        /// The process holding the lock when this viewer is read-only; 0 when the lock file did
        /// not say.
        [[nodiscard]] u64 LockHolder() const noexcept { return m_holder; }

    private:
        bool TakeLock()
        {
            const String path = PathJoin(m_folder.AsView(), kLockName);
            m_lock = FileOpen(path.AsView(), FileMode::ReadWrite);
            if (!FileIsValid(m_lock))
                return false;
            if (!FileTryLock(m_lock))
            {
                // The lock is held: read the holder's process id from the start of the file.
                char text[32] = {};
                const i64 read = FileRead(m_lock, text, sizeof(text) - 1);
                for (i64 i = 0; i < read && text[i] >= '0' && text[i] <= '9'; ++i)
                    m_holder = m_holder * 10 + static_cast<u64>(text[i] - '0');
                FileClose(m_lock);
                m_lock = kInvalidFile;
                return false;
            }
            const String mine = Format(u8"{}\n", ProcessId());
            (void)FileSeek(m_lock, 0, SeekOrigin::Begin);
            (void)FileWrite(m_lock, mine.Data(), mine.Size());
            return true;
        }

        String m_folder;
        FileHandle m_lock = kInvalidFile;
        bool m_readOnly = true;
        u64 m_holder = 0;
    };

    /// Imports a shortlist file (the text Shortlist::Write produces) into a top-level collection
    /// named Shortlist: an entry and a member for each starred model whose file exists, with the
    /// shortlist note as the entry's note. Returns how many models were imported.
    inline u32 ImportShortlistText(Library& library, StringView text)
    {
        Guid collection;
        String error;
        if (const Collection* existing = library.FindCollectionByName(u8"Shortlist"); existing != nullptr && existing->parent.IsNil())
            collection = existing->id;
        else if (!library.CreateCollection(u8"Shortlist", Guid{}, collection, error))
            return 0;
        u32 imported = 0;
        usize start = 0;
        while (start < text.Size())
        {
            usize end = start;
            while (end < text.Size() && text[end] != u8'\n')
                ++end;
            const StringView line = text.SubStr(start, end - start);
            start = end + 1;
            usize tab = 0;
            while (tab < line.Size() && line[tab] != u8'\t')
                ++tab;
            // Undo the shortlist's escapes in the note; paths have none in practice.
            const StringView path = line.SubStr(0, tab);
            String note;
            for (usize i = tab + 1; i < line.Size(); ++i)
            {
                if (line[i] == u8'\\' && i + 1 < line.Size())
                {
                    ++i;
                    note += line[i] == u8't' ? StringView(u8"\t") : line[i] == u8'n' ? StringView(u8"\n") : line.SubStr(i, 1);
                    continue;
                }
                note += line.SubStr(i, 1);
            }
            Guid entry;
            if (path.IsEmpty() || !library.AddEntry(path, entry))
                continue;
            if (!note.IsEmpty())
                (void)library.SetNote(entry, note.AsView());
            const Guid added[] = {entry};
            (void)library.AddMembers(collection, Span<const Guid>(added, 1), -1, error);
            ++imported;
        }
        return imported;
    }
}
