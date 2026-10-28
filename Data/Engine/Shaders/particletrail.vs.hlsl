// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#pragma pack_matrix(row_major)
cbuffer ParticleView : register(b0, space0) { float4x4 ViewProj; float4x4 View; float4 DepthParams; };
// Vertex inputs are TEXCOORD<location>: the DX12 input layout names every element TEXCOORD with
// the shader location as its index (DxRenderPipeline), so POSITION/COLOR semantics fail layout
// creation there and the trail draw silently dropped. Locations 0/1/2 are unchanged.
struct VSIn { float3 Position : TEXCOORD0; float2 UV : TEXCOORD1; float4 Color : TEXCOORD2; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; };
VSOut main(VSIn i) { VSOut o; o.pos = mul(float4(i.Position, 1.0), ViewProj); o.uv = i.UV; o.col = i.Color; return o; }
