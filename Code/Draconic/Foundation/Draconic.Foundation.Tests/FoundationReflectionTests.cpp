#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h" // brings <new> into reach for container instantiation (GCC)
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;

using namespace draconic::foundation;

// Reflection of Foundation value types is registered explicitly; idempotent, so each
// test can ensure it.
namespace
{
    void EnsureRegistered() { RegisterFoundationTypes(); }
}

TEST_CASE("foundation-reflection: value-type properties are reflected")
{
    EnsureRegistered();

    const TypeInfo& vec3 = TypeOf<Float3>();
    CHECK(Properties(vec3).Size() == 3u);

    const PropertyInfo* x = FindProperty(vec3, "x");
    const PropertyInfo* y = FindProperty(vec3, "y");
    const PropertyInfo* z = FindProperty(vec3, "z");
    REQUIRE(x != nullptr);
    REQUIRE(y != nullptr);
    REQUIRE(z != nullptr);
    CHECK(x->type == &TypeOf<f32>());
    CHECK(FindProperty(vec3, "w") == nullptr);
}

TEST_CASE("foundation-reflection: get / set a property through an Instance")
{
    EnsureRegistered();

    Float3 v{1.0f, 2.0f, 3.0f};
    Instance inst = Instance::From(&v);

    const PropertyInfo* y = FindProperty(TypeOf<Float3>(), "y");
    REQUIRE(y != nullptr);
    CHECK(GetProperty(*y, inst).Get<f32>() == 2.0f);

    CHECK(SetProperty(*y, inst, Variant::From(9.0f)).IsOk());
    CHECK(v.y == 9.0f);
    CHECK(GetProperty(*y, inst).Get<f32>() == 9.0f);

    // Wrong-typed set is rejected.
    CHECK_FALSE(SetProperty(*y, inst, Variant::From(7)).IsOk());
}

TEST_CASE("foundation-reflection: PropertyInfo::address exposes the raw field")
{
    EnsureRegistered();

    Float3 v{1.0f, 2.0f, 3.0f};
    Instance inst = Instance::From(&v);

    const PropertyInfo* y = FindProperty(TypeOf<Float3>(), "y");
    REQUIRE(y != nullptr);
    REQUIRE(y->address != nullptr);

    // The address path points at the live field: reads see the current value,
    // writes through it are visible to the normal getter.
    void* addr = y->address(inst);
    REQUIRE(addr == &v.y);
    CHECK(*static_cast<f32*>(addr) == 2.0f);
    *static_cast<f32*>(addr) = 8.0f;
    CHECK(GetProperty(*y, inst).Get<f32>() == 8.0f);
}

TEST_CASE("foundation-reflection: nested value-type properties (Transform)")
{
    EnsureRegistered();

    const TypeInfo& transform = TypeOf<Transform>();
    CHECK(Properties(transform).Size() == 3u);

    const PropertyInfo* position = FindProperty(transform, "position");
    const PropertyInfo* rotation = FindProperty(transform, "rotation");
    REQUIRE(position != nullptr);
    REQUIRE(rotation != nullptr);
    CHECK(position->type == &TypeOf<Float3>());
    CHECK(rotation->type == &TypeOf<Quaternion>());

    Transform t;
    Instance inst = Instance::From(&t);
    CHECK(SetProperty(*position, inst, Variant::From(Float3{4.0f, 5.0f, 6.0f})).IsOk());
    CHECK(t.position == Float3{4.0f, 5.0f, 6.0f});
    CHECK(GetProperty(*position, inst).Get<Float3>() == Float3{4.0f, 5.0f, 6.0f});
}

TEST_CASE("foundation-reflection: named constants are reflected")
{
    EnsureRegistered();

    const TypeInfo& vec3 = TypeOf<Float3>();
    CHECK(Constants(vec3).Size() == 5u);

    const ConstantInfo* zero = FindConstant(vec3, "Zero");
    const ConstantInfo* unitY = FindConstant(vec3, "UnitY");
    REQUIRE(zero != nullptr);
    REQUIRE(unitY != nullptr);
    CHECK(zero->type == &TypeOf<Float3>());
    CHECK(zero->value.Get<Float3>() == Float3::Zero);
    CHECK(unitY->value.Get<Float3>() == Float3::UnitY);
    CHECK(FindConstant(vec3, "Nope") == nullptr);

    // Constants on other types.
    CHECK(NearlyEqual(FindConstant(TypeOf<Quaternion>(), "Identity")->value.Get<Quaternion>(),
                      Quaternion::Identity));
    CHECK(FindConstant(TypeOf<Color>(), "Red")->value.Get<Color>() == Color::Red);
    CHECK(FindConstant(TypeOf<Guid>(), "Nil")->value.Get<Guid>() == Guid::Nil);
}

TEST_CASE("foundation-reflection: member, const, and static methods invoke")
{
    EnsureRegistered();

    // const member returning a value type
    Float4 v{1.0f, 2.0f, 3.0f, 4.0f};
    Instance vi = Instance::From(&v);
    const MethodInfo* xyz = FindMethod(TypeOf<Float4>(), "XYZ");
    REQUIRE(xyz != nullptr);
    CHECK_FALSE(xyz->isStatic);
    CHECK(xyz->isConst);
    CHECK(InvokeMethod(*xyz, vi, Span<Variant>{}).Value().Get<Float3>() ==
          Float3{1.0f, 2.0f, 3.0f});

    // const member returning a scalar
    Color white = Color::White;
    Instance wi = Instance::From(&white);
    const MethodInfo* toRGBA = FindMethod(TypeOf<Color>(), "ToRGBA8");
    REQUIRE(toRGBA != nullptr);
    CHECK(InvokeMethod(*toRGBA, wi, Span<Variant>{}).Value().Get<u32>() == 0xFFFFFFFFu);

    // static factory
    const MethodInfo* fromRGBA = FindMethod(TypeOf<Color>(), "FromRGBA8");
    REQUIRE(fromRGBA != nullptr);
    CHECK(fromRGBA->isStatic);
    Variant fromArgs[] = {Variant::From<u32>(0xFFFFFFFFu)};
    CHECK(InvokeStatic(*fromRGBA, Span<Variant>{fromArgs, 1}).Value().Get<Color>() == Color::White);

    // member taking an argument
    Guid nil = Guid::Nil;
    Instance gi = Instance::From(&nil);
    CHECK(InvokeMethod(*FindMethod(TypeOf<Guid>(), "IsNil"), gi, Span<Variant>{})
              .Value()
              .Get<bool>());
}

TEST_CASE("foundation-reflection: overloaded free functions reflect (disambiguated by cast)")
{
    EnsureRegistered();

    // Dot/Length were overloaded free functions; reflected as static methods.
    const MethodInfo* dot = FindMethod(TypeOf<Float3>(), "Dot");
    REQUIRE(dot != nullptr);
    CHECK(dot->isStatic);
    CHECK(dot->returnType() == &TypeOf<f32>());
    REQUIRE(dot->paramCount == 2u);
    CHECK(dot->params[0].type() == &TypeOf<Float3>());
    Variant dotArgs[] = {Variant::From(Float3{1.0f, 2.0f, 3.0f}),
                         Variant::From(Float3{4.0f, 5.0f, 6.0f})};
    CHECK(InvokeStatic(*dot, Span<Variant>{dotArgs, 2}).Value().Get<f32>() == 32.0f);
}

TEST_CASE("foundation-reflection: same-named overloads resolved by parameter type")
{
    EnsureRegistered();

    const Float3 a{2.0f, 3.0f, 4.0f};

    // Float3 * f32
    const TypeInfo* const scalarSig[] = {&TypeOf<Float3>(), &TypeOf<f32>()};
    const MethodInfo* mulScalar =
        FindMethod(TypeOf<Float3>(), "Mul", Span<const TypeInfo* const>{scalarSig, 2});
    REQUIRE(mulScalar != nullptr);

    // Float3 * Float3
    const TypeInfo* const vecSig[] = {&TypeOf<Float3>(), &TypeOf<Float3>()};
    const MethodInfo* mulVec =
        FindMethod(TypeOf<Float3>(), "Mul", Span<const TypeInfo* const>{vecSig, 2});
    REQUIRE(mulVec != nullptr);

    CHECK(mulScalar != mulVec); // distinct overloads selected by signature

    Variant scalarArgs[] = {Variant::From(a), Variant::From(2.0f)};
    CHECK(InvokeStatic(*mulScalar, Span<Variant>{scalarArgs, 2}).Value().Get<Float3>() ==
          Float3{4.0f, 6.0f, 8.0f});

    Variant vecArgs[] = {Variant::From(a), Variant::From(Float3{1.0f, 2.0f, 3.0f})};
    CHECK(InvokeStatic(*mulVec, Span<Variant>{vecArgs, 2}).Value().Get<Float3>() ==
          Float3{2.0f, 6.0f, 12.0f});

    // Name-only lookup still returns the first overload.
    CHECK(FindMethod(TypeOf<Float3>(), "Mul") != nullptr);
}

TEST_CASE("foundation-reflection: matrix elements via container + ops as methods")
{
    EnsureRegistered();
    const TypeInfo& mat4 = TypeOf<Float4x4>();

    // Element access through the container facility (flat, row-major).
    REQUIRE(IsContainer(mat4));
    const ContainerInfo* c = mat4.container;
    REQUIRE(c != nullptr);
    CHECK(c->elementType == &TypeOf<f32>());

    Float4x4 m = Float4x4::Identity();
    Instance inst = Instance::From(&m);
    CHECK(ContainerSize(*c, inst) == 16u);
    CHECK(ContainerGetAt(*c, inst, 0).Get<f32>() == 1.0f);          // m(0,0)
    CHECK(ContainerGetAt(*c, inst, 1).Get<f32>() == 0.0f);          // m(0,1)
    CHECK(ContainerSetAt(*c, inst, 5, Variant::From(7.0f)).IsOk()); // m(1,1)
    CHECK(m.m[1][1] == 7.0f);

    CHECK(ContainerSize(*TypeOf<Float3x3>().container, Instance::From(&m)) == 9u);

    // Static factory + free ops reflected as methods.
    Float4x4 id =
        InvokeStatic(*FindMethod(mat4, "Identity"), Span<Variant>{}).Value().Get<Float4x4>();
    CHECK(NearlyEqual(id, Float4x4::Identity()));

    const Float4x4 t = Float4x4::Translation(Float3{1.0f, 2.0f, 3.0f});
    Variant mulArgs[] = {Variant::From(t), Variant::From(Float4x4::Identity())};
    Float4x4 product =
        InvokeStatic(*FindMethod(mat4, "Mul"), Span<Variant>{mulArgs, 2}).Value().Get<Float4x4>();
    CHECK(NearlyEqual(product, t));

    Variant detArgs[] = {Variant::From(Float4x4::Identity())};
    CHECK(InvokeStatic(*FindMethod(mat4, "Determinant"), Span<Variant>{detArgs, 1})
              .Value()
              .Get<f32>() == 1.0f);
}

TEST_CASE("foundation-reflection: count / by-index accessors (binding-generator style)")
{
    EnsureRegistered();
    const TypeInfo& vec3 = TypeOf<Float3>();

    // Count/At agree with the Span views.
    REQUIRE(PropertyCount(vec3) == Properties(vec3).Size());
    CHECK(&PropertyAt(vec3, 2) == &Properties(vec3)[2]);

    REQUIRE(ConstantCount(vec3) == Constants(vec3).Size());
    CHECK(ConstantAt(vec3, 0).value.Get<Float3>() == Float3::Zero);

    REQUIRE(MethodCount(vec3) == Methods(vec3).Size());
    CHECK(MethodCount(vec3) >= 5u); // Dot, Length, Normalized, Mul x2

    // Iterate methods by index and read each signature via ParamCount/ParamAt
    // - exactly how a binding generator would walk the type. Two 2-arg methods
    // start with (Float3, f32) / (Float3, Float3): the Mul overloads.
    usize vec3FirstParam = 0;
    for (usize i = 0; i < MethodCount(vec3); ++i)
    {
        const MethodInfo& m = MethodAt(vec3, i);
        for (usize p = 0; p < ParamCount(m); ++p)
        {
            CHECK(ParamAt(m, p).type() != nullptr); // every param carries type info
        }
        if (ParamCount(m) >= 1 && ParamAt(m, 0).type() == &TypeOf<Float3>())
        {
            ++vec3FirstParam;
        }
    }
    CHECK(vec3FirstParam >= 2u);
}

TEST_CASE("foundation-reflection: construct value types via reflection")
{
    EnsureRegistered();
    const TypeInfo& vec3 = TypeOf<Float3>();
    CHECK(ConstructorCount(vec3) == 2u); // default + (f32,f32,f32)

    // Parameterized constructor.
    Variant args[] = {Variant::From(1.0f), Variant::From(2.0f), Variant::From(3.0f)};
    Result<Variant> made = Construct(vec3, Span<Variant>{args, 3});
    REQUIRE(made.HasValue());
    CHECK(made.Value().Get<Float3>() == Float3{1.0f, 2.0f, 3.0f});

    // Default constructor (overload picked by arity).
    CHECK(Construct(vec3, Span<Variant>{}).Value().Get<Float3>() == Float3::Zero);

    // No matching overload -> InvalidArgument.
    Variant bad[] = {Variant::From(1.0f)};
    CHECK(Construct(vec3, Span<Variant>{bad, 1}).Error() == ErrorCode::InvalidArgument);

    // Wrong arg type at a matching arity is also rejected.
    Variant wrong[] = {Variant::From(1), Variant::From(2), Variant::From(3)}; // int, not f32
    CHECK_FALSE(Construct(vec3, Span<Variant>{wrong, 3}).HasValue());

    // Aggregate value type (parenthesized aggregate init).
    Variant rectArgs[] = {Variant::From(1.0f), Variant::From(2.0f), Variant::From(3.0f),
                          Variant::From(4.0f)};
    Rectangle r =
        Construct(TypeOf<Rectangle>(), Span<Variant>{rectArgs, 4}).Value().Get<Rectangle>();
    CHECK(r.width == 3.0f);
}

TEST_CASE("foundation-reflection: namespace-level constants are registered")
{
    EnsureRegistered();
    ConstantRegistry& cr = GlobalConstantRegistry();

    const NamedConstant* pi = cr.Find("draconic::foundation", "kPi");
    REQUIRE(pi != nullptr);
    CHECK(pi->type == &TypeOf<f32>());
    CHECK(pi->value.Get<f32>() == kPi);

    CHECK(cr.Find("draconic::foundation", "kEpsilon")->value.Get<f32>() == kEpsilon);
    CHECK(cr.Find("draconic::foundation", "nope") == nullptr);
    CHECK(cr.Count() >= 8u);
    CHECK(cr.All().Size() == cr.Count());
    CHECK(&cr.At(0) == &cr.All()[0]);
}

TEST_CASE("foundation-reflection: public enums are reflected")
{
    EnsureRegistered();

    const TypeInfo& level = TypeOf<LogLevel>();
    CHECK(IsEnum(level));
    CHECK(EnumeratorCount(level) == 7u);

    i64 warning = -1;
    CHECK(EnumValueByName(level, "Warning", warning));
    CHECK(warning == static_cast<i64>(LogLevel::Warning));

    // EnumValueName (metadata names are ASCII char*).
    const char* name = EnumValueName(level, static_cast<i64>(LogLevel::Error));
    REQUIRE(name != nullptr);
    CHECK(EnumeratorAt(level, 4).value == static_cast<i64>(LogLevel::Error));

    // Other public enums.
    CHECK(EnumeratorCount(TypeOf<FileMode>()) == 4u);
    CHECK(EnumeratorCount(TypeOf<SeekOrigin>()) == 3u);
    i64 notFound = -1;
    CHECK(EnumValueByName(TypeOf<ErrorCode>(), "NotFound", notFound));
    CHECK(notFound == static_cast<i64>(ErrorCode::NotFound));

    CHECK(GlobalTypeRegistry().FindByName("draconic::foundation", "LogLevel") == &TypeOf<LogLevel>());
    CHECK(GlobalTypeRegistry().FindByName("draconic::foundation", "FileMode") == &TypeOf<FileMode>());
}

TEST_CASE("foundation-reflection: types are in the global registry by qualified name")
{
    EnsureRegistered();

    const TypeInfo* vec3 = GlobalTypeRegistry().FindByName("draconic::foundation", "Float3");
    REQUIRE(vec3 != nullptr);
    CHECK(vec3 == &TypeOf<Float3>());

    CHECK(GlobalTypeRegistry().FindByName("draconic::foundation", "Guid") == &TypeOf<Guid>());
    CHECK(GlobalTypeRegistry().FindByName("draconic::foundation", "Nope") == nullptr);
}

// --- Per-property attributes (PropAttribute) ---

namespace
{
    struct AttrWidget
    {
        f32 speed = 1.0f;
        i32 mode = 0;
        bool active = true;
    };
}

DRACONIC_REFLECT_VALUE(AttrWidget, "draconic::tests")
{
    builder.Property<&AttrWidget::speed>("speed")
        .PropAttribute("range", Float4{0.0f, 10.0f, 0.5f, 0.0f})
        .PropAttribute("description", String(u8"How fast"))
        .Property<&AttrWidget::mode>("mode")
        .Property<&AttrWidget::active>("active")
        .PropAttribute("visibleWhen", String(u8"mode=1"));
    builder.Attribute("category", String(u8"testing")); // type-level coexists
}

TEST_CASE("foundation-reflection: per-property attributes via PropAttribute")
{
    EnsureRegistered();
    DraconicRegisterValue_AttrWidget();

    const TypeInfo& type = TypeOf<AttrWidget>();
    const PropertyInfo* speed = FindProperty(type, "speed");
    const PropertyInfo* mode = FindProperty(type, "mode");
    const PropertyInfo* active = FindProperty(type, "active");
    REQUIRE(speed != nullptr);
    REQUIRE(mode != nullptr);
    REQUIRE(active != nullptr);

    // Attribute-less properties stay clean.
    CHECK(Attributes(*mode).Size() == 0u);
    CHECK(FindAttribute(*mode, u8"range") == nullptr);

    CHECK(Attributes(*speed).Size() == 2u);
    const Attribute* range = FindAttribute(*speed, u8"range");
    REQUIRE(range != nullptr);
    const Float4* r = range->value.TryGet<Float4>();
    REQUIRE(r != nullptr);
    CHECK(r->x == 0.0f);
    CHECK(r->y == 10.0f);
    CHECK(r->z == 0.5f);

    const Attribute* desc = FindAttribute(*speed, u8"description");
    REQUIRE(desc != nullptr);
    const String* text = desc->value.TryGet<String>();
    REQUIRE(text != nullptr);
    CHECK(text->AsView() == StringView(u8"How fast"));

    const Attribute* vis = FindAttribute(*active, u8"visibleWhen");
    REQUIRE(vis != nullptr);
    REQUIRE(vis->value.TryGet<String>() != nullptr);
    CHECK(vis->value.TryGet<String>()->AsView() == StringView(u8"mode=1"));

    // Type-level attributes are unaffected by the per-property storage.
    CHECK(FindAttribute(type, "category") != nullptr);
}
