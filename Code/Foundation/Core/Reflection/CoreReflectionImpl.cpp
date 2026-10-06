// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :core_reflection implementation unit
//
// The reflection bodies for Core's value types. Kept OUT of the :core_reflection
// interface partition: REFLECT_* bodies in a partition interface make GCC
// emit an unreadable gcm cluster for consumers (see gcc-module-interface-hygiene).
// The interface (CoreReflection.cppm) only declares RegisterCoreTypes(); this unit
// defines it plus every RttiRegisterValue_/RttiRegisterEnum_ body.
//
// Plain value types are reflected non-intrusively via REFLECT_VALUE, which
// patches each type's TypeOf<T>() in place. Matrices (Float3x3/Float4x4) expose their
// f32[N][N] storage through the container facility (flat, row-major) since a C array
// can't be a property; their ops are reflected as methods.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.core;

import :base;
import :type_info;
import :type_registry;
import :constant_registry;
import :reflection;
import :enum_reflection;
import :math;
import :content_fit; // FitMode
import :instance;
import :variant;
import :logger;
import :iserializer;
import :system;
import :float2;
import :float3;
import :float4;
import :color;
import :quaternion;
import :transform;
import :float3x3;
import :float4x4;
import :aabb;
import :plane;
import :rectangle;
import :guid;

namespace foundation::core
{
    namespace
    {
        // The unary minus and equality the math types define, as statics a reflected operator
        // can name (the member operator- and the constexpr operator== have no plain pointer).
        template <typename T>
        T Negated(T value) noexcept
        {
            return -value;
        }
        template <typename T>
        bool EqualValues(T a, T b) noexcept
        {
            return a == b;
        }
    }

    // Matrices store a C array (f32[N][N]) that can't be a property, so their
    // elements are exposed via the container facility: a flat, row-major view of
    // N*N scalars (read m(r,c) as element r*N + c). No change to the math types.
    template <typename MatT, usize N>
    void RegisterMatrixElements()
    {
        static const ContainerInfo info{
            &TypeOf<f32>(), [](const Instance&) noexcept -> usize { return N * N; },
            [](const Instance& i, usize index) -> Variant
            {
                return Variant::From<f32>((&static_cast<const MatT*>(i.Pointer())->m[0][0])[index]);
            },
            [](const Instance& i, usize index, const Variant& value) -> Status
            {
                const f32* typed = value.TryGet<f32>();
                if (typed == nullptr)
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                (&static_cast<MatT*>(i.Pointer())->m[0][0])[index] = *typed;
                return Status{};
            }};
        const_cast<TypeInfo&>(TypeOf<MatT>()).container = &info;
    }

    REFLECT_VALUE(Float2, "rtti::core")
    {
        builder.Property<&Float2::x>("x")
            .Property<&Float2::y>("y")
            .Constant("Zero", Float2::Zero)
            .Constant("One", Float2::One)
            .Constant("UnitX", Float2::UnitX)
            .Constant("UnitY", Float2::UnitY)
            // The operators Float2 defines, marked for the script surfaces (a + b, v * 2, -v).
            .Method<static_cast<Float2 (*)(Float2, Float2)>(&operator+)>("Add", {"a", "b"})
            .Operator(MethodOperator::Add)
            .Method<static_cast<Float2 (*)(Float2, Float2)>(&operator-)>("Sub", {"a", "b"})
            .Operator(MethodOperator::Subtract)
            .Method<static_cast<Float2 (*)(Float2, Float2)>(&operator*)>("Mul")
            .Operator(MethodOperator::Multiply)
            .Method<static_cast<Float2 (*)(Float2, f32)>(&operator*)>("Mul")
            .OverloadedName("MulScalar")
            .Operator(MethodOperator::Multiply)
            .Method<static_cast<Float2 (*)(Float2, f32)>(&operator/)>("Div")
            .OverloadedName("DivScalar")
            .Operator(MethodOperator::Divide)
            .Method<&Negated<Float2>>("Neg", {"v"})
            .Operator(MethodOperator::Negate)
            .Method<&EqualValues<Float2>>("Equals", {"a", "b"})
            .Operator(MethodOperator::Equals)
            .Constructor()
            .Constructor<f32, f32>();
    }

    REFLECT_VALUE(Float3, "rtti::core")
    {
        builder.Property<&Float3::x>("x")
            .Property<&Float3::y>("y")
            .Property<&Float3::z>("z")
            .Constant("Zero", Float3::Zero)
            .Constant("One", Float3::One)
            .Constant("UnitX", Float3::UnitX)
            .Constant("UnitY", Float3::UnitY)
            .Constant("UnitZ", Float3::UnitZ)
            // Overloaded free functions, disambiguated by an explicit cast.
            .Method<static_cast<f32 (*)(Float3, Float3)>(&Dot)>("Dot")
            .Method<static_cast<Float3 (*)(Float3, Float3)>(&Cross)>("Cross", {"a", "b"})
            .Method<static_cast<f32 (*)(Float3)>(&Length)>("Length")
            .Method<static_cast<f32 (*)(Float3)>(&LengthSquared)>("LengthSquared", {"v"})
            .Method<static_cast<f32 (*)(Float3, Float3)>(&Distance)>("Distance", {"a", "b"})
            .Method<static_cast<Float3 (*)(Float3)>(&Normalized)>("Normalized")
            .Method<static_cast<Float3 (*)(Float3, Float3, f32)>(&Lerp)>("Lerp", {"a", "b", "t"})
            // Vector add/sub as named statics (a dynamic script surface cannot resolve operators by value).
            // Each marked as the operator it is, for the script surfaces (a + b, v * 2, -v).
            .Method<static_cast<Float3 (*)(Float3, Float3)>(&operator+)>("Add", {"a", "b"})
            .Operator(MethodOperator::Add)
            .Method<static_cast<Float3 (*)(Float3, Float3)>(&operator-)>("Sub", {"a", "b"})
            .Operator(MethodOperator::Subtract)
            // Two same-named overloads, resolved by parameter type at lookup.
            .Method<static_cast<Float3 (*)(Float3, Float3)>(&operator*)>("Mul")
            .Operator(MethodOperator::Multiply)
            // Same arity as Mul, genuinely type-overloaded (vec*vec vs vec*scalar) - a distinct
            // script name, since a dynamically-typed surface cannot pick between them by value.
            .Method<static_cast<Float3 (*)(Float3, f32)>(&operator*)>("Mul").OverloadedName("MulScalar")
            .Operator(MethodOperator::Multiply)
            .Method<static_cast<Float3 (*)(Float3, Float3)>(&operator/)>("Div")
            .Operator(MethodOperator::Divide)
            .Method<static_cast<Float3 (*)(Float3, f32)>(&operator/)>("Div").OverloadedName("DivScalar")
            .Operator(MethodOperator::Divide)
            .Method<&Negated<Float3>>("Neg", {"v"})
            .Operator(MethodOperator::Negate)
            .Method<&EqualValues<Float3>>("Equals", {"a", "b"})
            .Operator(MethodOperator::Equals)
            .Constructor()
            .Constructor<f32, f32, f32>();
    }

    REFLECT_VALUE(Float4, "rtti::core")
    {
        builder.Property<&Float4::x>("x")
            .Property<&Float4::y>("y")
            .Property<&Float4::z>("z")
            .Property<&Float4::w>("w")
            .Constant("Zero", Float4::Zero)
            .Constant("One", Float4::One)
            .Method<&Float4::XYZ>("XYZ")
            .Method<static_cast<Float4 (*)(Float4, Float4)>(&operator+)>("Add", {"a", "b"})
            .Operator(MethodOperator::Add)
            .Method<static_cast<Float4 (*)(Float4, Float4)>(&operator-)>("Sub", {"a", "b"})
            .Operator(MethodOperator::Subtract)
            .Method<static_cast<Float4 (*)(Float4, f32)>(&operator*)>("Mul").OverloadedName("MulScalar")
            .Operator(MethodOperator::Multiply)
            .Method<&Negated<Float4>>("Neg", {"v"})
            .Operator(MethodOperator::Negate)
            .Method<&EqualValues<Float4>>("Equals", {"a", "b"})
            .Operator(MethodOperator::Equals)
            .Constructor()
            .Constructor<f32, f32, f32, f32>();
    }

    REFLECT_VALUE(Color, "rtti::core")
    {
        builder.Property<&Color::r>("r")
            .Property<&Color::g>("g")
            .Property<&Color::b>("b")
            .Property<&Color::a>("a")
            .Constant("White", Color::White)
            .Constant("Black", Color::Black)
            .Constant("Red", Color::Red)
            .Constant("Green", Color::Green)
            .Constant("Blue", Color::Blue)
            .Constant("Transparent", Color::Transparent)
            .Method<&Color::ToRGBA8>("ToRGBA8")     // const member
            .Method<&Color::FromRGBA8>("FromRGBA8") // static factory
            .Method<static_cast<Color (*)(Color, Color)>(&operator+)>("Add", {"a", "b"})
            .Operator(MethodOperator::Add)
            .Method<static_cast<Color (*)(Color, f32)>(&operator*)>("Mul").OverloadedName("MulScalar")
            .Operator(MethodOperator::Multiply)
            .Method<&EqualValues<Color>>("Equals", {"a", "b"})
            .Operator(MethodOperator::Equals)
            .Constructor()
            .Constructor<f32, f32, f32, f32>();
    }

    REFLECT_VALUE(Quaternion, "rtti::core")
    {
        builder.Property<&Quaternion::x>("x")
            .Property<&Quaternion::y>("y")
            .Property<&Quaternion::z>("z")
            .Property<&Quaternion::w>("w")
            .Constant("Identity", Quaternion::Identity)
            .Method<&Quaternion::FromAxisAngle>("FromAxisAngle", {"axis", "radians"}) // static factory
            .Method<static_cast<Quaternion (*)(f32, f32, f32)>(&FromYawPitchRoll)>(
                "FromYawPitchRoll", {"yaw", "pitch", "roll"})
            .Method<static_cast<Quaternion (*)(Quaternion, Quaternion)>(&operator*)>("Mul", {"a", "b"})
            .Operator(MethodOperator::Multiply) // composes in the engine's order
            .Method<static_cast<Float3 (*)(Quaternion, Float3)>(&RotateVector)>("RotateVector",
                                                                               {"q", "v"})
            .Method<static_cast<Quaternion (*)(Quaternion)>(&Normalized)>("Normalized", {"q"})
            .Method<static_cast<Quaternion (*)(Quaternion)>(&Conjugate)>("Conjugate", {"q"})
            .Method<static_cast<Quaternion (*)(Quaternion)>(&Inverse)>("Inverse", {"q"})
            .Method<static_cast<Quaternion (*)(Quaternion, Quaternion, f32)>(&Slerp)>(
                "Slerp", {"a", "b", "t"})
            .Constructor()
            .Constructor<f32, f32, f32, f32>();
    }

    REFLECT_VALUE(Transform, "rtti::core")
    {
        builder.Property<&Transform::position>("position")
            .Property<&Transform::rotation>("rotation")
            .Property<&Transform::scale>("scale")
            .Method<&Transform::ToMatrix>("ToMatrix")
            .Constructor();
    }

    // Matrices: no properties (element access is via the container facility,
    // registered separately); reflect the key static/free operations.
    REFLECT_VALUE(Float4x4, "rtti::core")
    {
        builder.Method<&Float4x4::Identity>("Identity")
            .Method<static_cast<Float4x4 (*)(const Float4x4&, const Float4x4&)>(&operator*)>("Mul")
            .Method<static_cast<f32 (*)(const Float4x4&)>(&Determinant)>("Determinant")
            .Method<static_cast<Float4x4 (*)(const Float4x4&)>(&Transpose)>("Transpose")
            .Method<static_cast<Float4x4 (*)(const Float4x4&)>(&Inverse)>("Inverse");
    }

    REFLECT_VALUE(Float3x3, "rtti::core")
    {
        builder.Method<&Float3x3::Identity>("Identity")
            .Method<static_cast<Float3x3 (*)(const Float3x3&, const Float3x3&)>(&operator*)>("Mul")
            .Method<static_cast<f32 (*)(const Float3x3&)>(&Determinant)>("Determinant")
            .Method<static_cast<Float3x3 (*)(const Float3x3&)>(&Transpose)>("Transpose")
            .Method<static_cast<Float3x3 (*)(const Float3x3&)>(&Inverse)>("Inverse");
    }

    // Scalar math: the free functions in foundation::core reflected as STATICS on the empty `Math`
    // anchor type, so scripts reach trig/roots/interpolation as `Math.Sin(x)` etc. There is no
    // reflecting a namespace, hence the anchor. Constants ride along as reflected constants.
    REFLECT_VALUE(Math, "rtti::core")
    {
        builder.Method<static_cast<f32 (*)(f32)>(&Abs)>("Abs", {"x"})
            .Method<&Sqrt>("Sqrt", {"x"})
            .Method<&Sin>("Sin", {"x"})
            .Method<&Cos>("Cos", {"x"})
            .Method<&Tan>("Tan", {"x"})
            .Method<&Asin>("Asin", {"x"})
            .Method<&Acos>("Acos", {"x"})
            .Method<&Atan2>("Atan2", {"y", "x"})
            .Method<&Floor>("Floor", {"x"})
            .Method<&Ceil>("Ceil", {"x"})
            .Method<&Round>("Round", {"x"})
            .Method<&Pow>("Pow", {"base", "exp"})
            .Method<&Log>("Log", {"x"})
            .Method<&Exp>("Exp", {"x"})
            .Method<&DegreesToRadians>("DegreesToRadians", {"degrees"})
            .Method<&RadiansToDegrees>("RadiansToDegrees", {"radians"})
            .Method<static_cast<f32 (*)(f32, f32, f32)>(&Lerp)>("Lerp", {"a", "b", "t"})
            .Constant("Pi", kPi)
            .Constant("TwoPi", kTwoPi)
            .Constant("HalfPi", kHalfPi)
            .Constant("DegToRad", kDegToRad)
            .Constant("RadToDeg", kRadToDeg);
    }

    REFLECT_VALUE(AABB, "rtti::core")
    {
        builder.Property<&AABB::min>("min")
            .Property<&AABB::max>("max")
            .Method<&AABB::Center>("Center")
            .Method<&AABB::Contains>("Contains")
            .Constructor()
            .Constructor<Float3, Float3>();
    }

    REFLECT_VALUE(Plane, "rtti::core")
    {
        builder.Property<&Plane::normal>("normal")
            .Property<&Plane::d>("d")
            .Method<&Plane::SignedDistance>("SignedDistance")
            .Constructor()
            .Constructor<Float3, f32>();
    }

    REFLECT_VALUE(Rectangle, "rtti::core")
    {
        builder.Property<&Rectangle::x>("x")
            .Property<&Rectangle::y>("y")
            .Property<&Rectangle::width>("width")
            .Property<&Rectangle::height>("height")
            .Constructor()
            .Constructor<f32, f32, f32, f32>();
    }

    REFLECT_VALUE(Guid, "rtti::core")
    {
        builder.Property<&Guid::high>("high")
            .Property<&Guid::low>("low")
            .Constant("Nil", Guid::Nil)
            .Method<&Guid::IsNil>("IsNil")
            // a == b in a script: which asset is this, is that the track already playing.
            .Method<&EqualValues<Guid>>("Equals", {"a", "b"})
            .Operator(MethodOperator::Equals)
            .Constructor()
            .Constructor<u64, u64>()
            // Guid("ac96b003-5b7c-...") for scripts/editor text documents. Declared as String
            // (not StringView) so the script backends' type mapping surfaces it as `string`;
            // the C++ side lands on the StringView ctor via String's implicit view conversion.
            .Constructor<String>({"text"});
    }

    // Public Core enums (scripting-relevant). Internal enums (PropertyFlags,
    // HashMap::State, BufferedStream::Mode) are deliberately not reflected.
    REFLECT_ENUM(LogLevel, "rtti::core")
    {
        builder.Value("Trace", LogLevel::Trace)
            .Value("Debug", LogLevel::Debug)
            .Value("Info", LogLevel::Info)
            .Value("Warning", LogLevel::Warning)
            .Value("Error", LogLevel::Error)
            .Value("Fatal", LogLevel::Fatal)
            .Value("Off", LogLevel::Off);
    }

    REFLECT_ENUM(ErrorCode, "rtti::core")
    {
        builder.Value("Ok", ErrorCode::Ok)
            .Value("Unknown", ErrorCode::Unknown)
            .Value("InvalidArgument", ErrorCode::InvalidArgument)
            .Value("OutOfRange", ErrorCode::OutOfRange)
            .Value("OutOfMemory", ErrorCode::OutOfMemory)
            .Value("NotFound", ErrorCode::NotFound)
            .Value("NotSupported", ErrorCode::NotSupported)
            .Value("AlreadyExists", ErrorCode::AlreadyExists)
            .Value("Internal", ErrorCode::Internal);
    }

    REFLECT_ENUM(SerializeMode, "rtti::core")
    {
        builder.Value("Read", SerializeMode::Read).Value("Write", SerializeMode::Write);
    }

    REFLECT_ENUM(FileMode, "rtti::core")
    {
        builder.Value("Read", FileMode::Read)
            .Value("Write", FileMode::Write)
            .Value("ReadWrite", FileMode::ReadWrite)
            .Value("Append", FileMode::Append);
    }

    // How fixed-size content fits a region of another shape (a game's render resolution in its
    // window, the Game tab's viewport): a setting the project and its export presets carry.
    REFLECT_ENUM(FitMode, "rtti::core")
    {
        builder.Value("Stretch", FitMode::Stretch)
            .Value("Letterbox", FitMode::Letterbox)
            .Value("Crop", FitMode::Crop)
            .Value("IntegerScale", FitMode::IntegerScale);
    }

    REFLECT_ENUM(SeekOrigin, "rtti::core")
    {
        builder.Value("Begin", SeekOrigin::Begin)
            .Value("Current", SeekOrigin::Current)
            .Value("End", SeekOrigin::End);
    }
}

namespace foundation::core
{
    // Registers all Core value types for reflection (patches each TypeOf<T>())
    // and adds them to the GlobalTypeRegistry. Idempotent; call once at startup.
    // Declared (exported) in the :core_reflection interface partition.
    // ONCE and thread-safe (a function-local static's initializer runs exactly once and blocks
    // concurrent callers until it finishes). Every cook worker calls this per script build: two
    // builds in flight rebuilt the enum tables at once (EnumBuilder::Build moves a fresh array
    // over the live one) and double-freed them - Tools.Cook aborted on any project with two
    // script assets. Same idiom as RegisterScriptFacadeReflection.
    void RegisterCoreTypes()
    {
        static const bool once = []()
        {
        RttiRegisterValue_Float2();
        GlobalTypeRegistry().Register(TypeOf<Float2>());
        RttiRegisterValue_Float3();
        GlobalTypeRegistry().Register(TypeOf<Float3>());
        RttiRegisterValue_Float4();
        GlobalTypeRegistry().Register(TypeOf<Float4>());
        RttiRegisterValue_Color();
        GlobalTypeRegistry().Register(TypeOf<Color>());
        RttiRegisterValue_Quaternion();
        GlobalTypeRegistry().Register(TypeOf<Quaternion>());
        RttiRegisterValue_Transform();
        GlobalTypeRegistry().Register(TypeOf<Transform>());
        RttiRegisterValue_Float4x4();
        GlobalTypeRegistry().Register(TypeOf<Float4x4>());
        RttiRegisterValue_Float3x3();
        GlobalTypeRegistry().Register(TypeOf<Float3x3>());
        RttiRegisterValue_Math();
        GlobalTypeRegistry().Register(TypeOf<Math>());
        RegisterMatrixElements<Float4x4, 4>(); // flat element access (after the patch above)
        RegisterMatrixElements<Float3x3, 3>();
        RttiRegisterValue_AABB();
        GlobalTypeRegistry().Register(TypeOf<AABB>());
        RttiRegisterValue_Plane();
        GlobalTypeRegistry().Register(TypeOf<Plane>());
        RttiRegisterValue_Rectangle();
        GlobalTypeRegistry().Register(TypeOf<Rectangle>());
        RttiRegisterValue_Guid();
        GlobalTypeRegistry().Register(TypeOf<Guid>());

        // Free-standing (namespace-level) math constants.
        ConstantRegistry& constants = GlobalConstantRegistry();
        const TypeInfo* f32Type = &TypeOf<f32>();
        constants.Register("rtti::core", "kPi", f32Type, Variant::From<f32>(kPi));
        constants.Register("rtti::core", "kTwoPi", f32Type, Variant::From<f32>(kTwoPi));
        constants.Register("rtti::core", "kHalfPi", f32Type, Variant::From<f32>(kHalfPi));
        constants.Register("rtti::core", "kInvPi", f32Type, Variant::From<f32>(kInvPi));
        constants.Register("rtti::core", "kDegToRad", f32Type, Variant::From<f32>(kDegToRad));
        constants.Register("rtti::core", "kRadToDeg", f32Type, Variant::From<f32>(kRadToDeg));
        constants.Register("rtti::core", "kEpsilon", f32Type, Variant::From<f32>(kEpsilon));
        constants.Register("rtti::core", "kFloatMax", f32Type, Variant::From<f32>(kFloatMax));

        // Public enums (patch TypeOf<E>() with enumerators, then register).
        RttiRegisterEnum_LogLevel();
        GlobalTypeRegistry().Register(TypeOf<LogLevel>());
        RttiRegisterEnum_ErrorCode();
        GlobalTypeRegistry().Register(TypeOf<ErrorCode>());
        RttiRegisterEnum_SerializeMode();
        GlobalTypeRegistry().Register(TypeOf<SerializeMode>());
        RttiRegisterEnum_FileMode();
        GlobalTypeRegistry().Register(TypeOf<FileMode>());
        RttiRegisterEnum_SeekOrigin();
        GlobalTypeRegistry().Register(TypeOf<SeekOrigin>());
        RttiRegisterEnum_FitMode();
        GlobalTypeRegistry().Register(TypeOf<FitMode>());
            return true;
        }();
        (void)once;
    }
}
