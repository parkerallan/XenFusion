// standard.hlsl — the built-in engine material: per-pixel diffuse / normal /
// specular lighting. (Custom project shaders live under the project's assets.)
// Shader Model 3.0 — the same source targets the Xbox 360's Xenos GPU (compiled
// offline by the XDK there; by fxc at build time / d3dcompiler at runtime here).
//
// Register layout (kept in sync with SceneRenderer.cpp):
//   VS  c0 = gWVP (world*view*proj)   c4 = gWorld
//   PS  c0 = light dir   c1 = camera pos   c2 = ambient
//       s0 = diffuse   s1 = normal   s2 = specular

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
sampler2D sDiffuse  : register(s0);
sampler2D sNormal   : register(s1);
sampler2D sSpecular : register(s2);

float4 PSMain(VSOut i) : COLOR
{
    float3 N = normalize(i.wn);
    float3 T = normalize(i.wt);
    float3 B = cross(N, T);
    float3 nt = tex2D(sNormal, i.uv).xyz * 2.0 - 1.0;   // tangent-space normal
    float3 n  = normalize(nt.x * T + nt.y * B + nt.z * N);

    float3 L   = -normalize(gLightDir);
    float  ndl = saturate(dot(n, L));
    float3 V   = normalize(gCameraPos - i.wpos);
    float3 H   = normalize(L + V);
    float  ndh = saturate(dot(n, H));

    float4 dtex = tex2D(sDiffuse, i.uv);
    float3 spec = tex2D(sSpecular, i.uv).rgb;
    float3 color = dtex.rgb * (gAmbient + ndl) + spec * pow(ndh, 40.0);
    return float4(color, dtex.a); // transparency from the diffuse's alpha
}
