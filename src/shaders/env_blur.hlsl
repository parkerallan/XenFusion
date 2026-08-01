// env_blur.hlsl — blurs one face of one level of the environment cube, so rough
// surfaces reflect a softened version of the captured scene. Driven by
// BuildEnvBlurChain in both renderers; standard.hlsl reads the result with
// texCUBElod at LOD = roughness.
//
// A 3x3 Gaussian, but taken in DIRECTION space: the destination texel is turned
// back into the world direction stored there, and taps that walk off the face
// land on its neighbour through the cube sampler. Filtering the face as a flat
// 2D image would clamp at the edges and leave six seams.
//
//   VS c0 = (1/destW, 1/destH) — D3D9 half-pixel offset (bloom convention)
//   PS c0/c1/c2 = face right / up / forward axes
//   PS c3.x = tap spacing, face-space units (0 = plain copy)
//   PS c3.y = LOD to read from the source cube
//   PS c3.z = edge-fixup stretch (envcube::EdgeFixup)
//   s0 = source cube

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

float4      gFaceR : register(c0);
float4      gFaceU : register(c1);
float4      gFaceF : register(c2);
float4      gStep  : register(c3); // x = tap spacing, y = source LOD
samplerCUBE sEnv   : register(s0);

float4 PSMain(VSOut i) : COLOR
{
    // Undo FaceView + FaceProj to get the direction this texel holds; texture v
    // runs opposite NDC y. Nothing here is normalized — a cube fetch takes a
    // direction, not a unit vector.
    float2 st  = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0) * gStep.z;
    float3 dir = gFaceF.xyz + st.x * gFaceR.xyz + st.y * gFaceU.xyz;

    // One step is one texel, and the texel grid runs continuously across a face
    // edge, so the kernel does too.
    float3 du = gFaceR.xyz * gStep.x;
    float3 dv = gFaceU.xyz * gStep.x;
    float  L  = gStep.y;

    float3 acc = 4.0 * texCUBElod(sEnv, float4(dir, L)).rgb;
    acc += 2.0 * (texCUBElod(sEnv, float4(dir + du, L)).rgb +
                  texCUBElod(sEnv, float4(dir - du, L)).rgb +
                  texCUBElod(sEnv, float4(dir + dv, L)).rgb +
                  texCUBElod(sEnv, float4(dir - dv, L)).rgb);
    acc +=        texCUBElod(sEnv, float4(dir + du + dv, L)).rgb +
                  texCUBElod(sEnv, float4(dir + du - dv, L)).rgb +
                  texCUBElod(sEnv, float4(dir - du + dv, L)).rgb +
                  texCUBElod(sEnv, float4(dir - du - dv, L)).rgb;
    return float4(acc * (1.0 / 16.0), 1.0);
}
