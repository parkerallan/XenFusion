// Animated Image attribute overlay (a .gif) — CONSOLE ONLY.
//
// image.hlsl's twin. The one difference is the fetch: the frames live in a
// Xenos stacked (array) texture, one slice per GIF frame, so the current frame
// is selected by a W coordinate rather than by swapping textures. PC D3D9 has
// no array textures, so the renderer animates by binding a different 2D texture
// per frame and keeps using image.hlsl — which is also why this file is left
// out of CMakeLists' shader copy list and only reaches the console, where
// deploy.ps1 compiles every .hlsl in this folder.
//
// VS c0 = (1/destW, 1/destH), PS c0 = tint.rgb + authored alpha,
// PS c1.x = the slice's normalized centre, gifanim::SliceCoord().
//
// The caller must set MINFILTERZ/MAGFILTERZ to POINT so the fetch lands on
// exactly one slice; with linear Z filtering the GPU blends two frames.

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
sampler gImage : register(s0);

float4 PSMain(VSOut input) : COLOR
{
    float4 color = tex3D(gImage, float3(input.uv, gSlice.x));
    return float4(color.rgb * gTint.rgb, color.a * gTint.a);
}
