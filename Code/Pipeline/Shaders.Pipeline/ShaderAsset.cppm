// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Shaders - the `shaders.pipeline` module (tooling).
//
// Source-side custom shaders + their cook (Specs/custom-shaders.md, phase 1):
//   * ShaderAsset (pipeline::Asset): a shader name, ONE .hlsl file (the inherited fileName)
//     holding both stages, and each stage's entry point (VSMain / PSMain by default).
//   * ShaderAssetBuilder (DefaultAssetBuilder): reads the file (a low-level shader, or a surface
//     shader assembled from the engine's templates into its forward, depth and pick programs),
//     replaces its properties block with the generated set-2 declarations, and COMPILES every
//     variant both stages declare,
//     with the engine's shader library on the include path. A failure fails the cook with
//     `File.hlsl:line: message` (the cook keeps the last good product); success writes a
//     ShaderSource carrying the expanded source, both entries, both lattices and the property
//     table every material on the shader is laid out by.
//   * CheckShaderSource (:check): what the cook proves, as data - the editor and the MCP tools
//     check a shader with it without cooking.
//   * RegisterShaderCreators / ShaderFileImporter: File > New's Shader (the kShaderStarter text)
//     and a dropped .hlsl, each a shader asset linking its file in Sources/.
//   * ShaderImporter: an authoring helper that fills a ShaderAsset from a file path.
//
// Reads the .hlsl text through the build context's sources mount (all file access via the
// VFS). Never linked by the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

export module shaders.pipeline;

import foundation.core;
import pipeline.core;
import foundation.vfs;
import foundation.shaders;
import foundation.shaders.system; // kShaderFolder: the engine's shader library under the data root
import foundation.shaders.resource;
import foundation.content;
import pipeline.importer;
export import :check;

using namespace foundation::core;
using namespace foundation::shaders;

export namespace pipeline{
    // The starter a new low-level shader's .hlsl holds (RegisterShaderCreators): lit by the
    // engine's lighting library, every pass contract kept, its parameters in a properties block.
    inline constexpr StringView kShaderStarter = u8R"hlsl(// A low-level shader: the whole pixel stage is yours. This one shades the engine's way, with
// every light, shadow and reflection probe; change the pixel stage to make it your own.
// variants(vertex): SKINNED INSTANCED WIND
// variants(fragment): ALPHA_TEST GBUFFER DITHER
// preserve-interface (the pixel stage keeps VSOutput's fade input when DITHER is off)

// The vertex stage: the engine's, under the name VSMain.
#define FORWARD_VS_ENTRY VSMain
#include "forward_vs.hlsli"

// The frame's lights, shadows and image-based light, and what the forward pass writes.
#include "lighting.hlsli"
#include "forward_ps.hlsli"

// The material's parameters: the material page shows these as its rows.
properties
{
    Color   BaseColor   = (1, 1, 1, 1);
    float   Roughness   = 0.5 [0, 1];
    float   Metallic    = 0   [0, 1];
    float   AlphaCutoff = 0.5 [0, 1];   // the ALPHA_TEST threshold
    Texture AlbedoMap;
    Sampler MainSampler;
}

#ifdef GBUFFER
GBufferOutput PSMain(VSOutput input) {
#else
float4 PSMain(VSOutput input) : SV_Target0 {
#endif
#ifdef DITHER
    ScreenDoorFade(input.clip.xy, input.fade);
#endif
    float4 albedoTex = AlbedoMap.Sample(MainSampler, input.uv);
    float3 albedo = input.color.rgb * BaseColor.rgb * albedoTex.rgb;
    float  alpha  = saturate(input.color.a * BaseColor.a * albedoTex.a);
#ifdef ALPHA_TEST
    if (alpha < AlphaCutoff) { discard; }
#endif
    float3 N = normalize(input.normalWS);
    float3 V = normalize(CameraPos - input.worldPos);
    float  viewDepth = -mul(float4(input.worldPos, 1.0), View).z;
    float  roughness = clamp(Roughness, 0.045, 1.0);
    float  metallic  = saturate(Metallic);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    uint lightCount;
    float3 color = DirectLight(input.clip.xy, input.worldPos, viewDepth, N, V, albedo, roughness,
                               metallic, F0, lightCount)
                 + AmbientLight(input.worldPos, N, V, albedo, roughness, metallic, F0, 1.0);

#ifdef GBUFFER
    GBufferOutput o;
    o.color    = float4(color, alpha);
    o.normal   = OctEncode(normalize(mul(float4(N, 0.0), View).xyz));
    o.velocity = MotionVector(input.curClip, input.prevClip);
    o.material = float2(roughness, metallic);
    o.albedo   = float4(albedo * (1.0 - metallic), 1.0);
    return o;
#else
    return float4(color, alpha);
#endif
}
)hlsl";

    // The starter a new lit surface shader's .hlsl holds: the lava of the custom shaders docs.
    inline constexpr StringView kLitSurfaceShaderStarter = u8R"hlsl(// A surface shader: say what the surface is, and the engine lights it, with every light,
// shadow and reflection probe, in every pass (shadows and picking included).
// surface: lit

// The material's parameters: the material page shows these as its rows.
properties
{
    Color   CrackColor = (1.0, 0.35, 0.05, 1.0);
    float   Speed      = 0.2 [0, 2];
    float   Glow       = 4.0 [0, 20];
    Texture Cracks;
    Sampler MainSampler;
}

// Glowing cracks scrolling over dark rock. Give the material a Cracks texture (white = crack).
void Surface(SurfaceInput input, inout SurfaceOutput surface)
{
    float2 uv = input.uv + float2(0.0, Time() * Speed);
    float crack = Cracks.Sample(MainSampler, uv).r;
    surface.albedo = lerp(float3(0.08, 0.05, 0.04), CrackColor.rgb, crack);
    surface.roughness = 0.9;
    surface.emissive = CrackColor.rgb * crack * Glow;
}

// Optional: move the surface's vertices, in world space (its shadow and depth follow), e.g. waves:
// void Vertex(inout VertexInput v)
// {
//     v.position.y += sin(v.position.x * 2.0 + Time()) * 0.1;
// }
)hlsl";

    // The starter a new unlit surface shader's .hlsl holds: a colour and a texture, unaffected
    // by light.
    inline constexpr StringView kUnlitSurfaceShaderStarter = u8R"hlsl(// An unlit surface shader: the surface's colour as it is, unaffected by light (it still casts
// shadows and can be picked).
// surface: unlit

properties
{
    Color   BaseColor   = (1, 1, 1, 1);
    float   AlphaCutoff = 0.5 [0, 1];   // where a masked material cuts out
    Texture AlbedoMap;
    Sampler MainSampler;
}

void Surface(SurfaceInput input, inout SurfaceOutput surface)
{
    float4 c = AlbedoMap.Sample(MainSampler, input.uv) * BaseColor * input.color;
    surface.albedo = c.rgb;
    surface.alpha = c.a;
}
)hlsl";

    // Source asset: a shader's name, its one .hlsl file (the inherited fileName, relative to the
    // sources mount at cook time) and the entry point each stage's function is called in it.
    class ShaderAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(ShaderAsset, pipeline::Asset)
    public:
        String name; // logical shader name (how materials reference it)
        String vertexEntryPoint = String(u8"VSMain");
        String fragmentEntryPoint = String(u8"PSMain");

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (the .hlsl)
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "vertexEntryPoint", vertexEntryPoint);
            foundation::core::Serialize(ar, "fragmentEntryPoint", fragmentEntryPoint);
        }
    };

    // Cooks a ShaderAsset: expands its properties block, compiles every declared variant of both
    // stages, and writes the ShaderSource the runtime registers.
    class ShaderAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        // `dataRoot`: the engine's data root, whose shader library (`Shaders/`) is on every
        // custom shader's include path. Empty = includes resolve nowhere.
        explicit ShaderAssetBuilder(StringView dataRoot = {})
            : m_shaderLibrary(dataRoot.IsEmpty()
                                  ? String()
                                  : foundation::vfs::DataPath(dataRoot, kShaderFolder))
        {
        }

        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &ShaderAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ShaderSource::StaticType();
        }
        // v2: the product names each stage's entry point. v3: one file, compiled at cook time;
        // the product carries the expanded source, the lattices and the property table. v4: the
        // product carries programs beside the main one (a surface shader's depth and pick). v5: a
        // surface shader's vertex lattices declare WIND (its material's Wind group sways it), and
        // an effect block expands to a ConstantBuffer read as `Wind.Strength`.
        [[nodiscard]] u32 Version() const override { return 5; }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const ShaderAsset& sa =
                static_cast<const ShaderAsset&>(asset); // guarded by AssetType()
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            const StringView file = sa.fileName.View();
            if (const String taken = EngineShaderFile(sa.name.AsView()); !taken.IsEmpty())
            {
                LOG_ERROR(u8"Shaders",
                          u8"{}: the name '{}' is the engine's shader ({}): one name keys a shader "
                          u8"in the shader system and a dist's pack, so rename it - cook failed",
                          file, sa.name, taken);
                return Status{ErrorCode::InvalidArgument};
            }

            String text;
            const Status read = ReadSourceText(ctx, file, text);
            if (!read.IsOk())
            {
                return read;
            }

            // What the cook proves: the block expands, every flag tested is declared, and every
            // variant of both stages compiles. Each error is logged where it is, and fails it.
            ShaderCheckOptions options;
            options.vertexEntryPoint = sa.vertexEntryPoint.AsView();
            options.fragmentEntryPoint = sa.fragmentEntryPoint.AsView();
            options.shaderLibrary = m_shaderLibrary.AsView();
            options.depth = ShaderCheckDepth::EveryVariant;
            const ShaderCheck check = CheckShaderSource(text.AsView(), file, options);
            for (const ShaderDiagnostic& d : check.errors)
            {
                LOG_ERROR(u8"Shaders", u8"{} - cook failed", FormatShaderDiagnostic(d));
            }
            if (!check.IsOk())
            {
                return Status{ErrorCode::InvalidArgument};
            }

            // The main program, then the others (a surface shader's depth and pick).
            const ShaderCheckProgram& main = check.programs[0];
            ShaderSource source;
            source.name = sa.name;
            source.vertexSource = main.text;
            source.fragmentSource = main.text;
            source.vertexEntryPoint = main.vertexEntryPoint;
            source.fragmentEntryPoint = main.fragmentEntryPoint;
            source.vertexVariantMask = static_cast<u32>(main.vertexMask);
            source.fragmentVariantMask = static_cast<u32>(main.fragmentMask);
            for (usize i = 1; i < check.programs.Size(); ++i)
            {
                const ShaderCheckProgram& p = check.programs[i];
                source.AddProgram(p.suffix.AsView(), p.text.AsView(), p.vertexEntryPoint.AsView(),
                                  p.vertexMask, p.text.AsView(), p.fragmentEntryPoint.AsView(),
                                  p.fragmentMask);
            }
            source.depthProgramsAlways = check.hasVertex; // it moves its vertices in every pass
            source.SetProperties(check.block);
            return ctx.output->WriteObject(source);
        }

    private:
        // The engine library's file for a shader of this name (`forward` -> forward.ps.hlsl), or
        // empty when the name is free. A project shader with an engine shader's name would
        // replace it everywhere the name is looked up.
        [[nodiscard]] String EngineShaderFile(StringView name) const
        {
            if (m_shaderLibrary.IsEmpty() || name.IsEmpty())
            {
                return {};
            }
            constexpr StringView kSuffixes[] = {u8".vs.hlsl", u8".ps.hlsl", u8".cs.hlsl"};
            for (const StringView suffix : kSuffixes)
            {
                String stageFile(name);
                stageFile += suffix;
                if (FileExists(PathJoin(m_shaderLibrary.AsView(), stageFile.AsView()).AsView()))
                {
                    return stageFile;
                }
            }
            return {};
        }

        String m_shaderLibrary; // <data root>/Shaders, the include path
    };

    // Authoring helper: fill a ShaderAsset from a name + its .hlsl path.
    class ShaderImporter
    {
    public:
        static void Import(StringView name, StringView file, ShaderAsset& outAsset)
        {
            outAsset.name = String(name);
            outAsset.fileName = foundation::vfs::SourcePath(file);
        }
    };

    // Registers the shader creators (File > New, asset_create): Lit Surface Shader, Unlit Surface
    // Shader and Low-Level Shader, each its starter in a new .hlsl in Sources/, linked to a shader
    // asset named like its instance.
    void RegisterShaderCreators(AssetCreatorRegistry& registry);

    /// Drag-drop importer for `.hlsl` files: the file is STAGED into the project's Sources/ tree
    /// and a shader asset, named after its stem, LINKS it through fileName (as a script and a UI
    /// document do). The entry points stay VSMain and PSMain; the shader page edits them.
    class ShaderFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Shader"; }
        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return extension == u8"hlsl";
        }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath); // one asset, named after the stem
        }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(foundation::content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath, u8"ShaderAsset");
        }

        [[nodiscard]] Result<foundation::content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context,
               foundation::content::Group& group, const pipeline::ImportOptions* options, Object*,
               Array<pipeline::DeferredImportWrite>*) override
        {
            Result<String> fileName = pipeline::CopyIntoSources(context, sourcePath);
            if (!fileName.HasValue())
            {
                return Err(fileName.Error());
            }
            const StringView stem = pipeline::FileStemOf(fileName.Value().AsView());
            foundation::content::Instance* instance = group.CreateInstance(
                pipeline::SingleAssetName(options, stem), ShaderAsset::StaticType());
            if (instance == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }
            ShaderAsset asset;
            asset.name = String(instance->Name()); // materials find it by this name
            asset.fileName = foundation::vfs::SourcePath(fileName.Value().AsView());
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    // Registers ShaderAsset for content-DB construction + deserialization.
    inline void RegisterShaderAsset()
    {
        GlobalTypeRegistry().Register(ShaderAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<ShaderAsset>();
        // The cooked PRODUCT: ReadObject constructs it by type name in cook hosts, so it
        // must be registered here (no other production site registers it).
        GlobalTypeRegistry().Register(foundation::shaders::ShaderSource::StaticType());
        RegisterSerializable<foundation::shaders::ShaderSource>();
    }

    // ShaderAsset::StaticType() is defined WITH reflected properties in ShaderAssetImpl.cpp.
}
