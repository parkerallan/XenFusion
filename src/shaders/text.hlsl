// Cooked Text attribute overlay. The atlas stores an SDF in A8.
// VS c0 = destination half-texel, PS c0 = text color + authored alpha.

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
sampler2D gAtlas : register(s0);

float4 PSMain(VSOut input) : COLOR
{
    float distance = tex2D(gAtlas, input.uv).a;
    float edgeWidth = max(abs(ddx(distance)) + abs(ddy(distance)), 1.0 / 255.0);
    float coverage = smoothstep(0.5 - edgeWidth, 0.5 + edgeWidth, distance);
    return float4(gTint.rgb, coverage * gTint.a);
}