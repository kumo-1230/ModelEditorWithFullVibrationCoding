#pragma once
// HLSL シェーダー (Shader Model 4.0: Direct3D 機能レベル 10_0 以上で動作)

static const char* kMeshHLSL = R"(
cbuffer Frame : register(b0) {
    float4x4 uViewProj;
    float4 uCamPos;
    float4 uLightDir;
    float4 uParams;      // x=ライト強度 y=環境光 z=露出 w=表示モード
};
cbuffer Draw : register(b1) {
    float4x4 uModel;
    float4x4 uNormalMat;
    float4 uBaseColor;
    float4 uEmissive;    // xyz=発光 w=法線スケール
    float4 uMat;         // x=メタリック y=ラフネス z=AO強度 w=カットオフ
    int4 uFlags0;        // x=アルファモード y=unlit z=法線あり w=接線あり
    int4 uFlags1;        // x=頂点カラー y=発光テクスチャあり z=発光UV
    int4 uHasTex;        // ベース, MR, 法線, AO
    int4 uUVSet;         // ベース, MR, 法線, AO
    float4 uOverrideColor;
    float4 uPickColor;
};

Texture2D tBase : register(t0);
Texture2D tMR : register(t1);
Texture2D tNormal : register(t2);
Texture2D tOcc : register(t3);
Texture2D tEmis : register(t4);
Texture2D tChecker : register(t5);
SamplerState sBase : register(s0);
SamplerState sMR : register(s1);
SamplerState sNormal : register(s2);
SamplerState sOcc : register(s3);
SamplerState sEmis : register(s4);
SamplerState sChecker : register(s5);

struct VSIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv0 : TEXCOORD0;
    float4 col : COLOR;
    float4 tan : TANGENT;
    float2 uv1 : TEXCOORD1;
    float weight : TEXCOORD2;
};
struct VSOut {
    float4 pos : SV_Position;
    float3 wpos : POSITION1;
    float3 nrm : NORMAL;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 col : COLOR;
    float4 tan : TANGENT;
    float weight : TEXCOORD2;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    float4 wp = mul(uModel, float4(i.pos, 1.0));
    o.wpos = wp.xyz;
    o.pos = mul(uViewProj, wp);
    o.nrm = mul((float3x3)uNormalMat, i.nrm);
    o.tan = float4(mul((float3x3)uModel, i.tan.xyz), i.tan.w);
    o.uv0 = i.uv0;
    o.uv1 = i.uv1;
    o.col = i.col;
    o.weight = i.weight;
    return o;
}

static const float PI = 3.14159265;
float2 uvSet(VSOut i, int s) { return s == 1 ? i.uv1 : i.uv0; }
float3 toLinear(float3 c) { return pow(max(c, 0.0), 2.2); }
float3 toSRGB(float3 c) { return pow(max(c, 0.0), 1.0 / 2.2); }
float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float3 heat(float t) {
    t = saturate(t);
    return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)));
}
float3 brdf(float3 N, float3 V, float3 L, float3 base, float metallic, float rough, float3 radiance) {
    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    float a = max(rough * rough, 1e-3);
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * d * d);
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float G = (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    float3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    float3 spec = D * G * F / max(4.0 * NdotL * NdotV, 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * base / PI + spec) * radiance * NdotL;
}

float4 PSMain(VSOut i, bool front : SV_IsFrontFace) : SV_Target {
    if (uOverrideColor.a > 0.0) return uOverrideColor;
    int mode = (int)uParams.w;
    int alphaMode = uFlags0.x;

    float4 base = uBaseColor;
    if (uHasTex.x == 1) {
        float4 t = tBase.Sample(sBase, uvSet(i, uUVSet.x));
        base *= float4(toLinear(t.rgb), t.a);
    }
    if (uFlags1.x == 1) base *= i.col;
    if (alphaMode == 1 && base.a < uMat.w) discard;
    if (alphaMode == 0) base.a = 1.0;

    // OpenGL と同じ向きの微分 (D3D の画面 Y は下向き)
    float3 dpx = ddx(i.wpos), dpy = -ddy(i.wpos);
    float3 Ng = normalize(cross(dpx, dpy));
    float3 N = uFlags0.z == 1 ? normalize(i.nrm) : Ng;
    if (!front) N = -N;

    float3 T = float3(1, 0, 0);
    if (uHasTex.z == 1 || mode == 9) {
        float3 B;
        float2 uv = uvSet(i, uUVSet.z);
        if (uFlags0.w == 1) {
            T = normalize(i.tan.xyz - N * dot(N, i.tan.xyz));
            B = cross(N, T) * i.tan.w;
        } else {
            float2 duv1 = ddx(uv), duv2 = -ddy(uv);
            float3 dp2perp = cross(dpy, N), dp1perp = cross(N, dpx);
            T = dp2perp * duv1.x + dp1perp * duv2.x;
            B = dp2perp * duv1.y + dp1perp * duv2.y;
            float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
            T *= invmax;
            B *= -invmax; // glTF の UV は V 下向き
        }
        if (uHasTex.z == 1) {
            float3 tn = tNormal.Sample(sNormal, uv).xyz * 2.0 - 1.0;
            tn.xy *= uEmissive.w;
            N = normalize(T * tn.x + B * tn.y + N * tn.z);
        }
    }

    float metallic = uMat.x, rough = uMat.y;
    if (uHasTex.y == 1) {
        float4 mr = tMR.Sample(sMR, uvSet(i, uUVSet.y));
        rough *= mr.g;
        metallic *= mr.b;
    }
    rough = clamp(rough, 0.03, 1.0);
    float ao = 1.0;
    if (uHasTex.w == 1) ao = 1.0 + uMat.z * (tOcc.Sample(sOcc, uvSet(i, uUVSet.w)).r - 1.0);
    float3 emissive = uEmissive.xyz;
    if (uFlags1.y == 1) emissive *= toLinear(tEmis.Sample(sEmis, uvSet(i, uFlags1.z)).rgb);

    float3 V = normalize(uCamPos.xyz - i.wpos);
    float3 L = normalize(-uLightDir.xyz);
    float lambert = max(dot(N, L), 0.0) * 0.7 + 0.3;
    float3 outColor;

    if (mode == 1) outColor = toSRGB(base.rgb);
    else if (mode == 2) outColor = N * 0.5 + 0.5;
    else if (mode == 3) outColor = float3(frac(i.uv0), 0.0);
    else if (mode == 4) outColor = tChecker.Sample(sChecker, i.uv0 * 4.0).rgb * lambert;
    else if (mode == 5) outColor = heat(i.weight) * lambert;
    else if (mode == 6) outColor = i.col.rgb * lambert;
    else if (mode == 7) outColor = float3(0.0, rough, metallic);
    else if (mode == 8) outColor = float3(ao, ao, ao);
    else if (mode == 9) outColor = uFlags0.w == 1 ? normalize(i.tan.xyz) * 0.5 + 0.5 : float3(1, 0, 1);
    else if (mode == 10) outColor = front ? float3(0.62, 0.66, 0.72) * lambert : float3(0.95, 0.12, 0.12) * (0.5 + 0.5 * lambert);
    else if (mode == 11) {
        // 頂点法線がカメラ側を向くかと、表面 (巻き順) がカメラ側を向くかが一致しなければ桃色
        if (uFlags0.z != 1) outColor = float3(1.0, 0.6, 0.0);
        else {
            bool agree = (dot(normalize(i.nrm), V) > 0.0) == front;
            outColor = agree ? float3(0.62, 0.66, 0.72) * lambert : float3(1.0, 0.2, 0.65);
        }
    }
    else if (mode == 12) outColor = heat(i.weight) * lambert;
    else if (uFlags0.y == 1) outColor = toSRGB(base.rgb);
    else {
        float3 c = brdf(N, V, L, base.rgb, metallic, rough, uParams.xxx);
        float3 L2 = normalize(float3(0.6, 0.3, 0.7));
        c += brdf(N, V, L2, base.rgb, metallic, rough, uParams.xxx * 0.25);
        float3 sky = float3(0.60, 0.65, 0.75), ground = float3(0.25, 0.22, 0.20);
        float3 amb = lerp(ground, sky, N.y * 0.5 + 0.5) * uParams.y;
        float3 F0 = lerp(float3(0.04, 0.04, 0.04), base.rgb, metallic);
        c += amb * (base.rgb * (1.0 - metallic) + F0 * (1.0 - rough * 0.7)) * ao;
        c += emissive;
        outColor = toSRGB(aces(c * uParams.z));
    }
    return float4(outColor, alphaMode == 2 ? base.a : 1.0);
}

float4 PSPick(VSOut i) : SV_Target { return uPickColor; }
)";

static const char* kLineHLSL = R"(
cbuffer Frame : register(b0) {
    float4x4 uViewProj;
    float4 uCamPos;
    float4 uLightDir;
    float4 uParams;
};
struct VSIn { float3 pos : POSITION; float4 col : COLOR; };
struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };
VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = mul(uViewProj, float4(i.pos, 1.0));
    o.col = i.col;
    return o;
}
float4 PSMain(VSOut i) : SV_Target { return i.col; }
)";
