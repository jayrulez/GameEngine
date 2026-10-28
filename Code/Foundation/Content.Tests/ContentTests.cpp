// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.xml.serialization;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::content;

namespace
{
    class MaterialResource final : public ISerializable
    {
        RTTI_OBJECT(MaterialResource, ISerializable)
    public:
        i32 shininess = 0;
        String shader;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "shininess", shininess);
            foundation::core::Serialize(ar, "shader", shader);
        }
    };

    // Counts its reads, so a test can tell a refused payload was never walked.
    class CountedRecord final : public ISerializable
    {
        RTTI_OBJECT(CountedRecord, ISerializable)
    public:
        static inline i32 reads = 0;
        i32 value = 0;

        void Serialize(ISerializer& ar) override
        {
            if (ar.Mode() == SerializeMode::Read)
            {
                ++reads;
            }
            foundation::core::Serialize(ar, "value", value);
        }
    };

    // A mount that passes everything to another and counts the reads reaching its files.
    class CountingMount final : public IFileSystem
    {
        class Counted final : public IStream
        {
        public:
            Counted(UniquePtr<IStream> inner, u64& readCount) : m_inner(Move(inner)), m_reads(&readCount) {}

            [[nodiscard]] u64 Read(void* data, u64 size) override
            {
                ++*m_reads;
                return m_inner->Read(data, size);
            }
            [[nodiscard]] u64 Write(const void* data, u64 size) override { return m_inner->Write(data, size); }
            [[nodiscard]] i64 Seek(i64 offset, SeekOrigin origin) override { return m_inner->Seek(offset, origin); }
            [[nodiscard]] i64 Tell() const override { return m_inner->Tell(); }
            [[nodiscard]] i64 Size() const override { return m_inner->Size(); }
            [[nodiscard]] bool IsValid() const override { return m_inner->IsValid(); }

        private:
            UniquePtr<IStream> m_inner;
            u64* m_reads;
        };

    public:
        explicit CountingMount(IFileSystem& inner) : m_inner(&inner) {}

        [[nodiscard]] UniquePtr<IStream> Open(StringView path, FileMode mode) override
        {
            UniquePtr<IStream> stream = m_inner->Open(path, mode);
            if (!stream)
            {
                return stream;
            }
            return MakeUnique<Counted>(DefaultAllocator(), Move(stream), reads);
        }
        [[nodiscard]] bool Exists(StringView path) override { return m_inner->Exists(path); }
        [[nodiscard]] IEnumerableFileSystem* AsEnumerable() noexcept override { return m_inner->AsEnumerable(); }
        [[nodiscard]] IWritableFileSystem* AsWritable() noexcept override { return m_inner->AsWritable(); }
        [[nodiscard]] IStatFileSystem* AsStat() noexcept override { return m_inner->AsStat(); }

        u64 reads = 0;

    private:
        IFileSystem* m_inner;
    };

    // Many small fields: a record whose binary envelope is read field by field.
    class WordsRecord final : public ISerializable
    {
        RTTI_OBJECT(WordsRecord, ISerializable)
    public:
        Array<String> words;

        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "words", words); }
    };

    // Every occurrence of `from` in `text` replaced with `to` (byte-wise; test-local helper).
    String ReplaceAll(StringView text, StringView from, StringView to)
    {
        StringBuilder out;
        usize i = 0;
        while (i < text.Size())
        {
            if (i + from.Size() <= text.Size() && text.SubStr(i, from.Size()) == from)
            {
                out.Append(to);
                i += from.Size();
                continue;
            }
            out.Append(text[i]);
            ++i;
        }
        return out.Take();
    }

    void RemoveTree(StringView root)
    {
        FileDelete(JoinPath(root, u8"materials/steel.rasset"));
        FileDelete(JoinPath(root, u8"materials/steel.xasset"));
        FileDelete(JoinPath(root, u8"materials/steel.extra.bin"));
        RemoveDirectory(JoinPath(root, u8"materials"));
        RemoveDirectory(root);
    }

    // Shared round-trip test body.
    void RunRoundTripTest(StringView dbDir, StringView ext, SerializerFactory (*makeFactory)())
    {
        GlobalTypeRegistry().Register(MaterialResource::StaticType());
        RegisterSerializable<MaterialResource>();

        RemoveTree(dbDir);
        NativeFileSystem mount(dbDir, DefaultAllocator());

        Guid steelId;
        const byte extra[] = {byte{0xAB}, byte{0xCD}, byte{0xEF}};

        // --- author ---
        {
            ContentDatabase db(DefaultAllocator(), mount, makeFactory(), ext);
            Group* materials = db.RootGroup()->CreateGroup(u8"materials");
            REQUIRE(materials != nullptr);

            foundation::content::Instance* steel =
                materials->CreateInstance(u8"steel", MaterialResource::StaticType());
            REQUIRE(steel != nullptr);
            steelId = steel->Id();
            CHECK(static_cast<bool>(steelId));
            CHECK(steel->Path() == u8"materials/steel");

            MaterialResource mat;
            mat.shininess = 64;
            mat.shader = u8"pbr/metal";
            CHECK(steel->WriteObject(mat).IsOk());
            CHECK(steel->WriteData(u8"extra", Span<const byte>{extra, ArrayCount(extra)}).IsOk());
        }

        // --- reopen: a fresh database scans the mount from disk ---
        {
            ContentDatabase db(DefaultAllocator(), mount, makeFactory(), ext);

            Group* materials = db.RootGroup()->GetGroup(u8"materials");
            REQUIRE(materials != nullptr);
            REQUIRE(materials->GetInstance(u8"steel") != nullptr);

            foundation::content::Instance* byPath = db.GetInstance(u8"materials/steel");
            foundation::content::Instance* byGuid = db.GetInstance(steelId);
            REQUIRE(byPath != nullptr);
            CHECK(byPath == byGuid);
            CHECK(byPath->Id() == steelId);

            RefPtr<ISerializable> obj = db.ReadObject(steelId);
            REQUIRE(obj.Get() != nullptr);
            MaterialResource* mat = Cast<MaterialResource>(obj.Get());
            REQUIRE(mat != nullptr);
            CHECK(mat->shininess == 64);
            CHECK(mat->shader == u8"pbr/metal");

            UniquePtr<IStream> data = byPath->ReadData(u8"extra");
            REQUIRE(static_cast<bool>(data));
            byte buffer[3] = {};
            CHECK(data->Read(buffer, 3) == 3u);
            CHECK(buffer[0] == byte{0xAB});
            CHECK(buffer[2] == byte{0xEF});

            CHECK(db.GetInstance(u8"materials/nope") == nullptr);
            CHECK(db.GetInstance(Guid{1, 2}) == nullptr);
        }

        RemoveTree(dbDir);
    }

    SerializerFactory MakeBinaryFactory() { return BinarySerializerFactory(); }
    SerializerFactory MakeXmlFactory() { return foundation::xml::XmlSerializerFactory(); }
}

RTTI_DEFINE_OBJECT(MaterialResource, "rtti::content::test")
RTTI_DEFINE_OBJECT(CountedRecord, "rtti::content::test")
RTTI_DEFINE_OBJECT(WordsRecord, "rtti::content::test")

TEST_CASE("content: binary round-trip")
{
    RunRoundTripTest(u8"scratch_content_test_db_bin", u8".rasset", MakeBinaryFactory);
}

TEST_CASE("content: XML round-trip")
{
    RunRoundTripTest(u8"scratch_content_test_db_xml", u8".xasset", MakeXmlFactory);
}

TEST_CASE("content: DeleteInstance removes envelope + stream sidecars + registrations")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    const StringView dir = u8"scratch_content_delete_db";
    RemoveTree(dir);
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".xasset");

    Group* materials = db.RootGroup()->CreateGroup(u8"materials");
    foundation::content::Instance* steel =
        materials->CreateInstance(u8"steel", MaterialResource::StaticType());
    REQUIRE(steel != nullptr);
    const Guid id = steel->Id();

    MaterialResource res;
    res.shininess = 3;
    REQUIRE(steel->WriteObject(res).IsOk());
    const byte extra[] = {byte{1}, byte{2}};
    REQUIRE(steel->WriteData(u8"extra", Span<const byte>(extra, 2)).IsOk());
    REQUIRE(mount.Exists(u8"materials/steel.xasset"));
    REQUIRE(mount.Exists(u8"materials/steel.extra.bin"));

    // Explicit-guid creation (the cook driver's product path): same id in another DB works.
    REQUIRE(db.DeleteInstance(id).IsOk());
    CHECK(db.GetInstance(id) == nullptr);
    CHECK(materials->GetInstance(u8"steel") == nullptr);
    CHECK_FALSE(mount.Exists(u8"materials/steel.xasset"));
    CHECK_FALSE(mount.Exists(u8"materials/steel.extra.bin"));

    // Unknown ids are a clean NotFound; a fresh explicit-id instance is registered.
    CHECK(db.DeleteInstance(id).Code() == ErrorCode::NotFound);
    foundation::content::Instance* again =
        materials->CreateInstanceWithId(id, u8"steel", MaterialResource::StaticType());
    REQUIRE(again != nullptr);
    CHECK(again->Id() == id);
    CHECK(db.GetInstance(id) == again);

    RemoveTree(dir);
}

TEST_CASE("content: Instance::DeleteData removes exactly one stream sidecar, idempotently")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    const StringView dir = u8"scratch_content_deletedata_db";
    RemoveTree(dir);
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".xasset");

    foundation::content::Instance* inst =
        db.RootGroup()->CreateInstance(u8"thing", MaterialResource::StaticType());
    REQUIRE(inst != nullptr);
    const byte bytes[] = {byte{7}, byte{8}};
    REQUIRE(inst->WriteData(u8"alpha", Span<const byte>(bytes, 2)).IsOk());
    REQUIRE(inst->WriteData(u8"beta", Span<const byte>(bytes, 2)).IsOk());
    REQUIRE(mount.Exists(u8"thing.alpha.bin"));
    REQUIRE(mount.Exists(u8"thing.beta.bin"));

    // Removes ONLY the named stream (instance-scoped path - no over-delete).
    CHECK(inst->DeleteData(u8"alpha").IsOk());
    CHECK_FALSE(mount.Exists(u8"thing.alpha.bin"));
    CHECK(mount.Exists(u8"thing.beta.bin"));

    // Idempotent: a missing sidecar is Ok, not an error.
    CHECK(inst->DeleteData(u8"alpha").IsOk());
    CHECK(inst->DeleteData(u8"never-written").IsOk());

    RemoveTree(dir);
}

namespace
{
    // The text-stream tests create their own instances; RemoveTree only knows the steel
    // fixture, so these clean their exact files - leftovers otherwise fail the NEXT run
    // (a fresh DB scans the stale envelope and CloneInstance hits AlreadyExists).
    void RemoveTextStreamScratch(StringView root)
    {
        static const char8_t* const kNames[] = {
            u8"level.xasset",  u8"level.scene.data",  u8"level.scene.bin",
            u8"level.geometry.bin", u8"level2.xasset", u8"level2.scene.data",
            u8"level2.scene.bin", u8"level2.geometry.bin", u8"world.xasset",
            u8"world.scene.data", u8"world.geometry.bin"};
        for (const char8_t* name : kNames)
        {
            FileDelete(JoinPath(root, name));
        }
        RemoveDirectory(root);
    }
}

TEST_CASE("content: text streams write .data sidecars; ReadData resolves a stream by name, not suffix")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    const StringView dir = u8"scratch_content_textstream_db";
    RemoveTextStreamScratch(dir);
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".xasset");

    foundation::content::Instance* inst =
        db.RootGroup()->CreateInstance(u8"level", MaterialResource::StaticType());
    REQUIRE(inst != nullptr);

    // Text encoding lands under ".data"; the binary suffix stays untouched.
    const char8_t xml[] = u8"<root/>";
    const Span<const byte> xmlBytes{reinterpret_cast<const byte*>(xml), sizeof(xml) - 1};
    REQUIRE(inst->WriteData(u8"scene", xmlBytes, StreamEncoding::Text).IsOk());
    CHECK(mount.Exists(u8"level.scene.data"));
    CHECK_FALSE(mount.Exists(u8"level.scene.bin"));

    // ReadData round-trips through the text sidecar.
    {
        UniquePtr<IStream> stream = inst->ReadData(u8"scene");
        REQUIRE(stream.Get() != nullptr);
        CHECK(static_cast<usize>(stream->Size()) == xmlBytes.Size());
    }

    // The suffix is not part of a stream's identity: readers do not know the encoding, so the
    // same stream stored under ".bin" resolves too (a stream never exists under both).
    REQUIRE(mount.AsWritable()->Delete(u8"level.scene.data").IsOk());
    REQUIRE(mount.AsWritable()->Save(u8"level.scene.bin", xmlBytes).IsOk());
    {
        UniquePtr<IStream> stream = inst->ReadData(u8"scene");
        REQUIRE(stream.Get() != nullptr);
        CHECK(static_cast<usize>(stream->Size()) == xmlBytes.Size());
    }

    // Re-saving as text writes ".data" and removes the ".bin" sibling (one suffix per stream).
    REQUIRE(inst->WriteData(u8"scene", xmlBytes, StreamEncoding::Text).IsOk());
    CHECK(mount.Exists(u8"level.scene.data"));
    CHECK_FALSE(mount.Exists(u8"level.scene.bin"));

    // DeleteData removes the text sidecar too.
    CHECK(inst->DeleteData(u8"scene").IsOk());
    CHECK_FALSE(mount.Exists(u8"level.scene.data"));

    RemoveTextStreamScratch(dir);
}

TEST_CASE("content: clone + rename preserve a stream's text/binary suffix")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    const StringView dir = u8"scratch_content_textclone_db";
    RemoveTextStreamScratch(dir);
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".xasset");

    foundation::content::Instance* inst =
        db.RootGroup()->CreateInstance(u8"level", MaterialResource::StaticType());
    REQUIRE(inst != nullptr);
    MaterialResource res;
    REQUIRE(inst->WriteObject(res).IsOk());
    const char8_t xml[] = u8"<root/>";
    REQUIRE(inst->WriteData(u8"scene",
                            Span<const byte>{reinterpret_cast<const byte*>(xml), sizeof(xml) - 1},
                            StreamEncoding::Text)
                .IsOk());
    const byte raw[] = {byte{9}};
    REQUIRE(inst->WriteData(u8"geometry", Span<const byte>(raw, 1)).IsOk());

    // Clone: each sidecar keeps its own suffix.
    foundation::content::Instance* copy = db.CloneInstance(inst->Id(), u8"level2");
    REQUIRE(copy != nullptr);
    CHECK(mount.Exists(u8"level2.scene.data"));
    CHECK(mount.Exists(u8"level2.geometry.bin"));
    CHECK_FALSE(mount.Exists(u8"level2.scene.bin"));

    // Rename: sidecars move under the new name, suffixes intact.
    REQUIRE(db.RenameInstance(inst->Id(), u8"world").IsOk());
    CHECK(mount.Exists(u8"world.scene.data"));
    CHECK(mount.Exists(u8"world.geometry.bin"));
    CHECK_FALSE(mount.Exists(u8"level.scene.data"));

    // DeleteInstance sweeps both suffix families.
    REQUIRE(db.DeleteInstance(inst->Id()).IsOk());
    CHECK_FALSE(mount.Exists(u8"world.scene.data"));
    CHECK_FALSE(mount.Exists(u8"world.geometry.bin"));

    RemoveTextStreamScratch(dir);
}

TEST_CASE("content: CloneInstance deep-copies object + sidecars under a fresh guid")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    const StringView dir = u8"scratch_content_clone_db";
    RemoveTree(dir);
    FileDelete(JoinPath(dir, u8"materials/copper.xasset"));
    FileDelete(JoinPath(dir, u8"materials/copper.extra.bin"));
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".xasset");

    Group* materials = db.RootGroup()->CreateGroup(u8"materials");
    foundation::content::Instance* steel =
        materials->CreateInstance(u8"steel", MaterialResource::StaticType());
    REQUIRE(steel != nullptr);
    MaterialResource res;
    res.shininess = 7;
    res.shader = String(u8"pbr");
    REQUIRE(steel->WriteObject(res).IsOk());
    const byte extra[] = {byte{9}, byte{8}, byte{7}};
    REQUIRE(steel->WriteData(u8"extra", Span<const byte>(extra, 3)).IsOk());

    foundation::content::Instance* copy = db.CloneInstance(steel->Id(), u8"copper");
    REQUIRE(copy != nullptr);
    CHECK(copy->Id() != steel->Id()); // fresh identity
    CHECK(copy->Name() == u8"copper");
    CHECK(copy->TypeName() == steel->TypeName());
    CHECK(&copy->OwningGroup() == materials);
    CHECK(mount.Exists(u8"materials/copper.xasset"));
    CHECK(mount.Exists(u8"materials/copper.extra.bin"));

    // Deep copy: the clone's primary object matches the source's content.
    RefPtr<ISerializable> obj = copy->ReadObject();
    auto* cloned = Cast<MaterialResource>(obj.Get());
    REQUIRE(cloned != nullptr);
    CHECK(cloned->shininess == 7);
    CHECK(cloned->shader == u8"pbr");
    UniquePtr<IStream> data = copy->ReadData(u8"extra");
    REQUIRE(data);
    byte bytes[3] = {};
    REQUIRE(data->Read(bytes, 3) == 3u);
    CHECK(bytes[0] == byte{9});
    CHECK(bytes[2] == byte{7});

    // Name collisions + unknown ids fail cleanly.
    CHECK(db.CloneInstance(steel->Id(), u8"copper") == nullptr);
    Random rng(99);
    CHECK(db.CloneInstance(Guid::Generate(rng), u8"x") == nullptr);

    (void)db.DeleteInstance(copy->Id());
    (void)db.DeleteInstance(steel->Id());
    RemoveTree(dir);
}

TEST_CASE("content: RenameInstance moves envelope + sidecars; RenameGroup moves the directory")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    const StringView dir = u8"scratch_content_rename_db";
    RemoveTree(dir);
    FileDelete(JoinPath(dir, u8"materials/bronze.xasset"));
    FileDelete(JoinPath(dir, u8"materials/bronze.extra.bin"));
    FileDelete(JoinPath(dir, u8"metals/bronze.xasset"));
    FileDelete(JoinPath(dir, u8"metals/bronze.extra.bin"));
    RemoveDirectory(JoinPath(dir, u8"metals"));
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".xasset");

    Group* materials = db.RootGroup()->CreateGroup(u8"materials");
    foundation::content::Instance* steel =
        materials->CreateInstance(u8"steel", MaterialResource::StaticType());
    REQUIRE(steel != nullptr);
    const Guid id = steel->Id();
    MaterialResource res;
    res.shininess = 5;
    REQUIRE(steel->WriteObject(res).IsOk());
    const byte extra[] = {byte{1}, byte{2}};
    REQUIRE(steel->WriteData(u8"extra", Span<const byte>(extra, 2)).IsOk());

    // Instance rename: files move, guid + group stay, content still reads.
    REQUIRE(db.RenameInstance(id, u8"bronze").IsOk());
    CHECK(steel->Name() == u8"bronze");
    CHECK(db.GetInstance(id) == steel); // guid identity untouched
    CHECK(materials->GetInstance(u8"bronze") == steel);
    CHECK(materials->GetInstance(u8"steel") == nullptr);
    CHECK_FALSE(mount.Exists(u8"materials/steel.xasset"));
    CHECK(mount.Exists(u8"materials/bronze.xasset"));
    CHECK_FALSE(mount.Exists(u8"materials/steel.extra.bin"));
    CHECK(mount.Exists(u8"materials/bronze.extra.bin"));
    {
        RefPtr<ISerializable> object = steel->ReadObject();
        auto* loaded = Cast<MaterialResource>(object.Get());
        REQUIRE(loaded != nullptr);
        CHECK(loaded->shininess == 5);
    }

    // Collisions + bad input fail cleanly.
    foundation::content::Instance* other =
        materials->CreateInstance(u8"iron", MaterialResource::StaticType());
    REQUIRE(other != nullptr);
    CHECK(db.RenameInstance(other->Id(), u8"bronze").Code() == ErrorCode::AlreadyExists);
    CHECK(db.RenameInstance(id, u8"").Code() == ErrorCode::InvalidArgument);

    // Group rename: the directory moves; child paths derive from the new name.
    REQUIRE(db.RenameGroup(*materials, u8"metals").IsOk());
    CHECK(materials->Name() == u8"metals");
    CHECK(mount.Exists(u8"metals/bronze.xasset"));
    CHECK_FALSE(mount.Exists(u8"materials/bronze.xasset"));
    CHECK(steel->Path() == u8"metals/bronze");
    CHECK(db.RenameGroup(*db.RootGroup(), u8"x").Code() == ErrorCode::NotSupported);

    (void)db.DeleteInstance(id);
    (void)db.DeleteInstance(other->Id());
    RemoveDirectory(JoinPath(dir, u8"metals"));
    RemoveDirectory(dir);
}

TEST_CASE("content: DeleteGroup removes the whole subtree - files, directories, registrations")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    const StringView dir = u8"scratch_content_delgroup_db";
    // Explicit cleanup (RemoveTree only knows the shared fixture paths).
    auto scrub = [&]()
    {
        FileDelete(JoinPath(dir, u8"outer/inner/b.xasset"));
        FileDelete(JoinPath(dir, u8"outer/a.xasset"));
        FileDelete(JoinPath(dir, u8"outer/a.extra.bin"));
        FileDelete(JoinPath(dir, u8"keep.xasset"));
        RemoveDirectory(JoinPath(dir, u8"outer/inner"));
        RemoveDirectory(JoinPath(dir, u8"outer"));
        RemoveDirectory(dir);
    };
    scrub();
    {
        NativeFileSystem mount(dir, DefaultAllocator());
        ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".xasset");

        // outer/ { a (+sidecar), inner/ { b } } and an unrelated sibling instance.
        Group* outer = db.RootGroup()->CreateGroup(u8"outer");
        Group* inner = outer->CreateGroup(u8"inner");
        foundation::content::Instance* a =
            outer->CreateInstance(u8"a", MaterialResource::StaticType());
        foundation::content::Instance* b =
            inner->CreateInstance(u8"b", MaterialResource::StaticType());
        foundation::content::Instance* keep =
            db.RootGroup()->CreateInstance(u8"keep", MaterialResource::StaticType());
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        REQUIRE(keep != nullptr);
        MaterialResource res;
        REQUIRE(a->WriteObject(res).IsOk());
        REQUIRE(b->WriteObject(res).IsOk());
        REQUIRE(keep->WriteObject(res).IsOk());
        const byte extra[] = {byte{7}};
        REQUIRE(a->WriteData(u8"extra", Span<const byte>(extra, 1)).IsOk());
        const Guid aId = a->Id();
        const Guid bId = b->Id();
        const Guid keepId = keep->Id();

        // The root refuses.
        CHECK(db.DeleteGroup(*db.RootGroup()).Code() == ErrorCode::NotSupported);

        REQUIRE(db.DeleteGroup(*outer).IsOk()); // outer/inner/a/b are DANGLING after this

        // Registrations gone (guid index + tree), the sibling untouched.
        CHECK(db.GetInstance(aId) == nullptr);
        CHECK(db.GetInstance(bId) == nullptr);
        CHECK(db.RootGroup()->GetGroup(u8"outer") == nullptr);
        CHECK(db.GetInstance(keepId) == keep);

        // Files AND directories gone (a rescan must not resurrect ghost groups).
        CHECK_FALSE(mount.Exists(u8"outer/a.xasset"));
        CHECK_FALSE(mount.Exists(u8"outer/a.extra.bin"));
        CHECK_FALSE(mount.Exists(u8"outer/inner/b.xasset"));
        CHECK_FALSE(mount.Exists(u8"outer/inner"));
        CHECK_FALSE(mount.Exists(u8"outer"));
        CHECK(mount.Exists(u8"keep.xasset"));
    }
    {
        // Rescan proves it: a fresh database over the same mount has no ghost of the group.
        NativeFileSystem mount(dir, DefaultAllocator());
        ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".xasset");
        CHECK(db.RootGroup()->GetGroup(u8"outer") == nullptr);
        CHECK(db.RootGroup()->GetInstance(u8"keep") != nullptr);
    }
    scrub();
}

TEST_CASE("content: instance guids are unique across database sessions")
{
    // Regression: the guid rng was default-seeded (fixed PCG constant), so every session
    // replayed the SAME guid sequence - a delete + reimport in a fresh session gave old
    // guids to different assets, cross-typing persisted cook records/products (crash).
    const StringView dirA = u8"scratch_content_guid_a";
    const StringView dirB = u8"scratch_content_guid_b";
    RemoveTree(dirA);
    RemoveTree(dirB);
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    NativeFileSystem mountA(dirA, DefaultAllocator());
    NativeFileSystem mountB(dirB, DefaultAllocator());
    ContentDatabase a(DefaultAllocator(), mountA, BinarySerializerFactory(), u8".rasset");
    ContentDatabase b(DefaultAllocator(), mountB, BinarySerializerFactory(), u8".rasset");

    // Two fresh databases minting the same creation sequence must NOT agree on guids.
    Array<Guid> fromA;
    Array<Guid> fromB;
    Group* ga = a.RootGroup()->CreateGroup(u8"materials");
    Group* gb = b.RootGroup()->CreateGroup(u8"materials");
    for (int i = 0; i < 4; ++i)
    {
        String name(u8"m");
        name.PushBack(static_cast<char8_t>(u8'0' + i));
        fromA.PushBack(ga->CreateInstance(name.AsView(), MaterialResource::StaticType())->Id());
        fromB.PushBack(gb->CreateInstance(name.AsView(), MaterialResource::StaticType())->Id());
    }
    usize collisions = 0;
    for (const Guid& x : fromA)
    {
        for (const Guid& y : fromB)
        {
            if (x == y)
            {
                ++collisions;
            }
        }
    }
    CHECK(collisions == 0u);

    RemoveTree(dirA);
    RemoveTree(dirB);
}

TEST_CASE("content: UniqueInstanceName / UniqueGroupName - the one general dedup")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    const StringView dir = u8"scratch_content_unique_name_db";
    RemoveTree(dir);
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    Group* group = db.RootGroup()->CreateGroup(u8"materials");
    REQUIRE(group != nullptr);

    // A free base comes back untouched.
    CHECK(group->UniqueInstanceName(u8"Thing") == u8"Thing");

    // A taken base steps to `base.2`, `base.3`, ... (CreateInstance would silently
    // return the EXISTING instance - the overwrite hazard the helper exists for).
    foundation::content::Instance* first =
        group->CreateInstance(u8"Thing", MaterialResource::StaticType());
    REQUIRE(first != nullptr);
    CHECK(group->CreateInstance(u8"Thing", MaterialResource::StaticType()) == first);
    CHECK(group->UniqueInstanceName(u8"Thing") == u8"Thing.2");

    // Digits emit most-significant first - counter 12 is "Thing.12", never "Thing.21"
    // (the bug every hand-rolled copy of this loop had).
    for (i32 counter = 2; counter <= 11; ++counter)
    {
        const String next = group->UniqueInstanceName(u8"Thing");
        REQUIRE(group->CreateInstance(next.AsView(), MaterialResource::StaticType()) != nullptr);
    }
    CHECK(group->UniqueInstanceName(u8"Thing") == u8"Thing.12");

    // Same convention for child groups.
    CHECK(group->UniqueGroupName(u8"sub") == u8"sub");
    REQUIRE(group->CreateGroup(u8"sub") != nullptr);
    CHECK(group->UniqueGroupName(u8"sub") == u8"sub.2");

    RemoveTree(dir);
}

TEST_CASE("content: the open scan reports envelope count + bytes opened (I4b instrumentation)")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    const StringView dir = u8"scratch_content_scanstats";
    // The suite's RemoveTree only knows the round-trip test's fixed paths; this test writes
    // its own instances, so clean recursively or the previous run's envelopes pollute the
    // construction scan (otherwise envelopes==3 on the authoring db).
    (void)RemoveDirectoryRecursive(dir);
    NativeFileSystem mount(dir, DefaultAllocator());
    {
        ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(), u8".xasset");
        Group* group = db.RootGroup()->CreateGroup(u8"assets");
        for (i32 i = 0; i < 3; ++i)
        {
            String name = Format(u8"m{}", i);
            foundation::content::Instance* inst =
                group->CreateInstance(name.AsView(), MaterialResource::StaticType());
            REQUIRE(inst != nullptr);
            MaterialResource mat;
            mat.shininess = i;
            CHECK(inst->WriteObject(mat).IsOk());
        }
        // The authoring db never scanned (built in memory): zero envelopes opened.
        CHECK(db.LastScanStats().envelopes == 0);
    }
    {
        ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(), u8".xasset");
        // The reopen scan touched all three envelopes and counted their full file sizes -
        // the number that prices the open-time header scan (XML DOM-parses whole files).
        CHECK(db.LastScanStats().envelopes == 3);
        CHECK(db.LastScanStats().bytesOpened > 0);
    }
}

TEST_CASE("content: group children stay name-sorted through create and rename")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    const StringView dir = u8"scratch_content_sort_db";
    RemoveDirectory(JoinPath(dir, u8"zinc"));
    RemoveDirectory(JoinPath(dir, u8"Alpha"));
    RemoveDirectory(JoinPath(dir, u8"metals"));
    RemoveDirectory(JoinPath(dir, u8"steel"));
    RemoveDirectory(dir);
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".xasset");

    // Created deliberately out of order (and mixed case) - readdir/import order must not
    // leak into the surfaced order.
    Group* root = db.RootGroup();
    (void)root->CreateGroup(u8"zinc");
    (void)root->CreateGroup(u8"Alpha");
    (void)root->CreateGroup(u8"metals");
    REQUIRE(root->Groups().Size() == 3u);
    CHECK(root->Groups()[0]->Name() == u8"Alpha");
    CHECK(root->Groups()[1]->Name() == u8"metals");
    CHECK(root->Groups()[2]->Name() == u8"zinc");

    Group* metals = root->GetGroup(u8"metals");
    (void)metals->CreateInstance(u8"tin", MaterialResource::StaticType());
    (void)metals->CreateInstance(u8"Bronze", MaterialResource::StaticType());
    foundation::content::Instance* steel =
        metals->CreateInstance(u8"steel", MaterialResource::StaticType());
    REQUIRE(metals->Instances().Size() == 3u);
    CHECK(metals->Instances()[0]->Name() == u8"Bronze");
    CHECK(metals->Instances()[1]->Name() == u8"steel");
    CHECK(metals->Instances()[2]->Name() == u8"tin");

    // A rename re-sorts its siblings.
    REQUIRE(db.RenameInstance(steel->Id(), u8"zeta").IsOk());
    CHECK(metals->Instances()[2]->Name() == u8"zeta");
    Group* alpha = root->GetGroup(u8"Alpha");
    REQUIRE(db.RenameGroup(*alpha, u8"omega").IsOk());
    CHECK(root->Groups()[0]->Name() == u8"metals");
    CHECK(root->Groups()[1]->Name() == u8"omega");
    CHECK(root->Groups()[2]->Name() == u8"zinc");

    RemoveDirectory(JoinPath(dir, u8"metals"));
    RemoveDirectory(dir);
}

// Sedulous 3dae9ac8: an author's edit of a stored envelope reads the way a load reads the stored
// one - the same instance, the same type, a payload this build reads - or is refused with why.
TEST_CASE("content: ReadObjectFrom reads an edited envelope, and refuses another's or a broken one")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();
    const StringView dir = u8"scratch_content_read_from_db";
    RemoveTree(dir);
    FileDelete(JoinPath(dir, u8"materials/copper.xasset"));
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, MakeXmlFactory(), u8".xasset");
    Group* materials = db.RootGroup()->CreateGroup(u8"materials");
    foundation::content::Instance* steel =
        materials->CreateInstance(u8"steel", MaterialResource::StaticType());
    foundation::content::Instance* copper =
        materials->CreateInstance(u8"copper", MaterialResource::StaticType());
    REQUIRE(steel != nullptr);
    REQUIRE(copper != nullptr);
    MaterialResource res;
    res.shininess = 7;
    res.shader = String(u8"pbr");
    REQUIRE(steel->WriteObject(res).IsOk());
    REQUIRE(copper->WriteObject(res).IsOk());

    const auto text = [](foundation::content::Instance& instance)
    {
        UniquePtr<IStream> envelope = instance.OpenEnvelope();
        REQUIRE(envelope);
        Array<byte> bytes;
        bytes.Resize(static_cast<usize>(envelope->Size()));
        REQUIRE(envelope->Read(bytes.Data(), bytes.Size()) == bytes.Size());
        return String(StringView(reinterpret_cast<const utf8char*>(bytes.Data()), bytes.Size()));
    };
    const auto readFrom = [](foundation::content::Instance& instance, StringView xml)
    {
        MemoryStream stream;
        (void)stream.Write(xml.Data(), xml.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        return instance.ReadObjectFrom(stream);
    };
    const String stored = text(*steel);
    REQUIRE(stored.AsView().ContainsIgnoreCase(u8">pbr<"));

    // The edit reads as the object, the stored one untouched until it is written.
    const String edited = ReplaceAll(stored.AsView(), u8">pbr<", u8">unlit<");
    auto read = readFrom(*steel, edited.AsView());
    REQUIRE(read.HasValue());
    auto* object = Cast<MaterialResource>(read.Value().Get());
    REQUIRE(object != nullptr);
    CHECK(object->shader == u8"unlit");
    CHECK(object->shininess == 7);

    // Another asset's envelope, a misspelled key, and text that is not an envelope: refused.
    auto other = readFrom(*copper, stored.AsView());
    REQUIRE_FALSE(other.HasValue());
    CHECK(other.Error().AsView().StartsWith(u8"the envelope's guid"));
    auto misspelled = readFrom(*steel, ReplaceAll(stored.AsView(), u8"shininess", u8"shine").AsView());
    REQUIRE_FALSE(misspelled.HasValue());
    CHECK(misspelled.Error().AsView().StartsWith(u8"the payload did not read"));
    // And says which key, where, and what stood there (Sedulous ac705dee).
    CHECK(misspelled.Error().AsView().ContainsIgnoreCase(
        u8" - no i32 'shininess' at payload (next there: <i32 name=\"shine\">)"));
    auto garbage = readFrom(*steel, u8"<nothing/>");
    REQUIRE_FALSE(garbage.HasValue());

    (void)db.DeleteInstance(copper->Id());
    (void)db.DeleteInstance(steel->Id());
    RemoveTree(dir);
}

// A payload stamped with another data version is in another layout: it is refused, and not
// read as this one (in binary that walked garbage counts past the end of the file).
TEST_CASE("content: a payload of another data version is refused unread")
{
    GlobalTypeRegistry().Register(CountedRecord::StaticType());
    RegisterSerializable<CountedRecord>();
    const StringView dir = u8"scratch_content_version_db";
    FileDelete(JoinPath(dir, u8"records/old.rasset"));
    RemoveDirectory(JoinPath(dir, u8"records"));
    RemoveDirectory(dir);
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, MakeBinaryFactory(), u8".rasset");
    Group* records = db.RootGroup()->CreateGroup(u8"records");
    foundation::content::Instance* old = records->CreateInstance(u8"old", CountedRecord::StaticType());
    REQUIRE(old != nullptr);
    CountedRecord record;
    record.value = 5;
    REQUIRE(old->WriteObject(record).IsOk()); // written at data version 0

    TypeInfo& type = const_cast<TypeInfo&>(CountedRecord::StaticType());
    CountedRecord::reads = 0;
    REQUIRE(old->ReadObject().Get() != nullptr); // the same version reads
    CHECK(CountedRecord::reads == 1);

    type.dataVersion = 1; // the layout moved on
    CountedRecord::reads = 0;
    CHECK(old->ReadObject().Get() == nullptr);
    CHECK(CountedRecord::reads == 0);
    type.dataVersion = 0;

    (void)db.DeleteInstance(old->Id());
    RemoveDirectory(JoinPath(dir, u8"records"));
    RemoveDirectory(dir);
}

// A binary envelope reads field by field; straight off a file each field was a system call.
TEST_CASE("content: an object reads from its file in a few large reads")
{
    GlobalTypeRegistry().Register(WordsRecord::StaticType());
    RegisterSerializable<WordsRecord>();
    const StringView dir = u8"scratch_content_buffered_db";
    FileDelete(JoinPath(dir, u8"words.rasset"));
    RemoveDirectory(dir);
    NativeFileSystem native(dir, DefaultAllocator());
    Guid id;
    {
        ContentDatabase db(DefaultAllocator(), native, MakeBinaryFactory(), u8".rasset");
        foundation::content::Instance* words = db.RootGroup()->CreateInstance(u8"words", WordsRecord::StaticType());
        REQUIRE(words != nullptr);
        id = words->Id();
        WordsRecord record;
        for (i32 i = 0; i < 500; ++i)
        {
            record.words.PushBack(Format(u8"word{}", i));
        }
        REQUIRE(words->WriteObject(record).IsOk());
    }

    CountingMount counting(native);
    ContentDatabase db(DefaultAllocator(), counting, MakeBinaryFactory(), u8".rasset");
    foundation::content::Instance* words = db.GetInstance(id);
    REQUIRE(words != nullptr);
    counting.reads = 0;
    RefPtr<ISerializable> object = words->ReadObject();
    auto* record = Cast<WordsRecord>(object.Get());
    REQUIRE(record != nullptr);
    REQUIRE(record->words.Size() == 500u);
    CHECK(record->words[499] == u8"word499");
    CHECK(counting.reads < 8u); // was one per count, length and character: thousands

    (void)db.DeleteInstance(id);
    RemoveDirectory(dir);
}

TEST_CASE("content: the revision moves with each change to the tree's shape, and not with an instance's content")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();
    const StringView dir = u8"scratch_content_revision_db";
    RemoveTree(dir);
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".xasset");

    u64 seen = db.Revision();
    const auto moved = [&db, &seen]()
    {
        const bool changed = db.Revision() != seen;
        seen = db.Revision();
        return changed;
    };
    Group* materials = db.RootGroup()->CreateGroup(u8"materials");
    CHECK(moved()); // a group added
    foundation::content::Instance* steel = materials->CreateInstance(u8"steel", MaterialResource::StaticType());
    REQUIRE(steel != nullptr);
    CHECK(moved()); // an instance added
    MaterialResource res;
    REQUIRE(steel->WriteObject(res).IsOk());
    CHECK_FALSE(moved()); // its content: not the shape
    REQUIRE(db.RenameInstance(steel->Id(), u8"bronze").IsOk());
    CHECK(moved());
    REQUIRE(db.RenameGroup(*materials, u8"metals").IsOk());
    CHECK(moved());
    REQUIRE(db.DeleteInstance(steel->Id()).IsOk());
    CHECK(moved());
    REQUIRE(db.DeleteGroup(*materials).IsOk());
    CHECK(moved());
    RemoveTree(dir);
}

TEST_CASE("content: MoveInstance moves the envelope and its sidecars to another group's folder, its guid and name kept")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();
    const StringView dir = u8"scratch_content_move_db";
    (void)RemoveDirectoryRecursive(dir); // its two folders whole: a run cut short leaves them
    NativeFileSystem mount(dir, DefaultAllocator());
    ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".xasset");
    Group* materials = db.RootGroup()->CreateGroup(u8"materials");
    Group* metals = db.RootGroup()->CreateGroup(u8"metals"); // no folder yet: the move makes it
    foundation::content::Instance* steel = materials->CreateInstance(u8"steel", MaterialResource::StaticType());
    REQUIRE(steel != nullptr);
    const Guid id = steel->Id();
    MaterialResource res;
    res.shininess = 7;
    REQUIRE(steel->WriteObject(res).IsOk());
    const byte extra[] = {byte{1}, byte{2}};
    REQUIRE(steel->WriteData(u8"extra", Span<const byte>(extra, 2)).IsOk());

    const u64 before = db.Revision();
    REQUIRE(db.MoveInstance(id, *metals).IsOk());
    CHECK(db.Revision() != before);
    CHECK(&steel->OwningGroup() == metals);
    CHECK(materials->GetInstance(u8"steel") == nullptr);
    CHECK(metals->GetInstance(u8"steel") == steel);
    CHECK(db.GetInstance(id) == steel);
    CHECK(steel->Path() == u8"metals/steel");
    CHECK_FALSE(mount.Exists(u8"materials/steel.xasset"));
    CHECK(mount.Exists(u8"metals/steel.xasset"));
    CHECK_FALSE(mount.Exists(u8"materials/steel.extra.bin"));
    CHECK(mount.Exists(u8"metals/steel.extra.bin"));
    {
        RefPtr<ISerializable> object = steel->ReadObject();
        auto* loaded = Cast<MaterialResource>(object.Get());
        REQUIRE(loaded != nullptr);
        CHECK(loaded->shininess == 7);
    }

    // Already there, and a name taken there: refused, nothing moved.
    CHECK(db.MoveInstance(id, *metals).Code() == ErrorCode::InvalidArgument);
    foundation::content::Instance* other = materials->CreateInstance(u8"steel", MaterialResource::StaticType());
    REQUIRE(other != nullptr);
    CHECK(db.MoveInstance(other->Id(), *metals).Code() == ErrorCode::AlreadyExists);
    CHECK(&other->OwningGroup() == materials);
    (void)RemoveDirectoryRecursive(dir);
}
