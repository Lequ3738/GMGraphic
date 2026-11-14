sampler sMain : register(s0);
sampler sRect : register(s5);
float4 uBlend : register(c5);   // .x = 当前遍混合模式(0=普通, 1=加法)
float4 uSysCol : register(c6);  // 系统级染色与不透明度乘数(gpart_system_color, 恒等 = 白/1)
// 矩形表尺寸(GP_TYPE_TEX_W × GP_RECT_TEX_FRAMES)由宿主编译前注入, 见 gpart.cpp
// type_layout_defines; 本文件不含布局字面量。

float4 main(float2 cuv : TEXCOORD0, float2 tinfo : TEXCOORD1, float4 col : COLOR0) : COLOR0
{
    float4 rect = tex2D(sRect, float2((tinfo.x + 0.5) / GP_TYPE_TEX_W, (tinfo.y + 0.5) / GP_RECT_TEX_FRAMES));
    float2 auv = rect.xy + rect.zw * cuv;
    float4 tex = tex2D(sMain, auv);
    
    float a = tex.a * col.a * uSysCol.a;
    
    float3 rgb = tex.rgb * col.rgb * uSysCol.rgb;
    rgb *= (uBlend.x > 0.5 || uBlend.y > 0.5) ? a : 1.0;
    
    return float4(rgb, a);
}
