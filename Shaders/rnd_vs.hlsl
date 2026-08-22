sampler sOvr : register(s0);
sampler sPos : register(s1);
sampler sLife : register(s2);
sampler sType : register(s3);
float4x4 uWVP : register(c0);
float4 uSys : register(c4);
float4 uBlend : register(c5);

struct VSIN { float3 c : TEXCOORD0; };   // c.xy = 角点{0,1}, c.z = 粒子 id

struct VSOUT {
    float4 pos : POSITION;
    float2 cuv : TEXCOORD0;   // 角点 uv(插值 = 粒子内 0..1)
    float2 tinfo : TEXCOORD1; // type, frame
    float4 col : COLOR0;      // rgb = 颜色, a = alpha
};

float h1(float a) {
    a = frac(a * 0.1031);
    a = frac(a * (a + 19.19));
    a = frac(a * (a + 33.33));
    return frac(a * 43758.5453);
}

// GM8 尺寸语义: size0 = 随机[min,max] + incr*age(clamp≥0); wiggle = ±wiggle 三角波摆动。
// snap=true 时尺寸取整(点采样像素对齐的一半, 锚点吸附在 main 里做)。
float2 particle_size(float4 T0, float4 T3, float4 T12, float id, float age, bool snap)
{
    float size0 = max(lerp(T0.z, T0.w, h1(id + 3.0)) + T12.y * age, 0.0);
    float swv = fmod(h1(id + 9.0) * 16.0 + age, 16.0) / 4.0;
    swv = swv > 2.0 ? 4.0 - swv : swv;
    float size = size0 + (swv - 1.0) * T12.z;
    float2 psize = size * float2(T3.x, T3.y) * T12.x;

    // 点采样像素对齐(uSys.w = pixelsnap): 尺寸取整 + 锚点吸附整数网格,
    // 未旋转粒子达成纹素↔像素 1:1, 消除非整坐标下纹素宽窄不一的形变。
    if (snap)
        psize = floor(psize + 0.5);
    return psize;
}

// 角度: 范围随机 + 增量 + 三角波摆动(mod 16 折返, 部分和有界),
// relative 型追加速度朝向。返回弧度。
float particle_angle(float4 T7, float4 T8, float id, float age, float2 vel)
{
    float ang = lerp(T7.x, T7.y, h1(id + 5.0)) + T7.z * age;
    // 角度摆动: 三角波(与引擎绘制 rot 的 wave 一致, mod 16 折返, 部分和有界)
    float owv = fmod(h1(id + 31.0) * 16.0 + age, 16.0) / 4.0;
    owv = owv > 2.0 ? 4.0 - owv : owv;
    ang += (owv - 1.0) * T7.w;
    if (T8.x > 0.5)
        ang += atan2(-vel.y, vel.x) * 57.29577951308232;
    return ang * 0.017453292519943295;
}

// 随机双向镜像(T12.w 旗标, gpart_type_flip_random): 确定性 hash 逐粒翻角点,
// 先镜像后旋转(与 GM8 draw_sprite_ext 的缩放→旋转顺序一致)。
float2 particle_flip(float4 T12, float id)
{
    float flipX = T12.w > 0.5 ? (h1(id + 11.3) > 0.5 ? -1.0 : 1.0) : 1.0;
    float flipY = T12.w > 0.5 ? (h1(id + 17.7) > 0.5 ? -1.0 : 1.0) : 1.0;
    return float2(flipX, flipY);
}

// 颜色渐变: 覆盖色优先, 否则按 colour 模式 1/2/3 随寿命 t 插值。
float3 particle_color(float4 ov, float4 T3, float4 T4, float4 T5, float4 T6, float t)
{
    float mode = T3.z;
    if (ov.w > 0.5 || mode > 3.5)
        return ov.rgb;
    if (mode < 1.5)
        return T4.rgb;
    if (mode < 2.5)
        return lerp(T4.rgb, float3(T4.w, T5.x, T5.y), t);
    return t < 0.5
        ? lerp(T4.rgb, float3(T4.w, T5.x, T5.y), t * 2.0)
        : lerp(float3(T4.w, T5.x, T5.y), float3(T5.z, T5.w, T6.x), (t - 0.5) * 2.0);
}

// alpha 渐变: 按 alpha 模式 1/2/3 随寿命 t 插值;
// 类型的混合旗标与当前遍不匹配 → 零贡献。
float particle_alpha(float4 T3, float4 T6, float t, float blendMode, float additiveFlag)
{
    float am = T3.w;
    float a;
    if (am < 1.5)
        a = T6.y;
    else if (am < 2.5)
        a = lerp(T6.y, T6.z, t);
    else
    {
        a = t < 0.5
            ? lerp(T6.y, T6.z, t * 2.0)
            : lerp(T6.z, T6.w, (t - 0.5) * 2.0);
    }

    if (abs(additiveFlag - blendMode) > 0.5)
        a = 0.0;   // 混合模式不匹配当前遍 → 零贡献
    return a;
}

VSOUT main(VSIN v) {
    VSOUT o;

    float id = v.c.z;
    float2 uv = (float2(fmod(id, 256.0), floor(id / 256.0)) + 0.5) * uSys.z;
    float4 pl = tex2Dlod(sPos, float4(uv, 0, 0));
    float4 st = tex2Dlod(sLife, float4(uv, 0, 0));
    float age = st.x, life = st.y;
    float type = st.z;
    float frame = st.w;
    float dead = (age >= life) ? 1.0 : 0.0;
    float2 tuv = float2((type + 0.5) / 256.0, 0.0);

    float4 T0 = tex2Dlod(sType, float4(tuv.x, 0.5 / 14.0, 0, 0));
    float4 T2 = tex2Dlod(sType, float4(tuv.x, 2.5 / 14.0, 0, 0));
    float4 T3 = tex2Dlod(sType, float4(tuv.x, 3.5 / 14.0, 0, 0));
    float4 T4 = tex2Dlod(sType, float4(tuv.x, 4.5 / 14.0, 0, 0));
    float4 T5 = tex2Dlod(sType, float4(tuv.x, 5.5 / 14.0, 0, 0));
    float4 T6 = tex2Dlod(sType, float4(tuv.x, 6.5 / 14.0, 0, 0));
    float4 T7 = tex2Dlod(sType, float4(tuv.x, 7.5 / 14.0, 0, 0));
    float4 T8 = tex2Dlod(sType, float4(tuv.x, 8.5 / 14.0, 0, 0));
    float4 ov = tex2Dlod(sOvr, float4(uv, 0, 0));
    float4 T12 = tex2Dlod(sType, float4(tuv.x, 12.5 / 14.0, 0, 0));

    bool snap = uSys.w > 0.5;
    float2 psize = particle_size(T0, T3, T12, id, age, snap);
    float ang = particle_angle(T7, T8, id, age, pl.zw);
    float ca = cos(ang), sa = sin(ang);

    float2 corner = (v.c.xy * 2.0 - 1.0) * psize * 0.5 * particle_flip(T12, id);
    float2 off = float2(ca * corner.x - sa * corner.y, sa * corner.x + ca * corner.y);
    float2 wpos = pl.xy + uSys.xy;
    // 吸附未旋转左下角(wpos - psize/2)到整数网格: 角点 = 整数原点 + 整数尺寸 → 全整,
    // 奇偶尺寸都严格 1:1(只吸附中心的话奇数尺寸会得到半整数角点)
    if (snap)
        wpos = floor(wpos - psize * 0.5 + 0.5) + psize * 0.5;

    float4 clip = mul(uWVP, float4(wpos + off, 0, 1));
    o.pos = dead > 0.5 ? float4(2.0, 2.0, 0.5, 1.0) : clip;   // 全部角点同点，零面积三角形被剔除
    o.cuv = v.c.xy;
    o.tinfo = float2(type, frame);

    float t = life > 0.0001 ? clamp(age / life, 0.0, 1.0) : 1.0;
    float3 col = particle_color(ov, T3, T4, T5, T6, t);
    float a = particle_alpha(T3, T6, t, uBlend.x, T2.w);
    o.col = float4(col, a);
    return o;
}
