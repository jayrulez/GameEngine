// draconic.settings: typed sections round-trip through the serializer abstraction (binary factory
// here; the editor exercises the XML factory). Also covers defaults, change notification, and the
// core UserDataDir / GetEnvironmentVariable helpers the store's storage location builds on.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.settings;
import draconic.xml.serialization; // XML factory, to exercise passthrough on both backends

using namespace draconic::foundation;
namespace settings = draconic::settings;
namespace xml = draconic::xml;

namespace
{
    // A test settings section (v1): plain String fields so it round-trips on any backend.
    class GameSettings : public ISerializable
    {
        DRACONIC_OBJECT(GameSettings, ISerializable)
    public:
        String profile = String(u8"default");
        String locale = String(u8"en");

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "profile", profile);
            draconic::foundation::Serialize(ar, "locale", locale);
        }
    };
    DRACONIC_DEFINE_OBJECT_VERSIONED(GameSettings, "draconic::test", 1)

    // Load needs the type resolvable by name + constructible by id (idempotent to call repeatedly).
    void EnsureRegistered()
    {
        GlobalTypeRegistry().Register(GameSettings::StaticType());
        RegisterSerializable<GameSettings>();
    }
}

TEST_CASE("settings: a fresh section reads its struct defaults")
{
    settings::Settings s;
    CHECK(s.SectionCount() == 0u);
    CHECK(s.Find<GameSettings>() == nullptr);

    GameSettings& g = s.Section<GameSettings>(); // lazily created
    CHECK(g.profile == u8"default");
    CHECK(g.locale == u8"en");
    CHECK(s.SectionCount() == 1u);
    CHECK(s.Find<GameSettings>() != nullptr);
}

TEST_CASE("settings: sections round-trip through the (binary) serializer factory")
{
    EnsureRegistered();

    MemoryStream stream;
    {
        settings::Settings s;
        GameSettings& g = s.Section<GameSettings>();
        g.profile = String(u8"hardcore");
        g.locale = String(u8"fr");
        REQUIRE(s.Save(stream, BinarySerializerFactory()).IsOk());
    }

    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        settings::Settings s;
        REQUIRE(s.Load(stream, BinarySerializerFactory()).IsOk());
        const GameSettings* g = s.Find<GameSettings>();
        REQUIRE(g != nullptr);
        CHECK(g->profile == u8"hardcore");
        CHECK(g->locale == u8"fr");
    }
}

TEST_CASE("settings: MarkChanged fires OnChanged with the section type name")
{
    settings::Settings s;
    String changed;
    s.OnChanged([&](StringView name) { changed = String(name); });

    s.Section<GameSettings>().profile = String(u8"x");
    s.MarkChanged<GameSettings>();
    CHECK(changed == u8"GameSettings");
}

TEST_CASE("core/system: GetEnvironmentVariable + UserDataDir")
{
    // PATH is defined on every platform we target; a bogus name is absent.
    CHECK(GetEnvironmentVariable(u8"PATH").HasValue());
    CHECK_FALSE(GetEnvironmentVariable(u8"DRACONIC_DEFINITELY_NOT_SET_XYZ_123").HasValue());
    CHECK(GetUserDataDirectory(u8"draconic").Size() > 0u);

    // The running test executable resolves, and its directory is a prefix of the full path.
    const String exePath = GetExecutablePath();
    const String exeDir = GetExecutableDirectory();
    CHECK(exePath.Size() > 0u);
    CHECK(exeDir.Size() > 0u);
    CHECK(exeDir.Size() < exePath.Size());
    CHECK(exePath.AsView().StartsWith(exeDir.AsView()));
}

// --- Unknown-section passthrough (both backends): a section whose type a build cannot instantiate is
//     preserved verbatim across Load/Save instead of aborting the store (the 2026-08-01 data loss). ---
namespace
{
    class SecA : public ISerializable
    {
        DRACONIC_OBJECT(SecA, ISerializable)
    public:
        i32 a = 0;
        void Serialize(ISerializer& ar) override { draconic::foundation::Serialize(ar, "a", a); }
    };
    DRACONIC_DEFINE_OBJECT_VERSIONED(SecA, "draconic::test", 1)

    class SecX : public ISerializable // the "unknown" one (registered only in the final registry)
    {
        DRACONIC_OBJECT(SecX, ISerializable)
    public:
        i32 x = 0;
        String tag = String(u8"");
        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "x", x);
            draconic::foundation::Serialize(ar, "tag", tag);
        }
    };
    DRACONIC_DEFINE_OBJECT_VERSIONED(SecX, "draconic::test", 1)

    class SecB : public ISerializable
    {
        DRACONIC_OBJECT(SecB, ISerializable)
    public:
        i32 b = 0;
        void Serialize(ISerializer& ar) override { draconic::foundation::Serialize(ar, "b", b); }
    };
    DRACONIC_DEFINE_OBJECT_VERSIONED(SecB, "draconic::test", 1)

    void RegisterAB(TypeRegistry& types, SerializableRegistry& ser)
    {
        types.Register(SecA::StaticType());
        RegisterSerializable<SecA>(ser);
        types.Register(SecB::StaticType());
        RegisterSerializable<SecB>(ser);
    }

    // Author {A, X, B} -> load with only A+B registered (X unknown, preserved) -> re-save -> load with
    // X now registered and confirm its data survived the round-trip through a build that did not know it.
    // `make` yields a FRESH factory per call (SerializerFactory is a move-only Function).
    void RunPassthrough(SerializerFactory (*make)())
    {
        MemoryStream stored;
        {
            settings::Settings src;
            src.Section<SecA>().a = 10;
            src.Section<SecX>().x = 42;
            src.Section<SecX>().tag = String(u8"keepme");
            src.Section<SecB>().b = 20;
            REQUIRE(src.Save(stored, make()).IsOk());
        }

        MemoryStream resaved;
        {
            TypeRegistry types;
            SerializableRegistry ser;
            RegisterAB(types, ser); // X is NOT registered here

            settings::Settings mid;
            REQUIRE(stored.Seek(0, SeekOrigin::Begin) == 0);
            REQUIRE(mid.Load(stored, make(), types, ser).IsOk()); // unknown X must NOT abort the store
            CHECK(mid.SectionCount() == 2u);        // A + B loaded
            CHECK(mid.UnknownSectionCount() == 1u); // X preserved verbatim
            REQUIRE(mid.Find<SecA>() != nullptr);
            CHECK(mid.Find<SecA>()->a == 10);
            REQUIRE(mid.Find<SecB>() != nullptr);
            CHECK(mid.Find<SecB>()->b == 20); // the regression: B is not dropped after the unknown X

            REQUIRE(mid.Save(resaved, make()).IsOk()); // re-emits A, B, and X
        }

        {
            TypeRegistry types;
            SerializableRegistry ser;
            RegisterAB(types, ser);
            types.Register(SecX::StaticType()); // now X is known
            RegisterSerializable<SecX>(ser);

            settings::Settings dst;
            REQUIRE(resaved.Seek(0, SeekOrigin::Begin) == 0);
            REQUIRE(dst.Load(resaved, make(), types, ser).IsOk());
            CHECK(dst.UnknownSectionCount() == 0u); // all resolved now
            REQUIRE(dst.Find<SecX>() != nullptr);
            CHECK(dst.Find<SecX>()->x == 42);
            CHECK(dst.Find<SecX>()->tag == u8"keepme"); // preserved-unknown data intact
            REQUIRE(dst.Find<SecA>() != nullptr);
            CHECK(dst.Find<SecA>()->a == 10);
            REQUIRE(dst.Find<SecB>() != nullptr);
            CHECK(dst.Find<SecB>()->b == 20);
        }
    }
}

TEST_CASE("settings: unknown-section passthrough round-trips (binary)")
{
    RunPassthrough(&BinarySerializerFactory);
}

TEST_CASE("settings: unknown-section passthrough round-trips (XML)")
{
    RunPassthrough(&xml::XmlSerializerFactory);
}

TEST_CASE("settings: repeated Load/Save does not duplicate a preserved unknown section")
{
    MemoryStream stored;
    {
        settings::Settings src;
        src.Section<SecA>().a = 1;
        src.Section<SecX>().x = 7;
        REQUIRE(src.Save(stored, BinarySerializerFactory()).IsOk());
    }
    // Cycle Load/Save three times with X unknown; the unknown count stays 1 (no growth/dup).
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        TypeRegistry types;
        SerializableRegistry ser;
        types.Register(SecA::StaticType());
        RegisterSerializable<SecA>(ser);

        settings::Settings s;
        REQUIRE(stored.Seek(0, SeekOrigin::Begin) == 0);
        REQUIRE(s.Load(stored, BinarySerializerFactory(), types, ser).IsOk());
        CHECK(s.UnknownSectionCount() == 1u);

        MemoryStream next;
        REQUIRE(s.Save(next, BinarySerializerFactory()).IsOk());
        stored = static_cast<MemoryStream&&>(next);
    }
}
