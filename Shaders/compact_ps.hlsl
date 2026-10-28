// 压缩搬移像素着色器: 新槽位 ← 重映射表指向的旧槽位原样转发, 无映射(-1)写死槽约定。
// 死槽约定各字段与演化着色器的死亡分支逐字段一致, 改一处必须同步另一处。
sampler sPos : register(s0);
sampler sLife : register(s1);
sampler sOvr : register(s2);
sampler sInv : register(s3);    // 逆映射表: .x = 源槽号, -1 = 新槽为死区

float4 uGlobal : register(c0);  // .z = 1/GRID, 与演化着色器同寄存器(复用 EVO_C_GLOBAL 上传)

static const float GRID = 256.0;

struct PS_OUT
{
    float4 c0 : COLOR0;
    float4 c1 : COLOR1;
    float4 c2 : COLOR2;
};

PS_OUT main(float4 vpos : VPOS)
{
    PS_OUT o;
    float2 uv = (vpos.xy + 0.5) * uGlobal.z;
    float src = tex2D(sInv, uv).x;

    if (src < 0.0)
    {
        // 死槽约定: pos=0 / vel=0 / type=0 / base=白 / has_ovr=0 / age=1 / life=0 / frame=0
        o.c0 = float4(0, 0, 0, 0);
        o.c1 = float4(1.0, 0.0, 0.0, 0.0);
        o.c2 = float4(1, 1, 1, 0);
    }
    else
    {
        float2 suv = (float2(fmod(src, GRID), floor(src / GRID)) + 0.5) * uGlobal.z;
        o.c0 = tex2D(sPos, suv);
        o.c1 = tex2D(sLife, suv);
        o.c2 = tex2D(sOvr, suv);
    }
    return o;
}
