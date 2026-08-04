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
//       s7 = roughness (r: 0 = glossy, 1 = matte) — widens every specular lobe
//            AND picks a blurrier mip of the env cube; also dials in a broad
//            dim reflection on plain dielectrics, which is what makes a matte
//            character still catch its surroundings. No map = black = 0 =
//            exactly the pre-roughness math.
//       c15 = glow-mask switch: 1 = output alpha carries the emissive strength
//             (the bloom chain's glow source — opaque subsets only), 0 = output
//             alpha is the diffuse's (cutout / blend passes need it)
//       c22.x = max env mip (3 when the blurred capture chain is bound, 0 when
//             s5 is the painted/black fallback, which has no mip chain)
//       c22.yzw = global baked environment
//       c23 = shadow texel size, depth bias, receiver strength
//       c24 = probe color + probe mix, c25 = probe direction + directionality
//       VS c225-c228 = four explicit light-space transform columns
//       s8 = lm0 RGBM illumination (16x range)
//       s9 = lm1 dominant direction + strength
// (PS c3/c4 stay free: the custom-shader convention claims them for gTime/gCamObj.)

float4x4 gWVP   : register(c0);
float4x4 gWorld : register(c4);

struct VSIn  { float3 pos:POSITION; float3 nrm:NORMAL; float3 tan:TANGENT; float2 uv:TEXCOORD0; };
struct LightmapVSIn { float3 pos:POSITION; float3 nrm:NORMAL; float3 tan:TANGENT; float2 uv:TEXCOORD0; float2 uv1:TEXCOORD1; };
struct VSOut { float4 pos:POSITION; float2 uv:TEXCOORD0; float3 wpos:TEXCOORD1; float3 wn:TEXCOORD2; float3 wt:TEXCOORD3; float2 uv1:TEXCOORD4; float4 shadowPos:TEXCOORD5; };
float4 gShadowX : register(c225);
float4 gShadowY : register(c226);
float4 gShadowZ : register(c227);
float4 gShadowW : register(c228);

float4 ShadowPosition(float4 worldPosition)
{
    return float4(dot(worldPosition, gShadowX), dot(worldPosition, gShadowY),
                  dot(worldPosition, gShadowZ), dot(worldPosition, gShadowW));
}

// Skinned meshes add a second 8-byte stream: four byte joint indices and four
// normalized byte weights. Each joint occupies three float4 constants holding
// the columns of a row-vector affine transform. c8-c223 supports 72 joints.
struct SkinVSIn
{
    float3 pos:POSITION;
    float3 nrm:NORMAL;
    float3 tan:TANGENT;
    float2 uv:TEXCOORD0;
    float4 joints:BLENDINDICES;
    float4 weights:BLENDWEIGHT;
};
float4 gSkinPalette[216] : register(c8);

float3 SkinTransform(float3 value, float vector_w, int joint)
{
    int base = joint * 3;
    float4 source = float4(value, vector_w);
    return float3(dot(source, gSkinPalette[base + 0]),
                  dot(source, gSkinPalette[base + 1]),
                  dot(source, gSkinPalette[base + 2]));
}

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos  = mul(float4(i.pos, 1.0), gWVP);
    o.wpos = mul(float4(i.pos, 1.0), gWorld).xyz;
    o.shadowPos = ShadowPosition(float4(o.wpos, 1.0));
    o.wn   = mul(i.nrm, (float3x3)gWorld);
    o.wt   = mul(i.tan, (float3x3)gWorld);
    o.uv   = i.uv;
    o.uv1  = 0;
    return o;
}

VSOut LightmapVSMain(LightmapVSIn i)
{
    VSOut o;
    o.pos  = mul(float4(i.pos, 1.0), gWVP);
    o.wpos = mul(float4(i.pos, 1.0), gWorld).xyz;
    o.shadowPos = ShadowPosition(float4(o.wpos, 1.0));
    o.wn   = mul(i.nrm, (float3x3)gWorld);
    o.wt   = mul(i.tan, (float3x3)gWorld);
    o.uv   = i.uv;
    o.uv1  = i.uv1;
    return o;
}

VSOut SkinVSMain(SkinVSIn i)
{
    VSOut o;
    float3 skinned_pos = 0;
    float3 skinned_nrm = 0;
    float3 skinned_tan = 0;
    [unroll] for (int influence = 0; influence < 4; ++influence)
    {
        int joint = (int)i.joints[influence];
        float weight = i.weights[influence];
        skinned_pos += SkinTransform(i.pos, 1.0, joint) * weight;
        skinned_nrm += SkinTransform(i.nrm, 0.0, joint) * weight;
        skinned_tan += SkinTransform(i.tan, 0.0, joint) * weight;
    }
    o.pos  = mul(float4(skinned_pos, 1.0), gWVP);
    o.wpos = mul(float4(skinned_pos, 1.0), gWorld).xyz;
    o.shadowPos = ShadowPosition(float4(o.wpos, 1.0));
    o.wn   = mul(skinned_nrm, (float3x3)gWorld);
    o.wt   = mul(skinned_tan, (float3x3)gWorld);
    o.uv   = i.uv;
    o.uv1  = 0;
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
sampler2D sRoughness : register(s7); // r: 0 = glossy (legacy), 1 = fully matte
sampler2D sLightmap0 : register(s8);
sampler2D sLightmap1 : register(s9);
sampler2D sShadow : register(s10);
float4 gRough : register(c22);       // x = max env mip; yzw = global baked env
float4 gShadowParams : register(c23);// xy = texel size, z = bias, w = receiver strength
float4 gProbeCol : register(c24);     // rgb = probe light, w = mover probe mix
float4 gProbeDir : register(c25);     // xyz = dir * 0.5 + 0.5, w = directionality

float3 PackShadowDepth(float depth)
{
    float3 packed = frac(saturate(depth) * float3(1.0, 255.0, 65025.0));
    packed -= packed.yzz * float3(1.0 / 255.0, 1.0 / 255.0, 0.0);
    return packed;
}

float UnpackShadowDepth(float3 packed)
{
    return dot(packed, float3(1.0, 1.0 / 255.0, 1.0 / 65025.0));
}

float ShadowVisibility(float4 shadowPosition)
{
    if (gShadowParams.w <= 0.0 || shadowPosition.w <= 0.0) return 1.0;
    float3 projected = shadowPosition.xyz / shadowPosition.w;
    float2 uv = float2(projected.x * 0.5 + 0.5, -projected.y * 0.5 + 0.5);
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || projected.z <= 0.0 || projected.z >= 1.0)
        return 1.0;
    float receiver = projected.z - gShadowParams.z;
    float2 halfTexel = gShadowParams.xy * 0.5;
    float visibility = 0.0;
    visibility += receiver <= UnpackShadowDepth(tex2D(sShadow, uv + float2(-halfTexel.x, -halfTexel.y)).rgb);
    visibility += receiver <= UnpackShadowDepth(tex2D(sShadow, uv + float2( halfTexel.x, -halfTexel.y)).rgb);
    visibility += receiver <= UnpackShadowDepth(tex2D(sShadow, uv + float2(-halfTexel.x,  halfTexel.y)).rgb);
    visibility += receiver <= UnpackShadowDepth(tex2D(sShadow, uv + float2( halfTexel.x,  halfTexel.y)).rgb);
    return lerp(1.0, visibility * 0.25, gShadowParams.w);
}

struct ShadowPSIn
{
    float2 uv : TEXCOORD0;
    float4 shadowPos : TEXCOORD5;
};

float4 ShadowPSMain(ShadowPSIn i) : COLOR
{
    clip(tex2D(sDiffuse, i.uv).a - 0.5);
    return float4(PackShadowDepth(i.shadowPos.z / i.shadowPos.w), 1.0);
}

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
    float  shadow = ShadowVisibility(i.shadowPos);

    float4 dtex = tex2D(sDiffuse, uv);
    float3 spec = tex2D(sSpecular, uv).rgb;

    // Roughness sets every "how tight is this highlight" number below. At
    // rough = 0 these are exactly 40 and 96, so a subset with no roughness map
    // (s7 = the 1x1 black default) matches the pre-roughness shader term for term.
    float rough    = tex2D(sRoughness, uv).r;
    float specPow  = 40.0 * exp2(-3.3219281 * rough); // 40 .. 4
    float glintPow = 96.0 * exp2(-4.5849625 * rough); // 96 .. 4

    // Directional light (diffuse + specular) plus up to four point lights
    // (diffuse only — the 360-era forward trade-off). Point falloff fades
    // smoothly to zero at range: (1 - (d/range)^2)^2, with w = 1/range^2.
    // The max() keeps rsqrt finite at the light's own position; unused slots
    // upload zero color and contribute nothing.
    // pglint: per-point-light specular, used by the METAL term only (dielectric
    // point lighting stays diffuse-only — the original trade-off). Same falloff
    // as the diffuse so a light's glint dies with its light.
    float3 diffuse = gLightColor * (ndl * shadow);
    float3 pglint  = 0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        float3 pl  = gPointPos[k].xyz - i.wpos;
        float  d2  = max(dot(pl, pl), 1e-4);
        float  att = saturate(1.0 - d2 * gPointPos[k].w);
        float3 Lp  = pl * rsqrt(d2);
        diffuse += gPointCol[k] * (saturate(dot(n, Lp)) * (att * att));
        float3 Hp  = normalize(Lp + V);
        pglint += gPointCol[k] * (pow(saturate(dot(n, Hp)), glintPow) * (att * att));
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
        pglint += gSpotCol[j].rgb * (pow(saturate(dot(n, Hs)), glintPow) * satt);
    }
    float probeMix = gProbeCol.w;
    float4 lm1 = lerp(tex2D(sLightmap1, i.uv1), gProbeDir, probeMix);
    float3 dominant = normalize(lm1.rgb * 2.0 - 1.0);
    float directional = (1.0 - lm1.a) + lm1.a * 2.0 * saturate(dot(n, dominant)) * shadow;
    float4 lm0 = tex2D(sLightmap0, i.uv1);
    float3 bakedColor = lerp(lm0.rgb * (lm0.a * 16.0), gProbeCol.rgb, probeMix);
    float3 baked = bakedColor * directional;
    float3 indirect = gAmbient + gRough.yzw * (1.0 - probeMix) + baked;
    float3 color = dtex.rgb * (indirect + diffuse)
                 + spec * (gLightColor * (pow(ndh, specPow) * shadow));

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
    // The one env sample, shared by the metal, clearcoat and matte terms below,
    // so a doubled reflection stays impossible. texCUBElod not texCUBEbias: the
    // mip must be the material's choice, not the rasterizer's. rough = 0 gives
    // LOD 0, the old texCUBE.
    float3 envc = texCUBElod(sEnv, float4(R, rough * gRough.x)).rgb;
    // Rough metal is duller as well as blurrier; blur alone keeps the average,
    // which leaves a matte metal reading as bright chrome.
    float3 metal = envc * dtex.rgb * (fres * (1.0 - 0.35 * rough))
                 + dtex.rgb * (gLightColor * (pow(ndh, glintPow) * shadow) + pglint);
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
    color += envc * (coat * cfres)
            + (gLightColor * (pow(ndh, glintPow) * shadow) + pglint) * coat;

    // Matte surfaces still catch the room: a broad, dim, surface-tinted
    // reflection the specular lobe cannot express, since that only knows about
    // the light. Scaled by roughness, so the map is the dial and a subset
    // without one contributes exactly zero. Its own Fresnel, not the
    // clearcoat's — that one is near zero face-on, which would reduce "matte"
    // to a rim effect visible only on silhouettes.
    float mfres = 0.25 + 0.75 * pow(1.0 - ndv, 3.0);
    color += envc * dtex.rgb * (mfres * rough * 0.40);

    float3 etex  = tex2D(sEmissive, uv).rgb; // self-illumination: unlit, additive
    color += etex;
    // Alpha: normally the diffuse's (cutout mask / blend factor). Opaque subsets
    // instead export the emissive strength — the bloom chain reads it back as
    // "how much should this pixel glow" (see bloom_bright.hlsl).
    float glow = max(etex.r, max(etex.g, etex.b));
    return float4(color, lerp(dtex.a, glow, gGlowMask));
}
