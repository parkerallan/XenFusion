// bloom_bright.hlsl — glow-source extraction (pass 1 of the bloom chain).
// The scene pass stores each pixel's emissive strength in the render target's
// ALPHA channel (standard.hlsl, gGlowMask); this pass downsamples rgb * a into
// a quarter-res target: only self-lit pixels survive, everything else is black.
// Shared by the editor (d3dcompiler/fxc) and the 360 (XDK fxc), like standard.hlsl.
//
//   VS c0 = (1/destW, 1/destH) — D3D9 half-pixel offset
//   s0    = source (the resolved scene, alpha = glow mask)

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

sampler2D sSrc : register(s0);

float4 PSMain(VSOut i) : COLOR
{
    float4 c = tex2D(sSrc, i.uv);
    return float4(c.rgb * c.a, 1.0);
}
