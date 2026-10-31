// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include "Core/Prelude.h" // brings <new> into reach for container instantiation (GCC)
#include "Core/Reflection/Reflect.h"

import foundation.core;

using namespace foundation::core;

// Reflection of Core value types is registered explicitly; idempotent, so each
// test can ensure it.
namespace
{
    void EnsureRegistered() { RegisterCoreTypes(); }
}

TEST_CASE("core-reflection: value-type properties are reflected")
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

TEST_CASE("core-reflection: get / set a property through an Instance")
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

TEST_CASE("core-reflection: PropertyInfo::address exposes the raw field")
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

TEST_CASE("core-reflection: nested value-type properties (Transform)")
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

TEST_CASE("core-reflection: named constants are reflected")
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

TEST_CASE("core-reflection: member, const, and static methods invoke")
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

TEST_CASE("core-reflection: overloaded free functions reflect (disambiguated by cast)")
{
    EnsureRegistered();

    // Dot and Length are free functions, reflected as static methods.
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

TEST_CASE("core-reflection: same-named overloads resolved by parameter type")
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

TEST_CASE("core-reflection: Float3 vector ops reflect (Cross/Add/Sub/Lerp/Distance)")
{
    EnsureRegistered();

    const auto invoke = [](const char* name, Span<Variant> args) {
        const MethodInfo* m = FindMethod(TypeOf<Float3>(), name);
        REQUIRE(m != nullptr);
        CHECK(m->isStatic);
        return InvokeStatic(*m, args).Value();
    };

    Variant crossArgs[] = {Variant::From(Float3{1.0f, 0.0f, 0.0f}),
                           Variant::From(Float3{0.0f, 1.0f, 0.0f})};
    CHECK(invoke("Cross", Span<Variant>{crossArgs, 2}).Get<Float3>() == Float3{0.0f, 0.0f, 1.0f});

    Variant addArgs[] = {Variant::From(Float3{1.0f, 2.0f, 3.0f}),
                         Variant::From(Float3{4.0f, 5.0f, 6.0f})};
    CHECK(invoke("Add", Span<Variant>{addArgs, 2}).Get<Float3>() == Float3{5.0f, 7.0f, 9.0f});

    Variant subArgs[] = {Variant::From(Float3{4.0f, 5.0f, 6.0f}),
                         Variant::From(Float3{1.0f, 2.0f, 3.0f})};
    CHECK(invoke("Sub", Span<Variant>{subArgs, 2}).Get<Float3>() == Float3{3.0f, 3.0f, 3.0f});

    Variant lerpArgs[] = {Variant::From(Float3{0.0f, 0.0f, 0.0f}),
                          Variant::From(Float3{10.0f, 20.0f, 30.0f}), Variant::From(0.5f)};
    CHECK(invoke("Lerp", Span<Variant>{lerpArgs, 3}).Get<Float3>() == Float3{5.0f, 10.0f, 15.0f});

    Variant distArgs[] = {Variant::From(Float3{0.0f, 0.0f, 0.0f}),
                          Variant::From(Float3{3.0f, 4.0f, 0.0f})};
    CHECK(NearlyEqual(invoke("Distance", Span<Variant>{distArgs, 2}).Get<f32>(), 5.0f));

    Variant lenSqArgs[] = {Variant::From(Float3{1.0f, 2.0f, 2.0f})};
    CHECK(NearlyEqual(invoke("LengthSquared", Span<Variant>{lenSqArgs, 1}).Get<f32>(), 9.0f));
}

// The math values' operators are marked, each a static whose first parameter is the left
// operand, so a script surface spells them as operators.
TEST_CASE("core-reflection: the math values mark the operators they define")
{
    EnsureRegistered();
    const auto ops = [](const TypeInfo& type, MethodOperator op)
    {
        u32 count = 0;
        for (const MethodInfo& method : Methods(type))
        {
            if (method.op == op)
            {
                CHECK(method.isStatic);
                CHECK(method.paramCount == (op == MethodOperator::Negate ? 1u : 2u));
                ++count;
            }
        }
        return count;
    };
    CHECK(ops(TypeOf<Float3>(), MethodOperator::Add) == 1u);
    CHECK(ops(TypeOf<Float3>(), MethodOperator::Multiply) == 2u); // by a vector, by a scalar
    CHECK(ops(TypeOf<Float3>(), MethodOperator::Divide) == 2u);
    CHECK(ops(TypeOf<Float3>(), MethodOperator::Negate) == 1u);
    CHECK(ops(TypeOf<Float3>(), MethodOperator::Equals) == 1u);
    CHECK(ops(TypeOf<Float2>(), MethodOperator::Subtract) == 1u);
    CHECK(ops(TypeOf<Float4>(), MethodOperator::Divide) == 0u); // Float4 defines no division
    CHECK(ops(TypeOf<Quaternion>(), MethodOperator::Multiply) == 1u);
    CHECK(ops(TypeOf<Color>(), MethodOperator::Add) == 1u);

    const MethodInfo* neg = FindMethod(TypeOf<Float3>(), "Neg");
    REQUIRE(neg != nullptr);
    Variant v[] = {Variant::From(Float3{1.0f, -2.0f, 3.0f})};
    CHECK(InvokeStatic(*neg, Span<Variant>{v, 1}).Value().Get<Float3>() == Float3{-1.0f, 2.0f, -3.0f});
    const MethodInfo* equals = FindMethod(TypeOf<Float3>(), "Equals");
    REQUIRE(equals != nullptr);
    Variant same[] = {Variant::From(Float3{1.0f, 2.0f, 3.0f}), Variant::From(Float3{1.0f, 2.0f, 3.0f})};
    CHECK(InvokeStatic(*equals, Span<Variant>{same, 2}).Value().Get<bool>());
}

TEST_CASE("core-reflection: Quaternion ops reflect (FromAxisAngle/Mul/RotateVector/Slerp)")
{
    EnsureRegistered();

    const auto method = [](const char* name) {
        const MethodInfo* m = FindMethod(TypeOf<Quaternion>(), name);
        REQUIRE(m != nullptr);
        CHECK(m->isStatic);
        return m;
    };

    // 90 degrees about +Y.
    Variant faaArgs[] = {Variant::From(Float3{0.0f, 1.0f, 0.0f}), Variant::From(kHalfPi)};
    const Quaternion q =
        InvokeStatic(*method("FromAxisAngle"), Span<Variant>{faaArgs, 2}).Value().Get<Quaternion>();
    CHECK(NearlyEqual(q, Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, kHalfPi)));

    // Identity * q == q.
    Variant mulArgs[] = {Variant::From(Quaternion::Identity), Variant::From(q)};
    CHECK(NearlyEqual(
        InvokeStatic(*method("Mul"), Span<Variant>{mulArgs, 2}).Value().Get<Quaternion>(), q));

    // Rotating +Z by 90deg about +Y yields +X.
    Variant rotArgs[] = {Variant::From(q), Variant::From(Float3{0.0f, 0.0f, 1.0f})};
    const Float3 rotated =
        InvokeStatic(*method("RotateVector"), Span<Variant>{rotArgs, 2}).Value().Get<Float3>();
    CHECK(NearlyEqual(rotated.x, 1.0f));
    CHECK(NearlyEqual(rotated.y, 0.0f));
    CHECK(NearlyEqual(rotated.z, 0.0f));

    // Slerp endpoints.
    Variant slerpArgs[] = {Variant::From(Quaternion::Identity), Variant::From(q),
                           Variant::From(0.0f)};
    CHECK(NearlyEqual(
        InvokeStatic(*method("Slerp"), Span<Variant>{slerpArgs, 3}).Value().Get<Quaternion>(),
        Quaternion::Identity));
}

TEST_CASE("core-reflection: Math scalar functions reflect as statics on the Math anchor")
{
    EnsureRegistered();

    const TypeInfo& math = TypeOf<Math>();

    const auto call1 = [&](const char* name, f32 x) {
        const MethodInfo* m = FindMethod(math, name);
        REQUIRE(m != nullptr);
        CHECK(m->isStatic);
        Variant args[] = {Variant::From(x)};
        return InvokeStatic(*m, Span<Variant>{args, 1}).Value().Get<f32>();
    };

    CHECK(NearlyEqual(call1("Sin", kHalfPi), 1.0f));
    CHECK(NearlyEqual(call1("Cos", 0.0f), 1.0f));
    CHECK(NearlyEqual(call1("Sqrt", 9.0f), 3.0f));
    CHECK(NearlyEqual(call1("Abs", -4.0f), 4.0f));
    CHECK(NearlyEqual(call1("Floor", 2.7f), 2.0f));
    CHECK(NearlyEqual(call1("Ceil", 2.1f), 3.0f));
    CHECK(NearlyEqual(call1("DegreesToRadians", 180.0f), kPi));

    // Two- and three-arg forms.
    const MethodInfo* atan2 = FindMethod(math, "Atan2");
    REQUIRE(atan2 != nullptr);
    Variant atanArgs[] = {Variant::From(1.0f), Variant::From(0.0f)};
    CHECK(NearlyEqual(InvokeStatic(*atan2, Span<Variant>{atanArgs, 2}).Value().Get<f32>(), kHalfPi));

    const MethodInfo* lerp = FindMethod(math, "Lerp");
    REQUIRE(lerp != nullptr);
    Variant lerpArgs[] = {Variant::From(0.0f), Variant::From(10.0f), Variant::From(0.5f)};
    CHECK(NearlyEqual(InvokeStatic(*lerp, Span<Variant>{lerpArgs, 3}).Value().Get<f32>(), 5.0f));

    const auto call2 = [&](const char* name, f32 a, f32 b) {
        const MethodInfo* m = FindMethod(math, name);
        REQUIRE(m != nullptr);
        Variant args[] = {Variant::From(a), Variant::From(b)};
        return InvokeStatic(*m, Span<Variant>{args, 2}).Value().Get<f32>();
    };
    CHECK(NearlyEqual(call2("Min", 3.0f, -2.0f), -2.0f));
    CHECK(NearlyEqual(call2("Max", 3.0f, -2.0f), 3.0f));
    const MethodInfo* clamp = FindMethod(math, "Clamp");
    REQUIRE(clamp != nullptr);
    const auto clamped = [&](f32 x) {
        Variant args[] = {Variant::From(x), Variant::From(0.0f), Variant::From(1.0f)};
        return InvokeStatic(*clamp, Span<Variant>{args, 3}).Value().Get<f32>();
    };
    CHECK(NearlyEqual(clamped(-0.5f), 0.0f));
    CHECK(NearlyEqual(clamped(0.25f), 0.25f));
    CHECK(NearlyEqual(clamped(7.0f), 1.0f));

    // Reflected constants.
    CHECK(NearlyEqual(FindConstant(math, "Pi")->value.Get<f32>(), kPi));
    CHECK(NearlyEqual(FindConstant(math, "TwoPi")->value.Get<f32>(), kTwoPi));

    // In the global registry by qualified name.
    CHECK(GlobalTypeRegistry().FindByName("rtti::core", "Math") == &math);
}

TEST_CASE("core-reflection: matrix elements via container + ops as methods")
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

    // Static factory and free operators reflected as methods.
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

TEST_CASE("core-reflection: count / by-index accessors (binding-generator style)")
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

    // Iterate methods by index and read each signature via ParamCount/ParamAt, the way a
    // binding generator walks the type. Two 2-arg methods start with (Float3, f32) and
    // (Float3, Float3): the Mul overloads.
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

TEST_CASE("core-reflection: construct value types via reflection")
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

    // No matching overload gives InvalidArgument.
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

    // Guid: default + (u64,u64) + the string form (the script backends surface the
    // String-declared overload as `Guid(string text)`).
    CHECK(ConstructorCount(TypeOf<Guid>()) == 3u);
    Variant guidText[] = {
        Variant::From<String>(String(u8"00000000-0000-0055-0000-000000000066"))};
    CHECK(Construct(TypeOf<Guid>(), Span<Variant>{guidText, 1}).Value().Get<Guid>() ==
          Guid{0x55, 0x66});
    Variant badText[] = {Variant::From<String>(String(u8"not-a-guid"))};
    CHECK(Construct(TypeOf<Guid>(), Span<Variant>{badText, 1}).Value().Get<Guid>().IsNil());
}

TEST_CASE("core-reflection: namespace-level constants are registered")
{
    EnsureRegistered();
    ConstantRegistry& cr = GlobalConstantRegistry();

    const NamedConstant* pi = cr.Find("rtti::core", "kPi");
    REQUIRE(pi != nullptr);
    CHECK(pi->type == &TypeOf<f32>());
    CHECK(pi->value.Get<f32>() == kPi);

    CHECK(cr.Find("rtti::core", "kEpsilon")->value.Get<f32>() == kEpsilon);
    CHECK(cr.Find("rtti::core", "nope") == nullptr);
    CHECK(cr.Count() >= 8u);
    CHECK(cr.All().Size() == cr.Count());
    CHECK(&cr.At(0) == &cr.All()[0]);
}

TEST_CASE("core-reflection: public enums are reflected")
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
    CHECK(EnumeratorCount(TypeOf<FitMode>()) == 4u);
    i64 integer = -1;
    CHECK(EnumValueByName(TypeOf<FitMode>(), "IntegerScale", integer));
    CHECK(integer == static_cast<i64>(FitMode::IntegerScale));

    // An enum field read and written through reflection, by its size.
    FitMode fit = FitMode::Letterbox;
    CHECK(ReadEnumValue(&fit, TypeOf<FitMode>()) == static_cast<i64>(FitMode::Letterbox));
    WriteEnumValue(&fit, TypeOf<FitMode>(), static_cast<i64>(FitMode::Crop));
    CHECK(fit == FitMode::Crop);
    i64 notFound = -1;
    CHECK(EnumValueByName(TypeOf<ErrorCode>(), "NotFound", notFound));
    CHECK(notFound == static_cast<i64>(ErrorCode::NotFound));

    CHECK(GlobalTypeRegistry().FindByName("rtti::core", "LogLevel") == &TypeOf<LogLevel>());
    CHECK(GlobalTypeRegistry().FindByName("rtti::core", "FileMode") == &TypeOf<FileMode>());
}

TEST_CASE("core-reflection: types are in the global registry by qualified name")
{
    EnsureRegistered();

    const TypeInfo* vec3 = GlobalTypeRegistry().FindByName("rtti::core", "Float3");
    REQUIRE(vec3 != nullptr);
    CHECK(vec3 == &TypeOf<Float3>());

    CHECK(GlobalTypeRegistry().FindByName("rtti::core", "Guid") == &TypeOf<Guid>());
    CHECK(GlobalTypeRegistry().FindByName("rtti::core", "Nope") == nullptr);
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

REFLECT_VALUE(AttrWidget, "rtti::tests")
{
    builder.Property<&AttrWidget::speed>("speed")
        .PropAttribute("range", Float4{0.0f, 10.0f, 0.5f, 0.0f})
        .PropAttribute("description", String(u8"How fast"))
        .Property<&AttrWidget::mode>("mode")
        .Property<&AttrWidget::active>("active")
        .PropAttribute("visibleWhen", String(u8"mode=1"));
    builder.Attribute("category", String(u8"testing")); // type-level coexists
}

TEST_CASE("core-reflection: per-property attributes via PropAttribute")
{
    EnsureRegistered();
    RttiRegisterValue_AttrWidget();

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

TEST_CASE("reflection: RegisterCoreTypes is safe to call concurrently (the cook workers do)")
{
    // The script cooks call RegisterCoreTypes per build on job-system workers, so concurrent
    // calls must not rebuild the global enum tables at the same time (which double-frees them).
    // Hammer it from many workers and then read the tables: intact, and registered exactly once.
    JobSystem jobs(DefaultAllocator(), 4);
    jobs.ParallelFor(64, [](u32) { RegisterCoreTypes(); }, 1);
    CHECK(EnumeratorCount(TypeOf<SeekOrigin>()) == 3u);
    CHECK(EnumeratorCount(TypeOf<FileMode>()) == 4u);
    CHECK(EnumValueName(TypeOf<SeekOrigin>(), static_cast<i64>(SeekOrigin::End)) != nullptr);
    i64 end = -1;
    CHECK(EnumValueByName(TypeOf<SeekOrigin>(), "End", end));
    CHECK(end == static_cast<i64>(SeekOrigin::End));
}

TEST_CASE("reflection: TypeRegistry::Register is safe under concurrent first-time registrations")
{
    // Cook workers register their product types as they go, so two first-time registrations at
    // once must not rehash the id map under each other. Sixty-four distinct types from four
    // workers into a private registry: every one lands exactly once and stays findable.
    static TypeInfo infos[64] = {};
    for (u32 i = 0; i < 64; ++i)
    {
        infos[i].id = 0x5EED0000ull + i;
        infos[i].name = "ConcurrentType";
        infos[i].namespaceName = "rtti::tests";
    }
    TypeRegistry registry;
    JobSystem jobs(DefaultAllocator(), 4);
    jobs.ParallelFor(64, [&registry](u32 i) { registry.Register(infos[i]); }, 1);
    // Re-registering is a no-op (idempotent), also from workers.
    jobs.ParallelFor(64, [&registry](u32 i) { registry.Register(infos[i]); }, 1);
    CHECK(registry.All().Size() == 64u);
    for (u32 i = 0; i < 64; ++i)
    {
        CHECK(registry.FindById(infos[i].id) == &infos[i]);
    }
}
