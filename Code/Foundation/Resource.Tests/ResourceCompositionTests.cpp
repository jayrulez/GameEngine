// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Resource composition (engine-composition.md D1-D3): a module's factory descriptions, the
// services a gated factory asks for by type, and the set that creates, owns and registers.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;

using namespace foundation::core;
using namespace foundation::resource;

namespace
{
    class PlainProduct final : public Object
    {
        RTTI_OBJECT(PlainProduct, Object)
    };
    class PlainSource final : public ISerializable
    {
        RTTI_OBJECT(PlainSource, ISerializable)
    public:
        void Serialize(ISerializer&) override {}
    };
    class GatedProduct final : public Object
    {
        RTTI_OBJECT(GatedProduct, Object)
    };
    RTTI_DEFINE_OBJECT(PlainProduct, "rtti::test::composition")
    RTTI_DEFINE_OBJECT(PlainSource, "rtti::test::composition")
    RTTI_DEFINE_OBJECT(GatedProduct, "rtti::test::composition")

    // The service a gated factory asks for: any type, identified by TypeOf<T>().
    struct FakeDevice
    {
        i32 generation = 7;
    };

    class PlainFactory final : public IResourceFactory
    {
    public:
        explicit PlainFactory(IAllocator&) noexcept {}
        [[nodiscard]] const TypeInfo* ProductType() const override { return &PlainProduct::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return &PlainSource::StaticType(); }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&, foundation::content::Instance&) override
        {
            return RefPtr<Object>{};
        }
    };

    class GatedFactory final : public IResourceFactory
    {
    public:
        GatedFactory(IAllocator&, FakeDevice& device) noexcept : m_device(&device) {}
        [[nodiscard]] const TypeInfo* ProductType() const override { return &GatedProduct::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return ProductType(); }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&, foundation::content::Instance&) override
        {
            return RefPtr<Object>{};
        }
        [[nodiscard]] FakeDevice& Device() const noexcept { return *m_device; }

    private:
        FakeDevice* m_device;
    };

    // A factory that uses the device when the host has one and works without it.
    class OptionalFactory final : public IResourceFactory
    {
    public:
        OptionalFactory(IAllocator&, FakeDevice* device) noexcept : m_device(device) {}
        [[nodiscard]] const TypeInfo* ProductType() const override { return &PlainProduct::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override { return &PlainSource::StaticType(); }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&, foundation::content::Instance&) override
        {
            return RefPtr<Object>{};
        }
        [[nodiscard]] FakeDevice* Device() const noexcept { return m_device; }

    private:
        FakeDevice* m_device;
    };

    int g_registered = 0;
    void RegisterFakeTypes() { ++g_registered; }

    inline constexpr ResourceFactoryDesc kFakeFactories[] = {
        FactoryWithAllocator<PlainProduct, PlainSource, PlainFactory>(),
        FactoryWithService<GatedProduct, GatedProduct, GatedFactory, FakeDevice>(),
    };
    inline constexpr ResourceModule kFakeModule{u8"fake", &RegisterFakeTypes, kFakeFactories,
                                                sizeof(kFakeFactories) / sizeof(kFakeFactories[0])};

    class DeviceServices final : public IResourceServices
    {
    public:
        explicit DeviceServices(FakeDevice& device) noexcept : m_device(&device) {}
        [[nodiscard]] void* Service(TypeId type) const noexcept override
        {
            return type == TypeOf<FakeDevice>().id ? static_cast<void*>(m_device) : nullptr;
        }

    private:
        FakeDevice* m_device;
    };
}

TEST_CASE("resource composition: a module describes its factories - product, cooked form, service")
{
    CHECK(kFakeModule.id == StringView(u8"fake"));
    REQUIRE(kFakeModule.Factories().Size() == 2);
    const ResourceFactoryDesc& plain = kFakeModule.Factories()[0];
    const ResourceFactoryDesc& gated = kFakeModule.Factories()[1];
    CHECK(plain.product() == &PlainProduct::StaticType());
    CHECK(plain.cooked() == &PlainSource::StaticType());
    CHECK(plain.service == nullptr);
    CHECK(gated.product() == &GatedProduct::StaticType());
    REQUIRE(gated.service != nullptr);
    CHECK(gated.service() == &TypeOf<FakeDevice>());
    // The descriptions agree with the factories they make.
    NoResourceServices none;
    UniquePtr<IResourceFactory> made = plain.create(DefaultAllocator(), none);
    REQUIRE(made.Get() != nullptr);
    CHECK(made->ProductType() == plain.product());
    CHECK(made->CookedType() == plain.cooked());
    // Type registration goes through the module.
    const int before = g_registered;
    kFakeModule.RegisterTypes();
    CHECK(g_registered == before + 1);
    ResourceModule silent{u8"silent", nullptr, nullptr, 0};
    silent.RegisterTypes(); // a module with no registrar is fine
    CHECK(silent.Factories().IsEmpty());
}

TEST_CASE("resource composition: the set creates what the services allow, reports the rest, fills "
          "the gap later and never creates a product twice")
{
    const ResourceModule* modules[] = {&kFakeModule};
    ResourceFactorySet set;

    // Headless: the gated factory is skipped and says which service it wanted.
    NoResourceServices none;
    set.Create(Span<const ResourceModule* const>{modules, 1}, DefaultAllocator(), none);
    CHECK(set.Count() == 1);
    CHECK(set.Has(PlainProduct::StaticType().id));
    CHECK_FALSE(set.Has(GatedProduct::StaticType().id));
    REQUIRE(set.Skipped().Size() == 1);
    CHECK(set.Skipped()[0]->product() == &GatedProduct::StaticType());
    CHECK(set.Skipped()[0]->service() == &TypeOf<FakeDevice>());

    // Still headless: nothing new, nothing duplicated, the skip still recorded once.
    set.Create(Span<const ResourceModule* const>{modules, 1}, DefaultAllocator(), none);
    CHECK(set.Count() == 1);
    CHECK(set.Skipped().Size() == 1);

    // The device arrives: the gap fills, the skip clears, the plain one is not made again.
    FakeDevice device;
    DeviceServices services(device);
    set.Create(Span<const ResourceModule* const>{modules, 1}, DefaultAllocator(), services);
    CHECK(set.Count() == 2);
    CHECK(set.Skipped().IsEmpty());
    usize seen = 0;
    set.ForEach(
        [&](const IResourceFactory& factory)
        {
            ++seen;
            if (factory.ProductType() == &GatedProduct::StaticType())
            {
                CHECK(&static_cast<const GatedFactory&>(factory).Device() == &device);
            }
        });
    CHECK(seen == 2);
}

TEST_CASE("resource composition: Register hands every created factory to a manager")
{
    foundation::vfs::NativeFileSystem mount(u8"scratch_resource_composition", DefaultAllocator());
    foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(),
                                            u8".rasset");
    ResourceManager manager(DefaultAllocator(), db);

    const ResourceModule* modules[] = {&kFakeModule};
    ResourceFactorySet set;
    FakeDevice device;
    DeviceServices services(device);
    set.Create(Span<const ResourceModule* const>{modules, 1}, DefaultAllocator(), services);
    set.Register(manager);
    CHECK(manager.FactoryCount() == 2);
    CHECK(manager.HasFactory(PlainProduct::StaticType().id));
    CHECK(manager.HasFactory(GatedProduct::StaticType().id));
}

TEST_CASE("resource composition: a factory with an optional service is made in every host, given it where there is one")
{
    constexpr ResourceFactoryDesc optional =
        FactoryWithOptionalService<PlainProduct, PlainSource, OptionalFactory, FakeDevice>();
    CHECK(optional.service == nullptr); // not gated: never skipped
    CHECK(optional.product() == &PlainProduct::StaticType());

    NoResourceServices none;
    UniquePtr<IResourceFactory> headless = optional.create(DefaultAllocator(), none);
    REQUIRE(headless.Get() != nullptr);
    CHECK(static_cast<OptionalFactory*>(headless.Get())->Device() == nullptr);

    FakeDevice device;
    DeviceServices services(device);
    UniquePtr<IResourceFactory> hosted = optional.create(DefaultAllocator(), services);
    REQUIRE(hosted.Get() != nullptr);
    CHECK(static_cast<OptionalFactory*>(hosted.Get())->Device() == &device);
}

