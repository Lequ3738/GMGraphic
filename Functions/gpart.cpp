#include "gpart.h"
#include "vertex.h"
#include "../Librarys/math_s.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <map>

static const float GP_PI = 3.14159265358979323846f;
// GP_FMT_16F 是 D3D9 专属格式常量(113), d3d8.h 里没有; gpart 仅 D3D9 运行。
static const DWORD GP_FMT_16F = 113;

// ============================================================================
// gpart_* GPU particle system (DX9 only)
//
// Stateful GPU simulation:
//   - Particle state lives in three A16B16G16R16F render-target textures
//     (256x256 grid = 65536 slots), ping-ponged each step:
//       tex[0] : pos.xy, vel.xy
//       tex[1] : age, life, type, (flags)
//       tex[2] : base color.rgb (mix/rgb/hsv/override), has_override
//   - gpart_system_update(): one 3-MRT fullscreen pass per spawn-batch chunk
//     integrates gravity/drag/motion and initialises newly spawned slots.
//   - gpart_system_drawit(): CPU builds a texture-grouped, depth-ordered index
//     vertex buffer (particle ids), then one point-sprite draw per group.
//     The VS reads particle state via VTF (vs_3_0 tex2Dlod) and outputs PSIZE.
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
static const int RND_C_TIM = 5;       // (now, 0, 0, 0)
static const int RND_C_VIEW = 6;      // (viewport w, h, 0, 0)

static const int GP_TYPE_ROWS = 9;
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
// 类型
// ============================================================================
struct GType
{
    float t[GP_TYPE_ROWS][4] = {};    // 与类型表纹理行一一对应
    std::vector<float> frame_tex;     // 精灵各帧的引擎纹理 id
    int shape = PT_SHAPE_PIXEL;       // 无精灵时的形状
    bool animat = false, stretch = false, random_frame = false;
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
};

struct GroupRange { uint32_t start = 0, count = 0; uint64_t key = 0; int blend = 0; };

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
    void* id_vb = nullptr;
    bool vb_dirty = true;
    std::vector<GroupRange> groups;
};

static std::unordered_map<int, GSystem> g_systems;
static int g_system_counter = 1;

// ============================================================================
// GPU 资源(全局, 惰性创建)
// ============================================================================
static void* g_type_tex = nullptr;         // 类型表纹理 256x9 A16B16G16R16F
static void* g_quad_vb = nullptr;          // 全屏四边形(剪辑空间, 4 顶点)
static void* g_shape_tex[PT_SHAPE_COUNT] = {};
static void* g_id_decl = nullptr;          // 渲染 pass 顶点声明(float1 id)
static void* g_quad_decl = nullptr;
static dword g_evo_vs = 0, g_evo_ps = 0;
static dword g_rnd_vs = 0, g_rnd_ps = 0;
static bool g_gpu_ready = false;
static bool g_gpu_failed = false;

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
"      if (distr > 0.5) {\n"
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
"    float4 T0 = tex2D(sType, float2((type + 0.5) / 256.0, 0.5 / 9.0));\n"
"    float4 T1 = tex2D(sType, float2((type + 0.5) / 256.0, 1.5 / 9.0));\n"
"    float4 T3 = tex2D(sType, float2((type + 0.5) / 256.0, 3.5 / 9.0));\n"
"    float4 T4 = tex2D(sType, float2((type + 0.5) / 256.0, 4.5 / 9.0));\n"
"    float4 T5 = tex2D(sType, float2((type + 0.5) / 256.0, 5.5 / 9.0));\n"
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
"      else { float3 hsv = lerp(T4.rgb, T5.rgb, rnd.z); base = hsv2rgb(hsv * float3(360.0/255.0, 1.0/255.0, 1.0/255.0)); }\n"
"    } else { base = T4.rgb; }\n"
"    has_ovr = b3.w;\n"
"  } else if (dead < 0.5) {\n"
"    pos = prev.xy;\n"
"    vel = prev.zw;\n"
"    age = st.x;\n"
"    life = st.y;\n"
"    type = st.z;\n"
"    base = ov.rgb;\n"
"    has_ovr = ov.w;\n"
"    float dt = uGlobal.y;\n"
"    float4 T2 = tex2D(sType, float2((type + 0.5) / 256.0, 2.5 / 9.0));\n"
"    float g = T2.y;\n"
"    float ga = T2.x * DEG2RAD;\n"
"    vel += g * dt * float2(cos(ga), -sin(ga));\n"
"    vel *= max(1.0 - clamp(T2.z, 0.0, 1.0) * dt, 0.0);\n"
"    pos += vel * dt;\n"
"    age += dt;\n"
"  } else {\n"
"    pos = 0; vel = 0; type = 0; base = float3(1,1,1); has_ovr = 0;\n"
"    age = 1.0; life = 0.0;\n"
"  }\n"
"  o.c0 = float4(pos, vel);\n"
"  o.c1 = float4(age, life, type, 0);\n"
"  o.c2 = float4(base, has_ovr);\n"
"  return o;\n"
"}\n";

static const char* RND_VS_HLSL =
"sampler sPos : register(s1);\n"
"sampler sLife : register(s2);\n"
"sampler sType : register(s3);\n"
"float4x4 uWVP : register(c0);\n"
"float4 uSys : register(c4);\n"
"float4 uView : register(c6);\n"
"struct VSIN { float id : TEXCOORD0; };\n"
"struct VSOUT {\n"
"  float4 pos : POSITION;\n"
"  float4 state : COLOR1;   // xy = 屏幕中心(像素), zw = 粒子状态 uv\n"
"  float ang : FOG;\n"
"  float psize : PSIZE;\n"
"};\n"
"float h1(float a) { return frac(43758.5453 * frac(a * 0.1031)); }\n"
"VSOUT main(VSIN v) {\n"
"  VSOUT o;\n"
"  float id = v.id;\n"
"  float2 uv = (float2(fmod(id, 256.0), floor(id / 256.0)) + 0.5) * uSys.z;\n"
"  float4 pl = tex2Dlod(sPos, float4(uv, 0, 0));\n"
"  float4 st = tex2Dlod(sLife, float4(uv, 0, 0));\n"
"  float age = st.x, life = st.y;\n"
"  float type = st.z;\n"
"  float2 tuv = float2((type + 0.5) / 256.0, 0.0);\n"
"  float4 T0 = tex2Dlod(sType, float4(tuv.x, 0.5 / 9.0, 0, 0));\n"
"  float4 T3 = tex2Dlod(sType, float4(tuv.x, 3.5 / 9.0, 0, 0));\n"
"  float4 T7 = tex2Dlod(sType, float4(tuv.x, 7.5 / 9.0, 0, 0));\n"
"  float4 T8 = tex2Dlod(sType, float4(tuv.x, 8.5 / 9.0, 0, 0));\n"
"  float dead = (age >= life) ? 1.0 : 0.0;\n"
"  float size = lerp(T0.z, T0.w, h1(id + 3.0));\n"
"  float psize = size * T3.x;\n"
"  o.psize = dead > 0.5 ? 0.0 : psize;\n"
"  float ang = lerp(T7.x, T7.y, h1(id + 5.0)) + T7.z * age;\n"
"  ang += (h1(id + floor(age) * 7.31) - 0.5) * 2.0 * T7.w;\n"
"  if (T8.x > 0.5) ang += atan2(-pl.w, pl.z) * 57.29577951308232;\n"
"  o.ang = ang * 0.017453292519943295;\n"
"  float4 clip = mul(uWVP, float4(pl.xy + uSys.xy, 0, 1));\n"
"  o.pos = clip;\n"
"  float2 ndc = clip.xy / clip.w;\n"
"  o.state = float4((ndc * 0.5 + 0.5) * uView.xy, uv);\n"
"  return o;\n"
"}\n";

static const char* RND_PS_HLSL =
"sampler sMain : register(s0);\n"
"sampler sLife : register(s2);\n"
"sampler sType : register(s3);\n"
"sampler sOvr : register(s4);\n"
"float h1(float a) { return frac(43758.5453 * frac(a * 0.1031)); }\n"
"float4 main(float4 vpos : VPOS, float4 state : COLOR1, float ang : FOG) : COLOR0 {\n"
"  float id = floor(state.z * 256.0) + floor(state.w * 256.0) * 256.0;\n"
"  float4 st = tex2D(sLife, state.zw);\n"
"  float age = st.x, life = st.y;\n"
"  float type = st.z;\n"
"  float t = life > 0.0001 ? clamp(age / life, 0.0, 1.0) : 1.0;\n"
"  float2 tuv = float2((type + 0.5) / 256.0, 0.0);\n"
"  float4 T0 = tex2D(sType, float2(tuv.x, 0.5 / 9.0));\n"
"  float4 T3 = tex2D(sType, float2(tuv.x, 3.5 / 9.0));\n"
"  float4 T4 = tex2D(sType, float2(tuv.x, 4.5 / 9.0));\n"
"  float4 T5 = tex2D(sType, float2(tuv.x, 5.5 / 9.0));\n"
"  float4 T6 = tex2D(sType, float2(tuv.x, 6.5 / 9.0));\n"
"  float4 ov = tex2D(sOvr, state.zw);\n"
"  float psize = lerp(T0.z, T0.w, h1(id + 3.0)) * T3.x;\n"
"  float2 uv = (vpos.xy - state.xy) / max(psize, 0.0001) + 0.5;\n"
"  float ca = cos(ang), sa = sin(ang);\n"
"  float2 p = uv - 0.5;\n"
"  float2 ru = float2(ca * p.x - sa * p.y, sa * p.x + ca * p.y) + 0.5;\n"
"  float4 tex = tex2D(sMain, ru);\n"
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
"  a = tex.a * a;\n"
"  return float4(tex.rgb * col * a, a);\n"
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
            px[(c * size + c) * 4 + 3] = 255;
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
}

// ============================================================================
// GPU 资源初始化(惰性, 失败置 g_gpu_failed)
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

        // 类型表纹理 256x9 A16B16G16R16F
        D3DCheck(d3d::create_texture(GP_TYPE_TEX_W, GP_TYPE_ROWS, 1, 0,
            GP_FMT_16F, D3DPOOL_DEFAULT, &g_type_tex), 1);
        std::vector<unsigned short> zero((size_t)GP_TYPE_TEX_W * GP_TYPE_ROWS * 4, 0);
        D3DCheck(d3d::upload_texture(g_type_tex, GP_TYPE_TEX_W, GP_TYPE_ROWS,
            GP_FMT_16F, zero.data(), GP_TYPE_TEX_W * 8), 2);

        // 全屏四边形(剪辑空间 TRIANGLESTRIP)
        static const float quad[GP_QUAD_VERTS * 4] = {
            -1, -1, 0, 1,   1, -1, 0, 1,   -1, 1, 0, 1,   1, 1, 0, 1
        };
        D3DCheck(d3d::create_vertex_buffer(sizeof(quad), &g_quad_vb), 3);
        D3DCheck(d3d::upload_vertex_buffer(g_quad_vb, quad, sizeof(quad)), 4);

        // 顶点声明
        vertex_element quad_e[] = { { 0, 0, VT_FLOAT4, 0, VU_POSITION, 0 } };
        D3DCheck(d3d::create_vertex_declaration(quad_e, 1, &g_quad_decl), 5);
        vertex_element id_e[] = { { 0, 0, VT_FLOAT1, 0, VU_TEXCOORD, 0 } };
        D3DCheck(d3d::create_vertex_declaration(id_e, 1, &g_id_decl), 6);

        // 形状纹理(64x64 A8R8G8B8 直通 alpha)
        for (int s = 0; s < PT_SHAPE_COUNT; ++s)
        {
            std::vector<BYTE> px((size_t)64 * 64 * 4, 0);
            gen_shape_tex(s, px.data(), 64);
            D3DCheck(d3d::create_texture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_shape_tex[s]), 7);
            D3DCheck(d3d::upload_texture(g_shape_tex[s], 64, 64, D3DFMT_A8R8G8B8, px.data(), 64 * 4), 8);
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
        d3d::upload_texture(g_type_tex, GP_TYPE_TEX_W, GP_TYPE_ROWS,
            GP_FMT_16F, px.data(), GP_TYPE_TEX_W * 8);
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

    for (int i = 0; i < n; ++i)
    {
        int slot = (start + i) % s.capacity;
        s.s_birth[slot] = s.now;
        s.s_type[slot] = type;
        auto it = g_types.find(type);
        if (it != g_types.end())
        {
            const GType& gt = it->second;
            float life = (float)lerp(gt.t[0][0], gt.t[0][1], gphashf((float)slot + b.seed * 17.0f));
            s.s_life[slot] = life;
            int frames = (int)gt.frame_tex.size();
            if (gt.random_frame && frames > 0)
                s.s_frame[slot] = (int)(gphashf((float)slot + b.seed * 17.0f + 9.0f) * frames) % frames;
            else
                s.s_frame[slot] = 0;
        }
        else
        {
            s.s_life[slot] = 30.0f;
            s.s_frame[slot] = 0;
        }
    }
    s.cursor = (s.cursor + n) % s.capacity;
    s.pending.push_back(b);
    s.vb_dirty = true;
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
    d3d::set_render_state(D3DRS_POINTSPRITEENABLE, r.ps_en);
    d3d::set_render_state(D3DRS_POINTSCALEENABLE, r.ps_scale);
    d3d::set_render_state(D3DRS_POINTSIZE_MIN, r.ps_min);
    d3d::set_render_state(D3DRS_POINTSIZE_MAX, r.ps_max);
    d3d::set_render_state(D3DRS_POINTSIZE, r.ps_size);
}
// 纹理 stage 保存/恢复(0..4 的绑定 + 寻址/过滤; addr 用 [i*2]=U, [i*2+1]=V)
static void stages_save(void* tex[5], dword addr[10], dword mag[5], dword minf[5], dword mip[5])
{
    for (int i = 0; i < 5; ++i)
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
static void stages_restore(void* tex[5], dword addr[10], dword mag[5], dword minf[5], dword mip[5])
{
    for (int i = 0; i < 5; ++i)
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
    for (int i = 0; i < 5; ++i)
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
    if (count <= 0) return;
    RsSave rs;
    rs_save(rs);
    void* tex[5] = {}; dword addr[10] = {}, mag[5] = {}, minf[5] = {}, mip[5] = {};
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
    s.now += dt;

    stages_restore(tex, addr, mag, minf, mip);
    rs_restore(rs);
}

// ============================================================================
// 渲染 pass
// ============================================================================
static void rebuild_render_vb(GSystem& s)
{
    s.groups.clear();
    std::map<uint64_t, std::vector<float>> by_key;

    for (int slot = 0; slot < s.capacity; ++slot)
    {
        float age = s.now - s.s_birth[slot];
        if (age < 0.0f || age >= s.s_life[slot]) continue;
        int type = s.s_type[slot];
        auto it = g_types.find(type);
        if (it == g_types.end()) continue;
        const GType& gt = it->second;

        int texkey;
        auto shape_key = [&gt]() -> int {
            return 0x80000000 | (gt.shape >= 0 && gt.shape < PT_SHAPE_COUNT
                ? gt.shape : (int)PT_SHAPE_PIXEL);
            };
        if (!gt.frame_tex.empty())
        {
            int frames = (int)gt.frame_tex.size();
            int frame = 0;
            if (gt.animat && frames > 1)
                frame = gt.stretch
                ? (int)std::min((float)(frames - 1), age / std::max(s.s_life[slot], 1.0f) * frames)
                : (int)age % frames;
            else
                frame = s.s_frame[slot] % frames;
            texkey = (int)gt.frame_tex[frame];
            if (texkey < 0) texkey = shape_key();
        }
        else
            texkey = shape_key();

        int blend = (gt.t[2][3] > 0.5f) ? 1 : 0;
        uint64_t key = ((uint64_t)(uint32_t)texkey << 1) | (uint64_t)blend;
        by_key[key].push_back((float)slot);
    }

    // 组内按出生时间排序(old→new 或 new→old)
    for (auto& kv : by_key)
    {
        auto& v = kv.second;
        if (s.old_to_new)
            std::sort(v.begin(), v.end(), [&s](float a, float b) {
            int ia = (int)a, ib = (int)b;
            return s.s_birth[ia] < s.s_birth[ib];
                });
        else
            std::sort(v.begin(), v.end(), [&s](float a, float b) {
            int ia = (int)a, ib = (int)b;
            return s.s_birth[ia] > s.s_birth[ib];
                });
    }

    // 组顺序 = 按键排序(稳定)
    std::vector<uint64_t> keys;
    for (auto& kv : by_key) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    std::vector<float> ids;
    for (uint64_t k : keys)
    {
        GroupRange gr;
        gr.start = (uint32_t)ids.size();
        gr.key = k;
        gr.blend = (int)(k & 1);
        auto& v = by_key[k];
        ids.insert(ids.end(), v.begin(), v.end());
        gr.count = (uint32_t)v.size();
        s.groups.push_back(gr);
    }

    if (ids.empty()) return;
    if (!s.id_vb)
    {
        D3DCheck(d3d::create_vertex_buffer((UINT)(s.capacity * 4), &s.id_vb), 1);
    }
    D3DCheck(d3d::upload_vertex_buffer(s.id_vb, ids.data(), (UINT)(ids.size() * 4)), 2);
    s.vb_dirty = false;
}

static void run_render(GSystem& s)
{
    if (s.groups.empty()) return;
    RsSave rs;
    rs_save(rs);
    void* tex[5] = {}; dword addr[10] = {}, mag[5] = {}, minf[5] = {}, mip[5] = {};
    stages_save(tex, addr, mag, minf, mip);

    // 点精灵状态(一次)
    D3DCheck(d3d::set_render_state(D3DRS_ZENABLE, FALSE), 1);
    D3DCheck(d3d::set_render_state(D3DRS_POINTSPRITEENABLE, TRUE), 2);
    D3DCheck(d3d::set_render_state(D3DRS_POINTSCALEENABLE, FALSE), 3);

    d3d::Caps caps;
    d3d::get_caps(caps);
    float zero_ps = 0.0f, max_ps = caps.max_point_size;
    D3DCheck(d3d::set_render_state(D3DRS_POINTSIZE_MIN, d3dvar(zero_ps)), 4);
    D3DCheck(d3d::set_render_state(D3DRS_POINTSIZE_MAX, d3dvar(max_ps)), 5);

    // 状态纹理绑定(stage 1..4) + 类型表
    stages_set_point();
    int w = s.cur;
    D3DCheck(d3d::set_texture(1, s.tex[0][w]), 6);
    D3DCheck(d3d::set_texture(2, s.tex[1][w]), 7);
    D3DCheck(d3d::set_texture(3, g_type_tex), 8);
    D3DCheck(d3d::set_texture(4, s.tex[2][w]), 9);

    // VS 常量
    float wvp[16];
    d3d::get_transform(D3DTS_WORLD, wvp);
    {
        float view[16], proj[16];
        d3d::get_transform(D3DTS_VIEW, view);
        d3d::get_transform(D3DTS_PROJECTION, proj);
        float wv[16];
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
    float tim[4] = { s.now, 0, 0, 0 };
    UINT vw = 1, vh = 1;
    d3d::get_viewport(&vw, &vh);
    float view[4] = { (float)vw, (float)vh, 0, 0 };
    D3DCheck(d3d::set_vs_const_typed(RND_C_WVP, d3d::CK_FLOAT, wvp, 4), 10);
    D3DCheck(d3d::set_vs_const_typed(RND_C_SYS, d3d::CK_FLOAT, sys, 1), 11);
    D3DCheck(d3d::set_vs_const_typed(RND_C_TIM, d3d::CK_FLOAT, tim, 1), 12);
    D3DCheck(d3d::set_vs_const_typed(RND_C_VIEW, d3d::CK_FLOAT, view, 1), 13);

    D3DCheck(d3d::set_vertex_declaration(g_id_decl), 14);
    D3DCheck(d3d::set_vertex_shader_handle(g_rnd_vs), 15);
    D3DCheck(d3d::set_pixel_shader(g_rnd_ps), 16);
    D3DCheck(d3d::set_stream_source(0, s.id_vb, 4), 17);

    for (auto& gr : s.groups)
    {
        // 组纹理
        void* maintex = nullptr;
        uint32_t texkey = (uint32_t)(gr.key >> 1);
        if (texkey & 0x80000000u)
        {
            uint32_t idx = texkey & 0x7fffffffu;
            if (idx >= (uint32_t)PT_SHAPE_COUNT) continue;   // 防御: 非法形状键
            maintex = g_shape_tex[idx];
        }
        else
        {
            int tid = (int)texkey;
            void* t = (void*)gmapi->GetDirect3DTexture(tid);
            maintex = t;
        }
        if (!maintex) continue;
        D3DCheck(d3d::set_texture(0, maintex), 17);

        // 混合
        if (gr.blend)
        {
            D3DCheck(d3d::set_render_state(D3DRS_ALPHABLENDENABLE, TRUE), 18);
            D3DCheck(d3d::set_render_state(D3DRS_SRCBLEND, D3DBLEND_ONE), 19);
            D3DCheck(d3d::set_render_state(D3DRS_DESTBLEND, D3DBLEND_ONE), 20);
        }
        else
        {
            D3DCheck(d3d::set_render_state(D3DRS_ALPHABLENDENABLE, TRUE), 21);
            D3DCheck(d3d::set_render_state(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA), 22);
            D3DCheck(d3d::set_render_state(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA), 23);
        }
        D3DCheck(d3d::draw_primitive(D3DPT_POINTLIST, gr.count, gr.start), 24);
    }

    stages_restore(tex, addr, mag, minf, mip);
    rs_restore(rs);
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
        s.capacity = (int)std::clamp((double)capacity, 1.0, (double)GP_MAX_CAPACITY);
        s.s_birth.resize(s.capacity, -1e9f);
        s.s_life.resize(s.capacity, 1.0f);
        s.s_type.resize(s.capacity, 0);
        s.s_frame.resize(s.capacity, 0);
        system_tex_create(s);
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
        s.cursor = 0;
        s.vb_dirty = true;
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
            if (s.pending.empty()) continue;
            // 分批 16 组演化
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
        if (s.vb_dirty) rebuild_render_vb(s);
        if (s.groups.empty()) return gtrue;
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
        it->second.vb_dirty = true;
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
        for (int i = 0; i < s.capacity; ++i)
        {
            float age = s.now - s.s_birth[i];
            if (age >= 0.0f && age < s.s_life[i]) n++;
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
        return (double)id;
    }
    simple_catch("gpart_type_create", gerror)
}

exp_real gpart_type_destroy(double type)
{
    try
    {
        auto it = g_types.find((int)type);
        if (it == g_types.end()) return gtrue;
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
        t->frame_tex.clear();
        t->shape = PT_SHAPE_PIXEL;
        t->animat = t->stretch = t->random_frame = false;
        type_table_upload();
        return gtrue;
    }
    simple_catch("gpart_type_clear", gerror)
}

exp_real gpart_type_sprite(double type, double sprite, double animat, double stretch, double random)
{
    try
    {
        GType* t = type_at((int)type);
        if (!t) return gfalse;
        int spr = (int)sprite;
        t->frame_tex.clear();
        for (int k = 0; k < GP_MAX_FRAMES; ++k)
        {
            int tex = gm::sprite_get_texture(spr, k);
            if (tex < 0) break;
            t->frame_tex.push_back((float)tex);
        }
        if (t->frame_tex.empty()) return gfalse;
        t->animat = animat > 0.5;
        t->stretch = stretch > 0.5;
        t->random_frame = random > 0.5;
        t->shape = -1;                 // 有精灵 → 形状路径失效
        t->t[8][1] = (float)t->frame_tex.size();
        t->t[8][2] = t->animat ? 1.0f : 0.0f;
        t->t[8][3] = t->stretch ? 1.0f : 0.0f;
        type_table_upload();
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
        t->frame_tex.clear();
        t->t[8][1] = 0;
        t->t[8][2] = 0;
        t->t[8][3] = 0;
        type_table_upload();
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

exp_real gpart_type_gravity(double type, double dir, double force)
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

        SpawnBatch b;
        b.shape = (float)g.shape;
        b.distr = (float)g.distr;
        b.xmin = g.xmin; b.ymin = g.ymin; b.xmax = g.xmax; b.ymax = g.ymax;
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

exp_real gpart_emitter_stream(double sys, double em, double parttype, double number)
{
    if (d3d::version() != d3d::V9) return gerror;
    return emitter_spawn_impl(sys, em, parttype, number);
}