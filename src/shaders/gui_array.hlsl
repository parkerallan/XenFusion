// Lua-scriptable GUI overlay, animated-.gif variant — CONSOLE ONLY.
//
// gui.hlsl's twin for one case: a gui.image whose texture is an animated GIF.
// On the console those frames live in a Xenos stacked (array) texture, so the
// current frame is chosen by a W coordinate instead of by binding a different
// texture. Only QuadTexture batches ever reach this shader — a GIF widget draws
// no glyphs — so the kind select collapses away and this is a straight modulate.
//
// PC D3D9 has no array textures: there, the host hands the GUI core a plain
// per-frame texture and the batch's slice stays negative, so gui.hlsl runs and
// this file is never needed. That is why it is left out of CMakeLists' shader
// copy list, while deploy.ps1 compiles every .hlsl in this folder for the 360.
//
//   VS c0 = destination half-texel (the D3D9 texel-centre fix)
//   PS c0 = quad color (rgb + alpha)
//   PS c1.x = the slice's normalized centre, gifanim::SliceCoord()
//
// The caller must set MINFILTERZ/MAGFILTERZ to POINT, or the fetch blends two
// frames together.

float4 gHalfTexel : register(c0);

struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(VSIn input)
{
    VSOut output;
    output.pos = float4(input.pos.x - gHalfTexel.x,
                        input.pos.y + gHalfTexel.y, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

float4 gTint  : register(c0);
float4 gSlice : register(c1);
sampler gTex : register(s0);

float4 PSMain(VSOut input) : COLOR
{
    float4 texel = tex3D(gTex, float3(input.uv, gSlice.x));
    return float4(texel.rgb * gTint.rgb, texel.a * gTint.a);
}
