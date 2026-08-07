#include <doctest/doctest.h>

#include <cstring>

#include "Draconic.Foundation/Debug/Assert.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;

using namespace draconic::foundation;

// --- IO: MemoryStream ------------------------------------------------------

TEST_CASE("io: MemoryStream write/read round-trip")
{
    MemoryStream stream;
    CHECK(stream.IsValid());
    CHECK(stream.Size() == 0);

    CHECK(stream.WriteValue<i32>(0x11223344));
    CHECK(stream.WriteValue<f32>(2.5f));
    const char text[] = "hi";
    CHECK(stream.Write(text, 2) == 2u);

    CHECK(stream.Size() == static_cast<i64>(sizeof(i32) + sizeof(f32) + 2));
    CHECK(stream.Tell() == stream.Size());

    // Rewind and read back.
    CHECK(stream.Seek(0, SeekOrigin::Begin) == 0);
    i32 a = 0;
    f32 b = 0.0f;
    char c[2] = {};
    CHECK(stream.ReadValue(a));
    CHECK(stream.ReadValue(b));
    CHECK(stream.Read(c, 2) == 2u);

    CHECK(a == 0x11223344);
    CHECK(b == 2.5f);
    CHECK(c[0] == 'h');
    CHECK(c[1] == 'i');

    // Reading past the end returns a short count.
    u8 extra = 0;
    CHECK(stream.Read(&extra, 1) == 0u);
}

TEST_CASE("io: MemoryStream seek bounds and overwrite")
{
    MemoryStream stream;
    CHECK(stream.WriteValue<i32>(10));
    CHECK(stream.WriteValue<i32>(20));

    // Seek out of range fails.
    CHECK(stream.Seek(-1, SeekOrigin::Begin) == -1);
    CHECK(stream.Seek(100, SeekOrigin::Begin) == -1);

    // Overwrite the first value in place.
    CHECK(stream.Seek(0, SeekOrigin::Begin) == 0);
    CHECK(stream.WriteValue<i32>(99));
    CHECK(stream.Size() == 8); // no growth

    CHECK(stream.Seek(0, SeekOrigin::Begin) == 0);
    i32 first = 0;
    i32 second = 0;
    CHECK(stream.ReadValue(first));
    CHECK(stream.ReadValue(second));
    CHECK(first == 99);
    CHECK(second == 20);
}

// --- IO: FileStream --------------------------------------------------------

TEST_CASE("io: FileStream writes then reads a file")
{
    const StringView path = u8"draconic_io_stream_test.tmp";

    {
        FileStream out(path, FileMode::Write);
        REQUIRE(out.IsValid());
        CHECK(out.WriteValue<u32>(0xDEADBEEF));
        CHECK(out.WriteValue<f64>(3.25));
    }

    {
        FileStream in(path, FileMode::Read);
        REQUIRE(in.IsValid());
        CHECK(in.Size() == static_cast<i64>(sizeof(u32) + sizeof(f64)));

        u32 magic = 0;
        f64 value = 0.0;
        CHECK(in.ReadValue(magic));
        CHECK(in.ReadValue(value));
        CHECK(magic == 0xDEADBEEFu);
        CHECK(value == 3.25);

        // Seek back to the float and re-read.
        CHECK(in.Seek(static_cast<i64>(sizeof(u32)), SeekOrigin::Begin) ==
              static_cast<i64>(sizeof(u32)));
        f64 again = 0.0;
        CHECK(in.ReadValue(again));
        CHECK(again == 3.25);
    }

    CHECK(FileDelete(path));
}

TEST_CASE("io: FileStream on an unopenable path is invalid")
{
    FileStream in(u8"draconic_io_missing_file.xyz", FileMode::Read);
    CHECK_FALSE(in.IsValid());
    u8 byte = 0;
    CHECK(in.Read(&byte, 1) == 0u);
}

// --- Serialization ---------------------------------------------------------

namespace
{
    // A type that describes its data once, used for both save and load.
    struct Particle
    {
        i32 id = 0;
        Float3 position;
        f32 mass = 0.0f;
    };

    void Serialize(ISerializer& ar, Particle& p)
    {
        Serialize(ar, p.id);
        Serialize(ar, p.position);
        Serialize(ar, p.mass);
    }
}

TEST_CASE("serialization: primitives and math round-trip")
{
    MemoryStream stream;
    {
        BinarySerializer saver(stream, SerializeMode::Write);
        CHECK(saver.IsWriting());

        i32 a = -7;
        f64 b = 1.5;
        Float3 v{1.0f, 2.0f, 3.0f};
        Serialize(saver, a);
        Serialize(saver, b);
        Serialize(saver, v);
        CHECK(saver.IsOk());
    }

    CHECK(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer loader(stream, SerializeMode::Read);
        CHECK(loader.IsReading());

        i32 a = 0;
        f64 b = 0.0;
        Float3 v;
        Serialize(loader, a);
        Serialize(loader, b);
        Serialize(loader, v);
        CHECK(loader.IsOk());

        CHECK(a == -7);
        CHECK(b == 1.5);
        CHECK(v == Float3{1.0f, 2.0f, 3.0f});
    }
}

TEST_CASE("serialization: String and Array round-trip")
{
    MemoryStream stream;
    {
        BinarySerializer saver(stream, SerializeMode::Write);
        String name = u8"draconic";
        Array<i32> values;
        for (i32 i = 0; i < 5; ++i)
        {
            values.PushBack(i * 11);
        }
        Serialize(saver, name);
        Serialize(saver, values);
        CHECK(saver.IsOk());
    }

    CHECK(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer loader(stream, SerializeMode::Read);
        String name;
        Array<i32> values;
        Serialize(loader, name);
        Serialize(loader, values);
        CHECK(loader.IsOk());

        CHECK(name == u8"draconic");
        REQUIRE(values.Size() == 5u);
        CHECK(values[0] == 0);
        CHECK(values[4] == 44);
    }
}

TEST_CASE("serialization: a user type serialized once for both directions")
{
    MemoryStream stream;

    Particle original;
    original.id = 99;
    original.position = Float3{4.0f, 5.0f, 6.0f};
    original.mass = 2.25f;

    {
        BinarySerializer saver(stream, SerializeMode::Write);
        Serialize(saver, original);
        CHECK(saver.IsOk());
    }

    CHECK(stream.Seek(0, SeekOrigin::Begin) == 0);

    Particle loaded;
    {
        BinarySerializer loader(stream, SerializeMode::Read);
        Serialize(loader, loaded);
        CHECK(loader.IsOk());
    }

    CHECK(loaded.id == 99);
    CHECK(loaded.position == Float3{4.0f, 5.0f, 6.0f});
    CHECK(loaded.mass == 2.25f);
}

TEST_CASE("serialization: nested Array<String> round-trips")
{
    MemoryStream stream;
    {
        BinarySerializer saver(stream, SerializeMode::Write);
        Array<String> words;
        words.PushBack(String(u8"alpha"));
        words.PushBack(String(u8"beta"));
        words.PushBack(String(u8"gamma"));
        Serialize(saver, words);
        CHECK(saver.IsOk());
    }

    CHECK(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer loader(stream, SerializeMode::Read);
        Array<String> words;
        Serialize(loader, words);
        CHECK(loader.IsOk());

        REQUIRE(words.Size() == 3u);
        CHECK(words[0] == u8"alpha");
        CHECK(words[2] == u8"gamma");
    }
}

TEST_CASE("serialization: short read on load is reported")
{
    MemoryStream stream;
    {
        BinarySerializer saver(stream, SerializeMode::Write);
        i16 small = 7;
        Serialize(saver, small); // only 2 bytes written
    }
    CHECK(stream.Seek(0, SeekOrigin::Begin) == 0);

    BinarySerializer loader(stream, SerializeMode::Read);
    i64 tooBig = 0; // wants 8 bytes
    Serialize(loader, tooBig);
    CHECK_FALSE(loader.IsOk()); // ran out of bytes
    CHECK(loader.GetStatus().Code() == ErrorCode::Internal);
}

// --- IO: BufferedStream ----------------------------------------------------

TEST_CASE("io: BufferedStream write then read round-trip (small buffer)")
{
    MemoryStream backing;
    {
        BufferedStream buffered(backing, 8); // tiny buffer forces many flushes
        for (i32 i = 0; i < 100; ++i)
        {
            CHECK(buffered.WriteValue<i32>(i));
        }
        buffered.Flush();
    }

    // Verify the bytes actually reached the backing store.
    CHECK(backing.Size() == static_cast<i64>(100 * sizeof(i32)));
    CHECK(backing.Seek(0, SeekOrigin::Begin) == 0);
    for (i32 i = 0; i < 100; ++i)
    {
        i32 v = -1;
        CHECK(backing.ReadValue(v));
        CHECK(v == i);
    }
}

TEST_CASE("io: BufferedStream read refills across the buffer boundary")
{
    MemoryStream backing;
    for (i32 i = 0; i < 50; ++i)
    {
        CHECK(backing.WriteValue<i32>(i * 2));
    }
    CHECK(backing.Seek(0, SeekOrigin::Begin) == 0);

    BufferedStream buffered(backing, 8);
    for (i32 i = 0; i < 50; ++i)
    {
        i32 v = -1;
        CHECK(buffered.ReadValue(v));
        CHECK(v == i * 2);
    }
    i32 extra = 0;
    CHECK(buffered.Read(&extra, sizeof(extra)) == 0u); // EOF
}

TEST_CASE("io: BufferedStream write-then-seek-then-read on one stream")
{
    MemoryStream backing;
    BufferedStream buffered(backing, 16);

    for (i32 i = 0; i < 20; ++i)
    {
        CHECK(buffered.WriteValue<i32>(i + 100));
    }

    // Seek flushes pending writes, then we read back from the start.
    CHECK(buffered.Seek(0, SeekOrigin::Begin) == 0);
    for (i32 i = 0; i < 20; ++i)
    {
        i32 v = -1;
        CHECK(buffered.ReadValue(v));
        CHECK(v == i + 100);
    }
}

// --- IO: Path --------------------------------------------------------------

TEST_CASE("io: Path query functions")
{
    CHECK(PathFilename(u8"/a/b/c.txt") == StringView(u8"c.txt"));
    CHECK(PathFilename(u8"noslash.dat") == StringView(u8"noslash.dat"));
    CHECK(PathExtension(u8"/a/b/c.txt") == StringView(u8".txt"));
    CHECK(PathExtension(u8"/a/b/c") == StringView(u8""));
    CHECK(PathExtension(u8"/a/.hidden") == StringView(u8"")); // dotfile has no ext
    CHECK(PathStem(u8"/a/b/c.txt") == StringView(u8"c"));
    CHECK(PathParent(u8"/a/b/c.txt") == StringView(u8"/a/b"));
    CHECK(PathParent(u8"file") == StringView(u8""));

    CHECK(PathIsAbsolute(u8"/etc/hosts"));
    CHECK_FALSE(PathIsAbsolute(u8"relative/path"));
}

TEST_CASE("io: PathJoin")
{
    CHECK(PathJoin(u8"/a/b", u8"c.txt") == u8"/a/b/c.txt");
    CHECK(PathJoin(u8"/a/b/", u8"c.txt") == u8"/a/b/c.txt"); // no double separator
    CHECK(PathJoin(u8"", u8"c.txt") == u8"c.txt");
    CHECK(PathJoin(u8"/a/b", u8"/absolute") == u8"/absolute"); // absolute rhs wins
}

// --- IO: FileSystem --------------------------------------------------------

TEST_CASE("io: ReadFile / WriteFile round-trip")
{
    const StringView path = u8"draconic_fs_roundtrip.tmp";
    const byte payload[] = {byte{1}, byte{2}, byte{3}, byte{0xFF}, byte{0}, byte{42}};

    CHECK(WriteFile(path, Span<const byte>{payload, ArrayCount(payload)}).IsOk());

    Result<Array<byte>> result = ReadFile(path);
    REQUIRE(result.HasValue());
    const Array<byte>& bytes = result.Value();
    REQUIRE(bytes.Size() == ArrayCount(payload));
    for (usize i = 0; i < bytes.Size(); ++i)
    {
        CHECK(bytes[i] == payload[i]);
    }

    CHECK(FileDelete(path));
    CHECK(ReadFile(u8"draconic_fs_missing.tmp").Error() == ErrorCode::NotFound);
}

TEST_CASE("io: directory create / exists / remove")
{
    const StringView dir = u8"draconic_fs_test_dir";
    CHECK_FALSE(DirectoryExists(dir));
    CHECK(CreateDirectory(dir));
    CHECK(DirectoryExists(dir));
    CHECK(CreateDirectory(dir)); // idempotent
    CHECK(RemoveDirectory(dir));
    CHECK_FALSE(DirectoryExists(dir));
}

// --- IO: Virtual file system -----------------------------------------------

// VFS (NativeFileSystem / VirtualFileSystem) moved to draconic.vfs - see
// Code/Draconic/VFS/Tests/VfsTests.cpp.

// --- Serialization: ISerializable ------------------------------------------

namespace
{
    // A serializable object: implements Serialize() once for both directions.
    class Widget final : public ISerializable
    {
        DRACONIC_OBJECT(Widget, ISerializable)
    public:
        i32 id = 0;
        String label;
        Float3 position;

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "id", id);
            draconic::foundation::Serialize(ar, "label", label);
            draconic::foundation::Serialize(ar, "position", position);
        }
    };
}

DRACONIC_DEFINE_OBJECT(Widget, "draconic::test")

TEST_CASE("serialization: ISerializable round-trips through BinarySerializer")
{
    // It is an Object, so it carries reflected type identity.
    CHECK(Widget::StaticType().base == &ISerializable::StaticType());
    CHECK(IsDerivedFrom(&Widget::StaticType(), &ISerializable::StaticType()));

    MemoryStream stream;
    {
        Widget w;
        w.id = 42;
        w.label = u8"hello";
        w.position = Float3{1.0f, 2.0f, 3.0f};
        BinarySerializer saver(stream, SerializeMode::Write);
        Serialize(saver, w); // free fn dispatches to w.Serialize(ar)
        CHECK(saver.IsOk());
    }

    CHECK(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        Widget w;
        BinarySerializer loader(stream, SerializeMode::Read);
        ISerializable& asBase = w; // through the base reference
        Serialize(loader, asBase);
        CHECK(loader.IsOk());

        CHECK(w.id == 42);
        CHECK(w.label == u8"hello");
        CHECK(w.position == Float3{1.0f, 2.0f, 3.0f});
    }
}

TEST_CASE("serialization: SerializableRegistry creates by type id, then deserializes")
{
    SerializableRegistry registry;
    RegisterSerializable<Widget>(registry);
    CHECK(registry.Contains(Widget::StaticType().id));
    CHECK_FALSE(registry.Contains(ISerializable::StaticType().id));

    MemoryStream stream;
    {
        Widget w;
        w.id = 7;
        w.label = u8"reg";
        w.position = Float3{9, 8, 7};
        BinarySerializer saver(stream, SerializeMode::Write);
        Serialize(saver, w);
    }
    CHECK(stream.Seek(0, SeekOrigin::Begin) == 0);

    // Polymorphic rebuild: create the concrete type from its id, deserialize via base.
    RefPtr<ISerializable> obj = registry.Create(Widget::StaticType().id);
    REQUIRE(obj.Get() != nullptr);
    BinarySerializer loader(stream, SerializeMode::Read);
    obj->Serialize(loader);
    CHECK(loader.IsOk());

    Widget* w = Cast<Widget>(obj.Get());
    REQUIRE(w != nullptr); // GetType() reports the concrete type
    CHECK(w->id == 7);
    CHECK(w->label == u8"reg");

    CHECK(registry.Create(ISerializable::StaticType().id).Get() == nullptr);
}
