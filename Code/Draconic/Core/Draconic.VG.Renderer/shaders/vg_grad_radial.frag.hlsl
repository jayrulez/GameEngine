// VG radial-gradient fragment shader (reference copy; the compiled source is
// Data/Shaders/vg_grad_radial.ps.hlsl). The tessellator emits (pos-center)/radius as TexCoord, so
// t = length(TexCoord) is computed per pixel (exact radial falloff), sampled from the baked ramp
// LUT at texel centers. Outputs premultiplied-alpha color (pairs with PremultipliedAlpha blend).
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
    float t = saturate(length(input.TexCoord));
    float u = (0.5 + t * 255.0) / 256.0; // texel center: pads + dodges the Repeat wrap seam
    float4 ramp = VGTexture.Sample(VGSampler, float2(u, 0.5));
    float4 result = ramp * input.Color;
    result.a *= input.Coverage;
    result.rgb *= result.a; // premultiplied output
    return result;
}
