// Reflection track P1: TextureAsset's reflected surface + the enum reflection its properties
// reference. Verifies the authored fields enumerate with tooling attributes, round-trip through
// get/set, and that the enum property types resolve named values (the generic asset page's
// enum-by-name dropdowns + the script backends read exactly this).
#include <doctest/doctest.h>
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"
#include <initializer_list>
import draconic.core;
import draconic.vfs;
import draconic.editor;
import draconic.image;
import draconic.texture;
import draconic.texture.editor;

using namespace draconic::core;
namespace image = draconic::image;

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
    draconic::texture::RegisterTextureAsset(); // registers the type + its enum reflection

    const TypeInfo& type = draconic::texture::TextureAsset::StaticType();

    // Identity preserved when the type went from identity-only to reflected.
    CHECK(CEq(type.name, "TextureAsset"));

    // The reflected set matches the authored/serialized surface (own properties, not inherited).
    CHECK(PropertyCount(type) == 11u);
    for (const char* name : {"colorSpace", "shape", "minFilter", "magFilter", "wrapU", "wrapV",
                             "wrapW", "generateMipmaps", "anisotropy", "embeddedWidth",
                             "embeddedHeight"})
    {
        CHECK_MESSAGE(FindProperty(type, name) != nullptr, name);
    }

    // Attributes the generic asset page consumes: friendly labels + a slider range.
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
    draconic::texture::RegisterTextureAsset();
    const TypeInfo& type = draconic::texture::TextureAsset::StaticType();

    draconic::texture::TextureAsset asset;
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
    draconic::texture::RegisterTextureAsset(); // also registers the enum reflection

    // The owning-module enums are reflected (enum-by-name dropdowns depend on this).
    const TypeInfo& shape = TypeOf<draconic::texture::TextureShape>();
    CHECK(IsEnum(shape));
    CHECK(EnumeratorCount(shape) == 5u);
    CHECK(CEq(EnumValueName(shape, static_cast<i64>(draconic::texture::TextureShape::Cubemap)),
              "Cubemap"));

    const TypeInfo& wrap = TypeOf<draconic::texture::TextureWrap>();
    CHECK(IsEnum(wrap));
    CHECK(CEq(EnumValueName(wrap, static_cast<i64>(draconic::texture::TextureWrap::ClampToEdge)),
              "ClampToEdge"));

    const TypeInfo& cs = TypeOf<image::ImageColorSpace>();
    CHECK(IsEnum(cs));
    CHECK(EnumeratorCount(cs) == 2u);
    CHECK(CEq(EnumValueName(cs, static_cast<i64>(image::ImageColorSpace::Linear)), "Linear"));

    // The colorSpace property's declared type IS the reflected enum (the page maps it to a dropdown).
    const PropertyInfo* colorSpace =
        FindProperty(draconic::texture::TextureAsset::StaticType(), "colorSpace");
    REQUIRE(colorSpace != nullptr);
    REQUIRE(colorSpace->type != nullptr);
    CHECK(IsEnum(*colorSpace->type));
}

TEST_CASE("reflection-p1: the base Asset::fileName is inherited by every concrete asset")
{
    draconic::editor::RegisterAssetReflection();
    draconic::texture::RegisterTextureAsset();

    const TypeInfo& type = draconic::texture::TextureAsset::StaticType();

    // fileName is NOT an own property of TextureAsset (it lives on the base)...
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
    draconic::texture::TextureAsset asset;
    Instance inst = Instance::From(&asset);
    CHECK(SetProperty(*fileName, inst,
                      Variant::From(draconic::vfs::SourcePath(u8"Textures/wood.png")))
              .IsOk());
    CHECK(asset.fileName.View() == StringView(u8"Textures/wood.png"));
    CHECK(GetProperty(*fileName, inst).Get<draconic::vfs::SourcePath>().View() ==
          StringView(u8"Textures/wood.png"));
}

TEST_CASE("reflection-p1: SourcePath reflects as a value type with read accessors")
{
    draconic::editor::RegisterAssetReflection(); // registers SourcePath reflection

    const TypeInfo& type = TypeOf<draconic::vfs::SourcePath>();
    CHECK(CEq(type.name, "SourcePath"));
    CHECK(FindMethod(type, "View") != nullptr);
    CHECK(FindMethod(type, "Stem") != nullptr);
    CHECK(FindMethod(type, "Extension") != nullptr);

    // Construct + read through reflection.
    Variant args[] = {Variant::From(StringView(u8"Audio/hit.wav"))};
    Result<Variant> made = Construct(type, Span<Variant>{args, 1});
    REQUIRE(made.HasValue());
    const draconic::vfs::SourcePath path = made.Value().Get<draconic::vfs::SourcePath>();
    CHECK(path.View() == StringView(u8"Audio/hit.wav"));
    CHECK(path.Extension().AsView() == StringView(u8"wav"));
}
