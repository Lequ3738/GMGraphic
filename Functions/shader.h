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

// SDF 字体着色器(asm ps_1.4)的 shader id 与 uniform 句柄。init() 里创建。
extern int sdf_shader;
extern int sdf_shader_premul;
extern int sdf_shader_uniform;   // c0(scale/thickness) 的 uniform 句柄 = shader_get_uniform(sdf_shader, "ps.0")

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

// uniform 写入: 固定参导出, GML 层变参脚本分发到 _4f/_4i/_4b。
exp_real shader_set_uniform_4f(double h, double x, double y, double z, double w);
exp_real shader_set_uniform_4i(double h, double x, double y, double z, double w);
exp_real shader_set_uniform_4b(double h, double x, double y, double z, double w);
exp_real shader_set_uniform_color(double h, double col, double alpha);
// mtx_type 掩码: world=1 / view=2 / projection=4 / wvp=7(gm82dx9 式)。size = 写几个寄存器(默认 4)。
exp_real shader_set_uniform_matrix(double h, double mtx_type, double size);

// 采样器 stage(对应旧 d3d_set_tex* 系列)。
exp_real texture_set_stage(double samp, double tex);
exp_real texture_set_stage_interpolation(double samp, double mode);
exp_real texture_set_stage_repeat(double samp, double h, double v, double border_col);  // 0→clamp, border_col 为 GM 颜色
exp_real texture_set_stage_border(double samp, double col, double alpha);
exp_real texture_set_stage_anisotropy(double samp, double aniso);
exp_real texture_set_stage_mipmap(double samp, double mode);

// 内部助手(不导出): 清空所有纹理 stage。
void texture_clear_all();

exp_real d3d_set_fog_state(double state);
exp_real d3d_set_fog_type(double fog_type);
exp_real d3d_set_fog_density(double density);
exp_real d3d_set_fog_color(double col);
exp_real d3d_set_fog_start(double dist);
exp_real d3d_set_fog_end(double dist);

exp_real d3d_set_point_size(double size);
exp_real d3d_set_point_size_min(double size);
exp_real d3d_set_point_size_max(double size);
exp_real d3d_set_point_scale(double state);
exp_real d3d_set_point_scale_coef(double coef1, double coef2, double coef3);
exp_real d3d_set_point_sprite(double state);

exp_real d3d_set_mask(double r, double g, double b, double a);
exp_real d3d_set_zwrite(double state);
exp_real d3d_set_alphatest(double value, double mode);
exp_real d3d_set_ztest(double mode);
exp_real d3d_set_zbias(double bias);
exp_real d3d_set_fillmode(double mode);
exp_real d3d_set_normal_auto(double state);
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