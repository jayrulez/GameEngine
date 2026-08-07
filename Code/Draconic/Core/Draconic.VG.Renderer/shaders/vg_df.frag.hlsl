// Vector Graphics Distance Field Fragment Shader
// Decodes a multi-channel signed distance field (MSDF) atlas into a crisp, screen-space
// antialiased alpha, then outputs premultiplied-alpha color. Reference copy - the compiled source
// consumers actually use is Data/Shaders/vg_df.ps.hlsl.

cbuffer VGUniforms : register(b0)
{
    float4x4 Projection;
    float DFPxRange;
    float DFAtlasW;
    float DFAtlasH;
    float _pad;
};

struct PSInput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
    float4 Color : COLOR0;
    float Coverage : COVERAGE;
};

Texture2D VGTexture : register(t0);
SamplerState VGSampler : register(s0);

float Median(float r, float g, float b)
{
    return max(min(r, g), min(max(r, g), b));
}

float4 main(PSInput input) : SV_Target
{
    float3 msd = VGTexture.Sample(VGSampler, input.TexCoord).rgb;
    float sd = Median(msd.r, msd.g, msd.b);

    // Convert the atlas-space DF spread to screen pixels via the texcoord derivatives, so the
    // antialiased edge stays ~1px wide at any magnification.
    float2 unitRange = float2(DFPxRange, DFPxRange) / float2(DFAtlasW, DFAtlasH);
    float2 screenTexSize = float2(1.0, 1.0) / max(fwidth(input.TexCoord), float2(1e-6, 1e-6));
    float screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);

    float opacity = clamp(screenPxRange * (sd - 0.5) + 0.5, 0.0, 1.0);

    float4 result = input.Color;
    result.a *= opacity * input.Coverage;
    result.rgb *= result.a; // premultiplied output
    return result;
}
