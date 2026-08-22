sampler sPos : register(s0);
sampler sLife : register(s1);
sampler sOvr : register(s2);
sampler sType : register(s3);
sampler sEff : register(s4);    // 特效器表 64x6 (每特效器 2 行:
                                // 行0/1=attractor, 行2/3=destroyer, 行4/5=deflector)
float4 uGlobal : register(c0);
float4 uBatchCount : register(c4);
float4 uMode : register(c5);      // .x = 1 → 仅出生不老化(多块演化用)
float4 uEff : register(c6);       // .x=attractor 数, .y=destroyer 数, .z=deflector 数
float4 uBatches[16] : register(c8);
static const float TWO_PI = 6.283185307179586;
static const float DEG2RAD = 0.017453292519943295;
static const float GRID = 256.0;

// 高熵 1D hash: 二次项打破 frac(a*k) 的线性周期(否则相邻 id 方向相关 → 射线)
float h1(float a)
{
    a = frac(a * 0.1031);
    a = frac(a * (a + 19.19));
    a = frac(a * (a + 33.33));
    return frac(a * 43758.5453);
}

float3 h3(float a)
{
    float3 r;
    r.x = h1(a); r.y = h1(a + 57.13); r.z = h1(a + 161.7);
    return r;
}

float3 hsv2rgb(float3 c)
{
    float3 p = abs(frac(c.x + float3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);
    return c.z * lerp(float3(1,1,1), clamp(p - 1.0, 0.0, 1.0), c.y);
}

struct PS_OUT
{
    float4 c0 : COLOR0;
    float4 c1 : COLOR1;
    float4 c2 : COLOR2;
};

// 扫描批次常量, 命中本粒子槽位的批次写入 b0..b3。返回是否命中。
float find_batch(float id, out float4 b0, out float4 b1,
    out float4 b2, out float4 b3)
{
    b0 = 0; b1 = 0; b2 = 0; b3 = 0;
    float seeded = 0.0;

    for (int b = 0; b < 16; ++b)
    {
        if (b >= uBatchCount.x)
            break;

        float4 bb0 = uBatches[b * 4 + 0];
        float hit = (id >= bb0.x && id < bb0.x + bb0.y) ? 1.0 : 0.0;
        if (hit > 0.5 && seeded < 0.5)
        {
            seeded = 1.0;
            b0 = bb0;
            b1 = uBatches[b * 4 + 1];
            b2 = uBatches[b * 4 + 2];
            b3 = uBatches[b * 4 + 3];
        }
    }
    return seeded;
}

// 出生位置四模式: 盒速度(-3)/源槽位(-2)/点(-1)/区域(≥0)。
// 源已死时置 srcdead, 出生分支据此令其出生即死。
float2 spawn_position(float4 b1, float4 b2, float3 rnd, float invGrid,
    out float srcdead)
{
    float2 p;
    srcdead = 0.0;

    if (b2.x < -2.5)
    {
        // 盒速度发射(shape=-3, gpart_particles_create_box): 位置=点,
        // 初速在 b1=(hmin,vmin,hmax,vmax) 盒内两轴独立均匀(GM8 hsp/vsp 语义)
        p = b2.zw;
    }
    else if (b2.x < -1.5)
    {
        // 源槽位生成(step/death): 位置 = 源粒子当前位置(读上一帧状态)。
        // 源已死(GPU 击杀/变形后的僵尸事件)则出生即死, 防 (0,0) 幽灵粒子。
        float src = floor(b2.z + 0.5);
        float2 suv = (float2(fmod(src, 256.0), floor(src / 256.0)) + 0.5) * invGrid;
        p = tex2D(sPos, suv).xy;
        float4 sst = tex2D(sLife, suv);
        srcdead = (sst.y <= 0.0 || sst.x >= sst.y) ? 1.0 : 0.0;
    }
    else if (b2.x < -0.5)
    {
        p = b2.zw;
    }
    else
    {
        float shape = floor(b2.x + 0.5);
        float distr = b2.y;
        float u = rnd.x, v = rnd.y;

        if (distr > 1.5)   // ps_distr_invgaussian: 边缘密集(均匀盘半径)
        {
            float r = sqrt(v);
            float a = TWO_PI * u;
            u = clamp(0.5 + 0.5 * r * cos(a), 0.0, 1.0);
            v = clamp(0.5 + 0.5 * r * sin(a), 0.0, 1.0);
        }
        else if (distr > 0.5)   // ps_distr_gaussian: 中心密集(Box-Muller)
        {
            float r = sqrt(-2.0 * log(max(1.0 - u, 0.0001)));
            float a = TWO_PI * v;
            u = clamp(0.5 + 0.5 * r * cos(a), 0.0, 1.0);
            v = clamp(0.5 + 0.5 * r * sin(a), 0.0, 1.0);
        }

        if (shape < 0.5)
            p = lerp(b1.xy, b1.zw, float2(u, v));
        else if (shape < 1.5)
        {
            float ang = TWO_PI * u;
            float rr = sqrt(v);
            float2 d = float2(cos(ang), sin(ang)) * rr;
            p = 0.5 * (b1.xy + b1.zw) + d * 0.5 * (b1.zw - b1.xy);
        }
        else if (shape < 2.5)
        {
            float uu = u * 2.0 - 1.0, vv = v * 2.0 - 1.0;
            float m = abs(uu) + abs(vv);
            if (m > 1.0) { uu /= m; vv /= m; }
            p = 0.5 * (b1.xy + b1.zw) + float2(uu, vv) * 0.5 * (b1.zw - b1.xy);
        }
        else
            p = lerp(b1.xy, b1.zw, float2(u, u));
    }
    return p;
}

// 出生初速: 盒速度模式直取盒采样, 否则按类型表速度/方向扇区。
float2 spawn_velocity(float4 b1, float4 b2, float4 T1, float3 rnd)
{
    if (b2.x < -2.5)
        return float2(lerp(b1.x, b1.z, rnd.y), lerp(b1.y, b1.w, rnd.z));

    float spd = lerp(T1.x, T1.y, rnd.y);
    float dir = lerp(T1.z, T1.w, rnd.z);
    float rad = dir * DEG2RAD;
    return spd * float2(cos(rad), -sin(rad));
}

// 出生颜色: 覆盖色 / mix / rgb / hsv 随机 / 单色。
float3 spawn_color(float4 b3, float4 T3, float4 T4, float4 T5,
    float id, float seed, out float has_ovr)
{
    has_ovr = b3.w;
    if (b3.w > 0.5)
        return b3.rgb;

    if (T3.z > 3.5)
    {
        float cr = h1(id + seed * 17.0 + 31.7);   // 颜色独立随机(与方向/速度解耦)
        if (T3.z < 4.5)
            return lerp(T4.rgb, float3(T4.w, T5.x, T5.y), cr);
        if (T3.z < 5.5)
            return lerp(T4.rgb, T5.rgb, cr);

        float3 hsv = lerp(T4.rgb, T5.rgb, cr);
        return hsv2rgb(hsv * float3(1.0/255.0, 1.0/255.0, 1.0/255.0));
    }
    return T4.rgb;
}

// 出生帧号: 随机起始帧——静态型全帧域(批次种子); 动画型前半段帧域
// (确定性 id hash, 存活分支用同款 hash 复现, 无需状态存储)。
float spawn_frame(float4 T8, float4 T9, float id, float seed)
{
    float nf = T8.y;
    if (T9.x <= 0.5 || nf <= 1.0)
        return 0.0;
    return (T8.z > 0.5)
        ? floor(h1(id + 9.7) * 0.5 * nf)
        : floor(h1(id + seed * 17.0 + 9.0) * nf);
}

// attractor 力(引擎 sub_4BDA50): 距离 ≤dist 内加力; kind 0=恒定 1=线性 2=二次衰减;
// additive=true 叠加到速度, false 只做位置修正(acc_pos)。最多 4 个(ps_3_0 展开预算)。
void apply_attractors(float2 pos, inout float2 vel, inout float2 acc_pos,
    float count, float dt)
{
    for (int aa = 0; aa < 4; ++aa)
    {
        if (aa >= count)
            break;

        float2 ac = tex2D(sEff, float2((aa + 0.5) / 64.0, 0.5 / 6.0)).xy;
        float2 af = tex2D(sEff, float2((aa + 0.5) / 64.0, 0.5 / 6.0)).zw;
        float4 as = tex2D(sEff, float2((aa + 0.5) / 64.0, 1.5 / 6.0));
        if (as.x < 0.5)
            continue;

        float2 d = ac - pos;
        float dist = length(d);
        if (dist <= af.y && dist > 0.0 && af.x != 0.0 && af.y != 0.0)
        {
            float2 f = af.x * d / dist;
            if (as.y > 0.5 && as.y < 1.5)
            {
                float k = (af.y - dist) / af.y;
                f *= k;
            }
            else if (as.y > 1.5)
            {
                float k = (af.y - dist) / af.y;
                f *= k * k;
            }

            if (as.z > 0.5)
                vel += f;
            else
                acc_pos += f;
        }
    }
}

// 速度摆动(每步随机 ±wiggle 当步位移抖动, 不进速度状态) + 方向摆动三角波 +
// 方向增量旋转 + 位置积分。
void integrate_motion(inout float2 pos, inout float2 vel, float4 T13,
    float dt, float id, float age, float2 acc_pos)
{
    float swing = (h1(id + floor(age) * 7.31) * 2.0 - 1.0) * T13.w * dt;
    float dw = fmod(h1(id + 23.0) * 24.0 + age, 24.0) / 6.0;
    dw = dw > 2.0 ? 4.0 - dw : dw;
    float da = T13.x * dt + (dw - 1.0) * T13.y;
    da *= DEG2RAD;
    float ca2 = cos(da), sa2 = sin(da);
    vel = float2(vel.x * ca2 + vel.y * sa2, -vel.x * sa2 + vel.y * ca2);
    pos += vel * dt + (vel / max(length(vel), 0.0001)) * swing + acc_pos;
}

// deflector: 区域内方向反射 + 位置镜像 + friction 减速。
// kind==1 horizontal(偏转水平速度): direction=180-dir → vel.x 取反 + x 镜像;
// kind!=1 vertical(偏转垂直速度): direction=360-dir → vel.y 取反 + y 镜像。
void apply_deflectors(float2 prev, inout float2 pos, inout float2 vel, float count)
{
    for (int de = 0; de < 4; ++de)
    {
        if (de >= count)
            break;

        float4 dr = tex2D(sEff, float2((de + 0.5) / 64.0, 4.5 / 6.0));
        float4 ds = tex2D(sEff, float2((de + 0.5) / 64.0, 5.5 / 6.0));
        if (ds.x < 0.5)
            continue;

        if (dr.x >= dr.y || dr.z >= dr.w)
            continue;

        if (pos.x >= dr.x && pos.x <= dr.y && pos.y >= dr.z && pos.y <= dr.w)
        {
            float cl = length(vel);
            if (ds.y > 0.5)
            {
                vel.x = -vel.x;
                pos.x = prev.x - (pos.x - prev.x);
            }
            else
            {
                vel.y = -vel.y;
                pos.y = prev.y - (pos.y - prev.y);
            }

            float nl = max(cl - ds.z, 0.0);
            if (cl > 0.0001)
                vel *= nl / cl;
        }
    }
}

// destroyer: 区域内(rect/ellipse/diamond)命中检测。免疫类型(T11.z)不命中。
float destroyer_killhit(float2 pos, float type, float count)
{
    float4 T11 = tex2D(sType, float2((type + 0.5) / 256.0, 11.5 / 14.0));

    for (int de2 = 0; de2 < 4; ++de2)
    {
        if (de2 >= count)
            break;

        float4 dr2 = tex2D(sEff, float2((de2 + 0.5) / 64.0, 2.5 / 6.0));
        float4 ds2 = tex2D(sEff, float2((de2 + 0.5) / 64.0, 3.5 / 6.0));
        if (ds2.x < 0.5)
            continue;

        if (dr2.x >= dr2.y || dr2.z >= dr2.w)
            continue;

        if (pos.x >= dr2.x && pos.x <= dr2.y && pos.y >= dr2.z && pos.y <= dr2.w)
        {
            float nx = 2.0 * (pos.x - (dr2.y + dr2.x) * 0.5) / (dr2.y - dr2.x);
            float ny = 2.0 * (pos.y - (dr2.w + dr2.z) * 0.5) / (dr2.w - dr2.z);
            float hit = ds2.y < 0.5 ? 1.0
                : (ds2.y > 0.5 && ds2.y < 1.5) ? (nx * nx + ny * ny <= 1.0 ? 1.0 : 0.0)
                : (abs(nx) + abs(ny) <= 1.0 ? 1.0 : 0.0);

            if (hit > 0.5 && T11.z < 0.5)
                return 1.0;
        }
    }
    return 0.0;
}

// destroyer 命中后的处理: 有 death 配置的类型当场变形为 death_type 粒子(GPU 侧
// part_type_death, 零 CPU 回读): 复用本槽位, 重按 death_type 初始化 age/life/vel,
// 并按新类型重算颜色(旧烘焙色/覆盖色不得继承)。death_number > 1 时只变形 1 个,
// 为 GPU 无回读语义的近似; 负值 = 概率模式。无配置或概率未中 → 直接销毁。
void apply_morph(inout float type, inout float age, inout float life,
    inout float2 vel, inout float3 base, inout float has_ovr,
    float killhit, float id)
{
    if (killhit < 0.5)
        return;

    float4 T10 = tex2D(sType, float2((type + 0.5) / 256.0, 10.5 / 14.0));
    float dn = T10.z, dtype = T10.w;
    if (dn != 0.0 && dtype >= 1.0 && dtype < 256.0
        && (dn > 0.0 || h1(id + 41.7) < 1.0 / -dn))
    {
        type = dtype;
        float4 TD0 = tex2D(sType, float2((type + 0.5) / 256.0, 0.5 / 14.0));
        float4 TD1 = tex2D(sType, float2((type + 0.5) / 256.0, 1.5 / 14.0));
        float rr2 = h1(id + 71.3);
        float rr3 = h1(id + 91.7);
        age = 0.0;
        life = lerp(TD0.x, TD0.y, rr2);
        float spd2 = lerp(TD1.x, TD1.y, rr2);
        float dir2 = lerp(TD1.z, TD1.w, rr3);
        float rad2 = dir2 * DEG2RAD;
        vel = spd2 * float2(cos(rad2), -sin(rad2));

        // 按新类型重算颜色: 颜色随机与变形概率(id+41.7)、寿命/速度(id+71.3/91.7)
        // 用不同偏移解耦。
        float4 TM3 = tex2D(sType, float2((type + 0.5) / 256.0, 3.5 / 14.0));
        float4 TM4 = tex2D(sType, float2((type + 0.5) / 256.0, 4.5 / 14.0));
        float4 TM5 = tex2D(sType, float2((type + 0.5) / 256.0, 5.5 / 14.0));
        float crm = h1(id + 61.3);

        if (TM3.z > 3.5)
        {
            if (TM3.z < 4.5)
                base = lerp(TM4.rgb, float3(TM4.w, TM5.x, TM5.y), crm);
            else if (TM3.z < 5.5)
                base = lerp(TM4.rgb, TM5.rgb, crm);
            else
            {
                base = hsv2rgb(lerp(TM4.rgb, TM5.rgb, crm) *
                    float3(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0));
            }
        }
        else
            base = TM4.rgb;

        has_ovr = 0.0;
    }
    else
    {
        // 无 death 配置(或概率未中): 直接销毁(age>=life 剔除)
        age = 1.0; life = 0.0;
    }
}

// 动画帧推进: stretch=按寿命比例播完一遍; 否则 fmod 循环。
// 随机起始帧(动画型): 与出生分支同款确定性 id hash 复现起始偏移, 无需状态存储。
float advance_frame(float st_frame, float type, float age, float life,
    float id, float killhit)
{
    float4 T8 = tex2D(sType, float2((type + 0.5) / 256.0, 8.5 / 14.0));
    float4 T9 = tex2D(sType, float2((type + 0.5) / 256.0, 9.5 / 14.0));
    float nf = T8.y;
    float frame = st_frame;

    if (T8.z > 0.5 && nf > 1.0)
    {
        float startF = (T9.x > 0.5) ? floor(h1(id + 9.7) * 0.5 * nf) : 0.0;
        frame = T8.w > 0.5
            ? min(nf - 1.0, startF + floor(age / max(life, 0.0001) * (nf - startF)))
            : fmod(startF + age, nf);
    }

    if (killhit > 0.5)
        frame = 0.0;   // 变形粒子从第 0 帧开始(非动画类型防旧帧越界)
    return frame;
}

// 存活粒子的单步演化: 老化/自然死亡清理/重力拖拽/速度增量/attractor/
// 摆动积分/deflector/destroyer 命中与变形/动画帧推进。
void evolve_alive(inout float2 pos, inout float2 vel, inout float type,
    inout float age, inout float life, inout float3 base, inout float has_ovr,
    inout float frame, float4 prev, float4 st, float4 ov, float id)
{
    float dt = uMode.x > 0.5 ? 0.0 : uGlobal.y;   // 仅出生 pass: 不推进物理/老化
    float nage = st.x + dt;

    if (nage >= st.y)
    {
        // 自然死亡: 立即清空(防僵尸粒子继续积分飞远, 污染 step/death 源槽读取)
        pos = 0; vel = 0; type = 0; base = float3(1,1,1); has_ovr = 0;
        age = 1.0; life = 0.0; frame = 0.0;
        return;
    }

    pos = prev.xy;
    vel = prev.zw;
    age = nage;
    life = st.y;
    type = st.z;
    base = ov.rgb;
    has_ovr = ov.w;

    float4 T2 = tex2D(sType, float2((type + 0.5) / 256.0, 2.5 / 14.0));
    float4 T13 = tex2D(sType, float2((type + 0.5) / 256.0, 13.5 / 14.0));

    // 重力 + 线性拖拽 + 速度增量(clamp≥0)
    float ga = T2.x * DEG2RAD;
    vel += T2.y * dt * float2(cos(ga), -sin(ga));
    vel *= max(1.0 - clamp(T2.z, 0.0, 1.0) * dt, 0.0);
    float len = max(length(vel) + T13.z * dt, 0.0);
    vel = vel * (len / max(length(vel), 0.0001));

    float2 acc_pos = 0;
    apply_attractors(pos, vel, acc_pos, uEff.x, dt);
    integrate_motion(pos, vel, T13, dt, id, age, acc_pos);
    apply_deflectors(prev.xy, pos, vel, uEff.z);

    float killhit = destroyer_killhit(pos, type, uEff.y);
    apply_morph(type, age, life, vel, base, has_ovr, killhit, id);
    frame = advance_frame(st.w, type, age, life, id, killhit);
}

PS_OUT main(float4 vpos : VPOS)
{
    PS_OUT o;
    float id = vpos.x + vpos.y * GRID;
    float2 uv = (vpos.xy + 0.5) * uGlobal.z;
    float4 prev = tex2D(sPos, uv);
    float4 st = tex2D(sLife, uv);
    float4 ov = tex2D(sOvr, uv);

    float4 b0, b1, b2, b3;
    float seeded = find_batch(id, b0, b1, b2, b3);

    float dead = (id >= uGlobal.w) ? 1.0 : 0.0;
    // 显式初始化: FXC 的数据流分析看不穿 evolve_alive 的 inout 全路径写入
    float age = 0, life = 0, type = 0;
    float2 pos = 0, vel = 0;
    float3 base = 0;
    float has_ovr = 0;
    float frame = 0;
    float srcdead = 0.0;

    if (seeded > 0.5 && dead < 0.5)
    {
        // ---- 出生: 位置/初速/寿命/颜色/起始帧 ----
        type = b0.z;
        float seed = b0.w;
        float3 rnd = h3(id + seed * 17.0);
        pos = spawn_position(b1, b2, rnd, uGlobal.z, srcdead);
        float4 T0 = tex2D(sType, float2((type + 0.5) / 256.0, 0.5 / 14.0));
        float4 T1 = tex2D(sType, float2((type + 0.5) / 256.0, 1.5 / 14.0));
        vel = spawn_velocity(b1, b2, T1, rnd);
        age = 0.0;
        life = lerp(T0.x, T0.y, rnd.x);
        if (srcdead > 0.5)
            life = 0.0;   // 源槽位已死: 出生即死(不渲染, 槽位自然回收)
        base = spawn_color(b3,
            tex2D(sType, float2((type + 0.5) / 256.0, 3.5 / 14.0)),
            tex2D(sType, float2((type + 0.5) / 256.0, 4.5 / 14.0)),
            tex2D(sType, float2((type + 0.5) / 256.0, 5.5 / 14.0)),
            id, seed, has_ovr);
        frame = spawn_frame(
            tex2D(sType, float2((type + 0.5) / 256.0, 8.5 / 14.0)),
            tex2D(sType, float2((type + 0.5) / 256.0, 9.5 / 14.0)),
            id, seed);
    }
    else if (dead < 0.5)
    {
        evolve_alive(pos, vel, type, age, life, base, has_ovr,
            frame, prev, st, ov, id);
    }
    else
    {
        pos = 0; vel = 0; type = 0; base = float3(1,1,1); has_ovr = 0;
        age = 1.0; life = 0.0; frame = 0.0;
    }

    o.c0 = float4(pos, vel);
    o.c1 = float4(age, life, type, frame);
    o.c2 = float4(base, has_ovr);
    return o;
}

