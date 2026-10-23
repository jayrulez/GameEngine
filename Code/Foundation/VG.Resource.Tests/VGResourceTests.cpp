// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// VG.Resource: a cooked vector image read back through its factory, and the module the engine
// composition takes it from.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <filesystem>

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.vg.resource;

using namespace foundation::core;
using namespace foundation::resource;
using namespace foundation::vg;
namespace content = foundation::content;

namespace
{
    constexpr StringView kHeartSvg =
        u8"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        u8"<path d=\"M12 21 L3 12 A5 5 0 0 1 12 5 A5 5 0 0 1 21 12 Z\" fill=\"#E53935\"/></svg>";

    void RemoveAll(StringView dir)
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(reinterpret_cast<const char*>(String(dir).CStr())), ec);
    }
}

TEST_CASE("vg.resource: a cooked vector image binds as its product, the SVG text intact")
{
    RegisterVGResource();
    RemoveAll(u8"scratch_vgresource_db");
    REQUIRE(CreateDirectories(u8"scratch_vgresource_db"));
    {
        foundation::vfs::NativeFileSystem mount(u8"scratch_vgresource_db", DefaultAllocator());
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        content::Instance* heart = db.RootGroup()->CreateInstance(u8"heart", VectorImageSource::StaticType());
        REQUIRE(heart != nullptr);
        VectorImageSource cooked;
        cooked.svg = String(kHeartSvg);
        REQUIRE(heart->WriteObject(cooked).IsOk());

        VectorImageFactory factory(DefaultAllocator());
        CHECK(factory.ProductType() == &VectorImage::StaticType());
        CHECK(factory.CookedType() == &VectorImageSource::StaticType());
        ResourceManager manager(DefaultAllocator(), db);
        manager.AddFactory(&factory);
        Proxy<VectorImage> image = manager.Bind<VectorImage>(heart->Id());
        REQUIRE(image);
        CHECK(image->svg.AsView() == kHeartSvg);
    }
    RemoveAll(u8"scratch_vgresource_db");
}

TEST_CASE("vg.resource: the module offers the vector image's factory under its own id")
{
    CHECK(kVGResourceModule.id == StringView(u8"vg"));
    REQUIRE(kVGResourceModule.factoryCount == 1u);
}
