sampler sOvr : register(s0);
sampler sPos : register(s1);
sampler sLife : register(s2);
sampler sType : register(s3);

float4x4 uWVP : register(c0);
float4 uSys : register(c4);
float4 uBlend : register(c5);   // .x = 当前遍混合模式(0=普通, 1=加法), .y = 预乘输出, .z = 当前桶号
// 区域覆盖器(gpart 扩展): 最多 4 个, 与演化特效器表无关, 全走 uniform 常量。
float4 uOvGeom[4] : register(c6);    // 每覆盖器 (xmin, xmax, ymin, ymax)
float4 uOvMeta[4] : register(c10);   // 每覆盖器 (active, shape, scale, alpha)
float4 uOvColor[4] : register(c14);  // 每覆盖器 (r, g, b, 0)
// 类型表行布局由宿主编译前注入: TYPE_ROWS_INV 与 ROW_<行名> 生成自 gpart.cpp 的
// GTypeRow 枚举(type_layout_defines), 本文件不含布局字面量。

struct VSIN
{
    float3 c : TEXCOORD0;     // c.xy = 角点{0,1}, c.z = 粒子 id
};

struct VSOUT
{
    float4 pos : POSITION;
    float2 cuv : TEXCOORD0;   // 角点 uv(插值 = 粒子内 0..1)
    float2 tinfo : TEXCOORD1; // type, frame
    float4 col : COLOR0;      // rgb = 颜色, a = alpha
};

float h1(float a)
{
    a = frac(a * 0.1031);
    a = frac(a * (a + 19.19));
    a = frac(a * (a + 33.33));
    return frac(a * 43758.5453);
}

// GM8 尺寸语义: size0 = 随机[min,max] + incr*age(clamp≥0); wiggle = ±wiggle 三角波摆动。
// 尺寸硬门(gpart 扩展): 开启时年龄课程整体替换——前 from 寿命内 portion 比例的粒子
// (h1 确定性分群)保持出生随机尺寸, 其余时刻/粒子恒为 end; incr 不参与, wiggle 照常叠加。
// snap=true 时尺寸取整(点采样像素对齐的一半, 锚点吸附在 main 里做)。
float2 particle_size(float4 T0, float4 T3, float4 T9, float4 T11, float4 T12, float4 T15, float id, float age, float t, bool snap)
{
    float birth = lerp(T0.z, T0.w, h1(id + 3.0));
    float size0;
    if (T11.w > 0.5)
        size0 = max((t < T15.z && h1(id + 47.7) < T15.w) ? birth : T9.z, 0.0);
    else
        size0 = max(birth + T12.y * age, 0.0);
    float swv = fmod(h1(id + 9.0) * 16.0 + age, 16.0) / 4.0;
    swv = swv > 2.0 ? 4.0 - swv : swv;
    float size = size0 + (swv - 1.0) * T12.z;
    
    float2 psize = size * float2(T3.x, T3.y) * T12.x;
    if (snap)  // 点采样像素对齐: 未旋转粒子消除非整坐标下纹素宽窄不一的形变。
        psize = floor(psize + 0.5);
    
    return psize;
}

// 角度: 范围随机 + 增量 + 三角波摆动(mod 16 折返, 部分和有界),
// relative 型追加速度朝向。返回弧度。
float particle_angle(float4 T7, float4 T8, float id, float age, float2 vel)
{
    float ang = lerp(T7.x, T7.y, h1(id + 5.0)) + T7.z * age;
    float owv = fmod(h1(id + 31.0) * 16.0 + age, 16.0) / 4.0; // 角度摆动
    owv = owv > 2.0 ? 4.0 - owv : owv;
    ang += (owv - 1.0) * T7.w;
    if (T8.x > 0.5)
        ang += atan2(-vel.y, vel.x) * 57.29577951308232;
    return ang * 0.017453292519943295;
}

// 随机双向镜像
float2 particle_flip(float4 T12, float id)
{
    float flipX = T12.w > 0.5 ? (h1(id + 11.3) > 0.5 ? -1.0 : 1.0) : 1.0;
    float flipY = T12.w > 0.5 ? (h1(id + 17.7) > 0.5 ? -1.0 : 1.0) : 1.0;
    return float2(flipX, flipY);
}

// 颜色渐变: 覆盖色优先, 否则按 color 关键帧模式 1/2/3/7 随寿命 t 插值。
// band=1 硬色阶: 按关键帧数等分寿命取最近帧; band=0 lerp: 相邻关键帧顺序插值。
// mix/rgb/hsv(4-6) 的出生随机色已烘进覆盖通道, 直接返回。
float3 particle_color(float4 ov, float4 T3, float4 T4, float4 T5, float4 T6,
    float4 T14, float band, float t)
{
    float mode = T3.z;
    if (ov.w > 0.5)
        return ov.rgb;
    if (mode < 1.5)
        return T4.rgb;
    if (mode < 2.5)
    {
        float3 c2 = float3(T4.w, T5.x, T5.y);
        return band > 0.5 ? (t < 0.5 ? T4.rgb : c2) : lerp(T4.rgb, c2, t);
    }
    if (mode < 3.5)
    {
        float3 c2 = float3(T4.w, T5.x, T5.y);
        float3 c3 = float3(T5.z, T5.w, T6.x);
        if (band > 0.5)
            return t < 1.0/3.0 ? T4.rgb : (t < 2.0/3.0 ? c2 : c3);
        return t < 0.5
            ? lerp(T4.rgb, c2, t * 2.0)
            : lerp(c2, c3, (t - 0.5) * 2.0);
    }
    if (mode > 6.5)
    {
        float3 c2 = float3(T4.w, T5.x, T5.y);
        float3 c3 = float3(T5.z, T5.w, T6.x);
        float3 c4 = T14.yzw;
        if (band > 0.5)
            return t < 0.25 ? T4.rgb : (t < 0.5 ? c2 : (t < 0.75 ? c3 : c4));
        return t < 1.0/3.0 ? lerp(T4.rgb, c2, t * 3.0)
             : t < 2.0/3.0 ? lerp(c2, c3, (t - 1.0/3.0) * 3.0)
             : lerp(c3, c4, (t - 2.0/3.0) * 3.0);
    }
    return ov.rgb;
}

// alpha 渐变: 按 alpha 模式 1/2/3 随寿命 t 插值。
// blendMode/additiveFlag 与 bucketFlag/当前桶号 任一不匹配当前遍 → 零贡献,
// 使同一静态 VB 可被逐遍复用(每遍只让目标粒子通过)。
float particle_alpha(float4 T3, float4 T6, float t, float blendMode, float additiveFlag,
    float bucketFlag)
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

    if (abs(additiveFlag - blendMode) > 0.5 || abs(bucketFlag - uBlend.z) > 0.5)
        a = 0.0;   // 混合模式或深度桶不匹配当前遍 → 零贡献
    return a;
}

// 区域覆盖器命中测试: 与 destroyer/deflector 同一坐标空间(粒子原始位置, 未加系统偏移)。
// 纯位置函数, 每帧重算 → 粒子进出区域自动生效/还原, 零粒子状态写入。
// 空槽(active<0.5)与退化区域同款守卫; 首个命中生效, 后续命中不覆盖。
void overlay_probe(float2 pos, out bool hit, out float3 ocol, out float oscale, out float oalpha)
{
    hit = false;
    ocol = float3(0.0, 0.0, 0.0);
    oscale = 1.0;
    oalpha = 1.0;

    for (int i = 0; i < 4; ++i)
    {
        float4 m = uOvMeta[i];
        float4 g = uOvGeom[i];
        if (m.x < 0.5)   // 空槽
            continue;
        if (g.x >= g.y || g.z >= g.w)   // 退化区域(与 destroyer 同款守卫)
            continue;

        if (pos.x >= g.x && pos.x <= g.y && pos.y >= g.z && pos.y <= g.w)
        {
            float nx = 2.0 * (pos.x - (g.y + g.x) * 0.5) / (g.y - g.x);
            float ny = 2.0 * (pos.y - (g.w + g.z) * 0.5) / (g.w - g.z);
            // shape: 0=矩形(先过了包围盒), 1=椭圆, 2=菱形
            float hf = m.y < 0.5 ? 1.0
                : (m.y > 0.5 && m.y < 1.5) ? (nx * nx + ny * ny <= 1.0 ? 1.0 : 0.0)
                : (abs(nx) + abs(ny) <= 1.0 ? 1.0 : 0.0);

            if (hf > 0.5 && !hit)
            {
                hit = true;
                ocol = uOvColor[i].rgb;
                oscale = m.z;
                oalpha = m.w;
            }
        }
    }
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
    float2 tuv = float2((type + 0.5) / GP_TYPE_TEX_W, 0.0);

    float4 T0 = tex2Dlod(sType, float4(tuv.x, (ROW_LifeSize + 0.5) * TYPE_ROWS_INV, 0, 0));
    float4 T2 = tex2Dlod(sType, float4(tuv.x, (ROW_GravityDragBlend + 0.5) * TYPE_ROWS_INV, 0, 0));
    float4 T3 = tex2Dlod(sType, float4(tuv.x, (ROW_ScaleModes + 0.5) * TYPE_ROWS_INV, 0, 0));
    float4 T4 = tex2Dlod(sType, float4(tuv.x, (ROW_ColorA + 0.5) * TYPE_ROWS_INV, 0, 0));
    float4 T5 = tex2Dlod(sType, float4(tuv.x, (ROW_ColorB + 0.5) * TYPE_ROWS_INV, 0, 0));
    float4 T6 = tex2Dlod(sType, float4(tuv.x, (ROW_ColorCAndAlpha + 0.5) * TYPE_ROWS_INV, 0, 0));
    float4 T7 = tex2Dlod(sType, float4(tuv.x, (ROW_Orientation + 0.5) * TYPE_ROWS_INV, 0, 0));
    float4 T8 = tex2Dlod(sType, float4(tuv.x, (ROW_Animation + 0.5) * TYPE_ROWS_INV, 0, 0));
    float4 T9 = tex2Dlod(sType, float4(tuv.x, (ROW_RandomFrame + 0.5) * TYPE_ROWS_INV, 0, 0));    // .y = 关键帧插值风格, .z = 尺寸硬门 end
    float4 ov = tex2Dlod(sOvr, float4(uv, 0, 0));
    float4 T11 = tex2Dlod(sType, float4(tuv.x, (ROW_FeatureFlags + 0.5) * TYPE_ROWS_INV, 0, 0));  // .w = 尺寸硬门开关
    float4 T12 = tex2Dlod(sType, float4(tuv.x, (ROW_RenderSize + 0.5) * TYPE_ROWS_INV, 0, 0));
    float4 T14 = tex2Dlod(sType, float4(tuv.x, (ROW_DepthBucket + 0.5) * TYPE_ROWS_INV, 0, 0));  // .x = 深度桶号, .yzw = 第四关键色
    float4 T15 = tex2Dlod(sType, float4(tuv.x, (ROW_FrameRate + 0.5) * TYPE_ROWS_INV, 0, 0));  // .z/.w = 尺寸硬门(from, portion)
    float4 T16 = tex2Dlod(sType, float4(tuv.x, (ROW_Wobble + 0.5) * TYPE_ROWS_INV, 0, 0));  // 位置摆动(amp min/max, freq, mode)
    float4 T17 = tex2Dlod(sType, float4(tuv.x, (ROW_FlickerEase + 0.5) * TYPE_ROWS_INV, 0, 0));  // 闪烁(period, duty, from)

    bool ovl_hit;
    float3 ovl_col;
    float ovl_scale, ovl_alpha;
    overlay_probe(pl.xy, ovl_hit, ovl_col, ovl_scale, ovl_alpha);

    bool snap = uSys.w > 0.5;
    float t = life > 0.0001 ? clamp(age / life, 0.0, 1.0) : 1.0;
    float2 psize = particle_size(T0, T3, T9, T11, T12, T15, id, age, t, false);
    if (ovl_hit)
        psize *= ovl_scale;
    // 像素对齐取整放在覆盖倍率之后: 倍率乘在类型 size 上, 点采样下覆盖放大的粒子同样整尺寸
    if (snap)
        psize = floor(psize + 0.5);
    float ang = particle_angle(T7, T8, id, age, pl.zw);
    float ca = cos(ang), sa = sin(ang);

    float2 corner = (v.c.xy * 2.0 - 1.0) * psize * 0.5 * particle_flip(T12, id);
    float2 off = float2(ca * corner.x - sa * corner.y, sa * corner.x + ca * corner.y);
    float2 wpos = pl.xy + uSys.xy;
    // 位置摆动: 渲染侧装饰偏移, 不进物理(特效器/覆盖器用原始位置)。振幅/相位由 id
    // 确定性导出; 加在 snap 前使摆动随像素网格量化。mode = axis + 3*unit + 6*grow。
    if (T16.z > 0.0)
    {
        float ax = fmod(T16.w, 3.0);
        float un = fmod(floor(T16.w / 3.0), 2.0);
        float gr = floor(T16.w / 6.0);
        float amp = lerp(T16.x, T16.y, h1(id + 37.0));
        float ph = un < 0.5 ? age / T16.z : t * T16.z;
        float sw = sin(6.283185307179586 * ph + h1(id + 43.7) * 6.283185307179586);
        if (gr > 1.5) amp *= 1.0 - t;
        else if (gr > 0.5) amp *= t;
        float2 wdir;
        if (ax < 0.5) wdir = float2(1.0, 0.0);
        else if (ax < 1.5) wdir = float2(0.0, 1.0);
        else
        {
            float2 vn = pl.zw;
            float vl = length(vn);
            wdir = vl > 0.0001 ? float2(-vn.y, vn.x) / vl : float2(1.0, 0.0);
        }
        wpos += wdir * (sw * amp);
    }
    if (snap)
        wpos = floor(wpos - psize * 0.5 + 0.5) + psize * 0.5;

    float4 clip = mul(uWVP, float4(wpos + off, 0, 1));
    o.pos = dead > 0.5 ? float4(2.0, 2.0, 0.5, 1.0) : clip;   // 全部角点同点，零面积三角形被剔除
    o.cuv = v.c.xy;
    o.tinfo = float2(type, frame);

    float3 col = particle_color(ov, T3, T4, T5, T6, T14, T9.y, t);
    float a = particle_alpha(T3, T6, t, uBlend.x, T2.w, T14.x);
    // 闪烁硬门: 寿命 t 越过 from 后按周期/占空比置零 alpha, 相位由 id 确定性导出
    if (T17.x > 0.0 && t > T17.z && frac(age / T17.x + h1(id + 53.3)) < T17.y)
        a = 0.0;
    // 覆盖色在混合/桶归零(a=0)之后生效: 零贡献乘覆盖 alpha 仍为零, 不会复活
    if (ovl_hit)
    {
        col = ovl_col;
        a *= ovl_alpha;
    }
    o.col = float4(col, a);
    return o;
}
