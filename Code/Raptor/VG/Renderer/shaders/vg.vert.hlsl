// Vector Graphics Vertex Shader
#pragma pack_matrix(row_major)

cbuffer VGUniforms : register(b0)
{
    float4x4 Projection;
    // Distance-field metadata (used by vg_df.frag.hlsl; ignored by vg.frag.hlsl).
    float DFPxRange;
    float DFAtlasW;
    float DFAtlasH;
    float _pad;
};

struct VSInput
{
    float2 Position : TEXCOORD0;
    float2 TexCoord : TEXCOORD1;
    float4 Color : TEXCOORD2;
    float Coverage : TEXCOORD3;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
    float4 Color : COLOR0;
    float Coverage : COVERAGE;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.Position = mul(float4(input.Position, 0.0, 1.0), Projection);
    output.TexCoord = input.TexCoord;
    output.Color = input.Color;
    output.Coverage = input.Coverage;
    return output;
}
