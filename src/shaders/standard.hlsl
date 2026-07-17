// standard.hlsl — the built-in engine material: per-pixel diffuse / normal /
// specular lighting. (Custom project shaders live under the project's assets.)
// Shader Model 3.0 — the same source targets the Xbox 360's Xenos GPU (compiled
// offline by the XDK there; by fxc at build time / d3dcompiler at runtime here).
//
// Register layout (kept in sync with SceneRenderer.cpp / SceneRuntime.cpp):
//   VS  c0 = gWVP (world*view*proj)   c4 = gWorld
//   PS  c0 = light dir   c1 = camera pos   c2 = ambient   c5 = bump scale
//       c6 = light color (directional rgb * intensity)
//       c7-c10 = point light pos + 1/range^2   c11-c14 = point light rgb * intensity
//       s0 = diffuse   s1 = normal (alpha = height field)   s2 = specular
// (PS c3/c4 stay free: the custom-shader convention claims them for gTime/gCamObj.)

float4x4 gWVP   : register(c0);
float4x4 gWorld : register(c4);

struct VSIn  { float3 pos:POSITION; float3 nrm:NORMAL; float3 tan:TANGENT; float2 uv:TEXCOORD0; };
struct VSOut { float4 pos:POSITION; float2 uv:TEXCOORD0; float3 wpos:TEXCOORD1; float3 wn:TEXCOORD2; float3 wt:TEXCOORD3; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos  = mul(float4(i.pos, 1.0), gWVP);
    o.wpos = mul(float4(i.pos, 1.0), gWorld).xyz;
    o.wn   = mul(i.nrm, (float3x3)gWorld);
    o.wt   = mul(i.tan, (float3x3)gWorld);
    o.uv   = i.uv;
    return o;
}

float3 gLightDir  : register(c0);
float3 gCameraPos : register(c1);
float3 gAmbient   : register(c2);
float  gBumpScale : register(c5); // max UV offset over the full height range; 0 = no height field
float3 gLightColor : register(c6);  // directional rgb * intensity (white = legacy sun)
float4 gPointPos[4] : register(c7); // xyz = world position, w = 1 / range^2
float3 gPointCol[4] : register(c11);// rgb * intensity; zero = unused slot
sampler2D sDiffuse  : register(s0);
sampler2D sNormal   : register(s1);
sampler2D sSpecular : register(s2);

float4 PSMain(VSOut i) : COLOR
{
    float3 N = normalize(i.wn);
    float3 T = normalize(i.wt);
    float3 B = cross(N, T);
    float3 V = normalize(gCameraPos - i.wpos);

    // Bump offset: the normal map's alpha is a height field (0.5 = neutral, the
    // Substance export convention: white = raised, black = recessed). A single
    // tap shifts the UVs along the tangent-space view ray — no ray march, so it
    // cannot band or staircase. The renderers upload gBumpScale = 0 for subsets
    // without a height field, making the offset exactly (0,0) — the cutout
    // alpha then samples the same texel it always did. The saturate() fades the
    // effect out below ~15° viewing angles — a single tap has no occlusion
    // information, so at grazing incidence it can only smear texels — and takes
    // it to zero on back faces (culling is off, and Assimp's left-handed import
    // flips authored quad normals, so back-face views are common).
    // Two taps: the first offset is corrected by re-reading the height at the
    // offset position (averaged to damp overshoot) — this keeps steep height
    // edges from striping/smearing, where a lone tap pairs a pixel with a
    // wildly wrong texel. Still fixed cost: no loop, no divergent flow.
    float3 vts  = float3(dot(V, T), dot(V, B), dot(V, N));
    float2 vuv  = gBumpScale * saturate(vts.z * 4.0) * vts.xy / (abs(vts.z) + 0.42);
    float  hgt  = tex2D(sNormal, i.uv).a;
    float  hgt2 = tex2D(sNormal, i.uv + (hgt - 0.5) * vuv).a;
    float2 uv   = i.uv + ((hgt + hgt2) * 0.5 - 0.5) * vuv;

    float3 nt = tex2D(sNormal, uv).xyz * 2.0 - 1.0;   // tangent-space normal
    float3 n  = normalize(nt.x * T + nt.y * B + nt.z * N);

    float3 L   = -normalize(gLightDir);
    float  ndl = saturate(dot(n, L));
    float3 H   = normalize(L + V);
    float  ndh = saturate(dot(n, H));

    float4 dtex = tex2D(sDiffuse, uv);
    float3 spec = tex2D(sSpecular, uv).rgb;

    // Directional light (diffuse + specular) plus up to four point lights
    // (diffuse only — the 360-era forward trade-off). Point falloff fades
    // smoothly to zero at range: (1 - (d/range)^2)^2, with w = 1/range^2.
    // The max() keeps rsqrt finite at the light's own position; unused slots
    // upload zero color and contribute nothing.
    float3 diffuse = gLightColor * ndl;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        float3 pl  = gPointPos[k].xyz - i.wpos;
        float  d2  = max(dot(pl, pl), 1e-4);
        float  att = saturate(1.0 - d2 * gPointPos[k].w);
        diffuse += gPointCol[k] * (saturate(dot(n, pl * rsqrt(d2))) * (att * att));
    }
    float3 color = dtex.rgb * (gAmbient + diffuse) + spec * (gLightColor * pow(ndh, 40.0));
    return float4(color, dtex.a); // transparency from the diffuse's alpha
}
