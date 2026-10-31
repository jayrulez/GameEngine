// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include <cstring>

#include "Core/Debug/Assert.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;

using namespace foundation::core;

// --- A small reflected hierarchy for the RTTI tests ------------------------
namespace
{
    class Animal : public Object
    {
        RTTI_OBJECT(Animal, Object)
    public:
        int legs = 4;

        int AddLegs(int n)
        {
            legs += n;
            return legs;
        }
        int GetLegs() const { return legs; }
        void AssignLegs(int n) { legs = n; } // the accessor property's setter (SetLegs below is a Method)
        static int DefaultLegs() { return 4; }

        // Object-argument forms: pointer and owning RefPtr.
        int LegsOf(Animal* other) const { return other != nullptr ? other->legs : -1; }
        bool IsSame(RefPtr<Animal> other) const { return other.Get() == this; }

        // Two-param instance method: exercises named-parameter ordering.
        int SetLegs(int front, int back)
        {
            legs = front + back;
            return legs;
        }
    };

    class Dog : public Animal
    {
        RTTI_OBJECT(Dog, Animal)
    public:
        const char* Speak() const { return "woof"; }
    };

    class Cat : public Animal
    {
        RTTI_OBJECT(Cat, Animal)
    };

    // A nested reflected structure (Object-derived, so non-copyable via RefCounted), the shape of
    // MaterialSource inside MaterialAsset, which Property<> cannot handle.
    class NestedLeaf : public Object
    {
        RTTI_OBJECT(NestedLeaf, Object)
    public:
        int value = 7;
    };

    class NestedOwner : public Object
    {
        RTTI_OBJECT(NestedOwner, Object)
    public:
        NestedLeaf leaf;               // value member (non-copyable)
        NestedLeaf* leafPtr = nullptr; // pointer member (nested-by-pointer)
    };

    // Factory-style statics for the ReturnType-override test: bodies return a Variant whose dynamic
    // type is (or is not) the declared return type.
    Variant MakeAnimalVariant() { return Variant::FromObject(MakeRef<Animal>(DefaultAllocator())); }
    Variant MakeWrongVariant() { return Variant::From<int>(7); }
    // A narrowing factory taking any handle as a Variant, declared as taking an Animal.
    Variant NarrowAnimal(Variant handle, int tag)
    {
        (void)tag;
        return handle.Type() == &Animal::StaticType() ? handle : Variant{};
    }
}

REFLECT_MEMBERS(Animal, "rtti::test")
{
    builder.Property<&Animal::legs>("legs");
    builder.Method<&Animal::AddLegs>("AddLegs", {"count"}); // one authored parameter name
    builder.Method<&Animal::GetLegs>("GetLegs");
    builder.Method<&Animal::DefaultLegs>("DefaultLegs");
    builder.Method<&Animal::LegsOf>("LegsOf");           // takes Animal* (unnamed, stays "")
    builder.Method<&Animal::IsSame>("IsSame");           // takes RefPtr<Animal>
    builder.Method<&Animal::SetLegs>("SetLegs", {"front", "back"}); // named params, in order
    builder.ComputedProperty<&Animal::GetLegs>("legsView"); // computed read-only getter property
    builder.AccessorProperty<&Animal::GetLegs, &Animal::AssignLegs>("legsAccessor"); // getter and setter
    builder.Attribute("scriptName", "Critter");
    builder.Attribute("maxLegs", 8);
    builder.Constructor(); // default ctor -> RefPtr<Animal> via MakeRef
    // ReturnType-override factories: C++ returns a Variant, the declared return is Animal, and the
    // dispatch validates the runtime type.
    builder.Method<&MakeAnimalVariant, Animal>("makeAnimal");
    builder.Method<&MakeWrongVariant, Animal>("makeWrong"); // body returns int -> validated to empty
    builder.Method<&NarrowAnimal, Animal, Animal>("narrow"); // its Variant parameter declared Animal
}
RTTI_DEFINE_OBJECT(Dog, "rtti::test")
RTTI_DEFINE_OBJECT(Cat, "rtti::test")

enum class TestColor : int
{
    Red = 1,
    Green = 2,
    Blue = 4
};

REFLECT_ENUM(TestColor, "rtti::test")
{
    builder.Value("Red", TestColor::Red);
    builder.Value("Green", TestColor::Green);
    builder.Value("Blue", TestColor::Blue);
}

REFLECT_MEMBERS(NestedLeaf, "rtti::test")
{
    builder.Property<&NestedLeaf::value>("value");
}

namespace
{
    // A count-bound inline vector: a fixed C-array and a live count (the particle curve and collision shape).
    struct BoundedThing
    {
        int values[4]{};
        int count = 0;
    };
}
REFLECT_VALUE(BoundedThing, "rtti::test")
{
    builder.BoundedArray<&BoundedThing::values, &BoundedThing::count>("values");
}
REFLECT_MEMBERS(NestedOwner, "rtti::test")
{
    builder.Nested<&NestedOwner::leaf>("leaf");        // value member
    builder.Nested<&NestedOwner::leafPtr>("leafPtr");  // pointer member (pointee via address)
}

namespace
{
    // A plain (non-Object), non-copyable reflected value in the ParticleSystem shape: held only in
    // an Array<UniquePtr<T>>, reached by address (not by Variant copy). The UniquePtr member makes it
    // genuinely move-only, so the container must never try to copy an element.
    struct UniqueLeaf
    {
        int value = 0;
        UniquePtr<int> tag; // makes UniqueLeaf move-only
    };
}
REFLECT_VALUE(UniqueLeaf, "rtti::test")
{
    builder.Property<&UniqueLeaf::value>("value");
}

// --- RTTI ------------------------------------------------------------------

TEST_CASE("rtti: stable type identity")
{
    CHECK(Dog::StaticType().id == Dog::StaticType().id);
    CHECK(Dog::StaticType().id != Cat::StaticType().id);
    CHECK(Dog::StaticType().base == &Animal::StaticType());
    CHECK(Animal::StaticType().base == &Object::StaticType());
    CHECK(Object::StaticType().base == nullptr);

    CHECK(std::strcmp(Dog::StaticType().name, "Dog") == 0);
    CHECK(std::strcmp(Dog::StaticType().namespaceName, "rtti::test") == 0);
}

TEST_CASE("rtti: GetType is virtual through a base pointer")
{
    RefPtr<Dog> dog = MakeRef<Dog>(DefaultAllocator());
    RefPtr<Object> asObject = dog; // upcast

    CHECK(asObject->GetType() == &Dog::StaticType());
    CHECK(dog->GetType()->id == Dog::StaticType().id);
}

TEST_CASE("rtti: object arguments marshal through reflected methods")
{
    RefPtr<Animal> self = MakeRef<Animal>(DefaultAllocator());
    self->legs = 6;
    RefPtr<Animal> other = MakeRef<Animal>(DefaultAllocator());
    other->legs = 4;
    Instance inst = Instance::From(self.Get());

    // U* parameter: pass an object-mode Variant.
    const MethodInfo* legsOf = FindMethod(Animal::StaticType(), "LegsOf");
    REQUIRE(legsOf != nullptr);
    CHECK(ParamAt(*legsOf, 0).type() == &Animal::StaticType()); // object's static type
    Variant otherArg[] = {Variant::From(other)};
    CHECK(InvokeMethod(*legsOf, inst, Span<Variant>{otherArg, 1}).Value().Get<int>() == 4);

    // Polymorphic: a Dog is accepted where Animal* is expected.
    RefPtr<Dog> dog = MakeRef<Dog>(DefaultAllocator()); // legs == 4
    Variant dogArg[] = {Variant::From(dog)};
    CHECK(InvokeMethod(*legsOf, inst, Span<Variant>{dogArg, 1}).Value().Get<int>() == 4);

    // RefPtr<U> parameter (owning): IsSame(self) -> true.
    Variant selfArg[] = {Variant::From(self)};
    CHECK(InvokeMethod(*FindMethod(Animal::StaticType(), "IsSame"), inst, Span<Variant>{selfArg, 1})
              .Value()
              .Get<bool>());

    // A non-object (value) argument is rejected.
    Variant valueArg[] = {Variant::From(5)};
    CHECK_FALSE(InvokeMethod(*legsOf, inst, Span<Variant>{valueArg, 1}).HasValue());

    // No leak from the call: self is held only by `self` and `selfArg` (== 2);
    // the RefPtr<Animal> param copy made during the call was released (else 3).
    CHECK(self->RefCount() == 2u);
}

TEST_CASE("rtti: Construct an Object-derived type via reflection")
{
    Result<Variant> created = Construct(Animal::StaticType(), Span<Variant>{});
    REQUIRE(created.HasValue());
    Variant& v = created.Value();
    CHECK(v.IsObject()); // object mode (RefPtr<Animal>)
    CHECK(v.Type() == &Animal::StaticType());
    Animal* animal = v.AsObject<Animal>();
    REQUIRE(animal != nullptr);
    CHECK(animal->legs == 4);
    CHECK(animal->RefCount() == 1u); // the Variant owns the only ref
}

TEST_CASE("variant: object mode owns a ref and reports the dynamic type")
{
    RefPtr<Dog> dog = MakeRef<Dog>(DefaultAllocator());
    CHECK(dog->RefCount() == 1u);

    {
        // From a RefPtr<Dog> -> object mode (auto-detected).
        Variant v = Variant::From(dog);
        CHECK(v.IsObject());
        CHECK(dog->RefCount() == 2u);          // Variant owns a strong ref
        CHECK(v.Type() == &Dog::StaticType()); // dynamic type, not RefPtr<Object>

        // Borrowed access, with down/up-cast.
        CHECK(v.AsObject() != nullptr);
        CHECK(v.AsObject<Animal>() != nullptr); // upcast
        CHECK(v.AsObject<Dog>() != nullptr);
        CHECK(v.AsObject<Cat>() == nullptr); // wrong branch

        // Copy shares ownership; move transfers it.
        Variant copy = v;
        CHECK(dog->RefCount() == 3u);
        CHECK(copy.Type() == &Dog::StaticType());
        Variant moved = Move(copy);
        CHECK(dog->RefCount() == 3u); // moved, not added
        CHECK(moved.AsObject<Dog>() != nullptr);
    }
    CHECK(dog->RefCount() == 1u); // all Variants released
}

TEST_CASE("variant: a null object ref falls back to the static type")
{
    Variant v = Variant::From(RefPtr<Dog>{});
    CHECK(v.IsObject());
    CHECK(v.Type() == &Dog::StaticType());
    CHECK(v.AsObject() == nullptr);
    CHECK(v.AsObject<Dog>() == nullptr);
}

TEST_CASE("rtti: Cast and IsA walk the inheritance chain")
{
    RefPtr<Dog> dog = MakeRef<Dog>(DefaultAllocator());
    Object* obj = dog.Get();

    CHECK(IsA<Dog>(obj));
    CHECK(IsA<Animal>(obj)); // up the chain
    CHECK(IsA<Object>(obj));
    CHECK_FALSE(IsA<Cat>(obj));

    // Down/cross casts
    REQUIRE(Cast<Animal>(obj) != nullptr);
    REQUIRE(Cast<Dog>(obj) != nullptr);
    CHECK(Cast<Cat>(obj) == nullptr);

    // Casting preserves the object and lets us call derived API.
    Dog* backToDog = Cast<Dog>(Cast<Animal>(obj));
    REQUIRE(backToDog != nullptr);
    CHECK(std::strcmp(backToDog->Speak(), "woof") == 0);

    CHECK_FALSE(IsA<Dog>(static_cast<Object*>(nullptr)));
}

TEST_CASE("rtti: explicit registration and lookup")
{
    TypeRegistry& registry = GlobalTypeRegistry();
    const usize before = registry.Count();

    registry.Register(Object::StaticType());
    registry.Register(Animal::StaticType());
    registry.Register(Dog::StaticType());
    registry.Register(Cat::StaticType());

    CHECK(registry.Count() >= before + 4);

    CHECK(registry.FindById(Dog::StaticType().id) == &Dog::StaticType());
    CHECK(registry.FindByName("rtti::test", "Cat") == &Cat::StaticType());
    CHECK(registry.FindByName("rtti::test", "Missing") == nullptr);

    // Idempotent: re-registering does not duplicate.
    const usize count = registry.Count();
    registry.Register(Dog::StaticType());
    CHECK(registry.Count() == count);
}

// --- RTTI: Variant ---------------------------------------------------------

TEST_CASE("variant: holds small values inline")
{
    Variant v = Variant::From(42);
    CHECK_FALSE(v.IsEmpty());
    CHECK(v.Is<int>());
    CHECK_FALSE(v.Is<float>());

    REQUIRE(v.TryGet<int>() != nullptr);
    CHECK(*v.TryGet<int>() == 42);
    CHECK(v.TryGet<float>() == nullptr);
    CHECK(v.Get<int>() == 42);

    CHECK(v.Type() == &TypeOf<int>());
}

TEST_CASE("variant: holds a Float3 and a large (heap) value")
{
    Variant small = Variant::From(Float3{1.0f, 2.0f, 3.0f});
    REQUIRE(small.Is<Float3>());
    CHECK(*small.TryGet<Float3>() == Float3{1.0f, 2.0f, 3.0f});

    Variant large = Variant::From(Float4x4::Translation(Float3{5.0f, 0.0f, 0.0f}));
    REQUIRE(large.Is<Float4x4>());
    CHECK(large.TryGet<Float4x4>()->m[3][0] == 5.0f);
}

TEST_CASE("variant: copy and move are independent")
{
    Variant a = Variant::From(7);
    Variant b = a; // copy
    *b.TryGet<int>() = 99;
    CHECK(*a.TryGet<int>() == 7);
    CHECK(*b.TryGet<int>() == 99);

    Variant c = Move(b); // move
    CHECK(*c.TryGet<int>() == 99);
    CHECK(b.IsEmpty());

    a.Reset();
    CHECK(a.IsEmpty());
}

TEST_CASE("variant: manages non-trivial payload lifetimes")
{
    struct Tracked
    {
        static int& Live()
        {
            static int n = 0;
            return n;
        }
        int value;
        explicit Tracked(int v = 0) : value(v) { ++Live(); }
        Tracked(const Tracked& o) : value(o.value) { ++Live(); }
        Tracked(Tracked&& o) noexcept : value(o.value) { ++Live(); }
        ~Tracked() { --Live(); }
    };

    Tracked::Live() = 0;
    {
        Variant v = Variant::From(Tracked{5});
        CHECK(Tracked::Live() == 1);
        Variant copy = v;
        CHECK(Tracked::Live() == 2);
        CHECK(copy.TryGet<Tracked>()->value == 5);
    }
    CHECK(Tracked::Live() == 0);
}

TEST_CASE("variant: Instance borrows without owning")
{
    int x = 17;
    Instance inst = Instance::From(&x);
    CHECK_FALSE(inst.IsEmpty());
    CHECK(inst.Type() == &TypeOf<int>());

    REQUIRE(inst.TryGet<int>() != nullptr);
    CHECK(*inst.TryGet<int>() == 17);
    *inst.TryGet<int>() = 23;
    CHECK(x == 23); // writes through to the borrowed object

    CHECK(inst.TryGet<float>() == nullptr); // wrong type
}

// --- RTTI: properties ------------------------------------------------------

TEST_CASE("rtti: reflected property is discoverable")
{
    CHECK(Properties(Animal::StaticType()).Size() == 3u); // "legs" (field), "legsView" (computed), "legsAccessor"

    const PropertyInfo* legs = FindProperty(Animal::StaticType(), "legs");
    REQUIRE(legs != nullptr);
    CHECK(std::strcmp(legs->name, "legs") == 0);
    CHECK(legs->type == &TypeOf<int>());

    CHECK(FindProperty(Animal::StaticType(), "missing") == nullptr);
}

TEST_CASE("rtti: property get/set through an Instance")
{
    RefPtr<Animal> animal = MakeRef<Animal>(DefaultAllocator());
    const PropertyInfo* legs = FindProperty(Animal::StaticType(), "legs");
    REQUIRE(legs != nullptr);

    Instance inst = Instance::From(animal.Get());

    Variant got = GetProperty(*legs, inst);
    REQUIRE(got.Is<int>());
    CHECK(got.Get<int>() == 4); // default

    CHECK(SetProperty(*legs, inst, Variant::From(6)).IsOk());
    CHECK(animal->legs == 6); // mutated the real object
    CHECK(GetProperty(*legs, inst).Get<int>() == 6);

    // Wrong-typed value -> error, object unchanged.
    Status bad = SetProperty(*legs, inst, Variant::From(3.5f));
    CHECK_FALSE(bad.IsOk());
    CHECK(bad.Code() == ErrorCode::InvalidArgument);
    CHECK(animal->legs == 6);
}

TEST_CASE("rtti: an AccessorProperty reads through its getter and writes through its setter")
{
    const PropertyInfo* accessor = FindProperty(Animal::StaticType(), "legsAccessor");
    REQUIRE(accessor != nullptr);
    CHECK(accessor->type == &TypeOf<int>());
    CHECK(accessor->address == nullptr); // no in-place editing: every write is the setter's
    CHECK((static_cast<u32>(accessor->flags) & static_cast<u32>(PropertyFlags::ReadOnly)) == 0);

    RefPtr<Animal> animal = MakeRef<Animal>(DefaultAllocator());
    Instance inst = Instance::From(animal.Get());
    animal->legs = 7;
    CHECK(GetProperty(*accessor, inst).Get<int>() == 7);
    CHECK(SetProperty(*accessor, inst, Variant::From<int>(3)).IsOk());
    CHECK(animal->legs == 3);
    // The declared type, exactly: a float is refused and the object untouched.
    const Status wrong = SetProperty(*accessor, inst, Variant::From<f32>(2.0f));
    CHECK_FALSE(wrong.IsOk());
    CHECK(wrong.Code() == ErrorCode::InvalidArgument);
    CHECK(animal->legs == 3);
}

TEST_CASE("rtti: a ComputedProperty is a read-only getter-backed property")
{
    const PropertyInfo* view = FindProperty(Animal::StaticType(), "legsView");
    REQUIRE(view != nullptr);
    CHECK(view->type == &TypeOf<int>());
    CHECK(view->address == nullptr); // computed: no field address to edit in place
    CHECK((static_cast<u32>(view->flags) & static_cast<u32>(PropertyFlags::ReadOnly)) != 0);

    RefPtr<Animal> animal = MakeRef<Animal>(DefaultAllocator());
    Instance inst = Instance::From(animal.Get());

    animal->legs = 7;
    CHECK(GetProperty(*view, inst).Get<int>() == 7); // reads the live computed value
    animal->legs = 9;
    CHECK(GetProperty(*view, inst).Get<int>() == 9); // recomputed on each read

    // set is not supported on a computed getter; the object is untouched.
    const Status st = SetProperty(*view, inst, Variant::From<int>(3));
    CHECK_FALSE(st.IsOk());
    CHECK(st.Code() == ErrorCode::NotSupported);
    CHECK(animal->legs == 9);
}

namespace
{
    // A reference-shaped value of this test's own: identified by its guid, read by identity,
    // pointing at a FakeTarget.
    struct FakeTarget
    {
        int payload = 0;
    };
    struct FakeRef
    {
        Guid id;
        int cached = 0;
    };
    struct RefHolder
    {
        FakeRef one;
        Array<FakeRef> many;
    };
}
namespace foundation::core
{
    template <>
    struct ReferenceTraits<FakeRef>
    {
        static constexpr bool isReference = true;
        [[nodiscard]] static const ReferenceOps& Ops() noexcept
        {
            static constexpr ReferenceOps ops{
                [](const void* value) -> const Guid* { return &static_cast<const FakeRef*>(value)->id; },
                [](void* value, const Guid& id) { static_cast<FakeRef*>(value)->id = id; },
                [](void* value) { static_cast<FakeRef*>(value)->cached = 0; },
                []() -> const TypeInfo* { return &TypeOf<FakeTarget>(); }};
            return ops;
        }
    };
}
REFLECT_VALUE(RefHolder, "rtti::test")
{
    builder.Property<&RefHolder::one>("one").Nested<&RefHolder::many>("many");
}

namespace
{
    struct Gauge
    {
        int reading = 3; // a stored field the type publishes read-only
        int knob = 0;
    };
}
REFLECT_VALUE(Gauge, "rtti::test")
{
    builder.Property<&Gauge::reading>("reading", PropertyFlags::ReadOnly).Property<&Gauge::knob>("knob");
}
void RttiRegisterValue_Gauge(); // emitted by REFLECT_VALUE above

TEST_CASE("rtti: a read-only stored property reads but refuses SetProperty - the flag is a contract")
{
    RttiRegisterValue_Gauge();
    Gauge gauge;
    const Instance inst = Instance::From(&gauge);
    const PropertyInfo* reading = FindProperty(TypeOf<Gauge>(), "reading");
    const PropertyInfo* knob = FindProperty(TypeOf<Gauge>(), "knob");
    REQUIRE(reading != nullptr);
    REQUIRE(knob != nullptr);
    CHECK(GetProperty(*reading, inst).Get<int>() == 3);
    CHECK(SetProperty(*reading, inst, Variant::From<int>(9)).Code() == ErrorCode::NotSupported);
    CHECK(gauge.reading == 3);
    CHECK(SetProperty(*knob, inst, Variant::From<int>(9)).IsOk());
    CHECK(gauge.knob == 9);
    // The raw address is the intended way around it (an editor's in-place tool).
    REQUIRE(reading->address != nullptr);
    *static_cast<int*>(reading->address(inst)) = 4;
    CHECK(GetProperty(*reading, inst).Get<int>() == 4);
}

void RttiRegisterValue_RefHolder(); // emitted by REFLECT_VALUE above

TEST_CASE("rtti: a reference-shaped value type is read by identity through its TypeInfo - as a "
          "Variant, a property and a container element - and every other type is not")
{
    RttiRegisterValue_RefHolder();
    RegisterArrayType<FakeRef>(); // the list's container reflection, as a component registers its lists
    const TypeInfo& refType = TypeOf<FakeRef>();
    REQUIRE(IsReferenceType(refType));
    CHECK(refType.reference->Target() == &TypeOf<FakeTarget>()); // what the guid names
    CHECK_FALSE(IsReferenceType(TypeOf<int>()));
    CHECK_FALSE(IsReferenceType(TypeOf<RefHolder>()));

    Random rng(3);
    RefHolder holder;
    holder.one.id = Guid::Generate(rng);
    holder.many.PushBack(FakeRef{Guid::Generate(rng), 1});
    holder.many.PushBack(FakeRef{Guid::Generate(rng), 2});
    const Instance inst = Instance::From(&holder);

    // The property's Variant: its type says reference, the reader gives the id inside.
    const PropertyInfo* one = FindProperty(TypeOf<RefHolder>(), "one");
    REQUIRE(one != nullptr);
    CHECK(IsReferenceType(*one->type));
    const Variant value = GetProperty(*one, inst);
    REQUIRE(IsReferenceType(*value.Type()));
    CHECK(*value.Type()->reference->Id(value.ValuePointer()) == holder.one.id);
    // A container of them: each element the same way.
    const PropertyInfo* many = FindProperty(TypeOf<RefHolder>(), "many");
    REQUIRE(many != nullptr);
    REQUIRE(many->type->container != nullptr);
    CHECK(IsReferenceType(*many->type->container->elementType));
    const Instance manyInst(many->address(inst), many->type);
    REQUIRE(ContainerSize(*many->type->container, manyInst) == 2u);
    const Variant second = ContainerGetAt(*many->type->container, manyInst, 1);
    CHECK(*second.Type()->reference->Id(second.ValuePointer()) == holder.many[1].id);
    // Writing by identity: the id set in place, the runtime binding dropped.
    const Guid fresh = Guid::Generate(rng);
    holder.one.cached = 7;
    refType.reference->SetId(one->address(inst), fresh);
    refType.reference->ClearBinding(one->address(inst));
    CHECK(holder.one.id == fresh);
    CHECK(holder.one.cached == 0);
}

TEST_CASE("rtti: a Nested property recurses into a non-copyable member via address")
{
    const PropertyInfo* leafProp = FindProperty(NestedOwner::StaticType(), "leaf");
    REQUIRE(leafProp != nullptr);
    CHECK(IsNested(*leafProp));
    CHECK(leafProp->type == &NestedLeaf::StaticType()); // the nested type, to recurse into

    NestedOwner owner;
    owner.leaf.value = 42;
    Instance ownerInst = Instance::From(&owner);

    // A nested member is never marshalled by value: get is empty, set is unsupported.
    CHECK(GetProperty(*leafProp, ownerInst).IsEmpty());
    CHECK_FALSE(SetProperty(*leafProp, ownerInst, Variant{}).IsOk());

    // Recurse: the address gives the live member in place, then read its own reflected property.
    void* leafAddr = leafProp->address(ownerInst);
    REQUIRE(leafAddr != nullptr);
    const Instance leafInst(leafAddr, leafProp->type);
    const PropertyInfo* valueProp = FindProperty(*leafProp->type, "value");
    REQUIRE(valueProp != nullptr);
    CHECK(GetProperty(*valueProp, leafInst).Get<int>() == 42);

    // A pointer nested member: null yields a null address (consumers must null-check); once set,
    // address is the pointee, and recursion reads through it.
    const PropertyInfo* ptrProp = FindProperty(NestedOwner::StaticType(), "leafPtr");
    REQUIRE(ptrProp != nullptr);
    CHECK(IsNested(*ptrProp));
    CHECK(ptrProp->type == &NestedLeaf::StaticType());
    CHECK(ptrProp->address(ownerInst) == nullptr); // null pointer member -> null address

    NestedLeaf other;
    other.value = 99;
    owner.leafPtr = &other;
    void* ptrAddr = ptrProp->address(ownerInst);
    CHECK(ptrAddr == &other); // the pointee, not the pointer's own address
    const Instance ptrInst(ptrAddr, ptrProp->type);
    CHECK(GetProperty(*valueProp, ptrInst).Get<int>() == 99);
}

void RttiRegisterValue_BoundedThing(); // emitted by REFLECT_VALUE above

TEST_CASE("rtti: a BoundedArray reflects a count-bound C-array as a clamped container")
{
    RttiRegisterValue_BoundedThing(); // patches TypeOf<BoundedThing> with its reflected surface
    const PropertyInfo* prop = FindProperty(TypeOf<BoundedThing>(), "values");
    REQUIRE(prop != nullptr);
    CHECK(IsNested(*prop));      // structural: harvest skips it, tooling recurses
    REQUIRE(prop->type != nullptr);
    REQUIRE(IsContainer(*prop->type)); // ... and it is a container to iterate

    BoundedThing t;
    t.values[0] = 10;
    t.values[1] = 20;
    t.count = 2;
    Instance inst = Instance::From(&t);
    const Instance sub(prop->address(inst), prop->type); // address = the owner identity
    const ContainerInfo& c = *prop->type->container;

    // Size follows the count, not the capacity, which it reports (an editor's "full"); elements
    // read/write through in place.
    CHECK(ContainerSize(c, sub) == 2u);
    CHECK(ContainerCapacity(c) == 4u);
    CHECK(ContainerGetAt(c, sub, 0).Get<int>() == 10);
    CHECK(ContainerGetAt(c, sub, 1).Get<int>() == 20);
    CHECK(ContainerSetAt(c, sub, 0, Variant::From(99)).IsOk());
    CHECK(t.values[0] == 99);

    // A corrupt / oversized count clamps to the capacity N (never reads out of bounds);
    // a negative count clamps to zero.
    t.count = 100;
    CHECK(ContainerSize(c, sub) == 4u);
    t.count = -5;
    CHECK(ContainerSize(c, sub) == 0u);
}

TEST_CASE("rtti: container mutation - emplaceDefault / removeAt / moveElement")
{
    // Homogeneous Array<int>.
    RegisterArrayType<int>();
    Array<int> arr;
    arr.PushBack(10);
    arr.PushBack(20);
    arr.PushBack(30);
    const ContainerInfo& c = *TypeOf<Array<int>>().container;
    Instance inst = Instance::From(&arr);
    CHECK(ContainerCapacity(c) == 0u); // an Array has no limit

    // Homogeneous value elements are also reachable by address (the list-editor descent path).
    CHECK(ContainerAddressAt(c, inst, 1).Pointer() == &arr[1]);
    CHECK(ContainerAddressAt(c, inst, 1).Type() == &TypeOf<int>());
    CHECK(ContainerAddressAt(c, inst, 9).Pointer() == nullptr); // out of range

    (void)ContainerEmplaceDefault(c, inst, 1); // [10, 0, 20, 30]
    REQUIRE(arr.Size() == 4u);
    CHECK(arr[1] == 0);
    CHECK(ContainerRemoveAt(c, inst, 1).IsOk()); // [10, 20, 30]
    REQUIRE(arr.Size() == 3u);
    CHECK(arr[1] == 20);
    CHECK(ContainerMoveElement(c, inst, 0, 2).IsOk()); // [20, 30, 10]
    CHECK(arr[0] == 20);
    CHECK(arr[1] == 30);
    CHECK(arr[2] == 10);
    CHECK_FALSE(ContainerRemoveAt(c, inst, 9).IsOk()); // out of range

    // BoundedArray mutation respects the count and capacity.
    RttiRegisterValue_BoundedThing();
    const PropertyInfo* prop = FindProperty(TypeOf<BoundedThing>(), "values");
    REQUIRE(prop != nullptr);
    const ContainerInfo& bc = *prop->type->container;
    BoundedThing t;
    t.values[0] = 5;
    t.count = 1;
    Instance ti = Instance::From(&t);
    const Instance sub(prop->address(ti), prop->type);

    (void)ContainerEmplaceDefault(bc, sub, 0); // count 2: [0, 5]
    CHECK(t.count == 2);
    CHECK(t.values[0] == 0);
    CHECK(t.values[1] == 5);
    (void)ContainerEmplaceDefault(bc, sub, 2); // count 3
    (void)ContainerEmplaceDefault(bc, sub, 3); // count 4 == N
    CHECK(t.count == 4);
    CHECK(ContainerEmplaceDefault(bc, sub, 4).Pointer() == nullptr); // full -> fails cleanly
    CHECK(t.count == 4);
    CHECK(ContainerRemoveAt(bc, sub, 1).IsOk()); // count 3
    CHECK(t.count == 3);
}

TEST_CASE("rtti: a polymorphic container resolves each element to its DYNAMIC type")
{
    RegisterPolymorphicArrayType<Animal>();
    const TypeInfo& t = TypeOf<Array<RefPtr<Animal>>>();
    REQUIRE(IsContainer(t));
    const ContainerInfo& c = *t.container;
    CHECK(IsPolymorphicContainer(c));
    CHECK(c.elementType == &Animal::StaticType()); // static base

    Array<RefPtr<Animal>> arr;
    RefPtr<Dog> dog = MakeRef<Dog>(DefaultAllocator());
    dog->legs = 4;
    RefPtr<Cat> cat = MakeRef<Cat>(DefaultAllocator());
    cat->legs = 3;
    arr.PushBack(dog);
    arr.PushBack(cat);
    arr.PushBack(RefPtr<Animal>{}); // a null element
    Instance inst = Instance::From(&arr);

    CHECK(ContainerSize(c, inst) == 3u);

    // Element 0 is a Dog (dynamic), element 1 a Cat: each reflects its own concrete type.
    Variant e0 = ContainerGetAt(c, inst, 0);
    REQUIRE(e0.IsObject());
    CHECK(e0.Type() == &Dog::StaticType());
    const Instance i0(e0.AsObject(), e0.Type());
    CHECK(GetProperty(*FindProperty(*e0.Type(), "legs"), i0).Get<int>() == 4);

    Variant e1 = ContainerGetAt(c, inst, 1);
    CHECK(e1.Type() == &Cat::StaticType());

    // Null element -> empty Variant (consumers null-check).
    CHECK(ContainerGetAt(c, inst, 2).IsEmpty());

    // setAt is unsupported (elements are non-copyable).
    CHECK_FALSE(ContainerSetAt(c, inst, 0, Variant{}).IsOk());

    // This container was registered without a create-by-type factory, so create is rejected (empty
    // Instance) and nothing is eligible. removeAt and moveElement still work.
    CHECK(ContainerCreateElement(c, inst, 0, Dog::StaticType()).Pointer() == nullptr);
    CHECK_FALSE(ContainerCanCreateElement(c, Dog::StaticType()));
    CHECK(ContainerSize(c, inst) == 3u); // unchanged
}

TEST_CASE("rtti: an addressAt descent reaches a polymorphic element without a Variant copy")
{
    // Same container as above, exercised through the uniform address path: addressAt yields a
    // borrowed Instance at the element's dynamic type, so a consumer recurses the concrete props.
    RegisterPolymorphicArrayType<Animal>();
    const ContainerInfo& c = *TypeOf<Array<RefPtr<Animal>>>().container;
    Array<RefPtr<Animal>> arr;
    RefPtr<Dog> dog = MakeRef<Dog>(DefaultAllocator());
    dog->legs = 5;
    arr.PushBack(dog);
    arr.PushBack(RefPtr<Animal>{}); // null element
    Instance inst = Instance::From(&arr);

    Instance a0 = ContainerAddressAt(c, inst, 0);
    REQUIRE(a0.Pointer() != nullptr);
    CHECK(a0.Type() == &Dog::StaticType());
    CHECK(GetProperty(*FindProperty(*a0.Type(), "legs"), a0).Get<int>() == 5);
    CHECK(ContainerAddressAt(c, inst, 1).Pointer() == nullptr); // null element
    CHECK(ContainerAddressAt(c, inst, 9).Pointer() == nullptr); // out of range
}

TEST_CASE("rtti: an Array<UniquePtr<T>> reflects a move-only value via addressAt")
{
    RttiRegisterValue_UniqueLeaf();
    RegisterUniquePtrArrayType<UniqueLeaf>();
    const TypeInfo& t = TypeOf<Array<UniquePtr<UniqueLeaf>>>();
    REQUIRE(IsContainer(t));
    const ContainerInfo& c = *t.container;
    CHECK_FALSE(IsPolymorphicContainer(c));
    CHECK(c.elementType == &TypeOf<UniqueLeaf>());

    Array<UniquePtr<UniqueLeaf>> arr;
    UniquePtr<UniqueLeaf> a = MakeUnique<UniqueLeaf>(DefaultAllocator());
    a->value = 11;
    UniquePtr<UniqueLeaf> b = MakeUnique<UniqueLeaf>(DefaultAllocator());
    b->value = 22;
    arr.PushBack(Move(a));
    arr.PushBack(Move(b));
    Instance inst = Instance::From(&arr);

    CHECK(ContainerSize(c, inst) == 2u);
    // getAt yields nothing (move-only, no Variant); descent is by address, like Nested.
    CHECK(ContainerGetAt(c, inst, 0).IsEmpty());

    Instance e0 = ContainerAddressAt(c, inst, 0);
    REQUIRE(e0.Pointer() != nullptr);
    CHECK(e0.Type() == &TypeOf<UniqueLeaf>());
    const PropertyInfo* valueProp = FindProperty(*e0.Type(), "value");
    REQUIRE(valueProp != nullptr);
    CHECK(GetProperty(*valueProp, e0).Get<int>() == 11);
    CHECK(GetProperty(*valueProp, ContainerAddressAt(c, inst, 1)).Get<int>() == 22);

    // Mutation: default-emplace grows the array, remove and move reorder (no copies).
    Instance grown = ContainerEmplaceDefault(c, inst, 2);
    CHECK(grown.Pointer() != nullptr);
    CHECK(ContainerSize(c, inst) == 3u);
    CHECK(GetProperty(*valueProp, ContainerAddressAt(c, inst, 2)).Get<int>() == 0);
    CHECK(ContainerMoveElement(c, inst, 0, 2).IsOk()); // 11 -> end
    CHECK(GetProperty(*valueProp, ContainerAddressAt(c, inst, 2)).Get<int>() == 11);
    CHECK(ContainerRemoveAt(c, inst, 0).IsOk());
    CHECK(ContainerSize(c, inst) == 2u);
    // setAt stays unsupported for an owned, move-only element.
    CHECK_FALSE(ContainerSetAt(c, inst, 0, Variant{}).IsOk());
}

TEST_CASE("rtti: a Variant borrow edits a nested value in place, pins its root, survives value writes")
{
    RefPtr<NestedOwner> owner = MakeRef<NestedOwner>(DefaultAllocator());
    owner->leaf.value = 3;
    Variant parent = Variant::From(owner); // object-mode root handle
    void* leafAddr = &owner->leaf;

    Variant borrow = Variant::Borrow(leafAddr, &NestedLeaf::StaticType(), parent);
    REQUIRE(borrow.IsBorrow());
    CHECK_FALSE(borrow.IsObject());          // a borrow is not an owned object handle
    CHECK(borrow.AsObject() == nullptr);     // ... and never hands back the keep-alive root
    CHECK(borrow.BorrowRoot() == owner.Get()); // the pinned root is reachable only by name
    CHECK(borrow.Type() == &NestedLeaf::StaticType());

    const PropertyInfo* vp = FindProperty(NestedLeaf::StaticType(), "value");
    REQUIRE(vp != nullptr);

    // A read and an in-place write through the borrow reach the real subobject.
    CHECK(GetProperty(*vp, ToInstance(borrow)).Get<int>() == 3);
    CHECK(SetProperty(*vp, ToInstance(borrow), Variant::From(9)).IsOk());
    CHECK(owner->leaf.value == 9);
    CHECK(borrow.BorrowValid()); // a value write does not invalidate a borrow (only structural ops)

    // Keep-alive: drop the caller's ref and the parent handle; the borrow's own keep-alive keeps the
    // graph alive, so the edit target is still valid.
    NestedOwner* raw = owner.Get();
    owner.Reset();
    parent = Variant{};
    Instance stillLive = ToInstance(borrow);
    REQUIRE(stillLive.Pointer() == &raw->leaf);
    CHECK(GetProperty(*vp, stillLive).Get<int>() == 9);
}

TEST_CASE("rtti: a Variant borrow goes stale on structural container mutation (no UAF)")
{
    RttiRegisterValue_UniqueLeaf();
    RegisterUniquePtrArrayType<UniqueLeaf>();
    const ContainerInfo& c = *TypeOf<Array<UniquePtr<UniqueLeaf>>>().container;
    Array<UniquePtr<UniqueLeaf>> arr;
    UniquePtr<UniqueLeaf> a = MakeUnique<UniqueLeaf>(DefaultAllocator());
    a->value = 11;
    arr.PushBack(Move(a));
    Instance arrInst = Instance::From(&arr);

    RefPtr<Animal> root = MakeRef<Animal>(DefaultAllocator()); // any object to pin
    Variant parent = Variant::From(root);

    const Instance elem = ContainerAddressAt(c, arrInst, 0);
    Variant borrow = Variant::Borrow(elem.Pointer(), &TypeOf<UniqueLeaf>(), parent);
    REQUIRE(borrow.BorrowValid());
    const PropertyInfo* vp = FindProperty(TypeOf<UniqueLeaf>(), "value");
    CHECK(GetProperty(*vp, ToInstance(borrow)).Get<int>() == 11);

    // A structural mutation bumps the generation, so the borrow is stale and dereferences to an
    // empty Instance rather than the freed pointee (ASAN would flag a use after free if it did).
    CHECK(ContainerRemoveAt(c, arrInst, 0).IsOk());
    CHECK_FALSE(borrow.BorrowValid());
    CHECK(ToInstance(borrow).Pointer() == nullptr);

    // An emplace (also structural) would likewise invalidate a fresh borrow.
    UniquePtr<UniqueLeaf> b = MakeUnique<UniqueLeaf>(DefaultAllocator());
    b->value = 22;
    arr.PushBack(Move(b));
    Variant borrow2 = Variant::Borrow(ContainerAddressAt(c, arrInst, 0).Pointer(),
                                      &TypeOf<UniqueLeaf>(), parent);
    REQUIRE(borrow2.BorrowValid());
    (void)ContainerEmplaceDefault(c, arrInst, 1);
    CHECK_FALSE(borrow2.BorrowValid());
}

TEST_CASE("rtti: a value-mode parent refuses to yield a borrow")
{
    Variant valueParent = Variant::From(42); // not an object: nothing to pin
    int scratch = 0;
    Variant borrow = Variant::Borrow(&scratch, &TypeOf<int>(), valueParent);
    CHECK(borrow.IsEmpty());
    CHECK_FALSE(borrow.IsBorrow());
}

TEST_CASE("rtti: inherited property is found through the base chain")
{
    // Dog declares no properties of its own but inherits 'legs' from Animal.
    CHECK(Properties(Dog::StaticType()).Size() == 0u);

    const PropertyInfo* legs = FindProperty(Dog::StaticType(), "legs");
    REQUIRE(legs != nullptr);

    RefPtr<Dog> dog = MakeRef<Dog>(DefaultAllocator());
    Instance inst = Instance::From(dog.Get());
    CHECK(SetProperty(*legs, inst, Variant::From(3)).IsOk());
    CHECK(dog->legs == 3);
}

// --- RTTI: methods ---------------------------------------------------------

TEST_CASE("rtti: instance method invoke with an argument and a return value")
{
    RefPtr<Animal> animal = MakeRef<Animal>(DefaultAllocator()); // legs = 4
    Instance inst = Instance::From(animal.Get());

    const MethodInfo* add = FindMethod(Animal::StaticType(), "AddLegs");
    REQUIRE(add != nullptr);
    CHECK_FALSE(add->isStatic);
    CHECK_FALSE(add->isConst);
    CHECK(add->returnType() == &TypeOf<int>());
    CHECK(add->paramCount == 1u);
    CHECK(add->params[0].type() == &TypeOf<int>());

    Variant args[] = {Variant::From(3)};
    Result<Variant> r = InvokeMethod(*add, inst, Span<Variant>{args, 1});
    REQUIRE(r.HasValue());
    CHECK(r.Value().Get<int>() == 7);
    CHECK(animal->legs == 7); // mutated the real object
}

TEST_CASE("rtti: A6 - authored parameter names reach ParamInfo; unnamed methods stay empty")
{
    // One-param named method: the authored name lands, the type is still correct.
    const MethodInfo* add = FindMethod(Animal::StaticType(), "AddLegs");
    REQUIRE(add != nullptr);
    REQUIRE(add->paramCount == 1u);
    CHECK(std::strcmp(add->params[0].name, "count") == 0);
    CHECK(add->params[0].type() == &TypeOf<int>());

    // Two-param named method: names land in order (the index mapping).
    const MethodInfo* set = FindMethod(Animal::StaticType(), "SetLegs");
    REQUIRE(set != nullptr);
    REQUIRE(set->paramCount == 2u);
    CHECK(std::strcmp(set->params[0].name, "front") == 0);
    CHECK(std::strcmp(set->params[1].name, "back") == 0);
    CHECK(set->params[0].type() == &TypeOf<int>());
    CHECK(set->params[1].type() == &TypeOf<int>());

    // Names do not disturb dispatch: the named method still invokes correctly.
    RefPtr<Animal> animal = MakeRef<Animal>(DefaultAllocator());
    Instance inst = Instance::From(animal.Get());
    Variant args[] = {Variant::From(2), Variant::From(5)};
    Result<Variant> r = InvokeMethod(*set, inst, Span<Variant>{args, 2});
    REQUIRE(r.HasValue());
    CHECK(r.Value().Get<int>() == 7);
    CHECK(animal->legs == 7);

    // An unnamed method's parameter name stays empty; the two paths coexist.
    const MethodInfo* legsOf = FindMethod(Animal::StaticType(), "LegsOf");
    REQUIRE(legsOf != nullptr);
    REQUIRE(legsOf->paramCount == 1u);
    CHECK(std::strcmp(legsOf->params[0].name, "") == 0);
}

namespace
{
    // What a resolve-mode handle's context points into (a GUI's element table): emptied by its
    // owner when the owner goes, kept alive by the handles that still point at it.
    class AnchorTable : public Object
    {
        RTTI_OBJECT(AnchorTable, Object)
    public:
        int value = 5;
        bool open = true;
    };
}
RTTI_DEFINE_OBJECT(AnchorTable, "rtti::tests")

TEST_CASE("rtti: an anchored resolve-mode handle keeps its table alive, and copies and moves share it")
{
    RefPtr<AnchorTable> owner = MakeRef<AnchorTable>(DefaultAllocator());
    AnchorTable* table = owner.Get();
    struct Ctx
    {
        AnchorTable* table;
    };
    auto resolver = [](const Variant& v) -> void*
    {
        AnchorTable* t = static_cast<const Ctx*>(v.ResolveContext())->table;
        return t->open ? &t->value : nullptr;
    };
    CHECK(table->RefCount() == 1u);
    {
        Variant handle = Variant::Resolving(&TypeOf<int>(), resolver, Ctx{table}, table);
        CHECK(handle.ResolveAnchor() == table);
        CHECK(table->RefCount() == 2u);
        Variant copy = handle;
        CHECK(table->RefCount() == 3u);
        Variant moved = Move(copy);
        CHECK(table->RefCount() == 3u); // a move carries the reference, takes none
        CHECK(copy.ResolveAnchor() == nullptr);
        Variant assigned;
        assigned = handle;
        CHECK(table->RefCount() == 4u);
        assigned = Variant{};
        CHECK(table->RefCount() == 3u);

        // The owner goes: it empties the table and drops its reference; the handles still read it.
        table->open = false;
        owner = RefPtr<AnchorTable>{};
        CHECK(table->RefCount() == 2u);
        CHECK(ToInstance(handle).Pointer() == nullptr); // gone, not freed memory
        CHECK(ToInstance(moved).Pointer() == nullptr);
        handle.Reset();
        CHECK(table->RefCount() == 1u);
    } // the last handle releases the table
}

TEST_CASE("rtti: Variant RESOLVE mode re-computes the live address every deref (entity.get core)")
{
    // A movable "component": `slot` selects which array element is live; the resolver returns its
    // current address, as a component manager's Get() does after a swap-remove.
    int values[4] = {10, 20, 30, 40};
    int slot = 2;
    struct Ctx
    {
        int* base;
        int* slot;
    };
    auto resolver = [](const Variant& v) -> void*
    {
        const Ctx* c = static_cast<const Ctx*>(v.ResolveContext());
        return (*c->slot >= 0) ? &c->base[*c->slot] : nullptr;
    };

    Variant rv = Variant::Resolving(&TypeOf<int>(), resolver, Ctx{values, &slot});
    CHECK(rv.IsResolving());
    CHECK_FALSE(rv.IsObject());
    CHECK_FALSE(rv.IsBorrow());
    CHECK_FALSE(rv.IsEmpty());
    CHECK(rv.Type() == &TypeOf<int>());

    Instance i0 = ToInstance(rv);
    REQUIRE(i0.Pointer() == &values[2]);
    CHECK(*static_cast<int*>(i0.Pointer()) == 30);

    // The component "moves" (swap-remove): re-resolution follows it, where a borrow would be stale.
    slot = 0;
    Instance i1 = ToInstance(rv);
    REQUIRE(i1.Pointer() == &values[0]);
    CHECK(*static_cast<int*>(i1.Pointer()) == 10);

    // Component "removed": resolve returns null, giving a clean empty Instance (no dangling deref).
    slot = -1;
    CHECK(ToInstance(rv).Pointer() == nullptr);

    // Copy carries the resolve mode and inline context.
    slot = 3;
    Variant copy = rv;
    CHECK(copy.IsResolving());
    CHECK(ToInstance(copy).Pointer() == &values[3]);

    // Move transfers the handle; the source is left empty.
    Variant moved = Move(rv);
    CHECK(moved.IsResolving());
    CHECK_FALSE(rv.IsResolving());
    CHECK(rv.IsEmpty());
    slot = 1;
    CHECK(ToInstance(moved).Pointer() == &values[1]);

    // From<Variant> passes a resolve handle through unchanged (no double-box), so a reflected
    // method returning a Variant preserves its dynamic type for the backend to wrap.
    Variant relayed = Variant::From<Variant>(Move(moved));
    CHECK(relayed.IsResolving());
    CHECK(relayed.Type() == &TypeOf<int>());
    slot = 3;
    CHECK(ToInstance(relayed).Pointer() == &values[3]);
}

TEST_CASE("rtti: Method<Member, ReturnAs> overrides the declared return type and validates runtime "
          "type (OPTION 1: RigidBody.of(entity) factory)")
{
    // The declared reflected return type is Animal, even though the C++ body returns a Variant.
    const MethodInfo* make = FindMethod(Animal::StaticType(), "makeAnimal");
    REQUIRE(make != nullptr);
    CHECK(make->isStatic);
    CHECK(make->returnType() == &Animal::StaticType()); // overridden declared return

    // The runtime type matches the declared return, so the Variant passes through unchanged.
    Result<Variant> r = InvokeStatic(*make, Span<Variant>{});
    REQUIRE(r.HasValue());
    CHECK(r.Value().Type() == &Animal::StaticType());

    // Validation: a factory whose body returns the wrong runtime type (int) resolves to empty,
    // never a type-confused handle handed to a backend that trusts the declared return.
    const MethodInfo* wrong = FindMethod(Animal::StaticType(), "makeWrong");
    REQUIRE(wrong != nullptr);
    CHECK(wrong->returnType() == &Animal::StaticType()); // declared Animal...
    Result<Variant> rw = InvokeStatic(*wrong, Span<Variant>{});
    REQUIRE(rw.HasValue());
    CHECK(rw.Value().IsEmpty()); // ...but the body returned int, validated to empty
}

TEST_CASE("rtti: Method<Member, ReturnAs, ParamAs> declares a Variant parameter's type")
{
    const MethodInfo* narrow = FindMethod(Animal::StaticType(), "narrow");
    REQUIRE(narrow != nullptr);
    REQUIRE(narrow->paramCount == 2u);
    CHECK(narrow->params[0].type() == &Animal::StaticType()); // the Variant, declared Animal
    CHECK(narrow->params[1].type() == &TypeOf<int>());        // the rest as they are
    CHECK(narrow->returnType() == &Animal::StaticType());

    // The body still receives the Variant.
    Variant args[2] = {MakeAnimalVariant(), Variant::From<int>(1)};
    Result<Variant> r = InvokeStatic(*narrow, Span<Variant>(args, 2));
    REQUIRE(r.HasValue());
    CHECK(r.Value().Type() == &Animal::StaticType());
}

TEST_CASE("rtti: const method and zero-arg invoke")
{
    RefPtr<Animal> animal = MakeRef<Animal>(DefaultAllocator());
    Instance inst = Instance::From(animal.Get());

    const MethodInfo* get = FindMethod(Animal::StaticType(), "GetLegs");
    REQUIRE(get != nullptr);
    CHECK(get->isConst);
    CHECK(get->paramCount == 0u);

    Result<Variant> r = InvokeMethod(*get, inst, Span<Variant>{});
    REQUIRE(r.HasValue());
    CHECK(r.Value().Get<int>() == 4);
}

TEST_CASE("rtti: static method invoke needs no instance")
{
    const MethodInfo* def = FindMethod(Animal::StaticType(), "DefaultLegs");
    REQUIRE(def != nullptr);
    CHECK(def->isStatic);

    Result<Variant> r = InvokeStatic(*def, Span<Variant>{});
    REQUIRE(r.HasValue());
    CHECK(r.Value().Get<int>() == 4);
}

TEST_CASE("rtti: method invoke rejects wrong arity and arg types")
{
    RefPtr<Animal> animal = MakeRef<Animal>(DefaultAllocator());
    Instance inst = Instance::From(animal.Get());
    const MethodInfo* add = FindMethod(Animal::StaticType(), "AddLegs");
    REQUIRE(add != nullptr);

    // Wrong arity.
    Result<Variant> noArgs = InvokeMethod(*add, inst, Span<Variant>{});
    CHECK_FALSE(noArgs.HasValue());
    CHECK(noArgs.Error() == ErrorCode::InvalidArgument);

    // Wrong argument type.
    Variant wrong[] = {Variant::From(2.5f)};
    Result<Variant> badType = InvokeMethod(*add, inst, Span<Variant>{wrong, 1});
    CHECK_FALSE(badType.HasValue());
    CHECK(badType.Error() == ErrorCode::InvalidArgument);

    CHECK(animal->legs == 4); // unchanged after failed calls
}

// --- RTTI: enums -----------------------------------------------------------

TEST_CASE("rtti: enum reflection exposes named values")
{
    RttiRegisterEnum_TestColor();

    const TypeInfo& type = TypeOf<TestColor>();
    CHECK(IsEnum(type));
    CHECK(Enumerators(type).Size() == 3u);
    CHECK(std::strcmp(type.name, "TestColor") == 0);

    CHECK(std::strcmp(EnumValueName(type, static_cast<i64>(TestColor::Green)), "Green") == 0);
    CHECK(EnumValueName(type, 999) == nullptr);

    i64 value = 0;
    CHECK(EnumValueByName(type, "Blue", value));
    CHECK(value == static_cast<i64>(TestColor::Blue));
    CHECK_FALSE(EnumValueByName(type, "Purple", value));
}

TEST_CASE("rtti: enum values round-trip through a Variant")
{
    RttiRegisterEnum_TestColor();

    Variant v = Variant::From(TestColor::Green);
    REQUIRE(v.Is<TestColor>());
    CHECK(v.Get<TestColor>() == TestColor::Green);
    CHECK(v.Type() == &TypeOf<TestColor>());
    CHECK(IsEnum(*v.Type()));
}

// --- RTTI: attributes ------------------------------------------------------

TEST_CASE("rtti: type attributes are queryable")
{
    const TypeInfo& type = Animal::StaticType();
    CHECK(Attributes(type).Size() == 2u);

    const Variant* scriptName = FindAttribute(type, "scriptName");
    REQUIRE(scriptName != nullptr);
    REQUIRE(scriptName->Is<const char*>());
    CHECK(std::strcmp(scriptName->Get<const char*>(), "Critter") == 0);

    const Variant* maxLegs = FindAttribute(type, "maxLegs");
    REQUIRE(maxLegs != nullptr);
    CHECK(maxLegs->Get<int>() == 8);

    CHECK(FindAttribute(type, "missing") == nullptr);
}

// --- RTTI: container reflection --------------------------------------------

TEST_CASE("rtti: Array reflected as a container, iterated generically")
{
    RegisterArrayType<int>();

    const TypeInfo& type = TypeOf<Array<int>>();
    REQUIRE(IsContainer(type));
    const ContainerInfo* container = type.container;
    REQUIRE(container != nullptr);
    CHECK(container->elementType == &TypeOf<int>());

    Array<int> values;
    values.PushBack(10);
    values.PushBack(20);
    values.PushBack(30);

    Instance inst = Instance::From(&values);

    CHECK(ContainerSize(*container, inst) == 3u);
    CHECK(ContainerGetAt(*container, inst, 1).Get<int>() == 20);

    // Generic sum without knowing the element type at the call site.
    i64 sum = 0;
    for (usize i = 0; i < ContainerSize(*container, inst); ++i)
    {
        sum += ContainerGetAt(*container, inst, i).Get<int>();
    }
    CHECK(sum == 60);

    // Generic write-back.
    CHECK(ContainerSetAt(*container, inst, 0, Variant::From(99)).IsOk());
    CHECK(values[0] == 99);

    // Type-checked: wrong element type rejected.
    CHECK(ContainerSetAt(*container, inst, 0, Variant::From(1.5f)).Code() ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("core: StringHash - constexpr identity over UTF-8 text")
{
    constexpr StringHash runtime{StringView(u8"Runtime")};
    constexpr StringHash editor{StringView(u8"Editor")};
    static_assert(runtime != editor);
    static_assert(StringHash{StringView(u8"Runtime")} == runtime);
    static_assert(!StringHash{});                 // zero = "no value"
    static_assert(static_cast<bool>(runtime));

    // The constexpr text path and the runtime byte path agree (same FNV-1a).
    const char8_t* text = u8"Runtime";
    CHECK(runtime.Value() == HashBytes(text, 7));
}

TEST_CASE("rtti: type domains - default Runtime, open tags, widen-only")
{
    TypeRegistry registry; // fresh, not the global one

    // Untagged registration = the default Runtime domain; unknown ids also read Runtime.
    registry.Register(Animal::StaticType());
    CHECK(registry.DomainOf(Animal::StaticType().id) == kRuntimeTypeDomain);
    CHECK(registry.DomainOf(TypeId{0xDEAD}) == kRuntimeTypeDomain);

    // An open, string-named domain sticks.
    registry.Register(Dog::StaticType(), TypeDomain(u8"Editor"));
    CHECK(registry.DomainOf(Dog::StaticType().id) == TypeDomain(u8"Editor"));
    CHECK(!(registry.DomainOf(Dog::StaticType().id) == kRuntimeTypeDomain));

    // Re-registration may widen to Runtime (order-independent: if any player path
    // registers the type, the player has it) ...
    registry.Register(Dog::StaticType());
    CHECK(registry.DomainOf(Dog::StaticType().id) == kRuntimeTypeDomain);

    // ... but never narrows: a later Editor-tagged duplicate is ignored.
    registry.Register(Dog::StaticType(), TypeDomain(u8"Editor"));
    CHECK(registry.DomainOf(Dog::StaticType().id) == kRuntimeTypeDomain);

    // Dedupe semantics are unchanged by tagging.
    const usize count = registry.Count();
    registry.Register(Dog::StaticType(), TypeDomain(u8"Editor"));
    CHECK(registry.Count() == count);
}

TEST_CASE("rtti: legacy namespace spellings do NOT resolve - current names only")
{
    // Serialized data stores qualified type names. Files written under the retired
    // "draconic::" / "rtti::editor::<lib>" spellings are not remapped: FindByName is an exact
    // lookup, so such a file fails to deserialize (re-import / re-save under the current
    // build) instead of being silently upgraded.
    TypeRegistry registry;
    registry.Register(Dog::StaticType());
    REQUIRE(registry.FindByName("rtti::test", "Dog") != nullptr);
    CHECK(registry.FindByName("draconic::test", "Dog") == nullptr);
    CHECK(registry.FindByName("draconic::engine::test", "Dog") == nullptr);
    CHECK(registry.FindByName("rtti::editor::test", "Dog") == nullptr);
    CHECK(registry.FindByName("rtti", "Dog") == nullptr);
}

// --- Shared-library identity: TypeId is the identity, pointers are a fast path.
// --- A shared library gets its own copy of every vague-linkage TypeInfo static,
// --- so these tests simulate the duplicate (same ids, different addresses):
// --- every check must behave exactly as it does on the original.
TEST_CASE("rtti-shared: value-type ids are signature-derived, distinct, and name-independent")
{
    // Distinct types get distinct ids; repeated calls are stable.
    const TypeInfo& f = TypeOf<f32>();
    const TypeInfo& i = TypeOf<i32>();
    const TypeInfo& u = TypeOf<u32>();
    CHECK(f.id != 0u);
    CHECK(f.id != i.id);
    CHECK(i.id != u.id);
    CHECK(TypeOf<f32>().id == f.id);
    // The id must not be the storage address.
    CHECK(f.id != static_cast<TypeId>(reinterpret_cast<uptr>(&f)));
}

TEST_CASE("rtti-shared: IsDerivedFrom matches a duplicated TypeInfo chain by id")
{
    // Simulate another library's copies: byte-wise clones at new addresses.
    TypeInfo dupAnimal = Animal::StaticType();
    TypeInfo dupDog = Dog::StaticType();
    dupDog.base = &dupAnimal; // the clone's chain points into the clone library

    // The original hierarchy resolves against the duplicate metadata...
    CHECK(IsDerivedFrom(&Dog::StaticType(), &dupAnimal));
    CHECK(IsDerivedFrom(&dupDog, &Animal::StaticType()));
    // ...and unrelated types still do not.
    CHECK(!IsDerivedFrom(&Cat::StaticType(), &dupDog));
    CHECK(!IsDerivedFrom(&dupDog, &Cat::StaticType()));
    CHECK(!IsDerivedFrom(&dupDog, nullptr));

    // IsA through an object whose GetType() came from "the other library".
    Dog dog;
    CHECK(IsDerivedFrom(dog.GetType(), &dupAnimal));
}

TEST_CASE("rtti-shared: Instance and Variant checks hold across duplicated metadata")
{
    // Instance built against a duplicated TypeInfo (different address, same id).
    TypeInfo dupF32 = TypeOf<f32>();
    f32 value = 4.0f;
    const Instance other{&value, &dupF32};
    CHECK(other.TryGet<f32>() == &value);
    CHECK(other.TryGet<i32>() == nullptr);

    // Variant's vtable fast path still works in-library...
    Variant v = Variant::From(7);
    CHECK(v.Is<i32>());
    CHECK(!v.Is<f32>());
    // ...and the id fallback agrees with the vtable-reported TypeInfo.
    REQUIRE(v.Type() != nullptr);
    CHECK(v.Type()->id == TypeOf<i32>().id);
}

TEST_CASE("rtti-shared: TypeOf resolves every image's prototype to one process-single slot")
{
    struct SlotProbe
    {
        int a;
    };
    // What a second image's TypeOf<SlotProbe>() does on its first use: same signature id,
    // its own prototype, and it must land on the very TypeInfo this image already holds.
    const TypeId signature = detail::SignatureTypeId<SlotProbe>();
    TypeInfo prototype = detail::ValuePrototype<SlotProbe>();
    TypeInfo& other = detail::TypeInfoSlot(signature, prototype);
    CHECK(&other == &TypeOf<SlotProbe>());
    CHECK(other.id == signature);

    // A patch made through one image's view is what the other image reads.
    const_cast<TypeInfo&>(TypeOf<SlotProbe>()).dataVersion = 7;
    CHECK(detail::TypeInfoSlot(signature, prototype).dataVersion == 7u);

    // Layout facts refresh from the prototype (a rebuilt module after hot reload);
    // registrar patches survive the refresh.
    prototype.size = 64;
    prototype.align = 16;
    TypeInfo& refreshed = detail::TypeInfoSlot(signature, prototype);
    CHECK(&refreshed == &other);
    CHECK(refreshed.size == 64u);
    CHECK(refreshed.align == 16u);
    CHECK(refreshed.dataVersion == 7u);

    // Distinct signatures get distinct slots.
    struct OtherProbe
    {
        int b;
    };
    CHECK(&TypeOf<OtherProbe>() != &other);
}

TEST_CASE("rtti-shared: REFLECT_VALUE patches made in one image are read by another")
{
    // Core registers Float3 inside its own image; a consumer image's TypeOf<Float3>() must
    // see the patched metadata (properties and authored id), not an unpatched per-image copy.
    // Simulate the consumer's first use directly.
    RegisterCoreTypes();
    const TypeInfo& seen =
        detail::TypeInfoSlot(detail::SignatureTypeId<Float3>(), detail::ValuePrototype<Float3>());
    CHECK(&seen == &TypeOf<Float3>());
    CHECK(seen.propertyCount == 3u);
    CHECK(seen.id == ComputeTypeId("rtti::core", "Float3"));
}

TEST_CASE("rtti-shared: Canonical resolves a duplicated TypeInfo to the registered one")
{
    GlobalTypeRegistry().Register(Animal::StaticType());
    // A clone (another library's copy): same id, different address, unpatched metadata.
    TypeInfo dup = Animal::StaticType();
    const TypeInfo& canonical = GlobalTypeRegistry().Canonical(dup);
    CHECK(&canonical == &Animal::StaticType());
    CHECK(&canonical != &dup);

    // Unregistered types resolve to the argument itself, so the result is always usable.
    const TypeInfo local = MakeTypeInfo<int>("NeverRegistered", "rtti::test", nullptr);
    CHECK(&GlobalTypeRegistry().Canonical(local) == &local);
}

TEST_CASE("rtti-shared: Unregister frees the id slot and a re-register takes it")
{
    // A private registry (the global one is shared test state).
    TypeRegistry registry;
    const TypeInfo infoA = MakeTypeInfo<int>("ReloadProbe", "rtti::test", nullptr);
    TypeInfo infoB = infoA; // "the rebuilt module's copy": same id, new address

    registry.Register(infoA);
    REQUIRE(registry.FindById(infoA.id) == &infoA);

    registry.Unregister(infoA.id);
    CHECK(registry.FindById(infoA.id) == nullptr);
    CHECK(registry.All().Size() == 0u);
    registry.Unregister(infoA.id); // idempotent

    registry.Register(infoB);
    CHECK(registry.FindById(infoA.id) == &infoB); // the freed slot re-registers
}

TEST_CASE("rtti-shared: the registration observer fires only on real inserts")
{
    TypeRegistry registry;
    const TypeInfo info = MakeTypeInfo<int>("ObserverProbe", "rtti::test", nullptr);

    Array<TypeId> recorded;
    registry.SetRegistrationObserver(
        [](void* ctx, TypeId id) { static_cast<Array<TypeId>*>(ctx)->PushBack(id); },
        &recorded);
    registry.Register(info);
    registry.Register(info); // duplicate: no-op, not recorded (owned by the first registrant)
    registry.SetRegistrationObserver(nullptr, nullptr);
    registry.Register(info); // observer cleared: nothing recorded either way

    REQUIRE(recorded.Size() == 1u);
    CHECK(recorded[0] == info.id);
}

namespace
{
    // A native API spelled with StringView (a view into the owner's text) and a creator, as a GUI
    // element is: reflected as it is, with no script twin.
    class Signboard : public Object
    {
        RTTI_OBJECT(Signboard, Object)
    public:
        [[nodiscard]] StringView GetCaption() const { return m_caption.AsView(); }
        void SetCaption(StringView caption) { m_caption = String(caption); }
        [[nodiscard]] bool Says(StringView text) const { return m_caption.AsView() == text; }
        [[nodiscard]] StringView Shout() const { return m_caption.AsView(); }

    private:
        String m_caption = String(u8"open");
    };
}

REFLECT_MEMBERS(Signboard, "rtti::test")
{
    builder.AccessorProperty<&Signboard::GetCaption, &Signboard::SetCaption>("caption");
    builder.Method<&Signboard::Says>("says");
    builder.Method<&Signboard::Shout>("shout");
    builder.Creator();
}

TEST_CASE("rtti: StringView accessors and methods cross as String")
{
    RefPtr<Signboard> board = MakeRef<Signboard>(DefaultAllocator());
    Instance inst = Instance::From(board.Get());
    const PropertyInfo* caption = FindProperty(Signboard::StaticType(), "caption");
    REQUIRE(caption != nullptr);
    CHECK(caption->type == &TypeOf<String>());
    Variant read = GetProperty(*caption, inst);
    REQUIRE(read.TryGet<String>() != nullptr);
    CHECK(read.TryGet<String>()->AsView() == StringView(u8"open"));
    CHECK(SetProperty(*caption, inst, Variant::From<String>(String(u8"closed"))).IsOk());
    CHECK(board->GetCaption() == StringView(u8"closed"));

    const MethodInfo* says = FindMethod(Signboard::StaticType(), "says");
    REQUIRE(says != nullptr);
    CHECK(ParamAt(*says, 0).type() == &TypeOf<String>());
    Variant args[] = {Variant::From<String>(String(u8"closed"))};
    CHECK(InvokeMethod(*says, inst, Span<Variant>{args, 1}).Value().Get<bool>());
    // Native callers may pass the view itself.
    Variant viewArgs[] = {Variant::From(StringView(u8"closed"))};
    CHECK(InvokeMethod(*says, inst, Span<Variant>{viewArgs, 1}).Value().Get<bool>());
    Variant shouted = InvokeMethod(*FindMethod(Signboard::StaticType(), "shout"), inst, Span<Variant>{}).Value();
    REQUIRE(shouted.TryGet<String>() != nullptr); // an owned copy, never a view into the board
    CHECK(shouted.TryGet<String>()->AsView() == StringView(u8"closed"));
}

TEST_CASE("rtti: a creator makes an Object from an owner's allocator; a type without one makes none")
{
    REQUIRE(Signboard::StaticType().creator != nullptr);
    CHECK(Signboard::StaticType().constructorCount == 0u); // nothing a script could construct
    RefPtr<Object> made = CreateObject(Signboard::StaticType(), DefaultAllocator());
    REQUIRE(made);
    CHECK(made->GetType() == &Signboard::StaticType());
    CHECK(&made->MemoryAllocator() == &DefaultAllocator());
    CHECK_FALSE(CreateObject(Animal::StaticType(), DefaultAllocator()));
}
