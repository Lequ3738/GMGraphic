#pragma once
#include "../main.h"
#include "structs.h"

extern vert_ext vbuff_ext_int[vb_count];
extern vert_default vbuff_default_int[vb_count];
extern uint vbuff_c;
extern D3DPRIMITIVETYPE vbuff_prim;
extern bool vbuff_usevs;
extern bool vbuff_autoinc;
extern bool vbuff_use_struct;
extern bool vbuff_use_ext;

// SDF 字体着色器。init() 里按后端创建:
//   DX8 = asm ps_1.4(ps_sdf / ps_sdf_premul); DX9 = HLSL ps_2_0/3_0(ps_sdf_hlsl, smoothstep 阈值)。
extern int sdf_shader;
extern int sdf_shader_premul;
extern int sdf_shader_uniform;          // DX8: c0(scale/thickness) 句柄 = shader_get_uniform(sdf_shader, "ps.0")
extern int sdf_shader_uniform_buffer;   // DX9: "u_buffer"(thickness) 句柄
extern int sdf_shader_uniform_gamma;    // DX9: "u_gamma"(边缘软度) 句柄

namespace gm
{
	extern int argument_list;
}

exp_real init(gm_real arg_list);

exp_str d3d_dev_get_name();
exp_real d3d_dev_get_point_max_size();
exp_real d3d_dev_get_ps_version();
exp_real d3d_dev_get_tex_max_width();
exp_real d3d_dev_get_tex_max_height();
exp_real d3d_dev_get_tex_max_stages();
exp_real d3d_dev_get_tex_mem();

// ---- Shader API(GMS2 风格, 权威文档: 桌面 "Shader API 设计.md") ----
// PS/VS 统一为一个 shader 资源(bundle); 空字符串 = 不用该阶段(空 VS = passthrough FVF)。
// uniform 句柄 = 统一 map 索引(不透明), 结构体 {ps_reg, vs_reg} 编码目标阶段。

// 创建 asm 着色器(D3D8/9 通用)。vs/ps 空字符串 = 不用该阶段; 双空返回 -1。
exp_real shader_create_asm(const char* vs, const char* ps);
// 创建 HLSL 着色器(单文件双入口, D3D9 only; D3D8 返回 -1)。
// vs_entry/ps_entry 空 → 用约定默认入口 mainVS/mainPS; 源码里没有则该阶段 passthrough。
exp_real shader_create(const char* src, const char* vs_entry, const char* ps_entry);
exp_real shader_destroy(double sh);      // 释放 vs/ps 对象 + 常量表 + 该 shader 的 uniform 条目
exp_real shader_set(double sh);          // 绑定 vs+ps; 自动 SetDefaults(仅 HLSL 常量表)
exp_real shader_reset();                 // 解除 shader(回 FVF), 等价 shader_set(-1)
exp_real shader_current();               // 当前绑定 shader, 无则 -1

// uniform: asm 传寄存器数字串("0"/"ps.0"/"vs.3"); HLSL 传常量名("uColor"/"ps.uColor"/"vs.uWVP")。
// 前缀大小写不敏感。找不到返回 -1。
exp_real shader_get_uniform(double sh, const char* uni);
exp_real shader_has_uniform(double sh, const char* uni);      // HLSL 专用(D3D9), 依赖常量表
exp_real shader_get_sampler_index(double sh, const char* uni);// 采样器句柄, 配 texture_set_stage

// uniform 写入: 固定参导出, GML 层变参脚本分发到 _f/_i/_b。
exp_real shader_set_uniform_f(double h, double x, double y, double z, double w);
exp_real shader_set_uniform_i(double h, double x, double y, double z, double w);
exp_real shader_set_uniform_b(double h, double x, double y, double z, double w);
exp_real shader_set_uniform_color(double h, double col, double alpha);
// mtx_type 掩码: world=1 / view=2 / projection=4 / wvp=7(gm82dx9 式)。size = 写几个寄存器(默认 4)。
exp_real shader_set_uniform_matrix(double h, double mtx_type, double size);

// 采样器 stage。绑纹理用 texture_set_stage(GMS2 同名); 参数控制用 gpu_set_tex*_ext(GMS2 gpu_* 家族)。
exp_real texture_set_stage(double samp, double tex);
// GMS2 gpu_set_texfilter_ext, 参数扩展为 D3D 过滤值: 0=point/1=linear/2=anisotropic/3=none。
exp_real gpu_set_texfilter_ext(double sampler, double filter);
// GMS2 gpu_set_texrepeat_ext, 扩展 h/v 双轴寻址 + border 色(0→clamp, 否则 D3DTADDRESS_* 值)。
exp_real gpu_set_texrepeat_ext(double sampler, double h, double v, double border_col);
// GMS2 gpu_set_tex_max_aniso_ext。值 1,2,4,8,16。1=无。
exp_real gpu_set_tex_max_aniso_ext(double sampler, double maxaniso);
// GMS2 gpu_set_tex_mip_filter_ext。0=none/1=point/3=linear。
exp_real gpu_set_tex_mip_filter_ext(double sampler, double filter);
// D3D 扩展(无 GMS2 对等): border 寻址模式用的颜色。
exp_real gpu_set_tex_border_ext(double sampler, double col, double alpha);

// 内部助手(不导出): 清空所有纹理 stage。
void texture_clear_all();

// ---- GPU Control(GMS2 gpu_* 系列)----
// Fog 合并成一个 gpu_set_fog(不严格对齐 GMS2, 扩展 mode/density 保留 D3D 控制)。
// mode: 0=线性(默认, 用 start/end)/1=exp/2=exp2(用 density)。GML 传 4 参 → GM8 补 mode=0/density=0。
exp_real gpu_set_fog(double enable, double colour, double start, double end, double mode, double density);

exp_real d3d_set_point_size(double size);
exp_real d3d_set_point_size_min(double size);
exp_real d3d_set_point_size_max(double size);
exp_real d3d_set_point_scale(double state);
exp_real d3d_set_point_scale_coef(double coef1, double coef2, double coef3);
exp_real d3d_set_point_sprite(double state);

// 颜色写掩码(GMS2 gpu_set_colourwriteenable): enable 总开关 + 各通道。全部启用时全开。
exp_real gpu_set_colourwriteenable(double enable, double r, double g, double b, double a);
exp_real gpu_set_zwriteenable(double enable);   // GMS2 同名, Z 缓冲写入
exp_real gpu_set_ztestenable(double enable);    // GMS2 同名, Z 测试开关(拆自 d3d_set_ztest)
exp_real gpu_set_ztestfunc(double func);        // GMS2 同名, Z 比较函数
exp_real gpu_set_alphatestenable(double enable);// GMS2 同名, Alpha 测试开关(拆自 d3d_set_alphatest)
exp_real gpu_set_alphatestref(double ref);      // GMS2 同名, 参考值 0-255
exp_real gpu_set_alphatestfunc(double func);    // D3D 扩展, Alpha 比较函数
exp_real gpu_set_depth(double depth);           // GMS2 同名, 深度偏移(原 d3d_set_zbias)
exp_real gpu_set_fillmode(double mode);         // GMS2 同名
exp_real gpu_set_normal_auto(double state);     // 自动归一化法线(GMS2 无对等, gpu_ 扩展)

exp_real d3d_use_ext_vertex_format(gm_real use);
exp_real d3d_primitive_begin_ext(double primitive, double textured);
exp_real d3d_vertex_ext(double x, double y, double z, double nx, double ny, double nz, double col, double alpha, double speccol, double specalpha);
exp_real d3d_vertex_ext_tex(double set, double xtex, double ytex);
exp_real d3d_vertex_ext_next();
exp_real d3d_primitive_end_ext();
exp_real draw_primitive_begin_ext(double primitive, double textured);
exp_real draw_vertex_ext(double x, double y, double col, double alpha, double speccol, double specalpha);
exp_real draw_vertex_ext_tex(double set, double xtex, double ytex);
exp_real draw_vertex_ext_next();
exp_real draw_primitive_end_ext();

namespace vertex
{
	void begin(D3DPRIMITIVETYPE primitive, bool textured);
	void add(float x, float y, float z, float nx, float ny, float nz, uint col, uint speccol);
	void add_tex(uint stage, float xtex, float ytex);
	void next();
	void end();

	inline void push_vertex_2d(float x, float y, float u, float v, dword c)
    {
        vbuff_use_struct = true;
        if (vbuff_use_ext)
        {
            vert_ext* vert = &vbuff_ext_int[vbuff_c++];
            vert->x = x; vert->y = y; vert->c = c; vert->uv[0] = u; vert->uv[1] = v;
        }
        else
        {
            vert_default* vert = &vbuff_default_int[vbuff_c++];
            vert->x = x; vert->y = y; vert->c = c; vert->uv[0] = u; vert->uv[1] = v;
        }
    }
}