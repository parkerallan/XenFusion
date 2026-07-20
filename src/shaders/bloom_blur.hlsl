// bloom_blur.hlsl — 9-tap separable Gaussian (passes 2+3 of the bloom chain).
// Run twice over the quarter-res glow source: once with a horizontal step, once
// vertical. The wide soft halo is what reads as "glow" (streetlamp corona).
//
//   VS c0 = (1/destW, 1/destH) — D3D9 half-pixel offset
//   PS c0 = uv advance per tap (blur direction * texel size * spacing)
//   s0    = source

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

float4    gStep : register(c0);
sampler2D sSrc  : register(s0);

static const float kWeight[9] =
    { 0.05, 0.09, 0.12, 0.15, 0.18, 0.15, 0.12, 0.09, 0.05 };

float4 PSMain(VSOut i) : COLOR
{
    float3 acc = 0;
    [unroll] for (int t = 0; t < 9; ++t)
        acc += kWeight[t] * tex2D(sSrc, i.uv + gStep.xy * (float)(t - 4)).rgb;
    return float4(acc, 1.0);
}
