// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Content - the `foundation.content` module.
//
// A content database: a hierarchical store of serializable objects, addressed by
// Guid (stable) or by path. A Group is a folder; an Instance is one stored unit
// (a Guid, a primary ISerializable object and named data streams for heavy
// blobs). Backed by a VFS mount (Group = directory, Instance = a file with a
// configurable extension, data streams = sidecar files). Identity is decoupled
// from byte access: the database owns the mapping of Guids to locations; the VFS
// owns the bytes.
//
// The serialization format is pluggable: the database receives a
// SerializerFactory that creates a Serializer for a given stream and mode. The
// factory hides format-specific construction (binary vs XML vs anything else).
// The database never imports a concrete serializer module.

module;
#include "Core/Prelude.h"

export module foundation.content;

import foundation.core;
import foundation.vfs;

using namespace foundation::core;
using namespace foundation::vfs;

export namespace foundation::content
{
    class ContentDatabase;
    class Group;

    // Path helpers (defined below; declared here for in-class use).
    [[nodiscard]] inline String JoinPath(StringView a, StringView b);
    [[nodiscard]] inline bool EndsWith(StringView str, StringView suffix);

    // How a data stream's bytes are encoded on disk. Text streams (XML scene/prefab
    // sources) get a ".data" sidecar suffix so external tools treat them as text;
    // binary streams keep ".bin". Readers accept either suffix for any stream.
    enum class StreamEncoding : u8
    {
        Binary,
        Text
    };

    // =======================================================================
    // Instance: one stored unit, with identity, a primary object and data streams.
    // =======================================================================
    class Instance
    {
    public:
        Instance(ContentDatabase& db, Group& group, const Guid& id, StringView name,
                 StringView typeNamespace, StringView typeName)
            : m_db(&db), m_group(&group), m_id(id), m_name(name), m_typeNamespace(typeNamespace),
              m_typeName(typeName)
        {
        }

        [[nodiscard]] const Guid& Id() const noexcept { return m_id; }
        [[nodiscard]] StringView Name() const noexcept { return m_name.AsView(); }
        [[nodiscard]] StringView TypeNamespace() const noexcept { return m_typeNamespace.AsView(); }
        [[nodiscard]] StringView TypeName() const noexcept { return m_typeName.AsView(); }
        [[nodiscard]] Group& OwningGroup() const noexcept { return *m_group; }

        // "group/path/name" (mount-relative, no extension).
        [[nodiscard]] String Path() const;

        // Deserializes the primary object (constructing the concrete type from
        // its stored type name). Null if the type isn't registered or on I/O error.
        [[nodiscard]] RefPtr<ISerializable> ReadObject() const;
        // Reads an envelope an author supplied (an agent's edit of the stored text) the way
        // ReadObject reads the stored one, so it accepts exactly what a load accepts: its
        // header must name this instance, its guid and its type, and its payload must read
        // under this build's version. The error says why when it does not.
        [[nodiscard]] Result<RefPtr<ISerializable>, String> ReadObjectFrom(IStream& stream) const;

        // Opens a named data stream for reading, or null if absent. Probes the text
        // sidecar (".data") first, then the binary/legacy one (".bin").
        [[nodiscard]] UniquePtr<IStream> ReadData(StringView streamName) const;
        // The raw on-disk envelope (identity header, primary object, stream directory); the
        // cook driver hashes it as the asset's settings and content fingerprint.
        [[nodiscard]] UniquePtr<IStream> OpenEnvelope() const;

        // --- tooling / write ---
        [[nodiscard]] Status WriteObject(ISerializable& object);
        // Writes the stream's sidecar under the encoding's suffix and removes the
        // other-suffix sibling, so a stream never exists under both at once (which also
        // moves an older text sidecar stored as ".bin" to ".data").
        [[nodiscard]] Status WriteData(StringView streamName, Span<const byte> data,
                                       StreamEncoding encoding = StreamEncoding::Binary);
        // Remove a named data stream's sidecar if present (idempotent: NotFound is not an error).
        // Cooks call this for an on-demand stream that a re-cook no longer produces, so a stale
        // sidecar from an earlier cook does not keep loading (e.g. a removed per-layer terrain map).
        [[nodiscard]] Status DeleteData(StringView streamName);

    private:
        friend class ContentDatabase; // storage layout (envelope/sidecar paths) for delete
        // The one envelope reader: `checkIdentity` holds the header to this instance.
        [[nodiscard]] Result<RefPtr<ISerializable>, String> ReadEnvelope(IStream& stream,
                                                                         bool checkIdentity) const;
        [[nodiscard]] String EnvelopePath() const; // "<path>.<ext>"
        // "<path>.<stream>.bin" (Binary) / "<path>.<stream>.data" (Text).
        [[nodiscard]] String DataPath(StringView streamName, StreamEncoding encoding) const;

        ContentDatabase* m_db;
        Group* m_group;
        Guid m_id;
        String m_name;
        String m_typeNamespace;
        String m_typeName;
    };

    // =======================================================================
    // Group: a folder in the tree, with child groups and instances.
    // =======================================================================
    class Group
    {
    public:
        Group(ContentDatabase& db, Group* parent, StringView name)
            : m_db(&db), m_parent(parent), m_name(name)
        {
        }

        [[nodiscard]] StringView Name() const noexcept { return m_name.AsView(); }
        [[nodiscard]] Group* Parent() const noexcept { return m_parent; }

        // Mount-relative folder path ("" at the root, "materials/metal" deeper).
        [[nodiscard]] String Path() const;

        [[nodiscard]] Span<Group* const> Groups() const noexcept { return m_groups.AsSpan(); }
        [[nodiscard]] Span<Instance* const> Instances() const noexcept
        {
            return m_instances.AsSpan();
        }

        [[nodiscard]] Group* GetGroup(StringView name) const;
        [[nodiscard]] Instance* GetInstance(StringView name) const;

        // --- tooling ---
        // Returns the existing child group of this name, or creates it (no disk
        // write until an instance is committed under it).
        Group* CreateGroup(StringView name);
        // Creates a new instance of `primaryType` with a fresh Guid. The on-disk
        // file appears once WriteObject() is called.
        // A taken name returns the existing instance (for reimport and cook idempotency), so
        // a New-Asset creator that then writes a starter with WriteObject() would overwrite
        // it. Such creators must pass UniqueInstanceName(base) instead of a fixed name.
        Instance* CreateInstance(StringView name, const TypeInfo& primaryType);

        // First free name from `base`: `base`, then `base.2`, `base.3`, ... The one
        // general dedup for New-Asset creators.
        [[nodiscard]] String UniqueInstanceName(StringView base) const;
        // Same convention for child-group names (probes GetGroup instead).
        [[nodiscard]] String UniqueGroupName(StringView base) const;
        // Same, but with a caller-chosen Guid (the cook driver: product guid = source guid).
        // Returns the existing instance when the name is already taken.
        Instance* CreateInstanceWithId(const Guid& id, StringView name,
                                       const TypeInfo& primaryType);

        // --- internal (used by the database scanner) ---
        // Children stay sorted by name (case-insensitive): the mount scan returns entries in
        // arbitrary readdir order and imports append, so without this every browser, tree and
        // picker would show a different order each session.
        Group* AddChildGroup(StringView name);
        Instance* AddInstance(const Guid& id, StringView name, StringView typeNs,
                              StringView typeName);
        void RemoveInstance(Instance* instance); // unlinks from this group (DB owns destruction)
        void ResortChildren(); // re-establish the name order after a child rename

    private:
        [[nodiscard]] static bool NameLess(StringView a, StringView b);
        friend class ContentDatabase; // rename rewrites m_name after moving the directory
        ContentDatabase* m_db;
        Group* m_parent;
        String m_name;
        Array<Group*> m_groups;       // owned by the database pool
        Array<Instance*> m_instances; // owned by the database pool
    };

    // =======================================================================
    // IContentDatabase: the database surface (backends implement it).
    // =======================================================================
    class IContentDatabase
    {
    public:
        virtual ~IContentDatabase() = default;

        [[nodiscard]] virtual Group* RootGroup() = 0;
        [[nodiscard]] virtual Instance* GetInstance(const Guid& id) = 0;
        [[nodiscard]] virtual Instance* GetInstance(StringView path) = 0;
        [[nodiscard]] virtual RefPtr<ISerializable> ReadObject(const Guid& id) = 0;
    };

    // =======================================================================
    // ContentDatabase: VFS-backed. Scans the mount on construction; reads and
    // writes through the mount's enumerable/writable capabilities. The
    // serialization format is determined by the SerializerFactory provided by
    // the caller.
    // =======================================================================
    class ContentDatabase final : public IContentDatabase
    {
    public:
        // `mount` must outlive the database and support enumerate and write. The
        // allocator (required; the owner decides) backs every group and instance node.
        ContentDatabase(IAllocator& allocator, IFileSystem& mount, SerializerFactory factory,
                                 StringView fileExtension,
                                 SerializableRegistry& serializables = GlobalSerializableRegistry(),
                                 TypeRegistry& types = GlobalTypeRegistry());

        ~ContentDatabase() override
        {
            for (Instance* instance : m_allInstances)
            {
                m_allocator->Delete(instance);
            }
            for (Group* group : m_allGroups)
            {
                m_allocator->Delete(group);
            }
        }

        ContentDatabase(const ContentDatabase&) = delete;
        ContentDatabase& operator=(const ContentDatabase&) = delete;

        [[nodiscard]] Group* RootGroup() override { return m_root; }

        /// What the open-time header scan cost: the envelope count and the bytes the serializer
        /// had to consume to reach the three header fields. For the XML factory that is the
        /// whole file (a DOM parse before the first read), so these numbers show whether a true
        /// partial header parse would pay.
        struct ScanStats
        {
            usize envelopes = 0;
            u64 bytesOpened = 0; // sum of envelope file sizes the scan opened
        };

        [[nodiscard]] const ScanStats& LastScanStats() const noexcept { return m_scanStats; }

        // Delete an instance: its envelope and every data-stream sidecar are removed from the
        // mount, and it is unregistered from the group tree and the GUID index (the cook's
        // orphan sweep and the browser's Delete use it). NotFound when the id is unknown.
        Status DeleteInstance(const Guid& id);

        // Clone an instance under `newName` in the same group, with a fresh Guid: the primary
        // object round-trips through its registered type (so the copy is deep and re-keyed) and
        // every data-stream sidecar is byte-copied. Null when the id is unknown, the name is
        // taken, or the primary type isn't registered. (The browser's Duplicate.)
        Instance* CloneInstance(const Guid& id, StringView newName);

        // Copy-forward: fill an existing instance `dest` (in this DB) with the primary object and
        // every data-stream sidecar of the instance `srcId` in `src`, byte for byte. `dest` and
        // the source are expected to share a guid (the variant cook keeps product guid == source
        // guid in both DBs), so the re-serialized object is identical. Used to carry
        // platform-invariant cooked products from the host DB into a per-target DB without
        // re-cooking (asset variants).
        // NotFound when `srcId` is unknown in `src`; other failures propagate from write.
        Status CopyContentForward(Instance& dest, ContentDatabase& src, const Guid& srcId);

        // Rename an instance in place (same group, same guid): moves the envelope and every
        // data-stream sidecar on disk (the name is the filename; envelopes don't store it).
        // Guid-based references (scene refs, cook records) are untouched.
        // AlreadyExists when the name is taken; NotSupported on read-only mounts.
        Status RenameInstance(const Guid& id, StringView newName);

        // Move an instance to another group, keeping its name and guid: its envelope and every
        // data-stream sidecar move to the group's folder (made when it has none yet). Guid-based
        // references are untouched. AlreadyExists when the group has one of that name;
        // InvalidArgument when it is already there; NotSupported on read-only mounts.
        Status MoveInstance(const Guid& id, Group& to);

        // Rename a group (directory move; child paths derive dynamically, so descendants
        // need no fixup). NotSupported for the root or read-only mounts.
        Status RenameGroup(Group& group, StringView newName);

        // Delete a group and everything under it: every instance (envelope and sidecars, via
        // DeleteInstance) and every child group, bottom-up, then the now-empty directories, so
        // a rescan cannot resurrect ghost groups. The Group object is destroyed; the caller's
        // pointer is dangling after success. NotSupported for the root or read-only mounts.
        Status DeleteGroup(Group& group);

        [[nodiscard]] Instance* GetInstance(const Guid& id) override
        {
            Instance* const* found = m_byGuid.Find(id);
            return (found != nullptr) ? *found : nullptr;
        }

        [[nodiscard]] Instance* GetInstance(StringView path) override
        {
            // Split "group/sub/name": walk the groups, then find the instance by name.
            Group* group = m_root;
            usize start = 0;
            for (usize i = 0; i <= path.Size(); ++i)
            {
                const bool atEnd = (i == path.Size());
                if (!atEnd && path[i] != utf8char('/'))
                {
                    continue;
                }
                const StringView part = path.SubStr(start, i - start);
                start = i + 1;
                if (part.IsEmpty())
                {
                    continue;
                }
                if (atEnd)
                {
                    return group->GetInstance(part);
                } // last segment = instance name
                Group* next = group->GetGroup(part);
                if (next == nullptr)
                {
                    return nullptr;
                }
                group = next;
            }
            return nullptr;
        }

        [[nodiscard]] RefPtr<ISerializable> ReadObject(const Guid& id) override
        {
            Instance* instance = GetInstance(id);
            return (instance != nullptr) ? instance->ReadObject() : RefPtr<ISerializable>{};
        }

        // --- accessors used by Group/Instance ---
        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }
        [[nodiscard]] IFileSystem& Mount() const noexcept { return *m_mount; }
        [[nodiscard]] SerializableRegistry& Serializables() const noexcept
        {
            return *m_serializables;
        }
        [[nodiscard]] TypeRegistry& Types() const noexcept { return *m_types; }
        [[nodiscard]] Random& Rng() noexcept { return m_rng; }
        [[nodiscard]] StringView Extension() const noexcept { return m_extension.AsView(); }

        [[nodiscard]] UniquePtr<SerializerContext> CreateSerializer(IStream& stream,
                                                                    SerializeMode mode) const
        {
            return m_factory(stream, mode);
        }

        /// Counts the changes to the tree's shape: a group or an instance added, removed, renamed
        /// or moved. A view of the tree (the asset browser) builds again when it moves, however
        /// the change came (a page, an import, an agent's write).
        [[nodiscard]] u64 Revision() const noexcept { return m_revision; }
        /// The shape changed (what the groups' own edits call).
        void Touch() noexcept { ++m_revision; }

        Group* NewGroup(Group* parent, StringView name)
        {
            Group* group = m_allocator->New<Group>(*this, parent, name);
            Touch();
            m_allGroups.PushBack(group);
            return group;
        }

        Instance* NewInstance(Group& group, const Guid& id, StringView name, StringView typeNs,
                              StringView typeName)
        {
            Instance* instance =
                m_allocator->New<Instance>(*this, group, id, name, typeNs, typeName);
            Touch();
            m_allInstances.PushBack(instance);
            if (!id.IsNil())
            {
                m_byGuid.InsertOrAssign(id, instance);
            }
            return instance;
        }

    private:
        // Recursively scans `folder` (mount-relative) into `group`.
        void Scan(Group& group, StringView folder)
        {
            IEnumerableFileSystem* enumerable = m_mount->AsEnumerable();
            if (enumerable == nullptr)
            {
                return;
            }

            Array<DirEntry> entries;
            if (!enumerable->Enumerate(folder, entries).IsOk())
            {
                return;
            }

            for (const DirEntry& entry : entries)
            {
                if (entry.isDirectory)
                {
                    Group* child = group.AddChildGroup(entry.name.AsView());
                    Scan(*child, JoinPath(folder, entry.name.AsView()));
                }
                else if (EndsWith(entry.name.AsView(), m_extension.AsView()))
                {
                    ScanInstance(group, folder, entry.name.AsView());
                }
            }
        }

        void ScanInstance(Group& group, StringView folder, StringView fileName)
        {
            const StringView instanceName =
                fileName.SubStr(0, fileName.Size() - m_extension.Size());
            UniquePtr<IStream> stream = m_mount->Open(JoinPath(folder, fileName), FileMode::Read);
            if (!stream)
            {
                return;
            }
            ++m_scanStats.envelopes;
            const i64 streamSize = stream->Size();
            m_scanStats.bytesOpened += streamSize > 0 ? static_cast<u64>(streamSize) : 0;

            UniquePtr<SerializerContext> ctx = m_factory(*stream, SerializeMode::Read);
            if (!ctx || ctx->serializer == nullptr)
            {
                return;
            }
            Serializer& ar = *ctx->serializer;

            Guid id;
            String typeNs;
            String typeName;
            ar.Key("guid");
            ar.GuidValue(id);
            ar.Key("typeNamespace");
            ar.Text(typeNs);
            ar.Key("typeName");
            ar.Text(typeName);
            if (!ar.IsOk())
            {
                return;
            }

            (void)group.AddInstance(id, instanceName, typeNs.AsView(), typeName.AsView());
        }

        IAllocator* m_allocator;
        IFileSystem* m_mount;
        SerializerFactory m_factory;
        String m_extension;
        ScanStats m_scanStats;
        SerializableRegistry* m_serializables;
        TypeRegistry* m_types;
        Random m_rng;
        Group* m_root = nullptr;
        Array<Group*> m_allGroups;
        Array<Instance*> m_allInstances;
        HashMap<Guid, Instance*> m_byGuid;
        u64 m_revision = 0; // Revision()
    };

    // -----------------------------------------------------------------------
    // Path helpers (mount-relative, forward-slash).
    // -----------------------------------------------------------------------
    [[nodiscard]] inline String JoinPath(StringView a, StringView b)
    {
        if (a.IsEmpty())
        {
            return String(b);
        }
        if (b.IsEmpty())
        {
            return String(a);
        }
        String out(a);
        out.PushBack(utf8char('/'));
        out.Append(b);
        return out;
    }

    [[nodiscard]] inline bool EndsWith(StringView str, StringView suffix)
    {
        return str.Size() >= suffix.Size() &&
               str.SubStr(str.Size() - suffix.Size(), suffix.Size()) == suffix;
    }

}
