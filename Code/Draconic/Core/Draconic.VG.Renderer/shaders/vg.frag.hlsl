// Vector Graphics Fragment Shader (reference copy; the compiled source is Data/Shaders/vg.ps.hlsl).
// Multiplies vertex color by coverage and outputs premultiplied-alpha color (pairs with the
// renderer's PremultipliedAlpha blend).

struct PSInput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
    float4 Color : COLOR0;
    float Coverage : COVERAGE;
};

Texture2D VGTexture : register(t0);
SamplerState VGSampler : register(s0);

float4 main(PSInput input) : SV_Target
{
    float4 texColor = VGTexture.Sample(VGSampler, input.TexCoord);
    float4 result = texColor * input.Color;
    result.a *= input.Coverage;
    result.rgb *= result.a; // premultiply
    return result;
}
