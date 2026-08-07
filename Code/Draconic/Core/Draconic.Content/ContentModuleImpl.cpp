// Draconic Content - draconic.content implementation unit.
//
// Out-of-line definitions for Instance/Group/ContentDatabase (sec 3.2 / sec 10.6).
// ContentModule.cppm keeps the class declarations + trivial inline accessors.

module;
#include "Draconic.Foundation/Prelude.h"
#include <chrono>
#include <random>

module draconic.content;

import draconic.foundation;
import draconic.vfs;

using namespace draconic::foundation;
using namespace draconic::vfs;

namespace draconic::content
{
    ContentDatabase::ContentDatabase(IFileSystem& mount, SerializerFactory factory,
                                     StringView fileExtension, SerializableRegistry& serializables,
                                     TypeRegistry& types)
        : m_mount(&mount), m_factory(static_cast<SerializerFactory&&>(factory)),
          m_extension(fileExtension), m_serializables(&serializables), m_types(&types)
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

    Group* Group::AddChildGroup(StringView name)
    {
        Group* child = m_db->NewGroup(this, name);
        m_groups.PushBack(child);
        return child;
    }

    Instance* Group::AddInstance(const Guid& id, StringView name, StringView typeNs,
                                 StringView typeName)
    {
        Instance* instance = m_db->NewInstance(*this, id, name, typeNs, typeName);
        m_instances.PushBack(instance);
        return instance;
    }

    void Group::RemoveInstance(Instance* instance)
    {
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

    String Instance::DataPath(StringView streamName) const
    {
        String path = Path();
        path.PushBack(utf8char('.'));
        path.Append(streamName);
        path.Append(u8".bin");
        return path;
    }

    RefPtr<ISerializable> Instance::ReadObject() const
    {
        UniquePtr<IStream> stream = m_db->Mount().Open(EnvelopePath().AsView(), FileMode::Read);
        if (!stream)
        {
            return RefPtr<ISerializable>{};
        }

        UniquePtr<SerializerContext> ctx = m_db->CreateSerializer(*stream, SerializeMode::Read);
        if (!ctx || ctx->serializer == nullptr)
        {
            return RefPtr<ISerializable>{};
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
            return RefPtr<ISerializable>{};
        }

        // Resolve the type and construct the object.
        const TypeInfo* type = m_db->Types().FindByName(reinterpret_cast<const char*>(ns.CStr()),
                                                        reinterpret_cast<const char*>(name.CStr()));
        if (type == nullptr)
        {
            return RefPtr<ISerializable>{};
        }

        RefPtr<ISerializable> object = m_db->Serializables().Create(type->id);
        if (object.Get() == nullptr)
        {
            return RefPtr<ISerializable>{};
        }

        // Deserialize the payload under the STORED data-version scope (migration branches in
        // Serialize see the version the envelope was written with).
        BeginVersionedPayload(ar, *type);
        ar.Key("payload");
        ar.BeginObject();
        object->Serialize(ar);
        ar.EndObject();
        EndVersionedPayload(ar);
        return ar.IsOk() ? object : RefPtr<ISerializable>{};
    }

    UniquePtr<IStream> Instance::ReadData(StringView streamName) const
    {
        return m_db->Mount().Open(DataPath(streamName).AsView(), FileMode::Read);
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

        // Write header + payload (payload wrapped in the object's data-version scope, so
        // Serialize bodies can branch on ar.Version() for migration).
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

    Status Instance::WriteData(StringView streamName, Span<const byte> data)
    {
        IWritableFileSystem* writable = m_db->Mount().AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }
        return writable->Save(DataPath(streamName).AsView(), data);
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

        // Sidecar streams keep no directory - enumerate "<srcName>.<stream>.bin" siblings (the
        // same prefix scan DeleteInstance uses) and byte-copy each under the clone's name.
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
                    if (!EndsWith(entry.name.AsView(), u8".bin"))
                    {
                        continue;
                    }
                    // "<src>.<stream>.bin" -> stream name between prefix and ".bin".
                    const StringView fileName = entry.name.AsView();
                    const StringView stream =
                        fileName.SubStr(prefix.Size(), fileName.Size() - prefix.Size() - 4);
                    if (stream.IsEmpty())
                    {
                        continue;
                    }
                    if (UniquePtr<IStream> data = src->ReadData(stream))
                    {
                        Array<byte> bytes;
                        bytes.Resize(static_cast<usize>(data->Size()));
                        if (data->Read(bytes.Data(), bytes.Size()) == bytes.Size())
                        {
                            (void)copy->WriteData(stream,
                                                  Span<const byte>{bytes.Data(), bytes.Size()});
                        }
                    }
                }
            }
        }
        return copy;
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

        // Sidecars first (prefix scan, like delete): "<old>.<stream>.bin" -> "<new>.<stream>.bin".
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
                    if (!EndsWith(entry.name.AsView(), u8".bin"))
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

        // Envelope + every "<name>.<stream>.bin" sidecar (streams keep no directory, so the
        // group folder is enumerated for siblings with the instance's file prefix).
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
                    if (!EndsWith(entry.name.AsView(), u8".bin"))
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
        DefaultAllocator().Delete(instance);
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
        DefaultAllocator().Delete(&group);
        return Status{};
    }
}
