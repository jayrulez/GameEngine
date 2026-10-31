// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::VG.Resource - the `foundation.vg.resource` module.
//
// Cooked vector images. The payload is validated text: the cook (VG.Pipeline) loads the `.svg`
// as the renderer reads it and fails on one it cannot, then ships the source text; whatever draws
// it parses it again. Nothing here is a UI's: a theme's icon, a game's HUD art or an editor's
// glyph is the same record, so it is free of the renderer and of either UI framework, and a
// headless tool reads it without them.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.vg.resource;

import foundation.core;
import foundation.resource;
import foundation.content;

using namespace foundation::core;
using namespace foundation::resource;

export namespace foundation::vg
{
    /// Cooked vector image: a validated `.svg` document.
    class VectorImageSource : public ISerializable
    {
        RTTI_OBJECT(VectorImageSource, ISerializable)
    public:
        String svg;

        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "svg", svg); }
    };

    /// The runtime product: the document's text, for whatever draws it.
    class VectorImage : public Object
    {
        RTTI_OBJECT(VectorImage, Object)
    public:
        String svg;
    };

    class VectorImageFactory final : public IResourceFactory
    {
    public:
        explicit VectorImageFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &VectorImage::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &VectorImageSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            VectorImageSource* source = Cast<VectorImageSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<VectorImage> image = MakeRef<VectorImage>(*m_allocator);
            image->svg = Move(source->svg);
            return image;
        }

    private:
        IAllocator* m_allocator;
    };

    inline void RegisterVGResource()
    {
        GlobalTypeRegistry().Register(VectorImageSource::StaticType());
        RegisterSerializable<VectorImageSource>();
        GlobalTypeRegistry().Register(VectorImage::StaticType());
    }

    RTTI_DEFINE_OBJECT(VectorImageSource, "rtti::vg")
    RTTI_DEFINE_OBJECT(VectorImage, "rtti::vg")
}

export namespace foundation::vg
{
    inline constexpr foundation::resource::ResourceFactoryDesc kVGResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<VectorImage, VectorImageSource, VectorImageFactory>(),
    };
    /// The vector graphics resource module: what the engine composition composes this
    /// library's factories from.
    inline constexpr foundation::resource::ResourceModule kVGResourceModule{
        u8"vg", &RegisterVGResource, kVGResourceFactories,
        sizeof(kVGResourceFactories) / sizeof(kVGResourceFactories[0])};
}
