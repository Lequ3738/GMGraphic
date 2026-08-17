#include "gpart.h"
#include "vertex.h"
#include "../Librarys/math_s.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <map>

static const float GP_PI = 3.14159265358979323846f;
// GP_FMT_16F / GP_FMT_32F 是 D3D9 专属格式常量(113/116), d3d8.h 里没有; gpart 仅 D3D9 运行。
static const DWORD GP_FMT_16F = 113;   // D3DFMT_A16B16G16R16F (PS 采样通用, VS/VTF 仅部分硬件支持)
static const DWORD GP_FMT_32F = 116;   // D3DFMT_A32B32G32R32F (VTF 顶点纹理采样标准支持)
// D3D9 顶点纹理采样槽位: VS 采样器 s0..s3 = D3DVERTEXTEXTURESAMPLER0..3 = 257..260,
// 与 PS 的 stage 0..N 是两套独立槽位 — SetTexture(stage) 直通, VS 采样必须绑到 257+。
static const DWORD GP_VTS0 = 257;      // D3DVERTEXTEXTURESAMPLER0

// ============================================================================
// gpart_* GPU particle system (DX9 only)
//
// Stateful GPU simulation:
//   - Particle state lives in three A16B16G16R16F render-target textures
//     (256x256 grid = 65536 slots), ping-ponged each step:
//       tex[0] : pos.xy, vel.xy
//       tex[1] : age, life, type, frame
//       tex[2] : base color.rgb (mix/rgb/hsv/override), has_override
//   - gpart_system_update(): one 3-MRT fullscreen pass per spawn-batch chunk
//     integrates gravity/drag/motion, initialises newly spawned slots and
//     computes the animation frame per particle.
//   - gpart_system_drawit(): STATIC vertex buffer (0..capacity-1, built once),
//     dead particles are skipped by the rasterizer via PSIZE=0. One or two
//     point-sprite draws (ring arcs for draw order). The VS reads particle
//     state via VTF (vs_3_0 tex2Dlod); the PS fetches the atlas rect from a
//     rect-table texture keyed by (type, frame). Zero per-frame CPU work.
//   - A mixed-blend system falls back to a per-frame assembled id VB.
//
// Semantics follow GM8's native particle API (step-based, no dt: one call to
// gpart_system_update() advances every system by exactly one step).
// ============================================================================

// ---- internal shader constant registers ----
static const int EVO_C_GLOBAL = 0;    // (now, dt, invGrid, capacity)
static const int EVO_C_BATCHN = 4;    // (.x = batch count)
static const int EVO_C_BATCHES = 8;   // 16 batches * 4 float4
static const int RND_C_WVP = 0;       // float4x4 (c0..c3)
static const int RND_C_SYS = 4;       // (sys_x, sys_y, invGrid, capacity)
static const int RND_C_BLEND = 5;     // (当前遍混合模式: 0=普通, 1=加法)

static const int GP_TYPE_ROWS = GP_TYPE_TEX_H;   // 类型表纹理行数(与 gpart.h 一致, 10)
static const int GP_QUAD_VERTS = 4;

// ============================================================================
// 确定性 hash(CPU/GPU 同公式, 无超越函数): frac(43758.5453 * frac(x * 0.1031))
// ============================================================================
static float gphashf(float a)
{
    float x = a * 0.1031f;
    x -= floorf(x);
    x *= 43758.5453f;
    x -= floorf(x);
    return x;
}

// float -> half (A16B16G16R16F 编码, 类型表 CPU 侧)
static unsigned short f2h(float f)
{
    unsigned int x;
    memcpy(&x, &f, 4);
    unsigned int sign = (x >> 16) & 0x8000u;
    unsigned int exp = (x >> 23) & 0xffu;
    unsigned int mant = x & 0x7fffffu;
    if (exp == 0xffu) { return (unsigned short)(sign | 0x7c00u | (mant ? 0x200u : 0)); }
    int e = (int)exp - 127 + 15;
    if (e >= 31) return (unsigned short)(sign | 0x7c00u);
    if (e <= 0)
    {
        if (e < -10) return (unsigned short)sign;
        mant |= 0x800000u;
        unsigned int shift = (unsigned int)(14 - e);
        unsigned int h = mant >> shift;
        if (mant & (1u << (shift - 1))) h++;   // 就近舍入
        return (unsigned short)(sign | h);
    }
    return (unsigned short)(sign | ((unsigned int)e << 10) | (mant >> 13));
}

// ============================================================================
// GPU 资源指针(前向声明, 供 GType 内联成员函数使用; 定义见下文)
// ============================================================================
static void* g_rect_tex = nullptr;         // 矩形表纹理 256x32 A16B16G16R16F

// ============================================================================
// 类型
// ============================================================================
struct GType
{
    struct FRect { float u0, v0, u1, v1; };

    float t[GP_TYPE_TEX_H][4] = {};   // 与类型表纹理行一一对应(行 9 = random_frame 标志)
    std::vector<FRect> frame_rect;    // 精灵各帧在粒子图集中的矩形(CPU 侧记录)
    int shape = PT_SHAPE_PIXEL;       // 无精灵时的形状
    bool animat = false, stretch = false, random_frame = false;

    // 形状矩形(内置形状烘焙在固定网格): (u0,v0,u1,v1)
    FRect shape_rect() const
    {
        int s = (shape >= 0 && shape < PT_SHAPE_COUNT) ? shape : (int)PT_SHAPE_PIXEL;
        float x = (float)((s % 16) * GP_ATLAS_TILE) / (float)GP_ATLAS_SIZE;
        float y = (float)((s / 16) * GP_ATLAS_TILE) / (float)GP_ATLAS_SIZE;
        float t = (float)GP_ATLAS_TILE / (float)GP_ATLAS_SIZE;
        return { x, y, t, t };
    }

    // 把该类型全部帧的矩形上传到矩形表纹理的第 type 列
    void upload_rect_table(int type_id) const
    {
        if (!g_rect_tex) return;
        std::vector<unsigned short> px((size_t)GP_RECT_TEX_FRAMES * 4, 0);
        auto put = [&](int frame, FRect r) {
            if (frame < 0 || frame >= GP_RECT_TEX_FRAMES) return;
            float v[4] = { r.u0, r.v0, r.u1, r.v1 };
            for (int c = 0; c < 4; ++c)
                px[((size_t)frame * 4) + c] = f2h(v[c]);
        };
        if (!frame_rect.empty())
        {
            for (int k = 0; k < (int)frame_rect.size(); ++k)
                put(k, frame_rect[k]);
        }
        else
            put(0, shape_rect());
        D3DCheck(d3d::upload_texture_rect(g_rect_tex, (UINT)type_id, 0, 1, GP_RECT_TEX_FRAMES,
            GP_FMT_16F, px.data(), 4 * 2), 1);
    }
};

static std::unordered_map<int, GType> g_types;
static int g_type_counter = 1;

// ============================================================================
// 发射批次 / 发射器 / 系统
// ============================================================================
struct SpawnBatch
{
    float start = 0, count = 0, type = 0, seed = 0;
    float xmin = 0, ymin = 0, xmax = 0, ymax = 0;
    float shape = 0, distr = 0, px = 0, py = 0;   // shape < 0 = 点发射(px,py)
    float ovr_r = 1, ovr_g = 1, ovr_b = 1, has_ovr = 0;
};

struct GEmitter
{
    float xmin = -8, xmax = 8, ymin = -8, ymax = 8;
    int shape = PS_SHAPE_RECTANGLE;
    int distr = PS_DISTR_LINEAR;
    int stream_type = -1;      // 流式配置: 每步自动发射的类型(-1 = 未启用)
    float stream_rate = 0;     // 流式配置: 每步数量
};

// 由发射器区域模板构造 SpawnBatch(burst/stream/update 共用)
static SpawnBatch emitter_batch(const GEmitter& g)
{
    SpawnBatch b;
    b.shape = (float)g.shape;
    b.distr = (float)g.distr;
    b.xmin = g.xmin; b.ymin = g.ymin; b.xmax = g.xmax; b.ymax = g.ymax;
    return b;
}

struct LiveEntry { int slot; float birth; };   // 活跃窗口项(带出生快照, 防槽复用冲突)

struct GSystem
{
    int capacity = 4096;
    bool old_to_new = true;
    float pos_x = 0, pos_y = 0;
    void* tex[3][2] = {};             // [kind][ping]; 0=pos/vel 1=age/life 2=color
    void* surf[3][2] = {};
    int cur = 0;
    float now = 0;                    // 系统时钟(步)
    int cursor = 0;
    std::vector<float> s_birth, s_life;
    std::vector<int> s_type, s_frame;
    std::vector<SpawnBatch> pending;
    std::unordered_map<int, GEmitter> emitters;
    int em_counter = 1;
    // 活跃窗口: 只遍历可能存活的槽(发射入列, 惰性剔除), O(活跃) 而非 O(capacity)
    std::vector<LiveEntry> live_window;
    // 静态 id VB: 创建时填 0..capacity-1, 永不重建(死粒子由 PSIZE=0 跳过)
    void* id_vb = nullptr;
    // 混合路径用: 每帧组装的 float5 数据(id + 图集矩形)与上传 VB
    void* mix_vb = nullptr;
    std::vector<float> mix_data;
    size_t n_normal = 0, n_total = 0;
    bool mix_dirty = true;
    // 系统内已使用过的混合类型掩码: 位 0=普通, 位 1=加法(静态路径判定)
    int blend_mask = 0;
};

static std::unordered_map<int, GSystem> g_systems;
static int g_system_counter = 1;

// ============================================================================
// GPU 资源(全局, 惰性创建)
// ============================================================================
static void* g_type_tex = nullptr;         // 类型表纹理 256x10 A16B16G16R16F
static void* g_atlas_tex = nullptr;        // 粒子图集 1024x1024 A8R8G8B8(形状+精灵帧)
static int g_atlas_x = 0, g_atlas_y = 0, g_atlas_row_h = 0;   // 图集 shelf 分配器
static void* g_quad_vb = nullptr;          // 全屏四边形(剪辑空间, 4 顶点)
static void* g_id_decl = nullptr;          // 渲染 pass 顶点声明(float1 id)
static void* g_quad_decl = nullptr;
static dword g_evo_vs = 0, g_evo_ps = 0;
static dword g_rnd_vs = 0, g_rnd_ps = 0;
static bool g_gpu_ready = false;
static bool g_gpu_failed = false;

// 图集分配(shelf): 同高度放一行, 行满换行; 失败返回 false
static bool atlas_alloc(int w, int h, int& x, int& y)
{
    if (g_atlas_x + w > GP_ATLAS_SIZE)
    {
        g_atlas_x = 0;
        g_atlas_y += g_atlas_row_h;
        g_atlas_row_h = 0;
    }
    if (g_atlas_y + h > GP_ATLAS_SIZE) return false;
    x = g_atlas_x;
    y = g_atlas_y;
    g_atlas_x += w;
    g_atlas_row_h = std::max(g_atlas_row_h, h);
    return true;
}

// ============================================================================
// 内嵌 HLSL
// ============================================================================
static const char* EVO_VS_HLSL =
    "struct VSIN { float4 pos : POSITION; };\n"
    "struct VSOUT { float4 pos : POSITION; };\n"
    "VSOUT main(VSIN v) { VSOUT o; o.pos = v.pos; return o; }\n";

static const char* EVO_PS_HLSL =
    "sampler sPos : register(s0);\n"
    "sampler sLife : register(s1);\n"
    "sampler sOvr : register(s2);\n"
    "sampler sType : register(s3);\n"
    "float4 uGlobal : register(c0);\n"
    "float4 uBatchCount : register(c4);\n"
    "float4 uBatches[64] : register(c8);\n"
    "static const float TWO_PI = 6.283185307179586;\n"
    "static const float DEG2RAD = 0.017453292519943295;\n"
    "static const float GRID = 256.0;\n"
    "float h1(float a) { return frac(43758.5453 * frac(a * 0.1031)); }\n"
    "float3 h3(float a) {\n"
    "  float3 r;\n"
    "  r.x = h1(a); r.y = h1(a + 57.13); r.z = h1(a + 161.7);\n"
    "  return r;\n"
    "}\n"
    "float3 hsv2rgb(float3 c) {\n"
    "  float3 p = abs(frac(c.x + float3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);\n"
    "  return c.z * lerp(float3(1,1,1), clamp(p - 1.0, 0.0, 1.0), c.y);\n"
    "}\n"
    "struct PS_OUT { float4 c0 : COLOR0; float4 c1 : COLOR1; float4 c2 : COLOR2; };\n"
    "PS_OUT main(float4 vpos : VPOS) {\n"
    "  PS_OUT o;\n"
    "  float id = vpos.x + vpos.y * GRID;\n"
    "  float2 uv = (vpos.xy + 0.5) * uGlobal.z;\n"
    "  float4 prev = tex2D(sPos, uv);\n"
    "  float4 st = tex2D(sLife, uv);\n"
    "  float4 ov = tex2D(sOvr, uv);\n"
    "  float4 b0 = 0; float4 b1 = 0; float4 b2 = 0; float4 b3 = 0;\n"
    "  float seeded = 0.0;\n"
    "  for (int b = 0; b < 16; ++b) {\n"
    "    if (b >= uBatchCount.x) break;\n"
    "    float4 bb0 = uBatches[b * 4 + 0];\n"
    "    float hit = (id >= bb0.x && id < bb0.x + bb0.y) ? 1.0 : 0.0;\n"
    "    if (hit > 0.5 && seeded < 0.5) {\n"
    "      seeded = 1.0;\n"
    "      b0 = bb0; b1 = uBatches[b*4+1]; b2 = uBatches[b*4+2]; b3 = uBatches[b*4+3];\n"
    "    }\n"
    "  }\n"
    "  float dead = (id >= uGlobal.w) ? 1.0 : 0.0;\n"
    "  float age, life, type;\n"
    "  float2 pos, vel;\n"
    "  float3 base;\n"
    "  float has_ovr;\n"
    "  float frame;\n"
    "  if (seeded > 0.5 && dead < 0.5) {\n"
    "    type = b0.z;\n"
    "    float seed = b0.w;\n"
    "    float3 rnd = h3(id + seed * 17.0);\n"
    "    float2 p;\n"
    "    if (b2.x < -0.5) {\n"
    "      p = b2.zw;\n"
    "    } else {\n"
    "      float shape = floor(b2.x + 0.5);\n"
    "      float distr = b2.y;\n"
    "      float u = rnd.x, v = rnd.y;\n"
    "      if (distr > 1.5) {\n"
    "        // ps_distr_invgaussian: 边缘密集(均匀盘半径)\n"
    "        float r = sqrt(v);\n"
    "        float a = TWO_PI * u;\n"
    "        u = clamp(0.5 + 0.5 * r * cos(a), 0.0, 1.0);\n"
    "        v = clamp(0.5 + 0.5 * r * sin(a), 0.0, 1.0);\n"
    "      } else if (distr > 0.5) {\n"
    "        // ps_distr_gaussian: 中心密集(Box-Muller)\n"
    "        float r = sqrt(-2.0 * log(max(1.0 - u, 0.0001)));\n"
    "        float a = TWO_PI * v;\n"
    "        u = clamp(0.5 + 0.5 * r * cos(a), 0.0, 1.0);\n"
    "        v = clamp(0.5 + 0.5 * r * sin(a), 0.0, 1.0);\n"
    "      }\n"
    "      if (shape < 0.5) { p = lerp(b1.xy, b1.zw, float2(u, v)); }\n"
    "      else if (shape < 1.5) {\n"
    "        float ang = TWO_PI * u;\n"
    "        float rr = sqrt(v);\n"
    "        float2 d = float2(cos(ang), sin(ang)) * rr;\n"
    "        p = 0.5 * (b1.xy + b1.zw) + d * 0.5 * (b1.zw - b1.xy);\n"
    "      } else if (shape < 2.5) {\n"
    "        float uu = u * 2.0 - 1.0, vv = v * 2.0 - 1.0;\n"
    "        float m = abs(uu) + abs(vv);\n"
    "        if (m > 1.0) { uu /= m; vv /= m; }\n"
    "        p = 0.5 * (b1.xy + b1.zw) + float2(uu, vv) * 0.5 * (b1.zw - b1.xy);\n"
    "      } else { p = lerp(b1.xy, b1.zw, float2(u, u)); }\n"
    "    }\n"
    "    pos = p;\n"
    "    float4 T0 = tex2D(sType, float2((type + 0.5) / 256.0, 0.5 / 10.0));\n"
    "    float4 T1 = tex2D(sType, float2((type + 0.5) / 256.0, 1.5 / 10.0));\n"
    "    float4 T3 = tex2D(sType, float2((type + 0.5) / 256.0, 3.5 / 10.0));\n"
    "    float4 T4 = tex2D(sType, float2((type + 0.5) / 256.0, 4.5 / 10.0));\n"
    "    float4 T5 = tex2D(sType, float2((type + 0.5) / 256.0, 5.5 / 10.0));\n"
    "    float spd = lerp(T1.x, T1.y, rnd.y);\n"
    "    float dir = lerp(T1.z, T1.w, rnd.z);\n"
    "    float rad = dir * DEG2RAD;\n"
    "    vel = spd * float2(cos(rad), -sin(rad));\n"
    "    age = 0.0;\n"
    "    life = lerp(T0.x, T0.y, rnd.x);\n"
    "    if (b3.w > 0.5) { base = b3.rgb; }\n"
    "    else if (T3.z > 3.5) {\n"
    "      if (T3.z < 4.5) { base = lerp(T4.rgb, float3(T4.w, T5.x, T5.y), rnd.z); }\n"
    "      else if (T3.z < 5.5) { base = lerp(T4.rgb, T5.rgb, rnd.z); }\n"
    "      else {\n"
    "        float3 hsv = lerp(T4.rgb, T5.rgb, rnd.z);\n"
    "        base = hsv2rgb(hsv * float3(360.0/255.0, 1.0/255.0, 1.0/255.0));\n"
    "      }\n"
    "    } else { base = T4.rgb; }\n"
    "    has_ovr = b3.w;\n"
    "    float4 T8 = tex2D(sType, float2((type + 0.5) / 256.0, 8.5 / 10.0));\n"
    "    float4 T9 = tex2D(sType, float2((type + 0.5) / 256.0, 9.5 / 10.0));\n"
    "    float nf = T8.y;\n"
    "    frame = (T9.x > 0.5 && nf > 1.0) ? floor(h1(id + seed * 17.0 + 9.0) * nf) : 0.0;\n"
    "  } else if (dead < 0.5) {\n"
    "    pos = prev.xy;\n"
    "    vel = prev.zw;\n"
    "    age = st.x;\n"
    "    life = st.y;\n"
    "    type = st.z;\n"
    "    base = ov.rgb;\n"
    "    has_ovr = ov.w;\n"
    "    float dt = uGlobal.y;\n"
    "    float4 T2 = tex2D(sType, float2((type + 0.5) / 256.0, 2.5 / 10.0));\n"
    "    float g = T2.y;\n"
    "    float ga = T2.x * DEG2RAD;\n"
    "    vel += g * dt * float2(cos(ga), -sin(ga));\n"
    "    vel *= max(1.0 - clamp(T2.z, 0.0, 1.0) * dt, 0.0);\n"
    "    pos += vel * dt;\n"
    "    age += dt;\n"
    "    float4 T8 = tex2D(sType, float2((type + 0.5) / 256.0, 8.5 / 10.0));\n"
    "    float nf = T8.y;\n"
    "    frame = st.w;\n"
    "    if (T8.z > 0.5 && nf > 1.0)\n"
    "      frame = T8.w > 0.5\n"
    "        ? min(nf - 1.0, floor(age / max(life, 0.0001) * nf))\n"
    "        : fmod(age, nf);\n"
    "  } else {\n"
    "    pos = 0; vel = 0; type = 0; base = float3(1,1,1); has_ovr = 0;\n"
    "    age = 1.0; life = 0.0; frame = 0.0;\n"
    "  }\n"
    "  o.c0 = float4(pos, vel);\n"
    "  o.c1 = float4(age, life, type, frame);\n"
    "  o.c2 = float4(base, has_ovr);\n"
    "  return o;\n"
    "}\n";

static const char* RND_VS_HLSL =
    // 四边形渲染(对齐 GMParty/GMS2 架构): 每粒子 6 顶点(2 三角形),
    // 角点位置/旋转在 VS 里算, UV 与颜色插值给 PS — 完全不依赖点精灵光栅化。
    "sampler sOvr : register(s0);\n"
    "sampler sPos : register(s1);\n"
    "sampler sLife : register(s2);\n"
    "sampler sType : register(s3);\n"
    "float4x4 uWVP : register(c0);\n"
    "float4 uSys : register(c4);\n"
    "float4 uBlend : register(c5);\n"
    "struct VSIN { float3 c : TEXCOORD0; };   // c.xy = 角点{0,1}, c.z = 粒子 id\n"
    "struct VSOUT {\n"
    "  float4 pos : POSITION;\n"
    "  float2 cuv : TEXCOORD0;    // 角点 uv(插值 = 粒子内 0..1)\n"
    "  float2 tinfo : TEXCOORD1;  // type, frame\n"
    "  float4 col : COLOR0;       // rgb = 颜色, a = alpha\n"
    "};\n"
    "float h1(float a) { return frac(43758.5453 * frac(a * 0.1031)); }\n"
    "VSOUT main(VSIN v) {\n"
    "  VSOUT o;\n"
    "  float id = v.c.z;\n"
    "  float2 uv = (float2(fmod(id, 256.0), floor(id / 256.0)) + 0.5) * uSys.z;\n"
    "  float4 pl = tex2Dlod(sPos, float4(uv, 0, 0));\n"
    "  float4 st = tex2Dlod(sLife, float4(uv, 0, 0));\n"
    "  float age = st.x, life = st.y;\n"
    "  float type = st.z;\n"
    "  float frame = st.w;\n"
    "  float dead = (age >= life) ? 1.0 : 0.0;\n"
    "  float2 tuv = float2((type + 0.5) / 256.0, 0.0);\n"
    "  float4 T0 = tex2Dlod(sType, float4(tuv.x, 0.5 / 10.0, 0, 0));\n"
    "  float4 T2 = tex2Dlod(sType, float4(tuv.x, 2.5 / 10.0, 0, 0));\n"
    "  float4 T3 = tex2Dlod(sType, float4(tuv.x, 3.5 / 10.0, 0, 0));\n"
    "  float4 T4 = tex2Dlod(sType, float4(tuv.x, 4.5 / 10.0, 0, 0));\n"
    "  float4 T5 = tex2Dlod(sType, float4(tuv.x, 5.5 / 10.0, 0, 0));\n"
    "  float4 T6 = tex2Dlod(sType, float4(tuv.x, 6.5 / 10.0, 0, 0));\n"
    "  float4 T7 = tex2Dlod(sType, float4(tuv.x, 7.5 / 10.0, 0, 0));\n"
    "  float4 T8 = tex2Dlod(sType, float4(tuv.x, 8.5 / 10.0, 0, 0));\n"
    "  float4 ov = tex2Dlod(sOvr, float4(uv, 0, 0));\n"
    "  float size = lerp(T0.z, T0.w, h1(id + 3.0));\n"
    "  float psize = size * T3.x;\n"
    "  float ang = lerp(T7.x, T7.y, h1(id + 5.0)) + T7.z * age;\n"
    "  ang += (h1(id + floor(age) * 7.31) - 0.5) * 2.0 * T7.w;\n"
    "  if (T8.x > 0.5) ang += atan2(-pl.w, pl.z) * 57.29577951308232;\n"
    "  ang *= 0.017453292519943295;\n"
    "  float ca = cos(ang), sa = sin(ang);\n"
    "  float2 corner = (v.c.xy * 2.0 - 1.0) * psize * 0.5;\n"
    "  float2 off = float2(ca * corner.x - sa * corner.y, sa * corner.x + ca * corner.y);\n"
    "  float4 clip = mul(uWVP, float4(pl.xy + uSys.xy + off, 0, 1));\n"
    "  o.pos = dead > 0.5 ? float4(2.0, 2.0, 0.5, 1.0) : clip;   // 死亡: 全部角点同点 → 零面积三角形被剔除\n"
    "  o.cuv = v.c.xy;\n"
    "  o.tinfo = float2(type, frame);\n"
    "  float t = life > 0.0001 ? clamp(age / life, 0.0, 1.0) : 1.0;\n"
    "  float3 col;\n"
    "  float mode = T3.z;\n"
    "  if (ov.w > 0.5 || mode > 3.5) col = ov.rgb;\n"
    "  else if (mode < 1.5) col = T4.rgb;\n"
    "  else if (mode < 2.5) col = lerp(T4.rgb, float3(T4.w, T5.x, T5.y), t);\n"
    "  else col = t < 0.5 ? lerp(T4.rgb, float3(T4.w, T5.x, T5.y), t * 2.0)\n"
    "                    : lerp(float3(T4.w, T5.x, T5.y), float3(T5.z, T5.w, T6.x), (t - 0.5) * 2.0);\n"
    "  float a;\n"
    "  float am = T3.w;\n"
    "  if (am < 1.5) a = T6.y;\n"
    "  else if (am < 2.5) a = lerp(T6.y, T6.z, t);\n"
    "  else a = t < 0.5 ? lerp(T6.y, T6.z, t * 2.0) : lerp(T6.z, T6.w, (t - 0.5) * 2.0);\n"
    "  if (abs(T2.w - uBlend.x) > 0.5) a = 0.0;   // 混合模式不匹配当前遍 → 零贡献\n"
    "  o.col = float4(col, a);\n"
    "  return o;\n"
    "}\n";

static const char* RND_PS_HLSL =
    "sampler sMain : register(s0);\n"
    "sampler sRect : register(s5);\n"
    "float4 main(float2 cuv : TEXCOORD0, float2 tinfo : TEXCOORD1, float4 col : COLOR0) : COLOR0 {\n"
    "  float4 rect = tex2D(sRect, float2((tinfo.x + 0.5) / 256.0, (tinfo.y + 0.5) / 32.0));\n"
    "  float2 auv = rect.xy + rect.zw * cuv;\n"
    "  float4 tex = tex2D(sMain, auv);\n"
    "  float a = tex.a * col.a;\n"
    "  return float4(tex.rgb * col.rgb * a, a);\n"
    "}\n";

// ============================================================================
// 形状纹理生成(64x64, 直通 alpha, 白 RGB)
// ============================================================================
static void shape_fill_circle(BYTE* px, int size, int cx, int cy, int r, bool soft)
{
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            float d = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy)));
            float a = (float)r - d;
            if (soft) a = std::clamp(a + 1.0f, 0.0f, 2.0f) * 0.5f;
            else a = a >= 0.0f ? 1.0f : 0.0f;
            if (a > 0.0f) px[(y * size + x) * 4 + 3] = (BYTE)(a * 255.0f);
        }
}
static void shape_fill_ring(BYTE* px, int size, int cx, int cy, int r, int thick)
{
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            float d = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy)));
            if (d >= (float)(r - thick) && d <= (float)r)
                px[(y * size + x) * 4 + 3] = 255;
        }
}
static bool in_star(float x, float y, float cx, float cy, float R, float r)
{
    float dx = x - cx, dy = y - cy;
    float ang = atan2f(dy, dx) + GP_PI / 2.0f;
    while (ang < 0) ang += 6.2831853f;
    int seg = (int)(ang / 0.62831853f);   // 36°
    float base = (float)seg * 0.62831853f;
    float t = (ang - base) / 0.62831853f;
    float rad = (seg & 1) ? (R - (R - r) * t) : (r + (R - r) * t);
    return (dx * dx + dy * dy) <= rad * rad;
}
static void gen_shape_tex(int shape, BYTE* px, int size)
{
    memset(px, 0, (size_t)size * size * 4);
    int c = size / 2;
    switch (shape)
    {
    case PT_SHAPE_PIXEL:
        // 白点(此前只写 alpha 导致 RGB=0, 画出来是黑的)
        memset(&px[(c * size + c) * 4], 255, 4);
        break;
        case PT_SHAPE_DISK:
            shape_fill_circle(px, size, c, c, size / 2 - 1, false);
            break;
        case PT_SHAPE_SQUARE:
            for (int y = 2; y < size - 2; ++y)
                for (int x = 2; x < size - 2; ++x)
                    px[(y * size + x) * 4 + 3] = 255;
            break;
        case PT_SHAPE_LINE:
            for (int y = c - 3; y <= c + 3; ++y)
                for (int x = 0; x < size; ++x)
                {
                    float e = std::clamp((float)(x < size / 4 ? x : size - 1 - x) / (float)(size / 4), 0.0f, 1.0f);
                    px[(y * size + x) * 4 + 3] = (BYTE)(e * 255.0f);
                }
            break;
        case PT_SHAPE_STAR:
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    if (in_star((float)x, (float)y, (float)c, (float)c, (float)(size / 2 - 2), (float)(size / 5)))
                        px[(y * size + x) * 4 + 3] = 255;
            break;
        case PT_SHAPE_CIRCLE:
            shape_fill_ring(px, size, c, c, size / 2 - 2, 5);
            break;
        case PT_SHAPE_RING:
            shape_fill_ring(px, size, c, c, size / 2 - 2, 2);
            break;
        case PT_SHAPE_SPHERE:
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    float d2 = (float)((x - c) * (x - c) + (y - c) * (y - c));
                    float r2 = (float)((size / 2 - 1) * (size / 2 - 1));
                    if (d2 > r2) continue;
                    float a = 1.0f - sqrtf(d2 / r2);
                    px[(y * size + x) * 4 + 3] = (BYTE)((0.3f + 0.7f * a) * 255.0f);
                }
            break;
        case PT_SHAPE_FLARE:
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    float dx = (float)(x - c) / (float)c, dy = (float)(y - c) / (float)c;
                    float a = std::max(1.0f - sqrtf(dx * dx), 0.0f) * std::max(1.0f - sqrtf(dy * dy), 0.0f);
                    px[(y * size + x) * 4 + 3] = (BYTE)(std::clamp(a, 0.0f, 1.0f) * 255.0f);
                }
            break;
        case PT_SHAPE_SPARK:
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    float d = fabsf((float)(x - 2) * 0.6f - (float)(y - 2) * 0.8f) / 8.0f;
                    float a = std::max(1.0f - d, 0.0f);
                    px[(y * size + x) * 4 + 3] = (BYTE)(std::clamp(a, 0.0f, 1.0f) * 255.0f);
                }
            break;
        case PT_SHAPE_EXPLOSION:
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    float d = sqrtf((float)((x - c) * (x - c) + (y - c) * (y - c)));
                    float n = gphashf((float)(x * 3 + y * 7));
                    float a = std::clamp((float)(size / 2) - d + n * 3.0f - 1.0f, 0.0f, 1.0f);
                    px[(y * size + x) * 4 + 3] = (BYTE)(a * 255.0f);
                }
            break;
        case PT_SHAPE_CLOUD:
            shape_fill_circle(px, size, c, c, size / 2 - 4, true);
            shape_fill_circle(px, size, c - 10, c + 6, size / 5, true);
            shape_fill_circle(px, size, c + 12, c + 8, size / 6, true);
            break;
        case PT_SHAPE_SMOKE:
            shape_fill_circle(px, size, c, c, size / 2, true);
            break;
        case PT_SHAPE_SNOW:
            for (int k = 0; k < 6; ++k)
            {
                float a = (float)k * 1.04719755f;
                float cx2 = cosf(a) * (float)(size / 4), cy2 = sinf(a) * (float)(size / 4);
                shape_fill_ring(px, size, c + (int)cx2, c + (int)cy2, size / 6, 2);
            }
            shape_fill_circle(px, size, c, c, size / 6, false);
            break;
        default:
            shape_fill_circle(px, size, c, c, size / 2 - 1, false);
            break;
    }
    // 关键: 形状只写了 alpha 通道, RGB 恒 0 = 透明黑。
    // 这里给所有 alpha>0 的像素补白(RGB=255), 否则粒子画出来全是黑的。
    for (int i = 0; i < size * size; ++i)
        if (px[i * 4 + 3] != 0)
            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 255;
}

// ============================================================================
// GPU 资源初始化(惰性, 失败置 g_gpu_failed)
// ============================================================================
// 调试日志(C++ 侧): 写入 OutputDebugString + gpart_debug.log
// ============================================================================
static void gpart_log(const std::string& msg)
{
    OutputDebugStringA(("gpart: " + msg + "\n").c_str());
    FILE* f = fopen("gpart_debug.log", "a");
    if (f) { fputs((msg + "\n").c_str(), f); fclose(f); }
}
// ============================================================================
// ============================================================================
static void probe_point_size()
{
    try
    {
        dword s_vs = 0, s_ps = 0, s_fvf = 0, s_psen = 0, s_pssc = 0, s_pmin = 0, s_pmax = 0, s_psz = 0;
        void* s_decl = nullptr, * s_rt = nullptr;
        UINT s_vw = 0, s_vh = 0;
        d3d::get_vertex_shader(&s_vs);
        d3d::get_pixel_shader(&s_ps);
        d3d::get_fvf(&s_fvf);
        d3d::get_vertex_declaration(&s_decl);
        d3d::get_render_target(0, &s_rt);
        d3d::get_viewport(&s_vw, &s_vh);
        d3d::get_render_state(D3DRS_POINTSPRITEENABLE, &s_psen);
        d3d::get_render_state(D3DRS_POINTSCALEENABLE, &s_pssc);
        d3d::get_render_state(D3DRS_POINTSIZE_MIN, &s_pmin);
        d3d::get_render_state(D3DRS_POINTSIZE_MAX, &s_pmax);
        d3d::get_render_state(D3DRS_POINTSIZE, &s_psz);

        void* ptex = nullptr, * psurf = nullptr;
        D3DCheck(d3d::create_texture(128, 128, 1, D3DUSAGE_RENDERTARGET, GP_FMT_16F,
            D3DPOOL_DEFAULT, &ptex), 1);
        D3DCheck(d3d::get_surface_level(ptex, 0, &psurf), 2);
        D3DCheck(d3d::set_render_target(0, psurf), 3);
        D3DCheck(d3d::set_viewport(128, 128), 4);
        D3DCheck(d3d::clear_target(0), 5);
        D3DCheck(d3d::set_render_state(D3DRS_ALPHABLENDENABLE, FALSE), 6);
        D3DCheck(d3d::set_render_state(D3DRS_ZENABLE, FALSE), 7);
        D3DCheck(d3d::set_render_state(D3DRS_POINTSPRITEENABLE, TRUE), 8);
        D3DCheck(d3d::set_render_state(D3DRS_POINTSCALEENABLE, FALSE), 9);
        float zmin = 0.0f, zmax = 256.0f;
        D3DCheck(d3d::set_render_state(D3DRS_POINTSIZE_MIN, d3dvar(zmin)), 10);
        D3DCheck(d3d::set_render_state(D3DRS_POINTSIZE_MAX, d3dvar(zmax)), 11);

        std::vector<BYTE> code;
        std::string err;
        void* table = nullptr;
        static const char* PVS =
            "struct VSIN { float4 pos : POSITION; };\n"
            "struct VSOUT { float4 pos : POSITION; float psize : PSIZE; };\n"
            "VSOUT main(VSIN v) { VSOUT o; o.pos = v.pos; o.psize = 16; return o; }\n";
        static const char* PPS =
            "float4 main() : COLOR0 { return float4(1, 1, 1, 1); }\n";
        D3DCheck(d3d::compile_hlsl(PVS, strlen(PVS), "main", "vs_3_0", code, &table, &err), 12);
        if (table) d3d::release(table);
        dword pvs = 0;
        D3DCheck(d3d::create_vertex_shader(d3d::VERT_DEFAULT, code.data(), nullptr, 0, &pvs), 13);
        code.clear();
        D3DCheck(d3d::compile_hlsl(PPS, strlen(PPS), "main", "ps_3_0", code, &table, &err), 14);
        if (table) d3d::release(table);
        dword pps = 0;
        D3DCheck(d3d::create_pixel_shader(code.data(), &pps), 15);

        D3DCheck(d3d::set_vertex_declaration(g_quad_decl), 16);
        D3DCheck(d3d::set_vertex_shader_handle(pvs), 17);
        D3DCheck(d3d::set_pixel_shader(pps), 18);
        // 两个点: 视口中心 + 左上角(位置依赖检测)
        float pts[8] = { 0, 0, 0, 1,  -0.5f, -0.5f, 0, 1 };
        void* pvb = nullptr;
        D3DCheck(d3d::create_vertex_buffer(sizeof(pts), &pvb), 19);
        D3DCheck(d3d::upload_vertex_buffer(pvb, pts, sizeof(pts)), 20);
        D3DCheck(d3d::set_stream_source(0, pvb, 16), 21);
        D3DCheck(d3d::draw_primitive(D3DPT_POINTLIST, 2, 0), 22);

        std::vector<float> rb;
        UINT rw = 0, rh = 0;
        if (SUCCEEDED(d3d::read_texture_float(ptex, rb, rw, rh)) && rw == 128 && rh == 128)
        {
            auto bbox = [&](int cx, int cy) {
                int x0 = 999, x1 = -1, y0 = 999, y1 = -1;
                for (int y = cy - 26; y <= cy + 26; ++y)
                    for (int x = cx - 26; x <= cx + 26; ++x)
                    {
                        if (x < 0 || y < 0 || x >= 128 || y >= 128) continue;
                        if (rb[((size_t)y * 128 + x) * 4] > 0.5f)
                        {
                            if (x < x0) x0 = x;
                            if (x > x1) x1 = x;
                            if (y < y0) y0 = y;
                            if (y > y1) y1 = y;
                        }
                    }
                char buf[160];
                sprintf(buf, "gpart probe_psize at(%d,%d) bbox=(%d..%d, %d..%d) size=(%d x %d)",
                    cx, cy, x0, x1, y0, y1, x1 - x0 + 1, y1 - y0 + 1);
                gpart_log(buf);
            };
            bbox(64, 64);   // 视口中心点
            bbox(32, 32);   // 角点(NDC -0.5,-0.5)
        }
        else gpart_log("gpart probe_psize = READ_FAILED");

        d3d::set_render_target(0, s_rt);
        if (s_rt) d3d::release(s_rt);
        d3d::set_viewport(s_vw, s_vh);
        d3d::set_render_state(D3DRS_POINTSPRITEENABLE, s_psen);
        d3d::set_render_state(D3DRS_POINTSCALEENABLE, s_pssc);
        d3d::set_render_state(D3DRS_POINTSIZE_MIN, s_pmin);
        d3d::set_render_state(D3DRS_POINTSIZE_MAX, s_pmax);
        d3d::set_render_state(D3DRS_POINTSIZE, s_psz);
        d3d::set_vertex_shader_handle(s_vs);
        d3d::set_pixel_shader(s_ps);
        d3d::set_vertex_declaration(s_decl);
        d3d::set_fvf(s_fvf);
        d3d::set_stream_source(0, nullptr, 0);
        if (pvb) d3d::release(pvb);
        if (pvs) d3d::delete_vertex_shader(pvs);
        if (pps) d3d::delete_pixel_shader(pps);
        if (psurf) d3d::release(psurf);
        if (ptex) d3d::release(ptex);
    }
    catch (const std::exception&)
    {
        gpart_log("gpart probe_psize = FAILED");
    }
}

// ============================================================================
static bool gpart_gpu_init()
{
    if (g_gpu_ready || g_gpu_failed) return g_gpu_ready;
    try
    {
        d3d::Caps caps;
        if (!d3d::get_caps(caps))
            throw std::runtime_error("无法读取设备能力。");
        if (caps.vertex_tex_filter_caps == 0)
            throw std::runtime_error("显卡不支持顶点纹理采样(VTF), gpart 不可用。");

        // 设备顶点处理模式与 VTF 能力日志(诊断 VTF 失效用)。
        // SWVP(软件顶点处理)下 texldl 不可用 —— GMDirectX9 补丁把 runner 的
        // 0x22(SWVP|FPU) 改 0x42(HWVP), 失败会回退 SWVP。
        {
            BOOL swvp = FALSE;
            if (SUCCEEDED(d3d::get_software_vertex_processing(&swvp)))
                gpart_log(std::string("device software_vertex_processing = ") + (swvp ? "TRUE (VTF broken)" : "FALSE (hardware VP)"));
            else
                gpart_log("device software_vertex_processing = UNKNOWN");
            char buf[160];
            sprintf(buf, "caps: vs=0x%X ps=0x%X vtf_caps=0x%X max_point_size=%.1f",
                caps.vertex_shader_version, caps.pixel_shader_version,
                caps.vertex_tex_filter_caps, caps.max_point_size);
            gpart_log(buf);
        }

        // 类型表纹理 256x10 A16B16G16R16F
        D3DCheck(d3d::create_texture(GP_TYPE_TEX_W, GP_TYPE_ROWS, 1, 0,
            GP_FMT_16F, D3DPOOL_DEFAULT, &g_type_tex), 1);
        std::vector<unsigned short> zero((size_t)GP_TYPE_TEX_W * GP_TYPE_ROWS * 4, 0);
        D3DCheck(d3d::upload_texture(g_type_tex, GP_TYPE_TEX_W, GP_TYPE_ROWS,
            GP_FMT_16F, zero.data(), GP_TYPE_TEX_W * 8), 2);
        // VTF 哨兵: 类型表 texel (0,0) 预填 0.5 —— 探针采样它, 读到 (0.5,0.5,0.5,0.5)
        // 即证明 VTF 正常(与图集内容/部署无关)。
        {
            std::vector<unsigned short> sent(4);
            sent[0] = f2h(0.5f); sent[1] = f2h(0.5f); sent[2] = f2h(0.5f); sent[3] = f2h(0.5f);
            D3DCheck(d3d::upload_texture_rect(g_type_tex, 0, 0, 1, 1, GP_FMT_16F,
                sent.data(), 4 * 2), 3);
        }

        // 粒子图集 1024x1024 A8R8G8B8: 先烘焙 14 个内置形状(固定网格 64x64)
        D3DCheck(d3d::create_texture(GP_ATLAS_SIZE, GP_ATLAS_SIZE, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_atlas_tex), 3);
        for (int s = 0; s < PT_SHAPE_COUNT; ++s)
        {
            std::vector<BYTE> px((size_t)GP_ATLAS_TILE * GP_ATLAS_TILE * 4, 0);
            gen_shape_tex(s, px.data(), GP_ATLAS_TILE);
            int ax = (s % 16) * GP_ATLAS_TILE, ay = (s / 16) * GP_ATLAS_TILE;
            D3DCheck(d3d::upload_texture_rect(g_atlas_tex, ax, ay,
                GP_ATLAS_TILE, GP_ATLAS_TILE, D3DFMT_A8R8G8B8, px.data(), GP_ATLAS_TILE * 4), 4);
        }
        g_atlas_x = 0;
        g_atlas_y = GP_ATLAS_TILE;   // 形状占满第 0 行, 精灵从第 1 行开始分配
        g_atlas_row_h = 0;

        // 全屏四边形(剪辑空间 TRIANGLESTRIP)
        static const float quad[GP_QUAD_VERTS * 4] = {
            -1, -1, 0, 1,   1, -1, 0, 1,   -1, 1, 0, 1,   1, 1, 0, 1
        };
        D3DCheck(d3d::create_vertex_buffer(sizeof(quad), &g_quad_vb), 5);
        D3DCheck(d3d::upload_vertex_buffer(g_quad_vb, quad, sizeof(quad)), 6);

        // 顶点声明
        vertex_element quad_e[] = { { 0, 0, VT_FLOAT4, 0, VU_POSITION, 0 } };
        D3DCheck(d3d::create_vertex_declaration(quad_e, 1, &g_quad_decl), 7);
        vertex_element id_e[] = {
            { 0, 0, VT_FLOAT3, 0, VU_TEXCOORD, 0 },   // 角点x, 角点y, 粒子 id
        };
        D3DCheck(d3d::create_vertex_declaration(id_e, 1, &g_id_decl), 8);

        // 矩形表纹理 256x32 A16B16G16R16F (类型 x 帧 → 图集矩形)
        D3DCheck(d3d::create_texture(GP_TYPE_TEX_W, GP_RECT_TEX_FRAMES, 1, 0,
            GP_FMT_16F, D3DPOOL_DEFAULT, &g_rect_tex), 9);
        {
            std::vector<unsigned short> z((size_t)GP_TYPE_TEX_W * GP_RECT_TEX_FRAMES * 4, 0);
            D3DCheck(d3d::upload_texture(g_rect_tex, GP_TYPE_TEX_W, GP_RECT_TEX_FRAMES,
                GP_FMT_16F, z.data(), GP_TYPE_TEX_W * 8), 10);
        }

        // shader 编译
        auto compile = [](const char* src, const char* entry, const char* profile,
            std::vector<BYTE>& code) {
                std::string err;
                void* table = nullptr;
                HRESULT hr = d3d::compile_hlsl(src, strlen(src), entry, profile, code, &table, &err);
                if (table) d3d::release(table);
                if (FAILED(hr))
                    throw std::runtime_error("gpart shader 编译失败 (" + std::string(entry) + "): " + err);
            };
        std::vector<BYTE> code;
        compile(EVO_VS_HLSL, "main", "vs_3_0", code);
        D3DCheck(d3d::create_vertex_shader(d3d::VERT_DEFAULT, code.data(), nullptr, 0, &g_evo_vs), 9);
        compile(EVO_PS_HLSL, "main", "ps_3_0", code);
        D3DCheck(d3d::create_pixel_shader(code.data(), &g_evo_ps), 10);
        compile(RND_VS_HLSL, "main", "vs_3_0", code);
        D3DCheck(d3d::create_vertex_shader(d3d::VERT_DEFAULT, code.data(), nullptr, 0, &g_rnd_vs), 11);
        compile(RND_PS_HLSL, "main", "ps_3_0", code);
        D3DCheck(d3d::create_pixel_shader(code.data(), &g_rnd_ps), 12);

        // ---- VTF/渲染链路自检(双测试, 完整保存/恢复引擎状态) ----
        // 控制组: PS 输出恒定色, VS 不采样 → 验证"清RT→绘制→16F读回→h2f"整条链。
        // VTF 组: VS tex2Dlod 采样已知白点(disk 形状 tile 1 中心) → 验证顶点纹理采样。
        auto probe_run = [&](const char* vs_src, const char* ps_src, void* bindtex, DWORD bindstage, float out[4]) -> bool {
            std::vector<BYTE> pcode;
            std::string perr;
            void* ptable = nullptr;
            HRESULT phr = d3d::compile_hlsl(vs_src, strlen(vs_src), "main", "vs_3_0",
                pcode, &ptable, &perr);
            if (ptable) d3d::release(ptable);
            dword pvs = 0, pps = 0;
            if (SUCCEEDED(phr))
                phr = d3d::create_vertex_shader(d3d::VERT_DEFAULT, pcode.data(), nullptr, 0, &pvs);
            if (SUCCEEDED(phr))
            {
                pcode.clear();
                phr = d3d::compile_hlsl(ps_src, strlen(ps_src), "main", "ps_3_0",
                    pcode, &ptable, &perr);
                if (ptable) d3d::release(ptable);
            }
            if (SUCCEEDED(phr))
                phr = d3d::create_pixel_shader(pcode.data(), &pps);
            if (FAILED(phr)) { if (pvs) d3d::delete_vertex_shader(pvs); return false; }

            // 保存引擎状态
            dword s_vs = 0, s_ps = 0, s_fvf = 0, s_blend = 0, s_z = 0;
            void* s_decl = nullptr, * s_rt = nullptr, * s_tex0 = nullptr, * s_vtex0 = nullptr;
            UINT s_vw = 0, s_vh = 0;
            d3d::get_vertex_shader(&s_vs);
            d3d::get_pixel_shader(&s_ps);
            d3d::get_fvf(&s_fvf);
            d3d::get_vertex_declaration(&s_decl);
            d3d::get_render_target(0, &s_rt);
            d3d::get_render_state(D3DRS_ALPHABLENDENABLE, &s_blend);
            d3d::get_render_state(D3DRS_ZENABLE, &s_z);
            d3d::get_viewport(&s_vw, &s_vh);
            d3d::get_texture(0, &s_tex0);
            d3d::get_texture(GP_VTS0, &s_vtex0);

            void* ptex = nullptr, * psurf = nullptr;
            phr = d3d::create_texture(1, 1, 1, D3DUSAGE_RENDERTARGET, GP_FMT_16F,
                D3DPOOL_DEFAULT, &ptex);
            if (SUCCEEDED(phr)) phr = d3d::get_surface_level(ptex, 0, &psurf);
            if (SUCCEEDED(phr)) phr = d3d::set_render_target(0, psurf);
            if (SUCCEEDED(phr)) phr = d3d::set_viewport(1, 1);
            if (SUCCEEDED(phr)) phr = d3d::set_render_state(D3DRS_ALPHABLENDENABLE, FALSE);
            if (SUCCEEDED(phr)) phr = d3d::set_render_state(D3DRS_ZENABLE, FALSE);
            if (SUCCEEDED(phr)) phr = d3d::clear_target(0);
            if (SUCCEEDED(phr)) phr = d3d::set_vertex_declaration(g_quad_decl);
            if (SUCCEEDED(phr)) phr = d3d::set_vertex_shader_handle(pvs);
            if (SUCCEEDED(phr)) phr = d3d::set_pixel_shader(pps);
            if (SUCCEEDED(phr)) phr = d3d::set_stream_source(0, g_quad_vb, 16);
            if (SUCCEEDED(phr)) phr = d3d::set_texture(bindstage, bindtex);
            if (SUCCEEDED(phr)) phr = d3d::set_tex_stage_state(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
            if (SUCCEEDED(phr)) phr = d3d::set_tex_stage_state(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
            if (SUCCEEDED(phr)) phr = d3d::set_tex_stage_state(bindstage, D3DTSS_MINFILTER, D3DTEXF_POINT);
            if (SUCCEEDED(phr)) phr = d3d::set_tex_stage_state(bindstage, D3DTSS_MAGFILTER, D3DTEXF_POINT);
            if (SUCCEEDED(phr)) phr = d3d::draw_primitive(D3DPT_TRIANGLESTRIP, 2, 0);
            if (SUCCEEDED(phr))
            {
                std::vector<float> rb;
                UINT rw = 0, rh = 0;
                phr = d3d::read_texture_float(ptex, rb, rw, rh);
                if (SUCCEEDED(phr) && rb.size() >= 4)
                { out[0]=rb[0]; out[1]=rb[1]; out[2]=rb[2]; out[3]=rb[3]; }
            }

            // 恢复引擎状态
            d3d::set_render_target(0, s_rt);
            if (s_rt) d3d::release(s_rt);
            d3d::set_render_target(1, nullptr);
            d3d::set_render_target(2, nullptr);
            d3d::set_viewport(s_vw, s_vh);
            d3d::set_render_state(D3DRS_ALPHABLENDENABLE, s_blend);
            d3d::set_render_state(D3DRS_ZENABLE, s_z);
            d3d::set_vertex_shader_handle(s_vs);
            d3d::set_pixel_shader(s_ps);
            d3d::set_vertex_declaration(s_decl);
            d3d::set_fvf(s_fvf);
            d3d::set_stream_source(0, nullptr, 0);
            d3d::set_texture(0, s_tex0);
            if (s_tex0) d3d::release(s_tex0);
            d3d::set_texture(GP_VTS0, s_vtex0);
            if (s_vtex0) d3d::release(s_vtex0);
            if (psurf) d3d::release(psurf);
            if (ptex) d3d::release(ptex);
            if (pvs) d3d::delete_vertex_shader(pvs);
            if (pps) d3d::delete_pixel_shader(pps);
            return true;
        };

        // 控制组: PS 恒定色 (1,0,1,1), VS 不采样
        {
            static const char* C_VS =
                "struct VSIN { float4 pos : POSITION; };\n"
                "struct VSOUT { float4 pos : POSITION; float4 col : COLOR0; };\n"
                "VSOUT main(VSIN v) { VSOUT o; o.pos = v.pos; o.col = float4(1,0,1,1); return o; }\n";
            static const char* C_PS =
                "float4 main(float4 col : COLOR0) : COLOR0 { return col; }\n";
            float v[4] = { -1,-1,-1,-1 };
            if (probe_run(C_VS, C_PS, g_atlas_tex, 0, v))
            {
                char buf[160];
                sprintf(buf, "gpart probe_ctrl = (%.3f, %.3f, %.3f, %.3f)", v[0], v[1], v[2], v[3]);
                gpart_log(buf);
            }
            else gpart_log("gpart probe_ctrl = FAILED");
        }
        // VTF 组 1: VS tex2Dlod 采样 disk 形状(tile 1)中心 = 白色(内容相关)
        {
            static const char* V_VS =
                "sampler sProbe : register(s0);\n"
                "struct VSIN { float4 pos : POSITION; };\n"
                "struct VSOUT { float4 pos : POSITION; float4 col : COLOR0; };\n"
                "VSOUT main(VSIN v) {\n"
                "  VSOUT o; o.pos = v.pos;\n"
                "  o.col = tex2Dlod(sProbe, float4(96.5 / 1024.0, 32.5 / 1024.0, 0, 0));\n"
                "  return o;\n"
                "}\n";
            static const char* V_PS =
                "float4 main(float4 col : COLOR0) : COLOR0 { return col; }\n";
            float v[4] = { -1,-1,-1,-1 };
            if (probe_run(V_VS, V_PS, g_atlas_tex, GP_VTS0, v))
            {
                char buf[160];
                sprintf(buf, "gpart probe_vtf_atlas = (%.3f, %.3f, %.3f, %.3f)", v[0], v[1], v[2], v[3]);
                gpart_log(buf);
            }
            else gpart_log("gpart probe_vtf_atlas = FAILED");
        }
        // VTF 组 2: VS 采样类型表哨兵 texel (0,0) = 0.5(内容无关, 决定性)
        {
            static const char* V_VS =
                "sampler sProbe : register(s0);\n"
                "struct VSIN { float4 pos : POSITION; };\n"
                "struct VSOUT { float4 pos : POSITION; float4 col : COLOR0; };\n"
                "VSOUT main(VSIN v) {\n"
                "  VSOUT o; o.pos = v.pos;\n"
                "  o.col = tex2Dlod(sProbe, float4(0.5 / 256.0, 0.5 / 10.0, 0, 0));\n"
                "  return o;\n"
                "}\n";
            static const char* V_PS =
                "float4 main(float4 col : COLOR0) : COLOR0 { return col; }\n";
            float v[4] = { -1,-1,-1,-1 };
            if (probe_run(V_VS, V_PS, g_type_tex, GP_VTS0, v))
            {
                char buf[160];
                sprintf(buf, "gpart probe_vtf_sentinel = (%.3f, %.3f, %.3f, %.3f)", v[0], v[1], v[2], v[3]);
                gpart_log(buf);
                if (v[0] < 0.49f && v[1] < 0.49f && v[2] < 0.49f)
                    gpart_log("WARNING: VTF sentinel probe wrong - render VS state sampling broken");
            }
            else gpart_log("gpart probe_vtf_sentinel = FAILED");
        }
        // 驱动声明的 VTF 格式支持(CheckDeviceFormat + D3DUSAGE_QUERY_VERTEXTEXTURE)
        {
            struct FmtName { DWORD fmt; const char* name; };
            static const FmtName fmts[] = {
                { 21,  "A8R8G8B8" },     // D3DFMT_A8R8G8B8
                { GP_FMT_16F, "A16B16G16R16F" },
                { 114, "R32F" },         // D3DFMT_R32F
                { GP_FMT_32F, "A32B32G32R32F" },
            };
            char buf[256];
            int off = sprintf(buf, "gpart vtf_formats: ");
            for (int i = 0; i < 4; ++i)
                off += sprintf(buf + off, "%s=%s ", fmts[i].name,
                    d3d::check_vtf_format(fmts[i].fmt) ? "OK" : "no");
            gpart_log(buf);
        }
        // VTF 决定性组: 1x1 A32B32G32R32F 哨兵(预填 0.5), VS 采样 → 读到 0.5 即 VTF 真可用
        {
            void* p32 = nullptr;
            float sent[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
            if (SUCCEEDED(d3d::create_texture(1, 1, 1, 0, GP_FMT_32F, D3DPOOL_DEFAULT, &p32)))
            {
                if (FAILED(d3d::upload_texture_rect(p32, 0, 0, 1, 1, GP_FMT_32F,
                    sent, (UINT)sizeof(sent))))
                {
                    gpart_log("gpart probe_vtf_32f_sentinel = UPLOAD_FAILED");
                }
                else
                {
                    static const char* V_VS =
                        "sampler sProbe : register(s0);\n"
                        "struct VSIN { float4 pos : POSITION; };\n"
                        "struct VSOUT { float4 pos : POSITION; float4 col : COLOR0; };\n"
                        "VSOUT main(VSIN v) {\n"
                        "  VSOUT o; o.pos = v.pos;\n"
                        "  o.col = tex2Dlod(sProbe, float4(0.5, 0.5, 0, 0));\n"
                        "  return o;\n"
                        "}\n";
                    static const char* V_PS =
                        "float4 main(float4 col : COLOR0) : COLOR0 { return col; }\n";
                    float v[4] = { -1,-1,-1,-1 };
                    if (probe_run(V_VS, V_PS, p32, GP_VTS0, v))
                    {
                        char buf[160];
                        sprintf(buf, "gpart probe_vtf_32f_sentinel = (%.3f, %.3f, %.3f, %.3f)",
                            v[0], v[1], v[2], v[3]);
                        gpart_log(buf);
                        if (v[0] < 0.49f)
                            gpart_log("WARNING: 32F VTF sentinel wrong - VTF truly unavailable");
                    }
                    else gpart_log("gpart probe_vtf_32f_sentinel = FAILED");
                }
                d3d::release(p32);
            }
            else gpart_log("gpart probe_vtf_32f_sentinel = CREATE_FAILED");
        }


        g_gpu_ready = true;
    }
    catch (const std::exception& e)
    {
        gm::show_error(std::string("gpart 初始化失败: ") + e.what(), false);
        g_gpu_failed = true;
    }
    return g_gpu_ready;
}

// 类型表 → 纹理上传(整表, 调用方在改完类型后触发)
static void type_table_upload()
{
    std::vector<unsigned short> px((size_t)GP_TYPE_TEX_W * GP_TYPE_ROWS * 4, 0);
    for (auto& kv : g_types)
    {
        int id = kv.first;
        if (id < 1 || id >= GP_TYPE_TEX_W) continue;
        const GType& gt = kv.second;
        for (int r = 0; r < GP_TYPE_ROWS; ++r)
            for (int c = 0; c < 4; ++c)
                px[((size_t)r * GP_TYPE_TEX_W + id) * 4 + c] = f2h(gt.t[r][c]);
    }
    if (g_type_tex)
        D3DCheck(d3d::upload_texture(g_type_tex, GP_TYPE_TEX_W, GP_TYPE_ROWS,
            GP_FMT_16F, px.data(), GP_TYPE_TEX_W * 8), 1);
}

// ============================================================================
// 系统状态纹理创建/销毁
// ============================================================================
static void system_tex_create(GSystem& s)
{
    for (int k = 0; k < 3; ++k)
        for (int p = 0; p < 2; ++p)
        {
            D3DCheck(d3d::create_texture(GP_GRID, GP_GRID, 1, D3DUSAGE_RENDERTARGET,
                GP_FMT_16F, D3DPOOL_DEFAULT, &s.tex[k][p]), 1);
            D3DCheck(d3d::get_surface_level(s.tex[k][p], 0, &s.surf[k][p]), 2);
        }
    // 清零: 全 0 = age 0 / life 0 → 死亡
    void* prevRT = nullptr;
    D3DCheck(d3d::get_render_target(0, &prevRT), 3);
    for (int p = 0; p < 2; ++p)
    {
        D3DCheck(d3d::set_render_target(0, s.surf[0][p]), 4);
        D3DCheck(d3d::set_render_target(1, s.surf[1][p]), 5);
        D3DCheck(d3d::set_render_target(2, s.surf[2][p]), 6);
        D3DCheck(d3d::set_viewport(GP_GRID, GP_GRID), 7);
        D3DCheck(d3d::clear_target(0), 8);
    }
    D3DCheck(d3d::set_render_target(0, prevRT), 9);
    d3d::release(prevRT);
    D3DCheck(d3d::set_render_target(1, nullptr), 10);
    D3DCheck(d3d::set_render_target(2, nullptr), 11);
}

static void system_tex_destroy(GSystem& s)
{
    for (int k = 0; k < 3; ++k)
        for (int p = 0; p < 2; ++p)
        {
            if (s.surf[k][p]) d3d::release(s.surf[k][p]);
            if (s.tex[k][p]) d3d::release(s.tex[k][p]);
        }
    if (s.id_vb) { d3d::release(s.id_vb); s.id_vb = nullptr; }
    if (s.mix_vb) { d3d::release(s.mix_vb); s.mix_vb = nullptr; }
}

// ============================================================================
// 发射批次入队(CPU 影子数组同步记录)
// ============================================================================
static void queue_spawn(GSystem& s, int type, int n, const SpawnBatch& tmpl)
{
    if (n <= 0 || s.capacity <= 0) return;
    n = std::min(n, s.capacity);

    int start = s.cursor;
    SpawnBatch b = tmpl;
    b.start = (float)start;
    b.count = (float)n;
    b.type = (float)type;
    b.seed = (float)((gphashf((float)(start + (int)s.now * 7)) * 100000.0f));

    // 跨环拆批: GPU 端按连续区间 [start, start+count) 判断, 不取模。
    // 若 start+n 越过 capacity, 拆成 [start, cap) 与 [0, n-(cap-start)) 两条,
    // 否则回绕部分的槽永远收不到出生指令。
    if (start + n > s.capacity)
    {
        int tail = s.capacity - start;          // 第一段: [start, cap)
        SpawnBatch b2 = b;
        b2.start = (float)start;
        b2.count = (float)tail;
        s.pending.push_back(b2);
        SpawnBatch b3 = b;
        b3.start = 0.0f;
        b3.count = (float)(n - tail);
        s.pending.push_back(b3);
    }
    else
    {
        s.pending.push_back(b);
    }

    for (int i = 0; i < n; ++i)
    {
        int slot = (start + i) % s.capacity;
        s.s_birth[slot] = s.now;
        s.s_type[slot] = type;
        s.live_window.push_back({ slot, s.now });
        auto it = g_types.find(type);
        if (it != g_types.end())
        {
            const GType& gt = it->second;
            float life = (float)lerp(gt.t[0][0], gt.t[0][1], gphashf((float)slot + b.seed * 17.0f));
            s.s_life[slot] = life;
            int frames = (int)gt.frame_rect.size();
            if (gt.random_frame && frames > 0)
                s.s_frame[slot] = (int)(gphashf((float)slot + b.seed * 17.0f + 9.0f) * frames) % frames;
            else
                s.s_frame[slot] = 0;
            // 系统混合掩码(静态路径判定用)
            s.blend_mask |= (gt.t[2][3] > 0.5f) ? 2 : 1;
            s.mix_dirty = true;
        }
        else
        {
            s.s_life[slot] = 30.0f;
            s.s_frame[slot] = 0;
        }
    }
    s.cursor = (s.cursor + n) % s.capacity;
    s.mix_dirty = true;
}

// ============================================================================
// 演化 pass
// ============================================================================
// 渲染状态保存/恢复
struct RsSave
{
    dword vs = 0, ps = 0, fvf = 0;
    void* decl = nullptr;
    void* rt0 = nullptr;
    dword zenable = 0, blend = 0, src = 0, dst = 0;
    dword cull = 0;
    dword ps_en = 0, ps_scale = 0, ps_min = 0, ps_max = 0, ps_size = 0;
    float minv = 0, maxv = 0, sizev = 0;
};
static void rs_save(RsSave& r)
{
    D3DCheck(d3d::get_vertex_shader(&r.vs), 1);
    D3DCheck(d3d::get_pixel_shader(&r.ps), 2);
    D3DCheck(d3d::get_fvf(&r.fvf), 3);
    D3DCheck(d3d::get_vertex_declaration(&r.decl), 4);
    D3DCheck(d3d::get_render_target(0, &r.rt0), 5);
    d3d::get_render_state(D3DRS_ZENABLE, &r.zenable);
    d3d::get_render_state(D3DRS_ALPHABLENDENABLE, &r.blend);
    d3d::get_render_state(D3DRS_SRCBLEND, &r.src);
    d3d::get_render_state(D3DRS_DESTBLEND, &r.dst);
    d3d::get_render_state(D3DRS_CULLMODE, &r.cull);
    d3d::get_render_state(D3DRS_POINTSPRITEENABLE, &r.ps_en);
    d3d::get_render_state(D3DRS_POINTSCALEENABLE, &r.ps_scale);
    d3d::get_render_state(D3DRS_POINTSIZE_MIN, &r.ps_min);
    d3d::get_render_state(D3DRS_POINTSIZE_MAX, &r.ps_max);
    d3d::get_render_state(D3DRS_POINTSIZE, &r.ps_size);
    memcpy(&r.minv, &r.ps_min, 4);
    memcpy(&r.maxv, &r.ps_max, 4);
    memcpy(&r.sizev, &r.ps_size, 4);
}
static void rs_restore(const RsSave& r)
{
    D3DCheck(d3d::set_vertex_shader_handle(r.vs), 1);
    D3DCheck(d3d::set_pixel_shader(r.ps), 2);
    D3DCheck(d3d::set_vertex_declaration(r.decl), 3);
    D3DCheck(d3d::set_fvf(r.fvf), 4);
    D3DCheck(d3d::set_render_target(0, r.rt0), 5);
    d3d::release(r.rt0);
    D3DCheck(d3d::set_render_target(1, nullptr), 6);
    D3DCheck(d3d::set_render_target(2, nullptr), 7);
    d3d::set_render_state(D3DRS_ZENABLE, r.zenable);
    d3d::set_render_state(D3DRS_ALPHABLENDENABLE, r.blend);
    d3d::set_render_state(D3DRS_SRCBLEND, r.src);
    d3d::set_render_state(D3DRS_DESTBLEND, r.dst);
    d3d::set_render_state(D3DRS_CULLMODE, r.cull);
    d3d::set_render_state(D3DRS_POINTSPRITEENABLE, r.ps_en);
    d3d::set_render_state(D3DRS_POINTSCALEENABLE, r.ps_scale);
    d3d::set_render_state(D3DRS_POINTSIZE_MIN, r.ps_min);
    d3d::set_render_state(D3DRS_POINTSIZE_MAX, r.ps_max);
    d3d::set_render_state(D3DRS_POINTSIZE, r.ps_size);
}
// 纹理 stage 保存/恢复(0..5 的绑定 + 寻址/过滤; addr 用 [i*2]=U, [i*2+1]=V)
// 注意必须覆盖到 stage 5(矩形表), 否则残留绑定会污染 ext 多纹理绘制(TEX8)。
static const int GP_STAGES = 6;
static void stages_save(void* tex[GP_STAGES], dword addr[GP_STAGES * 2],
                        dword mag[GP_STAGES], dword minf[GP_STAGES], dword mip[GP_STAGES])
{
    for (int i = 0; i < GP_STAGES; ++i)
    {
        tex[i] = nullptr;
        d3d::get_texture(i, &tex[i]);
        d3d::get_tex_stage_state(i, D3DTSS_ADDRESSU, &addr[i * 2]);
        d3d::get_tex_stage_state(i, D3DTSS_ADDRESSV, &addr[i * 2 + 1]);
        d3d::get_tex_stage_state(i, D3DTSS_MAGFILTER, &mag[i]);
        d3d::get_tex_stage_state(i, D3DTSS_MINFILTER, &minf[i]);
        d3d::get_tex_stage_state(i, D3DTSS_MIPFILTER, &mip[i]);
    }
}
static void stages_restore(void* tex[GP_STAGES], dword addr[GP_STAGES * 2],
                           dword mag[GP_STAGES], dword minf[GP_STAGES], dword mip[GP_STAGES])
{
    for (int i = 0; i < GP_STAGES; ++i)
    {
        d3d::set_tex_stage_state(i, D3DTSS_ADDRESSU, addr[i * 2]);
        d3d::set_tex_stage_state(i, D3DTSS_ADDRESSV, addr[i * 2 + 1]);
        d3d::set_tex_stage_state(i, D3DTSS_MAGFILTER, mag[i]);
        d3d::set_tex_stage_state(i, D3DTSS_MINFILTER, minf[i]);
        d3d::set_tex_stage_state(i, D3DTSS_MIPFILTER, mip[i]);
        d3d::set_texture(i, tex[i]);
    }
}
static void stages_set_point()
{
    for (int i = 0; i < GP_STAGES; ++i)
    {
        d3d::set_tex_stage_state(i, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
        d3d::set_tex_stage_state(i, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
        d3d::set_tex_stage_state(i, D3DTSS_MAGFILTER, D3DTEXF_POINT);
        d3d::set_tex_stage_state(i, D3DTSS_MINFILTER, D3DTEXF_POINT);
        d3d::set_tex_stage_state(i, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    }
}

static void run_evolution(GSystem& s, const std::vector<SpawnBatch>& batches, int count)
{
    if (count < 0) return;   // count == 0 合法: 纯老化 pass(无出生分支)
    RsSave rs;
    rs_save(rs);
    void* tex[GP_STAGES] = {};
    dword addr[GP_STAGES * 2] = {}, mag[GP_STAGES] = {}, minf[GP_STAGES] = {}, mip[GP_STAGES] = {};
    stages_save(tex, addr, mag, minf, mip);

    int w = s.cur, dst = s.cur ^ 1;
    D3DCheck(d3d::set_render_target(0, s.surf[0][dst]), 1);
    D3DCheck(d3d::set_render_target(1, s.surf[1][dst]), 2);
    D3DCheck(d3d::set_render_target(2, s.surf[2][dst]), 3);
    D3DCheck(d3d::set_viewport(GP_GRID, GP_GRID), 4);
    D3DCheck(d3d::set_render_state(D3DRS_ZENABLE, FALSE), 5);
    D3DCheck(d3d::set_render_state(D3DRS_ALPHABLENDENABLE, FALSE), 6);
    D3DCheck(d3d::set_render_state(D3DRS_POINTSPRITEENABLE, FALSE), 7);

    stages_set_point();
    D3DCheck(d3d::set_texture(0, s.tex[0][w]), 8);
    D3DCheck(d3d::set_texture(1, s.tex[1][w]), 9);
    D3DCheck(d3d::set_texture(2, s.tex[2][w]), 10);
    D3DCheck(d3d::set_texture(3, g_type_tex), 11);

    D3DCheck(d3d::set_vertex_declaration(g_quad_decl), 12);
    D3DCheck(d3d::set_vertex_shader_handle(g_evo_vs), 13);
    D3DCheck(d3d::set_pixel_shader(g_evo_ps), 14);
    D3DCheck(d3d::set_stream_source(0, g_quad_vb, 16), 15);

    float dt = 1.0f;
    float glob[4] = { s.now, dt, 1.0f / (float)GP_GRID, (float)s.capacity };
    D3DCheck(d3d::set_vs_const_typed(EVO_C_GLOBAL, d3d::CK_FLOAT, glob, 1), 16);
    D3DCheck(d3d::set_ps_const_typed(EVO_C_GLOBAL, d3d::CK_FLOAT, glob, 1), 17);
    float bn[4] = { (float)count, 0, 0, 0 };
    D3DCheck(d3d::set_ps_const_typed(EVO_C_BATCHN, d3d::CK_FLOAT, bn, 1), 18);

    for (int b = 0; b < count; ++b)
        D3DCheck(d3d::set_ps_const_typed(EVO_C_BATCHES + b * 4, d3d::CK_FLOAT, (const float*)&batches[b], 4), 19);

    D3DCheck(d3d::draw_primitive(D3DPT_TRIANGLESTRIP, 2, 0), 20);

    s.cur = dst;
    // 注意: s.now 不在这里推进! 一次 update 可能切块跑多次演化 pass,
    // 时钟只能推进一次(由 gpart_system_update 统一推进), 否则多批次时加速。

    stages_restore(tex, addr, mag, minf, mip);
    rs_restore(rs);
}

// ============================================================================
// 渲染 pass
// ============================================================================
// 重建渲染数据: 活跃窗口过滤(原地)+ 按出生序双桶(普通/加法)+ 图集矩形, 无排序
static void run_render(GSystem& s)
{
    // 快速跳过: 从未发射过/已清空
    if (s.live_window.empty()) return;

    RsSave rs;
    rs_save(rs);
    void* tex[GP_STAGES] = {};
    dword addr[GP_STAGES * 2] = {}, mag[GP_STAGES] = {}, minf[GP_STAGES] = {}, mip[GP_STAGES] = {};
    stages_save(tex, addr, mag, minf, mip);

    // 渲染状态: 三角形管线(四边形粒子), 无点精灵依赖
    D3DCheck(d3d::set_render_state(D3DRS_ZENABLE, FALSE), 1);
    D3DCheck(d3d::set_render_state(D3DRS_CULLMODE, D3DCULL_NONE), 2);

    // 纹理绑定: PS sMain=stage0(图集), PS sRect=stage5(矩形表);
    // VS(VTF 独立槽位): sOvr=257, sPos=258, sLife=259, sType=260
    stages_set_point();
    int w = s.cur;
    D3DCheck(d3d::set_texture(0, g_atlas_tex), 6);
    D3DCheck(d3d::set_texture(5, g_rect_tex), 11);
    D3DCheck(d3d::set_texture(GP_VTS0 + 0, s.tex[2][w]), 100);   // VS sOvr(覆盖色)
    D3DCheck(d3d::set_texture(GP_VTS0 + 1, s.tex[0][w]), 101);   // VS sPos(位置/速度)
    D3DCheck(d3d::set_texture(GP_VTS0 + 2, s.tex[1][w]), 102);   // VS sLife(age/life/type/frame)
    D3DCheck(d3d::set_texture(GP_VTS0 + 3, g_type_tex), 103);    // VS sType(类型表)
    // 图集(stage 0, PS sMain)用 LINEAR: 形状纹理采样需平滑, 状态/矩形表保持 POINT
    d3d::set_tex_stage_state(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    d3d::set_tex_stage_state(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);

    // VS 常量
    float wvp[16];
    d3d::get_transform(D3DTS_WORLD, wvp);
    {
        float view[16], proj[16];
        d3d::get_transform(D3DTS_VIEW, view);
        d3d::get_transform(D3DTS_PROJECTION, proj);
        float wv[16] = {};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
            {
                float sum = 0;
                for (int k = 0; k < 4; ++k) sum += wvp[r * 4 + k] * view[k * 4 + c];
                wv[r * 4 + c] = sum;
            }
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
            {
                float sum = 0;
                for (int k = 0; k < 4; ++k) sum += wv[r * 4 + k] * proj[k * 4 + c];
                wvp[r * 4 + c] = sum;
            }
    }
    float sys[4] = { s.pos_x, s.pos_y, 1.0f / (float)GP_GRID, (float)s.capacity };
    D3DCheck(d3d::set_vs_const_typed(RND_C_WVP, d3d::CK_FLOAT, wvp, 4), 12);
    D3DCheck(d3d::set_vs_const_typed(RND_C_SYS, d3d::CK_FLOAT, sys, 1), 13);

    D3DCheck(d3d::set_vertex_declaration(g_id_decl), 16);
    D3DCheck(d3d::set_vertex_shader_handle(g_rnd_vs), 17);
    D3DCheck(d3d::set_pixel_shader(g_rnd_ps), 18);
    D3DCheck(d3d::set_stream_source(0, s.id_vb, 12), 19);

    // 混合状态: 普通 = SRCALPHA/INVSRCALPHA, 加法 = ONE/ONE
    auto set_blend = [](int additive, int pos) {
        D3DCheck(d3d::set_render_state(D3DRS_ALPHABLENDENABLE, TRUE), pos);
        if (additive)
        {
            D3DCheck(d3d::set_render_state(D3DRS_SRCBLEND, D3DBLEND_ONE), pos + 1);
            D3DCheck(d3d::set_render_state(D3DRS_DESTBLEND, D3DBLEND_ONE), pos + 2);
        }
        else
        {
            D3DCheck(d3d::set_render_state(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA), pos + 1);
            D3DCheck(d3d::set_render_state(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA), pos + 2);
        }
    };

    // 混合分遍绘制: 每遍设 blend 状态 + uBlend 常量, VS 把不匹配粒子的 alpha 归零。
    // 静态四边形 VB, 零 CPU; 环形两弧保证出生序绘制顺序。
    int cap = s.capacity, cur = s.cursor;
    auto draw_ranges = [&](int pos) {
        // 注意: DrawPrimitive 第三参数 = 图元数(三角形), 每粒子 2 三角形; 顶点偏移 = 粒子*6
        if (s.old_to_new)
        {
            // 旧→新: 先 [cursor, cap) 后 [0, cursor)
            if (cur < cap)
                D3DCheck(d3d::draw_primitive(D3DPT_TRIANGLELIST,
                    (DWORD)(cap - cur) * 2, (DWORD)cur * 6), pos);
            if (cur > 0)
                D3DCheck(d3d::draw_primitive(D3DPT_TRIANGLELIST,
                    (DWORD)cur * 2, 0), pos + 1);
        }
        else
        {
            // 新→旧: 先 [0, cursor) 后 [cursor, cap)
            if (cur > 0)
                D3DCheck(d3d::draw_primitive(D3DPT_TRIANGLELIST,
                    (DWORD)cur * 2, 0), pos);
            if (cur < cap)
                D3DCheck(d3d::draw_primitive(D3DPT_TRIANGLELIST,
                    (DWORD)(cap - cur) * 2, (DWORD)cur * 6), pos + 1);
        }
    };

    float bl[4] = { 0, 0, 0, 0 };
    if (s.blend_mask == 3)
    {
        // 混合系统: 普通 + 加法两遍(VS 按 uBlend 零化不匹配粒子)
        set_blend(0, 20);
        D3DCheck(d3d::set_vs_const_typed(RND_C_BLEND, d3d::CK_FLOAT, bl, 1), 22);
        draw_ranges(23);
        set_blend(1, 25);
        bl[0] = 1.0f;
        D3DCheck(d3d::set_vs_const_typed(RND_C_BLEND, d3d::CK_FLOAT, bl, 1), 26);
        draw_ranges(27);
    }
    else
    {
        int additive = (s.blend_mask & 2) ? 1 : 0;
        set_blend(additive, 20);
        bl[0] = (float)additive;
        D3DCheck(d3d::set_vs_const_typed(RND_C_BLEND, d3d::CK_FLOAT, bl, 1), 22);
        draw_ranges(23);
    }

    // 解绑 VS 采样槽位: 防止游戏自己的 shader(FFP 模拟/SDF)在 VS 采样到我们的状态纹理
    d3d::set_texture(GP_VTS0 + 0, nullptr);
    d3d::set_texture(GP_VTS0 + 1, nullptr);
    d3d::set_texture(GP_VTS0 + 2, nullptr);
    d3d::set_texture(GP_VTS0 + 3, nullptr);

    stages_restore(tex, addr, mag, minf, mip);
    rs_restore(rs);
}


// GPU 端存活数(浮点读回状态纹理, 仅供日志诊断)
static int gpu_alive_count(GSystem& s)
{
    std::vector<float> px;
    UINT w = 0, h = 0;
    HRESULT hr = d3d::read_texture_float(s.tex[1][s.cur], px, w, h);
    if (FAILED(hr)) return -1;
    int n = 0;
    for (size_t i = 0; i < px.size(); i += 4)
    {
        float age = px[i + 0], life = px[i + 1];
        if (life > 0.0f && age < life) n++;
    }
    return n;
}
// ============================================================================
// 导出: 系统
// ============================================================================
exp_real gpart_system_create(double capacity)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        if (!gpart_gpu_init()) return gerror;
        GSystem s;
        // capacity <= 0 → 默认 4096(GML 包装脚本省略参数时为 0)
        s.capacity = (int)capacity <= 0
            ? 4096
            : (int)std::clamp(capacity, 1.0, (double)GP_MAX_CAPACITY);
        s.s_birth.resize(s.capacity, -1e9f);
        s.s_life.resize(s.capacity, 1.0f);
        s.s_type.resize(s.capacity, 0);
        s.s_frame.resize(s.capacity, 0);
        system_tex_create(s);

        // 静态四边形 VB: 每粒子 6 顶点(2 三角形) × (角点x, 角点y, id), 一次创建永不重建
        std::vector<float> qv((size_t)s.capacity * 6 * 3);
        static const float corners[6][2] = { {0,0},{1,0},{0,1},{1,0},{1,1},{0,1} };
        for (int i = 0; i < s.capacity; ++i)
            for (int k = 0; k < 6; ++k)
            {
                qv[(size_t)(i * 6 + k) * 3 + 0] = corners[k][0];
                qv[(size_t)(i * 6 + k) * 3 + 1] = corners[k][1];
                qv[(size_t)(i * 6 + k) * 3 + 2] = (float)i;
            }
        D3DCheck(d3d::create_vertex_buffer((UINT)(qv.size() * 4), &s.id_vb), 1);
        D3DCheck(d3d::upload_vertex_buffer(s.id_vb, qv.data(), (UINT)(qv.size() * 4)), 2);

        int id = g_system_counter++;
        g_systems.emplace(id, std::move(s));
        return (double)id;
    }
    simple_catch("gpart_system_create", gerror)
}

exp_real gpart_system_destroy(double sys)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gtrue;
        system_tex_destroy(it->second);
        g_systems.erase(it);
        return gtrue;
    }
    simple_catch("gpart_system_destroy", gerror)
}

exp_real gpart_system_exists(double sys)
{
    return g_systems.count((int)sys) ? gtrue : gfalse;
}

exp_real gpart_system_clear(double sys)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        GSystem& s = it->second;
        std::fill(s.s_birth.begin(), s.s_birth.end(), -1e9f);
        std::fill(s.s_life.begin(), s.s_life.end(), 1.0f);
        s.pending.clear();
        s.live_window.clear();
        s.cursor = 0;
        s.blend_mask = 0;
        s.mix_dirty = true;
        return gtrue;
    }
    simple_catch("gpart_system_clear", gerror)
}

exp_real gpart_system_update()
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        if (g_systems.empty()) return gtrue;
        for (auto& kv : g_systems)
        {
            GSystem& s = kv.second;
            // 活跃窗口家务(原地过滤): 代数校验防槽复用冲突 + 死亡剔除。
            // 静态路径没有每帧组装, 过滤必须在此处进行, 否则窗口无限增长。
            size_t w = 0;
            auto& win = s.live_window;
            for (size_t i = 0; i < win.size(); ++i)
            {
                const LiveEntry& e = win[i];
                if (e.birth != s.s_birth[e.slot]) continue;
                float age = s.now - e.birth;
                if (age < 0.0f || age >= s.s_life[e.slot]) continue;
                win[w++] = e;
            }
            win.resize(w);

            // 一次性诊断转储: 首次出现活跃粒子时, 记录 shader 各输入的真实数据。
            // CPU 侧(g_types 即类型表/矩形表上传源) + GPU 读回(状态纹理)双重核对。
            static bool g_diag_dumped = false;
            if (!g_diag_dumped && !s.live_window.empty())
            {
                g_diag_dumped = true;
                char buf[512];
                for (auto& kv : g_types)
                {
                    const GType& gt = kv.second;
                    auto r = gt.shape_rect();
                    sprintf(buf, "diag type=%d shape=%d size=(%.2f,%.2f) scale=(%.2f,%.2f) "
                        "rect=(%.4f,%.4f,%.4f,%.4f) frames=%d blend=%d",
                        kv.first, gt.shape, gt.t[0][2], gt.t[0][3], gt.t[3][0], gt.t[3][1],
                        r.u0, r.v0, r.u1, r.v1, (int)gt.frame_rect.size(),
                        (gt.t[2][3] > 0.5f) ? 1 : 0);
                    gpart_log(buf);
                }
                std::vector<float> px;
                UINT tw = 0, th = 0;
                if (SUCCEEDED(d3d::read_texture_float(s.tex[1][s.cur], px, tw, th)) && tw > 0)
                {
                    int shown = 0;
                    for (size_t i = 0; i < s.live_window.size() && shown < 6; ++i)
                    {
                        int slot = s.live_window[i].slot;
                        if (slot < 0 || slot >= (int)s.capacity) continue;
                        int k = ((slot / 256) * (int)tw + (slot % 256)) * 4;
                        if (k + 3 >= (int)px.size()) continue;
                        sprintf(buf, "diag slot=%d age=%.1f life=%.1f type=%.1f frame=%.1f",
                            slot, px[k + 0], px[k + 1], px[k + 2], px[k + 3]);
                        gpart_log(buf);
                        shown++;
                    }
                }
                // 位置读回(tex[0] = pos.xy, vel.xy) + 系统/发射器配置 + 渲染变换
                std::vector<float> pp;
                UINT pw = 0, ph = 0;
                if (SUCCEEDED(d3d::read_texture_float(s.tex[0][s.cur], pp, pw, ph)) && pw > 0)
                {
                    int shown = 0;
                    for (size_t i = 0; i < s.live_window.size() && shown < 6; ++i)
                    {
                        int slot = s.live_window[i].slot;
                        if (slot < 0 || slot >= (int)s.capacity) continue;
                        int k = ((slot / 256) * (int)pw + (slot % 256)) * 4;
                        if (k + 3 >= (int)pp.size()) continue;
                        sprintf(buf, "diag pos slot=%d pos=(%.1f,%.1f) vel=(%.2f,%.2f)",
                            slot, pp[k + 0], pp[k + 1], pp[k + 2], pp[k + 3]);
                        gpart_log(buf);
                        shown++;
                    }
                }
                {
                    sprintf(buf, "diag sys pos=(%.1f,%.1f) capacity=%d",
                        s.pos_x, s.pos_y, s.capacity);
                    gpart_log(buf);
                    for (auto& ekv : s.emitters)
                    {
                        const GEmitter& g = ekv.second;
                        sprintf(buf, "diag emitter region=(%.1f,%.1f,%.1f,%.1f) rate=%.1f type=%d",
                            g.xmin, g.ymin, g.xmax, g.ymax, g.stream_rate, g.stream_type);
                        gpart_log(buf);
                    }
                }
                // 视口 + WVP 关键元素(核对坐标映射) + 最老粒子位置
                {
                    UINT vw = 0, vh = 0;
                    d3d::get_viewport(&vw, &vh);
                    sprintf(buf, "diag viewport=(%u x %u)", vw, vh);
                    gpart_log(buf);
                    float wm[16], vm[16], pm[16];
                    d3d::get_transform(D3DTS_WORLD, wm);
                    d3d::get_transform(D3DTS_VIEW, vm);
                    d3d::get_transform(D3DTS_PROJECTION, pm);
                    sprintf(buf, "diag world m0=(%.2f,%.2f) t=(%.1f,%.1f) | view m0=(%.2f,%.2f) t=(%.1f,%.1f) | proj m0=(%.4f,%.4f) t=(%.1f,%.1f)",
                        wm[0], wm[5], wm[12], wm[13],
                        vm[0], vm[5], vm[12], vm[13],
                        pm[0], pm[5], pm[12], pm[13]);
                    gpart_log(buf);
                }
                if (SUCCEEDED(d3d::read_texture_float(s.tex[0][s.cur], pp, pw, ph)) && pw > 0)
                {
                    int shown = 0;
                    for (size_t i = s.live_window.size(); i-- > 0 && shown < 6;)
                    {
                        int slot = s.live_window[i].slot;
                        if (slot < 0 || slot >= (int)s.capacity) continue;
                        int k = ((slot / 256) * (int)pw + (slot % 256)) * 4;
                        if (k + 3 >= (int)pp.size()) continue;
                        sprintf(buf, "diag oldpos slot=%d pos=(%.1f,%.1f) vel=(%.2f,%.2f)",
                            slot, pp[k + 0], pp[k + 1], pp[k + 2], pp[k + 3]);
                        gpart_log(buf);
                        shown++;
                    }
                }
                // GPU 侧类型表/矩形表读回(核对 shader 实际采样的内容)
                {
                    std::vector<float> tt;
                    UINT tw = 0, th = 0;
                    if (SUCCEEDED(d3d::read_texture_float(g_type_tex, tt, tw, th)) && tw >= 256)
                    {
                        int c = 1;   // 测试类型 id = 1
                        int k0 = (0 * 256 + c) * 4, k3 = (3 * 256 + c) * 4;
                        sprintf(buf, "diag gpu_type col=%d row0(life,size)=(%.1f,%.1f,%.1f,%.1f) row3(scale,..)=(%.2f,%.2f,%.0f,%.0f)",
                            c, tt[k0 + 0], tt[k0 + 1], tt[k0 + 2], tt[k0 + 3],
                            tt[k3 + 0], tt[k3 + 1], tt[k3 + 2], tt[k3 + 3]);
                        gpart_log(buf);
                    }
                    else gpart_log("diag gpu_type = READ_FAILED");
                }
                {
                    std::vector<float> rt;
                    UINT rw = 0, rh = 0;
                    if (SUCCEEDED(d3d::read_texture_float(g_rect_tex, rt, rw, rh)) && rw >= 256)
                    {
                        int c = 1;
                        int k = (0 * 256 + c) * 4;
                        sprintf(buf, "diag gpu_rect col=%d row0=(%.4f,%.4f,%.4f,%.4f)",
                            c, rt[k + 0], rt[k + 1], rt[k + 2], rt[k + 3]);
                        gpart_log(buf);
                    }
                    else gpart_log("diag gpu_rect = READ_FAILED");
                }
                // 图集球体瓦片(tile 7, 448..511 x 0..63)内容核对
                {
                    std::vector<BYTE> ab;
                    UINT aw = 0, ah = 0;
                    if (SUCCEEDED(d3d::read_texture(g_atlas_tex, ab, aw, ah)) && aw >= 512)
                    {
                        auto px = [&](int x, int y) -> const BYTE* {
                            return &ab[((size_t)y * aw + x) * 4];
                        };
                        const BYTE* c0 = px(480, 32);   // 球体中心
                        const BYTE* e0 = px(448, 32);   // 球体左缘
                        const BYTE* o0 = px(0, 100);    // 图集空白区(tile 0 下方)
                        sprintf(buf, "diag atlas sphere center=(%d,%d,%d,%d) edge=(%d,%d,%d,%d) blank=(%d,%d,%d,%d)",
                            c0[0], c0[1], c0[2], c0[3], e0[0], e0[1], e0[2], e0[3],
                            o0[0], o0[1], o0[2], o0[3]);
                        gpart_log(buf);
                    }
                    else gpart_log("diag atlas = READ_FAILED");
                }
            }

            // 流式发射器: 每步自动发射(配置一次, 由 update 处理, 同 GM8 引擎语义)
            for (auto& ekv : s.emitters)
            {
                GEmitter& g = ekv.second;
                if (g.stream_rate > 0.0f && g.stream_type >= 1
                    && g_types.count(g.stream_type))
                {
                    SpawnBatch b = emitter_batch(g);
                    queue_spawn(s, g.stream_type, (int)g.stream_rate, b);
                }
            }

            // 无粒子且无发射 → 无事可做
            if (s.live_window.empty() && s.pending.empty()) continue;

            // 演化 pass: 无批次也要跑(老化, GM8 语义: 每次 update 推进一步);
            // 批次存在则带上出生分支(分 16 批切块, 时钟只推进一次)。
            if (s.pending.empty())
            {
                std::vector<SpawnBatch> none;
                run_evolution(s, none, 0);
            }
            else
            {
                size_t off = 0;
                while (off < s.pending.size())
                {
                    size_t n = std::min((size_t)GP_MAX_BATCHES, s.pending.size() - off);
                    std::vector<SpawnBatch> chunk(s.pending.begin() + off, s.pending.begin() + off + n);
                    run_evolution(s, chunk, (int)n);
                    off += n;
                }
                s.pending.clear();
            }
            s.now += 1.0f;   // 一次 update = 一步

            // 调试日志: 每 30 步输出一次 CPU/GPU 存活对照
            if ((int)s.now % 30 == 0)
            {
                int cpu = 0;
                for (auto& e : s.live_window)
                    if (e.birth == s.s_birth[e.slot])
                    {
                        float a = s.now - e.birth;
                        if (a >= 0.0f && a < s.s_life[e.slot]) cpu++;
                    }
                gpart_log("sys=" + std::to_string(kv.first) +
                    " step=" + std::to_string((int)s.now) +
                    " cpu_alive=" + std::to_string(cpu) +
                    " gpu_alive=" + std::to_string(gpu_alive_count(s)));
            }
        }
        return gtrue;
    }
    simple_catch("gpart_system_update", gerror)
}

exp_real gpart_system_drawit(double sys)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        GSystem& s = it->second;
        run_render(s);
        return gtrue;
    }
    simple_catch("gpart_system_drawit", gerror)
}

exp_real gpart_system_draw_order(double sys, double oldtonew)
{
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        it->second.old_to_new = oldtonew > 0.5;
        it->second.mix_dirty = true;
        return gtrue;
    }
    simple_catch("gpart_system_draw_order", gerror)
}

exp_real gpart_system_position(double sys, double x, double y)
{
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        it->second.pos_x = (float)x;
        it->second.pos_y = (float)y;
        return gtrue;
    }
    simple_catch("gpart_system_position", gerror)
}

exp_real gpart_system_capacity(double sys)
{
    auto it = g_systems.find((int)sys);
    return it == g_systems.end() ? gerror : (double)it->second.capacity;
}

// ============================================================================
// 导出: 粒子(直造)
// ============================================================================
static double particles_create_impl(double sys, double x, double y, double parttype,
    double color, bool has_color, double number)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        if (g_types.find((int)parttype) == g_types.end()) return gfalse;
        GSystem& s = it->second;

        SpawnBatch b;
        b.shape = -1.0f;             // 点发射
        b.px = (float)x;
        b.py = (float)y;
        if (has_color)
        {
            int c = (int)color;
            b.ovr_r = (float)col_red(c) / 255.0f;
            b.ovr_g = (float)col_green(c) / 255.0f;
            b.ovr_b = (float)col_blue(c) / 255.0f;
            b.has_ovr = 1.0f;
        }
        queue_spawn(s, (int)parttype, (int)number, b);
        return gtrue;
    }
    simple_catch("gpart_particles_create", gerror)
}

exp_real gpart_particles_create(double sys, double x, double y, double parttype, double number)
{
    return particles_create_impl(sys, x, y, parttype, 0, false, number);
}
exp_real gpart_particles_create_color(double sys, double x, double y, double parttype,
    double color, double number)
{
    return particles_create_impl(sys, x, y, parttype, color, true, number);
}
exp_real gpart_particles_clear(double sys) { return gpart_system_clear(sys); }
exp_real gpart_particles_count(double sys)
{
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gerror;
        GSystem& s = it->second;
        int n = 0;
        for (auto& e : s.live_window)
            if (e.birth == s.s_birth[e.slot])
            {
                float age = s.now - e.birth;
                if (age >= 0.0f && age < s.s_life[e.slot]) n++;
            }
        return (double)n;
    }
    simple_catch("gpart_particles_count", gerror)
}

// ============================================================================
// 导出: 类型
// ============================================================================
static GType* type_at(int id)
{
    auto it = g_types.find(id);
    return it == g_types.end() ? nullptr : &it->second;
}

exp_real gpart_type_create()
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        if ((int)g_types.size() >= GP_TYPE_TEX_W - 1)
            throw std::runtime_error("类型数量已达上限(255)。");
        GType t;
        // GM8 默认: life 30, size 64, speed 0, dir 0, 无重力, 白, alpha 1, 形状 pixel
        t.t[0][0] = 30; t.t[0][1] = 30; t.t[0][2] = 64; t.t[0][3] = 64;
        t.t[1][0] = 0;  t.t[1][1] = 0;  t.t[1][2] = 0;  t.t[1][3] = 0;
        t.t[2][0] = 270; t.t[2][1] = 0; t.t[2][2] = 0; t.t[2][3] = 0;
        t.t[3][0] = 1; t.t[3][1] = 1; t.t[3][2] = GP_COLOUR_ONE; t.t[3][3] = GP_ALPHA_ONE;
        t.t[4][0] = 1; t.t[4][1] = 1; t.t[4][2] = 1; t.t[4][3] = 1;
        t.t[5][0] = 1; t.t[5][1] = 1; t.t[5][2] = 1; t.t[5][3] = 1;
        t.t[6][0] = 1; t.t[6][1] = 1; t.t[6][2] = 1; t.t[6][3] = 1;
        int id = g_type_counter++;
        g_types.emplace(id, t);
        type_table_upload();
        g_types[id].upload_rect_table(id);
        return (double)id;
    }
    simple_catch("gpart_type_create", gerror)
}

exp_real gpart_type_destroy(double type)
{
    try
    {
        int id = (int)type;
        auto it = g_types.find(id);
        if (it == g_types.end()) return gtrue;
        // 清零类型表行 + 矩形表列: 仍在飞行的粒子 type 字段指向该行,
        // 清零后 life=0 → 演化 pass 判定死亡, 与混合路径行为一致。
        if (g_type_tex)
        {
            std::vector<unsigned short> zero((size_t)GP_TYPE_TEX_H * 4, 0);
            d3d::upload_texture_rect(g_type_tex, (UINT)id, 0, 1, GP_TYPE_TEX_H,
                GP_FMT_16F, zero.data(), 4 * 2);
        }
        if (g_rect_tex)
        {
            std::vector<unsigned short> zero((size_t)GP_RECT_TEX_FRAMES * 4, 0);
            d3d::upload_texture_rect(g_rect_tex, (UINT)id, 0, 1, GP_RECT_TEX_FRAMES,
                GP_FMT_16F, zero.data(), 4 * 2);
        }
        g_types.erase(it);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_destroy", gerror)
}

exp_real gpart_type_exists(double type)
{
    return g_types.count((int)type) ? gtrue : gfalse;
}

exp_real gpart_type_clear(double type)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        GType fresh;
        *t = fresh;
        t->t[0][0] = 30; t->t[0][1] = 30; t->t[0][2] = 64; t->t[0][3] = 64;
        t->t[1][0] = 0;  t->t[1][1] = 0;  t->t[1][2] = 0;  t->t[1][3] = 0;
        t->t[2][0] = 270; t->t[2][1] = 0; t->t[2][2] = 0; t->t[2][3] = 0;
        t->t[3][0] = 1; t->t[3][1] = 1; t->t[3][2] = GP_COLOUR_ONE; t->t[3][3] = GP_ALPHA_ONE;
        t->t[4][0] = 1; t->t[4][1] = 1; t->t[4][2] = 1; t->t[4][3] = 1;
        t->t[5][0] = 1; t->t[5][1] = 1; t->t[5][2] = 1; t->t[5][3] = 1;
        t->t[6][0] = 1; t->t[6][1] = 1; t->t[6][2] = 1; t->t[6][3] = 1;
        t->frame_rect.clear();
        t->shape = PT_SHAPE_PIXEL;
        t->animat = t->stretch = t->random_frame = false;
        t->t[9][0] = 0;
        type_table_upload();
        t->upload_rect_table((int)type);
        return gtrue;
    }
    simple_catch("gpart_type_clear", gerror)
}

exp_real gpart_type_sprite(double type, double sprite, double animat, double stretch, double random)
{
    try
    {
        if (d3d::version() != d3d::V9) return gerror;
        if (!gpart_gpu_init()) return gerror;
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        int spr = (int)sprite;

        t->frame_rect.clear();
        for (int k = 0; k < GP_MAX_FRAMES; ++k)
        {
            int tex = gm::sprite_get_texture(spr, k);
            if (tex < 0) break;
            void* dtex = (void*)gmapi->GetDirect3DTexture(tex);
            if (!dtex) break;

            std::vector<BYTE> px;
            UINT w = 0, h = 0;
            D3DCheck(d3d::read_texture(dtex, px, w, h), 1);
            if (w == 0 || h == 0 || w > GP_ATLAS_SIZE || h > GP_ATLAS_SIZE)
                throw std::runtime_error("粒子精灵尺寸超出图集(最大 " +
                    std::to_string(GP_ATLAS_SIZE) + "px)。");

            int ax = 0, ay = 0;
            if (!atlas_alloc((int)w, (int)h, ax, ay))
                throw std::runtime_error("粒子图集已满(1024x1024), 请减少精灵种类。");
            D3DCheck(d3d::upload_texture_rect(g_atlas_tex, (UINT)ax, (UINT)ay,
                w, h, D3DFMT_A8R8G8B8, px.data(), w * 4), 2);

            GType::FRect r = {
                .u0 = (float)ax / (float)GP_ATLAS_SIZE,
                .v0 = (float)ay / (float)GP_ATLAS_SIZE,
                .u1 = (float)w / (float)GP_ATLAS_SIZE,
                .v1 = (float)h / (float)GP_ATLAS_SIZE,
            };
            t->frame_rect.push_back(r);
        }
        if (t->frame_rect.empty()) return gfalse;

        t->animat = animat > 0.5;
        t->stretch = stretch > 0.5;
        t->random_frame = random > 0.5;
        t->shape = -1;                 // 有精灵 → 形状路径失效
        t->t[8][1] = (float)t->frame_rect.size();
        t->t[8][2] = t->animat ? 1.0f : 0.0f;
        t->t[8][3] = t->stretch ? 1.0f : 0.0f;
        t->t[9][0] = t->random_frame ? 1.0f : 0.0f;
        type_table_upload();
        t->upload_rect_table((int)type);
        return gtrue;
    }
    simple_catch("gpart_type_sprite", gerror)
}

exp_real gpart_type_shape(double type, double shape)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        int s = (int)shape;
        if (s < 0 || s >= PT_SHAPE_COUNT) return gfalse;
        t->shape = s;
        t->frame_rect.clear();
        t->t[8][1] = 0;
        t->t[8][2] = 0;
        t->t[8][3] = 0;
        t->t[9][0] = 0;
        type_table_upload();
        t->upload_rect_table((int)type);
        return gtrue;
    }
    simple_catch("gpart_type_shape", gerror)
}

exp_real gpart_type_scale(double type, double xscale, double yscale)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->t[3][0] = (float)xscale;
        t->t[3][1] = (float)yscale;
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_scale", gerror)
}

exp_real gpart_type_life(double type, double min, double max)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->t[0][0] = (float)std::min(min, max);
        t->t[0][1] = (float)std::max(min, max);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_life", gerror)
}

exp_real gpart_type_size(double type, double min, double max)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->t[0][2] = (float)std::min(min, max);
        t->t[0][3] = (float)std::max(min, max);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_size", gerror)
}

exp_real gpart_type_speed(double type, double min, double max)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->t[1][0] = (float)std::min(min, max);
        t->t[1][1] = (float)std::max(min, max);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_speed", gerror)
}

exp_real gpart_type_direction(double type, double min, double max)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->t[1][2] = (float)std::min(min, max);
        t->t[1][3] = (float)std::max(min, max);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_direction", gerror)
}

exp_real gpart_type_gravity(double type, double force, double dir)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->t[2][0] = (float)dir;
        t->t[2][1] = (float)force;
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_gravity", gerror)
}

exp_real gpart_type_drag(double type, double coeff)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->t[2][2] = (float)std::clamp(coeff, 0.0, 1.0);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_drag", gerror)
}

exp_real gpart_type_blend(double type, double additive)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->t[2][3] = additive > 0.5 ? 1.0f : 0.0f;
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_blend", gerror)
}

// 颜色模式统一入口: mode + 颜色分量(0..1)
static void type_set_colour(GType* t, int mode, const float c[9])
{
    t->t[3][2] = (float)mode;
    // T4: c1.r c1.g c1.b c2.r; T5: c2.g c2.b c3.r c3.g; T6.x: c3.b
    t->t[4][0] = c[0]; t->t[4][1] = c[1]; t->t[4][2] = c[2]; t->t[4][3] = c[3];
    t->t[5][0] = c[4]; t->t[5][1] = c[5]; t->t[5][2] = c[6]; t->t[5][3] = c[7];
    t->t[6][0] = c[8];
}

static void col_pack3(int c1, int c2, int c3, float out[9])
{
    out[0] = (float)col_red(c1) / 255.0f;
    out[1] = (float)col_green(c1) / 255.0f;
    out[2] = (float)col_blue(c1) / 255.0f;

    out[3] = (float)col_red(c2) / 255.0f;
    out[4] = (float)col_green(c2) / 255.0f;
    out[5] = (float)col_blue(c2) / 255.0f;

    out[6] = (float)col_red(c3) / 255.0f;
    out[7] = (float)col_green(c3) / 255.0f;
    out[8] = (float)col_blue(c3) / 255.0f;
}

exp_real gpart_type_colour1(double type, double c1)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        float c[9];
        col_pack3((int)c1, (int)c1, (int)c1, c);
        type_set_colour(t, GP_COLOUR_ONE, c);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_colour1", gerror)
}

exp_real gpart_type_colour2(double type, double c1, double c2)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        float c[9];
        col_pack3((int)c1, (int)c2, (int)c2, c);
        type_set_colour(t, GP_COLOUR_TWO, c);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_colour2", gerror)
}

exp_real gpart_type_colour3(double type, double c1, double c2, double c3)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        float c[9];
        col_pack3((int)c1, (int)c2, (int)c3, c);
        type_set_colour(t, GP_COLOUR_THREE, c);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_colour3", gerror)
}

exp_real gpart_type_colour_mix(double type, double c1, double c2)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        float c[9];
        col_pack3((int)c1, (int)c2, (int)c2, c);
        type_set_colour(t, GP_COLOUR_MIX, c);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_colour_mix", gerror)
}

exp_real gpart_type_colour_rgb(double type, double rmin, double rmax,
    double gmin, double gmax, double bmin, double bmax)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        float c[9] = {
            (float)std::min(rmin, rmax) / 255.0f,
            (float)std::min(gmin, gmax) / 255.0f,
            (float)std::min(bmin, bmax) / 255.0f,
            (float)std::max(rmin, rmax) / 255.0f,
            (float)std::max(gmin, gmax) / 255.0f,
            (float)std::max(bmin, bmax) / 255.0f,
            0, 0, 0
        };
        type_set_colour(t, GP_COLOUR_RGB, c);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_colour_rgb", gerror)
}

exp_real gpart_type_colour_hsv(double type, double hmin, double hmax,
    double smin, double smax, double vmin, double vmax)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        float c[9] = {
            (float)std::min(hmin, hmax),
            (float)std::min(smin, smax),
            (float)std::min(vmin, vmax),
            (float)std::max(hmin, hmax),
            (float)std::max(smin, smax),
            (float)std::max(vmin, vmax),
            0, 0, 0
        };
        type_set_colour(t, GP_COLOUR_HSV, c);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_colour_hsv", gerror)
}

static void type_set_alpha(GType* t, int mode, float a1, float a2, float a3)
{
    t->t[3][3] = (float)mode;
    t->t[6][1] = a1;
    t->t[6][2] = a2;
    t->t[6][3] = a3;
}

exp_real gpart_type_alpha1(double type, double a1)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        type_set_alpha(t, GP_ALPHA_ONE, (float)std::clamp(a1, 0.0, 1.0), 1.0f, 1.0f);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_alpha1", gerror)
}

exp_real gpart_type_alpha2(double type, double a1, double a2)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        type_set_alpha(t, GP_ALPHA_TWO, (float)std::clamp(a1, 0.0, 1.0),
            (float)std::clamp(a2, 0.0, 1.0), 1.0f);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_alpha2", gerror)
}

exp_real gpart_type_alpha3(double type, double a1, double a2, double a3)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        type_set_alpha(t, GP_ALPHA_THREE, (float)std::clamp(a1, 0.0, 1.0),
            (float)std::clamp(a2, 0.0, 1.0), (float)std::clamp(a3, 0.0, 1.0));
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_alpha3", gerror)
}

exp_real gpart_type_orientation(double type, double ang_min, double ang_max,
    double ang_incr, double ang_wiggle, double ang_relative)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->t[7][0] = (float)ang_min;
        t->t[7][1] = (float)ang_max;
        t->t[7][2] = (float)ang_incr;
        t->t[7][3] = (float)ang_wiggle;
        t->t[8][0] = ang_relative > 0.5 ? 1.0f : 0.0f;
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_orientation", gerror)
}

// ============================================================================
// 导出: 发射器
// ============================================================================
exp_real gpart_emitter_create(double sys)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        GSystem& s = it->second;
        GEmitter em;
        int id = s.em_counter++;
        s.emitters.emplace(id, em);
        return (double)id;
    }
    simple_catch("gpart_emitter_create", gerror)
}

exp_real gpart_emitter_destroy(double sys, double em)
{
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        it->second.emitters.erase((int)em);
        return gtrue;
    }
    simple_catch("gpart_emitter_destroy", gerror)
}

exp_real gpart_emitter_destroy_all(double sys)
{
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        it->second.emitters.clear();
        return gtrue;
    }
    simple_catch("gpart_emitter_destroy_all", gerror)
}

exp_real gpart_emitter_exists(double sys, double em)
{
    auto it = g_systems.find((int)sys);
    if (it == g_systems.end()) return gfalse;
    return it->second.emitters.count((int)em) ? gtrue : gfalse;
}

exp_real gpart_emitter_clear(double sys, double em)
{
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto e = it->second.emitters.find((int)em);
        if (e == it->second.emitters.end()) return gfalse;
        e->second = GEmitter();
        return gtrue;
    }
    simple_catch("gpart_emitter_clear", gerror)
}

exp_real gpart_emitter_region(double sys, double em, double xmin, double xmax,
    double ymin, double ymax, double shape, double distribution)
{
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto e = it->second.emitters.find((int)em);
        if (e == it->second.emitters.end()) return gfalse;
        GEmitter& g = e->second;
        g.xmin = (float)std::min(xmin, xmax);
        g.xmax = (float)std::max(xmin, xmax);
        g.ymin = (float)std::min(ymin, ymax);
        g.ymax = (float)std::max(ymin, ymax);
        g.shape = (int)shape;
        g.distr = (int)distribution;
        return gtrue;
    }
    simple_catch("gpart_emitter_region", gerror)
}

static double emitter_spawn_impl(double sys, double em, double parttype, double number)
{
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        GSystem& s = it->second;
        auto e = s.emitters.find((int)em);
        if (e == s.emitters.end()) return gfalse;
        if (g_types.find((int)parttype) == g_types.end()) return gfalse;
        const GEmitter& g = e->second;

        SpawnBatch b = emitter_batch(g);
        queue_spawn(s, (int)parttype, (int)number, b);
        return gtrue;
    }
    simple_catch("gpart_emitter_burst", gerror)
}

exp_real gpart_emitter_burst(double sys, double em, double parttype, double number)
{
    if (d3d::version() != d3d::V9) return gerror;
    return emitter_spawn_impl(sys, em, parttype, number);
}

// 配置流式发射(同 GM8 part_emitter_stream): 设置后每步自动发射 number 个,
// 由 gpart_system_update 处理; 清除用 gpart_emitter_clear。number <= 0 视为关闭。
exp_real gpart_emitter_stream(double sys, double em, double parttype, double number)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto e = it->second.emitters.find((int)em);
        if (e == it->second.emitters.end()) return gfalse;
        if (g_types.find((int)parttype) == g_types.end()) return gfalse;
        GEmitter& g = e->second;
        if (number > 0.0)
        {
            g.stream_type = (int)parttype;
            g.stream_rate = (float)number;
        }
        else
        {
            g.stream_type = -1;
            g.stream_rate = 0;
        }
        return gtrue;
    }
    simple_catch("gpart_emitter_stream", gerror)
}