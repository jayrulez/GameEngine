// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.settings: typed sections round-trip through the serializer abstraction (binary factory
// here; the editor exercises the XML factory). Also covers defaults, change notification, and the
// core UserDataDir / GetEnvironmentVariable helpers the store's storage location builds on.
#include <doctest/doctest.h>
#include <string>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.settings;
import foundation.xml.serialization; // XML factory, to exercise passthrough on both backends

using namespace foundation::core;
namespace settings = foundation::settings;
namespace xml = foundation::xml;

namespace
{
    // A test settings section (v1): plain String fields so it round-trips on any backend.
    class GameSettings : public ISerializable
    {
        RTTI_OBJECT(GameSettings, ISerializable)
    public:
        String profile = String(u8"default");
        String locale = String(u8"en");

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "profile", profile);
            foundation::core::Serialize(ar, "locale", locale);
        }
    };
    RTTI_DEFINE_OBJECT_VERSIONED(GameSettings, "rtti::test", 1)

    // Load needs the type resolvable by name + constructible by id (idempotent to call repeatedly).
    void EnsureRegistered()
    {
        GlobalTypeRegistry().Register(GameSettings::StaticType());
        RegisterSerializable<GameSettings>();
    }
}

TEST_CASE("settings: a fresh section reads its struct defaults")
{
    settings::Settings s(foundation::core::DefaultAllocator());
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
        settings::Settings s(foundation::core::DefaultAllocator());
        GameSettings& g = s.Section<GameSettings>();
        g.profile = String(u8"hardcore");
        g.locale = String(u8"fr");
        REQUIRE(s.Save(stream, BinarySerializerFactory()).IsOk());
    }

    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        settings::Settings s(foundation::core::DefaultAllocator());
        REQUIRE(s.Load(stream, BinarySerializerFactory()).IsOk());
        const GameSettings* g = s.Find<GameSettings>();
        REQUIRE(g != nullptr);
        CHECK(g->profile == u8"hardcore");
        CHECK(g->locale == u8"fr");
    }
}

TEST_CASE("settings: MarkChanged fires OnChanged with the section type name")
{
    settings::Settings s(foundation::core::DefaultAllocator());
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
    CHECK_FALSE(GetEnvironmentVariable(u8"ENV_DEFINITELY_NOT_SET_XYZ_123").HasValue());
    CHECK(GetUserDataDirectory().Size() > 0u);

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
        RTTI_OBJECT(SecA, ISerializable)
    public:
        i32 a = 0;
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "a", a); }
    };
    RTTI_DEFINE_OBJECT_VERSIONED(SecA, "rtti::test", 1)

    class SecX : public ISerializable // the "unknown" one (registered only in the final registry)
    {
        RTTI_OBJECT(SecX, ISerializable)
    public:
        i32 x = 0;
        String tag = String(u8"");
        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "x", x);
            foundation::core::Serialize(ar, "tag", tag);
        }
    };
    RTTI_DEFINE_OBJECT_VERSIONED(SecX, "rtti::test", 1)

    class SecB : public ISerializable
    {
        RTTI_OBJECT(SecB, ISerializable)
    public:
        i32 b = 0;
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "b", b); }
    };
    RTTI_DEFINE_OBJECT_VERSIONED(SecB, "rtti::test", 1)

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
            settings::Settings src(foundation::core::DefaultAllocator());
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

            settings::Settings mid(foundation::core::DefaultAllocator());
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

            settings::Settings dst(foundation::core::DefaultAllocator());
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
        settings::Settings src(foundation::core::DefaultAllocator());
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

        settings::Settings s(foundation::core::DefaultAllocator());
        REQUIRE(stored.Seek(0, SeekOrigin::Begin) == 0);
        REQUIRE(s.Load(stored, BinarySerializerFactory(), types, ser).IsOk());
        CHECK(s.UnknownSectionCount() == 1u);

        MemoryStream next;
        REQUIRE(s.Save(next, BinarySerializerFactory()).IsOk());
        stored = static_cast<MemoryStream&&>(next);
    }
}

// ---- SaveValues: a game's keyed saved values ----

TEST_CASE("settings: save values answer what was set, a fallback otherwise, and report changes")
{
    settings::SaveValues values;
    CHECK(values.SetInt(u8"best.level2", 4210));
    CHECK_FALSE(values.SetInt(u8"best.level2", 4210)); // the same value is no change
    CHECK(values.SetInt(u8"best.level2", 4000));
    CHECK(values.SetFloat(u8"time.level2", 41.5f));
    CHECK(values.SetBool(u8"unlocked.level3", true));
    CHECK(values.SetText(u8"name", u8"Hopper"));
    CHECK(values.SetBool(u8"zero", false)); // a new key is a change even at the default

    CHECK(values.GetInt(u8"best.level2", 0) == 4000);
    CHECK(values.GetFloat(u8"time.level2", 0.0f) == 41.5f);
    CHECK(values.GetBool(u8"unlocked.level3", false));
    CHECK(values.GetText(u8"name", u8"") == u8"Hopper");

    // Absent, or read as another kind: the fallback. An int still reads as a float.
    CHECK(values.GetInt(u8"missing", -1) == -1);
    CHECK(values.GetInt(u8"time.level2", -1) == -1);
    CHECK(values.GetText(u8"best.level2", u8"none") == u8"none");
    CHECK(values.GetFloat(u8"best.level2", 0.0f) == 4000.0f);

    // A key written as another kind takes the new kind.
    CHECK(values.SetText(u8"best.level2", u8"gold"));
    CHECK(values.GetText(u8"best.level2", u8"") == u8"gold");
    CHECK(values.GetInt(u8"best.level2", -1) == -1);

    // Kept in key order, whatever order they were set in.
    REQUIRE(values.Count() == 5u);
    for (usize i = 1; i < values.Count(); ++i)
    {
        CHECK(values.At(i - 1).key.AsView().Compare(values.At(i).key.AsView()) < 0);
    }

    CHECK(values.Remove(u8"name"));
    CHECK_FALSE(values.Remove(u8"name"));
    CHECK_FALSE(values.Has(u8"name"));
    CHECK(values.Clear());
    CHECK_FALSE(values.Clear());
    CHECK(values.Count() == 0u);
}

namespace
{
    void RoundTripSaveValues(SerializerFactory (*make)())
    {
        settings::RegisterSaveValuesType();
        MemoryStream stream;
        {
            settings::Settings store(foundation::core::DefaultAllocator());
            settings::SaveValues& values = store.Section<settings::SaveValues>();
            values.SetInt(u8"coins", 37);
            values.SetFloat(u8"best.time", 62.25f);
            values.SetBool(u8"won", true);
            values.SetText(u8"last.level", u8"Level 3");
            const f32 ghost[] = {1.5f, -2.25f, 0.0f, 1e6f};
            values.SetFloats(u8"ghost", Span<const f32>(ghost, 4));
            REQUIRE(store.Save(stream, make()).IsOk());
        }
        REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
        settings::Settings store(foundation::core::DefaultAllocator());
        REQUIRE(store.Load(stream, make()).IsOk());
        const settings::SaveValues* values = store.Find<settings::SaveValues>();
        REQUIRE(values != nullptr);
        CHECK(values->Count() == 5u);
        CHECK(values->GetInt(u8"coins", 0) == 37);
        CHECK(values->GetFloat(u8"best.time", 0.0f) == 62.25f);
        CHECK(values->GetBool(u8"won", false));
        CHECK(values->GetText(u8"last.level", u8"") == u8"Level 3");
        const Array<f32> ghost = values->GetFloats(u8"ghost");
        REQUIRE(ghost.Size() == 4u);
        CHECK(ghost[0] == 1.5f);
        CHECK(ghost[1] == -2.25f);
        CHECK(ghost[2] == 0.0f);
        CHECK(ghost[3] == 1e6f);
    }
}

TEST_CASE("settings: a list of numbers is a save value of its own kind")
{
    settings::SaveValues values;
    const f32 run[] = {0.0f, 1.0f, 2.5f};
    CHECK(values.SetFloats(u8"ghost", Span<const f32>(run, 3)));
    CHECK_FALSE(values.SetFloats(u8"ghost", Span<const f32>(run, 3))); // the same list is no change
    const f32 other[] = {0.0f, 1.0f, 2.75f};
    CHECK(values.SetFloats(u8"ghost", Span<const f32>(other, 3)));     // one number differs
    CHECK(values.SetFloats(u8"ghost", Span<const f32>(other, 2)));     // a shorter list
    REQUIRE(values.GetFloats(u8"ghost").Size() == 2u);
    CHECK(values.GetFloats(u8"ghost")[1] == 1.0f);

    // Absent, or another kind: an empty list; and the list is not a number to the scalar getters.
    CHECK(values.GetFloats(u8"missing").IsEmpty());
    values.SetInt(u8"best", 3);
    CHECK(values.GetFloats(u8"best").IsEmpty());
    CHECK(values.GetFloat(u8"ghost", -1.0f) == -1.0f);
    // An empty list is a value too.
    CHECK(values.SetFloats(u8"empty", Span<const f32>()));
    CHECK(values.Has(u8"empty"));
}

TEST_CASE("settings: a save value of a kind this build does not know is skipped, the rest read")
{
    settings::RegisterSaveValuesType();
    settings::Settings store(foundation::core::DefaultAllocator());
    store.Section<settings::SaveValues>().SetInt(u8"a.before", 1);
    store.Section<settings::SaveValues>().SetText(u8"m.marker", u8"zzzz");
    store.Section<settings::SaveValues>().SetInt(u8"z.after", 3);
    MemoryStream written;
    REQUIRE(store.Save(written, xml::XmlSerializerFactory()).IsOk());
    std::string text(reinterpret_cast<const char*>(written.Bytes().Data()), written.Bytes().Size());
    // A newer build's kind in place of the text one, with a value this build cannot read as text.
    const usize kind = text.find(">text<");
    REQUIRE(kind != std::string::npos);
    text.replace(kind, 6, ">sound<");

    MemoryStream edited;
    (void)edited.Write(text.data(), text.size());
    REQUIRE(edited.Seek(0, SeekOrigin::Begin) == 0);
    settings::Settings read(foundation::core::DefaultAllocator());
    REQUIRE(read.Load(edited, xml::XmlSerializerFactory()).IsOk());
    const settings::SaveValues* values = read.Find<settings::SaveValues>();
    REQUIRE(values != nullptr);
    CHECK(values->Count() == 2u);
    CHECK(values->GetInt(u8"a.before", 0) == 1);
    CHECK(values->GetInt(u8"z.after", 0) == 3);
    CHECK_FALSE(values->Has(u8"m.marker"));
}

TEST_CASE("settings: save values round-trip as a section (binary)")
{
    RoundTripSaveValues(&BinarySerializerFactory);
}

TEST_CASE("settings: save values round-trip as a section (XML), kinds written by name")
{
    RoundTripSaveValues(&xml::XmlSerializerFactory);

    settings::RegisterSaveValuesType();
    settings::Settings store(foundation::core::DefaultAllocator());
    store.Section<settings::SaveValues>().SetFloat(u8"best.time", 1.5f);
    MemoryStream stream;
    REQUIRE(store.Save(stream, xml::XmlSerializerFactory()).IsOk());
    const std::string text(reinterpret_cast<const char*>(stream.Bytes().Data()), stream.Bytes().Size());
    CHECK(text.find(">float<") != std::string::npos);
    const f32 ghost[] = {2.0f};
    store.Section<settings::SaveValues>().SetFloats(u8"ghost", Span<const f32>(ghost, 1));
    MemoryStream again;
    REQUIRE(store.Save(again, xml::XmlSerializerFactory()).IsOk());
    const std::string listed(reinterpret_cast<const char*>(again.Bytes().Data()), again.Bytes().Size());
    CHECK(listed.find(">floats<") != std::string::npos);
    CHECK(text.find("best.time") != std::string::npos);
}
