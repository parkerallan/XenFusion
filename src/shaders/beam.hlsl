// beam.hlsl — the spot light's volumetric shaft ("Project Light Shaft"): a
// 360-era stand-in for the Vulkan engine's ray-marched fog cone. The renderers
// draw a unit cone mesh (apex at the light, opening down +Z, scaled to
// tan(outer) * range by the world matrix) with ADDITIVE blending, depth test
// on / depth write off, both faces. Per-pixel the shaft is shaped by three
// fades so the polygonal cone reads as soft light in the air:
//   (1 - t)^2        — dies toward the far end, matching the spot's own
//                      (1 - d/range)^2 distance falloff
//   saturate(t * 6)  — eases in near the apex, where every triangle of the
//                      fan overlaps and additive blending would flare white
//   (n.v)^2          — fades the silhouette rim (and the view from inside),
//                      hiding the mesh edge; brightest looking through the
//                      cone's core
// Color arrives premultiplied: spot rgb * intensity * beam intensity * gain
// (gain lives in the renderers — kBeamGain, tuned by eye, keep in sync).
// Alpha out is 0 so ONE+ONE blending leaves the target's glow mask (the bloom
// source, standard.hlsl c15) untouched.
//
// Registers (same conventions as standard.hlsl):
//   VS c0 = gWVP   c4 = gWorld
//   PS c0 = beam color   c1 = camera pos   c2 = apex (world)
//      c3 = beam axis (xyz = unit dir, w = tan(outer half-angle))

float4x4 gWVP   : register(c0);
float4x4 gWorld : register(c4);

struct VSIn  { float3 pos:POSITION; float3 nrm:NORMAL; float3 tan:TANGENT; float2 uv:TEXCOORD0; };
struct VSOut { float4 pos:POSITION; float2 uv:TEXCOORD0; float3 wpos:TEXCOORD1; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos  = mul(float4(i.pos, 1.0), gWVP);
    o.wpos = mul(float4(i.pos, 1.0), gWorld).xyz;
    o.uv   = i.uv; // uv.x = t along the beam: 0 = apex, 1 = far end
    return o;
}

float3 gBeamColor : register(c0);
float3 gCameraPos : register(c1);
float3 gBeamApex  : register(c2);
float4 gBeamAxis  : register(c3); // xyz = unit beam direction, w = tan(outer)

float4 PSMain(VSOut i) : COLOR
{
    // Analytic cone normal, rebuilt per-pixel from the apex + axis: a vertex-
    // normal fan can NOT interpolate continuously across its shared apex, so
    // every segment edge showed as a hard line down the shaft. For a cone of
    // half-angle a, the outward normal at a surface point P is
    // normalize(r_hat - tan(a) * axis), with r_hat the radial direction of P
    // around the axis. The max() guards the apex, where the radial component
    // vanishes (the t-fade hides that region anyway). World-space throughout,
    // so the world matrix's non-uniform (r, r, range) scale can't skew it.
    float3 rel   = i.wpos - gBeamApex;
    float3 rad   = rel - dot(rel, gBeamAxis.xyz) * gBeamAxis.xyz;
    float3 r_hat = rad * rsqrt(max(dot(rad, rad), 1e-6));
    float3 N     = normalize(r_hat - gBeamAxis.w * gBeamAxis.xyz);
    float3 V     = normalize(gCameraPos - i.wpos);
    float  ndv   = abs(dot(N, V)); // double-sided: both faces shade alike
    float  t     = i.uv.x;
    float  fade  = (1.0 - t) * (1.0 - t) * saturate(t * 6.0) * (ndv * ndv);
    return float4(gBeamColor * fade, 0.0);
}
