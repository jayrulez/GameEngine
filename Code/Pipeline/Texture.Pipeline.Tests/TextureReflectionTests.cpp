// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// TextureAsset's reflected surface and the enum reflection its properties
// reference. Verifies the authored fields enumerate with tooling attributes, round-trip through
// get/set, and that the enum property types resolve named values (the generic asset page's
// enum-by-name dropdowns and the script backends read exactly this).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <initializer_list>
import foundation.core;
import foundation.vfs;
import pipeline.core;
import foundation.image;
import foundation.texture;
import texture.pipeline;

using namespace foundation::core;
using namespace pipeline;
namespace image = foundation::image;

namespace
{
    // Reflection identifiers (type/property/enum names) are plain const char*; compare directly.
    bool CEq(const char* a, const char* b)
    {
        if (a == nullptr || b == nullptr)
        {
            return a == b;
        }
        while (*a != '\0' && *b != '\0')
        {
            if (*a != *b)
            {
                return false;
            }
            ++a;
            ++b;
        }
        return *a == *b;
    }

    // Reads a property's "displayName" attribute value (empty if none).
    StringView DisplayName(const PropertyInfo& p)
    {
        if (const Attribute* a = FindAttribute(p, u8"displayName"))
        {
            return a->value.Get<String>().AsView();
        }
        return StringView{};
    }
}

TEST_CASE("reflection-p1: TextureAsset exposes its authored properties with tooling attributes")
{
    pipeline::RegisterTextureAsset(); // registers the type and its enum reflection

    const TypeInfo& type = pipeline::TextureAsset::StaticType();

    // Identity preserved when the type went from identity-only to reflected.
    CHECK(CEq(type.name, "TextureAsset"));

    // The reflected set matches the authored/serialized surface (own properties, not inherited).
    CHECK(PropertyCount(type) == 13u);
    for (const char* name : {"colorSpace", "usage", "compression", "shape", "minFilter", "magFilter",
                             "wrapU", "wrapV", "wrapW", "generateMipmaps", "anisotropy",
                             "embeddedWidth", "embeddedHeight"})
    {
        CHECK_MESSAGE(FindProperty(type, name) != nullptr, name);
    }

    // Attributes the generic asset page consumes: friendly labels and a slider range.
    const PropertyInfo* aniso = FindProperty(type, "anisotropy");
    REQUIRE(aniso != nullptr);
    CHECK(DisplayName(*aniso) == StringView(u8"Anisotropy"));
    const Attribute* range = FindAttribute(*aniso, u8"range");
    REQUIRE(range != nullptr);
    CHECK(range->value.Get<Float4>().x == doctest::Approx(1.0f));
    CHECK(range->value.Get<Float4>().y == doctest::Approx(16.0f));

    const PropertyInfo* colorSpace = FindProperty(type, "colorSpace");
    REQUIRE(colorSpace != nullptr);
    CHECK(DisplayName(*colorSpace) == StringView(u8"Color Space"));
}

TEST_CASE("reflection-p1: TextureAsset scalar/bool properties round-trip through get/set")
{
    pipeline::RegisterTextureAsset();
    const TypeInfo& type = pipeline::TextureAsset::StaticType();

    pipeline::TextureAsset asset;
    Instance inst = Instance::From(&asset);

    const PropertyInfo* aniso = FindProperty(type, "anisotropy");
    REQUIRE(aniso != nullptr);
    CHECK(SetProperty(*aniso, inst, Variant::From(8.0f)).IsOk());
    CHECK(GetProperty(*aniso, inst).Get<f32>() == doctest::Approx(8.0f));
    CHECK(asset.anisotropy == doctest::Approx(8.0f));

    const PropertyInfo* mips = FindProperty(type, "generateMipmaps");
    REQUIRE(mips != nullptr);
    CHECK(SetProperty(*mips, inst, Variant::From(false)).IsOk());
    CHECK(GetProperty(*mips, inst).Get<bool>() == false);
    CHECK(asset.generateMipmaps == false);

    const PropertyInfo* w = FindProperty(type, "embeddedWidth");
    REQUIRE(w != nullptr);
    CHECK(SetProperty(*w, inst, Variant::From<u32>(256u)).IsOk());
    CHECK(GetProperty(*w, inst).Get<u32>() == 256u);
}

TEST_CASE("reflection-p1: enum property types resolve named values")
{
    pipeline::RegisterTextureAsset(); // also registers the enum reflection

    // The owning-module enums are reflected (enum-by-name dropdowns depend on this).
    const TypeInfo& shape = TypeOf<foundation::texture::TextureShape>();
    CHECK(IsEnum(shape));
    CHECK(EnumeratorCount(shape) == 5u);
    CHECK(CEq(EnumValueName(shape, static_cast<i64>(foundation::texture::TextureShape::Cubemap)),
              "Cubemap"));

    const TypeInfo& wrap = TypeOf<foundation::texture::TextureWrap>();
    CHECK(IsEnum(wrap));
    CHECK(CEq(EnumValueName(wrap, static_cast<i64>(foundation::texture::TextureWrap::ClampToEdge)),
              "ClampToEdge"));

    const TypeInfo& cs = TypeOf<image::ImageColorSpace>();
    CHECK(IsEnum(cs));
    CHECK(EnumeratorCount(cs) == 2u);
    CHECK(CEq(EnumValueName(cs, static_cast<i64>(image::ImageColorSpace::Linear)), "Linear"));

    // The colorSpace property's declared type is the reflected enum (the page maps it to a dropdown).
    const PropertyInfo* colorSpace =
        FindProperty(pipeline::TextureAsset::StaticType(), "colorSpace");
    REQUIRE(colorSpace != nullptr);
    REQUIRE(colorSpace->type != nullptr);
    CHECK(IsEnum(*colorSpace->type));
}

TEST_CASE("reflection-p1: the base Asset::fileName is inherited by every concrete asset")
{
    pipeline::RegisterAssetReflection();
    pipeline::RegisterTextureAsset();

    const TypeInfo& type = pipeline::TextureAsset::StaticType();

    // fileName is not an own property of TextureAsset (it lives on the base)...
    CHECK(FindProperty(type, "fileName") != nullptr);         // ...but the base chain finds it
    bool ownHasFileName = false;
    for (const PropertyInfo& p : Properties(type))
    {
        ownHasFileName = ownHasFileName || CEq(p.name, "fileName");
    }
    CHECK_FALSE(ownHasFileName); // proves it comes from the base, not duplicated per asset

    // Round-trip a SourcePath through the inherited property.
    const PropertyInfo* fileName = FindProperty(type, "fileName");
    REQUIRE(fileName != nullptr);
    pipeline::TextureAsset asset;
    Instance inst = Instance::From(&asset);
    CHECK(SetProperty(*fileName, inst,
                      Variant::From(foundation::vfs::SourcePath(u8"Textures/wood.png")))
              .IsOk());
    CHECK(asset.fileName.View() == StringView(u8"Textures/wood.png"));
    CHECK(GetProperty(*fileName, inst).Get<foundation::vfs::SourcePath>().View() ==
          StringView(u8"Textures/wood.png"));
}

TEST_CASE("reflection-p1: SourcePath reflects as a value type with read accessors")
{
    pipeline::RegisterAssetReflection(); // registers SourcePath reflection

    const TypeInfo& type = TypeOf<foundation::vfs::SourcePath>();
    CHECK(CEq(type.name, "SourcePath"));
    CHECK(FindMethod(type, "View") != nullptr);
    CHECK(FindMethod(type, "Stem") != nullptr);
    CHECK(FindMethod(type, "Extension") != nullptr);

    // Construct and read through reflection.
    Variant args[] = {Variant::From(StringView(u8"Audio/hit.wav"))};
    Result<Variant> made = Construct(type, Span<Variant>{args, 1});
    REQUIRE(made.HasValue());
    const foundation::vfs::SourcePath path = made.Value().Get<foundation::vfs::SourcePath>();
    CHECK(path.View() == StringView(u8"Audio/hit.wav"));
    CHECK(path.Extension().AsView() == StringView(u8"wav"));
}
