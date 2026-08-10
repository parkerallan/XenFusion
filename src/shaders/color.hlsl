// Solid Color attribute overlay — a flat screen-space block.
// VS c0 = (1/destW, 1/destH), PS c0 = authored rgb + alpha.
//
// No sampler: the constant IS the whole pixel, so a fade-to-black or a letterbox
// bar costs nothing but the fill. The VS still takes a UV because the overlay
// pass shares one FVF (XYZ|TEX1) across every quad kind, but it stops there —
// interpolating it to a PS that never reads it just burns a register, and fxc
// warns (X3596) about it, which deploy.ps1 treats as fatal.

float4 gHalfTexel : register(c0);

struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; };

VSOut VSMain(VSIn input)
{
    VSOut output;
    output.pos = float4(input.pos.x - gHalfTexel.x,
                        input.pos.y + gHalfTexel.y, 0.0, 1.0);
    return output;
}

float4 gColor : register(c0);

float4 PSMain() : COLOR
{
    return gColor;
}
