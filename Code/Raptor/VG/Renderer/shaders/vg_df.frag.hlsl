// Vector Graphics Distance Field Fragment Shader
// Decodes multi-channel signed distance field atlas data into sharp alpha.

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
    float4 msd = VGTexture.Sample(VGSampler, input.TexCoord);
    float sd = Median(msd.r, msd.g, msd.b);
    float opacity = clamp(4.0 * (sd - 0.5) + 0.5, 0.0, 1.0);

    float4 result = input.Color;
    result.a *= opacity;
    return result;
}
