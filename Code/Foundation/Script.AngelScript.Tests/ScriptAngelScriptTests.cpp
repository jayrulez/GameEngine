// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Script.AngelScript tests: the reflected-type EMISSION
// suite in AngelScript syntax (the battery certifies the context contract;
// emission is certified per backend), the shared conformance battery, and the
// backend-registry integration (both languages resolving side by side).
//
// AngelScript has no top-level statements, so scripts follow the backend's load
// convention: module globals + a `void main()` that Load runs after Build.

#include <doctest/doctest.h>

#include <cstring> // std::strcmp (alias-name assertions)

#include "Core/Prelude.h" // <new> reachability for reflection containers (GCC)
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.script;
import foundation.script.angelscript;
#ifdef OPTION_HAS_LUAU
import foundation.script.luau;
#endif

using namespace foundation::core;
using namespace foundation::script;

// A reflected Object-derived type to exercise object classes in AngelScript.
namespace
{
    class Widget : public Object
    {
        RTTI_OBJECT(Widget, Object)
    public:
        int id = 0;
        int serial = 7; // published read-only: a script reads it, never assigns it
        int doubled() const { return id * 2; }
        int idOf(Widget* other) const { return other != nullptr ? other->id : -1; }
    };
}

REFLECT_MEMBERS(Widget, "rtti::script::test")
{
    builder.Property<&Widget::id>("id");
    builder.Property<&Widget::serial>("serial", PropertyFlags::ReadOnly);
    builder.Method<&Widget::doubled>("doubled");
    builder.Method<&Widget::idOf>("idOf");
    builder.Constructor();
}

// Exercises natural-typed numeric facades: a 64-bit integer argument + return must round-trip
// EXACTLY, not funnel through double (which would corrupt values above 2^53). 9007199254740993
// is 2^53 + 1 - the smallest integer a double cannot represent, so it discriminates the fixed
// integer-exact marshalling from the old double currency.
namespace
{
    class NumProbe : public Object
    {
        RTTI_OBJECT(NumProbe, Object)
    public:
        i64 lastI64 = 0;
        i32 lastI32 = 0;
        void takeI64(i64 v) { lastI64 = v; }
        void takeI32(i32 v) { lastI32 = v; }
        bool argWasExact() const { return lastI64 == 9007199254740993LL; } // 2^53 + 1
        i32 echoI32() const { return lastI32; }
        i64 bigConst() const { return 9007199254740993LL; } // 2^53 + 1
        // A 9-arg method: exceeds the old 8-arg marshalling cap (kMaxArgs). The trailing args must
        // NOT be truncated - matches facades like DebugDraw.line (6 coords + 3 color = 9 args).
        f32 lastSum9 = 0.0f;
        void take9(f32 a, f32 b, f32 c, f32 d, f32 e, f32 f, f32 g, f32 h, f32 i)
        {
            lastSum9 = a + b + c + d + e + f + g + h + i;
        }
        [[nodiscard]] f32 sum9() const { return lastSum9; }
        // A 16-arg method: the TOP of the raised marshalling cap (kMaxArgs = 16). The last arg is
        // the one a cap-boundary off-by-one would drop.
        f32 lastSum16 = 0.0f;
        void take16(f32 a, f32 b, f32 c, f32 d, f32 e, f32 f, f32 g, f32 h, f32 i, f32 j, f32 k,
                    f32 l, f32 m, f32 n, f32 o, f32 p)
        {
            lastSum16 = a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p;
        }
        [[nodiscard]] f32 sum16() const { return lastSum16; }
    };
}

REFLECT_MEMBERS(NumProbe, "rtti::script::test")
{
    builder.Method<&NumProbe::takeI64>("takeI64");
    builder.Method<&NumProbe::takeI32>("takeI32");
    builder.Method<&NumProbe::argWasExact>("argWasExact");
    builder.Method<&NumProbe::echoI32>("echoI32");
    builder.Method<&NumProbe::bigConst>("bigConst");
    builder.Method<&NumProbe::take9>("take9",
                                     {"a", "b", "c", "d", "e", "f", "g", "h", "i"});
    builder.Method<&NumProbe::sum9>("sum9");
    builder.Method<&NumProbe::take16>("take16", {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j",
                                                 "k", "l", "m", "n", "o", "p"});
    builder.Method<&NumProbe::sum16>("sum16");
    builder.Constructor();
}

// The ScriptName alias mechanism: a type declares a "scriptName" class attribute and binds to scripts
// under THAT, not its C++ class name. AliasProbe binds as `aka`; AliasClash also claims `aka` (to
// exercise the collision guard). The native TYPE name + registry lookup stay on the C++ name.
namespace
{
    class AliasProbe : public Object
    {
        RTTI_OBJECT(AliasProbe, Object)
    public:
        static i32 answer() { return 42; }
    };
    class AliasClash : public Object
    {
        RTTI_OBJECT(AliasClash, Object)
    public:
    };
}

REFLECT_MEMBERS(AliasProbe, "rtti::script::test")
{
    builder.Attribute("scriptName", "aka"); // bind to scripts as `aka`, not `AliasProbe`
    builder.Method<&AliasProbe::answer>("answer");
    builder.Constructor();
}
REFLECT_MEMBERS(AliasClash, "rtti::script::test")
{
    builder.Attribute("scriptName", "aka"); // deliberately the SAME script name as AliasProbe
    builder.Constructor();
}

TEST_CASE("script: ScriptTypeName resolves the alias; FindScriptTypeNameCollision guards duplicates")
{
    // ScriptTypeName: the "scriptName" alias when set, else the C++ name.
    CHECK(std::strcmp(ScriptTypeName(AliasProbe::StaticType()), "aka") == 0);
    CHECK(std::strcmp(ScriptTypeName(Widget::StaticType()), "Widget") == 0); // no alias -> C++ name

    // NATIVE identity is untouched: the C++ type name (serialization + registry FindByName) is unaliased.
    CHECK(std::strcmp(AliasProbe::StaticType().name, "AliasProbe") == 0);

    // A clean set: distinct script names -> no collision.
    const TypeInfo* clean[] = {&AliasProbe::StaticType(), &Widget::StaticType()};
    CHECK(FindScriptTypeNameCollision(Span<const TypeInfo* const>{clean, 2}) == nullptr);

    // A colliding set: two types bind to the same script name `aka` -> reported (FinalizeTypes traps).
    const TypeInfo* clash[] = {&AliasProbe::StaticType(), &AliasClash::StaticType()};
    const char* hit = FindScriptTypeNameCollision(Span<const TypeInfo* const>{clash, 2});
    REQUIRE(hit != nullptr);
    CHECK(std::strcmp(hit, "aka") == 0);
}

TEST_CASE("angelscript: a scriptName alias binds the class under the alias, not the C++ name")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(AliasProbe::StaticType());
    manager->FinalizeTypes();

    RefPtr<IScriptContext> ctx = manager->CreateContext();
    const Status ok = ctx->Load(u8"double Got = 0;\n"
                                u8"void main() { Got = double(aka::answer()); }\n", // the ALIAS
                                u8"m");
    REQUIRE(ok.IsOk());
    CHECK(ctx->GetGlobal(u8"Got").Get<f64>() == 42.0);

    // The C++ name is NOT a script type (the alias replaces it on the surface): referencing it fails.
    RefPtr<IScriptContext> ctx2 = manager->CreateContext();
    const Status bad =
        ctx2->Load(u8"void main() { AliasProbe::answer(); }\n", u8"m2"); // C++ name -> undeclared
    CHECK_FALSE(bad.IsOk());
}

// A reflected Object with a property but NO reflected constructor, plus a factory that returns one:
// AngelScript declares + binds a type's properties independent of any factory, so a constructor-less
// handle handed back from a facade method is fully usable (the collections-lift precondition).
namespace
{
    class Leaf : public Object
    {
        RTTI_OBJECT(Leaf, Object)
    public:
        int value = 0;
    };

    class LeafFactory : public Object
    {
        RTTI_OBJECT(LeafFactory, Object)
    public:
        RefPtr<Leaf> make() const { return MakeRef<Leaf>(DefaultAllocator()); }
    };
}

REFLECT_MEMBERS(Leaf, "rtti::script::test")
{
    builder.Property<&Leaf::value>("value"); // deliberately no Constructor()
}
REFLECT_MEMBERS(LeafFactory, "rtti::script::test")
{
    builder.Method<&LeafFactory::make>("make");
    builder.Constructor();
}

// A polymorphic-container owner (Zoo holds Array<RefPtr<Animal>>); Dog/Cat are the concrete
// elements. Exercises container-member binding (count / at / add-by-name / removeAt) in AngelScript,
// where at/add return the element base handle (Animal@) and the concrete object is boxed inside.
namespace
{
    class Animal : public Object
    {
        RTTI_OBJECT(Animal, Object)
    public:
        int legs = 4;
    };
    class Dog : public Animal
    {
        RTTI_OBJECT(Dog, Animal)
    public:
        int barks = 1;
    };
    class Cat : public Animal
    {
        RTTI_OBJECT(Cat, Animal)
    public:
        int meows = 1;
    };
    RefPtr<Animal> CreateAnimal(const TypeInfo& t)
    {
        if (t.id == Dog::StaticType().id)
        {
            return MakeRef<Dog>(DefaultAllocator());
        }
        if (t.id == Cat::StaticType().id)
        {
            return MakeRef<Cat>(DefaultAllocator());
        }
        return RefPtr<Animal>{};
    }
    bool CanCreateAnimal(const TypeInfo& t)
    {
        return t.id == Dog::StaticType().id || t.id == Cat::StaticType().id;
    }
    class Zoo : public Object
    {
        RTTI_OBJECT(Zoo, Object)
    public:
        Array<RefPtr<Animal>> animals;
    };
}

REFLECT_MEMBERS(Animal, "rtti::script::test")
{
    builder.Property<&Animal::legs>("legs");
}
REFLECT_MEMBERS(Dog, "rtti::script::test")
{
    builder.Property<&Dog::barks>("barks");
}
REFLECT_MEMBERS(Cat, "rtti::script::test")
{
    builder.Property<&Cat::meows>("meows");
}
REFLECT_MEMBERS(Zoo, "rtti::script::test")
{
    builder.Nested<&Zoo::animals>("animals");
    builder.Constructor();
}

namespace
{
    void RegisterZoo(IScriptManager& manager)
    {
        static bool once = [] {
            RegisterPolymorphicArrayType<Animal>(&CreateAnimal, &CanCreateAnimal);
            GlobalTypeRegistry().Register(Animal::StaticType());
            GlobalTypeRegistry().Register(Dog::StaticType());
            GlobalTypeRegistry().Register(Cat::StaticType());
            GlobalTypeRegistry().Register(Zoo::StaticType());
            return true;
        }();
        (void)once;
        manager.RegisterType(Animal::StaticType());
        manager.RegisterType(Dog::StaticType());
        manager.RegisterType(Cat::StaticType());
        manager.RegisterType(Zoo::StaticType());
    }
}

// Non-Object VALUE type reached only by address: House has a nested-value member, Shelf a homogeneous
// Array<UniquePtr<Room>>. Both need borrow-mode handles (unit 2b).
namespace
{
    struct Room
    {
        int size = 0;
    };
    class House : public Object
    {
        RTTI_OBJECT(House, Object)
    public:
        Room room;
    };
    class Shelf : public Object
    {
        RTTI_OBJECT(Shelf, Object)
    public:
        Array<UniquePtr<Room>> rooms;
    };
}
REFLECT_VALUE(Room, "rtti::script::test")
{
    builder.Property<&Room::size>("size");
}
REFLECT_MEMBERS(House, "rtti::script::test")
{
    builder.Nested<&House::room>("room");
    builder.Constructor();
}
REFLECT_MEMBERS(Shelf, "rtti::script::test")
{
    builder.Nested<&Shelf::rooms>("rooms");
    builder.Constructor();
}
namespace
{
    void RegisterHouse(IScriptManager& manager)
    {
        static bool once = [] {
            RttiRegisterValue_Room();
            RegisterUniquePtrArrayType<Room>();
            return true;
        }();
        (void)once;
        manager.RegisterType(TypeOf<Room>());
        manager.RegisterType(House::StaticType());
        manager.RegisterType(Shelf::StaticType());
    }
}

// A facade that returns an engine Array<T> renders as a native `array<T>`:
// a numeric array (scalar element path) and a reflected-value array (Room boxes as a handle element,
// the same flavor as Entity in the physics facades).
namespace
{
    class Bag : public Object
    {
        RTTI_OBJECT(Bag, Object)
    public:
        Array<i32> numbers() const
        {
            Array<i32> out;
            out.PushBack(10);
            out.PushBack(20);
            out.PushBack(30);
            return out;
        }
        Array<Room> rooms() const
        {
            Array<Room> out;
            Room a;
            a.size = 3;
            Room b;
            b.size = 4;
            out.PushBack(a);
            out.PushBack(b);
            return out;
        }
        Array<i32> empty() const { return {}; }
        Array<String> names() const
        {
            Array<String> out;
            out.PushBack(String(u8"ab"));
            out.PushBack(String(u8"cde"));
            return out;
        }
        // Lists handed IN by a script (array parameters).
        f32 sum(Array<f32> values) const
        {
            f32 total = 0.0f;
            for (f32 v : values)
            {
                total += v;
            }
            return total;
        }
        i32 letters(Array<String> words) const
        {
            i32 total = 0;
            for (const String& w : words)
            {
                total += static_cast<i32>(w.Size());
            }
            return total;
        }
        Array<f32> doubled(Array<f32> values) const
        {
            for (f32& v : values)
            {
                v *= 2.0f;
            }
            return values;
        }
    };
}
REFLECT_MEMBERS(Bag, "rtti::script::test")
{
    builder.Method<&Bag::numbers>("numbers");
    builder.Method<&Bag::rooms>("rooms");
    builder.Method<&Bag::empty>("empty");
    builder.Method<&Bag::names>("names");
    builder.Method<&Bag::sum>("sum", {"values"});
    builder.Method<&Bag::letters>("letters", {"words"});
    builder.Method<&Bag::doubled>("doubled", {"values"});
    builder.Constructor();
}
namespace
{
    void RegisterBag(IScriptManager& manager)
    {
        static bool once = [] {
            RttiRegisterValue_Room();
            RegisterArrayType<i32>();
            RegisterArrayType<Room>();
            RegisterArrayType<String>();
            RegisterArrayType<f32>();
            return true;
        }();
        (void)once;
        manager.RegisterType(TypeOf<Room>());
        manager.RegisterType(Bag::StaticType());
    }
}

TEST_CASE("angelscript: a facade Array<T> return crosses as a native array<T> (numeric + value element)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterBag(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    const Status status = ctx->Load(u8"double N; double R; double E; double L;\n"
                                    u8"void main() {\n"
                                    u8"  Bag b;\n"
                                    u8"  array<int>@ ns = b.numbers();\n"
                                    u8"  N = ns.length() + ns[0] + ns[1] + ns[2];\n" // 3 + 60 = 63
                                    u8"  array<Room@>@ rs = b.rooms();\n"
                                    u8"  R = rs.length() + rs[0].size + rs[1].size;\n" // 2 + 7 = 9
                                    u8"  E = b.empty().length();\n"                    // 0
                                    u8"  array<string>@ ss = b.names();\n"
                                    u8"  L = ss.length() + ss[0].length() + ss[1].length();\n" // 2+2+3=7
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"N").Get<f64>() == 63.0);
    CHECK(ctx->GetGlobal(u8"R").Get<f64>() == 9.0);
    CHECK(ctx->GetGlobal(u8"E").Get<f64>() == 0.0);
    CHECK(ctx->GetGlobal(u8"L").Get<f64>() == 7.0);
}

TEST_CASE("angelscript: a script array crosses into a facade Array<T> parameter")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterBag(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    const Status status = ctx->Load(u8"double S; double L; double E; double D;\n"
                                    u8"void main() {\n"
                                    u8"  Bag b;\n"
                                    u8"  array<float> xs = {1.5f, 2.25f, -0.75f};\n"
                                    u8"  S = b.sum(xs);\n"                                // 3.0
                                    u8"  array<string> ws = {\"ab\", \"cde\"};\n"
                                    u8"  L = b.letters(ws);\n"                            // 5
                                    u8"  array<float> none;\n"
                                    u8"  E = b.sum(none);\n"                              // 0
                                    u8"  array<float>@ twice = b.doubled(xs);\n"
                                    u8"  D = twice.length() + twice[0] + twice[2] + xs[0];\n" // 3 + 3 - 1.5 + 1.5 = 6
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"S").Get<f64>() == doctest::Approx(3.0));
    CHECK(ctx->GetGlobal(u8"L").Get<f64>() == 5.0);
    CHECK(ctx->GetGlobal(u8"E").Get<f64>() == 0.0);
    CHECK(ctx->GetGlobal(u8"D").Get<f64>() == doctest::Approx(6.0)); // the script's own array unchanged
}

TEST_CASE("angelscript: a nested-value member is a borrow handle edited in place (unit 2b)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterHouse(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    const Status status = ctx->Load(u8"double S;\n"
                                    u8"void main() {\n"
                                    u8"  House h;\n"
                                    u8"  Room@ r = h.room;\n" // borrow over the House's Room subobject
                                    u8"  r.size = 7;\n"
                                    u8"  S = h.room.size;\n" // re-fetched borrow sees the write
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"S").Get<f64>() == 7.0);
}

TEST_CASE("angelscript: a UniquePtr (non-Object) container element is a borrow handle (unit 2b)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterHouse(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    const Status status = ctx->Load(u8"double C; double Z;\n"
                                    u8"void main() {\n"
                                    u8"  Shelf s;\n"
                                    u8"  Room@ room = s.rooms_add();\n" // default-constructs + borrows
                                    u8"  room.size = 42;\n"
                                    u8"  C = s.rooms_count();\n"
                                    u8"  Z = s.rooms_at(0).size;\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"C").Get<f64>() == 1.0);
    CHECK(ctx->GetGlobal(u8"Z").Get<f64>() == 42.0);
}

TEST_CASE("angelscript: a polymorphic container member binds as script ops (count/at/add/removeAt)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterZoo(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    const Status status = ctx->Load(u8"double C0; double C1; double L; double C2; double C3;\n"
                                    u8"void main() {\n"
                                    u8"  Zoo z;\n"
                                    u8"  C0 = z.animals_count();\n"        // 0
                                    u8"  Animal@ d = z.animals_add(\"Dog\");\n"
                                    u8"  d.legs = 3;\n"
                                    u8"  C1 = z.animals_count();\n"        // 1
                                    u8"  L = z.animals_at(0).legs;\n"      // 3 (write-through)
                                    u8"  z.animals_add(\"Cat\");\n"
                                    u8"  C2 = z.animals_count();\n"        // 2
                                    u8"  z.animals_removeAt(0);\n"
                                    u8"  C3 = z.animals_count();\n"        // 1
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"C0").Get<f64>() == 0.0);
    CHECK(ctx->GetGlobal(u8"C1").Get<f64>() == 1.0);
    CHECK(ctx->GetGlobal(u8"L").Get<f64>() == 3.0);
    CHECK(ctx->GetGlobal(u8"C2").Get<f64>() == 2.0);
    CHECK(ctx->GetGlobal(u8"C3").Get<f64>() == 1.0);
}

TEST_CASE("angelscript: a constructor-less reflected type is usable as a returned handle")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(Leaf::StaticType());        // no ctor - declared + props bound anyway
    manager->RegisterType(LeafFactory::StaticType()); // hands a Leaf@ back
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    const Status status = ctx->Load(u8"double V;\n"
                                    u8"void main() {\n"
                                    u8"  LeafFactory f;\n"
                                    u8"  Leaf@ leaf = f.make();\n"
                                    u8"  leaf.value = 7;\n"
                                    u8"  V = leaf.value;\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"V").Get<f64>() == 7.0);
}

TEST_CASE("angelscript: reflected value types support value assignment (Float3 p = expr)")
{
    // All reflected types register as asOBJ_REF boxes; without a registered opAssign, `Float3 p = q;`
    // failed with "no appropriate opAssign". The generic opAssign copies the boxed value.
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    REQUIRE(static_cast<bool>(ctx));

    const Status status =
        ctx->Load(u8"double X; double Y; double Z;\n"
                  u8"void main() {\n"
                  u8"  Float3 a = Float3(1, 2, 3);\n" // copy-init from a factory return (opAssign)
                  u8"  Float3 b = a;\n"               // value assignment from another local
                  u8"  X = b.x;\n"
                  u8"  b = Float3(7, 8, 9);\n" // re-assign; must NOT alias a
                  u8"  Y = b.y;\n"
                  u8"  Z = a.x;\n" // a stays 1 (value copy, not a shared handle)
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"X").Get<f64>() == 1.0);
    CHECK(ctx->GetGlobal(u8"Y").Get<f64>() == 8.0);
    CHECK(ctx->GetGlobal(u8"Z").Get<f64>() ==
          1.0); // value semantics: a not mutated by b's reassign
}

// Sedulous 22a73e31: the math values' operators, bound from what reflection marks: a + b, v * 2,
// -v, a == b, the compound p += v, and a quaternion product composing in the engine's order.
TEST_CASE("angelscript: reflected operators - a + b, v * 2.0f, -v, a == b, p += v, q * r")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    REQUIRE(static_cast<bool>(ctx));

    const Status status =
        ctx->Load(u8"double SumY; double ScaledZ; double NegX; bool Same; bool Differ;\n"
                  u8"double MovedX; double HalfY; double TwoD; double QW; double Hue;\n"
                  u8"void main() {\n"
                  u8"  Float3 a = Float3(1, 2, 3);\n"
                  u8"  Float3 b = Float3(4, 5, 6);\n"
                  u8"  Float3 sum = a + b;\n"
                  u8"  SumY = sum.y;\n"
                  u8"  ScaledZ = (a * 2.0f).z;\n"
                  u8"  NegX = (-a).x;\n"
                  u8"  Same = a == Float3(1, 2, 3);\n"
                  u8"  Differ = a != b;\n"
                  u8"  Float3 p = a;\n"
                  u8"  p += b;\n"
                  u8"  p -= Float3(1, 1, 1);\n"
                  u8"  MovedX = p.x;\n"
                  u8"  HalfY = (b / 2.0f).y;\n"
                  u8"  Float2 d = Float2(1, 1) - Float2(3, 4);\n"
                  u8"  TwoD = d.y;\n"
                  u8"  Quaternion q = Quaternion(0, 0, 0, 1) * Quaternion(0, 0, 0, 1);\n"
                  u8"  QW = q.w;\n"
                  u8"  Hue = (Color(0.25f, 0, 0, 1) + Color(0.5f, 0, 0, 0)).r;\n"
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"SumY").Get<f64>() == doctest::Approx(7.0));
    CHECK(ctx->GetGlobal(u8"ScaledZ").Get<f64>() == doctest::Approx(6.0));
    CHECK(ctx->GetGlobal(u8"NegX").Get<f64>() == doctest::Approx(-1.0));
    CHECK(ctx->GetGlobal(u8"Same").Get<bool>());
    CHECK(ctx->GetGlobal(u8"Differ").Get<bool>());
    CHECK(ctx->GetGlobal(u8"MovedX").Get<f64>() == doctest::Approx(4.0)); // 1 + 4 - 1
    CHECK(ctx->GetGlobal(u8"HalfY").Get<f64>() == doctest::Approx(2.5));
    CHECK(ctx->GetGlobal(u8"TwoD").Get<f64>() == doctest::Approx(-3.0));
    CHECK(ctx->GetGlobal(u8"QW").Get<f64>() == doctest::Approx(1.0));
    CHECK(ctx->GetGlobal(u8"Hue").Get<f64>() == doctest::Approx(0.75));
}

TEST_CASE("angelscript: 64-bit integer facade args/returns round-trip exactly (no double funnel)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    REQUIRE(static_cast<bool>(manager));
    manager->RegisterType(NumProbe::StaticType());
    RefPtr<IScriptContext> ctx = manager->CreateContext(); // defensively finalizes
    REQUIRE(static_cast<bool>(ctx));

    // ArgOk: script passes 2^53+1 into an int64 facade param -> C++ must see it exactly.
    // ReturnOk: an int64 facade return equals the same literal, compared in-script (int64==int64).
    // I32Ok: a 32-bit natural-typed param also survives.
    const Status status = ctx->Load(u8"bool ArgOk; bool ReturnOk; bool I32Ok;\n"
                                    u8"void main() {\n"
                                    u8"  NumProbe p;\n"
                                    u8"  p.takeI64(9007199254740993);\n"
                                    u8"  ArgOk = p.argWasExact();\n"
                                    u8"  ReturnOk = (p.bigConst() == 9007199254740993);\n"
                                    u8"  p.takeI32(1234567);\n"
                                    u8"  I32Ok = (p.echoI32() == 1234567);\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(ctx->GetGlobal(u8"ArgOk").Get<bool>() == true);
    CHECK(ctx->GetGlobal(u8"ReturnOk").Get<bool>() == true);
    CHECK(ctx->GetGlobal(u8"I32Ok").Get<bool>() == true);
}

TEST_CASE("angelscript: a reflected method marshals 9 args without truncating the trailing ones")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    REQUIRE(static_cast<bool>(manager));
    manager->RegisterType(NumProbe::StaticType());
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    REQUIRE(static_cast<bool>(ctx));

    // 9 args exceed the old 8-arg marshalling cap (kMaxArgs). Powers of two so any dropped or garbled
    // argument changes the sum: 1+2+4+8+16+32+64+128+256 = 511. Truncation would lose the 256.
    // (This is the bug that made DebugDraw.line - 6 coords + 3 color = 9 args - a silent no-op.)
    const Status status =
        ctx->Load(u8"float S;\n"
                  u8"void main() {\n"
                  u8"  NumProbe p;\n"
                  u8"  p.take9(1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f, 256.0f);\n"
                  u8"  S = p.sum9();\n"
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"S").Get<f64>() == doctest::Approx(511.0));
}

TEST_CASE("angelscript: a reflected method marshals 16 args - the top of the raised cap")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    REQUIRE(static_cast<bool>(manager));
    manager->RegisterType(NumProbe::StaticType());
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    REQUIRE(static_cast<bool>(ctx));

    // 16 args = kMaxArgs exactly (raised from 8). Powers of two so any dropped or garbled argument
    // changes the sum: 1+2+...+32768 = 65535. A boundary off-by-one would lose the trailing 32768.
    const Status status =
        ctx->Load(u8"float S;\n"
                  u8"void main() {\n"
                  u8"  NumProbe p;\n"
                  u8"  p.take16(1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f,\n"
                  u8"           256.0f, 512.0f, 1024.0f, 2048.0f, 4096.0f, 8192.0f,\n"
                  u8"           16384.0f, 32768.0f);\n"
                  u8"  S = p.sum16();\n"
                  u8"}\n",
                  u8"main");
    REQUIRE(status.IsOk());
    CHECK(ctx->GetGlobal(u8"S").Get<f64>() == doctest::Approx(65535.0));
}

TEST_CASE("angelscript: a context runs valid source")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    REQUIRE(static_cast<bool>(manager));

    RefPtr<IScriptContext> ctx = manager->CreateContext();
    REQUIRE(static_cast<bool>(ctx));

    // A real class-based program: define a class, instantiate, call a method.
    const Status status = ctx->Load(u8"class Greeter {\n"
                                    u8"  string name;\n"
                                    u8"  Greeter(string n) { name = n; }\n"
                                    u8"  string greet() { return \"hi \" + name; }\n"
                                    u8"}\n"
                                    u8"string G;\n"
                                    u8"void main() { Greeter g(\"engine\"); G = g.greet(); }\n",
                                    u8"main");
    CHECK(status.IsOk());
    CHECK(ctx->GetGlobal(u8"G").Get<String>() == u8"hi engine");
}

TEST_CASE("angelscript: string concatenates numbers (opAdd from RegisterStdString, no utils needed)")
{
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(static_cast<bool>(ctx));

    // Documents that a HUD can build "Score: 1200" without RegisterStdStringUtils: the primitive
    // opAdd(int64/double) operators ship with RegisterStdString (int widens to int64).
    const Status status = ctx->Load(u8"string C;\n"
                                    u8"void main() {\n"
                                    u8"  int score = 1200;\n"
                                    u8"  double t = 3.5;\n"
                                    u8"  C = \"Score: \" + score + \" (\" + t + \")\";\n"
                                    u8"}\n",
                                    u8"main");
    CHECK(status.IsOk());
    CHECK(ctx->GetGlobal(u8"C").Get<String>() == u8"Score: 1200 (3.5)");
}

TEST_CASE("angelscript: a compile error is reported")
{
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    const Status status = ctx->Load(u8"this is not valid angelscript @#$", u8"main");
    CHECK_FALSE(status.IsOk());
    CHECK(status.Code() == ErrorCode::InvalidArgument);
}

namespace
{
    struct CapturingErrors final : IScriptErrorHandler
    {
        int count = 0;
        ScriptErrorKind lastKind = ScriptErrorKind::Compile;
        String lastMessage;
        i32 lastLine = -1;

        void OnError(const ScriptError& error) override
        {
            ++count;
            lastKind = error.kind;
            lastMessage = String(error.message);
            lastLine = error.line;
        }
    };
}

TEST_CASE("angelscript: errors are surfaced to a handler")
{
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    CapturingErrors errors;
    ctx->SetErrorHandler(&errors);

    // Compile error: kind + non-empty message + a source line.
    CHECK_FALSE(ctx->Load(u8"int = = =", u8"main").IsOk());
    CHECK(errors.count >= 1);
    CHECK(errors.lastKind == ScriptErrorKind::Compile);
    CHECK(!errors.lastMessage.IsEmpty());
    CHECK(errors.lastLine >= 1);

    // Runtime error (via the main() load convention): kind switches to Runtime.
    const int afterCompile = errors.count;
    CHECK_FALSE(
        ctx->Load(u8"void main() { int zero = 0; int boom = 10 / zero; }\n", u8"main").IsOk());
    CHECK(errors.count > afterCompile);
    CHECK(errors.lastKind == ScriptErrorKind::Runtime);
    CHECK(!errors.lastMessage.IsEmpty());

    // Clearing the handler restores default (console) reporting - no more captures.
    ctx->SetErrorHandler(nullptr);
    const int afterRuntime = errors.count;
    CHECK_FALSE(ctx->Load(u8"more @#$ garbage", u8"main").IsOk());
    CHECK(errors.count == afterRuntime);
}

TEST_CASE("angelscript: a runtime error is reported")
{
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    const Status status =
        ctx->Load(u8"void main() { int zero = 0; int boom = 10 / zero; }\n", u8"main");
    CHECK_FALSE(status.IsOk());
    CHECK(status.Code() == ErrorCode::Internal);
}

TEST_CASE("angelscript: a runtime fault in a GLOBAL INITIALIZER is a Runtime error")
{
    // Build both compiles and runs global initializers; the backend classifies a
    // failed init as Runtime (not Compile) by Build's return code.
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    CapturingErrors errors;
    ctx->SetErrorHandler(&errors);
    const Status status = ctx->Load(u8"int Zero() { return 0; }\n"
                                    u8"int boom = 10 / Zero();\n",
                                    u8"main");
    CHECK_FALSE(status.IsOk());
    CHECK(status.Code() == ErrorCode::Internal);
    CHECK(errors.count >= 1);
    CHECK(errors.lastKind == ScriptErrorKind::Runtime);
}

TEST_CASE("angelscript: each context is isolated")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RefPtr<IScriptContext> a = manager->CreateContext();
    RefPtr<IScriptContext> b = manager->CreateContext();
    CHECK(a.Get() != b.Get());
    CHECK(a->Load(u8"double f() { return 1; }\n", u8"main").IsOk());
    CHECK(b->Load(u8"double g() { return 2; }\n", u8"main").IsOk());
    CHECK(a->HasFunction(u8"f"));
    CHECK_FALSE(a->HasFunction(u8"g"));
    CHECK_FALSE(b->HasFunction(u8"f"));
}

TEST_CASE("angelscript: read module globals as Variant")
{
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(ctx->Load(u8"int Answer = 42;\n"
                      u8"string Name = \"engine\";\n"
                      u8"bool Flag = true;\n"
                      u8"double Pi = 3.5;\n",
                      u8"main")
                .IsOk());

    CHECK(ctx->GetGlobal(u8"Answer").Get<f64>() == 42.0); // numbers surface as f64
    CHECK(ctx->GetGlobal(u8"Name").Get<String>() == u8"engine");
    CHECK(ctx->GetGlobal(u8"Flag").Get<bool>() == true);
    CHECK(ctx->GetGlobal(u8"Pi").Get<f64>() == 3.5);

    CHECK(ctx->GetGlobal(u8"Missing").IsEmpty()); // absent -> empty Variant
}

TEST_CASE("angelscript: SetGlobal writes typed module globals")
{
    // AngelScript globals are
    // directly writable - the contract's SetGlobal is real here.
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(ctx->Load(u8"double Speed = 1;\n"
                      u8"string Tag = \"none\";\n"
                      u8"double ReadSpeed() { return Speed; }\n"
                      u8"string ReadTag() { return Tag; }\n",
                      u8"main")
                .IsOk());

    ctx->SetGlobal(u8"Speed", Variant::From(4.5));
    ctx->SetGlobal(u8"Tag", Variant::From(String(u8"fast")));
    CHECK(ctx->Call(u8"ReadSpeed", Span<Variant>{}).Value().Get<f64>() == 4.5);
    CHECK(ctx->Call(u8"ReadTag", Span<Variant>{}).Value().Get<String>() == u8"fast");
}

TEST_CASE("angelscript: a read-only property reads, and an assignment fails to compile - no setter exists")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(Widget::StaticType());
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    REQUIRE(ctx->Load(u8"double S = 0;\n"
                      u8"void main() { Widget@ w = Widget(); S = w.serial; }\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"S").Get<f64>() == 7.0);

    // The assignment is refused by the compiler: the backend registers get_serial only.
    RefPtr<IScriptContext> other = manager->CreateContext();
    CHECK_FALSE(other->Load(u8"void main() { Widget@ w = Widget(); w.serial = 3; }\n", u8"main").IsOk());
}

TEST_CASE("angelscript: reflected value types are usable from script (construct + properties)")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager); // collect + FinalizeTypes (two-phase)
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // Construct a reflected Float3, read and write its properties.
    const Status status = ctx->Load(u8"Float3@ v = Float3(1, 2, 3);\n"
                                    u8"double X = 0;\n"
                                    u8"double Z = 0;\n"
                                    u8"double X2 = 0;\n"
                                    u8"void main() {\n"
                                    u8"  X = v.x;\n"
                                    u8"  Z = v.z;\n"
                                    u8"  v.x = 9;\n"
                                    u8"  X2 = v.x;\n"
                                    u8"}\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(ctx->GetGlobal(u8"X").Get<f64>() == 1.0); // construct + getter
    CHECK(ctx->GetGlobal(u8"Z").Get<f64>() == 3.0);
    CHECK(ctx->GetGlobal(u8"X2").Get<f64>() == 9.0); // setter took effect
}

TEST_CASE("angelscript: call reflected methods (static, instance, struct return, object args)")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // Static method (namespace emission): Float3::Dot(a, b) with object args.
    REQUIRE(ctx->Load(u8"Float3@ a = Float3(1, 2, 3);\n"
                      u8"Float3@ b = Float3(4, 5, 6);\n"
                      u8"double D = 0;\n"
                      u8"void main() { D = Float3::Dot(a, b); }\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"D").Get<f64>() == 32.0);

    // Instance method returning a struct (Float4.XYZ() -> Float3), then read it.
    REQUIRE(ctx->Load(u8"Float4@ v4 = Float4(7, 8, 9, 10);\n"
                      u8"double XX = 0;\n"
                      u8"double ZZ = 0;\n"
                      u8"void main() { Float3@ xyz = v4.XYZ(); XX = xyz.x; ZZ = xyz.z; }\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"XX").Get<f64>() == 7.0);
    CHECK(ctx->GetGlobal(u8"ZZ").Get<f64>() == 9.0);

    // Instance method taking an object arg, returning bool; constructed from
    // object args too (AABB(Float3, Float3)).
    REQUIRE(ctx->Load(u8"AABB@ box = AABB(Float3(0, 0, 0), Float3(10, 10, 10));\n"
                      u8"bool inside = false;\n"
                      u8"bool outside = true;\n"
                      u8"void main() {\n"
                      u8"  inside = box.Contains(Float3(5, 5, 5));\n"
                      u8"  outside = box.Contains(Float3(20, 0, 0));\n"
                      u8"}\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"inside").Get<bool>() == true);
    CHECK(ctx->GetGlobal(u8"outside").Get<bool>() == false);
}

TEST_CASE("angelscript: same-arity type overloads carry distinct script names (overloadedName)")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // Same-arity type overloads carry distinct script names (the overloadedName contract): Mul
    // (Float3, Float3) componentwise and MulScalar (Float3, float) scale, both bound.
    REQUIRE(ctx->Load(u8"Float3@ p = Float3(2, 3, 4);\n"
                      u8"double CX = 0;\n"
                      u8"double CZ = 0;\n"
                      u8"double SX = 0;\n"
                      u8"void main() {\n"
                      u8"  Float3@ comp = Float3::Mul(p, Float3(1, 2, 3));\n" // -> (2, 6, 12)
                      u8"  Float3@ scaled = Float3::MulScalar(p, 2.0f);\n"     // -> (4, 6, 8)
                      u8"  CX = comp.x;\n"
                      u8"  CZ = comp.z;\n"
                      u8"  SX = scaled.x;\n"
                      u8"}\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"CX").Get<f64>() == 2.0); // chose (Float3, Float3)
    CHECK(ctx->GetGlobal(u8"CZ").Get<f64>() == 12.0);
    CHECK(ctx->GetGlobal(u8"SX").Get<f64>() == 4.0); // chose (Float3, float)
}

TEST_CASE("angelscript: reflected math ops (Math statics, Float3/Quaternion vector ops)")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    REQUIRE(ctx->Load(u8"double R = 0;\n"
                      u8"double CZ = 0;\n"
                      u8"double RX = 0;\n"
                      u8"void main() {\n"
                      u8"  R = Math::Sqrt(16.0f);\n"                                // -> 4
                      u8"  Float3@ c = Float3::Cross(Float3(1, 0, 0), Float3(0, 1, 0));\n" // -> +Z
                      u8"  CZ = c.z;\n"
                      // 90 degrees about +Y rotates +Z to +X.
                      u8"  Quaternion@ q = Quaternion::FromAxisAngle(Float3(0, 1, 0), 1.5707963f);\n"
                      u8"  Float3@ rv = Quaternion::RotateVector(q, Float3(0, 0, 1));\n"
                      u8"  RX = rv.x;\n"
                      u8"}\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"R").Get<f64>() == 4.0);
    CHECK(ctx->GetGlobal(u8"CZ").Get<f64>() == 1.0);
    CHECK(ctx->GetGlobal(u8"RX").Get<f64>() == doctest::Approx(1.0));
}

TEST_CASE("angelscript: bound-api signatures carry reflected parameter names")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(Widget::StaticType()); // a test-local type beside the registry's
    RegisterReflectedTypes(*manager);

    const Array<ScriptApiType> api = manager->DescribeBoundApi();

    const auto findType = [&](StringView name) -> const ScriptApiType* {
        for (const ScriptApiType& t : api)
        {
            if (t.scriptName.AsView() == name)
            {
                return &t;
            }
        }
        return nullptr;
    };
    const auto findMember = [](const ScriptApiType& t, StringView name) -> const ScriptApiMember* {
        for (const ScriptApiMember& m : t.members)
        {
            if (m.name.AsView() == name)
            {
                return &m;
            }
        }
        return nullptr;
    };

    // The signature now spells parameter names, not just types (API-browser affordance): it ends
    // with the last parameter's name before the closing paren.
    const ScriptApiType* float3 = findType(u8"Float3");
    REQUIRE(float3 != nullptr);
    const ScriptApiMember* cross = findMember(*float3, u8"Cross");
    REQUIRE(cross != nullptr);
    CHECK(cross->signature.AsView().EndsWith(u8" b)"));
    // The operators it binds are listed as such, as a script writes them.
    const ScriptApiMember* plus = findMember(*float3, u8"+");
    REQUIRE(plus != nullptr);
    CHECK(plus->kind == ScriptApiMemberKind::Operator);
    CHECK(plus->signature.AsView().StartsWith(u8"Float3 + "));
    CHECK(plus->signature.AsView().EndsWith(u8"(and +=)"));

    const ScriptApiType* math = findType(u8"Math");
    REQUIRE(math != nullptr);
    const ScriptApiMember* atan2 = findMember(*math, u8"Atan2");
    REQUIRE(atan2 != nullptr);
    CHECK(atan2->signature.AsView().EndsWith(u8" x)")); // Atan2(y, x)

    // A read-only property says so: the flag for tooling, the suffix for a reader of the
    // signature (the API browser), so an agent knows before it writes.
    const ScriptApiType* widget = findType(u8"Widget");
    REQUIRE(widget != nullptr);
    const ScriptApiMember* serial = findMember(*widget, u8"serial");
    REQUIRE(serial != nullptr);
    CHECK(serial->readOnly);
    CHECK(serial->kind == ScriptApiMemberKind::Property);
    CHECK(serial->signature.AsView() == u8"Widget.serial (read only)");
    const ScriptApiMember* id = findMember(*widget, u8"id");
    REQUIRE(id != nullptr);
    CHECK_FALSE(id->readOnly);
    CHECK(id->signature.AsView() == u8"Widget.id");
}

TEST_CASE("angelscript: Object-derived type as a script-visible class")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(Widget::StaticType());           // register just the Object type
    RefPtr<IScriptContext> ctx = manager->CreateContext(); // defensively finalizes

    REQUIRE(ctx->Load(u8"double I = 0;\n"
                      u8"double D = 0;\n"
                      u8"double O = 0;\n"
                      u8"void main() {\n"
                      u8"  Widget@ w = Widget();\n"
                      u8"  w.id = 21;\n"
                      u8"  Widget@ w2 = Widget();\n"
                      u8"  w2.id = 5;\n"
                      u8"  I = w.id;\n"
                      u8"  D = w.doubled();\n" // instance method -> 42
                      u8"  O = w.idOf(w2);\n"  // object argument -> 5
                      u8"}\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"I").Get<f64>() == 21.0);
    CHECK(ctx->GetGlobal(u8"D").Get<f64>() == 42.0);
    CHECK(ctx->GetGlobal(u8"O").Get<f64>() == 5.0);
}

TEST_CASE("angelscript: a default-constructed reflected type")
{
    RegisterCoreTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // Guid has a default ctor and (u64, u64); its props surface as doubles.
    REQUIRE(ctx->Load(u8"Guid@ g = Guid(7, 42);\n"
                      u8"double Hi = 0;\n"
                      u8"double Lo = 0;\n"
                      u8"void main() { Hi = double(g.high); Lo = double(g.low); }\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"Hi").Get<f64>() == 7.0);
    CHECK(ctx->GetGlobal(u8"Lo").Get<f64>() == 42.0);
}

TEST_CASE("angelscript: call a script function with marshalled args")
{
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(ctx->Load(u8"double add(double a, double b) { return a + b; }\n"
                      u8"string greeting() { return \"hi\"; }\n"
                      u8"string greet(string name) { return \"hi \" + name; }\n",
                      u8"main")
                .IsOk());

    CHECK(ctx->HasFunction(u8"add"));
    CHECK_FALSE(ctx->HasFunction(u8"nope"));

    // int args marshal to script doubles; result comes back as f64.
    Variant addArgs[] = {Variant::From(2), Variant::From(3)};
    CHECK(ctx->Call(u8"add", Span<Variant>{addArgs, 2}).Value().Get<f64>() == 5.0);

    // no-arg call returning a string.
    CHECK(ctx->Call(u8"greeting", Span<Variant>{}).Value().Get<String>() == u8"hi");

    // String argument in, string out.
    Variant greetArgs[] = {Variant::From(String(u8"engine"))};
    CHECK(ctx->Call(u8"greet", Span<Variant>{greetArgs, 1}).Value().Get<String>() ==
          u8"hi engine");

    // missing callable -> NotFound.
    CHECK(ctx->Call(u8"nope", Span<Variant>{}).Error() == ErrorCode::NotFound);
}

TEST_CASE("angelscript: instantiate a script class and invoke its methods")
{
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(ctx->Load(u8"class Counter {\n"
                      u8"  double n;\n"
                      u8"  Counter(double start) { n = start; }\n"
                      u8"  void add(double x) { n = n + x; }\n"
                      u8"  double value() { return n; }\n"
                      u8"  void reset() { n = 0; }\n"
                      u8"}\n",
                      u8"main")
                .IsOk());

    Variant ctorArgs[] = {Variant::From(10)};
    RefPtr<ScriptObject> counter = ctx->CreateInstance(u8"Counter", Span<Variant>{ctorArgs, 1});
    REQUIRE(static_cast<bool>(counter));

    Variant addArgs[] = {Variant::From(5)};
    CHECK(counter->Invoke(u8"add", Span<Variant>{addArgs, 1}).HasValue());

    CHECK(counter->Invoke(u8"value", Span<Variant>{}).Value().Get<f64>() == 15.0);
    // A member field reads as a property; a method or a missing name does not.
    CHECK(counter->GetProperty(u8"n").Value().Get<f64>() == 15.0);
    CHECK_FALSE(counter->GetProperty(u8"value").HasValue());
    CHECK_FALSE(counter->GetProperty(u8"missing").HasValue());

    CHECK(counter->Invoke(u8"reset", Span<Variant>{}).HasValue());
    CHECK(counter->Invoke(u8"value", Span<Variant>{}).Value().Get<f64>() == 0.0);
}

TEST_CASE("angelscript: CreateInstance returns null for an unknown class")
{
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
    REQUIRE(ctx->Load(u8"class Known { Known() {} }\n", u8"main").IsOk());
    CHECK_FALSE(static_cast<bool>(ctx->CreateInstance(u8"Missing", Span<Variant>{})));
}

TEST_CASE("angelscript: a script object outlives the local context reference")
{
    RefPtr<ScriptObject> obj;
    {
        RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager(foundation::core::DefaultAllocator())->CreateContext();
        REQUIRE(ctx->Load(u8"class Echo {\n"
                          u8"  Echo() {}\n"
                          u8"  double ping() { return 42; }\n"
                          u8"}\n",
                          u8"main")
                    .IsOk());
        obj = ctx->CreateInstance(u8"Echo", Span<Variant>{});
        REQUIRE(static_cast<bool>(obj));
        // ctx goes out of scope here; obj retains it (keeps the engine alive).
    }
    CHECK(obj->Invoke(u8"ping", Span<Variant>{}).Value().Get<f64>() == 42.0);
}

#ifdef OPTION_HAS_LUAU // proves AngelScript + a second backend resolve side by side
TEST_CASE("angelscript: registry - both backends resolve side by side")
{
    angelscript::RegisterAngelScriptBackend();
    RegisterLuauScriptBackend();
    ScriptBackendRegistry& registry = ScriptBackendRegistry::Get();

    const ScriptBackendDesc* as = registry.FindByLanguage(u8"angelscript");
    REQUIRE(as != nullptr);
    CHECK(as->displayName == u8"AngelScript");
    REQUIRE(as->fileExtensions.Size() == 1u);
    CHECK(as->fileExtensions[0] == u8"as");
    CHECK(registry.FindByExtension(u8"as") == as);

    // Extension dispatch creates the RIGHT manager: prove it by feeding each one
    // its own language's source.
    RefPtr<IScriptManager> asManager = CreateScriptManagerForFile(u8"Scripts/game.as", DefaultAllocator());
    REQUIRE(asManager.Get() != nullptr);
    RefPtr<IScriptContext> asCtx = asManager->CreateContext();
    CHECK(asCtx->Load(u8"double f() { return 1; }\n", u8"probe").IsOk());
    CHECK_FALSE(asCtx->Load(u8"local W = 1\n", u8"probe").IsOk()); // Luau source rejected

    RefPtr<IScriptManager> luauManager = CreateScriptManagerForFile(u8"Scripts/game.luau", DefaultAllocator());
    REQUIRE(luauManager.Get() != nullptr);
    RefPtr<IScriptContext> luauCtx = luauManager->CreateContext();
    CHECK(luauCtx->Load(u8"local A = 1\n", u8"main").IsOk());
    CHECK_FALSE(luauCtx->Load(u8"double f() { return 1; }", u8"main").IsOk()); // AS source rejected
}
#endif // OPTION_HAS_LUAU

// Regression: the behaviors module is loaded with each class in its OWN script section
// named by its sourceName (not one flat "behaviors#N"). This is what makes editor gutter
// breakpoints - keyed on the source file - line up with what GetLineNumber reports. Proves a
// breakpoint set on ("Mover.as", line) stops AND CaptureStackFrames()[0].file == "Mover.as".
TEST_CASE("angelscript: LoadBehaviorModule reports each class's sourceName as its section")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    REQUIRE(static_cast<bool>(manager));
    UniquePtr<IScriptDebugger> debugger = manager->CreateDebugger();
    REQUIRE(debugger.Get() != nullptr);

    RefPtr<IScriptContext> ctx = manager->CreateContext();
    REQUIRE(static_cast<bool>(ctx));

    // One behavior class, source file "Mover.as"; line 5 is the breakpoint line.
    const StringView moverSource =
        u8"class Mover {\n"        // 1
        u8"  double x;\n"          // 2
        u8"  Mover() { x = 0; }\n" // 3
        u8"  void tick() {\n"      // 4
        u8"    x = x + 1;\n"       // 5  <- breakpoint (keyed on the source file)
        u8"  }\n"                  // 6
        u8"}\n";                   // 7
    const BehaviorModuleClass classes[] = {{u8"Mover.as", moverSource, {}}};
    REQUIRE(ctx->LoadBehaviorModule(Span<const BehaviorModuleClass>{classes, 1}, u8"behaviors#1")
                .IsOk());

    struct Sink final : IScriptDebuggerListener
    {
        ScriptDebuggerState last = ScriptDebuggerState::Running;
        void OnDebuggerStateChanged(ScriptDebuggerState state) override { last = state; }
    } sink;
    debugger->SetListener(&sink);

    // The breakpoint is keyed on the SOURCE FILE, exactly as an editor gutter would set it -
    // NOT on the "behaviors#1" module name.
    debugger->SetBreakpoint(u8"Mover.as", 5);

    RefPtr<ScriptObject> mover = ctx->CreateInstance(u8"Mover", Span<Variant>{});
    REQUIRE(static_cast<bool>(mover));
    (void)mover->Invoke(u8"tick", Span<Variant>{}); // suspends at the breakpoint

    CHECK(sink.last == ScriptDebuggerState::Breakpoint);
    Array<ScriptStackFrame> frames = debugger->CaptureStackFrames();
    REQUIRE_FALSE(frames.IsEmpty());
    CHECK(frames[0].line == 5);
    CHECK(StringView(frames[0].file) == u8"Mover.as"); // the section IS the source file

    debugger->Continue();
    CHECK(sink.last == ScriptDebuggerState::Terminated);
    debugger->SetListener(nullptr);
}

#include "../Script.Tests/BackendConformance.h"

TEST_CASE("angelscript: CERTIFIED - the backend conformance battery")
{
    foundation::script::conformance::Dialect dialect;
    dialect.languageId = u8"angelscript";
    dialect.functionsModule = u8"int answer = 42;\n"
                              u8"double add(double a, double b) { return a + b; }\n"
                              u8"string greeting() { return \"hi\"; }\n";
    dialect.counterClass = u8"class Counter {\n"
                           u8"  double n;\n"
                           u8"  Counter(double start) { n = start; }\n"
                           u8"  void increment() { n = n + 1; }\n"
                           u8"  double value() { return n; }\n"
                           u8"}\n";
    dialect.compileBroken = u8"int = = = @#$";
    dialect.runtimeFault = u8"void main() { int zero = 0; int boom = 10 / zero; }\n";
    // Delegate: subscribe an AngelScript funcdef handle (value * 2) to DelegateSignal.
    dialect.delegateModule = u8"double dbl(double x) { return x * 2; }\n"
                             u8"DelegateSignal@ signal = DelegateSignal();\n"
                             u8"void main() { signal.Connect(ScriptDelegate(dbl)); }\n";
    // Overload contract: arity family (ping / ping(x), AS overloads natively) + distinct-name
    // overload (combineText). Set the globals in main() (run at load, like the delegate module).
    dialect.overloadModule = u8"Overloads@ over = Overloads();\n"
                             u8"double OP0 = 0; double OP1 = 0; double OC = 0; double OCT = 0;\n"
                             u8"void main() {\n"
                             u8"  OP0 = over.ping();\n"
                             u8"  OP1 = over.ping(5);\n"
                             u8"  OC = over.combine(2, 3);\n"
                             u8"  OCT = over.combineText(\"x\", 7);\n"
                             u8"}\n";
    // Container contract: two adds through returned handles, zero-based at, move, removeAt.
    dialect.containerModule =
        u8"Crate@ crate = Crate();\n"
        u8"double KN = 0; double KZ = 0; double KM = 0; double KR = 0; double KL = 0;\n"
        u8"void main() {\n"
        u8"  CrateItem@ a = crate.items_add(); a.size = 5;\n"
        u8"  CrateItem@ b = crate.items_add(); b.size = 9;\n"
        u8"  KN = crate.items_count();\n"
        u8"  KZ = crate.items_at(0).size;\n"
        u8"  crate.items_move(1, 0);\n"
        u8"  KM = crate.items_at(0).size;\n"
        u8"  crate.items_removeAt(0);\n"
        u8"  KR = crate.items_count();\n"
        u8"  KL = crate.items_at(0).size;\n"
        u8"}\n";
    // AngelScript's natural coroutine surface: a delegate to a method (`this.RunWait`)
    // wrapped in the ScriptCoroutine funcdef, started with startCoroutine; `wait` is a
    // host function, `waitUntil` a script helper (injected per module). The shared
    // coroutine CONCEPT, spelled in each backend's own syntax - and that is the point.
    dialect.coroutineClass =
        u8"class Coro {\n"
        u8"  double p;\n"
        u8"  bool gate;\n"
        u8"  Coro() { p = 0; gate = false; }\n"
        u8"  double progress() { return p; }\n"
        u8"  void flip() { gate = true; }\n"
        u8"  bool GateOpen() { return gate; }\n"
        u8"  void RunWait() { wait(1.0f); p = 1; }\n"
        u8"  void RunUntil() { Coroutine::waitUntil(CoroutinePredicate(this.GateOpen)); p = 1; }\n"
        u8"  void begin() { startCoroutine(ScriptCoroutine(this.RunWait)); }\n"
        u8"  void beginUntil() { startCoroutine(ScriptCoroutine(this.RunUntil)); }\n"
        u8"}\n";
    // Debugger: a zero-arg entry. Line 1 `gLast`, line 2 the signature, line 3 sets `tag`,
    // line 4 (the breakpoint) has `tag == "hit"` in scope; a step lands on line 5, and
    // continue runs to completion.
    dialect.debugModule = u8"int gLast = 0;\n"          // 1
                          u8"void debugRun() {\n"       // 2
                          u8"  string tag = \"hit\";\n" // 3
                          u8"  int a = 7;\n"            // 4  <- breakpoint (tag in scope, == "hit")
                          u8"  gLast = a;\n"            // 5
                          u8"}\n";                      // 6
    dialect.debugSection = u8"debug.script";
    dialect.debugFunction = u8"debugRun";
    dialect.debugBreakLine = 4;
    dialect.debugLocalName = u8"tag";
    dialect.debugLocalValue = u8"\"hit\"";

    foundation::script::conformance::RunScriptBackendConformance(
        []() { return foundation::script::angelscript::CreateScriptManager(foundation::core::DefaultAllocator()); }, dialect);
}

TEST_CASE("angelscript: declares Coroutines + Delegates + Debugger + Bytecode; profiler absent (B4)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Coroutines));
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Delegates));
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Debugger));
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Bytecode));
    CHECK_FALSE(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Profiler));
    CHECK(manager->CreateDebugger().Get() != nullptr); // Debugger declared -> real factory
    CHECK(manager->CreateProfiler().Get() == nullptr);
}

TEST_CASE("angelscript: bytecode capability - compile to blob, serialize, load in the player")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());

    // COOK side: compile source (module-level functions) to an opaque bytecode blob.
    constexpr StringView kSource = u8"double add(double a, double b) { return a + b; }\n"
                                   u8"double answer() { return 42.0; }\n";
    Result<RefPtr<IScriptBlob>> compiled = manager->CompileToBlob(kSource, u8"as.blob");
    REQUIRE(compiled.HasValue());
    REQUIRE(compiled.Value().Get() != nullptr);

    // STORE + reconstruct: the blob serializes to bytes; a fresh blob deserializes them.
    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        compiled.Value()->Serialize(writer);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    RefPtr<IScriptBlob> reloaded = manager->CreateBlob();
    REQUIRE(reloaded.Get() != nullptr);
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        reloaded->Serialize(reader);
    }

    // PLAYER side: LoadByteCode into a context (no Build) and call the restored functions.
    RefPtr<IScriptContext> context = manager->CreateContext();
    REQUIRE(context->LoadBlob(*reloaded).IsOk());
    Variant args[] = {Variant::From<f64>(2.0), Variant::From<f64>(3.0)};
    CHECK(context->Call(u8"add", Span<Variant>{args, 2}).Value().Get<f64>() == doctest::Approx(5.0));
    CHECK(context->Call(u8"answer", Span<Variant>{}).Value().Get<f64>() == doctest::Approx(42.0));
}

TEST_CASE("angelscript: CompileToBlob rejects a broken source at cook")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    CHECK_FALSE(manager->CompileToBlob(u8"int broken( {", u8"as.broken").HasValue());
}

TEST_CASE("angelscript: a script function is a native callback via IScriptDelegate (the real use)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(conformance::DelegateSignal::StaticType());
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // A behavior subscribes a function handle to a native event; native code fires it.
    REQUIRE(ctx->Load(u8"double add5(double x) { return x + 5; }\n"
                      u8"DelegateSignal@ signal = DelegateSignal();\n"
                      u8"void main() { signal.Connect(ScriptDelegate(add5)); }\n",
                      u8"main")
                .IsOk());

    Variant signalVar = ctx->GetGlobal(u8"signal");
    REQUIRE(signalVar.IsObject());
    conformance::DelegateSignal* signal = signalVar.AsObject<conformance::DelegateSignal>();
    REQUIRE(signal != nullptr);

    CHECK(signal->Emit(10.0) == doctest::Approx(15.0));   // native fires -> function runs
    CHECK(signal->Emit(100.0) == doctest::Approx(105.0)); // reusable across firings
}

// A reflected static that resolves a PER-CONTEXT service through CurrentScriptContext() - the
// exact shape a facade uses to reach an engine singleton without a process global. If a delegate
// callback does not scope its owning context, CurrentScriptContext() is null during the call and
// this silently no-ops (the bug); with the context scoped it resolves and writes the flag.
namespace
{
    class ContextProbe : public Object
    {
        RTTI_OBJECT(ContextProbe, Object)
    public:
        void Poke() const
        {
            if (IScriptContext* context = CurrentScriptContext())
            {
                if (int* flag = static_cast<int*>(context->GetService(u8"probe.flag")))
                {
                    *flag = 42;
                }
            }
        }
    };
}

REFLECT_MEMBERS(ContextProbe, "rtti::script::test")
{
    builder.Method<&ContextProbe::Poke>("Poke");
    builder.Constructor();
}

TEST_CASE("angelscript: a delegate callback scopes its owning context (facades resolve services)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(conformance::DelegateSignal::StaticType());
    manager->RegisterType(ContextProbe::StaticType());
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    conformance::CapturedErrors errors;
    ctx->SetErrorHandler(&errors);

    // The per-context service the handler's facade will resolve (engine singletons reach
    // scripts through CurrentScriptContext()->GetService, never a process global).
    int flag = 0;
    ctx->SetService(u8"probe.flag", &flag);

    // A script function subscribed to a native signal; its body calls the reflected static that
    // reads CurrentScriptContext()->GetService and writes through it.
    REQUIRE(ctx->Load(u8"double onFire(double x) { ContextProbe p; p.Poke(); return x; }\n"
                      u8"DelegateSignal@ signal = DelegateSignal();\n"
                      u8"void main() { signal.Connect(ScriptDelegate(onFire)); }\n",
                      u8"main")
                .IsOk());
    CHECK(errors.count == 0);

    Variant signalVar = ctx->GetGlobal(u8"signal");
    REQUIRE(signalVar.IsObject());
    conformance::DelegateSignal* signal = signalVar.AsObject<conformance::DelegateSignal>();
    REQUIRE(signal != nullptr);

    CHECK(flag == 0);   // not fired yet
    signal->Emit(1.0);  // native fires the delegate; Invoke scopes the owning context
    CHECK(flag == 42);  // the handler's facade resolved THIS context's service
}

TEST_CASE("angelscript: a METHOD delegate is a native callback via IScriptDelegate")
{
    // The first real use of a delegate-to-method (`ScriptDelegate(h.bump)`, an asFUNC_DELEGATE
    // that binds an object + its method) through the IScriptDelegate seam. A global-function
    // delegate already works above; this covers the bound-method form: it must reach
    // ValueFromArg -> MakeAngelScriptDelegateVariant and survive AddRef, then invoke on the
    // captured object so the object's field is mutated.
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
    manager->RegisterType(conformance::DelegateSignal::StaticType());
    RefPtr<IScriptContext> ctx = manager->CreateContext();
    conformance::CapturedErrors errors;
    ctx->SetErrorHandler(&errors);

    // A script class whose method matches the ScriptDelegate funcdef (double(double)); a bound
    // method delegate to a live instance is subscribed to the native signal.
    REQUIRE(ctx->Load(u8"class Handler { double v = 0;\n"
                      u8"  double bump(double x) { v += x; return v; } }\n"
                      u8"Handler@ h = Handler();\n"
                      u8"DelegateSignal@ signal = DelegateSignal();\n"
                      u8"void main() { signal.Connect(ScriptDelegate(h.bump)); }\n",
                      u8"main")
                .IsOk());
    CHECK(errors.count == 0);

    Variant signalVar = ctx->GetGlobal(u8"signal");
    REQUIRE(signalVar.IsObject());
    conformance::DelegateSignal* signal = signalVar.AsObject<conformance::DelegateSignal>();
    REQUIRE(signal != nullptr);

    CHECK(signal->Emit(10.0) == doctest::Approx(10.0));  // native fires -> h.bump ran (v: 0 -> 10)
    CHECK(signal->Emit(5.0) == doctest::Approx(15.0));   // same bound object accumulates (v -> 15)
}

TEST_CASE("angelscript: many engine lifecycles in one process (the cook pattern)")
{
    // The cook builds one engine per script; interleaved create/destroy used to unbalance
    // the process-global thread manager and assert at teardown. The constructor's
    // process-lifetime pin makes any sequence safe - this exercises the multi-engine path.
    for (int i = 0; i < 8; ++i)
    {
        RefPtr<IScriptManager> manager = angelscript::CreateScriptManager(foundation::core::DefaultAllocator());
        REQUIRE(manager.Get() != nullptr);
        manager->FinalizeTypes();
        RefPtr<IScriptContext> ctx = manager->CreateContext();
        REQUIRE(ctx.Get() != nullptr);
        CHECK(ctx->Load(u8"void main() { }", u8"cook_cycle").IsOk());
    }
}
