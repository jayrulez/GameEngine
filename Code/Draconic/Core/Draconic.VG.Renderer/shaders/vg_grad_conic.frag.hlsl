// VG conic-gradient fragment shader (reference copy; the compiled source is
// Data/Shaders/vg_grad_conic.ps.hlsl). The tessellator emits (pos-center) rotated by -startAngle as
// TexCoord, so t = frac(atan2(y,x)/2pi) is computed per pixel (exact angular sweep), sampled from
// the baked ramp LUT at texel centers. Outputs premultiplied-alpha color (pairs with the blend).
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
    const float kInvTwoPi = 0.15915494309; // 1/(2pi)
    float a = atan2(input.TexCoord.y, input.TexCoord.x) * kInvTwoPi; // (-0.5, 0.5]
    float t = a - floor(a);                                          // [0, 1), 0 at +x axis
    float u = (0.5 + t * 255.0) / 256.0; // texel center: pads + dodges the Repeat wrap seam
    float4 ramp = VGTexture.Sample(VGSampler, float2(u, 0.5));
    float4 result = ramp * input.Color;
    result.a *= input.Coverage;
    result.rgb *= result.a; // premultiplied output
    return result;
}
