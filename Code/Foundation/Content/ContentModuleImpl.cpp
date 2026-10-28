// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Content - foundation.content implementation unit.
//
// Out-of-line definitions for Instance/Group/ContentDatabase (sec 3.2 / sec 10.6).
// ContentModule.cppm keeps the class declarations + trivial inline accessors.

module;
#include "Core/Prelude.h"
#include <chrono>
#include <random>

module foundation.content;

import foundation.core;
import foundation.vfs;

using namespace foundation::core;
using namespace foundation::vfs;

namespace foundation::content
{
    namespace
    {
        // Sidecar-suffix probe for the "<name>.<stream>.<suffix>" prefix scans:
        // returns the suffix length (".data" text / ".bin" binary), 0 = not a sidecar.
        [[nodiscard]] usize SidecarSuffixSize(StringView fileName)
        {
            if (EndsWith(fileName, u8".data"))
            {
                return 5;
            }
            if (EndsWith(fileName, u8".bin"))
            {
                return 4;
            }
            return 0;
        }
    } // namespace

    ContentDatabase::ContentDatabase(IAllocator& allocator, IFileSystem& mount,
                                     SerializerFactory factory, StringView fileExtension,
                                     SerializableRegistry& serializables, TypeRegistry& types)
        : m_allocator(&allocator), m_mount(&mount),
          m_factory(static_cast<SerializerFactory&&>(factory)), m_extension(fileExtension),
          m_serializables(&serializables), m_types(&types), m_allGroups(allocator),
          m_allInstances(allocator)
    {
        std::random_device entropy;
        const u64 seed =
            (static_cast<u64>(entropy()) << 32) ^ static_cast<u64>(entropy()) ^
            static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
        const u64 sequence = (static_cast<u64>(entropy()) << 32) ^ static_cast<u64>(entropy()) ^
                             reinterpret_cast<u64>(this);
        m_rng = Random(seed, sequence);
        m_root = NewGroup(nullptr, u8"");
        Scan(*m_root, u8"");
    }

    // -----------------------------------------------------------------------
    // Group method definitions.
    // -----------------------------------------------------------------------
    String Group::Path() const
    {
        if (m_parent == nullptr)
        {
            return String();
        } // root
        return JoinPath(m_parent->Path().AsView(), m_name.AsView());
    }

    Group* Group::GetGroup(StringView name) const
    {
        for (Group* group : m_groups)
        {
            if (group->Name() == name)
            {
                return group;
            }
        }
        return nullptr;
    }

    Instance* Group::GetInstance(StringView name) const
    {
        for (Instance* instance : m_instances)
        {
            if (instance->Name() == name)
            {
                return instance;
            }
        }
        return nullptr;
    }

    bool Group::NameLess(StringView a, StringView b)
    {
        const usize n = Min(a.Size(), b.Size());
        for (usize i = 0; i < n; ++i)
        {
            utf8char ca = a[i];
            utf8char cb = b[i];
            if (ca >= utf8char('A') && ca <= utf8char('Z'))
            {
                ca = static_cast<utf8char>(ca + 32);
            }
            if (cb >= utf8char('A') && cb <= utf8char('Z'))
            {
                cb = static_cast<utf8char>(cb + 32);
            }
            if (ca != cb)
            {
                return ca < cb;
            }
        }
        return a.Size() < b.Size();
    }

    Group* Group::AddChildGroup(StringView name)
    {
        Group* child = m_db->NewGroup(this, name);
        usize at = m_groups.Size();
        for (usize i = 0; i < m_groups.Size(); ++i)
        {
            if (NameLess(child->Name(), m_groups[i]->Name()))
            {
                at = i;
                break;
            }
        }
        m_groups.Insert(at, child);
        return child;
    }

    Instance* Group::AddInstance(const Guid& id, StringView name, StringView typeNs,
                                 StringView typeName)
    {
        Instance* instance = m_db->NewInstance(*this, id, name, typeNs, typeName);
        usize at = m_instances.Size();
        for (usize i = 0; i < m_instances.Size(); ++i)
        {
            if (NameLess(instance->Name(), m_instances[i]->Name()))
            {
                at = i;
                break;
            }
        }
        m_instances.Insert(at, instance);
        return instance;
    }

    void Group::ResortChildren()
    {
        m_db->Touch(); // a child renamed
        m_groups.Sort([](Group* a, Group* b) { return NameLess(a->Name(), b->Name()); });
        m_instances.Sort([](Instance* a, Instance* b)
                         { return NameLess(a->Name(), b->Name()); });
    }

    void Group::RemoveInstance(Instance* instance)
    {
        m_db->Touch();
        for (usize i = 0; i < m_instances.Size(); ++i)
        {
            if (m_instances[i] == instance)
            {
                m_instances.RemoveAt(i);
                return;
            }
        }
    }

    Group* Group::CreateGroup(StringView name)
    {
        Group* existing = GetGroup(name);
        return (existing != nullptr) ? existing : AddChildGroup(name);
    }

    // The one name-dedup loop: `base`, then `base.2`, `base.3`, ... against whatever
    // `taken` probes (instances or child groups).
    template <typename TakenPredicate>
    static String FirstFreeName(StringView base, TakenPredicate&& taken)
    {
        if (!taken(base))
        {
            return String(base);
        }
        for (i32 counter = 2;; ++counter)
        {
            String candidate(base);
            candidate.PushBack(u8'.');
            utf8char digits[12];
            i32 digitCount = 0;
            for (i32 value = counter; value > 0 && digitCount < 12; value /= 10)
            {
                digits[digitCount++] = static_cast<utf8char>('0' + value % 10);
            }
            while (digitCount > 0)
            {
                candidate.PushBack(digits[--digitCount]);
            }
            if (!taken(candidate.AsView()))
            {
                return candidate;
            }
        }
    }

    String Group::UniqueInstanceName(StringView base) const
    {
        return FirstFreeName(base,
                             [this](StringView name) { return GetInstance(name) != nullptr; });
    }

    String Group::UniqueGroupName(StringView base) const
    {
        return FirstFreeName(base,
                             [this](StringView name) { return GetGroup(name) != nullptr; });
    }

    Instance* Group::CreateInstance(StringView name, const TypeInfo& primaryType)
    {
        if (Instance* existing = GetInstance(name))
        {
            return existing;
        }
        // Mint a GUID that isn't already in use. The RNG is deterministic and Scan() (load-from-disk)
        // does NOT advance it past the instances it loads - so a fresh instance added to a scanned DB
        // would otherwise reproduce the FIRST-cooked instance's GUID and alias it (e.g. a runtime-cooked
        // texture colliding with a model's first texture). Re-roll until the id is free.
        Guid id = Guid::Generate(m_db->Rng());
        while (m_db->GetInstance(id) != nullptr)
        {
            id = Guid::Generate(m_db->Rng());
        }
        // TypeInfo names are narrow ASCII; wrap in StringView.
        const StringView ns(reinterpret_cast<const utf8char*>(primaryType.namespaceName));
        const StringView nm(reinterpret_cast<const utf8char*>(primaryType.name));
        return AddInstance(id, name, ns, nm);
    }

    Instance* Group::CreateInstanceWithId(const Guid& id, StringView name,
                                          const TypeInfo& primaryType)
    {
        if (Instance* existing = GetInstance(name))
        {
            return existing;
        }
        const StringView ns(reinterpret_cast<const utf8char*>(primaryType.namespaceName));
        const StringView nm(reinterpret_cast<const utf8char*>(primaryType.name));
        return AddInstance(id, name, ns, nm);
    }

    // -----------------------------------------------------------------------
    // Instance method definitions.
    // -----------------------------------------------------------------------
    String Instance::Path() const { return JoinPath(m_group->Path().AsView(), m_name.AsView()); }

    String Instance::EnvelopePath() const
    {
        String path = Path();
        path.Append(m_db->Extension());
        return path;
    }

    String Instance::DataPath(StringView streamName, StreamEncoding encoding) const
    {
        String path = Path();
        path.PushBack(utf8char('.'));
        path.Append(streamName);
        path.Append(encoding == StreamEncoding::Text ? u8".data" : u8".bin");
        return path;
    }

    RefPtr<ISerializable> Instance::ReadObject() const
    {
        UniquePtr<IStream> stream = m_db->Mount().Open(EnvelopePath().AsView(), FileMode::Read);
        if (!stream)
        {
            return RefPtr<ISerializable>{};
        }
        // Buffered: a binary envelope reads field by field, and straight off a file each
        // field is a system call (a cooked script's were thousands, dearer still on Windows).
        BufferedStream buffered(*stream, 64 * 1024);
        Result<RefPtr<ISerializable>, String> object = ReadEnvelope(buffered, false);
        return object.HasValue() ? Move(object.Value()) : RefPtr<ISerializable>{};
    }

    Result<RefPtr<ISerializable>, String> Instance::ReadObjectFrom(IStream& stream) const
    {
        return ReadEnvelope(stream, true);
    }

    Result<RefPtr<ISerializable>, String> Instance::ReadEnvelope(IStream& stream,
                                                                 bool checkIdentity) const
    {
        UniquePtr<SerializerContext> ctx = m_db->CreateSerializer(stream, SerializeMode::Read);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Err(String(u8"the text did not open as an envelope"));
        }
        Serializer& ar = *ctx->serializer;

        // Read header.
        Guid id;
        String ns;
        String name;
        ar.Key("guid");
        ar.GuidValue(id);
        ar.Key("typeNamespace");
        ar.Text(ns);
        ar.Key("typeName");
        ar.Text(name);
        if (!ar.IsOk())
        {
            return Err(String(u8"no envelope header (guid, typeNamespace, typeName)"));
        }
        if (checkIdentity && id != m_id)
        {
            utf8char asked[37];
            utf8char mine[37];
            id.ToChars(asked);
            m_id.ToChars(mine);
            return Err(Format(u8"the envelope's guid {} is not this asset's ({})",
                              StringView(asked, 36), StringView(mine, 36)));
        }
        if (checkIdentity && (ns.AsView() != TypeNamespace() || name.AsView() != TypeName()))
        {
            return Err(Format(u8"the envelope's type '{}.{}' is not this asset's ('{}.{}')",
                              ns.AsView(), name.AsView(), TypeNamespace(), TypeName()));
        }

        // Resolve the type and construct the object.
        const TypeInfo* type = m_db->Types().FindByName(reinterpret_cast<const char*>(ns.CStr()),
                                                        reinterpret_cast<const char*>(name.CStr()));
        if (type == nullptr)
        {
            return Err(Format(u8"the type '{}' is not registered in this build", name.AsView()));
        }

        RefPtr<ISerializable> object = m_db->Serializables().Create(type->id);
        if (object.Get() == nullptr)
        {
            return Err(Format(u8"the type '{}' is not serializable in this build", name.AsView()));
        }

        // Deserialize the payload under its data-version scope (a version other than the
        // type's current one fails the read here - no migration). A refused payload is not
        // read: it is in another layout, and reading it as this one only walks garbage.
        BeginVersionedPayload(ar, *type);
        if (ar.IsOk())
        {
            ar.Key("payload");
            ar.BeginObject();
            object->Serialize(ar);
            ar.EndObject();
        }
        EndVersionedPayload(ar);
        if (!ar.IsOk())
        {
            String error(u8"the payload did not read: a missing or misspelled key, a value "
                         u8"of the wrong kind, or dataVersions other than this build's");
            String place;
            ar.DescribeFailure(place);
            if (!place.IsEmpty())
            {
                error += StringView(u8" - ");
                error += place.AsView();
            }
            return Err(Move(error));
        }
        return object;
    }

    UniquePtr<IStream> Instance::ReadData(StringView streamName) const
    {
        if (UniquePtr<IStream> text =
                m_db->Mount().Open(DataPath(streamName, StreamEncoding::Text).AsView(),
                                   FileMode::Read))
        {
            return text;
        }
        return m_db->Mount().Open(DataPath(streamName, StreamEncoding::Binary).AsView(),
                                  FileMode::Read);
    }

    Status Instance::WriteObject(ISerializable& object)
    {
        IWritableFileSystem* writable = m_db->Mount().AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        MemoryStream buffer;
        UniquePtr<SerializerContext> ctx = m_db->CreateSerializer(buffer, SerializeMode::Write);
        if (!ctx || ctx->serializer == nullptr)
        {
            return Status{ErrorCode::Internal};
        }
        Serializer& ar = *ctx->serializer;

        // Write header + payload (payload wrapped in the object's data-version scope).
        String ns(m_typeNamespace);
        String nm(m_typeName);
        ar.Key("guid");
        ar.GuidValue(const_cast<Guid&>(m_id));
        ar.Key("typeNamespace");
        ar.Text(ns);
        ar.Key("typeName");
        ar.Text(nm);
        BeginVersionedPayload(ar, *object.GetType());
        ar.Key("payload");
        ar.BeginObject();
        object.Serialize(ar);
        ar.EndObject();
        EndVersionedPayload(ar);

        // Let the context flush (e.g., XML writes its text output here).
        ctx->Flush(buffer);

        return writable->Save(EnvelopePath().AsView(), buffer.Bytes());
    }

    Status Instance::WriteData(StringView streamName, Span<const byte> data,
                               StreamEncoding encoding)
    {
        IWritableFileSystem* writable = m_db->Mount().AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }
        const Status saved = writable->Save(DataPath(streamName, encoding).AsView(), data);
        if (saved.IsOk())
        {
            // One suffix per stream: drop the other-encoding sibling so a stream never
            // exists under both suffixes at once.
            const String other = DataPath(streamName, encoding == StreamEncoding::Text
                                                          ? StreamEncoding::Binary
                                                          : StreamEncoding::Text);
            if (m_db->Mount().Exists(other.AsView()))
            {
                (void)writable->Delete(other.AsView());
            }
        }
        return saved;
    }

    Status Instance::DeleteData(StringView streamName)
    {
        IWritableFileSystem* writable = m_db->Mount().AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }
        Status result{};
        for (StreamEncoding encoding : {StreamEncoding::Text, StreamEncoding::Binary})
        {
            const String path = DataPath(streamName, encoding);
            if (!m_db->Mount().Exists(path.AsView()))
            {
                continue; // idempotent: nothing to remove
            }
            const Status deleted = writable->Delete(path.AsView());
            if (!deleted.IsOk())
            {
                result = deleted;
            }
        }
        return result;
    }

    UniquePtr<IStream> Instance::OpenEnvelope() const
    {
        return m_db->Mount().Open(EnvelopePath().AsView(), FileMode::Read);
    }

    Instance* ContentDatabase::CloneInstance(const Guid& id, StringView newName)
    {
        Instance* src = GetInstance(id);
        if (src == nullptr)
        {
            return nullptr;
        }
        Group& group = src->OwningGroup();
        if (group.GetInstance(newName) != nullptr)
        {
            return nullptr;
        }

        // The primary object must round-trip (re-serialized under the clone's identity - a raw
        // envelope byte-copy would carry the SOURCE guid).
        RefPtr<ISerializable> object = src->ReadObject();
        if (object.Get() == nullptr)
        {
            return nullptr;
        }

        Guid cloneId = Guid::Generate(Rng());
        while (GetInstance(cloneId) != nullptr)
        {
            cloneId = Guid::Generate(Rng());
        }
        Instance* copy = group.AddInstance(cloneId, newName, src->TypeNamespace(), src->TypeName());
        if (copy == nullptr)
        {
            return nullptr;
        }
        if (!copy->WriteObject(*object).IsOk())
        {
            return nullptr;
        }

        // Sidecar streams keep no directory - enumerate "<srcName>.<stream>.<suffix>" siblings
        // (the same prefix scan DeleteInstance uses) and byte-copy each under the clone's name,
        // preserving the source's text/binary suffix.
        if (IEnumerableFileSystem* enumerable = m_mount->AsEnumerable())
        {
            const String folder = group.Path();
            String prefix(src->Name());
            prefix.PushBack(utf8char('.'));
            Array<DirEntry> entries;
            if (enumerable->Enumerate(folder.AsView(), entries).IsOk())
            {
                for (const DirEntry& entry : entries)
                {
                    if (entry.isDirectory || entry.name.Size() <= prefix.Size())
                    {
                        continue;
                    }
                    if (entry.name.AsView().SubStr(0, prefix.Size()) != prefix.AsView())
                    {
                        continue;
                    }
                    const usize suffix = SidecarSuffixSize(entry.name.AsView());
                    if (suffix == 0)
                    {
                        continue;
                    }
                    // "<src>.<stream>.<suffix>" -> stream name between prefix and suffix.
                    const StringView fileName = entry.name.AsView();
                    const StringView stream =
                        fileName.SubStr(prefix.Size(), fileName.Size() - prefix.Size() - suffix);
                    if (stream.IsEmpty())
                    {
                        continue;
                    }
                    const StreamEncoding encoding =
                        suffix == 5 ? StreamEncoding::Text : StreamEncoding::Binary;
                    if (UniquePtr<IStream> data = src->ReadData(stream))
                    {
                        Array<byte> bytes;
                        bytes.Resize(static_cast<usize>(data->Size()));
                        if (data->Read(bytes.Data(), bytes.Size()) == bytes.Size())
                        {
                            (void)copy->WriteData(
                                stream, Span<const byte>{bytes.Data(), bytes.Size()}, encoding);
                        }
                    }
                }
            }
        }
        return copy;
    }

    Status ContentDatabase::CopyContentForward(Instance& dest, ContentDatabase& src,
                                               const Guid& srcId)
    {
        Instance* source = src.GetInstance(srcId);
        if (source == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }

        // Primary object: round-trips under dest's identity. dest.guid == src.guid (the variant cook
        // keeps product-guid == source-guid in every DB), so the re-serialized bytes are identical.
        RefPtr<ISerializable> object = source->ReadObject();
        if (object.Get() == nullptr)
        {
            return Status{ErrorCode::Unknown};
        }
        const Status wrote = dest.WriteObject(*object);
        if (!wrote.IsOk())
        {
            return wrote;
        }

        // Every "<srcName>.<stream>.<suffix>" sidecar (streams keep no directory) -> byte-copy
        // into dest under the same stream name and text/binary suffix. Same prefix scan
        // CloneInstance uses, but across DBs/mounts.
        if (IEnumerableFileSystem* enumerable = src.m_mount->AsEnumerable())
        {
            const String folder = source->OwningGroup().Path();
            String prefix(source->Name());
            prefix.PushBack(utf8char('.'));
            Array<DirEntry> entries;
            if (enumerable->Enumerate(folder.AsView(), entries).IsOk())
            {
                for (const DirEntry& entry : entries)
                {
                    if (entry.isDirectory || entry.name.Size() <= prefix.Size())
                    {
                        continue;
                    }
                    if (entry.name.AsView().SubStr(0, prefix.Size()) != prefix.AsView())
                    {
                        continue;
                    }
                    const usize suffix = SidecarSuffixSize(entry.name.AsView());
                    if (suffix == 0)
                    {
                        continue;
                    }
                    const StringView fileName = entry.name.AsView();
                    const StringView stream =
                        fileName.SubStr(prefix.Size(), fileName.Size() - prefix.Size() - suffix);
                    if (stream.IsEmpty())
                    {
                        continue;
                    }
                    const StreamEncoding encoding =
                        suffix == 5 ? StreamEncoding::Text : StreamEncoding::Binary;
                    if (UniquePtr<IStream> data = source->ReadData(stream))
                    {
                        Array<byte> bytes;
                        bytes.Resize(static_cast<usize>(data->Size()));
                        if (data->Read(bytes.Data(), bytes.Size()) == bytes.Size())
                        {
                            (void)dest.WriteData(
                                stream, Span<const byte>{bytes.Data(), bytes.Size()}, encoding);
                        }
                    }
                }
            }
        }
        return Status{};
    }

    Status ContentDatabase::RenameInstance(const Guid& id, StringView newName)
    {
        Instance* instance = GetInstance(id);
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        if (newName.IsEmpty() || newName == instance->Name())
        {
            return Status{ErrorCode::InvalidArgument};
        }
        if (instance->OwningGroup().GetInstance(newName) != nullptr)
        {
            return Status{ErrorCode::AlreadyExists};
        }
        IWritableFileSystem* writable = m_mount->AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        const String folder = instance->OwningGroup().Path();
        const String oldEnvelope = instance->EnvelopePath();

        // Sidecars first (prefix scan, like delete): "<old>.<stream>.<suffix>" ->
        // "<new>.<stream>.<suffix>" (the tail keeps the stream's text/binary suffix).
        if (IEnumerableFileSystem* enumerable = m_mount->AsEnumerable())
        {
            String prefix(instance->Name());
            prefix.PushBack(utf8char('.'));
            Array<DirEntry> entries;
            if (enumerable->Enumerate(folder.AsView(), entries).IsOk())
            {
                for (const DirEntry& entry : entries)
                {
                    if (entry.isDirectory || entry.name.Size() <= prefix.Size())
                    {
                        continue;
                    }
                    if (entry.name.AsView().SubStr(0, prefix.Size()) != prefix.AsView())
                    {
                        continue;
                    }
                    if (SidecarSuffixSize(entry.name.AsView()) == 0)
                    {
                        continue;
                    }
                    String renamed(newName);
                    renamed.Append(entry.name.AsView().SubStr(
                        instance->Name().Size(), entry.name.Size() - instance->Name().Size()));
                    (void)writable->Move(JoinPath(folder.AsView(), entry.name.AsView()).AsView(),
                                         JoinPath(folder.AsView(), renamed.AsView()).AsView());
                }
            }
        }

        // The envelope may not exist yet (instance created, never written) - that's fine.
        instance->m_name = String(newName);
        instance->OwningGroup().ResortChildren();
        if (m_mount->Exists(oldEnvelope.AsView()))
        {
            const Status moved =
                writable->Move(oldEnvelope.AsView(), instance->EnvelopePath().AsView());
            if (!moved.IsOk())
            {
                return moved;
            }
        }
        return Status{};
    }

    Status ContentDatabase::MoveInstance(const Guid& id, Group& to)
    {
        Instance* instance = GetInstance(id);
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        Group& from = instance->OwningGroup();
        if (&from == &to)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        if (to.GetInstance(instance->Name()) != nullptr)
        {
            return Status{ErrorCode::AlreadyExists};
        }
        IWritableFileSystem* writable = m_mount->AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        const String fromFolder = from.Path();
        const String toFolder = to.Path();
        const String oldEnvelope = instance->EnvelopePath();
        if (!toFolder.IsEmpty() && !m_mount->Exists(toFolder.AsView()))
        {
            (void)writable->CreateDirectory(toFolder.AsView());
        }
        // Sidecars first (prefix scan, as a rename does): "<name>.<stream>.<suffix>" each moves.
        if (IEnumerableFileSystem* enumerable = m_mount->AsEnumerable())
        {
            String prefix(instance->Name());
            prefix.PushBack(utf8char('.'));
            Array<DirEntry> entries;
            if (enumerable->Enumerate(fromFolder.AsView(), entries).IsOk())
            {
                for (const DirEntry& entry : entries)
                {
                    if (entry.isDirectory || entry.name.Size() <= prefix.Size() ||
                        entry.name.AsView().SubStr(0, prefix.Size()) != prefix.AsView() ||
                        SidecarSuffixSize(entry.name.AsView()) == 0)
                    {
                        continue;
                    }
                    (void)writable->Move(JoinPath(fromFolder.AsView(), entry.name.AsView()).AsView(),
                                         JoinPath(toFolder.AsView(), entry.name.AsView()).AsView());
                }
            }
        }

        // Out of its group and into the other, in its name order (the envelope may not exist
        // yet: an instance created and never written).
        from.RemoveInstance(instance);
        instance->m_group = &to;
        usize at = to.m_instances.Size();
        for (usize i = 0; i < to.m_instances.Size(); ++i)
        {
            if (Group::NameLess(instance->Name(), to.m_instances[i]->Name()))
            {
                at = i;
                break;
            }
        }
        to.m_instances.Insert(at, instance);
        Touch();
        if (m_mount->Exists(oldEnvelope.AsView()))
        {
            const Status moved = writable->Move(oldEnvelope.AsView(), instance->EnvelopePath().AsView());
            if (!moved.IsOk())
            {
                return moved;
            }
        }
        return Status{};
    }

    Status ContentDatabase::RenameGroup(Group& group, StringView newName)
    {
        if (group.Parent() == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        } // the root
        if (newName.IsEmpty() || newName == group.Name())
        {
            return Status{ErrorCode::InvalidArgument};
        }
        if (group.Parent()->GetGroup(newName) != nullptr)
        {
            return Status{ErrorCode::AlreadyExists};
        }
        IWritableFileSystem* writable = m_mount->AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        const String oldPath = group.Path();
        group.m_name = String(newName);
        group.Parent()->ResortChildren();
        // The directory may not exist yet (group created, nothing written under it).
        if (!oldPath.IsEmpty() && m_mount->Exists(oldPath.AsView()))
        {
            const Status moved = writable->Move(oldPath.AsView(), group.Path().AsView());
            if (!moved.IsOk())
            {
                return moved;
            }
        }
        return Status{};
    }

    Status ContentDatabase::DeleteInstance(const Guid& id)
    {
        Instance* instance = GetInstance(id);
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        IWritableFileSystem* writable = m_mount->AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        // Envelope + every "<name>.<stream>.<suffix>" sidecar (streams keep no directory, so
        // the group folder is enumerated for siblings with the instance's file prefix).
        (void)writable->Delete(instance->EnvelopePath().AsView());
        if (IEnumerableFileSystem* enumerable = m_mount->AsEnumerable())
        {
            const String folder = instance->OwningGroup().Path();
            String prefix(instance->Name());
            prefix.PushBack(utf8char('.'));
            Array<DirEntry> entries;
            if (enumerable->Enumerate(folder.AsView(), entries).IsOk())
            {
                for (const DirEntry& entry : entries)
                {
                    if (entry.isDirectory || entry.name.Size() <= prefix.Size())
                    {
                        continue;
                    }
                    if (entry.name.AsView().SubStr(0, prefix.Size()) != prefix.AsView())
                    {
                        continue;
                    }
                    if (SidecarSuffixSize(entry.name.AsView()) == 0)
                    {
                        continue;
                    }
                    (void)writable->Delete(JoinPath(folder.AsView(), entry.name.AsView()).AsView());
                }
            }
        }

        m_byGuid.Remove(id);
        instance->OwningGroup().RemoveInstance(instance);
        for (usize i = 0; i < m_allInstances.Size(); ++i)
        {
            if (m_allInstances[i] == instance)
            {
                m_allInstances.RemoveAtSwap(i);
                break;
            }
        }
        m_allocator->Delete(instance);
        return Status{};
    }

    Status ContentDatabase::DeleteGroup(Group& group)
    {
        if (group.Parent() == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        } // the root
        IWritableFileSystem* writable = m_mount->AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        // Instances first (copy the id list - DeleteInstance unlinks from m_instances)...
        Array<Guid> ids;
        for (Instance* instance : group.Instances())
        {
            ids.PushBack(instance->Id());
        }
        for (const Guid& id : ids)
        {
            (void)DeleteInstance(id);
        }
        // ...then child groups, bottom-up (copy - the recursion unlinks from m_groups).
        Array<Group*> children;
        for (Group* child : group.Groups())
        {
            children.PushBack(child);
        }
        for (Group* child : children)
        {
            (void)DeleteGroup(*child);
        }

        // The directory may never have materialized (group created, nothing written).
        const String path = group.Path();
        if (!path.IsEmpty() && m_mount->Exists(path.AsView()))
        {
            const Status removed = writable->DeleteDirectory(path.AsView());
            if (!removed.IsOk())
            {
                return removed;
            }
        }

        // Unregister from the parent and the pool, then destroy.
        Touch();
        Group* parent = group.Parent();
        for (usize i = 0; i < parent->m_groups.Size(); ++i)
        {
            if (parent->m_groups[i] == &group)
            {
                parent->m_groups.RemoveAt(i);
                break;
            }
        }
        for (usize i = 0; i < m_allGroups.Size(); ++i)
        {
            if (m_allGroups[i] == &group)
            {
                m_allGroups.RemoveAtSwap(i);
                break;
            }
        }
        m_allocator->Delete(&group);
        return Status{};
    }
}
