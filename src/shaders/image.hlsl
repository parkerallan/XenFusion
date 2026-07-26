// Static Image attribute overlay.
// VS c0 = (1/destW, 1/destH), PS c0 = tint.rgb + authored alpha.

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

float4 gTint : register(c0);
sampler2D gImage : register(s0);

float4 PSMain(VSOut input) : COLOR
{
    float4 color = tex2D(gImage, input.uv);
    return float4(color.rgb * gTint.rgb, color.a * gTint.a);
}
