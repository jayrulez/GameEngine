#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h" // <new> reachability for reflection containers (GCC)
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.script;
import draconic.script.wren;

using namespace draconic::foundation;
using namespace draconic::script;

// A reflected Object-derived type to exercise object foreign classes in Wren.
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

// A reflected Object with a property but NO reflected constructor: not script-constructable, only
// obtainable as a handle handed back by a factory. Plus a factory that returns one. Together they
// exercise the collections lift - constructor-less types reachable from a bound method get emitted.
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

// A polymorphic-container owner: Zoo holds Array<RefPtr<Animal>>; Dog/Cat are the concrete elements
// (each with its OWN reflected property, the module-list shape). Exercises the container-member
// binding (count / at / add-by-name / removeAt / move) with owned object-element handles.
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
    builder.Nested<&Zoo::animals>("animals"); // a polymorphic container member
    builder.Constructor();
}

namespace
{
    // Register the Zoo graph once (idempotent): the polymorphic container factory + the derived types
    // in the global registry (EnumerateDerived + reachability need them), then on the manager.
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

TEST_CASE("wren: a context runs valid source")
{
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    REQUIRE(static_cast<bool>(manager));

    RefPtr<IScriptContext> ctx = manager->CreateContext();
    REQUIRE(static_cast<bool>(ctx));

    // A real class-based Wren program: define a class, instantiate, call a method.
    const Status status = ctx->Load(u8"class Greeter {\n"
                                    u8"  construct new(name) { _name = name }\n"
                                    u8"  greet() { System.print(\"hi %(_name)\") }\n"
                                    u8"}\n"
                                    u8"Greeter.new(\"draconic\").greet()\n",
                                    u8"main");
    CHECK(status.IsOk());
}

TEST_CASE("wren: a compile error is reported")
{
    RefPtr<IScriptContext> ctx = wren::CreateScriptManager()->CreateContext();
    const Status status = ctx->Load(u8"this is not valid wren @#$", u8"main");
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

TEST_CASE("wren: errors are surfaced to a handler")
{
    RefPtr<IScriptContext> ctx = wren::CreateScriptManager()->CreateContext();
    CapturingErrors errors;
    ctx->SetErrorHandler(&errors);

    // Compile error: kind + non-empty message + a source line.
    CHECK_FALSE(ctx->Load(u8"var = = =", u8"main").IsOk());
    CHECK(errors.count >= 1);
    CHECK(errors.lastKind == ScriptErrorKind::Compile);
    CHECK(!errors.lastMessage.IsEmpty());
    CHECK(errors.lastLine >= 1);

    // Runtime error: kind switches to Runtime.
    const int afterCompile = errors.count;
    CHECK_FALSE(ctx->Load(u8"Fiber.abort(\"boom\")", u8"main").IsOk());
    CHECK(errors.count > afterCompile);
    CHECK(errors.lastKind == ScriptErrorKind::Runtime);
    CHECK(!errors.lastMessage.IsEmpty());

    // Clearing the handler restores default (console) reporting - no more captures.
    ctx->SetErrorHandler(nullptr);
    const int afterRuntime = errors.count;
    CHECK_FALSE(ctx->Load(u8"more @#$ garbage", u8"main").IsOk());
    CHECK(errors.count == afterRuntime);
}

TEST_CASE("wren: a runtime error is reported")
{
    RefPtr<IScriptContext> ctx = wren::CreateScriptManager()->CreateContext();
    const Status status = ctx->Load(u8"Fiber.abort(\"boom\")", u8"main");
    CHECK_FALSE(status.IsOk());
    CHECK(status.Code() == ErrorCode::Internal);
}

TEST_CASE("wren: each context is an isolated VM")
{
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    RefPtr<IScriptContext> a = manager->CreateContext();
    RefPtr<IScriptContext> b = manager->CreateContext();
    CHECK(a.Get() != b.Get());
    CHECK(a->Load(u8"var X = 1", u8"main").IsOk());
    CHECK(b->Load(u8"var Y = 2", u8"main").IsOk());
}

TEST_CASE("wren: read module globals as Variant")
{
    RefPtr<IScriptContext> ctx = wren::CreateScriptManager()->CreateContext();
    REQUIRE(ctx->Load(u8"var Answer = 42\n"
                      u8"var Name = \"draconic\"\n"
                      u8"var Flag = true\n",
                      u8"main")
                .IsOk());

    CHECK(ctx->GetGlobal(u8"Answer").Get<f64>() == 42.0); // Wren numbers are doubles
    CHECK(ctx->GetGlobal(u8"Name").Get<String>() == u8"draconic");
    CHECK(ctx->GetGlobal(u8"Flag").Get<bool>() == true);

    CHECK(ctx->GetGlobal(u8"Missing").IsEmpty()); // absent -> empty Variant
}

TEST_CASE("wren: reflected value types are usable from script (construct + properties)")
{
    RegisterFoundationTypes();
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    RegisterReflectedTypes(*manager);                      // reflection -> manager
    RefPtr<IScriptContext> ctx = manager->CreateContext(); // emits Wren foreign classes

    // Construct a reflected Float3 from Wren, read and write its properties.
    const Status status = ctx->Load(u8"var v = Float3.new(1, 2, 3)\n"
                                    u8"var X = v.x\n"
                                    u8"var Z = v.z\n"
                                    u8"v.x = 9\n"
                                    u8"var X2 = v.x\n",
                                    u8"main");
    REQUIRE(status.IsOk());

    CHECK(ctx->GetGlobal(u8"X").Get<f64>() == 1.0); // construct + getter
    CHECK(ctx->GetGlobal(u8"Z").Get<f64>() == 3.0);
    CHECK(ctx->GetGlobal(u8"X2").Get<f64>() == 9.0); // setter took effect
}

TEST_CASE("wren: call reflected methods (static, instance, struct return, foreign args)")
{
    RegisterFoundationTypes();
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // Static method with foreign args, scalar return: Float3.Dot(a, b).
    REQUIRE(ctx->Load(u8"var a = Float3.new(1, 2, 3)\n"
                      u8"var b = Float3.new(4, 5, 6)\n"
                      u8"var D = Float3.Dot(a, b)\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"D").Get<f64>() == 32.0);

    // Instance method returning a struct (Float4.XYZ() -> Float3), then read it.
    REQUIRE(ctx->Load(u8"var v4 = Float4.new(7, 8, 9, 10)\n"
                      u8"var xyz = v4.XYZ()\n"
                      u8"var XX = xyz.x\n"
                      u8"var ZZ = xyz.z\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"XX").Get<f64>() == 7.0);
    CHECK(ctx->GetGlobal(u8"ZZ").Get<f64>() == 9.0);

    // Instance method taking a foreign arg, returning bool; constructed from
    // foreign args too (AABB.new(Float3, Float3)).
    REQUIRE(ctx->Load(u8"var box = AABB.new(Float3.new(0, 0, 0), Float3.new(10, 10, 10))\n"
                      u8"var inside = box.Contains(Float3.new(5, 5, 5))\n"
                      u8"var outside = box.Contains(Float3.new(20, 0, 0))\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"inside").Get<bool>() == true);
    CHECK(ctx->GetGlobal(u8"outside").Get<bool>() == false);
}

TEST_CASE("wren: same-name overloads resolve by argument type")
{
    RegisterFoundationTypes();
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // Float3.Mul has two overloads: (Float3, Float3) componentwise and (Float3, f32) scale.
    REQUIRE(ctx->Load(u8"var p = Float3.new(2, 3, 4)\n"
                      u8"var comp = Float3.Mul(p, Float3.new(1, 2, 3))\n" // -> (2, 6, 12)
                      u8"var scaled = Float3.Mul(p, 2)\n"                 // -> (4, 6, 8)
                      u8"var CX = comp.x\n"
                      u8"var CZ = comp.z\n"
                      u8"var SX = scaled.x\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"CX").Get<f64>() == 2.0); // chose (Float3, Float3)
    CHECK(ctx->GetGlobal(u8"CZ").Get<f64>() == 12.0);
    CHECK(ctx->GetGlobal(u8"SX").Get<f64>() == 4.0); // chose (Float3, f32)
}

TEST_CASE("wren: Object-derived type as a foreign class")
{
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    manager->RegisterType(Widget::StaticType()); // register just the Object type
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    REQUIRE(ctx->Load(u8"var w = Widget.new()\n"
                      u8"w.id = 21\n"
                      u8"var w2 = Widget.new()\n"
                      u8"w2.id = 5\n"
                      u8"var I = w.id\n"
                      u8"var D = w.doubled()\n" // instance method -> 42
                      u8"var O = w.idOf(w2)\n", // object argument -> 5
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"I").Get<f64>() == 21.0);
    CHECK(ctx->GetGlobal(u8"D").Get<f64>() == 42.0);
    CHECK(ctx->GetGlobal(u8"O").Get<f64>() == 5.0);
}

TEST_CASE("wren: a constructor-less reflected type is usable as a returned handle (collections lift)")
{
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    manager->RegisterType(Leaf::StaticType());        // no ctor - reachable only via the factory
    manager->RegisterType(LeafFactory::StaticType()); // seed: has a ctor + a method returning Leaf
    manager->FinalizeTypes();
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // Leaf has no reflected constructor, so it is not script-constructable (no allocator emitted).
    CHECK_FALSE(ctx->Load(u8"var x = Leaf.new()", u8"main").IsOk());

    // But a factory hands one back, and the returned handle exposes Leaf's reflected property
    // (get + set write through to the same native object).
    REQUIRE(ctx->Load(u8"var f = LeafFactory.new()\n"
                      u8"var leaf = f.make()\n"
                      u8"leaf.value = 7\n"
                      u8"var V = leaf.value\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"V").Get<f64>() == 7.0);
}

// Non-Object VALUE type + owners that reach it only by address: House has a nested-value member,
// Shelf a homogeneous Array<UniquePtr<Room>>. Both need borrow-mode handles (unit 2b).
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
        Room room; // nested value member
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

TEST_CASE("wren: a nested-value member is a borrow handle edited in place (unit 2b)")
{
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    RegisterHouse(*manager);
    manager->FinalizeTypes();
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // h.room returns a borrow over the House's Room subobject; editing it writes through.
    REQUIRE(ctx->Load(u8"var h = House.new()\n"
                      u8"var r = h.room\n"
                      u8"r.size = 7\n"
                      u8"var S = h.room.size\n", // re-fetched borrow sees the write
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"S").Get<f64>() == 7.0);
}

TEST_CASE("wren: a UniquePtr (non-Object) container element is a borrow handle (unit 2b)")
{
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    RegisterHouse(*manager);
    manager->FinalizeTypes();
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // rooms_add() default-constructs a Room (homogeneous UniquePtr container) and hands back a borrow.
    REQUIRE(ctx->Load(u8"var s = Shelf.new()\n"
                      u8"var room = s.rooms_add()\n"
                      u8"room.size = 42\n"
                      u8"var C = s.rooms_count\n"
                      u8"var Z = s.rooms_at(0).size\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"C").Get<f64>() == 1.0);
    CHECK(ctx->GetGlobal(u8"Z").Get<f64>() == 42.0);
}

TEST_CASE("wren: a polymorphic container member binds as script ops (count/at/add/removeAt)")
{
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    RegisterZoo(*manager);
    manager->FinalizeTypes();
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // Add by element-type name, edit the returned handle, read it back through the container, remove.
    REQUIRE(ctx->Load(u8"var z = Zoo.new()\n"
                      u8"var C0 = z.animals_count\n"     // 0
                      u8"var d = z.animals_add(\"Dog\")\n" // -> Dog handle
                      u8"d.barks = 5\n"
                      u8"var C1 = z.animals_count\n"     // 1
                      u8"var B = z.animals_at(0).barks\n" // 5 (write-through to the same object)
                      u8"z.animals_add(\"Cat\")\n"
                      u8"var C2 = z.animals_count\n"     // 2
                      u8"z.animals_removeAt(0)\n"
                      u8"var C3 = z.animals_count\n",    // 1
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"C0").Get<f64>() == 0.0);
    CHECK(ctx->GetGlobal(u8"C1").Get<f64>() == 1.0);
    CHECK(ctx->GetGlobal(u8"B").Get<f64>() == 5.0);
    CHECK(ctx->GetGlobal(u8"C2").Get<f64>() == 2.0);
    CHECK(ctx->GetGlobal(u8"C3").Get<f64>() == 1.0);
}

TEST_CASE("wren: a default-constructed reflected type")
{
    RegisterFoundationTypes();
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    RegisterReflectedTypes(*manager);
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // Guid has a default ctor and (u64, u64); its props round-trip through doubles.
    REQUIRE(ctx->Load(u8"var g = Guid.new(7, 42)\n"
                      u8"var Hi = g.high\n"
                      u8"var Lo = g.low\n",
                      u8"main")
                .IsOk());
    CHECK(ctx->GetGlobal(u8"Hi").Get<f64>() == 7.0);
    CHECK(ctx->GetGlobal(u8"Lo").Get<f64>() == 42.0);
}

TEST_CASE("wren: call a script function with marshalled args")
{
    RefPtr<IScriptContext> ctx = wren::CreateScriptManager()->CreateContext();
    REQUIRE(ctx->Load(u8"var add = Fn.new { |a, b| a + b }\n"
                      u8"var greeting = Fn.new { \"hi\" }\n",
                      u8"main")
                .IsOk());

    CHECK(ctx->HasFunction(u8"add"));
    CHECK_FALSE(ctx->HasFunction(u8"nope"));

    // int args marshal to Wren numbers; result comes back as a double.
    Variant addArgs[] = {Variant::From(2), Variant::From(3)};
    CHECK(ctx->Call(u8"add", Span<Variant>{addArgs, 2}).Value().Get<f64>() == 5.0);

    // no-arg call returning a string.
    CHECK(ctx->Call(u8"greeting", Span<Variant>{}).Value().Get<String>() == u8"hi");

    // missing callable -> NotFound.
    CHECK(ctx->Call(u8"nope", Span<Variant>{}).Error() == ErrorCode::NotFound);
}

TEST_CASE("wren: instantiate a script class and invoke its methods")
{
    RefPtr<IScriptContext> ctx = wren::CreateScriptManager()->CreateContext();
    REQUIRE(
        ctx->Load(u8"class Counter {\n"
                  u8"  construct new(start) { _n = start }\n"
                  u8"  add(x) { _n = _n + x }\n"
                  u8"  value() { _n }\n" // a zero-arg method (Invoke models methods, not getters)
                  u8"  reset() { _n = 0 }\n"
                  u8"}\n",
                  u8"main")
            .IsOk());

    Variant ctorArgs[] = {Variant::From(10)};
    RefPtr<ScriptObject> counter = ctx->CreateInstance(u8"Counter", Span<Variant>{ctorArgs, 1});
    REQUIRE(static_cast<bool>(counter));

    Variant addArgs[] = {Variant::From(5)};
    CHECK(counter->Invoke(u8"add", Span<Variant>{addArgs, 1}).HasValue());

    // Zero-arg getter: signature has no parens, so call it by its bare name.
    CHECK(counter->Invoke(u8"value", Span<Variant>{}).Value().Get<f64>() == 15.0);

    CHECK(counter->Invoke(u8"reset", Span<Variant>{}).HasValue());
    CHECK(counter->Invoke(u8"value", Span<Variant>{}).Value().Get<f64>() == 0.0);
}

TEST_CASE("wren: CreateInstance returns null for an unknown class")
{
    RefPtr<IScriptContext> ctx = wren::CreateScriptManager()->CreateContext();
    REQUIRE(ctx->Load(u8"class Known { construct new() {} }\n", u8"main").IsOk());
    CHECK_FALSE(static_cast<bool>(ctx->CreateInstance(u8"Missing", Span<Variant>{})));
}

TEST_CASE("wren: a script object outlives the local context reference")
{
    RefPtr<ScriptObject> obj;
    {
        RefPtr<IScriptContext> ctx = wren::CreateScriptManager()->CreateContext();
        REQUIRE(ctx->Load(u8"class Echo {\n"
                          u8"  construct new() {}\n"
                          u8"  ping() { 42 }\n"
                          u8"}\n",
                          u8"main")
                    .IsOk());
        obj = ctx->CreateInstance(u8"Echo", Span<Variant>{});
        REQUIRE(static_cast<bool>(obj));
        // ctx goes out of scope here; obj retains it (keeps the VM alive).
    }
    CHECK(obj->Invoke(u8"ping", Span<Variant>{}).Value().Get<f64>() == 42.0);
}

#include "../Draconic.Script.Tests/BackendConformance.h"

TEST_CASE("wren: CERTIFIED - the backend conformance battery (scripting.md B2)")
{
    draconic::script::conformance::Dialect dialect;
    dialect.languageId = u8"wren";
    dialect.functionsModule = u8"var answer = 42\n"
                              u8"var add = Fn.new {|a, b| a + b }\n"
                              u8"var greeting = Fn.new { \"hi\" }\n";
    dialect.counterClass = u8"class Counter {\n"
                           u8"  construct new(n) { _n = n }\n"
                           u8"  increment() { _n = _n + 1 }\n"
                           u8"  value() { _n }\n"
                           u8"}\n";
    dialect.compileBroken = u8"var = = = @#$";
    dialect.runtimeFault = u8"Fiber.abort(\"conformance fault\")";
    // Delegate: subscribe a Wren fn/closure (value * 2) to the native DelegateSignal.
    dialect.delegateModule = u8"var signal = DelegateSignal.new()\n"
                             u8"signal.Connect(Fn.new {|x| x * 2 })\n";
    // Self-contained coroutine class: the `Behavior` base inlined (the subsystem injects
    // it at runtime; the raw battery does not), then a `Coro` that opts in. Coroutine
    // bodies use an explicit receiver (`me`) + getter/setter methods so no field is
    // touched inside a closure (a Wren restriction).
    dialect.coroutineClass = u8"class Behavior {\n"
                             u8"  construct new(entity) {\n"
                             u8"    _entity = entity\n"
                             u8"    _drCoroutines = []\n"
                             u8"  }\n"
                             u8"  startCoroutine(fn) {\n"
                             u8"    var fiber = Fiber.new(fn)\n"
                             u8"    var w = fiber.call()\n"
                             u8"    if (fiber.isDone) return -1\n"
                             u8"    if (!(w is Num)) w = 0\n"
                             u8"    var id = drRegisterCoroutine(fiber, w)\n"
                             u8"    _drCoroutines.add(id)\n"
                             u8"    return id\n"
                             u8"  }\n"
                             u8"  wait(seconds) { Fiber.yield(seconds) }\n"
                             u8"  waitUntil(fn) {\n"
                             u8"    while (!fn.call()) {\n"
                             u8"      Fiber.yield(0)\n"
                             u8"    }\n"
                             u8"  }\n"
                             u8"  foreign drRegisterCoroutine(fiber, w)\n"
                             u8"  foreign drUnregisterCoroutine(id)\n"
                             u8"  drCancelCoroutines() {\n"
                             u8"    for (id in _drCoroutines) {\n"
                             u8"      drUnregisterCoroutine(id)\n"
                             u8"    }\n"
                             u8"    _drCoroutines.clear()\n"
                             u8"  }\n"
                             u8"}\n"
                             u8"class Coro is Behavior {\n"
                             u8"  construct new() {\n"
                             u8"    super(null)\n"
                             u8"    _p = 0\n"
                             u8"    _gate = false\n"
                             u8"  }\n"
                             u8"  progress() { _p }\n"
                             u8"  flip() { _gate = true }\n"
                             u8"  markDone() { _p = 1 }\n"
                             u8"  gateOpen { _gate }\n"
                             u8"  begin() {\n"
                             u8"    var me = this\n"
                             u8"    startCoroutine(Fn.new {\n"
                             u8"      me.wait(1)\n"
                             u8"      me.markDone()\n"
                             u8"    })\n"
                             u8"  }\n"
                             u8"  beginUntil() {\n"
                             u8"    var me = this\n"
                             u8"    startCoroutine(Fn.new {\n"
                             u8"      me.waitUntil(Fn.new { me.gateOpen })\n"
                             u8"      me.markDone()\n"
                             u8"    })\n"
                             u8"  }\n"
                             u8"}\n";

    draconic::script::conformance::RunScriptBackendConformance(
        []() { return draconic::script::wren::CreateScriptManager(); }, dialect);
}

TEST_CASE("wren: declares the Coroutines + Delegates capabilities; seams absent (B4)")
{
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Coroutines));
    CHECK(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Delegates));
    // Committed-but-unimplemented seams stay ABSENT (their factories return null).
    CHECK_FALSE(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Debugger));
    CHECK_FALSE(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Profiler));
    CHECK_FALSE(HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Bytecode));
    CHECK(manager->CreateDebugger().Get() == nullptr);
    CHECK(manager->CreateProfiler().Get() == nullptr);
    CHECK(manager->CompileToBlob(u8"", u8"blob").Error() == ErrorCode::NotSupported);
}

TEST_CASE("wren: a script function is a native callback via IScriptDelegate (the real use)")
{
    RefPtr<IScriptManager> manager = wren::CreateScriptManager();
    manager->RegisterType(conformance::DelegateSignal::StaticType());
    RefPtr<IScriptContext> ctx = manager->CreateContext();

    // A behavior subscribes a closure to a native event; native code fires it.
    REQUIRE(ctx->Load(u8"var signal = DelegateSignal.new()\n"
                      u8"signal.Connect(Fn.new {|x| x + 5 })\n",
                      u8"main")
                .IsOk());

    Variant signalVar = ctx->GetGlobal(u8"signal");
    REQUIRE(signalVar.IsObject());
    conformance::DelegateSignal* signal = signalVar.AsObject<conformance::DelegateSignal>();
    REQUIRE(signal != nullptr);

    CHECK(signal->Emit(10.0) == doctest::Approx(15.0));   // native fires -> closure runs
    CHECK(signal->Emit(100.0) == doctest::Approx(105.0)); // reusable across firings
}
