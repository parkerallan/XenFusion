// skybox.hlsl — draws the "Skybox" attribute's equirectangular (lat-long) image
// as the scene background, on both the editor viewport and the 360. Driven by
// RenderSkybox in each renderer, and again into every face of the reflection
// capture so metal and rough surfaces mirror the sky instead of the clear colour.
//
// A screen-space quad, so the direction each pixel looks down has to be rebuilt.
// The host does that with camera/SkyView.h and hands the camera basis in; here
// it is just interpolated across the quad and turned into lat-long coordinates.
//
//   VS c0/c1/c2 = camera right / up / forward, pre-scaled by the lens (SkyView.h)
//   PS c0.x     = rotation, in turns (degrees / 360)
//   s0          = the sky image, WRAP on u, CLAMP on v
//
// Unlike the other screen-space passes here there is NO half-texel offset: those
// read a screen-sized texture and need their texels lining up, this reads a
// texture with no relation to the screen and has to cover the viewport exactly.
// Half a pixel of shift would leave an unwritten sliver down one edge.

float4 gRight : register(c0);
float4 gUp    : register(c1);
float4 gFwd   : register(c2);

struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float3 dir : TEXCOORD0; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    // z = 1 puts the quad on the far plane, so with ZFUNC LESSEQUAL against a
    // depth buffer cleared to 1 it survives only where nothing was drawn — the
    // sky is rejected by early-Z under geometry and costs nothing there.
    o.pos = float4(i.pos.x, i.pos.y, 1.0, 1.0);
    o.dir = gFwd.xyz + i.pos.x * gRight.xyz + i.pos.y * gUp.xyz;
    return o;
}

float4    gSky : register(c0); // x = rotation in turns
sampler2D sSky : register(s0);

float4 PSMain(VSOut i) : COLOR
{
    // acos needs a unit y; atan2 would not care, but one normalize covers both.
    float3 d = normalize(i.dir);

    // Rotation is a plain u offset — one full revolution is one wrap of the
    // image — so spinning the sky never costs a matrix.
    float u = atan2(d.z, d.x) * 0.15915494 + 0.5 + gSky.x;   // 1 / 2pi
    float v = acos(clamp(d.y, -1.0, 1.0)) * 0.31830989;      // 1 / pi

    // Explicit LOD, not tex2D: u steps by a full turn across the seam behind the
    // camera, and the hardware derivative there picks the coarsest mip, leaving a
    // one-pixel column of mush. Level 0 is the right level anyway — a full
    // revolution spans several screen widths, so the image is magnified.
    float3 c = tex2Dlod(sSky, float4(u, v, 0.0, 0.0)).rgb;

    // Alpha 0: the target's alpha channel is the emissive glow mask. Colour
    // writes are RGB-only at this point in both renderers, but a sky that leaked
    // into the mask would bloom the entire frame.
    return float4(c, 0.0);
}
