// bloom_combine.hlsl — textured copy with a gain (final pass of the bloom chain).
// Drawn additively (ONE/ONE, set by the host) to lay the blurred glow over the
// scene; the 360 also uses it with blending off as the plain full-screen copy
// when compositing into its post target. Alpha is forced to 1 so the editor's
// ImGui viewport (which samples the target's alpha) stays opaque.
//
//   VS c0 = (1/destW, 1/destH) — D3D9 half-pixel offset
//   PS c0.x = gain (glow intensity on the additive pass, 1 on the copy pass)
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

float4    gGain : register(c0);
sampler2D sSrc  : register(s0);

float4 PSMain(VSOut i) : COLOR
{
    return float4(tex2D(sSrc, i.uv).rgb * gGain.x, 1.0);
}
