// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Shaders as resources: build a ShaderResource through the ResourceManager from an
// authored ShaderSource, compile variants through it, and verify a reload bumps the
// shader's version (the PSO-cache reload signal). Uses real DXC, the Null RHI and a content DB.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.rhi;
import foundation.rhi.null;
import foundation.shaders;
import foundation.shaders.system;
import foundation.shaders.resource;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::shaders;
namespace rhi = foundation::rhi;

namespace
{
    constexpr const char8_t* kVtx =
        u8"float4 main(uint id : SV_VertexID) : SV_Position { return float4(0, 0, 0, 1); }\n";
    constexpr const char8_t* kFrag =
        u8"float4 main() : SV_Target {\n#ifndef NORMAL_MAP\n#error NORMAL_MAP required\n#endif\n   "
        u8" return float4(1, 0, 0, 1);\n}\n";

    void RemoveTree()
    {
        FileDelete(u8"scratch_shader_res_db/lit.rasset");
        RemoveDirectory(u8"scratch_shader_res_db");
    }
}

TEST_CASE("shader resource: built via the resource manager; reload bumps version")
{
    Compiler* compiler = nullptr;
    if (!createCompiler(CompilerDesc{}, compiler).IsOk())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }

    GlobalTypeRegistry().Register(ShaderSource::StaticType());
    RegisterSerializable<ShaderSource>();
    GlobalTypeRegistry().Register(ShaderResource::StaticType());

    RemoveTree();
    NativeFileSystem mount(u8"scratch_shader_res_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"lit", ShaderSource::StaticType());
        id = inst->Id();
        ShaderSource s;
        s.name = String(u8"lit");
        s.vertexSource = String(kVtx);
        s.fragmentSource = String(kFrag);
        // What a cook writes beside the source: each stage's entry and declared lattice.
        s.vertexEntryPoint = String(kCorpusEntryPoint);
        s.fragmentEntryPoint = String(kCorpusEntryPoint);
        s.fragmentVariantMask = static_cast<u32>(ShaderFlags::NormalMap);
        REQUIRE(inst->WriteObject(s).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    rhi::null::NullDevice device{DefaultAllocator()};
    ShaderSystem system(*compiler, device);
    ShaderFactory factory(DefaultAllocator(), system);
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<ShaderResource> shader = manager.Bind<ShaderResource>(id);
    REQUIRE(shader);
    CHECK(shader->Name() == u8"lit");
    CHECK(shader->GetVariant(ShaderStage::Vertex, ShaderFlags::None) != nullptr);
    CHECK(shader->GetVariant(ShaderStage::Fragment, ShaderFlags::NormalMap) != nullptr);
    CHECK(shader->GetVariant(ShaderStage::Fragment, ShaderFlags::None) == nullptr); // #error guard

    const u64 v0 = shader->Version();
    CHECK(v0 >= 1u); // creation registered and invalidated, so the version was bumped

    CHECK(manager.Reload(id));
    CHECK(shader->Version() > v0); // reload bumped the version (proxy follows the new product)

    RemoveTree();
    compiler->Destroy();
}

// A shader with programs beside its main one (a surface shader's depth and pick): each is
// registered under the shader's name plus its suffix, and the resource says which a material uses
// (always, or only for a masked one).
TEST_CASE("shader resource: a shader's depth and pick programs register beside it")
{
    Compiler* compiler = nullptr;
    if (!createCompiler(CompilerDesc{}, compiler).IsOk())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }
    GlobalTypeRegistry().Register(ShaderSource::StaticType());
    RegisterSerializable<ShaderSource>();
    GlobalTypeRegistry().Register(ShaderResource::StaticType());
    RemoveTree();
    FileDelete(u8"scratch_shader_res_db/waves.rasset");
    NativeFileSystem mount(u8"scratch_shader_res_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(DefaultAllocator(), mount,
                                                BinarySerializerFactory(), u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"waves", ShaderSource::StaticType());
        id = inst->Id();
        ShaderSource s;
        s.name = String(u8"waves");
        s.vertexSource = String(kVtx);
        s.fragmentSource = String(kVtx);
        s.vertexEntryPoint = String(kCorpusEntryPoint);
        s.fragmentEntryPoint = String(u8"none"); // the main fragment is not asked for here
        s.AddProgram(kDepthProgramSuffix, kVtx, kCorpusEntryPoint, ShaderFlags::None, kVtx,
                     u8"none", ShaderFlags::None);
        s.AddProgram(kPickProgramSuffix, kVtx, kCorpusEntryPoint, ShaderFlags::None, kVtx,
                     u8"none", ShaderFlags::None);
        REQUIRE(inst->WriteObject(s).IsOk());
    }

    foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(),
                                            u8".rasset");
    {
        RefPtr<ISerializable> object = db.ReadObject(id);
        const auto* read = Cast<ShaderSource>(object.Get());
        REQUIRE(read != nullptr);
        const Array<ShaderProgram> programs = read->Programs(); // round-tripped
        REQUIRE(programs.Size() == 3u);
        CHECK(programs[0].name == u8"waves");
        CHECK(programs[1].name == u8"waves.depth");
        CHECK(programs[2].name == u8"waves.pick");
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    ShaderSystem system(*compiler, device);
    ShaderFactory factory(DefaultAllocator(), system);
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);
    Proxy<ShaderResource> shader = manager.Bind<ShaderResource>(id);
    REQUIRE(shader);
    CHECK(system.GetVariant(u8"waves.depth", ShaderStage::Vertex, ShaderFlags::None) != nullptr);
    CHECK(system.GetVariant(u8"waves.pick", ShaderStage::Vertex, ShaderFlags::None) != nullptr);
    // Not always: only a masked material uses them.
    CHECK(shader->DepthProgram(false).IsEmpty());
    CHECK(shader->DepthProgram(true) == u8"waves.depth");
    CHECK(shader->PickProgram(true) == u8"waves.pick");

    FileDelete(u8"scratch_shader_res_db/waves.rasset");
    RemoveTree();
    compiler->Destroy();
}

TEST_CASE("shader source: stripping an export's copy empties every program's HLSL")
{
    ShaderSource s;
    s.name = String(u8"waves");
    s.vertexSource = String(kVtx);
    s.fragmentSource = String(kVtx);
    s.AddProgram(kDepthProgramSuffix, kVtx, u8"VSMain", ShaderFlags::None, kVtx, u8"PSMain",
                 ShaderFlags::None);
    s.depthProgramsAlways = true;
    s.StripSources();
    for (const ShaderProgram& p : s.Programs())
    {
        CHECK(p.vertexSource.IsEmpty());
        CHECK(p.fragmentSource.IsEmpty());
    }
    CHECK(s.Programs()[1].vertexEntryPoint == u8"VSMain"); // entries and lattices stay
}
