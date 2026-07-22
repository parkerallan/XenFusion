// video.hlsl — the Video attribute's screen-space overlay quad. Samples the
// VideoPlayer's three L8 YUV planes (Y full res, Cb/Cr half) and converts to
// RGB here, so the CPU never touches pixels beyond the plane memcpy. Drawn
// over the finished scene with straight alpha blending (host sets the states);
// the host masks alpha writes so the target's alpha (editor viewport opacity /
// console bloom mask) is untouched.
//
//   VS c0 = (1/destW, 1/destH) — D3D9 half-pixel offset (bloom convention)
//   PS c0 = tint.rgb, alpha in w
//   s0/s1/s2 = Y / Cb / Cr planes (quad uv already spans the padded planes'
//              display region, computed by the host)

float4 gHalfTexel : register(c0);

struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = float4(i.pos.x - gHalfTexel.x, i.pos.y + gHalfTexel.y, 0.0, 1.0);
    o.uv  = i.uv;
    return o;
}

float4    gTint : register(c0);
sampler2D sY    : register(s0);
sampler2D sCb   : register(s1);
sampler2D sCr   : register(s2);

float4 PSMain(VSOut i) : COLOR
{
    // Limited-range BT.601, matching pl_mpeg's own YCbCr->RGB constants.
    float y  = (tex2D(sY,  i.uv).r - 16.0 / 255.0) * 1.164;
    float cb = tex2D(sCb, i.uv).r - 0.5;
    float cr = tex2D(sCr, i.uv).r - 0.5;
    float3 rgb;
    rgb.r = y + 1.596 * cr;
    rgb.g = y - 0.813 * cr - 0.391 * cb;
    rgb.b = y + 2.018 * cb;
    return float4(saturate(rgb) * gTint.rgb, gTint.a);
}
