// standard.hlsl — the built-in engine material: per-pixel diffuse / normal /
// specular lighting. (Custom project shaders live under the project's assets.)
// Shader Model 3.0 — the same source targets the Xbox 360's Xenos GPU (compiled
// offline by the XDK there; by fxc at build time / d3dcompiler at runtime here).
//
// Register layout (kept in sync with SceneRenderer.cpp / SceneRuntime.cpp):
//   VS  c0 = gWVP (world*view*proj)   c4 = gWorld
//   PS  c0 = light dir   c1 = camera pos   c2 = ambient ("Environment Light"
//       sum, or the fixed legacy default)   c5 = bump scale
//       c6 = light color (directional rgb * intensity)
//       c7-c10 = point light pos + 1/range^2   c11-c14 = point light rgb * intensity
//       c16-c17 = spot light pos + 1/range   c18-c19 = spot dir + cos(inner)
//       c20-c21 = spot rgb * intensity + cos(outer)
//       s0 = diffuse   s1 = normal (alpha = height field)   s2 = specular
//       s3 = emissive (rgb added after lighting; black = none)
//       s4 = metallic (r: 0 = dielectric, 1 = metal)   s5 = environment cube
//            (metal reflects the env tinted by the diffuse; black cube = none)
//       s6 = clearcoat (r: clear-lacquer amount — untinted env sheen ADDED over
//            the lit surface; the paint keeps its color, car-paint style)
//       c15 = glow-mask switch: 1 = output alpha carries the emissive strength
//             (the bloom chain's glow source — opaque subsets only), 0 = output
//             alpha is the diffuse's (cutout / blend passes need it)
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
float  gGlowMask   : register(c15); // 1 = alpha out = emissive strength (bloom source)
float4 gSpotPos[2] : register(c16); // xyz = world position, w = 1 / range
float4 gSpotDir[2] : register(c18); // xyz = beam direction (+Z fwd), w = cos(inner half-angle)
float4 gSpotCol[2] : register(c20); // rgb * intensity (zero = unused), w = cos(outer half-angle)
sampler2D sDiffuse  : register(s0);
sampler2D sNormal   : register(s1);
sampler2D sSpecular : register(s2);
sampler2D sEmissive : register(s3);
sampler2D sMetallic : register(s4);
samplerCUBE sEnv    : register(s5);
sampler2D sClearcoat : register(s6);

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
    // pglint: per-point-light specular, used by the METAL term only (dielectric
    // point lighting stays diffuse-only — the original trade-off). Same falloff
    // as the diffuse so a light's glint dies with its light.
    float3 diffuse = gLightColor * ndl;
    float3 pglint  = 0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        float3 pl  = gPointPos[k].xyz - i.wpos;
        float  d2  = max(dot(pl, pl), 1e-4);
        float  att = saturate(1.0 - d2 * gPointPos[k].w);
        float3 Lp  = pl * rsqrt(d2);
        diffuse += gPointCol[k] * (saturate(dot(n, Lp)) * (att * att));
        float3 Hp  = normalize(Lp + V);
        pglint += gPointCol[k] * (pow(saturate(dot(n, Hp)), 96.0) * (att * att));
    }
    // Up to two spot lights, treated like the points (diffuse here, glint into
    // the metal/clearcoat terms) with the Vulkan editor's spot falloff pair:
    // (1 - d/range)^2 distance fade x a smoothstep-shaped cone ramp between the
    // outer and inner half-angle cosines. Hand-rolled smoothstep with a max()ed
    // divisor: if the cone edges ever collapse (all-zero constants under an
    // exe that predates spot uploads), smoothstep(0,0,x) would go NaN and eat
    // the whole diffuse term — this form just degrades to a hard cone edge.
    [unroll] for (int j = 0; j < 2; ++j)
    {
        float3 sl   = gSpotPos[j].xyz - i.wpos;
        float  sd2  = max(dot(sl, sl), 1e-4);
        float  sinv = rsqrt(sd2);
        float3 Ls   = sl * sinv;
        float  satt = saturate(1.0 - sd2 * sinv * gSpotPos[j].w); // 1 - d/range
        float  st   = saturate((dot(-gSpotDir[j].xyz, Ls) - gSpotCol[j].w)
                               / max(gSpotDir[j].w - gSpotCol[j].w, 1e-4));
        satt = satt * satt * (st * st * (3.0 - 2.0 * st));
        diffuse += gSpotCol[j].rgb * (saturate(dot(n, Ls)) * satt);
        float3 Hs = normalize(Ls + V);
        pglint += gSpotCol[j].rgb * (pow(saturate(dot(n, Hs)), 96.0) * satt);
    }
    float3 color = dtex.rgb * (gAmbient + diffuse) + spec * (gLightColor * pow(ndh, 40.0));

    // Metal (360-era env-map look): the surface IS its environment — no flat
    // shading terms at all. Three parts, all tinted by the diffuse (the metal's
    // COLOR: near-white = silver, warm = gold):
    // Metal = its environment, nothing else. ONE env sample — along the
    // reflection vector, Fresnel-boosted at grazing angles (the rim-brightening
    // that reads as "metal") — plus a tight glint from the directional light.
    // Exactly one sample means a doubled image is structurally impossible.
    // The diffuse map is the metal's COLOR (near-white = silver, warm = gold).
    // m = 0 reduces exactly to `color` above.
    float  m    = tex2D(sMetallic, uv).r;
    float3 R    = reflect(-V, n);
    float  ndv  = saturate(dot(n, V));
    float  fres = 0.55 + 0.45 * pow(1.0 - ndv, 2.0);
    float3 metal = texCUBE(sEnv, R).rgb * dtex.rgb * fres
                 + dtex.rgb * (gLightColor * pow(ndh, 96.0) + pglint);
    color = lerp(color, metal, m);

    // Clearcoat: a clear lacquer over the lit surface — an UNTINTED env sheen
    // ADDED on top (the paint underneath keeps its full color, unlike metal
    // which replaces it), plus the lacquer's own light glints: the directional
    // and point-light highlights, white/untinted (the coat is clear), same
    // tightness as the metal glints. Dielectric Fresnel on the env sheen:
    // faint face-on, mirror at grazing. Reuses the single env sample above —
    // no second sample, so no ghosting.
    float coat  = tex2D(sClearcoat, uv).r;
    float cfres = 0.05 + 0.95 * pow(1.0 - ndv, 4.0);
    color += texCUBE(sEnv, R).rgb * (coat * cfres)
           + (gLightColor * pow(ndh, 96.0) + pglint) * coat;

    float3 etex  = tex2D(sEmissive, uv).rgb; // self-illumination: unlit, additive
    color += etex;
    // Alpha: normally the diffuse's (cutout mask / blend factor). Opaque subsets
    // instead export the emissive strength — the bloom chain reads it back as
    // "how much should this pixel glow" (see bloom_bright.hlsl).
    float glow = max(etex.r, max(etex.g, etex.b));
    return float4(color, lerp(dtex.a, glow, gGlowMask));
}
