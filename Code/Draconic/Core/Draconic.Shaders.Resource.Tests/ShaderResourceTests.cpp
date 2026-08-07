// Shaders as resources: build a ShaderResource through the ResourceManager from an
// authored ShaderSource, compile variants through it, and verify a reload bumps the
// shader's version (the PSO-cache reload signal). Real DXC + Null RHI + content DB.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.rhi;
import draconic.rhi.null;
import draconic.shaders;
import draconic.shaders.system;
import draconic.shaders.resource;

using namespace draconic::foundation;
using namespace draconic::vfs;
using namespace draconic::resource;
using namespace draconic::shaders;
namespace rhi = draconic::rhi;

namespace
{
    constexpr const char8_t* kVtx =
        u8"float4 main(uint id : SV_VertexID) : SV_Position { return float4(0, 0, 0, 1); }\n";
    constexpr const char8_t* kFrag =
        u8"float4 main() : SV_Target {\n#ifndef NORMAL_MAP\n#error NORMAL_MAP required\n#endif\n   "
        u8" return float4(1, 0, 0, 1);\n}\n";

    void RemoveTree()
    {
        FileDelete(u8"draconic_shader_res_db/lit.rasset");
        RemoveDirectory(u8"draconic_shader_res_db");
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
    NativeFileSystem mount(u8"draconic_shader_res_db");

    Guid id;
    {
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"lit", ShaderSource::StaticType());
        id = inst->Id();
        ShaderSource s;
        s.name = String(u8"lit");
        s.vertexSource = String(kVtx);
        s.fragmentSource = String(kFrag);
        REQUIRE(inst->WriteObject(s).IsOk());
    }

    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    rhi::null::NullDevice device{DefaultAllocator()};
    ShaderSystem system(*compiler, device);
    ShaderFactory factory(system);
    ResourceManager manager(db);
    manager.AddFactory(&factory);

    Proxy<ShaderResource> shader = manager.Bind<ShaderResource>(id);
    REQUIRE(shader);
    CHECK(shader->Name() == u8"lit");
    CHECK(shader->GetVariant(ShaderStage::Vertex, ShaderFlags::None) != nullptr);
    CHECK(shader->GetVariant(ShaderStage::Fragment, ShaderFlags::NormalMap) != nullptr);
    CHECK(shader->GetVariant(ShaderStage::Fragment, ShaderFlags::None) == nullptr); // #error guard

    const u64 v0 = shader->Version();
    CHECK(v0 >= 1u); // creation registered + invalidated -> version bumped

    CHECK(manager.Reload(id));
    CHECK(shader->Version() > v0); // reload bumped the version (proxy follows the new product)

    RemoveTree();
    compiler->Destroy();
}
