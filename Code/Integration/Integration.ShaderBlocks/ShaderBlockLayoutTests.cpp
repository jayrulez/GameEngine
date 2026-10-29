// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Integration.ShaderBlocks - every engine shader block against its C++ mirror, field by field
// (Specs/shader-blocks-cleanup.md, phase 0). The engine's shaders are cooked to SPIR-V as the
// engine cook does (CookEngineShaders), and each block's members are read from what DXC compiled:
// its OpMemberName and OpMemberDecorate Offset, the offsets Vulkan uses and the WGSL translation
// starts from. A field added, moved or renamed on one side only fails here, which a size check
// alone does not catch (two fields swapped, a value moved into padding of the same size).
#include <doctest/doctest.h>
#include <cstddef>
#include <cstring>
#include "Core/Prelude.h"

import foundation.core;
import foundation.materials;
import foundation.render;
import foundation.shaders;
import foundation.vfs;
import foundation.vg.renderer;
import engine.particles;
import engine.terrain;

using namespace foundation::core;
namespace materials = foundation::materials;
namespace render = foundation::render;
namespace shaders = foundation::shaders;
namespace vg = foundation::vg;
namespace particles = engine::particles;
namespace terrain = engine::terrain;

namespace
{
    // A block's members as DXC compiled them: name and byte offset, in declaration order.
    struct SpirvMember
    {
        String name;
        u32 offset = 0;
        bool hasOffset = false;
    };

    // Reads the debug names and the member offsets of every named struct type in a SPIR-V module.
    // Only OpName (5), OpMemberName (6) and OpMemberDecorate (72) with Offset (35) are read; the
    // annotations all precede the first function, so the walk stops there (OpFunction, 54).
    class SpirvBlocks
    {
    public:
        explicit SpirvBlocks(const Array<byte>& blob)
        {
            const usize count = blob.Size() / 4u;
            if (count < 5u)
            {
                return;
            }
            const auto word = [&](usize i)
            {
                u32 w = 0;
                std::memcpy(&w, blob.Data() + i * 4u, 4u);
                return w;
            };
            if (word(0) != 0x07230203u)
            {
                return;
            }
            usize i = 5u;
            while (i < count)
            {
                const u32 head = word(i);
                const u32 op = head & 0xFFFFu;
                const u32 length = head >> 16u;
                if (length == 0u || i + length > count || op == 54u)
                {
                    break;
                }
                const auto text = [&](usize at)
                {
                    const char* s = reinterpret_cast<const char*>(blob.Data() + at * 4u);
                    usize n = 0;
                    while (at * 4u + n < (i + length) * 4u && s[n] != '\0')
                    {
                        ++n;
                    }
                    return String(StringView(reinterpret_cast<const char8_t*>(s), n));
                };
                if (op == 5u && length >= 3u)
                {
                    m_typeNames.PushBack(TypeName{word(i + 1u), text(i + 2u)});
                }
                else if (op == 6u && length >= 4u)
                {
                    Member(word(i + 1u), word(i + 2u)).name = text(i + 3u);
                }
                else if (op == 72u && length >= 5u && word(i + 3u) == 35u)
                {
                    SpirvMember& m = Member(word(i + 1u), word(i + 2u));
                    m.offset = word(i + 4u);
                    m.hasOffset = true;
                }
                i += length;
            }
        }

        // The members of the struct named `name` (DXC's names: `type.<cbuffer>`,
        // `type.ConstantBuffer.<struct>`, or a structured buffer element's own name); null if absent.
        [[nodiscard]] const Array<SpirvMember>* Find(StringView name) const
        {
            for (const TypeName& type : m_typeNames)
            {
                if (type.name.AsView() == name)
                {
                    for (const TypeMembers& members : m_members)
                    {
                        if (members.type == type.id)
                        {
                            return &members.members;
                        }
                    }
                }
            }
            return nullptr;
        }

    private:
        struct TypeName
        {
            u32 id = 0;
            String name;
        };
        struct TypeMembers
        {
            u32 type = 0;
            Array<SpirvMember> members;
        };

        SpirvMember& Member(u32 type, u32 index)
        {
            TypeMembers* found = nullptr;
            for (TypeMembers& members : m_members)
            {
                if (members.type == type)
                {
                    found = &members;
                    break;
                }
            }
            if (found == nullptr)
            {
                m_members.PushBack(TypeMembers{type, Array<SpirvMember>{}});
                found = &m_members.Back();
            }
            while (found->members.Size() <= index)
            {
                found->members.PushBack(SpirvMember{});
            }
            return found->members[index];
        }

        Array<TypeName> m_typeNames;
        Array<TypeMembers> m_members;
    };

    // One member as the C++ mirror lays it out, under the shader's name for it.
    struct FieldRow
    {
        const char8_t* name;
        usize offset;
    };

    // One block: the program (and variant) that declares it, DXC's name for it, and its mirror's
    // fields.
    struct BlockRow
    {
        const char8_t* shader;
        shaders::ShaderStage stage;
        shaders::ShaderFlags flags;
        const char8_t* spirvName;
        Span<const FieldRow> fields;
    };

#define FIELD(Type, Member, Name) FieldRow{u8##Name, offsetof(Type, Member)}

    // ---- The table: every engine shader block and its C++ mirror ----

    const FieldRow kGpuLight[] = {
        FIELD(render::GpuLight, positionWS, "positionWS"),
        FIELD(render::GpuLight, range, "range"),
        FIELD(render::GpuLight, color, "color"),
        FIELD(render::GpuLight, intensity, "intensity"),
        FIELD(render::GpuLight, directionWS, "directionWS"),
        FIELD(render::GpuLight, type, "type"),
        FIELD(render::GpuLight, innerCos, "innerCos"),
        FIELD(render::GpuLight, outerCos, "outerCos"),
        FIELD(render::GpuLight, shadowIndex, "shadowIndex"),
        FIELD(render::GpuLight, shadowStrength, "shadowStrength"),
    };
    const FieldRow kGpuLocalShadow[] = {
        FIELD(render::GpuLocalShadow, viewProj, "viewProj"),
        FIELD(render::GpuLocalShadow, atlasScaleBias, "atlasScaleBias"),
        FIELD(render::GpuLocalShadow, depthBias, "depthBias"),
        FIELD(render::GpuLocalShadow, atlasSelect, "atlasSelect"),
        FIELD(render::GpuLocalShadow, normalBiasPerDistance, "normalBiasPerDistance"),
        FIELD(render::GpuLocalShadow, pad2, "_localPad"),
    };
    const FieldRow kGpuProbe[] = {
        FIELD(render::GpuProbe, center, "center"),
        FIELD(render::GpuProbe, intensity, "intensity"),
        FIELD(render::GpuProbe, boxMin, "boxMin"),
        FIELD(render::GpuProbe, blendDistance, "blendDistance"),
        FIELD(render::GpuProbe, boxMax, "boxMax"),
        FIELD(render::GpuProbe, slice, "slice"),
        FIELD(render::GpuProbe, mipCount, "mipCount"),
        FIELD(render::GpuProbe, priority, "priority"),
        FIELD(render::GpuProbe, parallax, "parallax"),
        FIELD(render::GpuProbe, _pad, "_probeRecordPad"),
    };
    const FieldRow kDecalUniforms[] = {
        FIELD(render::DecalUniforms, world, "World"),
        FIELD(render::DecalUniforms, invWorld, "InvWorld"),
        FIELD(render::DecalUniforms, invViewProj, "InvViewProj"),
        FIELD(render::DecalUniforms, color, "Color"),
        FIELD(render::DecalUniforms, params, "Params"),
        FIELD(render::DecalUniforms, flip, "Flip"),
    };

    // The forward view (view.hlsli), as every forward stage declares it.
    const FieldRow kView[] = {
        FIELD(render::GpuView, viewProj, "ViewProj"),
        FIELD(render::GpuView, view, "View"),
        FIELD(render::GpuView, cascadeViewProj, "CascadeViewProj"),
        FIELD(render::GpuView, cameraPos, "CameraPos"),
        FIELD(render::GpuView, lightCount, "LightCount"),
        FIELD(render::GpuView, lightOffset, "LightOffset"),
        FIELD(render::GpuView, clusterViewportX, "ClusterVpX"),
        FIELD(render::GpuView, clusterViewportY, "ClusterVpY"),
        FIELD(render::GpuView, iblMaxLod, "IBLMaxLod"),
        FIELD(render::GpuView, clusterGridX, "ClusterGridX"),
        FIELD(render::GpuView, clusterGridY, "ClusterGridY"),
        FIELD(render::GpuView, clusterSliceCount, "ClusterSliceCount"),
        FIELD(render::GpuView, clusterTileSize, "ClusterTileSize"),
        FIELD(render::GpuView, clusterNear, "ClusterNear"),
        FIELD(render::GpuView, clusterFar, "ClusterFar"),
        FIELD(render::GpuView, clusterLogScale, "ClusterLogScale"),
        FIELD(render::GpuView, clusterLogBias, "ClusterLogBias"),
        FIELD(render::GpuView, ambient, "Ambient"),
        FIELD(render::GpuView, shadowCascadeCount, "ShadowCascadeCount"),
        FIELD(render::GpuView, cascadeSplitFar, "CascadeSplitFar"),
        FIELD(render::GpuView, cascadeTexelSize, "CascadeTexelSize"),
        FIELD(render::GpuView, shadowNormalBias, "ShadowNormalBias"),
        FIELD(render::GpuView, shadowDepthBias, "ShadowDepthBias"),
        FIELD(render::GpuView, cascadeLayerBase, "CascadeLayerBase"),
        FIELD(render::GpuView, localShadowBase, "LocalShadowBase"),
        FIELD(render::GpuView, prevViewProj, "PrevViewProj"),
        FIELD(render::GpuView, jitter, "Jitter"),
        FIELD(render::GpuView, probeBase, "ProbeBase"),
        FIELD(render::GpuView, probeCount, "ProbeCount"),
        FIELD(render::GpuView, _probePad0, "_probePad0"),
        FIELD(render::GpuView, _probePad1, "_probePad1"),
        FIELD(render::GpuView, shadowParams, "ShadowParams"),
        FIELD(render::GpuView, debugParams, "DebugParams"),
        FIELD(render::GpuView, iblDiffuse, "IblDiffuse"),
        FIELD(render::GpuView, iblSpecular, "IblSpecular"),
        FIELD(render::GpuView, sceneTime, "SceneTime"),
        FIELD(render::GpuView, prevSceneTime, "PrevSceneTime"),
    };
    // The per-draw block: one C++ struct and one declaration (object_block.hlsli) that the forward,
    // shadow depth and pick vertex stages all include, every field by its name in each.
    const FieldRow kObject[] = {
        FIELD(render::GpuObject, world, "World"),
        FIELD(render::GpuObject, prevWorld, "PrevWorld"),
        FIELD(render::GpuObject, tint, "Tint"),
        FIELD(render::GpuObject, boneBase, "BoneBase"),
        FIELD(render::GpuObject, prevBoneBase, "PrevBoneBase"),
        FIELD(render::GpuObject, pickIndex, "PickIndex"),
        FIELD(render::GpuObject, pickGeneration, "PickGeneration"),
        FIELD(render::GpuObject, fade, "Fade"),
        FIELD(render::GpuObject, _objectPad0, "_objectPad0"),
        FIELD(render::GpuObject, _objectPad1, "_objectPad1"),
        FIELD(render::GpuObject, _objectPad2, "_objectPad2"),
    };
    const FieldRow kInstance[] = {
        FIELD(render::GpuInstance, world, "World"),
        FIELD(render::GpuInstance, prevWorld, "PrevWorld"),
        FIELD(render::GpuInstance, tint, "Tint"),
    };
    const FieldRow kShadowView[] = {
        FIELD(render::GpuShadowView, lightViewProj, "LightViewProj"),
        FIELD(render::GpuShadowView, camera, "ShadowCamera"),
        FIELD(render::GpuShadowView, fadeFromCamera, "ShadowFadeFromCamera"),
        FIELD(render::GpuShadowView, sceneTime, "SceneTime"),
        FIELD(render::GpuShadowView, _pad0, "_shadowPad0"),
        FIELD(render::GpuShadowView, _pad1, "_shadowPad1"),
        FIELD(render::GpuShadowView, _pad2, "_shadowPad2"),
    };
    const FieldRow kPickView[] = {
        FIELD(render::GpuPickView, viewProj, "ViewProj"),
        FIELD(render::GpuPickView, sceneTime, "SceneTime"),
        FIELD(render::GpuPickView, _pad0, "_pickPad0"),
        FIELD(render::GpuPickView, _pad1, "_pickPad1"),
        FIELD(render::GpuPickView, _pad2, "_pickPad2"),
        FIELD(render::GpuPickView, camera, "PickCamera"),
        FIELD(render::GpuPickView, _pad3, "_pickPad3"),
    };
    // An instanced draw's set: its fade window, count and set pick id (instance_fade.hlsli).
    const FieldRow kInstanceSet[] = {
        FIELD(render::GpuInstanceSet, fadeStart, "FadeStart"),
        FIELD(render::GpuInstanceSet, fadeEnd, "FadeEnd"),
        FIELD(render::GpuInstanceSet, instanceCount, "InstanceCount"),
        FIELD(render::GpuInstanceSet, _pad0, "_setPad0"),
        FIELD(render::GpuInstanceSet, pickIndex, "SetPickIndex"),
        FIELD(render::GpuInstanceSet, pickGeneration, "SetPickGeneration"),
        FIELD(render::GpuInstanceSet, _pad1, "_setPad1"),
        FIELD(render::GpuInstanceSet, _pad2, "_setPad2"),
    };

    // ---- Sky and image lighting ----

    const FieldRow kSky[] = {
        FIELD(render::GpuSky, invViewProj, "InvViewProj"),
        FIELD(render::GpuSky, prevViewProj, "PrevViewProj"),
        FIELD(render::GpuSky, camPosIntensity, "CamPosIntensity"),
        FIELD(render::GpuSky, sunDir, "SunDir"),
        FIELD(render::GpuSky, sunColor, "SunColor"),
        FIELD(render::GpuSky, jitter, "Jitter"),
        FIELD(render::GpuSky, skyFlags, "SkyFlags"),
    };
    const FieldRow kIblPush[] = {
        FIELD(render::GpuIblPush, faceIndex, "FaceIndex"),
        FIELD(render::GpuIblPush, mode, "Mode"),
        FIELD(render::GpuIblPush, roughness, "Roughness"),
        FIELD(render::GpuIblPush, skyIntensity, "SkyIntensity"),
        FIELD(render::GpuIblPush, sun, "Sun"),
        FIELD(render::GpuIblPush, horizon, "Horizon"),
        FIELD(render::GpuIblPush, zenith, "Zenith"),
        FIELD(render::GpuIblPush, ground, "Ground"),
    };
    // probe_prefilter_ps.ps.hlsl's struct Push; its float2 Pad is the mirror's pad0 and pad1.
    const FieldRow kPrefilterPush[] = {
        FIELD(render::PrefilterPush, faceIndex, "FaceIndex"),
        FIELD(render::PrefilterPush, roughness, "Roughness"),
        FIELD(render::PrefilterPush, pad0, "Pad"),
    };

    // ---- Clustered lighting ----

    const FieldRow kClusterBuildParams[] = {
        FIELD(render::GpuClusterBuildParams, gridX, "GridX"),
        FIELD(render::GpuClusterBuildParams, gridY, "GridY"),
        FIELD(render::GpuClusterBuildParams, sliceCount, "SliceCount"),
        FIELD(render::GpuClusterBuildParams, tileSize, "TileSize"),
        FIELD(render::GpuClusterBuildParams, nearZ, "Near"),
        FIELD(render::GpuClusterBuildParams, farZ, "Far"),
        FIELD(render::GpuClusterBuildParams, logScale, "LogScale"),
        FIELD(render::GpuClusterBuildParams, logBias, "LogBias"),
        FIELD(render::GpuClusterBuildParams, lightCount, "LightCount"),
        FIELD(render::GpuClusterBuildParams, lightOffset, "LightOffset"),
        FIELD(render::GpuClusterBuildParams, orthographic, "Orthographic"),
        FIELD(render::GpuClusterBuildParams, pad0, "_pad"),
        FIELD(render::GpuClusterBuildParams, viewMatrix, "ViewMatrix"),
        FIELD(render::GpuClusterBuildParams, invProjection, "InvProjection"),
    };

    // ---- Screen-space effects ----

    const FieldRow kGtaoPush[] = {
        FIELD(render::GpuGtaoPush, invProj, "InvProj"),
        FIELD(render::GpuGtaoPush, texelSize, "TexelSize"),
        FIELD(render::GpuGtaoPush, radius, "Radius"),
        FIELD(render::GpuGtaoPush, intensity, "Intensity"),
        FIELD(render::GpuGtaoPush, projScaleY, "ProjScaleY"),
        FIELD(render::GpuGtaoPush, frameMod, "FrameMod"),
        FIELD(render::GpuGtaoPush, debugMode, "DebugMode"),
        FIELD(render::GpuGtaoPush, orthographic, "Orthographic"),
    };
    const FieldRow kSsaoPush[] = {
        FIELD(render::GpuSsaoPush, invProj, "InvProj"),
        FIELD(render::GpuSsaoPush, texelSize, "TexelSize"),
        FIELD(render::GpuSsaoPush, jitter, "Jitter"),
        FIELD(render::GpuSsaoPush, projXX, "ProjXX"),
        FIELD(render::GpuSsaoPush, projYY, "ProjYY"),
        FIELD(render::GpuSsaoPush, radius, "Radius"),
        FIELD(render::GpuSsaoPush, intensity, "Intensity"),
        FIELD(render::GpuSsaoPush, bias, "Bias"),
        FIELD(render::GpuSsaoPush, sampleCount, "SampleCount"),
        FIELD(render::GpuSsaoPush, debugMode, "DebugMode"),
        FIELD(render::GpuSsaoPush, orthographic, "Orthographic"),
        FIELD(render::GpuSsaoPush, pad, "_pad"),
    };
    const FieldRow kAoBlurPush[] = {
        FIELD(render::GpuAoBlurPush, dir, "Dir"),
        FIELD(render::GpuAoBlurPush, texelSize, "TexelSize"),
        FIELD(render::GpuAoBlurPush, depthSigma, "DepthSigma"),
        FIELD(render::GpuAoBlurPush, p0, "_pad0"),
        FIELD(render::GpuAoBlurPush, p1, "_pad1"),
        FIELD(render::GpuAoBlurPush, p2, "_pad2"),
    };
    const FieldRow kAoApplyPush[] = {
        FIELD(render::GpuAoApplyPush, strength, "Strength"),
        FIELD(render::GpuAoApplyPush, flipAoY, "FlipAoY"),
        FIELD(render::GpuAoApplyPush, p1, "_pad1"),
        FIELD(render::GpuAoApplyPush, p2, "_pad2"),
    };
    const FieldRow kBloomPush[] = {
        FIELD(render::GpuBloomPush, srcTexel, "SrcTexel"),
        FIELD(render::GpuBloomPush, threshold, "Threshold"),
        FIELD(render::GpuBloomPush, knee, "Knee"),
        FIELD(render::GpuBloomPush, firstPass, "FirstPass"),
        FIELD(render::GpuBloomPush, pad0, "_pad0"),
        FIELD(render::GpuBloomPush, pad1, "_pad1"),
        FIELD(render::GpuBloomPush, pad2, "_pad2"),
    };
    const FieldRow kTaaPush[] = {
        FIELD(render::GpuTaaPush, texelSize, "TexelSize"),
        FIELD(render::GpuTaaPush, blendFactor, "BlendFactor"),
        FIELD(render::GpuTaaPush, historyValid, "HistoryValid"),
        FIELD(render::GpuTaaPush, varianceGamma, "VarianceGamma"),
        FIELD(render::GpuTaaPush, motionScale, "MotionScale"),
        FIELD(render::GpuTaaPush, nearPlane, "NearPlane"),
        FIELD(render::GpuTaaPush, farPlane, "FarPlane"),
        FIELD(render::GpuTaaPush, orthographic, "Orthographic"),
        FIELD(render::GpuTaaPush, pad0, "_pad0"),
        FIELD(render::GpuTaaPush, pad1, "_pad1"),
        FIELD(render::GpuTaaPush, pad2, "_pad2"),
    };
    const FieldRow kSsgiPush[] = {
        FIELD(render::GpuSsgiPush, invProj, "InvProj"),
        FIELD(render::GpuSsgiPush, vpMin, "VpMin"),
        FIELD(render::GpuSsgiPush, vpSize, "VpSize"),
        FIELD(render::GpuSsgiPush, jitter, "Jitter"),
        FIELD(render::GpuSsgiPush, projXX, "ProjXX"),
        FIELD(render::GpuSsgiPush, projYY, "ProjYY"),
        FIELD(render::GpuSsgiPush, thickness, "Thickness"),
        FIELD(render::GpuSsgiPush, radius, "Radius"),
        FIELD(render::GpuSsgiPush, maxSteps, "MaxSteps"),
        FIELD(render::GpuSsgiPush, rayCount, "RayCount"),
        FIELD(render::GpuSsgiPush, ySign, "YSign"),
        FIELD(render::GpuSsgiPush, frameIndex, "FrameIndex"),
        FIELD(render::GpuSsgiPush, maxRadiance, "MaxRadiance"),
        FIELD(render::GpuSsgiPush, pad0, "_pad0"),
    };
    const FieldRow kSsgiDownPush[] = {
        FIELD(render::GpuSsgiDownPush, srcTexelSize, "SrcTexelSize"),
        FIELD(render::GpuSsgiDownPush, pad0, "_pad0"),
    };
    const FieldRow kSsgiBlurPush[] = {
        FIELD(render::GpuSsgiBlurPush, texelSize, "TexelSize"),
        FIELD(render::GpuSsgiBlurPush, depthSigma, "DepthSigma"),
        FIELD(render::GpuSsgiBlurPush, step, "Step"),
        FIELD(render::GpuSsgiBlurPush, invProj, "InvProj"),
        FIELD(render::GpuSsgiBlurPush, vpMin, "VpMin"),
        FIELD(render::GpuSsgiBlurPush, vpSize, "VpSize"),
        FIELD(render::GpuSsgiBlurPush, ySign, "YSign"),
        FIELD(render::GpuSsgiBlurPush, pad1, "_pad1"),
    };
    // The shader packs ViewToWorld0 to the next 16-byte row; the mirror's pad0 and pad1 fill the gap
    // and are no members of the shader's block.
    const FieldRow kSsgiResolvePush[] = {
        FIELD(render::GpuSsgiResolvePush, vpMin, "VpMin"),
        FIELD(render::GpuSsgiResolvePush, vpSize, "VpSize"),
        FIELD(render::GpuSsgiResolvePush, texelSize, "TexelSize"),
        FIELD(render::GpuSsgiResolvePush, blendFactor, "BlendFactor"),
        FIELD(render::GpuSsgiResolvePush, historyValid, "HistoryValid"),
        FIELD(render::GpuSsgiResolvePush, varianceGamma, "VarianceGamma"),
        FIELD(render::GpuSsgiResolvePush, motionScale, "MotionScale"),
        FIELD(render::GpuSsgiResolvePush, temporalOn, "TemporalOn"),
        FIELD(render::GpuSsgiResolvePush, debug, "Debug"),
        FIELD(render::GpuSsgiResolvePush, outputAccum, "OutputAccum"),
        FIELD(render::GpuSsgiResolvePush, intensity, "Intensity"),
        FieldRow{u8"ViewToWorld0", offsetof(render::GpuSsgiResolvePush, viewToWorld) + 0u},
        FieldRow{u8"ViewToWorld1", offsetof(render::GpuSsgiResolvePush, viewToWorld) + 16u},
        FieldRow{u8"ViewToWorld2", offsetof(render::GpuSsgiResolvePush, viewToWorld) + 32u},
        FIELD(render::GpuSsgiResolvePush, skyAmbient, "SkyAmbient"),
    };
    const FieldRow kSsrPush[] = {
        FIELD(render::GpuSsrPush, invProj, "InvProj"),
        FIELD(render::GpuSsrPush, vpMin, "VpMin"),
        FIELD(render::GpuSsrPush, vpSize, "VpSize"),
        FIELD(render::GpuSsrPush, jitter, "Jitter"),
        FIELD(render::GpuSsrPush, projXX, "ProjXX"),
        FIELD(render::GpuSsrPush, projYY, "ProjYY"),
        FIELD(render::GpuSsrPush, thickness, "Thickness"),
        FIELD(render::GpuSsrPush, intensity, "Intensity"),
        FIELD(render::GpuSsrPush, edgeFade, "EdgeFade"),
        FIELD(render::GpuSsrPush, roughCutoff, "RoughnessCutoff"),
        FIELD(render::GpuSsrPush, maxSteps, "MaxSteps"),
        FIELD(render::GpuSsrPush, ySign, "YSign"),
        FIELD(render::GpuSsrPush, debug, "Debug"),
        FIELD(render::GpuSsrPush, glossy, "Glossy"),
    };
    const FieldRow kSsrResolvePush[] = {
        FIELD(render::GpuSsrResolvePush, vpMin, "VpMin"),
        FIELD(render::GpuSsrResolvePush, vpSize, "VpSize"),
        FIELD(render::GpuSsrResolvePush, texelSize, "TexelSize"),
        FIELD(render::GpuSsrResolvePush, blendFactor, "BlendFactor"),
        FIELD(render::GpuSsrResolvePush, historyValid, "HistoryValid"),
        FIELD(render::GpuSsrResolvePush, varianceGamma, "VarianceGamma"),
        FIELD(render::GpuSsrResolvePush, motionScale, "MotionScale"),
        FIELD(render::GpuSsrResolvePush, temporalOn, "TemporalOn"),
        FIELD(render::GpuSsrResolvePush, debug, "Debug"),
        FIELD(render::GpuSsrResolvePush, ghostReject, "GhostReject"),
    };

    // ---- Resolve, anti-aliasing, exposure, debug views ----

    const FieldRow kTonemapPush[] = {
        FIELD(render::GpuTonemapPush, exposure, "Exposure"),
        FIELD(render::GpuTonemapPush, bloomIntensity, "BloomIntensity"),
        FIELD(render::GpuTonemapPush, uvScale, "UvScale"),
        FIELD(render::GpuTonemapPush, uvOffset, "UvOffset"),
        FIELD(render::GpuTonemapPush, aoStrength, "AoStrength"),
        FIELD(render::GpuTonemapPush, debugShowAo, "DebugShowAo"),
        FIELD(render::GpuTonemapPush, toneOperator, "Operator"),
        FIELD(render::GpuTonemapPush, flipSceneY, "FlipSceneY"),
        FIELD(render::GpuTonemapPush, autoExposure, "AutoExposure"),
        FIELD(render::GpuTonemapPush, autoKey, "AutoKey"),
        FIELD(render::GpuTonemapPush, autoMin, "AutoMin"),
        FIELD(render::GpuTonemapPush, autoMax, "AutoMax"),
        FIELD(render::GpuTonemapPush, gradeIntensity, "GradeIntensity"),
        FIELD(render::GpuTonemapPush, lutSize, "LutSize"),
        FIELD(render::GpuTonemapPush, encodeOutput, "EncodeOutput"),
        FIELD(render::GpuTonemapPush, rotation, "Rotation"),
        FIELD(render::GpuTonemapPush, pad1, "Pad1"),
        FIELD(render::GpuTonemapPush, pad2, "Pad2"),
    };
    const FieldRow kFxaaPush[] = {
        FIELD(render::GpuFxaaPush, texelSize, "TexelSize"),
        FIELD(render::GpuFxaaPush, uvScale, "UvScale"),
        FIELD(render::GpuFxaaPush, uvOffset, "UvOffset"),
        FIELD(render::GpuFxaaPush, subpixelQuality, "SubpixelQuality"),
        FIELD(render::GpuFxaaPush, edgeThreshold, "EdgeThreshold"),
        FIELD(render::GpuFxaaPush, edgeThresholdMin, "EdgeThresholdMin"),
        FIELD(render::GpuFxaaPush, linearInput, "LinearInput"),
    };
    const FieldRow kExposurePush[] = {
        FIELD(render::GpuExposurePush, uvScale, "UvScale"),
        FIELD(render::GpuExposurePush, uvOffset, "UvOffset"),
        FIELD(render::GpuExposurePush, dtSpeed, "DtSpeed"),
        FIELD(render::GpuExposurePush, historyValid, "HistoryValid"),
        FIELD(render::GpuExposurePush, pad0, "Pad0"),
        FIELD(render::GpuExposurePush, pad1, "Pad1"),
    };
    // The shader's float2s are the mirror's scalar pairs (uvScaleX and uvScaleY, ...).
    const FieldRow kDebugBlitPush[] = {
        FIELD(render::GpuDebugBlitPush, uvScaleX, "UvScale"),
        FIELD(render::GpuDebugBlitPush, uvOffsetX, "UvOffset"),
        FIELD(render::GpuDebugBlitPush, sourceW, "SourceSize"),
        FIELD(render::GpuDebugBlitPush, rangeMin, "RangeMin"),
        FIELD(render::GpuDebugBlitPush, rangeMax, "RangeMax"),
        FIELD(render::GpuDebugBlitPush, nearZ, "NearZ"),
        FIELD(render::GpuDebugBlitPush, farZ, "FarZ"),
        FIELD(render::GpuDebugBlitPush, mode, "Mode"),
        FIELD(render::GpuDebugBlitPush, pad, "_pad"),
    };
    const FieldRow kDebugGeomPush[] = {
        FIELD(render::GpuDebugGeomPush, viewProj, "ViewProj"),
    };
    const FieldRow kDebugGridPush[] = {
        FIELD(render::GpuDebugGridPush, viewProj, "ViewProj"),
        FIELD(render::GpuDebugGridPush, originExtent, "OriginExtent"),
        FIELD(render::GpuDebugGridPush, axisUSpacing, "AxisUSpacing"),
        FIELD(render::GpuDebugGridPush, axisVBlend, "AxisVBlend"),
        FIELD(render::GpuDebugGridPush, cameraFade, "CameraFade"),
    };
    const FieldRow kDebugScreenPush[] = {
        FIELD(render::GpuDebugScreenPush, invSize, "InvSize"),
        FIELD(render::GpuDebugScreenPush, pad, "_pad"),
    };

    // ---- Sprites, particles, terrain ----

    const FieldRow kSpriteView[] = {
        FIELD(render::GpuSpriteView, viewProj, "ViewProj"),
        FIELD(render::GpuSpriteView, view, "View"),
    };
    const FieldRow kParticleView[] = {
        FIELD(particles::GpuParticleView, viewProj, "ViewProj"),
        FIELD(particles::GpuParticleView, view, "View"),
        FIELD(particles::GpuParticleView, depthParams, "DepthParams"),
    };
    // TerrainView as terrain.ps.hlsl declares it; terrain.vs.hlsl declares its first twelve
    // members and terrain_depth.vs.hlsl its first two (the same rows, a prefix of this table).
    const FieldRow kTerrainView[] = {
        FIELD(terrain::GpuTerrainView, chunkToWorld, "ChunkToWorld"),
        FIELD(terrain::GpuTerrainView, viewProj, "ViewProj"),
        FIELD(terrain::GpuTerrainView, view, "View"),
        FIELD(terrain::GpuTerrainView, prevViewProj, "PrevViewProj"),
        FIELD(terrain::GpuTerrainView, cascadeViewProj, "CascadeViewProj"),
        FIELD(terrain::GpuTerrainView, lightDir, "LightDir"),
        FIELD(terrain::GpuTerrainView, cameraPos, "CameraPos"),
        FIELD(terrain::GpuTerrainView, jitter, "Jitter"),
        FIELD(terrain::GpuTerrainView, cascadeSplitFar, "CascadeSplitFar"),
        FIELD(terrain::GpuTerrainView, cascadeTexelSize, "CascadeTexelSize"),
        FIELD(terrain::GpuTerrainView, shadowMeta, "ShadowMeta"),
        FIELD(terrain::GpuTerrainView, shadowParams, "ShadowParams"),
        FIELD(terrain::GpuTerrainView, splatParams, "SplatParams"),
        FIELD(terrain::GpuTerrainView, splatParams2, "SplatParams2"),
        FIELD(terrain::GpuTerrainView, shadowLight, "ShadowLight"),
    };
    constexpr usize kTerrainVertexViewMembers = 12; // through ShadowParams (terrain.vs.hlsl)
    constexpr usize kTerrainDepthViewMembers = 2;   // ChunkToWorld, ViewProj (terrain_depth.vs.hlsl)
    const FieldRow kTerrainPickView[] = {
        FIELD(terrain::GpuTerrainPickView, chunkToWorld, "ChunkToWorld"),
        FIELD(terrain::GpuTerrainPickView, viewProj, "ViewProj"),
        FIELD(terrain::GpuTerrainPickView, pickIndex, "PickId"),
        FIELD(terrain::GpuTerrainPickView, p0, "_pvPad"),
    };
    const FieldRow kTerrainChunk[] = {
        FIELD(terrain::GpuTerrainChunk, originXZ, "OriginXZ"),
        FIELD(terrain::GpuTerrainChunk, sizeXZ, "SizeXZ"),
        FIELD(terrain::GpuTerrainChunk, texelBase, "TexelBase"),
        FIELD(terrain::GpuTerrainChunk, texelSpan, "TexelSpan"),
        FIELD(terrain::GpuTerrainChunk, heightRange, "HeightRange"),
        FIELD(terrain::GpuTerrainChunk, gridSize, "GridSize"),
        FIELD(terrain::GpuTerrainChunk, skirt, "Skirt"),
    };

    // ---- Vector graphics ----

    const FieldRow kVGUniforms[] = {
        FIELD(vg::renderer::VGUniforms, projection, "Projection"),
        FIELD(vg::renderer::VGUniforms, distanceFieldPixelRange, "DistanceFieldPixelRange"),
        FIELD(vg::renderer::VGUniforms, distanceFieldAtlasWidth, "DistanceFieldAtlasWidth"),
        FIELD(vg::renderer::VGUniforms, distanceFieldAtlasHeight, "DistanceFieldAtlasHeight"),
        FIELD(vg::renderer::VGUniforms, blendSpace, "BlendSpace"),
    };
    const FieldRow kVGBlur[] = {
        FIELD(vg::renderer::VGBlurUniforms, step, "Step"),
        FIELD(vg::renderer::VGBlurUniforms, region, "Region"),
        FIELD(vg::renderer::VGBlurUniforms, sigma, "Sigma"),
        FIELD(vg::renderer::VGBlurUniforms, radius, "Radius"),
        FIELD(vg::renderer::VGBlurUniforms, unused, "Unused"),
    };

#undef FIELD

    using shaders::ShaderFlags;
    using shaders::ShaderStage;
    const BlockRow kBlocks[] = {
        {u8"forward", ShaderStage::Fragment, ShaderFlags::None, u8"type.View", Span<const FieldRow>(kView)},
        {u8"forward", ShaderStage::Vertex, ShaderFlags::None, u8"type.Object", Span<const FieldRow>(kObject)},
        {u8"forward", ShaderStage::Vertex, ShaderFlags::Instanced, u8"InstanceData", Span<const FieldRow>(kInstance)},
        {u8"shadow_depth", ShaderStage::Vertex, ShaderFlags::None, u8"type.Object", Span<const FieldRow>(kObject)},
        {u8"shadow_depth", ShaderStage::Vertex, ShaderFlags::Instanced, u8"InstanceData", Span<const FieldRow>(kInstance)},
        {u8"shadow_depth", ShaderStage::Vertex, ShaderFlags::None, u8"type.ShadowView", Span<const FieldRow>(kShadowView)},
        {u8"pick_ids", ShaderStage::Vertex, ShaderFlags::None, u8"type.Object", Span<const FieldRow>(kObject)},
        {u8"pick_ids", ShaderStage::Vertex, ShaderFlags::Instanced, u8"InstanceData", Span<const FieldRow>(kInstance)},
        {u8"pick_ids", ShaderStage::Vertex, ShaderFlags::None, u8"type.PickView", Span<const FieldRow>(kPickView)},
        {u8"forward", ShaderStage::Vertex, ShaderFlags::Instanced, u8"type.InstanceSet", Span<const FieldRow>(kInstanceSet)},
        {u8"shadow_depth", ShaderStage::Vertex, ShaderFlags::Instanced, u8"type.InstanceSet", Span<const FieldRow>(kInstanceSet)},
        {u8"pick_ids", ShaderStage::Vertex, ShaderFlags::Instanced, u8"type.InstanceSet", Span<const FieldRow>(kInstanceSet)},
        {u8"forward", ShaderStage::Fragment, ShaderFlags::None, u8"GpuLight", Span<const FieldRow>(kGpuLight)},
        {u8"cluster_build", ShaderStage::Compute, ShaderFlags::None, u8"GpuLight", Span<const FieldRow>(kGpuLight)},
        {u8"forward", ShaderStage::Fragment, ShaderFlags::None, u8"GpuLocalShadow", Span<const FieldRow>(kGpuLocalShadow)},
        {u8"forward", ShaderStage::Fragment, ShaderFlags::None, u8"GpuProbe", Span<const FieldRow>(kGpuProbe)},
        {u8"decal", ShaderStage::Fragment, ShaderFlags::None, u8"type.DecalUniforms", Span<const FieldRow>(kDecalUniforms)},
        // Phong and Unlit share forward's vertex stage (forward_vs.hlsli) and its view (view.hlsli).
        {u8"phong", ShaderStage::Vertex, ShaderFlags::None, u8"type.Object", Span<const FieldRow>(kObject)},
        {u8"phong", ShaderStage::Fragment, ShaderFlags::None, u8"type.View", Span<const FieldRow>(kView)},
        {u8"unlit", ShaderStage::Vertex, ShaderFlags::None, u8"type.Object", Span<const FieldRow>(kObject)},
        {u8"unlit", ShaderStage::Fragment, ShaderFlags::GBuffer, u8"type.View", Span<const FieldRow>(kView)},
        {u8"sky", ShaderStage::Vertex, ShaderFlags::None, u8"type.Sky", Span<const FieldRow>(kSky)},
        {u8"sky", ShaderStage::Fragment, ShaderFlags::None, u8"type.Sky", Span<const FieldRow>(kSky)},
        {u8"ibl_procenv", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.IblPush", Span<const FieldRow>(kIblPush)},
        {u8"ibl_downsample", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.IblPush", Span<const FieldRow>(kIblPush)},
        {u8"ibl_prefilter", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.IblPush", Span<const FieldRow>(kIblPush)},
        {u8"probe_prefilter_ps", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.Push", Span<const FieldRow>(kPrefilterPush)},
        {u8"cluster_build", ShaderStage::Compute, ShaderFlags::None, u8"type.ClusterBuildParams", Span<const FieldRow>(kClusterBuildParams)},
        {u8"ao_gtao", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.GtaoPush", Span<const FieldRow>(kGtaoPush)},
        {u8"ao_ssao", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.SsaoPush", Span<const FieldRow>(kSsaoPush)},
        {u8"ao_blur", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.BlurPush", Span<const FieldRow>(kAoBlurPush)},
        {u8"ao_apply", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.ApplyPush", Span<const FieldRow>(kAoApplyPush)},
        {u8"bloom_ds", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.BloomPush", Span<const FieldRow>(kBloomPush)},
        {u8"bloom_us", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.BloomPush", Span<const FieldRow>(kBloomPush)},
        {u8"taa", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.TaaPush", Span<const FieldRow>(kTaaPush)},
        {u8"ssgi", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.SsgiPush", Span<const FieldRow>(kSsgiPush)},
        {u8"ssgi_down", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.SsgiDownPush", Span<const FieldRow>(kSsgiDownPush)},
        {u8"ssgi_blur", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.SsgiBlurPush", Span<const FieldRow>(kSsgiBlurPush)},
        {u8"ssgi_resolve", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.SsgiResolvePush", Span<const FieldRow>(kSsgiResolvePush)},
        {u8"ssr", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.SsrPush", Span<const FieldRow>(kSsrPush)},
        {u8"ssr_resolve", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.SsrResolvePush", Span<const FieldRow>(kSsrResolvePush)},
        {u8"tonemap", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.TonemapPush", Span<const FieldRow>(kTonemapPush)},
        {u8"fxaa", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.FxaaPush", Span<const FieldRow>(kFxaaPush)},
        {u8"exposure_measure", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.ExposurePush", Span<const FieldRow>(kExposurePush)},
        {u8"debug_blit", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.DebugBlitPush", Span<const FieldRow>(kDebugBlitPush)},
        {u8"debug_geom", ShaderStage::Vertex, ShaderFlags::None, u8"type.ConstantBuffer.GeomPush", Span<const FieldRow>(kDebugGeomPush)},
        {u8"debug_grid", ShaderStage::Vertex, ShaderFlags::None, u8"type.ConstantBuffer.GridPush", Span<const FieldRow>(kDebugGridPush)},
        {u8"debug_grid", ShaderStage::Fragment, ShaderFlags::None, u8"type.ConstantBuffer.GridPush", Span<const FieldRow>(kDebugGridPush)},
        {u8"debug_screen", ShaderStage::Vertex, ShaderFlags::None, u8"type.ConstantBuffer.ScreenPush", Span<const FieldRow>(kDebugScreenPush)},
        {u8"sprite", ShaderStage::Vertex, ShaderFlags::None, u8"type.SpriteView", Span<const FieldRow>(kSpriteView)},
        {u8"particle", ShaderStage::Vertex, ShaderFlags::None, u8"type.ParticleView", Span<const FieldRow>(kParticleView)},
        {u8"particle", ShaderStage::Fragment, ShaderFlags::None, u8"type.ParticleView", Span<const FieldRow>(kParticleView)},
        {u8"particletrail", ShaderStage::Vertex, ShaderFlags::None, u8"type.ParticleView", Span<const FieldRow>(kParticleView)},
        {u8"terrain", ShaderStage::Fragment, ShaderFlags::None, u8"type.TerrainView", Span<const FieldRow>(kTerrainView)},
        {u8"terrain", ShaderStage::Vertex, ShaderFlags::None, u8"type.TerrainView", Span<const FieldRow>(kTerrainView, kTerrainVertexViewMembers)},
        {u8"terrain_depth", ShaderStage::Vertex, ShaderFlags::None, u8"type.TerrainView", Span<const FieldRow>(kTerrainView, kTerrainDepthViewMembers)},
        {u8"terrain_pick", ShaderStage::Vertex, ShaderFlags::None, u8"type.TerrainView", Span<const FieldRow>(kTerrainPickView)},
        {u8"terrain", ShaderStage::Vertex, ShaderFlags::None, u8"type.TerrainChunk", Span<const FieldRow>(kTerrainChunk)},
        {u8"terrain_depth", ShaderStage::Vertex, ShaderFlags::None, u8"type.TerrainChunk", Span<const FieldRow>(kTerrainChunk)},
        {u8"terrain_pick", ShaderStage::Vertex, ShaderFlags::None, u8"type.TerrainChunk", Span<const FieldRow>(kTerrainChunk)},
        {u8"vg", ShaderStage::Vertex, ShaderFlags::None, u8"type.VGUniforms", Span<const FieldRow>(kVGUniforms)},
        {u8"vg_blur", ShaderStage::Fragment, ShaderFlags::None, u8"type.VGBlur", Span<const FieldRow>(kVGBlur)},
    };

    // The engine's shaders cooked once to SPIR-V, as the engine cook does.
    struct CookedCorpus
    {
        shaders::Compiler* compiler = nullptr;
        shaders::CookedShaderPack pack;
        bool ok = false;

        CookedCorpus()
        {
            if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
            {
                return;
            }
            const String dataRoot = foundation::vfs::FindDataRoot();
            if (dataRoot.IsEmpty())
            {
                return;
            }
            const String shaderDir = foundation::vfs::DataPath(dataRoot.AsView(), u8"Engine/Shaders");
            const shaders::CookedShaderFormat formats[] = {shaders::CookedShaderFormat::SpirV};
            shaders::ShaderCookOptions opts;
            opts.shaderDir = shaderDir.AsView();
            opts.scratchDir = u8".test-scratch";
            opts.formats = Span<const shaders::CookedShaderFormat>(formats, 1);
            ok = shaders::CookEngineShaders(*compiler, opts, pack).success;
        }
        ~CookedCorpus()
        {
            if (compiler != nullptr)
            {
                compiler->Destroy();
            }
        }
        CookedCorpus(const CookedCorpus&) = delete;
        CookedCorpus& operator=(const CookedCorpus&) = delete;
    };
}

TEST_CASE("shader blocks: every engine block matches its C++ mirror, member by member")
{
    CookedCorpus corpus;
    if (corpus.compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping the shader block layouts");
        return;
    }
    REQUIRE(corpus.ok);

    for (const BlockRow& block : kBlocks)
    {
        CAPTURE(reinterpret_cast<const char*>(block.shader));
        CAPTURE(reinterpret_cast<const char*>(block.spirvName));
        const Array<byte>* blob = corpus.pack.Find(StringView(block.shader), block.stage, block.flags,
                                                   shaders::CookedShaderFormat::SpirV);
        REQUIRE(blob != nullptr);
        const SpirvBlocks module(*blob);
        const Array<SpirvMember>* members = module.Find(StringView(block.spirvName));
        REQUIRE(members != nullptr);
        CHECK(members->Size() == block.fields.Size()); // no member on one side only
        for (usize i = 0; i < block.fields.Size() && i < members->Size(); ++i)
        {
            const FieldRow& field = block.fields[i];
            const SpirvMember& member = (*members)[i];
            CAPTURE(reinterpret_cast<const char*>(field.name));
            CHECK(member.name.AsView() == StringView(field.name));
            REQUIRE(member.hasOffset);
            CHECK(member.offset == field.offset);
        }
    }
}

TEST_CASE("shader blocks: the built-in materials' buffers match their MaterialBuilder layouts")
{
    // A built-in material's cbuffer Material has no C++ struct: MaterialBuilder lays its uniforms out
    // by HLSL packing (CreatePBR, CreatePhong, CreateUnlit). Each shader member must be a uniform
    // property of the same name at the same offset, and each uniform property a shader member.
    CookedCorpus corpus;
    if (corpus.compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping the material buffer layouts");
        return;
    }
    REQUIRE(corpus.ok);

    struct MaterialRow
    {
        const char8_t* shader;
        shaders::ShaderStage stage;
        shaders::ShaderFlags flags;
        RefPtr<materials::Material> material;
    };
    // The vertex stages declare no material buffer: the wind reads its own block (the next test).
    const MaterialRow rows[] = {
        {u8"forward", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None, materials::CreatePBR(u8"pbr")},
        {u8"phong", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None, materials::CreatePhong(u8"phong")},
        {u8"unlit", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None, materials::CreateUnlit(u8"unlit")},
    };
    for (const MaterialRow& row : rows)
    {
        CAPTURE(reinterpret_cast<const char*>(row.shader));
        CAPTURE(static_cast<u32>(row.stage));
        REQUIRE(row.material.Get() != nullptr);
        const Array<byte>* blob = corpus.pack.Find(StringView(row.shader), row.stage, row.flags,
                                                   shaders::CookedShaderFormat::SpirV);
        REQUIRE(blob != nullptr);
        const SpirvBlocks module(*blob);
        const Array<SpirvMember>* members = module.Find(u8"type.Material");
        REQUIRE(members != nullptr);

        usize uniformCount = 0;
        for (const materials::MaterialPropertyDef& property : row.material->Properties())
        {
            if (property.IsUniform())
            {
                ++uniformCount;
            }
        }
        // Padding is padding (shader-blocks-cleanup.md, rule 2): a member named `_...` holds a
        // place (PBR's former wind lanes) and is no property.
        usize memberCount = 0;
        for (const SpirvMember& member : *members)
        {
            if (!member.name.AsView().StartsWith(u8"_"))
            {
                ++memberCount;
            }
        }
        CHECK(memberCount == uniformCount); // no uniform on one side only
        for (const SpirvMember& member : *members)
        {
            if (member.name.AsView().StartsWith(u8"_"))
            {
                continue;
            }
            CAPTURE(reinterpret_cast<const char*>(member.name.CStr()));
            const materials::MaterialPropertyDef* property = row.material->FindProperty(member.name.AsView());
            REQUIRE(property != nullptr);
            CHECK(property->IsUniform());
            REQUIRE(member.hasOffset);
            CHECK(property->offset == member.offset);
        }
    }
}

TEST_CASE("shader blocks: every WIND vertex stage's Wind block matches the effect's table")
{
    // The effect block in wind.hlsli is the one declaration of the wind's parameters: the cook
    // records its table in the pack, and the material system fills the buffer by that table's
    // offsets. Every stage that sways must read the same layout.
    CookedCorpus corpus;
    if (corpus.compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping the effect block layouts");
        return;
    }
    REQUIRE(corpus.ok);
    const shaders::ShaderEffect* wind = corpus.pack.Effects().ForFlag(shaders::ShaderFlags::Wind);
    REQUIRE(wind != nullptr);
    CHECK(wind->name.AsView() == StringView(u8"Wind"));
    REQUIRE(wind->Gate() != nullptr);
    CHECK(wind->Gate()->name.AsView() == StringView(u8"Strength"));

    String spirvName(u8"type.ConstantBuffer.");
    spirvName += shaders::EffectStructName(wind->name.AsView());
    const char8_t* const stages[] = {u8"forward", u8"phong", u8"unlit", u8"shadow_depth", u8"pick_ids"};
    for (const char8_t* shader : stages)
    {
        CAPTURE(reinterpret_cast<const char*>(shader));
        const Array<byte>* blob = corpus.pack.Find(StringView(shader), shaders::ShaderStage::Vertex,
                                                   shaders::ShaderFlags::Wind,
                                                   shaders::CookedShaderFormat::SpirV);
        REQUIRE(blob != nullptr);
        const SpirvBlocks module(*blob);
        const Array<SpirvMember>* members = module.Find(spirvName.AsView());
        REQUIRE(members != nullptr);
        CHECK(members->Size() == wind->properties.Size());
        for (usize i = 0; i < members->Size() && i < wind->properties.Size(); ++i)
        {
            const SpirvMember& member = (*members)[i];
            CHECK(member.name.AsView() == wind->properties[i].name.AsView());
            REQUIRE(member.hasOffset);
            CHECK(member.offset == wind->properties[i].offset);
        }
        // Not the material's buffer: no vertex stage declares one for the wind any more.
        CHECK(module.Find(u8"type.Material") == nullptr);
    }

    // What each built-in's vertex stage declares, as the C++ table states it (the effects a
    // material on it may carry where no shader system is at hand): its `// variants` line, as the
    // cook read it.
    for (const materials::BuiltinShadingInfo& builtin : materials::kBuiltinShadings)
    {
        CAPTURE(reinterpret_cast<const char*>(builtin.shaderName.Data()));
        CHECK(corpus.pack.DeclaredMask(shaders::ShaderNameHash(builtin.shaderName), shaders::ShaderStage::Vertex) ==
              builtin.vertexVariants);
    }
}

TEST_CASE("shader blocks: the reader finds a block's members and offsets in compiled SPIR-V")
{
    shaders::Compiler* compiler = nullptr;
    if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
    {
        MESSAGE("DXC runtime unavailable; skipping");
        return;
    }
    const char source[] = "cbuffer Probe : register(b0) { float4 A; float B; float C; float4 D; };\n"
                          "float4 main() : SV_Target0 { return A * B + D * C; }\n";
    shaders::CompileResult result{};
    REQUIRE(compiler
                ->compile(reinterpret_cast<const u8*>(source), sizeof(source) - 1u, shaders::ShaderStage::Fragment,
                          u8"main", shaders::ShaderTarget::SPIRV, shaders::CompileOptions{}, result)
                .IsOk());
    Array<byte> blob;
    blob.Resize(result.bytecodeSize);
    std::memcpy(blob.Data(), result.bytecode, result.bytecodeSize);
    compiler->freeResult(result);
    compiler->Destroy();

    const SpirvBlocks module(blob);
    const Array<SpirvMember>* members = module.Find(u8"type.Probe");
    REQUIRE(members != nullptr);
    REQUIRE(members->Size() == 4u);
    CHECK((*members)[0].name.AsView() == StringView(u8"A"));
    CHECK((*members)[0].offset == 0u);
    CHECK((*members)[1].name.AsView() == StringView(u8"B"));
    CHECK((*members)[1].offset == 16u);
    CHECK((*members)[2].offset == 20u);
    CHECK((*members)[3].name.AsView() == StringView(u8"D"));
    CHECK((*members)[3].offset == 32u); // HLSL packing: a float4 does not straddle a 16-byte row
    CHECK(module.Find(u8"type.Missing") == nullptr);
}
