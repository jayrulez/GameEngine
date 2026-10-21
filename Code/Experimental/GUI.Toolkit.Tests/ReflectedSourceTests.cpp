// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - the reflected property source (T3 of Specs/gui-lab-evolution-3.md): a reflected type's
// properties as rows (values, enums and flags, Float2/3/4, a Quaternion as Euler degrees, value
// structs as groups, other types hinted with their names), the attributes the editor's inspector
// reads, writes through addresses and setters, and each commit told as one change the text format
// writes and Restore reads back.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import experimental.gui;
import experimental.gui.toolkit;

using namespace experimental::gui;
namespace core = foundation::core;
using core::f32;
using core::f64;
using core::i32;
using core::i64;
using core::u8;
using core::u32;
using core::usize;

namespace reflected
{
    enum class LampKind : u8
    {
        Point,
        Spot,
        Area,
    };
    enum class LampLayers : u32
    {
        None = 0,
        Main = 1,
        Shadows = 2,
        Reflections = 4,
    };
    struct LampShape
    {
        f32 radius = 1.0f;
        f32 softness = 0.5f;
    };

    class TestLamp : public core::Object
    {
        RTTI_OBJECT(TestLamp, core::Object)
    public:
        core::String name = core::String(u8"Lamp");
        bool enabled = true;
        u8 priority = 3;
        i32 bounces = 2;
        f32 intensity = 5.0f;
        f64 range = 10.0;
        LampKind kind = LampKind::Point;
        LampLayers layers = LampLayers::Main;
        f32 coneAngle = 45.0f;
        core::Float2 cone{20.0f, 35.0f};
        core::Float3 position{1.0f, 2.0f, 3.0f};
        core::Quaternion rotation = core::Quaternion::Identity;
        LampShape shape;
        core::Color tint{1.0f, 1.0f, 1.0f, 1.0f};
        i64 id = 101;

        [[nodiscard]] f32 GetExposure() const { return m_exposure; }
        void SetExposure(f32 exposure) { m_exposure = exposure; }

    private:
        f32 m_exposure = 0.0f;
    };

    REFLECT_ENUM(LampKind, "rtti::tests::reflected")
    {
        builder.Value("Point", LampKind::Point);
        builder.Value("Spot", LampKind::Spot);
        builder.Value("Area", LampKind::Area);
    }
    REFLECT_ENUM(LampLayers, "rtti::tests::reflected")
    {
        builder.Value("Main", LampLayers::Main);
        builder.Value("Shadows", LampLayers::Shadows);
        builder.Value("Reflections", LampLayers::Reflections);
    }
    REFLECT_VALUE(LampShape, "rtti::tests::reflected")
    {
        builder.Property<&LampShape::radius>("radius").Property<&LampShape::softness>("softness");
    }
    REFLECT_MEMBERS(TestLamp, "rtti::tests::reflected")
    {
        builder.Property<&TestLamp::name>("name").PropAttribute("description", core::String(u8"What the scene calls it"));
        builder.Property<&TestLamp::enabled>("enabled");
        builder.Property<&TestLamp::priority>("priority");
        builder.Property<&TestLamp::bounces>("bounces");
        builder.Property<&TestLamp::intensity>("intensity").PropAttribute("range", core::Float4{0.0f, 100.0f, 0.5f, 0.0f});
        builder.Property<&TestLamp::range>("range").PropAttribute("displayName", core::String(u8"Reach"));
        builder.Property<&TestLamp::kind>("kind");
        builder.Property<&TestLamp::layers>("layers").PropAttribute("flags", true);
        builder.Property<&TestLamp::coneAngle>("coneAngle").PropAttribute("visibleWhen", core::String(u8"kind=1"));
        builder.Property<&TestLamp::cone>("cone");
        builder.Property<&TestLamp::position>("position");
        builder.Property<&TestLamp::rotation>("rotation");
        builder.Property<&TestLamp::shape>("shape");
        builder.Property<&TestLamp::tint>("tint");
        builder.Property<&TestLamp::id>("id", core::PropertyFlags::ReadOnly);
        builder.AccessorProperty<&TestLamp::GetExposure, &TestLamp::SetExposure>("exposure");
        builder.Creator();
    }

    void Register()
    {
        core::RegisterCoreTypes(); // as an application does: Color, Float3 and the rest named
        RttiRegisterEnum_LampKind();
        RttiRegisterEnum_LampLayers();
        RttiRegisterValue_LampShape();
    }

    // A format of its own for the tests: Core's binary serializer, its bytes held as the text.
    class BlobFormat : public IPropertyTextFormat
    {
    public:
        core::String Write(core::Function<void(core::ISerializer&)> write) override
        {
            core::MemoryStream stream;
            core::BinarySerializer ar(stream, core::SerializeMode::Write);
            write(ar);
            const core::Span<const core::byte> bytes = stream.Bytes();
            return ar.IsOk() ? core::String(core::StringView(reinterpret_cast<const char8_t*>(bytes.Data()), bytes.Size())) : core::String{};
        }
        bool Read(core::StringView text, core::Function<void(core::ISerializer&)> read) override
        {
            core::MemoryStream stream;
            (void)stream.Write(text.Data(), text.Size());
            (void)stream.Seek(0, core::SeekOrigin::Begin);
            core::BinarySerializer ar(stream, core::SerializeMode::Read);
            read(ar);
            return ar.IsOk();
        }
    };
}

namespace
{
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    struct Bed
    {
        core::RefPtr<reflected::TestLamp> a = core::MakeRef<reflected::TestLamp>(core::DefaultAllocator());
        core::RefPtr<reflected::TestLamp> b = core::MakeRef<reflected::TestLamp>(core::DefaultAllocator());
        ReflectedPropertySource source{core::DefaultAllocator()};
        core::Array<ReflectedChange> changes{core::DefaultAllocator()};

        explicit Bed(bool both = false)
        {
            reflected::Register();
            core::Instance objects[] = {core::Instance(a.Get(), a->GetType()), core::Instance(b.Get(), b->GetType())}; // an Object's own type
            source.SetObjects(core::Span<const core::Instance>(objects, both ? 2u : 1u));
            source.SetOnChange([this](const ReflectedChange& change) { changes.PushBack(change); });
        }
        const PropertyInfo* Find(const core::Array<PropertyInfo>& infos, const char8_t* name)
        {
            for (const PropertyInfo& info : infos)
                if (info.Name == core::String(name))
                    return &info;
            return nullptr;
        }
        void Commit(const char8_t* path, const Variant& value)
        {
            source.BeginEdit(SV(path));
            source.CommitEdit(SV(path), PropertyWrite{value});
        }
    };
}

TEST_CASE("reflected source: a type's properties as rows, each of its kind, with the attributes the inspector reads")
{
    Bed bed;
    core::Array<PropertyInfo> rows(core::DefaultAllocator());
    bed.source.GetProperties(SV(u8""), rows);
    REQUIRE(rows.Size() == 16u);
    CHECK(rows[0].Name == core::String(u8"name"));
    CHECK(rows[0].DisplayName == core::String(u8"Name"));
    CHECK(rows[0].Description == core::String(u8"What the scene calls it"));
    CHECK(rows[0].Kind == PropertyKind::String);
    CHECK(bed.Find(rows, u8"enabled")->Kind == PropertyKind::Bool);
    CHECK(bed.Find(rows, u8"priority")->Kind == PropertyKind::Integer); // a u8
    const PropertyInfo& intensity = *bed.Find(rows, u8"intensity");
    CHECK(intensity.Kind == PropertyKind::Float);
    CHECK(intensity.HasRange);
    CHECK(intensity.Max == doctest::Approx(100.0));
    CHECK(intensity.Step == doctest::Approx(0.5));
    CHECK(bed.Find(rows, u8"range")->DisplayName == core::String(u8"Reach"));
    const PropertyInfo& kind = *bed.Find(rows, u8"kind");
    CHECK(kind.Kind == PropertyKind::Enum);
    REQUIRE(kind.Choices.Size() == 3u);
    CHECK(kind.Choices[1].Name == core::String(u8"Spot"));
    CHECK(bed.Find(rows, u8"layers")->Kind == PropertyKind::Flags);
    CHECK(bed.Find(rows, u8"coneAngle")->DisplayName == core::String(u8"Cone Angle"));
    CHECK_FALSE(bed.Find(rows, u8"coneAngle")->Visible); // a point light: visibleWhen kind=1
    CHECK(bed.Find(rows, u8"cone")->Hint == core::String(u8"Float2"));
    CHECK(bed.Find(rows, u8"position")->Hint == core::String(u8"Float3"));
    CHECK(bed.Find(rows, u8"rotation")->Hint == core::String(u8"Float3")); // Euler degrees
    CHECK(bed.Find(rows, u8"shape")->Kind == PropertyKind::Group);
    CHECK(bed.Find(rows, u8"shape")->Hint == core::String(u8"LampShape")); // no editor takes it: its rows
    CHECK(bed.Find(rows, u8"tint")->Hint == core::String(u8"Color")); // for T4, or an application's editor
    CHECK(bed.Find(rows, u8"id")->ReadOnly);
    CHECK(bed.Find(rows, u8"exposure")->Kind == PropertyKind::Float); // a setter's

    core::Array<PropertyInfo> parts(core::DefaultAllocator());
    bed.source.GetProperties(SV(u8"rotation"), parts);
    REQUIRE(parts.Size() == 3u);
    CHECK(parts[0].Name == core::String(u8"pitch"));
    parts.Clear();
    bed.source.GetProperties(SV(u8"shape"), parts);
    REQUIRE(parts.Size() == 2u);
    CHECK(parts[1].DisplayName == core::String(u8"Softness"));

    bed.a->kind = reflected::LampKind::Spot;
    rows.Clear();
    bed.source.GetProperties(SV(u8""), rows);
    CHECK(bed.Find(rows, u8"coneAngle")->Visible);
}

TEST_CASE("reflected source: values read and written through addresses and setters, parts and Euler degrees")
{
    Bed bed;
    CHECK(bed.source.GetValue(SV(u8"name"), 0).AsString() == SV(u8"Lamp"));
    CHECK(bed.source.GetValue(SV(u8"priority"), 0).AsInt() == 3);
    CHECK(bed.source.GetValue(SV(u8"position.y"), 0).AsFloat() == doctest::Approx(2.0));
    CHECK(bed.source.GetValue(SV(u8"shape.softness"), 0).AsFloat() == doctest::Approx(0.5));
    CHECK(bed.source.GetValue(SV(u8"kind"), 0).AsInt() == 0);

    bed.Commit(u8"priority", Variant(i64{7}));
    CHECK(bed.a->priority == 7);
    bed.Commit(u8"range", Variant(2.5));
    CHECK(bed.a->range == doctest::Approx(2.5));
    bed.Commit(u8"kind", Variant(i64{2}));
    CHECK(bed.a->kind == reflected::LampKind::Area);
    bed.source.BeginEdit(SV(u8"layers"));
    bed.source.CommitEdit(SV(u8"layers"), PropertyWrite{Variant(i64{4}), 4}); // a flag alone
    CHECK(static_cast<u32>(bed.a->layers) == 5u);
    bed.Commit(u8"position.z", Variant(9.0));
    CHECK(bed.a->position.z == doctest::Approx(9.0f));
    bed.Commit(u8"shape.radius", Variant(4.0));
    CHECK(bed.a->shape.radius == doctest::Approx(4.0f));
    bed.Commit(u8"exposure", Variant(1.5)); // through its setter
    CHECK(bed.a->GetExposure() == doctest::Approx(1.5f));
    CHECK(bed.source.GetValue(SV(u8"exposure"), 0).AsFloat() == doctest::Approx(1.5));

    // A Quaternion's yaw in degrees.
    bed.Commit(u8"rotation.yaw", Variant(90.0));
    CHECK(bed.source.GetValue(SV(u8"rotation.yaw"), 0).AsFloat() == doctest::Approx(90.0).epsilon(0.001));
    f32 yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    core::ToYawPitchRoll(bed.a->rotation, yaw, pitch, roll);
    CHECK(core::RadiansToDegrees(yaw) == doctest::Approx(90.0f).epsilon(0.001));

    // Read only: refused.
    bed.Commit(u8"id", Variant(i64{5}));
    CHECK(bed.a->id == 101);
}

TEST_CASE("reflected source: several objects mixed, a preview on each, and a commit told as one change Restore reads back")
{
    Bed bed(true);
    reflected::BlobFormat format;
    bed.source.SetTextFormat(&format);
    bed.b->intensity = 8.0f;
    CHECK(ReadPropertyValue(bed.source, SV(u8"intensity"), PropertyKind::Float).Mixed);
    CHECK_FALSE(ReadPropertyValue(bed.source, SV(u8"position.x"), PropertyKind::Float).Mixed);

    bed.source.BeginEdit(SV(u8"intensity"));
    bed.source.PreviewEdit(SV(u8"intensity"), PropertyWrite{Variant(20.0)});
    CHECK(bed.a->intensity == doctest::Approx(20.0f));
    CHECK(bed.b->intensity == doctest::Approx(20.0f));
    bed.source.CommitEdit(SV(u8"intensity"), PropertyWrite{Variant(30.0)});
    REQUIRE(bed.changes.Size() == 1u);
    const ReflectedChange& change = bed.changes[0];
    CHECK(change.Path == core::String(u8"intensity"));
    REQUIRE(change.Before.Size() == 2u);

    CHECK(bed.source.Restore(change, true)); // an undo: each its own value back
    CHECK(bed.a->intensity == doctest::Approx(5.0f));
    CHECK(bed.b->intensity == doctest::Approx(8.0f));
    CHECK(bed.source.Restore(change, false)); // a redo
    CHECK(bed.b->intensity == doctest::Approx(30.0f));

    // A group's change is the whole group, so a revert or a paste undoes in one.
    bed.Commit(u8"shape.radius", Variant(6.0));
    bed.a->shape.softness = 0.9f; // changed beside it, outside the source
    bed.source.Restore(bed.changes.Back(), true);
    CHECK(bed.a->shape.radius == doctest::Approx(1.0f));

    // Cancelled: each back.
    bed.source.BeginEdit(SV(u8"range"));
    bed.source.PreviewEdit(SV(u8"range"), PropertyWrite{Variant(99.0)});
    bed.source.CancelEdit(SV(u8"range"));
    CHECK(bed.a->range == doctest::Approx(10.0));
}

TEST_CASE("reflected source: the grid shows it")
{
    Bed bed;
    auto root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
    root->SetSize(core::Float2{400.0f, 600.0f});
    root->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Start; });
    auto grid = core::MakeRef<PropertyGrid>(core::DefaultAllocator());
    grid->EditLayoutStyle([](LayoutStyle& s)
    {
        s.Width = LayoutLength::Px(360.0f);
        s.Height = LayoutLength::Px(560.0f);
    });
    root->AddChild(grid.Get());
    grid->SetSource(&bed.source);
    (void)root->UpdateLayout();
    CHECK(core::Cast<FloatsPropertyEditor>(grid->FindEditor(SV(u8"position"))) != nullptr);
    CHECK(core::Cast<FloatsPropertyEditor>(grid->FindEditor(SV(u8"rotation"))) != nullptr);
    CHECK(grid->FindEditor(SV(u8"tint")) == nullptr); // a Color no editor takes yet (T4): its r, g, b, a
    CHECK(grid->FindEditor(SV(u8"shape")) == nullptr); // a struct no editor takes: a group of its rows
    CHECK(grid->GetRow(grid->FindRow(SV(u8"shape")))->HasChildren);
    CHECK(core::Cast<EnumPropertyEditor>(grid->FindEditor(SV(u8"kind"))) != nullptr);
    CHECK(core::Cast<FlagsPropertyEditor>(grid->FindEditor(SV(u8"layers"))) != nullptr);
    CHECK(grid->FindRow(SV(u8"coneAngle")) < 0);
    grid->SetSource(nullptr);
}
