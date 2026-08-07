// Materials as resources: author a Material, capture it into a MaterialSource that
// references a ShaderSource by id, then build the Material through the ResourceManager
// with both factories registered. Verifies the cooked Material resolves the shader's
// name + properties + default uniforms, and that binding the shader mid-build recorded
// a material->shader dependency edge (so a shader reload propagates). Real DXC + Null RHI.
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
import draconic.materials;
import draconic.materials.resource;

using namespace draconic::foundation;
using namespace draconic::vfs;
using namespace draconic::resource;
using namespace draconic::materials;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;

namespace
{
    constexpr const char8_t* kVtx =
        u8"float4 main(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n";
    constexpr const char8_t* kFrag = u8"float4 main() : SV_Target { return float4(1,0,0,1); }\n";

    void RemoveTree()
    {
        FileDelete(u8"draconic_mat_res_db/lit_shader.rasset");
        FileDelete(u8"draconic_mat_res_db/lit_mat.rasset");
        RemoveDirectory(u8"draconic_mat_res_db");
    }
}

TEST_CASE("material resource: built via the manager; resolves shader + records the dependency")
{
    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk())
    {
        MESSAGE("DXC unavailable; skipping");
        return;
    }

    GlobalTypeRegistry().Register(shaders::ShaderSource::StaticType());
    RegisterSerializable<shaders::ShaderSource>();
    GlobalTypeRegistry().Register(shaders::ShaderResource::StaticType());
    GlobalTypeRegistry().Register(MaterialSource::StaticType());
    RegisterSerializable<MaterialSource>();
    GlobalTypeRegistry().Register(Material::StaticType());

    RemoveTree();
    NativeFileSystem mount(u8"draconic_mat_res_db");

    Guid shaderId, matId;
    {
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                              u8".rasset");

        auto* shaderInst =
            db.RootGroup()->CreateInstance(u8"lit_shader", shaders::ShaderSource::StaticType());
        shaderId = shaderInst->Id();
        shaders::ShaderSource ss;
        ss.name = String(u8"lit");
        ss.vertexSource = String(kVtx);
        ss.fragmentSource = String(kFrag);
        REQUIRE(shaderInst->WriteObject(ss).IsOk());

        // author a material in code, then capture it into a MaterialSource referencing the shader
        RefPtr<Material> authored = MaterialBuilder(u8"litMat")
                                        .Shader(u8"lit")
                                        .Color(u8"tint", Float4{0.25f, 0.5f, 0.75f, 1.0f})
                                        .Float(u8"roughness", 0.4f)
                                        .Texture(u8"albedoMap")
                                        .Build();

        MaterialSource ms;
        MaterialSource::FromMaterial(*authored, shaderId, ms);

        auto* matInst = db.RootGroup()->CreateInstance(u8"lit_mat", MaterialSource::StaticType());
        matId = matInst->Id();
        REQUIRE(matInst->WriteObject(ms).IsOk());
    }

    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    rhi::null::NullDevice device{DefaultAllocator()};
    shaders::ShaderSystem system(*compiler, device);
    shaders::ShaderFactory shaderFactory(system);
    MaterialFactory materialFactory;
    ResourceManager manager(db);
    manager.AddFactory(&shaderFactory);
    manager.AddFactory(&materialFactory);

    Proxy<Material> mat = manager.Bind<Material>(matId);
    REQUIRE(mat);
    CHECK(mat->name == u8"litMat");
    CHECK(mat->shaderName == u8"lit"); // resolved from the bound ShaderResource
    CHECK(mat->PropertyCount() == 3);
    CHECK(mat->UniformDataSize() == 32); // float4 (16) + float (4), 16-byte rounded

    // default uniform data survived the round-trip (tint = 0.25,0.5,0.75,1 at offset 0)
    const Span<const u8> defaults = mat->DefaultUniformData();
    REQUIRE(defaults.Size() == 32);
    const f32* tint = reinterpret_cast<const f32*>(defaults.Data());
    CHECK(tint[0] == doctest::Approx(0.25f));
    CHECK(tint[2] == doctest::Approx(0.75f));
    CHECK(*reinterpret_cast<const f32*>(defaults.Data() + 16) ==
          doctest::Approx(0.4f)); // roughness

    // the factory's Bind of the shader recorded material -> shader
    const Span<const Guid> dependents = manager.Dependents(shaderId);
    bool found = false;
    for (const Guid& g : dependents)
    {
        if (g == matId)
        {
            found = true;
        }
    }
    CHECK(found);

    // reloading the shader is accepted (and transitively touches the material)
    CHECK(manager.Reload(shaderId));
    Proxy<Material> matAfter = manager.Bind<Material>(matId);
    REQUIRE(matAfter);
    CHECK(matAfter->shaderName == u8"lit");

    RemoveTree();
    compiler->Destroy();
}

TEST_CASE("material: CreatePBR packs EmissiveColor at the shader's cbuffer offset")
{
    // The forward cbuffer: BaseColor(0..16), Metallic(16..20), Roughness(20..24), pads to 32,
    // EmissiveColor(32..48). The builder's std140-ish packing must land the same offsets.
    RefPtr<Material> pbr = CreatePBR(u8"m");
    const MaterialPropertyDef* emissive = pbr->FindProperty(u8"EmissiveColor");
    REQUIRE(emissive != nullptr);
    CHECK(emissive->offset == 32u);
    CHECK(emissive->size == 16u);
    // The straggler scalars pack sequentially into the row after EmissiveColor.
    const MaterialPropertyDef* occ = pbr->FindProperty(u8"OcclusionStrength");
    const MaterialPropertyDef* ns = pbr->FindProperty(u8"NormalScale");
    const MaterialPropertyDef* ac = pbr->FindProperty(u8"AlphaCutoff");
    REQUIRE(occ != nullptr);
    REQUIRE(ns != nullptr);
    REQUIRE(ac != nullptr);
    CHECK(occ->offset == 48u);
    CHECK(ns->offset == 52u);
    CHECK(ac->offset == 56u);
    // Black default: adding the field changes nothing visually.
    const Span<const u8> defaults = pbr->DefaultUniformData();
    REQUIRE(defaults.Size() >= 48u);
    f32 rgb[3];
    MemCopy(rgb, defaults.Data() + 32, sizeof(rgb));
    CHECK(rgb[0] == 0.0f);
    CHECK(rgb[1] == 0.0f);
    CHECK(rgb[2] == 0.0f);
}

TEST_CASE("material: pre-emissive forward sources upgrade in memory (offset/pad/idempotent)")
{
    // Simulate an asset authored BEFORE EmissiveColor existed: the old CreatePBR property set.
    RefPtr<Material> old = MaterialBuilder(u8"legacy")
                               .Shader(u8"forward")
                               .VertexLayout(VertexLayoutType::Mesh)
                               .Color(u8"BaseColor", Float4{0.5f, 0.25f, 0.125f, 1.0f})
                               .Float(u8"Metallic", 1.0f)
                               .Float(u8"Roughness", 0.25f)
                               .Texture(u8"AlbedoMap")
                               .Sampler(u8"MainSampler")
                               .Build();
    MaterialSource src;
    MaterialSource::FromMaterial(*old, Guid{}, src);
    src.shaderName = String(u8"forward");
    REQUIRE(src.uniformDefaults.Size() == 32u); // the pre-emissive block, 16-byte rounded

    UpgradeForwardMaterialSource(src);
    REQUIRE(src.propNames.Size() ==
            9u); // + EmissiveColor/OcclusionStrength/NormalScale/AlphaCutoff
    // (appended AFTER the texture props - safe: the set-2 layout emits the uniform buffer
    // first regardless of property order)
    const auto find = [&](StringView name) -> usize
    {
        for (usize i = 0; i < src.propNames.Size(); ++i)
        {
            if (src.propNames[i].AsView() == name)
            {
                return i;
            }
        }
        return src.propNames.Size();
    };
    const usize e = find(u8"EmissiveColor");
    const usize occ = find(u8"OcclusionStrength");
    const usize ns = find(u8"NormalScale");
    const usize ac = find(u8"AlphaCutoff");
    REQUIRE(e < src.propNames.Size());
    CHECK(src.propOffsets[e] == 32u); // straight past the rounded pre-emissive block
    CHECK(src.propSizes[e] == 16u);
    REQUIRE(occ < src.propNames.Size());
    CHECK(src.propOffsets[occ] == 48u); // matches the shader cbuffer row
    CHECK(src.propOffsets[ns] == 52u);
    CHECK(src.propOffsets[ac] == 56u);
    REQUIRE(src.uniformDefaults.Size() == 60u);
    // The original bytes are untouched; the appended defaults are the NEUTRALS, not zeros.
    const auto readF32 = [&](u32 offset)
    {
        f32 v = 0.0f;
        MemCopy(&v, src.uniformDefaults.Data() + offset, sizeof(v));
        return v;
    };
    CHECK(readF32(0) == 0.5f);  // BaseColor.r preserved
    CHECK(readF32(32) == 0.0f); // emissive black
    CHECK(readF32(44) == 1.0f); // emissive alpha
    CHECK(readF32(48) == 1.0f); // occlusion strength
    CHECK(readF32(52) == 1.0f); // normal scale
    CHECK(readF32(56) == 0.5f); // alpha cutoff

    // Idempotent + non-forward untouched.
    UpgradeForwardMaterialSource(src);
    CHECK(src.propNames.Size() == 9u);
    MaterialSource unlit;
    unlit.shaderName = String(u8"unlit");
    UpgradeForwardMaterialSource(unlit);
    CHECK(unlit.propNames.Size() == 0u);
}

TEST_CASE("material source: sampler address modes round-trip (v2)")
{
    using namespace draconic::materials;
    NativeFileSystem mount(u8"draconic_mat_sampler_db");

    Guid id;
    {
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                              u8".rasset");
        MaterialSource source;
        source.name = u8"wrapped";
        source.shaderName = u8"forward";
        source.samplerU = 2; // rhi::AddressMode::ClampToEdge
        source.samplerV = 1; // rhi::AddressMode::MirrorRepeat
        auto* inst = db.RootGroup()->CreateInstance(u8"wrapped", MaterialSource::StaticType());
        id = inst->Id();
        REQUIRE(inst->WriteObject(source).IsOk());
    }
    {
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                              u8".rasset");
        RefPtr<ISerializable> object = db.ReadObject(id);
        auto* read = Cast<MaterialSource>(object.Get());
        REQUIRE(read != nullptr);
        CHECK(read->samplerU == 2); // the v2 envelope round-trips the modes
        CHECK(read->samplerV == 1);
    }

    FileDelete(u8"draconic_mat_sampler_db/wrapped.rasset");
    RemoveDirectory(u8"draconic_mat_sampler_db");
}

TEST_CASE("material resource: the retyped render-state enum fields round-trip byte-identically")
{
    // u8 -> BlendMode/DepthMode/CullModeConfig/VertexLayoutType (Fable Q2). Each enum serializes as
    // its underlying u8, so the wire is unchanged and old bytes load into the enum. These four are
    // serialized unconditionally (before the version-gated sampler fields), so version is irrelevant.
    MaterialSource out;
    out.blendMode = BlendMode::Additive;               // != default Opaque
    out.depthMode = DepthMode::WriteOnly;              // != default ReadWrite
    out.cullMode = CullModeConfig::Front;              // != default Back
    out.vertexLayout = VertexLayoutType::SkinnedMesh;  // != default Mesh

    MemoryStream buffer;
    {
        BinarySerializer writer(buffer, SerializeMode::Write);
        out.Serialize(writer);
    }
    MaterialSource in;
    {
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(buffer, SerializeMode::Read);
        in.Serialize(reader);
    }

    CHECK(in.blendMode == BlendMode::Additive);
    CHECK(in.depthMode == DepthMode::WriteOnly);
    CHECK(in.cullMode == CullModeConfig::Front);
    CHECK(in.vertexLayout == VertexLayoutType::SkinnedMesh);
}
