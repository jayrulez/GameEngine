// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Heightfield as a resource: capture a grid into a HeightfieldSource, cook it into a content DB,
// then build it back through the ResourceManager via the factory and verify the runtime grid
// round-trips its parameters and samples. Also the direct source round-trip and the
// inconsistent-cook guards.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.heightfield;
import foundation.heightfield.resource;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::heightfield;

namespace
{
    // A 65-grid over 64x64 world, Y range [0,10], samples rising along +X (a known ramp).
    RefPtr<Heightfield> MakeRampX()
    {
        RefPtr<Heightfield> hf =
            MakeRef<Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
        for (i32 z = 0; z < 65; ++z)
        {
            for (i32 x = 0; x < 65; ++x)
            {
                hf->SetSample(x, z, static_cast<Height>(static_cast<f32>(x) / 64.0f * 65535.0f + 0.5f));
            }
        }
        return hf;
    }

    void RemoveTree()
    {
        FileDelete(u8"scratch_hf_res_db/ramp.rasset");
        RemoveDirectory(u8"scratch_hf_res_db");
    }
}

TEST_CASE("heightfield resource: cook round-trips through the resource manager")
{
    RegisterHeightfieldResourceTypes();

    RemoveTree();
    NativeFileSystem mount(u8"scratch_hf_res_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"ramp", HeightfieldSource::StaticType());
        id = inst->Id();

        RefPtr<Heightfield> ramp = MakeRampX();
        HeightfieldSource src;
        HeightfieldSource::FromHeightfield(*ramp, src);
        ramp->SetHole(3, 4, true); // a cut sample rides the second stream
        REQUIRE(inst->WriteObject(src).IsOk());
        REQUIRE(inst->WriteData(kHeightStream, HeightfieldSource::HeightBlob(*ramp)).IsOk());
        REQUIRE(inst->WriteData(kHoleStream, HeightfieldSource::HoleBlob(*ramp)).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    HeightfieldFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<Heightfield> hf = manager.Bind<Heightfield>(id);
    REQUIRE(hf);
    CHECK(hf->Size() == 65);
    CHECK(hf->WorldSize().x == doctest::Approx(64.0f));
    CHECK(hf->MinY() == doctest::Approx(0.0f));
    CHECK(hf->MaxY() == doctest::Approx(10.0f));
    // Samples and sampled height survive the cook.
    CHECK(hf->GetSample(0, 0) == 0);
    CHECK(hf->GetSample(64, 0) == 65535);
    CHECK(hf->GetHeightAt(0.0f, 0.0f) == doctest::Approx(5.0f).epsilon(0.01));
    CHECK(hf->HoleCount() == 1u); // and the hole plane came back with it
    CHECK(hf->IsHole(3, 4));
    CHECK_FALSE(hf->IsHole(4, 4));
    CHECK(hf->HoleCount() == 1u); // and the hole plane came back with it
    CHECK(hf->IsHole(3, 4));
    CHECK_FALSE(hf->IsHole(4, 4));

    RemoveTree();
}

TEST_CASE("heightfield resource: FromHeightfield / Build reproduces the grid directly")
{
    RefPtr<Heightfield> ramp = MakeRampX();
    HeightfieldSource src;
    HeightfieldSource::FromHeightfield(*ramp, src);

    CHECK(src.size == 65);
    const Span<const byte> blob = HeightfieldSource::HeightBlob(*ramp);
    CHECK(blob.Size() == 65u * 65u * sizeof(Height));

    CHECK(HeightfieldSource::HoleBlob(*ramp).Size() == 65u * 65u);
    RefPtr<Heightfield> built = src.Build(blob, HeightfieldSource::HoleBlob(*ramp), DefaultAllocator());
    REQUIRE(built);
    CHECK(built->Size() == 65);
    CHECK(built->MaxY() == doctest::Approx(10.0f));
    // Every sample matches.
    bool allEqual = true;
    for (i32 z = 0; z < 65 && allEqual; ++z)
    {
        for (i32 x = 0; x < 65; ++x)
        {
            if (built->GetSample(x, z) != ramp->GetSample(x, z))
            {
                allEqual = false;
                break;
            }
        }
    }
    CHECK(allEqual);
}

TEST_CASE("heightfield resource: an inconsistent cook builds an empty grid, never a malformed one")
{
    Array<u8> bytes;
    // Invalid size.
    {
        HeightfieldSource src;
        src.size = 64; // not 64k+1
        src.worldSize = Float2{64.0f, 64.0f};
        src.maxY = 10.0f;
        bytes.Resize(64u * 64u * sizeof(Height));
        Array<u8> holes;
        holes.Resize(64u * 64u);
        RefPtr<Heightfield> built =
            src.Build(Span<const byte>(reinterpret_cast<const byte*>(bytes.Data()), bytes.Size()),
                      Span<const byte>(reinterpret_cast<const byte*>(holes.Data()), holes.Size()), DefaultAllocator());
        REQUIRE(built);
        CHECK(built->IsEmpty());
    }
    // Valid size but a blob that does not match size*size*2.
    {
        HeightfieldSource src;
        src.size = 65;
        src.worldSize = Float2{64.0f, 64.0f};
        src.maxY = 10.0f;
        bytes.Resize(10); // wrong length
        Array<u8> holes;
        holes.Resize(65u * 65u);
        RefPtr<Heightfield> built =
            src.Build(Span<const byte>(reinterpret_cast<const byte*>(bytes.Data()), bytes.Size()),
                      Span<const byte>(reinterpret_cast<const byte*>(holes.Data()), holes.Size()), DefaultAllocator());
        REQUIRE(built);
        CHECK(built->IsEmpty());
    }
    // Valid heights but a hole plane that does not match size*size: refused the same way.
    {
        HeightfieldSource src;
        src.size = 65;
        src.worldSize = Float2{64.0f, 64.0f};
        src.maxY = 10.0f;
        bytes.Resize(65u * 65u * sizeof(Height));
        Array<u8> holes;
        holes.Resize(7);
        RefPtr<Heightfield> built =
            src.Build(Span<const byte>(reinterpret_cast<const byte*>(bytes.Data()), bytes.Size()),
                      Span<const byte>(reinterpret_cast<const byte*>(holes.Data()), holes.Size()), DefaultAllocator());
        REQUIRE(built);
        CHECK(built->IsEmpty());
    }
}
