// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;

namespace
{
    // Source: serializable, lives in the content database. Carries editor-only
    // data (editorNote) that the runtime product must not inherit.
    class MaterialResource final : public ISerializable
    {
        RTTI_OBJECT(MaterialResource, ISerializable)
    public:
        i32 shininess = 0;
        String shader;
        String editorNote; // editor-only

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "shininess", shininess);
            foundation::core::Serialize(ar, "shader", shader);
            foundation::core::Serialize(ar, "editorNote", editorNote);
        }
    };

    // Product: lean runtime object built from the source. No editorNote.
    class Material final : public Object
    {
        RTTI_OBJECT(Material, Object)
    public:
        f32 specular = 0.0f;
        String shader;
    };

    // Factory: builds a Material product from a MaterialResource source.
    class MaterialFactory final : public IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &Material::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override { return ProductType(); }

        int builds = 0;              // observe rebuilds (incl. dependency-propagated reloads)
        HashMap<Guid, Guid> bindMap; // when building key, Bind value (a child) -> auto-edge

        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            ++builds;
            // Resolving a child via the manager mid-build auto-records a dependency.
            if (Guid* child = bindMap.Find(instance.Id()))
            {
                (void)manager.Bind(Material::StaticType(), *child);
            }
            RefPtr<ISerializable> source = instance.ReadObject();
            MaterialResource* res = Cast<MaterialResource>(source.Get());
            if (res == nullptr)
            {
                return RefPtr<Object>{};
            }

            RefPtr<Material> material = MakeRef<Material>(DefaultAllocator());
            material->specular = static_cast<f32>(res->shininess) / 128.0f;
            material->shader = res->shader;
            // editorNote is dropped on purpose: the product is runtime-only.
            return material;
        }
    };

    void RemoveTree()
    {
        FileDelete(u8"scratch_resource_test_db/steel.rasset");
        RemoveDirectory(u8"scratch_resource_test_db");
    }

    void WriteSource(foundation::content::ContentDatabase& db, const Guid& id, i32 shininess,
                     StringView shader)
    {
        auto* instance = db.GetInstance(id);
        REQUIRE(instance != nullptr);
        MaterialResource r;
        r.shininess = shininess;
        r.shader = String(shader);
        r.editorNote = u8"node@(10,20)";
        REQUIRE(instance->WriteObject(r).IsOk());
    }
}

RTTI_DEFINE_OBJECT(MaterialResource, "rtti::resource::test")
RTTI_DEFINE_OBJECT(Material, "rtti::resource::test")

TEST_CASE("resource: bind builds a product from a source, with caching")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    RemoveTree();
    NativeFileSystem mount(u8"scratch_resource_test_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* steel = db.RootGroup()->CreateInstance(u8"steel", MaterialResource::StaticType());
        id = steel->Id();
        WriteSource(db, id, 64, u8"pbr");
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    MaterialFactory factory;
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    // Bind: source to product. The product is derived and lean (no editorNote field).
    Proxy<Material> p = manager.Bind<Material>(id);
    REQUIRE(p);
    CHECK(p->shader == u8"pbr");
    CHECK(p->specular == 0.5f); // 64 / 128

    // Cache: binding the same id returns the same handle.
    Proxy<Material> p2 = manager.Bind<Material>(id);
    CHECK(p2.Handle() == p.Handle());

    // Unknown id: an invalid proxy.
    Proxy<Material> none = manager.Bind<Material>(Guid{1, 2});
    CHECK_FALSE(none);

    // Flush drops the product; the proxy follows the handle and goes invalid,
    // then a rebind rebuilds into the same handle with a fresh product.
    CHECK(manager.Flush(id));
    CHECK_FALSE(p);
    Proxy<Material> p3 = manager.Bind<Material>(id);
    CHECK(p3.Handle() == p.Handle());
    REQUIRE(p);                 // p recovers through the shared handle
    CHECK(p->specular == 0.5f); // rebuilt correctly

    RemoveTree();
}

TEST_CASE("resource: unresolved binds are enumerable, and heal off the list")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    // This case adds late.rasset, which the shared RemoveTree does not know about; a
    // leftover from a previous run would make the "missing" id resolve immediately.
    FileDelete(u8"scratch_resource_test_db/late.rasset");
    RemoveTree();
    NativeFileSystem mount(u8"scratch_resource_test_db", DefaultAllocator());
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    MaterialFactory factory;
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    auto* steel = db.RootGroup()->CreateInstance(u8"steel", MaterialResource::StaticType());
    WriteSource(db, steel->Id(), 64, u8"pbr");
    Proxy<Material> good = manager.Bind<Material>(steel->Id());
    REQUIRE(good);

    // A bind against an id with no backing instance stays cached with a null product
    // (an editor page referencing a not-yet-cooked asset), and it must be reported.
    const Guid missing{7, 7};
    Proxy<Material> pending = manager.Bind<Material>(missing);
    CHECK_FALSE(pending);

    Array<Guid> unresolved;
    manager.CollectUnresolved(unresolved);
    REQUIRE(unresolved.Size() == 1);
    CHECK(unresolved[0] == missing);

    // The missing instance appears (a cook landed): after the reload the id resolves
    // and drops off the unresolved list, and the original proxy heals through the handle.
    auto* late =
        db.RootGroup()->CreateInstanceWithId(missing, u8"late", MaterialResource::StaticType());
    REQUIRE(late != nullptr);
    WriteSource(db, missing, 32, u8"unlit");
    CHECK(manager.Reload(missing));
    REQUIRE(pending);
    CHECK(pending->shader == u8"unlit");

    unresolved.Clear();
    manager.CollectUnresolved(unresolved);
    CHECK(unresolved.IsEmpty());

    FileDelete(u8"scratch_resource_test_db/late.rasset");
    RemoveTree();
}

TEST_CASE("resource: reload rebuilds the product and proxies see the new value")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    RemoveTree();
    NativeFileSystem mount(u8"scratch_resource_test_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* steel = db.RootGroup()->CreateInstance(u8"steel", MaterialResource::StaticType());
        id = steel->Id();
        WriteSource(db, id, 64, u8"pbr");
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    MaterialFactory factory;
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<Material> p = manager.Bind<Material>(id);
    REQUIRE(p);
    CHECK(p->specular == 0.5f);

    // Source changes on disk; Reload rebuilds the product behind the handle.
    WriteSource(db, id, 128, u8"pbr2");
    CHECK(manager.Reload(id));
    CHECK(p->specular == 1.0f); // same proxy, new product
    CHECK(p->shader == u8"pbr2");

    RemoveTree();
}

namespace
{
    // Cleanup for the dependency tests (its own db dir; one .rasset per instance).
    void RemoveDepTree()
    {
        const StringView names[] = {u8"a", u8"b", u8"c", u8"parent", u8"child"};
        for (StringView n : names)
        {
            String f = String(u8"scratch_resource_dep_db/");
            f.Append(n);
            f.Append(u8".rasset");
            FileDelete(f.AsView());
        }
        RemoveDirectory(u8"scratch_resource_dep_db");
    }

    // Create an instance and write a MaterialResource source; returns its id.
    Guid MakeInstance(foundation::content::ContentDatabase& db, StringView name, i32 shininess)
    {
        auto* inst = db.RootGroup()->CreateInstance(name, MaterialResource::StaticType());
        WriteSource(db, inst->Id(), shininess, name);
        return inst->Id();
    }
}

TEST_CASE("resource: a factory-resolved child is an auto-recorded dependency")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    RemoveDepTree();
    NativeFileSystem mount(u8"scratch_resource_dep_db", DefaultAllocator());

    Guid parentId, childId;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        childId = MakeInstance(db, u8"child", 64);
        parentId = MakeInstance(db, u8"parent", 32);
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    MaterialFactory factory;
    factory.bindMap.InsertOrAssign(parentId, childId); // building parent Binds child
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<Material> parent = manager.Bind<Material>(parentId); // builds parent -> binds child
    REQUIRE(parent);
    CHECK(factory.builds == 2); // parent + the child it pulled in

    Span<const Guid> deps = manager.Dependents(childId); // edge was recorded
    REQUIRE(deps.Size() == 1u);
    CHECK(deps[0] == parentId);

    const int b0 = factory.builds;
    CHECK(manager.Reload(childId));  // child reload propagates to parent
    CHECK(factory.builds == b0 + 2); // both rebuilt (child + dependent parent)
    REQUIRE(parent);                 // proxy still valid after the swap

    RemoveDepTree();
}

TEST_CASE("resource: reload propagates transitively, each resource once")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    RemoveDepTree();
    NativeFileSystem mount(u8"scratch_resource_dep_db", DefaultAllocator());

    Guid a, b, c;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        a = MakeInstance(db, u8"a", 16);
        b = MakeInstance(db, u8"b", 32);
        c = MakeInstance(db, u8"c", 64);
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    MaterialFactory factory;
    factory.bindMap.InsertOrAssign(a, b); // a -> b
    factory.bindMap.InsertOrAssign(b, c); // b -> c
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<Material> pa = manager.Bind<Material>(a); // builds a -> b -> c
    REQUIRE(pa);
    CHECK(factory.builds == 3);

    const int b0 = factory.builds;
    CHECK(manager.Reload(c)); // c -> b -> a, each exactly once
    CHECK(factory.builds == b0 + 3);

    RemoveDepTree();
}

TEST_CASE("resource: a rebuild drops stale dependency edges")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    RegisterSerializable<MaterialResource>();

    RemoveDepTree();
    NativeFileSystem mount(u8"scratch_resource_dep_db", DefaultAllocator());

    Guid parentId, childId;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        childId = MakeInstance(db, u8"child", 64);
        parentId = MakeInstance(db, u8"parent", 32);
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    MaterialFactory factory;
    factory.bindMap.InsertOrAssign(parentId, childId);
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<Material> parent = manager.Bind<Material>(parentId);
    REQUIRE(parent);
    CHECK(manager.Dependents(childId).Size() == 1u);

    // Parent stops referencing the child; rebuilding parent must drop the edge.
    factory.bindMap.Remove(parentId);
    CHECK(manager.Reload(parentId));
    CHECK(manager.Dependents(childId).Size() == 0u);

    // Now a child reload rebuilds only the child.
    const int b0 = factory.builds;
    CHECK(manager.Reload(childId));
    CHECK(factory.builds == b0 + 1);

    RemoveDepTree();
}

TEST_CASE("resource: deserializing a ref drops the stale binding when the id changes")
{
    // A directly-bound ref (procedural or picker-assigned) reads a different id from a blob
    // (component paste, prefab revert): the old binding must not keep rendering. Reading
    // the same id keeps the binding (steady-state reload of an unchanged component).
    RefPtr<Material> live = MakeRef<Material>(DefaultAllocator());
    Ref<Material> ref;
    ref.SetDirect(RefPtr<Material>(live.Get()));
    ref.id = Guid{0x1, 0x1};
    REQUIRE(ref.Get() == live.Get());

    // Write a ref whose id is nil ("no resource"), then read it over the live one.
    MemoryStream buffer;
    {
        Ref<Material> cleared;
        BinarySerializer ar(buffer, SerializeMode::Write);
        Serialize(ar, cleared);
    }
    (void)buffer.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer ar(buffer, SerializeMode::Read);
        Serialize(ar, ref);
    }
    CHECK(ref.id == Guid{});
    CHECK(ref.Get() == nullptr); // the stale direct binding is gone

    // Same-id read keeps the binding.
    Ref<Material> stable;
    stable.SetDirect(RefPtr<Material>(live.Get()));
    stable.id = Guid{};
    (void)buffer.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer ar(buffer, SerializeMode::Read);
        Serialize(ar, stable);
    }
    CHECK(stable.Get() == live.Get());
}

namespace
{
    // A product whose destructor re-enters the manager (drops a proxy and triggers a reload
    // that pushes a fresh grave): the graveyard-collection re-entrancy scenario.
    class Reentrant final : public Object
    {
        RTTI_OBJECT(Reentrant, Object)
    public:
        static inline bool reenterOnDestroy = false; // off during manager teardown
        ResourceManager* manager = nullptr;
        Guid other;
        Proxy<Material> child;
        ~Reentrant() override
        {
            child = Proxy<Material>{}; // handle bookkeeping re-entry
            if (reenterOnDestroy && manager != nullptr && !other.IsNil())
            {
                (void)manager->Reload(other); // pushes a new grave mid-collect
            }
        }
    };

    class ReentrantSource final : public ISerializable
    {
        RTTI_OBJECT(ReentrantSource, ISerializable)
    public:
        Guid other;
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, other); }
    };
    RTTI_DEFINE_OBJECT(ReentrantSource, "test")

    class ReentrantFactory final : public IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &Reentrant::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override { return ProductType(); }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            auto* src = Cast<ReentrantSource>(object.Get());
            RefPtr<Reentrant> product = MakeRef<Reentrant>(DefaultAllocator());
            product->manager = &manager;
            if (src != nullptr)
            {
                product->other = src->other;
            }
            return product;
        }
    };
}

RTTI_DEFINE_OBJECT(Reentrant, "test")

TEST_CASE("resource: garbage collection survives destructor re-entry into the manager")
{
    GlobalTypeRegistry().Register(ReentrantSource::StaticType());
    RegisterSerializable<ReentrantSource>();
    GlobalTypeRegistry().Register(Reentrant::StaticType());

    RemoveTree();
    NativeFileSystem mount(u8"scratch_res_reentry_db", DefaultAllocator());
    (void)CreateDirectory(u8"scratch_res_reentry_db");
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");

    auto* a = db.RootGroup()->CreateInstance(u8"A", ReentrantSource::StaticType());
    auto* b = db.RootGroup()->CreateInstance(u8"B", ReentrantSource::StaticType());
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    ReentrantSource sa, sb;
    sa.other = b->Id(); // A's destructor reloads B (pushing B's old product to the grave)
    REQUIRE(a->WriteObject(sa).IsOk());
    REQUIRE(b->WriteObject(sb).IsOk());

    ResourceManager resources(DefaultAllocator(), db);
    ReentrantFactory factory;
    resources.AddFactory(&factory);

    Proxy<Reentrant> pa = resources.Bind<Reentrant>(a->Id());
    Proxy<Reentrant> pb = resources.Bind<Reentrant>(b->Id());
    REQUIRE(pa.Get() != nullptr);
    REQUIRE(pb.Get() != nullptr);

    // Reload both: two graves. Age them out together; dropping A's old product re-enters the
    // manager (it reloads B again and pushes another grave) while collection is walking.
    Reentrant::reenterOnDestroy = true;
    REQUIRE(resources.Reload(a->Id()));
    REQUIRE(resources.Reload(b->Id()));
    for (int frame = 0; frame < 16; ++frame)
    {
        resources.CollectGarbage();
    }
    Reentrant::reenterOnDestroy = false; // manager teardown must not re-enter

    CHECK(pa.Get() != nullptr);
    CHECK(pb.Get() != nullptr);
}

namespace
{
    // A factory whose second build binds a burst of fresh children, forcing the handle map
    // to grow (rehash) in the middle of a Reload cascade.
    class Burst final : public Object
    {
        RTTI_OBJECT(Burst, Object)
    public:
        i32 generation = 0;
    };

    class BurstSource final : public ISerializable
    {
        RTTI_OBJECT(BurstSource, ISerializable)
    public:
        Array<Guid> children;
        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "children", children);
        }
    };
    RTTI_DEFINE_OBJECT(BurstSource, "test")

    class BurstFactory final : public IResourceFactory
    {
    public:
        i32 builds = 0;
        [[nodiscard]] const TypeInfo* ProductType() const override { return &Burst::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return ProductType(); }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            ++builds;
            RefPtr<ISerializable> object = instance.ReadObject();
            auto* src = Cast<BurstSource>(object.Get());
            RefPtr<Burst> product = MakeRef<Burst>(DefaultAllocator());
            product->generation = builds;
            // Second-generation build: bind every child (fresh handle-map inserts).
            if (src != nullptr && builds > 1)
            {
                for (const Guid& child : src->children)
                {
                    (void)manager.Bind<Burst>(child);
                }
            }
            return product;
        }
    };
}

RTTI_DEFINE_OBJECT(Burst, "test")

TEST_CASE("resource: reload survives the handle map rehashing mid-cascade")
{
    // Reload must not hold a raw pointer into the handle map across the recursive rebuild: the
    // rebuild's child Binds grow the map, a rehash moves the slots, and the dangling pointer would
    // read freed memory. Bind's cached branch has the same hazard (returning *cached after
    // BuildInto).
    GlobalTypeRegistry().Register(BurstSource::StaticType());
    RegisterSerializable<BurstSource>();
    GlobalTypeRegistry().Register(Burst::StaticType());

    RemoveTree();
    NativeFileSystem mount(u8"scratch_res_rehash_db", DefaultAllocator());
    (void)CreateDirectory(u8"scratch_res_rehash_db");
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");

    BurstSource parentSource;
    Array<foundation::content::Instance*> children;
    for (i32 i = 0; i < 64; ++i)
    {
        String name = Format(u8"child{}", i);
        auto* child = db.RootGroup()->CreateInstance(name.AsView(), BurstSource::StaticType());
        REQUIRE(child != nullptr);
        BurstSource empty;
        REQUIRE(child->WriteObject(empty).IsOk());
        parentSource.children.PushBack(child->Id());
        children.PushBack(child);
    }
    auto* parent = db.RootGroup()->CreateInstance(u8"parent", BurstSource::StaticType());
    REQUIRE(parent != nullptr);
    REQUIRE(parent->WriteObject(parentSource).IsOk());

    ResourceManager resources(DefaultAllocator(), db);
    BurstFactory factory;
    resources.AddFactory(&factory);

    // First bind: tiny map (just the parent). Reload: the rebuild binds 64 fresh children,
    // guaranteeing growth and a rehash while the cascade is in flight.
    Proxy<Burst> proxy = resources.Bind<Burst>(parent->Id());
    REQUIRE(proxy.Get() != nullptr);
    CHECK(proxy->generation == 1);

    CHECK(resources.Reload(parent->Id()));
    REQUIRE(proxy.Get() != nullptr);
    CHECK(proxy->generation == 2);
}

TEST_CASE("resource: the live-product report counts by type and flags cache-only handles")
{
    GlobalTypeRegistry().Register(MaterialResource::StaticType());
    GlobalTypeRegistry().Register(Material::StaticType());
    RegisterSerializable<MaterialResource>();

    RemoveTree();
    NativeFileSystem mount(u8"scratch_resource_test_db", DefaultAllocator());
    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), 
            mount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* steel = db.RootGroup()->CreateInstance(u8"steel", MaterialResource::StaticType());
        id = steel->Id();
        WriteSource(db, id, 64, u8"pbr");
    }
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    MaterialFactory factory;
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Array<ResourceManager::LiveProductRow> rows;
    manager.ReportLiveProducts(rows);
    CHECK(rows.IsEmpty()); // nothing bound yet

    {
        Proxy<Material> proxy = manager.Bind<Material>(id);
        REQUIRE(proxy);
        manager.ReportLiveProducts(rows);
        REQUIRE(rows.Size() == 1);
        CHECK(rows[0].live == 1);
        // An outside Proxy holds the handle: not cache-only.
        CHECK(rows[0].unreferenced == 0);
    }
    // Proxy dropped: the cache is the only owner, the signal that marks a purge candidate.
    manager.ReportLiveProducts(rows);
    REQUIRE(rows.Size() == 1);
    CHECK(rows[0].live == 1);
    CHECK(rows[0].unreferenced == 1);

    RemoveTree();
}

TEST_CASE("resource: Ref<T> is a reference-shaped value - its TypeInfo reads the id, for any T")
{
    Ref<Material> ref;
    CHECK(IsReferenceType(TypeOf<Ref<Material>>()));
    CHECK(TypeOf<Ref<Material>>().reference->Target() == &Material::StaticType()); // what it points at
    CHECK(TypeOf<Ref<Material>>().reference->Id(&ref)->IsNil());
    Random rng(9);
    ref.SetId(Guid::Generate(rng));
    CHECK(*TypeOf<Ref<Material>>().reference->Id(&ref) == ref.id);
    // Through a Variant, as generic tooling meets it.
    const Variant held = Variant::From(ref);
    REQUIRE(IsReferenceType(*held.Type()));
    CHECK(*held.Type()->reference->Id(held.ValuePointer()) == ref.id);
    // Written by identity: the id set, a direct binding dropped.
    RefPtr<Material> direct = MakeRef<Material>(DefaultAllocator());
    ref = direct;
    REQUIRE(ref.Get() != nullptr);
    const Guid other = Guid::Generate(rng);
    TypeOf<Ref<Material>>().reference->SetId(&ref, other);
    TypeOf<Ref<Material>>().reference->ClearBinding(&ref);
    CHECK(ref.id == other);
    CHECK(ref.Get() == nullptr);
}

