// Draconic::ScriptAngelScript tests: the Wren backend's reflected-type EMISSION
// suite ported to AngelScript syntax (the battery certifies the context contract;
// emission is certified per backend), the shared conformance battery, and the
// backend-registry integration (both languages resolving side by side).
//
// AngelScript has no top-level statements, so scripts follow the backend's load
// convention: module globals + a `void main()` that Load runs after Build.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h" // <new> reachability for reflection containers (GCC)
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.script;
import draconic.script.angelscript;
import draconic.script.wren;

using namespace draconic::foundation;
using namespace draconic::script;

// A reflected Object-derived type to exercise object classes in AngelScript.
namespace
{
    class Widget : public Object
    {
        DRACONIC_OBJECT(Widget, Object)
    public:
        int id = 0;
        int doubled() const { return id * 2; }
        int idOf(Widget* other) const { return other != nullptr ? other->id : -1; }
    };
}

DRACONIC_REFLECT(Widget, "draconic::script::test")
{
    builder.Property<&Widget::id>("id");
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
        DRACONIC_OBJECT(NumProbe, Object)
    public:
        i64 lastI64 = 0;
        i32 lastI32 = 0;
        void takeI64(i64 v) { lastI64 = v; }
        void takeI32(i32 v) { lastI32 = v; }
        bool argWasExact() const { return lastI64 == 9007199254740993LL; } // 2^53 + 1
        i32 echoI32() const { return lastI32; }
        i64 bigConst() const { return 9007199254740993LL; } // 2^53 + 1
    };
}

DRACONIC_REFLECT(NumProbe, "draconic::script::test")
{
    builder.Method<&NumProbe::takeI64>("takeI64");
    builder.Method<&NumProbe::takeI32>("takeI32");
    builder.Method<&NumProbe::argWasExact>("argWasExact");
    builder.Method<&NumProbe::echoI32>("echoI32");
    builder.Method<&NumProbe::bigConst>("bigConst");
    builder.Constructor();
}

// A reflected Object with a property but NO reflected constructor, plus a factory that returns one:
// AngelScript declares + binds a type's properties independent of any factory, so a constructor-less
// handle handed back from a facade method is fully usable (the collections-lift precondition).
namespace
{
    class Leaf : public Object
    {
        DRACONIC_OBJECT(Leaf, Object)
    public:
        int value = 0;
    };

    class LeafFactory : public Object
    {
        DRACONIC_OBJECT(LeafFactory, Object)
    public:
        RefPtr<Leaf> make() const { return MakeRef<Leaf>(DefaultAllocator()); }
    };
}

DRACONIC_REFLECT(Leaf, "draconic::script::test")
{
    builder.Property<&Leaf::value>("value"); // deliberately no Constructor()
}
DRACONIC_REFLECT(LeafFactory, "draconic::script::test")
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
        DRACONIC_OBJECT(Animal, Object)
    public:
        int legs = 4;
    };
    class Dog : public Animal
    {
        DRACONIC_OBJECT(Dog, Animal)
    public:
        int barks = 1;
    };
    class Cat : public Animal
    {
        DRACONIC_OBJECT(Cat, Animal)
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
        DRACONIC_OBJECT(Zoo, Object)
    public:
        Array<RefPtr<Animal>> animals;
    };
}

DRACONIC_REFLECT(Animal, "draconic::script::test")
{
    builder.Property<&Animal::legs>("legs");
}
DRACONIC_REFLECT(Dog, "draconic::script::test")
{
    builder.Property<&Dog::barks>("barks");
}
DRACONIC_REFLECT(Cat, "draconic::script::test")
{
    builder.Property<&Cat::meows>("meows");
}
DRACONIC_REFLECT(Zoo, "draconic::script::test")
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
        DRACONIC_OBJECT(House, Object)
    public:
        Room room;
    };
    class Shelf : public Object
    {
        DRACONIC_OBJECT(Shelf, Object)
    public:
        Array<UniquePtr<Room>> rooms;
    };
}
DRACONIC_REFLECT_VALUE(Room, "draconic::script::test")
{
    builder.Property<&Room::size>("size");
}
DRACONIC_REFLECT(House, "draconic::script::test")
{
    builder.Nested<&House::room>("room");
    builder.Constructor();
}
DRACONIC_REFLECT(Shelf, "draconic::script::test")
{
    builder.Nested<&Shelf::rooms>("rooms");
    builder.Constructor();
}
namespace
{
    void RegisterHouse(IScriptManager& manager)
    {
        static bool once = [] {
            DraconicRegisterValue_Room();
            RegisterUniquePtrArrayType<Room>();
            return true;
        }();
        (void)once;
        manager.RegisterType(TypeOf<Room>());
        manager.RegisterType(House::StaticType());
        manager.RegisterType(Shelf::StaticType());
    }
}

TEST_CASE("angelscript: a nested-value member is a borrow handle edited in place (unit 2b)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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
    RegisterFoundationTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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

TEST_CASE("angelscript: 64-bit integer facade args/returns round-trip exactly (no double funnel)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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

TEST_CASE("angelscript: a context runs valid source")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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
                                    u8"void main() { Greeter g(\"draconic\"); G = g.greet(); }\n",
                                    u8"main");
    CHECK(status.IsOk());
    CHECK(ctx->GetGlobal(u8"G").Get<String>() == u8"hi draconic");
}

TEST_CASE("angelscript: a compile error is reported")
{
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager()->CreateContext();
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
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager()->CreateContext();
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
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager()->CreateContext();
    const Status status =
        ctx->Load(u8"void main() { int zero = 0; int boom = 10 / zero; }\n", u8"main");
    CHECK_FALSE(status.IsOk());
    CHECK(status.Code() == ErrorCode::Internal);
}

TEST_CASE("angelscript: a runtime fault in a GLOBAL INITIALIZER is a Runtime error")
{
    // Build both compiles and runs global initializers; the backend classifies a
    // failed init as Runtime (not Compile) by Build's return code.
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager()->CreateContext();
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
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager()->CreateContext();
    REQUIRE(ctx->Load(u8"int Answer = 42;\n"
                      u8"string Name = \"draconic\";\n"
                      u8"bool Flag = true;\n"
                      u8"double Pi = 3.5;\n",
                      u8"main")
                .IsOk());

    CHECK(ctx->GetGlobal(u8"Answer").Get<f64>() == 42.0); // numbers surface as f64
    CHECK(ctx->GetGlobal(u8"Name").Get<String>() == u8"draconic");
    CHECK(ctx->GetGlobal(u8"Flag").Get<bool>() == true);
    CHECK(ctx->GetGlobal(u8"Pi").Get<f64>() == 3.5);

    CHECK(ctx->GetGlobal(u8"Missing").IsEmpty()); // absent -> empty Variant
}

TEST_CASE("angelscript: SetGlobal writes typed module globals")
{
    // Unlike Wren (whose C API cannot set variables), AngelScript globals are
    // directly writable - the contract's SetGlobal is real here.
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager()->CreateContext();
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

TEST_CASE("angelscript: reflected value types are usable from script (construct + properties)")
{
    RegisterFoundationTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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
    RegisterFoundationTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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

TEST_CASE("angelscript: same-name overloads register per exact signature")
{
    RegisterFoundationTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // Float3::Mul has two overloads: (Float3, Float3) componentwise and
    // (Float3, float) scale - AngelScript resolves them statically by signature.
    REQUIRE(ctx->Load(u8"Float3@ p = Float3(2, 3, 4);\n"
                      u8"double CX = 0;\n"
                      u8"double CZ = 0;\n"
                      u8"double SX = 0;\n"
                      u8"void main() {\n"
                      u8"  Float3@ comp = Float3::Mul(p, Float3(1, 2, 3));\n" // -> (2, 6, 12)
                      u8"  Float3@ scaled = Float3::Mul(p, 2.0f);\n"          // -> (4, 6, 8)
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

TEST_CASE("angelscript: Object-derived type as a script-visible class")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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
    RegisterFoundationTypes();
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager()->CreateContext();
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
    Variant greetArgs[] = {Variant::From(String(u8"draconic"))};
    CHECK(ctx->Call(u8"greet", Span<Variant>{greetArgs, 1}).Value().Get<String>() ==
          u8"hi draconic");

    // missing callable -> NotFound.
    CHECK(ctx->Call(u8"nope", Span<Variant>{}).Error() == ErrorCode::NotFound);
}

TEST_CASE("angelscript: instantiate a script class and invoke its methods")
{
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager()->CreateContext();
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

    CHECK(counter->Invoke(u8"reset", Span<Variant>{}).HasValue());
    CHECK(counter->Invoke(u8"value", Span<Variant>{}).Value().Get<f64>() == 0.0);
}

TEST_CASE("angelscript: CreateInstance returns null for an unknown class")
{
    RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager()->CreateContext();
    REQUIRE(ctx->Load(u8"class Known { Known() {} }\n", u8"main").IsOk());
    CHECK_FALSE(static_cast<bool>(ctx->CreateInstance(u8"Missing", Span<Variant>{})));
}

TEST_CASE("angelscript: a script object outlives the local context reference")
{
    RefPtr<ScriptObject> obj;
    {
        RefPtr<IScriptContext> ctx = angelscript::CreateScriptManager()->CreateContext();
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

TEST_CASE("angelscript: registry - both backends resolve side by side")
{
    angelscript::RegisterAngelScriptBackend();
    wren::RegisterWrenScriptBackend();
    ScriptBackendRegistry& registry = ScriptBackendRegistry::Get();

    const ScriptBackendDesc* as = registry.FindByLanguage(u8"angelscript");
    REQUIRE(as != nullptr);
    CHECK(as->displayName == u8"AngelScript");
    REQUIRE(as->fileExtensions.Size() == 1u);
    CHECK(as->fileExtensions[0] == u8"as");
    CHECK(registry.FindByExtension(u8"as") == as);

    // Extension dispatch creates the RIGHT manager: prove it by feeding each one
    // its own language's source.
    RefPtr<IScriptManager> asManager = CreateScriptManagerForFile(u8"Scripts/game.as");
    REQUIRE(asManager.Get() != nullptr);
    RefPtr<IScriptContext> asCtx = asManager->CreateContext();
    CHECK(asCtx->Load(u8"double f() { return 1; }\n", u8"probe").IsOk());
    CHECK_FALSE(asCtx->Load(u8"var W = Fn.new { 1 }", u8"probe").IsOk()); // Wren source rejected

    RefPtr<IScriptManager> wrenManager = CreateScriptManagerForFile(u8"Scripts/game.wren");
    REQUIRE(wrenManager.Get() != nullptr);
    RefPtr<IScriptContext> wrenCtx = wrenManager->CreateContext();
    CHECK(wrenCtx->Load(u8"var A = 1", u8"main").IsOk());
    CHECK_FALSE(wrenCtx->Load(u8"double f() { return 1; }", u8"main").IsOk()); // AS source rejected
}

// P1.5 regression: the behaviors module is loaded with each class in its OWN script section
// named by its sourceName (not one flat "behaviors#N"). This is what makes editor gutter
// breakpoints - keyed on the source file - line up with what GetLineNumber reports. Proves a
// breakpoint set on ("Mover.as", line) stops AND CaptureStackFrames()[0].file == "Mover.as".
TEST_CASE("angelscript: LoadBehaviorModule reports each class's sourceName as its section")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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
    const BehaviorModuleClass classes[] = {{u8"Mover.as", moverSource}};
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

#include "../Draconic.Script.Tests/BackendConformance.h"

TEST_CASE("angelscript: CERTIFIED - the backend conformance battery (scripting.md B2)")
{
    draconic::script::conformance::Dialect dialect;
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
    // AngelScript's natural coroutine surface: a delegate to a method (`this.RunWait`)
    // wrapped in the ScriptCoroutine funcdef, started with startCoroutine; `wait` is a
    // host function, `waitUntil` a script helper (injected per module). Same concept as
    // Wren, different syntax (delegate vs fiber block) - and that is the point.
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

    draconic::script::conformance::RunScriptBackendConformance(
        []() { return draconic::script::angelscript::CreateScriptManager(); }, dialect);
}

TEST_CASE("angelscript: declares Coroutines + Delegates + Debugger; profiler/bytecode absent (B4)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Coroutines));
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Delegates));
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Debugger));
    CHECK_FALSE(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Profiler));
    CHECK_FALSE(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Bytecode));
    CHECK(manager->CreateDebugger().Get() != nullptr); // Debugger declared -> real factory
    CHECK(manager->CreateProfiler().Get() == nullptr);
    CHECK(manager->CompileToBlob(u8"", u8"blob").Error() == ErrorCode::NotSupported);
}

TEST_CASE("angelscript: a script function is a native callback via IScriptDelegate (the real use)")
{
    RefPtr<IScriptManager> manager = angelscript::CreateScriptManager();
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
