sampler sMain : register(s0);
sampler sRect : register(s5);
float4 uBlend : register(c5);   // .x = 当前遍混合模式(0=普通, 1=加法)

float4 main(float2 cuv : TEXCOORD0, float2 tinfo : TEXCOORD1, float4 col : COLOR0) : COLOR0
{
    float4 rect = tex2D(sRect, float2((tinfo.x + 0.5) / 256.0, (tinfo.y + 0.5) / 32.0));
    float2 auv = rect.xy + rect.zw * cuv;
    float4 tex = tex2D(sMain, auv);
    
    float a = tex.a * col.a;
    
    float3 rgb = tex.rgb * col.rgb;
    rgb *= (uBlend.x > 0.5 || uBlend.y > 0.5) ? a : 1.0;
    
    return float4(rgb, a);
}
