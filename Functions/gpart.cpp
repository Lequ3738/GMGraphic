#include "gpart.h"
#include "vertex.h"
#include "xxhash.hpp"          // shader 缓存 key 哈希
#include "../Librarys/math_s.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <fstream>

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
static const int EVO_C_MODE = 5;      // (.x = 1 → 仅出生不老化)
static const int EVO_C_BATCHES = 8;   // 48 batches * 4 float4(ps_3_0 常量上限 224)
static const int EVO_C_EFF = 6;       // (.x=attractor 数, .y=destroyer 数, .z=deflector 数)
static const int RND_C_WVP = 0;       // float4x4 (c0..c3)
static const int RND_C_SYS = 4;       // (sys_x, sys_y, invGrid, capacity)
static const int RND_C_BLEND = 5;     // (当前遍混合模式: 0=普通, 1=加法)

static const int GP_TYPE_ROWS = GP_TYPE_TEX_H;   // 类型表纹理行数(与 gpart.h 一致)
static const int GP_QUAD_VERTS = 4;
static const int GP_EFF_TEX_W = 64;              // 特效器表宽(每行一特效器)
static const int GP_EFF_TEX_H = 6;               // 特效器表高(3 类 × 2 行)
static const int GP_EFF_MAX = 4;                 // 每类最多生效特效器(ps_3_0 展开预算)

// The GPU table is still a compact float4 matrix. Keep its physical layout in
// one named enum so the CPU-side code does not depend on unexplained row ids.
enum class GTypeRow : size_t
{
    LifeSize = 0,
    SpeedDirection = 1,
    GravityDragBlend = 2,
    ScaleModes = 3,
    ColorA = 4,
    ColorB = 5,
    ColorCAndAlpha = 6,
    Orientation = 7,
    Animation = 8,
    RandomFrame = 9,
    StepDeath = 10,
    FeatureFlags = 11,
    RenderSize = 12,
    DirectionExt = 13,   // 方向增量/摆动(每步方向 += incr + ±wiggle, GM8 part_type_direction)
};

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

// 粒子图集占用区域(像素坐标): 类型换精灵/销毁时回收, 复用给后续精灵
struct AtlasRegion { int x, y, w, h; };
static std::vector<AtlasRegion> g_atlas_free;   // 已释放的图集区域(首次适配)

// ============================================================================
// 类型
// ============================================================================
struct GType
{
    struct FRect { float u0, v0, u1, v1; };

    float t[GP_TYPE_TEX_H][4] = {};   // 与 GPU 类型表一一对应；字段名见 GTypeRow
    std::vector<FRect> frame_rect;    // 精灵各帧在粒子图集中的矩形(CPU 侧记录)
    std::vector<AtlasRegion> atlas_owned;   // 本类型在图集中占用的区域(释放时回收)
    int shape = PT_SHAPE_PIXEL;       // 无精灵时的形状
    bool animat = false, stretch = false, random_frame = false;

    float* row(GTypeRow r) { return t[static_cast<size_t>(r)]; }
    const float* row(GTypeRow r) const { return t[static_cast<size_t>(r)]; }
    float& field(GTypeRow r, size_t column) { return row(r)[column]; }
    const float& field(GTypeRow r, size_t column) const { return row(r)[column]; }

    float& life_min() { return field(GTypeRow::LifeSize, 0); }
    float& life_max() { return field(GTypeRow::LifeSize, 1); }
    float& size_min() { return field(GTypeRow::LifeSize, 2); }
    float& size_max() { return field(GTypeRow::LifeSize, 3); }
    const float& life_min() const { return field(GTypeRow::LifeSize, 0); }
    const float& life_max() const { return field(GTypeRow::LifeSize, 1); }
    const float& size_min() const { return field(GTypeRow::LifeSize, 2); }
    const float& size_max() const { return field(GTypeRow::LifeSize, 3); }

    float& speed_min() { return field(GTypeRow::SpeedDirection, 0); }
    float& speed_max() { return field(GTypeRow::SpeedDirection, 1); }
    float& direction_min() { return field(GTypeRow::SpeedDirection, 2); }
    float& direction_max() { return field(GTypeRow::SpeedDirection, 3); }

    float& gravity_direction() { return field(GTypeRow::GravityDragBlend, 0); }
    float& gravity_amount() { return field(GTypeRow::GravityDragBlend, 1); }
    float& drag() { return field(GTypeRow::GravityDragBlend, 2); }
    float& additive() { return field(GTypeRow::GravityDragBlend, 3); }
    const float& gravity_direction() const { return field(GTypeRow::GravityDragBlend, 0); }
    const float& gravity_amount() const { return field(GTypeRow::GravityDragBlend, 1); }
    const float& drag() const { return field(GTypeRow::GravityDragBlend, 2); }
    const float& additive() const { return field(GTypeRow::GravityDragBlend, 3); }

    float& scale_x() { return field(GTypeRow::ScaleModes, 0); }
    float& scale_y() { return field(GTypeRow::ScaleModes, 1); }
    float& colour_mode() { return field(GTypeRow::ScaleModes, 2); }
    float& alpha_mode() { return field(GTypeRow::ScaleModes, 3); }
    const float& scale_x() const { return field(GTypeRow::ScaleModes, 0); }
    const float& scale_y() const { return field(GTypeRow::ScaleModes, 1); }
    const float& colour_mode() const { return field(GTypeRow::ScaleModes, 2); }
    const float& alpha_mode() const { return field(GTypeRow::ScaleModes, 3); }

    float& relative_angle() { return field(GTypeRow::Animation, 0); }
    float& frame_count() { return field(GTypeRow::Animation, 1); }
    float& animation_enabled() { return field(GTypeRow::Animation, 2); }
    float& stretch_animation() { return field(GTypeRow::Animation, 3); }
    float& random_frame_flag() { return field(GTypeRow::RandomFrame, 0); }

    float& step_number() { return field(GTypeRow::StepDeath, 0); }
    float& step_type_id() { return field(GTypeRow::StepDeath, 1); }
    float& death_number() { return field(GTypeRow::StepDeath, 2); }
    float& death_type_id() { return field(GTypeRow::StepDeath, 3); }
    const float& step_number() const { return field(GTypeRow::StepDeath, 0); }
    const float& step_type_id() const { return field(GTypeRow::StepDeath, 1); }
    const float& death_number() const { return field(GTypeRow::StepDeath, 2); }
    const float& death_type_id() const { return field(GTypeRow::StepDeath, 3); }
    bool has_step() const { return field(GTypeRow::FeatureFlags, 0) > 0.5f; }
    bool has_death() const { return field(GTypeRow::FeatureFlags, 1) > 0.5f; }
    bool destroyer_immune() const { return field(GTypeRow::FeatureFlags, 2) > 0.5f; }
    float& destroyer_immune_flag() { return field(GTypeRow::FeatureFlags, 2); }

    float& pixel_scale() { return field(GTypeRow::RenderSize, 0); }
    float& size_increment() { return field(GTypeRow::RenderSize, 1); }
    float& size_wiggle() { return field(GTypeRow::RenderSize, 2); }

    float& direction_increment() { return field(GTypeRow::DirectionExt, 0); }
    float& direction_wiggle() { return field(GTypeRow::DirectionExt, 1); }
    float& speed_increment() { return field(GTypeRow::DirectionExt, 2); }
    float& speed_wiggle() { return field(GTypeRow::DirectionExt, 3); }

    void set_defaults()
    {
        life_min() = life_max() = 100;
        size_min() = 0;
        size_max() = 1;
        speed_min() = speed_max() = 0;
        direction_min() = direction_max() = 0;
        direction_increment() = direction_wiggle() = 0;
        speed_increment() = speed_wiggle() = 0;
        gravity_direction() = 270;
        gravity_amount() = drag() = additive() = 0;
        scale_x() = scale_y() = 1;
        colour_mode() = GP_COLOUR_ONE;
        alpha_mode() = GP_ALPHA_ONE;
        for (int c = 0; c < 4; ++c)
        {
            row(GTypeRow::ColorA)[c] = 1;
            row(GTypeRow::ColorB)[c] = 1;
            row(GTypeRow::ColorCAndAlpha)[c] = 1;
        }
        relative_angle() = 0;
        frame_count() = animation_enabled() = stretch_animation() = 0;
        random_frame_flag() = 0;
        step_number() = step_type_id() = death_number() = death_type_id() = 0;
        field(GTypeRow::FeatureFlags, 0) = field(GTypeRow::FeatureFlags, 1) =
            field(GTypeRow::FeatureFlags, 2) = 0;
        pixel_scale() = 64;   // GM8 内置形状精灵实测 64×64(origin 32,32 中心); size 为缩放倍数
        size_increment() = size_wiggle() = 0;
    }

    float step_num() const { return step_number(); }
    float step_type() const { return step_type_id(); }
    float death_num() const { return death_number(); }
    float death_type() const { return death_type_id(); }

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
static int g_type_counter = 1;               // 下一个从未分配过的类型 id(1..255)
static std::vector<int> g_type_free_ids;     // 销毁回收的类型 id(优先复用)
static bool g_any_step_death = false;   // 有类型配置了 step/death → update 事件检测开关

// 遍历全部类型重算 step/death 检测开关(类型配置变更后调用, 避免标志永不回落)
static void recompute_step_death()
{
    g_any_step_death = false;
    for (auto& kv : g_types)
    {
        const GType& gt = kv.second;
        if (gt.has_step() || gt.has_death())
        {
            g_any_step_death = true;
            break;
        }
    }
}

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

// 区域特效器(GM8 part_attractor_/part_destroyer_/part_deflector_ 语义)。
// 坐标相对粒子系统(与粒子 pos/emitter region 同坐标系, 原样比较)。
struct GAttractor { float x = 0, y = 0, force = 0, dist = 0; int kind = 0; bool additive = false; };
struct GDestroyer { float xmin = 0, xmax = 0, ymin = 0, ymax = 0; int shape = 0; };
struct GDeflector { float xmin = 0, xmax = 0, ymin = 0, ymax = 0; int kind = 0; float friction = 0; };

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
    // 系统内已使用过的混合类型掩码: 位 0=普通, 位 1=加法(静态路径判定)
    int blend_mask = 0;
    // 区域特效器(attractor/destroyer/deflector), id 自增分配(GM8 复用空槽, 自增即可)
    std::unordered_map<int, GAttractor> attractors;
    std::unordered_map<int, GDestroyer> destroyers;
    std::unordered_map<int, GDeflector> deflectors;
    int att_counter = 1, des_counter = 1, def_counter = 1;
    void* eff_tex = nullptr;          // 特效器表 64x6 A16B16G16R16F(每特效器 2 行)
};

static std::unordered_map<int, GSystem> g_systems;
static int g_system_counter = 1;

// ============================================================================
// GPU 资源(全局, 惰性创建)
// ============================================================================
static void* g_type_tex = nullptr;         // 类型表纹理 256x13 A16B16G16R16F
static void* g_atlas_tex = nullptr;        // 粒子图集 1024x1024 A8R8G8B8(形状+精灵帧)
static int g_atlas_x = 0, g_atlas_y = 0, g_atlas_row_h = 0;   // 图集 shelf 分配器
static void* g_quad_vb = nullptr;          // 全屏四边形(剪辑空间, 4 顶点)
static void* g_id_decl = nullptr;          // 渲染 pass 顶点声明(float1 id)
static void* g_quad_decl = nullptr;
static dword g_evo_vs = 0, g_evo_ps = 0;
static dword g_rnd_vs = 0, g_rnd_ps = 0;
static bool g_gpu_ready = false;
static bool g_gpu_failed = false;
// 粒子输出 alpha 模式: -1=自动检测当前混合状态(ONE/INVSRCALPHA→预乘, SRCALPHA→straight),
// 0=强制 straight(SRCALPHA/INVSRCALPHA), 1=强制预乘(ONE/INVSRCALPHA)。默认 -1 适配任意管线。
static int g_gpart_premul = -1;

// 图集分配: 先复用已释放区域(首次适配, 可切分), 否则走 shelf 分配器
static bool atlas_alloc(int w, int h, int& x, int& y)
{
    for (size_t i = 0; i < g_atlas_free.size(); ++i)
    {
        AtlasRegion& r = g_atlas_free[i];
        if (r.w >= w && r.h >= h)
        {
            x = r.x;
            y = r.y;
            if (r.w - w > 0) g_atlas_free.push_back({ r.x + w, r.y, r.w - w, h });
            if (r.h - h > 0) g_atlas_free.push_back({ r.x, r.y + h, r.w, r.h - h });
            g_atlas_free.erase(g_atlas_free.begin() + i);
            return true;
        }
    }
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

// 释放类型占用的全部图集区域(换精灵/清类型/毁类型时调用, 空间可复用)
static void atlas_free_regions(GType& gt)
{
    for (auto& r : gt.atlas_owned)
        g_atlas_free.push_back(r);
    gt.atlas_owned.clear();
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
    "sampler sEff : register(s4);\n"      // 特效器表 64x6 (每特效器 2 行: 行0/1=attractor, 
    "float4 uGlobal : register(c0);\n"    // 行2/3=destroyer, 行4/5=deflector)
    "float4 uBatchCount : register(c4);\n"
    "float4 uMode : register(c5);\n"      // .x = 1 → 仅出生不老化(多块演化用)
    "float4 uEff : register(c6);\n"       // .x=attractor 数, .y=destroyer 数, .z=deflector 数
    "float4 uBatches[64] : register(c8);\n"

    "static const float TWO_PI = 6.283185307179586;\n"
    "static const float DEG2RAD = 0.017453292519943295;\n"
    "static const float GRID = 256.0;\n"

    // 高熵 1D hash: 二次项打破 frac(a*k) 的线性周期(否则相邻 id 方向相关 → 射线)
    "float h1(float a) {\n"
    "  a = frac(a * 0.1031);\n"
    "  a = frac(a * (a + 19.19));\n"
    "  a = frac(a * (a + 33.33));\n"
    "  return frac(a * 43758.5453);\n"
    "}\n"

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

    "    if (b2.x < -1.5) {\n"
           // 源槽位生成(step/death): 位置 = 源粒子当前位置(读上一帧状态)
    "      float src = floor(b2.z + 0.5);\n"
    "      float2 suv = (float2(fmod(src, 256.0), floor(src / 256.0)) + 0.5) * uGlobal.z;\n"
    "      p = tex2D(sPos, suv).xy;\n"
    "    } else if (b2.x < -0.5) {\n"
    "      p = b2.zw;\n"
    "    } else {\n"
    "      float shape = floor(b2.x + 0.5);\n"
    "      float distr = b2.y;\n"
    "      float u = rnd.x, v = rnd.y;\n"

    "      if (distr > 1.5) {\n"  // ps_distr_invgaussian: 边缘密集(均匀盘半径)
    "        float r = sqrt(v);\n"
    "        float a = TWO_PI * u;\n"
    "        u = clamp(0.5 + 0.5 * r * cos(a), 0.0, 1.0);\n"
    "        v = clamp(0.5 + 0.5 * r * sin(a), 0.0, 1.0);\n"
    "      } else if (distr > 0.5) {\n"  // ps_distr_gaussian: 中心密集(Box-Muller)
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

    "    float4 T0 = tex2D(sType, float2((type + 0.5) / 256.0, 0.5 / 14.0));\n"
    "    float4 T1 = tex2D(sType, float2((type + 0.5) / 256.0, 1.5 / 14.0));\n"
    "    float4 T3 = tex2D(sType, float2((type + 0.5) / 256.0, 3.5 / 14.0));\n"
    "    float4 T4 = tex2D(sType, float2((type + 0.5) / 256.0, 4.5 / 14.0));\n"
    "    float4 T5 = tex2D(sType, float2((type + 0.5) / 256.0, 5.5 / 14.0));\n"
    "    float spd = lerp(T1.x, T1.y, rnd.y);\n"
    "    float dir = lerp(T1.z, T1.w, rnd.z);\n"
    "    float rad = dir * DEG2RAD;\n"
    "    vel = spd * float2(cos(rad), -sin(rad));\n"
    "    age = 0.0;\n"
    "    life = lerp(T0.x, T0.y, rnd.x);\n"

    "    if (b3.w > 0.5) { base = b3.rgb; }\n"
    "    else if (T3.z > 3.5) {\n"
    "      float cr = h1(id + seed * 17.0 + 31.7);\n"   // 颜色独立随机(与方向/速度解耦)
    "      if (T3.z < 4.5) { base = lerp(T4.rgb, float3(T4.w, T5.x, T5.y), cr); }\n"
    "      else if (T3.z < 5.5) { base = lerp(T4.rgb, T5.rgb, cr); }\n"
    "      else {\n"
    "        float3 hsv = lerp(T4.rgb, T5.rgb, cr);\n"
    "        base = hsv2rgb(hsv * float3(360.0/255.0, 1.0/255.0, 1.0/255.0));\n"
    "      }\n"
    "    } else { base = T4.rgb; }\n"
    "    has_ovr = b3.w;\n"

    "    float4 T8 = tex2D(sType, float2((type + 0.5) / 256.0, 8.5 / 14.0));\n"
    "    float4 T9 = tex2D(sType, float2((type + 0.5) / 256.0, 9.5 / 14.0));\n"
    "    float nf = T8.y;\n"
    "    frame = (T9.x > 0.5 && nf > 1.0) ? floor(h1(id + seed * 17.0 + 9.0) * nf) : 0.0;\n"
    "  } else if (dead < 0.5) {\n"
    "    float dt = uMode.x > 0.5 ? 0.0 : uGlobal.y;\n"   // 仅出生 pass: 不推进物理/老化
    "    float nage = st.x + dt;\n"
    "    if (nage >= st.y) {\n"
           // 自然死亡: 立即清空(防僵尸粒子继续积分飞远, 污染 step/death 源槽读取)
    "      pos = 0; vel = 0; type = 0; base = float3(1,1,1); has_ovr = 0;\n"
    "      age = 1.0; life = 0.0; frame = 0.0;\n"
    "    } else {\n"
    "    pos = prev.xy;\n"
    "    vel = prev.zw;\n"
    "    age = nage;\n"
    "    life = st.y;\n"
    "    type = st.z;\n"
    "    base = ov.rgb;\n"
    "    has_ovr = ov.w;\n"
    "    float4 T2 = tex2D(sType, float2((type + 0.5) / 256.0, 2.5 / 14.0));\n"
    "    float g = T2.y;\n"
    "    float ga = T2.x * DEG2RAD;\n"
    "    vel += g * dt * float2(cos(ga), -sin(ga));\n"
    "    vel *= max(1.0 - clamp(T2.z, 0.0, 1.0) * dt, 0.0);\n"
    "    float4 T13 = tex2D(sType, float2((type + 0.5) / 256.0, 13.5 / 14.0));\n"
         // 速度/方向更新(与引擎 sub_4BDA50 一致):
         // speed 每步 += speed_incr 后 clamp≥0; 移动前 speed += (tri((age+4φ)%20)/5 - 1)×speed_wiggle;
         // direction 每步 += dir_incr; 移动前 direction += (tri((age+3φ)%24)/6 - 1)×dir_wiggle。
         // tri(x) = x>2 ? 4-x : x; 两 wave 均 -1 → 值域 [-1,1) 对称平均 0(摆动无漂移)。
    "    float len = max(length(vel) + T13.z * dt, 0.0);\n"
    "    vel = vel * (len / max(length(vel), 0.0001));\n"
         // attractor 力(引擎 sub_4BDA50): 距离 ≤dist 内加力; kind 0=恒定 1=线性 2=二次衰减;
         // additive=true 叠加到速度, false 只做位置修正。最多 4 个(ps_3_0 展开预算)。
    "    float2 acc_pos = 0;\n"
    "    for (int aa = 0; aa < 4; ++aa) {\n"
    "      if (aa >= uEff.x) break;\n"
    "      float2 ac = tex2D(sEff, float2((aa + 0.5) / 64.0, 0.5 / 6.0)).xy;\n"
    "      float2 af = tex2D(sEff, float2((aa + 0.5) / 64.0, 0.5 / 6.0)).zw;\n"
    "      float4 as = tex2D(sEff, float2((aa + 0.5) / 64.0, 1.5 / 6.0));\n"
    "      if (as.x < 0.5) continue;\n"
    "      float2 d = ac - pos;\n"
    "      float dist = length(d);\n"
    "      if (dist <= af.y && dist > 0.0 && af.x != 0.0 && af.y != 0.0) {\n"
    "        float2 f = af.x * d / dist;\n"
    "        if (as.y > 0.5 && as.y < 1.5) { float k = (af.y - dist) / af.y; f *= k; }\n"
    "        else if (as.y > 1.5) { float k = (af.y - dist) / af.y; f *= k * k; }\n"
    "        if (as.z > 0.5) vel += f; else acc_pos += f;\n"
    "      }\n"
    "    }\n"
         // 速度摆动: 每步随机 ±wiggle, 当步位移抖动(不进速度状态, 无累积/整流; GMParty 同模式)
    "    float swing = (h1(id + floor(age) * 7.31) * 2.0 - 1.0) * T13.w * dt;\n"
         // 方向摆动: 三角波(部分和有界 ±3×wiggle, 与引擎 sub_4BDA50 的 waveA 一致)
    "    float dw = fmod(h1(id + 23.0) * 24.0 + age, 24.0) / 6.0;\n"
    "    dw = dw > 2.0 ? 4.0 - dw : dw;\n"
    "    float da = T13.x * dt + (dw - 1.0) * T13.y;\n"
    "    da *= DEG2RAD;\n"
    "    float ca2 = cos(da), sa2 = sin(da);\n"
    "    vel = float2(vel.x * ca2 + vel.y * sa2, -vel.x * sa2 + vel.y * ca2);\n"
    "    pos += vel * dt + (vel / max(length(vel), 0.0001)) * swing + acc_pos;\n"
         // deflector(引擎 sub_4BDED4): 区域内方向反射 + 位置镜像 + friction 减速。
         // kind==1 horizontal(偏转水平速度): direction=180-dir → vel.x 取反 + x 镜像;
         // kind!=1 vertical(偏转垂直速度): direction=360-dir → vel.y 取反 + y 镜像。
    "    for (int de = 0; de < 4; ++de) {\n"
    "      if (de >= uEff.z) break;\n"
    "      float4 dr = tex2D(sEff, float2((de + 0.5) / 64.0, 4.5 / 6.0));\n"
    "      float4 ds = tex2D(sEff, float2((de + 0.5) / 64.0, 5.5 / 6.0));\n"
    "      if (ds.x < 0.5) continue;\n"
    "      if (dr.x >= dr.y || dr.z >= dr.w) continue;\n"
    "      if (pos.x >= dr.x && pos.x <= dr.y && pos.y >= dr.z && pos.y <= dr.w) {\n"
    "        float cl = length(vel);\n"
    "        if (ds.y > 0.5) { vel.x = -vel.x; pos.x = prev.x - (pos.x - prev.x); }\n"
    "        else { vel.y = -vel.y; pos.y = prev.y - (pos.y - prev.y); }\n"
    "        float nl = max(cl - ds.z, 0.0);\n"
    "        if (cl > 0.0001) vel *= nl / cl;\n"
    "      }\n"
    "    }\n"
         // destroyer(引擎 sub_4BE394): 区域内(rect/ellipse/diamond)立即销毁;
    "    float4 T11 = tex2D(sType, float2((type + 0.5) / 256.0, 11.5 / 14.0));\n"
    "    for (int de2 = 0; de2 < 4; ++de2) {\n"
    "      if (de2 >= uEff.y) break;\n"
    "      float4 dr2 = tex2D(sEff, float2((de2 + 0.5) / 64.0, 2.5 / 6.0));\n"
    "      float4 ds2 = tex2D(sEff, float2((de2 + 0.5) / 64.0, 3.5 / 6.0));\n"
    "      if (ds2.x < 0.5) continue;\n"
    "      if (dr2.x >= dr2.y || dr2.z >= dr2.w) continue;\n"
    "      if (pos.x >= dr2.x && pos.x <= dr2.y && pos.y >= dr2.z && pos.y <= dr2.w) {\n"
    "        float nx = 2.0 * (pos.x - (dr2.y + dr2.x) * 0.5) / (dr2.y - dr2.x);\n"
    "        float ny = 2.0 * (pos.y - (dr2.w + dr2.z) * 0.5) / (dr2.w - dr2.z);\n"
    "        float hit = ds2.y < 0.5 ? 1.0\n"
    "          : (ds2.y > 0.5 && ds2.y < 1.5) ? (nx * nx + ny * ny <= 1.0 ? 1.0 : 0.0)\n"
    "          : (abs(nx) + abs(ny) <= 1.0 ? 1.0 : 0.0);\n"
    "        if (hit > 0.5 && T11.z < 0.5) { age = 1.0; life = 0.0; }\n"   // 立即销毁(age>=life 剔除)
    "      }\n"
    "    }\n"
    "    float4 T8 = tex2D(sType, float2((type + 0.5) / 256.0, 8.5 / 14.0));\n"
    "    float nf = T8.y;\n"
    "    frame = st.w;\n"
    "    if (T8.z > 0.5 && nf > 1.0)\n"
    "      frame = T8.w > 0.5\n"
    "        ? min(nf - 1.0, floor(age / max(life, 0.0001) * nf))\n"
    "        : fmod(age, nf);\n"
    "    }\n"
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
    "sampler sOvr : register(s0);\n"
    "sampler sPos : register(s1);\n"
    "sampler sLife : register(s2);\n"
    "sampler sType : register(s3);\n"
    "float4x4 uWVP : register(c0);\n"
    "float4 uSys : register(c4);\n"
    "float4 uBlend : register(c5);\n"
    "struct VSIN { float3 c : TEXCOORD0; };\n"   // c.xy = 角点{0,1}, c.z = 粒子 id
    "struct VSOUT {\n"
    "  float4 pos : POSITION;\n"
    "  float2 cuv : TEXCOORD0;\n"    // 角点 uv(插值 = 粒子内 0..1)
    "  float2 tinfo : TEXCOORD1;\n"  // type, frame
    "  float4 col : COLOR0;\n"       // rgb = 颜色, a = alpha
    "};\n"

    "float h1(float a) {\n"
    "  a = frac(a * 0.1031);\n"
    "  a = frac(a * (a + 19.19));\n"
    "  a = frac(a * (a + 33.33));\n"
    "  return frac(a * 43758.5453);\n"
    "}\n"

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
    "  float4 T0 = tex2Dlod(sType, float4(tuv.x, 0.5 / 14.0, 0, 0));\n"
    "  float4 T2 = tex2Dlod(sType, float4(tuv.x, 2.5 / 14.0, 0, 0));\n"
    "  float4 T3 = tex2Dlod(sType, float4(tuv.x, 3.5 / 14.0, 0, 0));\n"
    "  float4 T4 = tex2Dlod(sType, float4(tuv.x, 4.5 / 14.0, 0, 0));\n"
    "  float4 T5 = tex2Dlod(sType, float4(tuv.x, 5.5 / 14.0, 0, 0));\n"
    "  float4 T6 = tex2Dlod(sType, float4(tuv.x, 6.5 / 14.0, 0, 0));\n"
    "  float4 T7 = tex2Dlod(sType, float4(tuv.x, 7.5 / 14.0, 0, 0));\n"
    "  float4 T8 = tex2Dlod(sType, float4(tuv.x, 8.5 / 14.0, 0, 0));\n"
    "  float4 ov = tex2Dlod(sOvr, float4(uv, 0, 0));\n"
    "  float4 T12 = tex2Dlod(sType, float4(tuv.x, 12.5 / 14.0, 0, 0));\n"
       // GM8 尺寸语义: size0 = 随机[min,max] + incr*age(clamp≥0); wiggle = ±wiggle 三角波摆动
    "  float size0 = max(lerp(T0.z, T0.w, h1(id + 3.0)) + T12.y * age, 0.0);\n"
    "  float swv = fmod(h1(id + 9.0) * 16.0 + age, 16.0) / 4.0;\n"
    "  swv = swv > 2.0 ? 4.0 - swv : swv;\n"
    "  float size = size0 + (swv - 1.0) * T12.z;\n"
    "  float2 psize = size * float2(T3.x, T3.y) * T12.x;\n"
    "  float ang = lerp(T7.x, T7.y, h1(id + 5.0)) + T7.z * age;\n"
       // 角度摆动: 三角波(与引擎绘制 rot 的 wave 一致, mod 16 折返, 部分和有界)
    "  float owv = fmod(h1(id + 31.0) * 16.0 + age, 16.0) / 4.0;\n"
    "  owv = owv > 2.0 ? 4.0 - owv : owv;\n"
    "  ang += (owv - 1.0) * T7.w;\n"
    "  if (T8.x > 0.5) ang += atan2(-pl.w, pl.z) * 57.29577951308232;\n"
    "  ang *= 0.017453292519943295;\n"
    "  float ca = cos(ang), sa = sin(ang);\n"
    "  float2 corner = (v.c.xy * 2.0 - 1.0) * psize * 0.5;\n"
    "  float2 off = float2(ca * corner.x - sa * corner.y, sa * corner.x + ca * corner.y);\n"
    "  float4 clip = mul(uWVP, float4(pl.xy + uSys.xy + off, 0, 1));\n"
    "  o.pos = dead > 0.5 ? float4(2.0, 2.0, 0.5, 1.0) : clip;\n" // 全部角点同点，零面积三角形被剔除
    "  o.cuv = v.c.xy;\n"
    "  o.tinfo = float2(type, frame);\n"

    "  float t = life > 0.0001 ? clamp(age / life, 0.0, 1.0) : 1.0;\n"
    "  float3 col;\n"
    "  float mode = T3.z;\n"
    "  if (ov.w > 0.5 || mode > 3.5) col = ov.rgb;\n"
    "  else if (mode < 1.5) col = T4.rgb;\n"
    "  else if (mode < 2.5) col = lerp(T4.rgb, float3(T4.w, T5.x, T5.y), t);\n"
    "  else col = t < 0.5 ? lerp(T4.rgb, float3(T4.w, T5.x, T5.y), t * 2.0)\n"
    "    : lerp(float3(T4.w, T5.x, T5.y), float3(T5.z, T5.w, T6.x), (t - 0.5) * 2.0);\n"
    "  float a;\n"
    "  float am = T3.w;\n"
    "  if (am < 1.5) a = T6.y;\n"
    "  else if (am < 2.5) a = lerp(T6.y, T6.z, t);\n"
    "  else a = t < 0.5 ? lerp(T6.y, T6.z, t * 2.0) : lerp(T6.z, T6.w, (t - 0.5) * 2.0);\n"
    "  if (abs(T2.w - uBlend.x) > 0.5) a = 0.0;\n"   // 混合模式不匹配当前遍 → 零贡献
    "  o.col = float4(col, a);\n"
    "  return o;\n"
    "}\n";

static const char* RND_PS_HLSL =
    "sampler sMain : register(s0);\n"
    "sampler sRect : register(s5);\n"
    "float4 uBlend : register(c5);\n"   // .x = 当前遍混合模式(0=普通, 1=加法)
    "float4 main(float2 cuv : TEXCOORD0, float2 tinfo : TEXCOORD1, float4 col : COLOR0) : COLOR0 {\n"
    "  float4 rect = tex2D(sRect, float2((tinfo.x + 0.5) / 256.0, (tinfo.y + 0.5) / 32.0));\n"
    "  float2 auv = rect.xy + rect.zw * cuv;\n"
    "  float4 tex = tex2D(sMain, auv);\n"
    "  float a = tex.a * col.a;\n"
    // uBlend.x = 当前遍(0=普通, 1=加色); uBlend.y = 预乘输出(1=预乘管线/自动检测到 ONE 混合)。
    // 加色遍或预乘模式 → rgb *= a(预乘); 否则 straight(默认 SRCALPHA 管线)。
    "  float3 rgb = tex.rgb * col.rgb;\n"
    "  rgb *= (uBlend.x > 0.5 || uBlend.y > 0.5) ? a : 1.0;\n"
    "  return float4(rgb, a);\n"
    "}\n";


// ============================================================================
// GPU 资源初始化(惰性, 失败置 g_gpu_failed)
// ============================================================================

// 预加载: 调用 GM8 引擎 sub_4BB120(生成 14 个形状精灵并填充形状表数组)。
// 地址 0x4BB120 实测固定(空工程与 Nature Edition 一致, 引擎同一编译模板;
// .data 基址 0x189000 稳定, 与 draw_text.cpp 读引擎全局同模式)。
// 仅当引擎标志未置位时调用(幂等)。若调用时引擎子系统未就绪, 形状表由引擎
// 自身的 part_system_create 填充, ensure_gm8_shapes() 每帧重试兜底。
static void call_gm8_shape_init()
{
    if (*(volatile BYTE*)0x58D5A0) return;   // 引擎已生成
    typedef void(__cdecl* fn_t)();
    ((fn_t)0x4BB120)();
}

// 从 GM8 引擎内置形状精灵表抓取位图(固定地址, 与 draw_text.cpp 读引擎全局同模式)。
// 0x58F3A0 处是一个 DWORD 变量, 其值 = 精灵指针数组基址(引擎 sub_4BB120 在
// part_system_create 时填充; 汇编: mov edx, off_58F3A0; mov edi, [edx+eax*4])。
// 精灵结构: +4 = subimageCount, +8 = 帧数据指针数组, [0] = 帧数据。
// 帧数据: +4 = 宽, +8 = 高, +12 = 像素数据指针(DWORD/像素, 实测 0xAARRGGBB)。
// 实测形状精灵为 64×64(origin 32,32 = 中心)。成功返回 [R][G][B][A] 像素与宽高。
static bool grab_gm8_shape(int shape, std::vector<BYTE>& out, int& ow, int& oh)
{
    constexpr DWORD GM8_SHAPE_TABLE = 0x58F3A0;   // 变量: 值 = 精灵指针数组基址
    try
    {
        DWORD array_base = *(DWORD*)GM8_SHAPE_TABLE;
        if (!array_base) return false;
        BYTE* spr = *(BYTE**)(array_base + 4 * shape);
        if (!spr) return false;
        BYTE** frames = *(BYTE***)(spr + 8);
        if (!frames || !frames[0]) return false;
        BYTE* frame = frames[0];
        int w = *(int*)(frame + 4);
        int h = *(int*)(frame + 8);
        DWORD* px = *(DWORD**)(frame + 12);
        if (!px || w < 1 || h < 1 || w > 128 || h > 128) return false;
        out.resize((size_t)w * h * 4);
        for (int i = 0; i < w * h; ++i)
        {
            DWORD d = px[i];
            out[(size_t)i * 4 + 0] = (BYTE)((d >> 16) & 0xFF);   // R
            out[(size_t)i * 4 + 1] = (BYTE)((d >> 8) & 0xFF);    // G
            out[(size_t)i * 4 + 2] = (BYTE)(d & 0xFF);            // B
            out[(size_t)i * 4 + 3] = (BYTE)((d >> 24) & 0xFF);    // A
        }
        ow = w;
        oh = h;
        return true;
    }
    catch (...) { return false; }
}

// 双线性缩放到 64×64 图集 tile(任意输入尺寸)
static void upscale_to_tile(const std::vector<BYTE>& src, int sw, int sh, std::vector<BYTE>& d64)
{
    d64.assign((size_t)GP_ATLAS_TILE * GP_ATLAS_TILE * 4, 0);
    const int D = GP_ATLAS_TILE;
    for (int y = 0; y < D; ++y)
        for (int x = 0; x < D; ++x)
        {
            float fx = (x + 0.5f) * (float)sw / (float)D - 0.5f;
            float fy = (y + 0.5f) * (float)sh / (float)D - 0.5f;
            int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
            float tx = fx - x0, ty = fy - y0;
            int x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
            x0 = std::max(x0, 0); y0 = std::max(y0, 0);
            for (int c = 0; c < 4; ++c)
            {
                float v = (1 - ty) * ((1 - tx) * src[((size_t)y0 * sw + x0) * 4 + c] + 
                    tx * src[((size_t)y0 * sw + x1) * 4 + c]) + ty * ((1 - tx) * 
                    src[((size_t)y1 * sw + x0) * 4 + c] + tx * src[((size_t)y1 * sw + x1) * 4 + c]);
                d64[((size_t)y * D + x) * 4 + c] = (BYTE)(v + 0.5f);
            }
        }
}

static bool g_gm8_shapes_grabbed = false;   // GM8 形状精灵抓取完成标志

// 尝试抓取全部 14 个形状覆盖图集 tile(引擎 sub_4BB120 由 CPU part_system_create
// 触发, 时序不确定; 每帧尝试直到成功, 消除粒子延迟显现的几十帧空窗)。
static void ensure_gm8_shapes()
{
    if (g_gm8_shapes_grabbed || !g_atlas_tex) return;
    bool all = true;
    for (int s = 0; s < PT_SHAPE_COUNT; ++s)
    {
        std::vector<BYTE> src, d64;
        int w = 0, h = 0;
        if (!grab_gm8_shape(s, src, w, h))
        {
            all = false;
            break;
        }
        upscale_to_tile(src, w, h, d64);
        int ax = (s % 16) * GP_ATLAS_TILE, ay = (s / 16) * GP_ATLAS_TILE;
        D3DCheck(d3d::upload_texture_rect(g_atlas_tex, ax, ay,
            GP_ATLAS_TILE, GP_ATLAS_TILE, D3DFMT_A8R8G8B8, d64.data(), GP_ATLAS_TILE * 4), 1);
    }
    if (all)
        g_gm8_shapes_grabbed = true;
}

// ---- shader 字节码缓存(现代预编译: 跳过 d3dcompiler 的 HLSL→asm 编译) ----
// 缓存文件: <dir>\gpart_shader_<name>.bin, 头 16 字节 = magic("GPCS") + src_hash(XXH64) + len。
// src_hash = XXH64(shader 源串 + profile), shader 改动自动失效 → 重新编译覆盖。
// DX9 asm bytecode 与驱动无关(驱动每次创建时即时编 ISA), 驱动更新无需重编译。
static bool shader_cache_read(const char* dir, const char* name, xxh::hash64_t src_hash,
    std::vector<BYTE>& out)
{
    if (!dir || dir[0] == '\0')
        return false;

    std::ifstream f(std::string(dir) + "\\gpart_shader_" + name + ".bin", std::ios::binary);
    if (!f) return false;

    unsigned magic = 0, len = 0;
    xxh::hash64_t sh = 0;
    if (!f.read(reinterpret_cast<char*>(&magic), 4)
        || !f.read(reinterpret_cast<char*>(&sh), 8)
        || !f.read(reinterpret_cast<char*>(&len), 4)
        || magic != 0x43535047u || sh != src_hash || len == 0 || len >= (1u << 20))
        return false;

    out.resize(len);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), len));
}

static void shader_cache_write(const char* dir, const char* name, xxh::hash64_t src_hash,
    const void* data, size_t len)
{
    if (!dir || dir[0] == '\0')
        return;

    CreateDirectoryA(dir, nullptr);   // 已存在则失败, 忽略
    std::ofstream f(std::string(dir) + "\\gpart_shader_" + name + ".bin",
        std::ios::binary | std::ios::trunc);
    if (!f) return;

    unsigned magic = 0x43535047u, l = static_cast<unsigned>(len);
    f.write(reinterpret_cast<const char*>(&magic), 4);
    f.write(reinterpret_cast<const char*>(&src_hash), 8);
    f.write(reinterpret_cast<const char*>(&l), 4);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
}

static bool gpu_init_internal(const char* cache_dir)
{
    if (g_gpu_ready || g_gpu_failed) return g_gpu_ready;
    try
    {
        d3d::Caps caps;
        if (!d3d::get_caps(caps))
            throw std::runtime_error("无法读取设备能力。");
        if (caps.vertex_tex_filter_caps == 0)
            throw std::runtime_error("显卡不支持顶点纹理采样(VTF), gpart 不可用。");

        // 类型表纹理 256x13 A16B16G16R16F
        D3DCheck(d3d::create_texture(GP_TYPE_TEX_W, GP_TYPE_ROWS, 1, 0,
            GP_FMT_16F, D3DPOOL_DEFAULT, &g_type_tex), 1);
        std::vector<unsigned short> zero((size_t)GP_TYPE_TEX_W * GP_TYPE_ROWS * 4, 0);
        D3DCheck(d3d::upload_texture(g_type_tex, GP_TYPE_TEX_W, GP_TYPE_ROWS,
            GP_FMT_16F, zero.data(), GP_TYPE_TEX_W * 8), 2);

        // 粒子图集 1024x1024 A8R8G8B8: 形状 tile 由 GM8 引擎形状精灵填充(第 0 行 64x64 网格)
        D3DCheck(d3d::create_texture(GP_ATLAS_SIZE, GP_ATLAS_SIZE, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_atlas_tex), 3);
        g_atlas_x = 0;
        g_atlas_y = GP_ATLAS_TILE;   // 形状占满第 0 行, 精灵从第 1 行开始分配
        g_atlas_row_h = 0;
        call_gm8_shape_init();       // 预加载: 主动触发引擎形状精灵生成
        ensure_gm8_shapes();         // 抓取 GM8 引擎形状精灵

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

        // shader 编译(带字节码缓存: 命中直接读文件, 未命中编译并写缓存)
        auto load_or_compile = [&](const char* name, const char* src, const char* entry,
            const char* profile, std::vector<BYTE>& code)
        {
			std::string src_str(src);
            std::string key = src_str + "|" + profile;
            xxh::hash64_t h = xxh::xxhash<64>(key.data(), key.size());
            if (shader_cache_read(cache_dir, name, h, code))
                return;   // 缓存命中, 跳过编译

            std::string err;
            void* table = nullptr;
            HRESULT hr = d3d::compile_hlsl(src_str.data(), src_str.length(), entry, 
                profile, code, &table, &err);
            if (table) d3d::release(table);
            if (FAILED(hr))
                throw std::runtime_error("gpart shader 编译失败 (" + std::string(entry) + "): " + err);

            shader_cache_write(cache_dir, name, h, code.data(), code.size());
        };

        std::vector<BYTE> code;
        load_or_compile("evo_vs", EVO_VS_HLSL, "main", "vs_3_0", code);
        D3DCheck(d3d::create_vertex_shader(d3d::VERT_DEFAULT, code.data(), nullptr, 0, &g_evo_vs), 9);
        load_or_compile("evo_ps", EVO_PS_HLSL, "main", "ps_3_0", code);
        D3DCheck(d3d::create_pixel_shader(code.data(), &g_evo_ps), 10);
        load_or_compile("rnd_vs", RND_VS_HLSL, "main", "vs_3_0", code);
        D3DCheck(d3d::create_vertex_shader(d3d::VERT_DEFAULT, code.data(), nullptr, 0, &g_rnd_vs), 11);
        load_or_compile("rnd_ps", RND_PS_HLSL, "main", "ps_3_0", code);
        D3DCheck(d3d::create_pixel_shader(code.data(), &g_rnd_ps), 12);

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
                px[((size_t)r * GP_TYPE_TEX_W + id) * 4 + c] =
                    f2h(gt.row(static_cast<GTypeRow>(r))[c]);
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
    // 特效器表 64x6 A16B16G16R16F(每特效器 2 行: 行0/1=attractor, 行2/3=destroyer, 行4/5=deflector)
    D3DCheck(d3d::create_texture(GP_EFF_TEX_W, GP_EFF_TEX_H, 1, 0,
        GP_FMT_16F, D3DPOOL_DEFAULT, &s.eff_tex), 12);
    std::vector<unsigned short> ez((size_t)GP_EFF_TEX_W * GP_EFF_TEX_H * 4, 0);
    D3DCheck(d3d::upload_texture(s.eff_tex, GP_EFF_TEX_W, GP_EFF_TEX_H,
        GP_FMT_16F, ez.data(), GP_EFF_TEX_W * 8), 13);
}

static void system_tex_destroy(GSystem& s)
{
    for (int k = 0; k < 3; ++k)
        for (int p = 0; p < 2; ++p)
        {
            if (s.surf[k][p]) d3d::release(s.surf[k][p]);
            if (s.tex[k][p]) d3d::release(s.tex[k][p]);
        }
    if (s.eff_tex) { d3d::release(s.eff_tex); s.eff_tex = nullptr; }
    if (s.id_vb) { d3d::release(s.id_vb); s.id_vb = nullptr; }
}

// 打包当前系统特效器到 eff_tex(行0/1=attractor, 行2/3=destroyer, 行4/5=deflector)。
// 每特效器 1 列 × 2 行: 行0/2/4 = 几何参数, 行1/3/5 = (active, kind/shape, 附加, 0)。
// 只上传前 GP_EFF_MAX 个(ps_3_0 展开预算), 超出的特效器静默忽略。
static void upload_effectors(GSystem& s)
{
    std::vector<unsigned short> px((size_t)GP_EFF_TEX_W * GP_EFF_TEX_H * 4, 0);
    auto put = [&](int row, int col, float x, float y, float z, float w)
    {
        unsigned short* p = &px[((size_t)row * GP_EFF_TEX_W + col) * 4];
        p[0] = f2h(x); p[1] = f2h(y); p[2] = f2h(z); p[3] = f2h(w);
    };
    int ai = 0;
    for (auto& kv : s.attractors)
    {
        if (ai >= GP_EFF_MAX) break;
        const GAttractor& a = kv.second;
        put(0, ai, a.x, a.y, a.force, a.dist);
        put(1, ai, 1.0f, (float)a.kind, a.additive ? 1.0f : 0.0f, 0.0f);
        ++ai;
    }
    int di = 0;
    for (auto& kv : s.destroyers)
    {
        if (di >= GP_EFF_MAX) break;
        const GDestroyer& d = kv.second;
        put(2, di, d.xmin, d.xmax, d.ymin, d.ymax);
        put(3, di, 1.0f, (float)d.shape, 0.0f, 0.0f);
        ++di;
    }
    int fi = 0;
    for (auto& kv : s.deflectors)
    {
        if (fi >= GP_EFF_MAX) break;
        const GDeflector& d = kv.second;
        put(4, fi, d.xmin, d.xmax, d.ymin, d.ymax);
        put(5, fi, 1.0f, (float)d.kind, d.friction, 0.0f);
        ++fi;
    }
    D3DCheck(d3d::upload_texture(s.eff_tex, GP_EFF_TEX_W, GP_EFF_TEX_H,
        GP_FMT_16F, px.data(), GP_EFF_TEX_W * 8), 14);
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
            float life = (float)lerp(gt.life_min(), gt.life_max(),
                gphashf((float)slot + b.seed * 17.0f));
            s.s_life[slot] = life;
            int frames = (int)gt.frame_rect.size();
            if (gt.random_frame && frames > 0)
                s.s_frame[slot] = (int)(gphashf((float)slot + b.seed * 17.0f + 9.0f) * frames) % frames;
            else
                s.s_frame[slot] = 0;
            // 系统混合掩码(静态路径判定用)
            s.blend_mask |= (gt.additive() > 0.5f) ? 2 : 1;
        }
        else
        {
            s.s_life[slot] = 30.0f;
            s.s_frame[slot] = 0;
        }
    }
    s.cursor = (s.cursor + n) % s.capacity;
}

// 源槽位生成批次(part_type_step/death): 目的地槽位的新粒子位置 = 源粒子当前位置(GPU 读取)
static void queue_source_spawn(GSystem& s, int type, int source_slot, int n)
{
    if (n <= 0 || s.capacity <= 0) return;
    SpawnBatch b;
    b.shape = -2.0f;                 // 源槽位模式(b2.x < -1.5)
    b.distr = 0;
    b.px = (float)source_slot;
    queue_spawn(s, type, n, b);
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
    UINT vp_w = 0, vp_h = 0;   // viewport(演化 pass 改 256x256, 必须还原)
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
    d3d::get_viewport(&r.vp_w, &r.vp_h);
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
    d3d::set_viewport(r.vp_w, r.vp_h);
}
// 纹理 stage 绑定保存/恢复(0..5 的 texture)。注意覆盖到 stage 5(矩形表),
// 否则残留绑定会污染 ext 多纹理绘制(TEX8)。
// 重要: 绝不读写 D3DTSS_ 过滤/寻址状态 —— D3D9 固定管线(FVF 无 shader)的过滤只认
// D3DTSS_MINFILTER/MAGFILTER, 而 GMDirectX9 把引擎的过滤设置 patch 到了 SetSamplerState
// (D3DSAMP_)。GetTextureStageState 读回的是从未被写过的 D3DTSS 影子表(陈旧默认
// MIN/MAG=LINEAR/WRAP), 若再 SetTextureStageState 写回会把 LINEAR 写进共享底层状态,
// 污染引擎后续固定管线绘制(纹理变双线性平滑)。shader 路径不读 D3DTSS, 无需保存过滤。
static const int GP_STAGES = 6;
static void stages_save_textures(void* tex[GP_STAGES])
{
    for (int i = 0; i < GP_STAGES; ++i)
    {
        tex[i] = nullptr;
        d3d::get_texture(i, &tex[i]);
    }
}
static void stages_restore_textures(void* tex[GP_STAGES])
{
    for (int i = 0; i < GP_STAGES; ++i)
        d3d::set_texture(i, tex[i]);
}
// 渲染状态 + 纹理绑定 RAII 守卫: 任何返回/异常路径都还原(rs + 纹理绑定)。
// 过滤/寻址(D3DTSS_)完全不碰, 避免污染引擎固定管线(见上)。
struct RenderStateGuard
{
    RsSave rs;
    void* tex[GP_STAGES] = {};
    RenderStateGuard()
    {
        rs_save(rs);
        stages_save_textures(tex);
    }
    ~RenderStateGuard()
    {
        stages_restore_textures(tex);
        rs_restore(rs);
    }
};

static void run_evolution(GSystem& s, const std::vector<SpawnBatch>& batches, int count, 
    bool spawn_only = false)
{
    if (count < 0) return;   // count == 0 合法: 纯老化 pass(无出生分支)
    RenderStateGuard rsg;    // RAII: rs + stages 保存, 析构无条件还原(含异常路径)

    int w = s.cur, dst = s.cur ^ 1;
    D3DCheck(d3d::set_render_target(0, s.surf[0][dst]), 1);
    D3DCheck(d3d::set_render_target(1, s.surf[1][dst]), 2);
    D3DCheck(d3d::set_render_target(2, s.surf[2][dst]), 3);
    D3DCheck(d3d::set_viewport(GP_GRID, GP_GRID), 4);
    D3DCheck(d3d::set_render_state(D3DRS_ZENABLE, FALSE), 5);
    D3DCheck(d3d::set_render_state(D3DRS_ALPHABLENDENABLE, FALSE), 6);
    D3DCheck(d3d::set_render_state(D3DRS_POINTSPRITEENABLE, FALSE), 7);

    D3DCheck(d3d::set_texture(0, s.tex[0][w]), 8);
    D3DCheck(d3d::set_texture(1, s.tex[1][w]), 9);
    D3DCheck(d3d::set_texture(2, s.tex[2][w]), 10);
    D3DCheck(d3d::set_texture(3, g_type_tex), 11);
    upload_effectors(s);                     // 特效器表打包上传(每步, 64x6 极小)
    D3DCheck(d3d::set_texture(4, s.eff_tex), 12);

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
    float mode[4] = { spawn_only ? 1.0f : 0.0f, 0, 0, 0 };
    D3DCheck(d3d::set_ps_const_typed(EVO_C_MODE, d3d::CK_FLOAT, mode, 1), 19);
    // spawn_only(仅出生分块)时特效器计数置 0: 特效器循环立即 break, 不影响存活粒子
    float eff[4] = { 0, 0, 0, 0 };
    if (!spawn_only)
    {
        eff[0] = (float)std::min((int)s.attractors.size(), GP_EFF_MAX);
        eff[1] = (float)std::min((int)s.destroyers.size(), GP_EFF_MAX);
        eff[2] = (float)std::min((int)s.deflectors.size(), GP_EFF_MAX);
    }
    D3DCheck(d3d::set_ps_const_typed(EVO_C_EFF, d3d::CK_FLOAT, eff, 1), 20);

    for (int b = 0; b < count; ++b)
    {
        D3DCheck(d3d::set_ps_const_typed(EVO_C_BATCHES + b * 4, d3d::CK_FLOAT,
            (const float*)&batches[b], 4), 20);
    }

    D3DCheck(d3d::draw_primitive(D3DPT_TRIANGLESTRIP, 2, 0), 20);

    s.cur = dst;
    // 注意: s.now 不在这里推进! 一次 update 可能切块跑多次演化 pass,
    // 时钟只能推进一次(由 gpart_system_update 统一推进), 否则多批次时加速。
    // (rsg 析构在此函数末尾还原渲染状态/纹理阶段)
}

// ============================================================================
// 渲染 pass
// ============================================================================
// 重建渲染数据: 活跃窗口过滤(原地)+ 按出生序双桶(普通/加法)+ 图集矩形, 无排序
static void run_render(GSystem& s)
{
    // 快速跳过: 从未发射过/已清空
    if (s.live_window.empty()) return;

    RenderStateGuard rsg;    // RAII: rs + stages 保存, 析构无条件还原(含异常路径)
    // 图集采样用 POINT(像素游戏风格, 与游戏纹理一致): 完全不改任何采样器过滤状态,
    // 从根源避免污染引擎/后续绘制的采样过滤(此前 LINEAR 方案无论怎么恢复都有泄漏风险)。

    // 渲染状态: 三角形管线(四边形粒子), 无点精灵依赖
    D3DCheck(d3d::set_render_state(D3DRS_ZENABLE, FALSE), 1);
    D3DCheck(d3d::set_render_state(D3DRS_CULLMODE, D3DCULL_NONE), 2);

    // 纹理绑定: PS sMain=stage0(图集), PS sRect=stage5(矩形表);
    // VS(VTF 独立槽位): sOvr=257, sPos=258, sLife=259, sType=260
    int w = s.cur;
    D3DCheck(d3d::set_texture(0, g_atlas_tex), 6);
    D3DCheck(d3d::set_texture(5, g_rect_tex), 11);
    D3DCheck(d3d::set_texture(GP_VTS0 + 0, s.tex[2][w]), 100);   // VS sOvr(覆盖色)
    D3DCheck(d3d::set_texture(GP_VTS0 + 1, s.tex[0][w]), 101);   // VS sPos(位置/速度)
    D3DCheck(d3d::set_texture(GP_VTS0 + 2, s.tex[1][w]), 102);   // VS sLife(age/life/type/frame)
    D3DCheck(d3d::set_texture(GP_VTS0 + 3, g_type_tex), 103);    // VS sType(类型表)

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

    // 混合: 加法 = ONE/ONE(预乘输出); 普通遍按预乘模式选 ONE/INVSRCALPHA(预乘管线)
    // 或 SRCALPHA/INVSRCALPHA(默认 straight 管线)。premul 由 gpart_set_premul 或自动检测:
    // 检测进入本函数时的 SRCBLEND(rs.src) == D3DBLEND_ONE → 当前是预乘管线(psPremul)。
    bool premul = g_gpart_premul >= 0 ? (g_gpart_premul != 0)
        : (rsg.rs.src == 2 /* D3DBLEND_ONE */);
    auto set_blend = [&](int additive, int pos) {
        D3DCheck(d3d::set_render_state(D3DRS_ALPHABLENDENABLE, TRUE), pos);
        if (additive)
        {
            D3DCheck(d3d::set_render_state(D3DRS_SRCBLEND, D3DBLEND_ONE), pos + 1);
            D3DCheck(d3d::set_render_state(D3DRS_DESTBLEND, D3DBLEND_ONE), pos + 2);
        }
        else
        {
            D3DCheck(d3d::set_render_state(D3DRS_SRCBLEND, premul ? D3DBLEND_ONE : D3DBLEND_SRCALPHA), pos + 1);
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

    float bl[4] = { 0, premul ? 1.0f : 0.0f, 0, 0 };   // .x=加色遍, .y=预乘输出
    if (s.blend_mask == 3)
    {
        // 混合系统: 普通 + 加法两遍(VS/PS 按 uBlend 零化不匹配粒子/切换预乘)
        set_blend(0, 20);
        D3DCheck(d3d::set_vs_const_typed(RND_C_BLEND, d3d::CK_FLOAT, bl, 1), 22);
        D3DCheck(d3d::set_ps_const_typed(RND_C_BLEND, d3d::CK_FLOAT, bl, 1), 22);
        draw_ranges(23);
        set_blend(1, 25);
        bl[0] = 1.0f;
        D3DCheck(d3d::set_vs_const_typed(RND_C_BLEND, d3d::CK_FLOAT, bl, 1), 26);
        D3DCheck(d3d::set_ps_const_typed(RND_C_BLEND, d3d::CK_FLOAT, bl, 1), 26);
        draw_ranges(27);
    }
    else
    {
        int additive = (s.blend_mask & 2) ? 1 : 0;
        set_blend(additive, 20);
        bl[0] = (float)additive;
        D3DCheck(d3d::set_vs_const_typed(RND_C_BLEND, d3d::CK_FLOAT, bl, 1), 22);
        D3DCheck(d3d::set_ps_const_typed(RND_C_BLEND, d3d::CK_FLOAT, bl, 1), 22);
        draw_ranges(23);
    }

    // 解绑 VS 采样槽位: 防止游戏自己的 shader(FFP 模拟/SDF)在 VS 采样到我们的状态纹理
    d3d::set_texture(GP_VTS0 + 0, nullptr);
    d3d::set_texture(GP_VTS0 + 1, nullptr);
    d3d::set_texture(GP_VTS0 + 2, nullptr);
    d3d::set_texture(GP_VTS0 + 3, nullptr);
    // (rsg 析构在此函数末尾还原渲染状态/纹理阶段, 异常路径也执行)
}


// ============================================================================
// 导出: 系统
// ============================================================================

// 立即执行 GPU 初始化(创建类型表/图集/矩形表纹理、编译 shader、抓取引擎形状精灵)。
// 幂等: 已就绪/已失败时直接返回现状; 游戏可在加载画面主动预热, 避免首次
// update/particles_create 时的卡顿。成功返回 gtrue, 失败(含 VTF 不支持)返回 gerror。
// cache_dir: 着色器字节码缓存目录; 空字符串 = 不使用缓存(每次重新编译)。
exp_real gpart_gpu_init(const char* cache_dir)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        return gpu_init_internal(cache_dir ? cache_dir : "") ? gtrue : gerror;
    }
    simple_catch("gpart_gpu_init", gerror)
}

// gpart 扩展: 设置粒子输出 alpha 模式(类似 sdf_draw_set_premul)。
// mode: -1=自动检测当前混合状态(默认, ONE/INVSRCALPHA→预乘, SRCALPHA→straight);
//        0=强制 straight(SRCALPHA/INVSRCALPHA); 1=强制预乘(ONE/INVSRCALPHA)。
// 默认自动即可适配 application_surface(psPremul) 与普通绘制管线; 特殊场景可手动强制。
exp_real gpart_set_premul(double mode)
{
    if (d3d::version() != d3d::V9) return gerror;
    g_gpart_premul = (int)mode;
    return gtrue;
}

exp_real gpart_system_create(double capacity)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        if (!gpu_init_internal("")) return gerror;
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
                if (age < 0.0f || age >= s.s_life[e.slot])
                {
                    // 自然死亡 → part_type_death 事件(CPU 版本, 位置由 GPU 按源槽位读取)
                    if (g_any_step_death)
                    {
                        auto it = g_types.find(s.s_type[e.slot]);
                        if (it != g_types.end())
                        {
                            const GType& gt = it->second;
                            if (gt.death_num() != 0.0f && gt.death_type() >= 1.0f
                                && g_types.count((int)gt.death_type()))
                            {
                                if (gt.death_num() > 0.0f)
                                    queue_source_spawn(s, (int)gt.death_type(), e.slot,
                                        (int)gt.death_num());
                                else if (gphashf((float)e.slot + s.now * 3.71f + 17.0f)
                                    < 1.0f / -gt.death_num())
                                    queue_source_spawn(s, (int)gt.death_type(), e.slot, 1);
                            }
                        }
                    }
                    continue;
                }
                win[w++] = e;
            }
            win.resize(w);

            // part_type_step 事件: 每个带 step 配置的存活粒子, 每步按数量(或 1/|n| 概率)生成
            if (g_any_step_death)
            {
                for (auto& e : win)
                {
                    auto it = g_types.find(s.s_type[e.slot]);
                    if (it == g_types.end()) continue;
                    const GType& gt = it->second;
                    if (gt.step_num() != 0.0f && gt.step_type() >= 1.0f
                        && g_types.count((int)gt.step_type()))
                    {
                        if (gt.step_num() > 0.0f)
                            queue_source_spawn(s, (int)gt.step_type(), e.slot,
                                (int)gt.step_num());
                        else if (gphashf((float)e.slot + s.now * 7.31f)
                            < 1.0f / -gt.step_num())
                            queue_source_spawn(s, (int)gt.step_type(), e.slot, 1);
                    }
                }
            }

            // 流式发射器: 每步自动发射(配置一次, 由 update 处理, 同 GM8 引擎语义);
            // number < 0 = 每步 1/|n| 概率产生 1 个(确定性 hash 判定)
            for (auto& ekv : s.emitters)
            {
                GEmitter& g = ekv.second;
                if (g.stream_type < 1 || !g_types.count(g.stream_type)) continue;
                SpawnBatch b = emitter_batch(g);
                if (g.stream_rate > 0.0f)
                    queue_spawn(s, g.stream_type, (int)g.stream_rate, b);
                else if (g.stream_rate < 0.0f
                    && gphashf((float)(ekv.first + (int)s.now * 13)) < 1.0f / -g.stream_rate)
                    queue_spawn(s, g.stream_type, 1, b);
            }

            // 无粒子且无发射 → 无事可做, 但时钟必须推进:
            // 负 stream/step/death 的概率 hash 依赖 s.now, 不推进则判定永远相同(卡死不发射)
            if (s.live_window.empty() && s.pending.empty())
            {
                s.now += 1.0f;
                continue;
            }

            // 演化 pass: 无批次也要跑(老化, GM8 语义: 每次 update 推进一步);
            // 批次存在则带上出生分支(分 16 批切块, 时钟只推进一次)。
            if (s.pending.empty())
            {
                std::vector<SpawnBatch> none;
                run_evolution(s, none, 0);
            }
            else
            {
                // 分块演化: 第一块正常(老化+该块出生), 其余块仅出生不老化,
                // 避免多块时粒子被多次老化(时钟加速)。
                size_t off = 0;
                bool first_chunk = true;
                while (off < s.pending.size())
                {
                    size_t n = std::min((size_t)GP_MAX_BATCHES, s.pending.size() - off);
                    std::vector<SpawnBatch> chunk(s.pending.begin() + off, s.pending.begin() + off + n);
                    run_evolution(s, chunk, (int)n, !first_chunk);
                    off += n;
                    first_chunk = false;
                }
                s.pending.clear();
            }
            s.now += 1.0f;   // 一次 update = 一步

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

        ensure_gm8_shapes();   // 引擎形状精灵懒初始化完成后的重试(一次性)

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
        if ((int)g_types.size() + (int)g_type_free_ids.size() >= GP_TYPE_TEX_W - 1)
            throw std::runtime_error("类型 id 空间已耗尽(上限 255), 需先销毁旧类型。");
        GType t;
        t.set_defaults();
        int id;
        if (!g_type_free_ids.empty())
        {
            id = g_type_free_ids.back();       // 优先复用已回收的 id
            g_type_free_ids.pop_back();
        }
        else
            id = g_type_counter++;
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
        atlas_free_regions(it->second);        // 回收图集空间
        g_type_free_ids.push_back(id);         // 回收类型 id
        g_types.erase(it);
        type_table_upload();
        recompute_step_death();
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
        atlas_free_regions(*t);                // 先回收图集(在 *t 被覆盖前)
        GType fresh;
        fresh.set_defaults();
        *t = fresh;
        t->frame_rect.clear();
        t->shape = PT_SHAPE_PIXEL;
        t->animat = t->stretch = t->random_frame = false;
        t->random_frame_flag() = 0;
        type_table_upload();
        t->upload_rect_table((int)type);
        recompute_step_death();
        return gtrue;
    }
    simple_catch("gpart_type_clear", gerror)
}

exp_real gpart_type_sprite(double type, double sprite, double animat, double stretch, double random)
{
    try
    {
        if (d3d::version() != d3d::V9) return gerror;
        if (!gpu_init_internal("")) return gerror;
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        int spr = (int)sprite;

        atlas_free_regions(*t);                // 换精灵前回收旧区域
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
            t->atlas_owned.push_back({ ax, ay, (int)w, (int)h });   // 记录占用(异常/清理时可回收)
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
        if (t->frame_rect.empty())
        {
            atlas_free_regions(*t);            // 无有效帧 → 回退刚分配的区域
            return gfalse;
        }
        // 精灵像素宽作为尺寸基准(GM8: 屏幕像素 = size × scale × 精灵宽)
        t->pixel_scale() = t->frame_rect[0].u1 * (float)GP_ATLAS_SIZE;

        t->animat = animat > 0.5;
        t->stretch = stretch > 0.5;
        t->random_frame = random > 0.5;
        t->shape = -1;                 // 有精灵 → 形状路径失效
        t->frame_count() = (float)t->frame_rect.size();
        t->animation_enabled() = t->animat ? 1.0f : 0.0f;
        t->stretch_animation() = t->stretch ? 1.0f : 0.0f;
        t->random_frame_flag() = t->random_frame ? 1.0f : 0.0f;
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
        t->frame_count() = 0;
        t->animation_enabled() = 0;
        t->stretch_animation() = 0;
        t->random_frame_flag() = 0;
        t->pixel_scale() = 64.0f;   // GM8: 内置形状精灵 64×64(pixel 也是 64 精灵, 内容中心 1px)
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
        t->scale_x() = (float)xscale;
        t->scale_y() = (float)yscale;
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
        t->life_min() = (float)std::min(min, max);
        t->life_max() = (float)std::max(min, max);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_life", gerror)
}

exp_real gpart_type_size(double type, double min, double max, double incr, double wiggle)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->size_min() = (float)std::min(min, max);
        t->size_max() = (float)std::max(min, max);
        t->size_increment() = (float)incr;
        t->size_wiggle() = (float)wiggle;
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_size", gerror)
}

// GM8 part_type_speed(ind, speed_min, speed_max, speed_incr, speed_wiggle):
// 每步速度 += speed_incr(速度永不为负) + 随机 ±speed_wiggle 摆动。
// 增量/摆动为 per-step(与 dt=1 的演化一致), 速度下限 0 由 shader clamp。
exp_real gpart_type_speed(double type, double min, double max, double incr, double wiggle)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->speed_min() = (float)std::min(min, max);
        t->speed_max() = (float)std::max(min, max);
        t->speed_increment() = (float)incr;
        t->speed_wiggle() = (float)wiggle;
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_speed", gerror)
}

// GM8 part_type_direction(ind, dir_min, dir_max, dir_incr, dir_wiggle):
// 方向范围(逆时针, 0=右); dir_incr = 每步方向增量; dir_wiggle = 每步 ±偏移。均为 per-step。
exp_real gpart_type_direction(double type, double min, double max, double incr, double wiggle)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->direction_min() = (float)std::min(min, max);
        t->direction_max() = (float)std::max(min, max);
        t->direction_increment() = (float)incr;
        t->direction_wiggle() = (float)wiggle;
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
        t->gravity_direction() = (float)dir;
        t->gravity_amount() = (float)force;
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
        t->drag() = (float)std::clamp(coeff, 0.0, 1.0);
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_drag", gerror)
}

exp_real gpart_type_step(double type, double step_number, double step_type)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->step_number() = (float)step_number;
        t->step_type_id() = (float)step_type;
        t->field(GTypeRow::FeatureFlags, 0) =
            (step_number != 0.0 && step_type >= 1.0) ? 1.0f : 0.0f;
        recompute_step_death();                // 清空配置时标志回落, 不再白跑遍历
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_step", gerror)
}

exp_real gpart_type_death(double type, double death_number, double death_type)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->death_number() = (float)death_number;
        t->death_type_id() = (float)death_type;
        t->field(GTypeRow::FeatureFlags, 1) =
            (death_number != 0.0 && death_type >= 1.0) ? 1.0f : 0.0f;
        recompute_step_death();                // 清空配置时标志回落, 不再白跑遍历
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_death", gerror)
}

// gpart 扩展: 设置类型是否豁免 destroyer 特效器(区域销毁条不会销毁此类型)。
// 用于 part_type_death 生成的锚定粒子(如水面涟漪), 防止被触发销毁的销毁条二次销毁。
exp_real gpart_type_immune_destroyer(double type, double immune)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->destroyer_immune_flag() = (immune >= 0.5) ? 1.0f : 0.0f;
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_immune_destroyer", gerror)
}

exp_real gpart_type_blend(double type, double additive)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        t->additive() = additive > 0.5 ? 1.0f : 0.0f;
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_blend", gerror)
}

// 颜色模式统一入口: mode + 颜色分量(0..1)
static void type_set_colour(GType* t, int mode, const float c[9])
{
    t->colour_mode() = (float)mode;
    // T4: c1.r c1.g c1.b c2.r; T5: c2.g c2.b c3.r c3.g; T6.x: c3.b
    float* color_a = t->row(GTypeRow::ColorA);
    float* color_b = t->row(GTypeRow::ColorB);
    float* color_c_alpha = t->row(GTypeRow::ColorCAndAlpha);
    color_a[0] = c[0]; color_a[1] = c[1]; color_a[2] = c[2]; color_a[3] = c[3];
    color_b[0] = c[4]; color_b[1] = c[5]; color_b[2] = c[6]; color_b[3] = c[7];
    color_c_alpha[0] = c[8];
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
    t->alpha_mode() = (float)mode;
    float* color_c_alpha = t->row(GTypeRow::ColorCAndAlpha);
    color_c_alpha[1] = a1;
    color_c_alpha[2] = a2;
    color_c_alpha[3] = a3;
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
        float* orientation = t->row(GTypeRow::Orientation);
        orientation[0] = (float)ang_min;
        orientation[1] = (float)ang_max;
        orientation[2] = (float)ang_incr;
        orientation[3] = (float)ang_wiggle;
        t->relative_angle() = ang_relative > 0.5 ? 1.0f : 0.0f;
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
// number < 0 时每步 1/|number| 概率产生 1 个(GM8 语义); number = 0 关闭流式。
// 由 gpart_system_update 处理; 清除用 gpart_emitter_clear。
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
        if (number != 0.0)
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

// ============================================================================
// attractors (GM8 part_attractor_*, GPU 演化 pass 应用)
// ============================================================================

exp_real gpart_attractor_create(double sys)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return -1;
        int id = it->second.att_counter++;
        it->second.attractors[id] = GAttractor();
        return (double)id;
    }
    simple_catch("gpart_attractor_create", gerror)
}

exp_real gpart_attractor_destroy(double sys, double ind)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        return it->second.attractors.erase((int)ind) ? gtrue : gfalse;
    }
    simple_catch("gpart_attractor_destroy", gerror)
}

exp_real gpart_attractor_destroy_all(double sys)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        it->second.attractors.clear();
        return gtrue;
    }
    simple_catch("gpart_attractor_destroy_all", gerror)
}

exp_real gpart_attractor_exists(double sys, double ind)
{
    auto it = g_systems.find((int)sys);
    if (it == g_systems.end()) return gfalse;
    return it->second.attractors.count((int)ind) ? gtrue : gfalse;
}

exp_real gpart_attractor_clear(double sys, double ind)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto a = it->second.attractors.find((int)ind);
        if (a == it->second.attractors.end()) return gfalse;
        a->second = GAttractor();
        return gtrue;
    }
    simple_catch("gpart_attractor_clear", gerror)
}

exp_real gpart_attractor_position(double sys, double ind, double x, double y)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto a = it->second.attractors.find((int)ind);
        if (a == it->second.attractors.end()) return gfalse;
        a->second.x = (float)x;
        a->second.y = (float)y;
        return gtrue;
    }
    simple_catch("gpart_attractor_position", gerror)
}

exp_real gpart_attractor_force(double sys, double ind, double force, double dist, 
    double kind, double aditive)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto a = it->second.attractors.find((int)ind);
        if (a == it->second.attractors.end()) return gfalse;
        a->second.force = (float)force;
        a->second.dist = (float)dist;
        a->second.kind = (int)kind;
        a->second.additive = aditive != 0.0;
        return gtrue;
    }
    simple_catch("gpart_attractor_force", gerror)
}

// ============================================================================
// destroyers (GM8 part_destroyer_*, GPU 演化 pass 应用)
// ============================================================================

exp_real gpart_destroyer_create(double sys)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return -1;
        int id = it->second.des_counter++;
        it->second.destroyers[id] = GDestroyer();
        return (double)id;
    }
    simple_catch("gpart_destroyer_create", gerror)
}

exp_real gpart_destroyer_destroy(double sys, double ind)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        return it->second.destroyers.erase((int)ind) ? gtrue : gfalse;
    }
    simple_catch("gpart_destroyer_destroy", gerror)
}

exp_real gpart_destroyer_destroy_all(double sys)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        it->second.destroyers.clear();
        return gtrue;
    }
    simple_catch("gpart_destroyer_destroy_all", gerror)
}

exp_real gpart_destroyer_exists(double sys, double ind)
{
    auto it = g_systems.find((int)sys);
    if (it == g_systems.end()) return gfalse;
    return it->second.destroyers.count((int)ind) ? gtrue : gfalse;
}

exp_real gpart_destroyer_clear(double sys, double ind)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto d = it->second.destroyers.find((int)ind);
        if (d == it->second.destroyers.end()) return gfalse;
        d->second = GDestroyer();
        return gtrue;
    }
    simple_catch("gpart_destroyer_clear", gerror)
}

exp_real gpart_destroyer_region(double sys, double ind, double xmin, double xmax, 
    double ymin, double ymax, double shape)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto d = it->second.destroyers.find((int)ind);
        if (d == it->second.destroyers.end()) return gfalse;
        d->second.xmin = (float)std::min(xmin, xmax);
        d->second.xmax = (float)std::max(xmin, xmax);
        d->second.ymin = (float)std::min(ymin, ymax);
        d->second.ymax = (float)std::max(ymin, ymax);
        d->second.shape = (int)shape;
        return gtrue;
    }
    simple_catch("gpart_destroyer_region", gerror)
}

// ============================================================================
// deflectors (GM8 part_deflector_*, GPU 演化 pass 应用)
// ============================================================================

exp_real gpart_deflector_create(double sys)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return -1;
        int id = it->second.def_counter++;
        it->second.deflectors[id] = GDeflector();
        return (double)id;
    }
    simple_catch("gpart_deflector_create", gerror)
}

exp_real gpart_deflector_destroy(double sys, double ind)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        return it->second.deflectors.erase((int)ind) ? gtrue : gfalse;
    }
    simple_catch("gpart_deflector_destroy", gerror)
}

exp_real gpart_deflector_destroy_all(double sys)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        it->second.deflectors.clear();
        return gtrue;
    }
    simple_catch("gpart_deflector_destroy_all", gerror)
}

exp_real gpart_deflector_exists(double sys, double ind)
{
    auto it = g_systems.find((int)sys);
    if (it == g_systems.end()) return gfalse;
    return it->second.deflectors.count((int)ind) ? gtrue : gfalse;
}

exp_real gpart_deflector_clear(double sys, double ind)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto d = it->second.deflectors.find((int)ind);
        if (d == it->second.deflectors.end()) return gfalse;
        d->second = GDeflector();
        return gtrue;
    }
    simple_catch("gpart_deflector_clear", gerror)
}

exp_real gpart_deflector_region(double sys, double ind, double xmin, double xmax, 
    double ymin, double ymax)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto d = it->second.deflectors.find((int)ind);
        if (d == it->second.deflectors.end()) return gfalse;
        d->second.xmin = (float)std::min(xmin, xmax);
        d->second.xmax = (float)std::max(xmin, xmax);
        d->second.ymin = (float)std::min(ymin, ymax);
        d->second.ymax = (float)std::max(ymin, ymax);
        return gtrue;
    }
    simple_catch("gpart_deflector_region", gerror)
}

exp_real gpart_deflector_kind(double sys, double ind, double kind)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto d = it->second.deflectors.find((int)ind);
        if (d == it->second.deflectors.end()) return gfalse;
        d->second.kind = (int)kind;
        return gtrue;
    }
    simple_catch("gpart_deflector_kind", gerror)
}

exp_real gpart_deflector_friction(double sys, double ind, double friction)
{
    if (d3d::version() != d3d::V9) return gerror;
    try
    {
        auto it = g_systems.find((int)sys);
        if (it == g_systems.end()) return gfalse;
        auto d = it->second.deflectors.find((int)ind);
        if (d == it->second.deflectors.end()) return gfalse;
        d->second.friction = (float)friction;
        return gtrue;
    }
    simple_catch("gpart_deflector_friction", gerror)
}
