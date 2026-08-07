// Shader Extension
// Version: 2.0
// Updated: 2026/8/7  by Lequ
//          shader API 重构: HLSL 支持 + GMS2 风格命名。权威文档: 桌面 "Shader API 设计.md"
//          旧 d3d_ps_* / d3d_vs_* / d3d_set_tex* / d3d_conf_* 已删除(不做旧名兼容)。

#include "math_s.h"
#include "shader.h"
#include "draw_text.h"
#include "pixel_shader_defs.h"

// ============================================================================
// Variables
// ============================================================================

static d3d::Caps d3dcaps;                   // GPU capability struct(经 d3d::get_caps 填充, 双后端统一字段)

vert_ext vbuff_ext_int[vb_count];           // Internal vertex buffer (ext)
vert_default vbuff_default_int[vb_count];   // Internal vertex buffer (default)
uint vbuff_c;                               // Internal counter
D3DPRIMITIVETYPE vbuff_prim;                // Primitive to draw
bool vbuff_usevs;                           // Use vertex shader?
bool vbuff_autoinc;                         // Automatic increment?
bool vbuff_use_struct;                      // Use struct vertices instead of raw data?
bool vbuff_use_ext = false;                 // Use ext vertex buffer?

namespace gm
{
    int argument_list = noone;
}

// ============================================================================
// Shader bundle 与 uniform 句柄(设计 §2 / §7)
// ============================================================================

struct ShaderBundle
{
    dword vs = NULL;          // D3D8=DWORD 句柄; D3D9=对象指针(dword 往返)。NULL = 该阶段未用
    void* vs_table = nullptr; // ID3DXConstantTable*(HLSL 编译才有); asm 恒 NULL
    dword ps = NULL;
    void* ps_table = nullptr;
};

struct UniformHandle
{
    int owner_shader = -1;   // 回指 shader, 用于销毁清理
    int ps_reg = -1;         // -1 = 该阶段未命中
    int vs_reg = -1;
};

static std::unordered_map<int, ShaderBundle>  shaders;    // shader id → bundle
static std::unordered_map<int, UniformHandle> uniforms;   // uniform handle → 结构体
static int shader_id_counter = 1;                          // 从 1 递增(0 保留)
static int uniform_counter = 1;
static int current_shader = -1;                            // shader_current() 用

int sdf_shader = -1;          // SDF 字体着色器(id, init() 里创建)
int sdf_shader_premul = -1;
int sdf_shader_uniform = -1;  // c0(scale/thickness) 的 uniform 句柄 = shader_get_uniform(sdf_shader, "ps.0")

// ============================================================================
// Initialisation
// ============================================================================

// Initialises device pointer, GPU information, buffers, etc.
exp_real init(gm_real arg_list)
{
    // 检测后端(D3D8 / D3D9)并缓存; 之后所有 d3d:: 调用按此分发。
    d3d::ensure_version((void*)gmapi->GetDirect3DDevice(), (void*)gmapi->GetDirect3DInterface());
    d3d::get_caps(d3dcaps);

    gm::argument_list = (int)arg_list;

    sdf_shader        = (int)shader_create_asm("", ps_sdf);
    sdf_shader_premul = (int)shader_create_asm("", ps_sdf_premul);
    sdf_shader_uniform = (int)shader_get_uniform(sdf_shader, "ps.0");   // 只写 ps 寄存器 0
    sdf::shader       = sdf_shader;

    return gtrue;
}

// ============================================================================
// Information
// ============================================================================

// GPU name.
exp_str d3d_dev_get_name() { return_string(d3dcaps.adapter_desc); }

// Maximum size of point primitives.
exp_real d3d_dev_get_point_max_size() { return (double)d3dcaps.max_point_size; }

// GPU pixel shader version. 10 to 14.
// Most modern GPUs support higher versions but they're not reported here.
exp_real d3d_dev_get_ps_version()
{
    uint v = (uint)d3dcaps.pixel_shader_version;
    return (double)((((v >> 8) & 0xFF) * 10) + v & 0xFF);
}

// Maximum texture width. Applies to all graphical resources.
exp_real d3d_dev_get_tex_max_width() { return (double)d3dcaps.max_tex_w; }

// Height.
exp_real d3d_dev_get_tex_max_height() { return (double)d3dcaps.max_tex_h; }

// Maximum simultaneous textures. Limits how many texture stages you can use.
exp_real d3d_dev_get_tex_max_stages()
{
    return (double)d3dcaps.max_tex_stages;
}

// Free texture memory in bytes. Approximate. This ISN'T the VRAM size.
exp_real d3d_dev_get_tex_mem()
{
    return (double)d3d::get_available_tex_mem();
}

// ============================================================================
// Shader API(GMS2 风格)
//
// 设计要点(权威: 桌面 "Shader API 设计.md"):
//   - shader = bundle { vs, ps, 常量表 }; 空字符串阶段 = 不用(空 VS = passthrough FVF 固定管线)。
//   - uniform 句柄 = 统一 map 索引(从 1 递增), 结构体 {ps_reg, vs_reg}; asm/HLSL 共用同一 map。
//   - 写入一律按寄存器走 d3d::set_ps_const / d3d::set_vs_const, 与来源(asm/HLSL)无关。
//   - HLSL 仅 D3D9; D3D8 下 shader_create / shader_has_uniform 返回 -1 / gfalse。
// ============================================================================

static bool is_d3d9() { return d3d::version() == d3d::V9; }

// HLSL profile 自适应(设计 §4.3): SM2.0 与 SM3.0 语法完全相同, 只是能力上限不同。
static const char* ps_profile()
{
    return (d3dcaps.pixel_shader_version >= D3DPS_VERSION(3, 0)) ? "ps_3_0" : "ps_2_0";
}
static const char* vs_profile()
{
    return (d3dcaps.vertex_shader_version >= D3DVS_VERSION(3, 0)) ? "vs_3_0" : "vs_2_0";
}

static void bundle_release(ShaderBundle& b)
{
    if (b.vs) d3d::delete_vertex_shader(b.vs);
    if (b.ps) d3d::delete_pixel_shader(b.ps);
    if (b.vs_table) d3d::release(b.vs_table);   // ID3DXConstantTable 是 COM 对象
    if (b.ps_table) d3d::release(b.ps_table);
    b = ShaderBundle{};
}

// 创建类导出共用兜底: 失败时释放已创建的对象(避免半成品泄漏) + 弹 gm 错误框。
// 不能用 simple_catch —— 那会漏掉 bundle_release(b)。需要局部变量 b 在作用域内。
#define shader_create_catch(funcname)                                                   \
    catch (const std::exception& e)                                                     \
    {                                                                                   \
        bundle_release(b);                                                              \
        gm::show_error("An error occurred while executing function " funcname           \
            " in GMGraphic.dll:\r\n" + std::string(e.what()), false);                   \
        return gerror;                                                                  \
    }

// 前缀语法 "ps." / "vs."(大小写不敏感, 设计 §3)。返回 0=无前缀, 1=ps, 2=vs;
// *rest 指向去掉前缀后的名字/寄存器串。
static int parse_prefix(const char* uni, const char** rest)
{
    if (_strnicmp(uni, "ps.", 3) == 0) { *rest = uni + 3; return 1; }
    if (_strnicmp(uni, "vs.", 3) == 0) { *rest = uni + 3; return 2; }
    *rest = uni;
    return 0;
}

// ---- 创建 ----

// 编译 HLSL 单个阶段并创建设备对象。
// 返回 true = 阶段已创建; false = 该阶段 passthrough(未使用)。
// entry 非空 → 直接编译(失败抛异常 = gm 错误框);
// entry 空 → 源码含 fallback 入口名(mainVS/mainPS)才编译, 否则该阶段 passthrough。
static bool compile_hlsl_stage(const char* src, const char* entry, const char* fallback,
                               const char* profile, bool is_vs, ShaderBundle& b)
{
    const char* use = (entry && entry[0]) ? entry : nullptr;
    if (!use)
    {
        if (!strstr(src, fallback)) return false;   // 源码里没有默认入口 → passthrough
        use = fallback;
    }

    std::vector<BYTE> code;
    void* table = nullptr;
    std::string err;
    // 编译失败: 弹 gm 错误框, 附带 D3DX 编译器详细错误文本(设计 §0.6)。
    if (FAILED(d3d::compile_hlsl(src, strlen(src), use, profile, code, &table, &err)))
    {
        if (table) d3d::release(table);   // 防御: 失败时 D3DX 理论上置空
        throw std::runtime_error("Shader compile error:\r\n\r\n" + err + "\r\n\r\n" + std::string(src));
    }

    // 常量表先挂到 bundle: 若创建失败, 由外层 shader_create_catch 的 bundle_release 统一释放。
    if (is_vs)
    {
        b.vs_table = table;
        D3DCheck(d3d::create_vertex_shader(d3d::VERT_EXT, code.data(), nullptr, 0, &b.vs), 1);
    }
    else
    {
        b.ps_table = table;
        D3DCheck(d3d::create_pixel_shader(code.data(), &b.ps), 1);
    }
    return true;
}

// 创建 HLSL 着色器(单文件双入口, D3D9 only; D3D8 返回 -1)。GML 层变参 vs_entry/ps_entry。
exp_real shader_create(const char* src, const char* vs_entry, const char* ps_entry)
{
    ShaderBundle b;
    try
    {
        if (!is_d3d9()) return gerror;   // HLSL 仅 D3D9(设计 §5)

        compile_hlsl_stage(src, vs_entry, "mainVS", vs_profile(), true,  b);
        compile_hlsl_stage(src, ps_entry, "mainPS", ps_profile(), false, b);

        if (!b.vs && !b.ps) return gerror;   // 双 passthrough 没意义 → -1

        int id = shader_id_counter++;
        shaders.emplace(id, b);
        return (double)id;
    }
    shader_create_catch("shader_create")
}

// 创建 asm 着色器(D3D8/9 通用)。vs/ps 空字符串 = 不用该阶段; 双空返回 -1(设计 §1)。
exp_real shader_create_asm(const char* vs_src, const char* ps_src)
{
    ShaderBundle b;
    try
    {
        if (vs_src && vs_src[0])
        {
            std::vector<BYTE> code, constants;
            std::string err;
            // 汇编失败附编译器文本; 设备创建失败走 D3DCheck。
            if (FAILED(d3d::assemble_vs(vs_src, strlen(vs_src), code, constants, &err)))
                throw std::runtime_error("Shader assembly error:\r\n\r\n" + err + "\r\n\r\n" + vs_src);
            D3DCheck(d3d::create_vertex_shader(d3d::VERT_EXT, code.data(), constants.data(), constants.size(), &b.vs), 2);
        }

        if (ps_src && ps_src[0])
        {
            if (d3dcaps.pixel_shader_version < D3DPS_VERSION(1, 4))
                throw std::runtime_error("PS 1.4 unsupported by GPU.");
            std::vector<BYTE> code;
            std::string err;
            if (FAILED(d3d::assemble_ps(ps_src, strlen(ps_src), code, &err)))
                throw std::runtime_error("Shader assembly error:\r\n\r\n" + err + "\r\n\r\n" + ps_src);
            D3DCheck(d3d::create_pixel_shader(code.data(), &b.ps), 3);
        }

        if (!b.vs && !b.ps) return gerror;   // 双空 → -1

        int id = shader_id_counter++;
        shaders.emplace(id, b);
        return (double)id;
    }
    shader_create_catch("shader_create_asm")
}

// ---- 销毁 / 设置 ----

// 释放 vs/ps 对象 + 常量表; 清理该 shader 的 uniform map 条目(设计 §2)。
exp_real shader_destroy(double sh)
{
    try
    {
        int id = (int)sh;
        auto it = shaders.find(id);
        if (it == shaders.end()) return gerror;

        for (auto uit = uniforms.begin(); uit != uniforms.end(); )
        {
            if (uit->second.owner_shader == id) uit = uniforms.erase(uit);
            else ++uit;
        }

        bundle_release(it->second);   // COM Release 无 HRESULT, 不需 D3DCheck
        shaders.erase(it);

        if (current_shader == id) current_shader = -1;
        return gtrue;
    }
    simple_catch("shader_destroy", gerror)
}

// 绑定 vs+ps; 自动 SetDefaults(仅非空常量表 = HLSL; asm 靠字节码里的 def, 跳过)。
exp_real shader_set(double sh)
{
    try
    {
        int id = (int)sh;
        if (id < 0) return shader_reset();

        auto it = shaders.find(id);
        if (it == shaders.end()) return gerror;

        ShaderBundle& b = it->second;

        // VS: 非空 → 绑顶点着色器; 空 → passthrough(FVF 固定管线, Option A, 设计 §4.1)。
        if (b.vs)
        {
            vbuff_usevs = true;
            D3DCheck(d3d::set_vertex_shader(false, 0, b.vs), 1);
        }
        else
        {
            vbuff_usevs = false;
            D3DCheck(d3d::set_vertex_shader(true, fvf_ext, 0), 2);
        }
        // PS: 非空 → 绑; 空 → 固定像素管线。
        D3DCheck(d3d::set_pixel_shader(b.ps), 3);
        // 默认常量(仅 HLSL 有常量表; asm 靠 def, 跳过)。
        if (b.vs_table) D3DCheck(d3d::constant_table_set_defaults(b.vs_table), 4);
        if (b.ps_table) D3DCheck(d3d::constant_table_set_defaults(b.ps_table), 5);

        current_shader = id;
        return gtrue;
    }
    simple_catch("shader_set", gerror)
}

// 解除 shader(回到 FVF), 等价 shader_set(-1)。
exp_real shader_reset()
{
    try
    {
        vbuff_usevs = false;
        D3DCheck(d3d::set_vertex_shader(true, fvf_ext, 0), 1);
        D3DCheck(d3d::set_pixel_shader(0), 2);
        current_shader = -1;
        return gtrue;
    }
    simple_catch("shader_reset", gerror)
}

// 当前绑定 shader, 无则 -1。
exp_real shader_current() { return (double)current_shader; }

// ---- uniform 查找 ----

// 按名字/寄存器拿句柄(设计 §2/§3)。asm 传数字串("0"/"ps.0"/"vs.3");
// HLSL 传常量名("uColor"/"ps.uColor"/"vs.uWVP")。找不到返回 -1。
exp_real shader_get_uniform(double sh, const char* uni)
{
    int id = (int)sh;
    auto it = shaders.find(id);
    if (it == shaders.end()) return gerror;

    ShaderBundle& b = it->second;
    const char* name = nullptr;
    int prefix = parse_prefix(uni, &name);

    UniformHandle uh;
    uh.owner_shader = id;

    bool has_hlsl = (b.ps_table || b.vs_table);
    if (has_hlsl)
    {
        // HLSL: 按名字查常量表。无前缀 → 先 ps 后 vs; 同名两阶段都设(设计 §4.4)。
        if (prefix == 0 || prefix == 1)
        {
            uh.ps_reg = -1;
            if (b.ps_table)
            {
                void* h = d3d::constant_table_get_constant_by_name(b.ps_table, name);
                if (h) uh.ps_reg = d3d::constant_table_get_register(b.ps_table, h);
            }
        }
        if (prefix == 0 || prefix == 2)
        {
            uh.vs_reg = -1;
            if (b.vs_table)
            {
                void* h = d3d::constant_table_get_constant_by_name(b.vs_table, name);
                if (h) uh.vs_reg = d3d::constant_table_get_register(b.vs_table, h);
            }
        }
    }
    else
    {
        // asm: uni 是寄存器数字串(设计约定)。无前缀 → 恒两阶段同号寄存器(设计 §3)。
        int reg = atoi(name);
        if (prefix == 0)      { uh.ps_reg = reg; uh.vs_reg = reg; }
        else if (prefix == 1) { uh.ps_reg = reg; uh.vs_reg = -1; }
        else                  { uh.ps_reg = -1; uh.vs_reg = reg; }
    }

    // 找不到 → -1(设计 §1); 至少一个阶段命中才算有效句柄。
    if (uh.ps_reg < 0 && uh.vs_reg < 0) return gerror;

    int hid = uniform_counter++;
    uniforms.emplace(hid, uh);
    return (double)hid;
}

// uniform 是否存在(HLSL 专用, 依赖常量表; D3D8 返回 gfalse)。
exp_real shader_has_uniform(double sh, const char* uni)
{
    if (!is_d3d9()) return gfalse;

    int id = (int)sh;
    auto it = shaders.find(id);
    if (it == shaders.end()) return gfalse;

    ShaderBundle& b = it->second;
    if (!b.ps_table && !b.vs_table) return gfalse;   // asm 无常量表

    const char* name = nullptr;
    int prefix = parse_prefix(uni, &name);

    bool found = false;
    if (prefix == 0 || prefix == 1)
        if (b.ps_table && d3d::constant_table_get_constant_by_name(b.ps_table, name)) found = true;
    if (prefix == 0 || prefix == 2)
        if (b.vs_table && d3d::constant_table_get_constant_by_name(b.vs_table, name)) found = true;
    return found ? gtrue : gfalse;
}

// 采样器句柄(配 texture_set_stage)。asm 数字串转整数; HLSL 查常量表(只认采样器常量)。
exp_real shader_get_sampler_index(double sh, const char* uni)
{
    int id = (int)sh;
    auto it = shaders.find(id);
    if (it == shaders.end()) return gerror;

    ShaderBundle& b = it->second;
    const char* name = nullptr;
    int prefix = parse_prefix(uni, &name);

    if (!b.ps_table && !b.vs_table)
    {
        // asm: 数字字符串转整数(设计 §1)
        return (double)atoi(name);
    }

    // HLSL: 查 ps 表优先, 其次 vs 表; 只认采样器(D3DXPC_OBJECT + D3DXPT_SAMPLER)。
    void* h = nullptr;
    if ((prefix == 0 || prefix == 1) && b.ps_table)
        h = d3d::constant_table_get_constant_by_name(b.ps_table, name);
    int stage = -1;
    if (h) stage = d3d::constant_table_get_sampler_register(b.ps_table, h);
    if (stage < 0 && (prefix == 0 || prefix == 2) && b.vs_table)
    {
        h = d3d::constant_table_get_constant_by_name(b.vs_table, name);
        if (h) stage = d3d::constant_table_get_sampler_register(b.vs_table, h);
    }
    if (stage < 0) return gerror;
    return (double)stage;
}

// ---- uniform 写入(固定参导出, GML 层变参脚本分发到 _4f/_4i/_4b) ----

// 查句柄 → 按结构体写对应阶段(设计 §2: 对 -1 的阶段跳过)。
// 非法句柄 / D3D 调用失败 → 抛异常, 由各导出 try/simple_catch 统一兜底(弹错误框)。
static void uniform_set_impl(double h, const float v[4])
{
    auto it = uniforms.find((int)h);
    if (it == uniforms.end())
        throw std::runtime_error("Invalid uniform handle.");

    const UniformHandle& uh = it->second;
    if (uh.ps_reg >= 0) D3DCheck(d3d::set_ps_const((DWORD)uh.ps_reg, v, 1), 1);
    if (uh.vs_reg >= 0) D3DCheck(d3d::set_vs_const((DWORD)uh.vs_reg, v, 1), 2);
}

exp_real shader_set_uniform_4f(double h, double x, double y, double z, double w)
{
    try
    {
        float v[4] = { (float)x, (float)y, (float)z, (float)w };
        uniform_set_impl(h, v);
        return gtrue;
    }
    simple_catch("shader_set_uniform_4f", gerror)
}

exp_real shader_set_uniform_4i(double h, double x, double y, double z, double w)
{
    // D3D9 SM2/3 无 int 寄存器(D3DX9 把 int 常量编进 float 寄存器), 整数以 float 写入。
    try
    {
        float v[4] = { (float)x, (float)y, (float)z, (float)w };
        uniform_set_impl(h, v);
        return gtrue;
    }
    simple_catch("shader_set_uniform_4i", gerror)
}

exp_real shader_set_uniform_4b(double h, double x, double y, double z, double w)
{
    try
    {
        float v[4] = {
            (x >= 0.5) ? 1.f : 0.f,
            (y >= 0.5) ? 1.f : 0.f,
            (z >= 0.5) ? 1.f : 0.f,
            (w >= 0.5) ? 1.f : 0.f
        };
        uniform_set_impl(h, v);
        return gtrue;
    }
    simple_catch("shader_set_uniform_4b", gerror)
}

exp_real shader_set_uniform_color(double h, double col, double alpha)
{
    try
    {
        int c = (int)col;
        float v[4] = { (float)col_red(c) / 255.0f, (float)col_green(c) / 255.0f,
                       (float)col_blue(c) / 255.0f, (float)clamp(alpha, 0.0, 1.0) };
        uniform_set_impl(h, v);
        return gtrue;
    }
    simple_catch("shader_set_uniform_color", gerror)
}

// 行主序手写 4x4 乘: c = a * b(与旧代码一致, 去掉 D3DX 数学依赖)。
static void mat_mul(float* c, const float* a, const float* b)
{
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a[r * 4 + k] * b[k * 4 + col];
            c[r * 4 + col] = s;
        }
}

// 设矩阵 uniform。mtx_type 掩码: world=1 / view=2 / projection=4 / wvp=7(gm82dx9 式)。
// size = 写几个寄存器(默认 4)。行主序直写不转置(与 gm82dx9 一致, HLSL 用 mul(pos, mtx))。
exp_real shader_set_uniform_matrix(double h, double mtx_type, double size)
{
    try
    {
        auto it = uniforms.find((int)h);
        if (it == uniforms.end())
            throw std::runtime_error("Invalid uniform handle.");
        const UniformHandle& uh = it->second;

        int mask = (int)mtx_type;
        int regs = (int)size; if (regs < 1) regs = 4;

        float m[16];
        if (mask & 1)
            D3DCheck(d3d::get_transform(D3DTS_WORLD, m), 1);
        else
            for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;   // 单位矩阵

        // 基准 = world(或 identity); 依次右乘 view / projection。mat_mul 不能原地(会读到已写值)。
        if (mask & 2)
        {
            float t[16], r[16];
            D3DCheck(d3d::get_transform(D3DTS_VIEW, t), 2);
            mat_mul(r, m, t);
            memcpy(m, r, sizeof r);
        }
        if (mask & 4)
        {
            float t[16], r[16];
            D3DCheck(d3d::get_transform(D3DTS_PROJECTION, t), 3);
            mat_mul(r, m, t);
            memcpy(m, r, sizeof r);
        }

        if (uh.ps_reg >= 0) D3DCheck(d3d::set_ps_const((DWORD)uh.ps_reg, m, (DWORD)regs), 4);
        if (uh.vs_reg >= 0) D3DCheck(d3d::set_vs_const((DWORD)uh.vs_reg, m, (DWORD)regs), 5);
        return gtrue;
    }
    simple_catch("shader_set_uniform_matrix", gerror)
}

// ============================================================================
// Sampler stages(对应旧 d3d_set_tex* 系列)
// ============================================================================

// 绑定纹理到采样器 stage(旧 d3d_set_tex)。tex < 0 清空该 stage。
exp_real texture_set_stage(double samp, double tex)
{
    uint s = (uint)samp;

    if (s >= d3dcaps.max_tex_stages)
        return gerror;
    
    if (tex < 0.0)
    {
        d3dcheck(d3d::set_texture(s, nullptr));
    }
    else
    {
        void* t = (void*)gmapi->GetDirect3DTexture((int)tex);   // 不透明: 不直接调它的方法

        if (t == nullptr)
            return gerror;

        d3dcheck(d3d::set_texture(s, t));
    }

    return gtrue;
}

// 内部助手(不导出): 清空所有纹理 stage(替代旧 d3d_set_tex_all)。
void texture_clear_all()
{
    for (uint i = 0; i < d3dcaps.max_tex_stages; i++)
        d3d::set_texture(i, nullptr);
}

// 设置采样器插值模式(旧 d3d_set_tex_int)。默认 nearest, 通常不理想。
exp_real texture_set_stage_interpolation(double samp, double mode)
{
    dword s = (dword)samp;

    if (s >= d3dcaps.max_tex_stages)
        return gerror;

    if ((D3D_OK == d3d::set_tex_stage_state(s, D3DTSS_MAGFILTER, (dword)mode))
        && (D3D_OK == d3d::set_tex_stage_state(s, D3DTSS_MINFILTER, (dword)mode)))
    {
        return gtrue;
    }

    return gfalse;
}

// 对应旧 d3d_set_tex_wrap + border 色。h/v: 0 → clamp(D3DTADDRESS_CLAMP), 否则透传
// D3DTADDRESS_* 值(WRAP=1/MIRROR=2/CLAMP=3/BORDER=4)。border_col 为 GM 颜色。
exp_real texture_set_stage_repeat(double samp, double h, double v, double border_col)
{
    dword s = (dword)samp;

    if (s >= d3dcaps.max_tex_stages)
        return gerror;

    dword u = (dword)h, w = (dword)v;
    if (u == 0) u = D3DTADDRESS_CLAMP;   // 0 = clamp(gm82dx9 式)
    if (w == 0) w = D3DTADDRESS_CLAMP;

    HRESULT hr = d3d::set_tex_stage_state(s, D3DTSS_ADDRESSU, u);
    if (SUCCEEDED(hr)) hr = d3d::set_tex_stage_state(s, D3DTSS_ADDRESSV, w);
    if (SUCCEEDED(hr)) hr = d3d::set_tex_stage_state(s, D3DTSS_BORDERCOLOR, col_d3d((int)border_col, 1.0));
    return SUCCEEDED(hr) ? gtrue : gfalse;
}

// 设置 border 模式使用的颜色(旧 d3d_set_tex_border)。
exp_real texture_set_stage_border(double samp, double col, double alpha)
{
    dword s = (dword)samp;

    if (s >= d3dcaps.max_tex_stages)
        return gerror;

    d3dcheck(d3d::set_tex_stage_state(s, D3DTSS_BORDERCOLOR, col_d3d((int)col, alpha)));
}

// 设置各向异性过滤级别(旧 d3d_set_tex_aniso)。值: 1,2,4,8,16。1=无, 默认 1。
exp_real texture_set_stage_anisotropy(double samp, double aniso)
{
    dword s = (dword)samp;

    if (s >= d3dcaps.max_tex_stages)
        return gerror;

    d3dcheck(d3d::set_tex_stage_state(s, D3DTSS_MAXANISOTROPY,
        (dword)std::min((dword)aniso, d3dcaps.max_aniso)));
}

// 设置 mipmap 过滤模式(旧 d3d_set_tex_mip)。tex_int_nearest 或 tex_int_bilinear。
exp_real texture_set_stage_mipmap(double samp, double mode)
{
    dword s = (dword)samp;

    if (s >= d3dcaps.max_tex_stages)
        return gerror;

    d3dcheck(d3d::set_tex_stage_state(s, D3DTSS_MIPFILTER, (dword)mode));
}

// ============================================================================
// Fog
// ============================================================================

// Turn fog on or off.
exp_real d3d_set_fog_state(double state) { d3dcrs(D3DRS_FOGENABLE, (state > 0.0)); }

// Set the type of fog.  Use fog_type_ constants.  Linear by default.
exp_real d3d_set_fog_type(double fog_type)
{
    d3dcrs(D3DRS_FOGTABLEMODE, (dword)fog_type);
}

// Controls density of exponential fog. 0-1.
exp_real d3d_set_fog_density(double density)
{
    float d = (float)clamp(density, 0, 1);
    d3dcrs(D3DRS_FOGDENSITY, d3dvar(d));
}

// Set fog colour.
exp_real d3d_set_fog_color(double col)
{
    d3dcrs(D3DRS_FOGCOLOR, col_d3d((int)col, 0.0));
}

// Set fog start.
exp_real d3d_set_fog_start(double dist)
{
    float d = (float)dist;
    d3dcrs(D3DRS_FOGSTART, d3dvar(d));
}

// Set fog end.
exp_real d3d_set_fog_end(double dist)
{
    float d = (float)dist;
    d3dcrs(D3DRS_FOGEND, d3dvar(d));
}

// ============================================================================
// Points
// ============================================================================

// Set drawing size for point primitives.
exp_real d3d_set_point_size(double size)
{
    float x = (float)clamp(size, 1.0, d3dcaps.max_point_size);
    d3dcrs(D3DRS_POINTSIZE, d3dvar(x));
}

// Set size clamp, useful for scaled points in 3D mode. Defaults to 1.
exp_real d3d_set_point_size_min(double size)
{
    float x = (float)clamp(size, 1.0, d3dcaps.max_point_size);
    d3dcrs(D3DRS_POINTSIZE_MIN, d3dvar(x));
}

// Set size clamp.  Defaults to 64.
exp_real d3d_set_point_size_max(double size)
{
    float x = (float)clamp(size, 1.0, d3dcaps.max_point_size);
    d3dcrs(D3DRS_POINTSIZE_MAX, d3dvar(x));
}

// Enable point scaling. This scales points based on their distance from the camera.
exp_real d3d_set_point_scale(double state)
{
    d3dcrs(D3DRS_POINTSCALEENABLE, (state > 0.0));
}

// Configure point scaling formula.  Defaults to (1,0,0).
// The formula is:
// size * sqrt(1/( ceof1 + (coef2*distancetocamera) + (coef3*sqr(distancetocamera)) ))
exp_real d3d_set_point_scale_coef(double coef1, double coef2, double coef3)
{
    dword a, b, c;
    float ca, cb, cc;

    ca = (float)abs(coef1);
    cb = (float)abs(coef2);
    cc = (float)abs(coef3);

    a = d3drs(D3DRS_POINTSCALE_A, d3dvar(ca));
    b = d3drs(D3DRS_POINTSCALE_B, d3dvar(cb));
    c = d3drs(D3DRS_POINTSCALE_C, d3dvar(cc));
    return (double)(a == D3D_OK && b == D3D_OK && c == D3D_OK);
}

// When enabled, this causes point primitives to be drawn with textures applied.
exp_real d3d_set_point_sprite(double state)
{
    d3dcrs(D3DRS_POINTSPRITEENABLE, (state > 0.0));
}

// ============================================================================
// Render control
// ============================================================================

// Set colour writemask.
// You can enable/disable writing of each channel independently. All enabled by default.
exp_real d3d_set_mask(double r, double g, double b, double a)
{
    if (!(d3dcaps.prim_misc_caps & D3DPMISCCAPS_COLORWRITEENABLE))
        return gfalse;

    dword mask[4]{};
    mask[0] = (r > 0.0) ? D3DCOLORWRITEENABLE_RED : 0;
    mask[1] = (g > 0.0) ? D3DCOLORWRITEENABLE_GREEN : 0;
    mask[2] = (b > 0.0) ? D3DCOLORWRITEENABLE_BLUE : 0;
    mask[3] = (a > 0.0) ? D3DCOLORWRITEENABLE_ALPHA : 0;

    d3dcrs(D3DRS_COLORWRITEENABLE, mask[0] | mask[1] | mask[2] | mask[3]);
}

// Set z-buffer writing.
// Enabled by default. Disabling it prevents overwriting of the z-buffer.
exp_real d3d_set_zwrite(double state)
{
    d3dcrs(D3DRS_ZWRITEENABLE, (state > 0.0));
}

// Prevents drawing of pixels that don't meet the given alpha criteria.
// Use the cmp_ constants. Pass -1 as value to disable alpha testing.
exp_real d3d_set_alphatest(double value, double mode)
{
    dword a, b, c;
    if (value < 0.0)
    {
        d3dcrs(D3DRS_ALPHATESTENABLE, false);
    }
    else
    {
        a = d3drs(D3DRS_ALPHATESTENABLE, true);
        b = d3drs(D3DRS_ALPHAREF, (dword)clamp(value, 0x00000000, 0x000000FF));
        c = d3drs(D3DRS_ALPHAFUNC, (dword)mode);
    }

    return (a == D3D_OK && b == D3D_OK && c == D3D_OK);
}

// Prevents drawing of pixels that don't meet the given depth criteria.
// Use the cmp_ constants. Defaults to <=. The other value is the current z-buffer value.
exp_real d3d_set_ztest(double mode)
{
    if (!(d3dcaps.raster_caps & D3DPRASTERCAPS_ZTEST))
        return gfalse;

    d3dcrs(D3DRS_ZFUNC, (dword)mode);
}

// Offsets the drawing depth, allowing polygons with the same positions to be
// drawn without z-fighting artifacts.  Useful for shadows, decals, etc.
// Integer 0-16, defaults to 0. Higher values appear in front of lower ones.
exp_real d3d_set_zbias(double bias)
{
    d3dcrs(D3DRS_ZBIAS, (uint)floor(clamp(bias, 0, 16)));
}

// Set how D3D renders polygons: point, wireframe or solid.
// Use d3d_fillmode_ constants. Defaults to solid.
exp_real d3d_set_fillmode(double mode) { d3dcrs(D3DRS_FILLMODE, (dword)mode); }

// Automatically normalises vectors when rendering.
// Should solve problems with models changing brightness when scaled.
exp_real d3d_set_normal_auto(double state)
{
    d3dcrs(D3DRS_NORMALIZENORMALS, (state > 0.0));
}

// ============================================================================
// Primitives
// ============================================================================

// Begin drawing an extended primitive.
void vertex::begin(D3DPRIMITIVETYPE primitive, bool textured)
{
    vbuff_c = 0;
    vbuff_autoinc = textured;
    vbuff_prim = primitive;
    vbuff_use_struct = false;

    // Zero the buffer.
    if (vbuff_use_ext)
	    std::memset(vbuff_ext_int, 0, vb_ext_bytes);
    else
        std::memset(vbuff_default_int, 0, vb_default_bytes);

    if (!textured)
        texture_clear_all();
}

exp_real d3d_primitive_begin_ext(double primitive, double textured)
{
    D3DPRIMITIVETYPE prim = D3DPT_POINTLIST;
    switch ((int)primitive)
    {
        case 1: { prim = D3DPT_POINTLIST; } break;
        case 2: { prim = D3DPT_LINELIST; } break;
        case 3: { prim = D3DPT_LINESTRIP; } break;
        case 4: { prim = D3DPT_TRIANGLELIST; } break;
        case 5: { prim = D3DPT_TRIANGLESTRIP; } break;
        case 6: { prim = D3DPT_TRIANGLEFAN; } break;
        default: { return gerror; } break;
    }

    vbuff_use_ext = true;
    vertex::begin(prim, textured < 0.5);
    return gtrue;
}

exp_real d3d_use_ext_vertex_format(gm_real use)
{
    bool use_ext = use > 0.5;
    if (vbuff_use_ext != use_ext)
    {
        atlas::end_draw();
        vbuff_use_ext = use_ext;
    }
    return gtrue;
}

// Position, normal, diffuse/specular colour and alpha.
void vertex::add(float x, float y, float z, float nx, float ny, float nz, uint col, uint speccol)
{
    vbuff_ext_int[vbuff_c].x = x;
    vbuff_ext_int[vbuff_c].y = y;
    vbuff_ext_int[vbuff_c].z = z;
    vbuff_ext_int[vbuff_c].nx = nx;
    vbuff_ext_int[vbuff_c].ny = ny;
    vbuff_ext_int[vbuff_c].nz = nz;
    vbuff_ext_int[vbuff_c].c = col;
    vbuff_ext_int[vbuff_c].s = speccol;

    if (vbuff_autoinc)
        vbuff_c++;
}

exp_real d3d_vertex_ext(double x, double y, double z, double nx, double ny, double nz,
    double col, double alpha, double speccol, double specalpha)
{
    vertex::add((float)x, (float)y, (float)z, (float)nx, (float)ny, (float)nz,
        col_d3d((int)col, alpha), col_d3d((int)speccol, specalpha));

    return gtrue;
}

// Set vertex texture coordinates.
// There are eight sets indexed 0-7, one for each texture stage.
void vertex::add_tex(uint stage, float xtex, float ytex)
{
    if (stage < 0) stage = 0;
    else if (stage > 7) stage = 7;

    uint ind = (uint)(stage * 2);

    vbuff_ext_int[vbuff_c].uv[ind] = xtex;
    vbuff_ext_int[vbuff_c].uv[ind + 1] = ytex;
}

exp_real d3d_vertex_ext_tex(double stage, double xtex, double ytex)
{
    vertex::add_tex((uint)stage, (float)xtex, (float)ytex);
    return gtrue;
}

// Call when finished with the current vertex to start defining the next one.
// Only required for textured primitives.
void vertex::next()
{
    if (vbuff_autoinc)
    {
        gm::show_error("d3d_vertex_ext_next() is called automatically for untextured"
            "extended primitives.", false);
    }
    vbuff_c++;
}

exp_real d3d_vertex_ext_next()
{
    vertex::next();
    return gtrue;
}

// Draw the primitive.
void vertex::end()
{
    try
    {
		uint count = vbuff_c;
        if (!vbuff_autoinc && !vbuff_use_struct)
            count += 1;

        if (count < 1)
            return;

        uint prims = 0;

        switch (vbuff_prim)
        {
            case D3DPT_POINTLIST:     prims = count; break;
            case D3DPT_LINELIST:      prims = count / 2; break;
            case D3DPT_LINESTRIP:     prims = count - 1; break;
            case D3DPT_TRIANGLELIST:  prims = count / 3; break;
            case D3DPT_TRIANGLESTRIP: prims = count - 2; break;
            case D3DPT_TRIANGLEFAN:   prims = count - 2; break;
            default: throw std::runtime_error("Unknown primitive type."); break;
        }

        if (prims < 1)
            return;

        if (!vbuff_usevs)
            D3DCheck(d3d::set_vertex_shader(true, vbuff_use_ext ? fvf_ext : fvf_default, 0), 3);

        if (vbuff_use_ext)
        {
            D3DCheck(d3d::draw_primitive_up((DWORD)vbuff_prim, prims, vbuff_ext_int,
                sizeof(vert_ext)), 4);
        }
        else
        {
            D3DCheck(d3d::draw_primitive_up((DWORD)vbuff_prim, prims, vbuff_default_int,
                sizeof(vert_default)), 4);
        }

        vbuff_c = 0;
    }
    transpond_catch("vertex::end()")
}

exp_real d3d_primitive_end_ext()
{
    try
    {
        vertex::end();
        vbuff_use_ext = false;
        return gtrue;
    }
    simple_catch("d3d_primitive_end_ext", gerror)
}

// 2D equivalent.
exp_real draw_primitive_begin_ext(double primitive, double textured)
{
    return d3d_primitive_begin_ext(primitive, textured);
}

// 2D equivalent.
// The buffer is zeroed by begin_ext; no need to set the 3D stuff here.
exp_real draw_vertex_ext(double x, double y, double col, double alpha,
    double speccol, double specalpha)
{
    vbuff_ext_int[vbuff_c].x = (float)x;
    vbuff_ext_int[vbuff_c].y = (float)y;

    vbuff_ext_int[vbuff_c].c = col_d3d((int)col, alpha);
    vbuff_ext_int[vbuff_c].s = col_d3d((int)speccol, specalpha);

    if (vbuff_autoinc)
        vbuff_c++;

    return gtrue;
}

// 2D equivalent.
exp_real draw_vertex_ext_tex(double stage, double xtex, double ytex)
{
    return d3d_vertex_ext_tex(stage, xtex, ytex);
}

// 2D equivalent.
exp_real draw_vertex_ext_next() { return d3d_vertex_ext_next(); }

// 2D equivalent.
exp_real draw_primitive_end_ext() { return d3d_primitive_end_ext(); }
