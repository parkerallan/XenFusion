// Lua-scriptable GUI overlay. One VS/PS pair covers all three quad kinds, so a
// whole menu ships as a single .cso pair on both targets.
//
//   VS c0 = destination half-texel (the D3D9 texel-centre fix)
//   PS c0 = quad color (rgb + alpha)
//   PS c1 = kind select — .x = 1 for a glyph, .y = 1 when the texture modulates
//           Solid (0,0)  Texture (0,1)  Glyph (1,0)
//
// Glyph deliberately leaves .y at 0: the font atlas is A8, so its rgb carries
// nothing and the color must come through untouched.

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
float4 gMode : register(c1);
sampler2D gTex : register(s0);

float4 PSMain(VSOut input) : COLOR
{
    float4 texel = tex2D(gTex, input.uv);

    // SDF coverage — the atlas stores distance in A8, same convention as the
    // cooked Text attribute atlas, so both fonts sources work unchanged.
    float distance = texel.a;
    float edgeWidth = max(abs(ddx(distance)) + abs(ddy(distance)), 1.0 / 255.0);
    float coverage = smoothstep(0.5 - edgeWidth, 0.5 + edgeWidth, distance);

    // Branchless kind select: the constants are uniform across the whole draw,
    // so two lerps beat a dynamic branch on Xenos.
    float3 rgb = lerp(float3(1.0, 1.0, 1.0), texel.rgb, gMode.y);
    float alpha = lerp(lerp(1.0, texel.a, gMode.y), coverage, gMode.x);

    return float4(rgb * gTint.rgb, alpha * gTint.a);
}
