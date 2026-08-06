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

extern dword ps_sdf_comp;
extern dword ps_sdf_premul_comp;

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

exp_real d3d_ps_create(const char* src_asm);
exp_real d3d_ps_destroy(double shader);
exp_real d3d_set_ps(double shader);
exp_real d3d_set_ps_ext(double shader, double conf);
exp_real d3d_set_ps_const(double constant, double r, double g, double b, double a);
exp_real d3d_set_ps_const_col(double constant, double col, double alpha);
exp_real d3d_set_ps_conf(double conf);

exp_real d3d_vs_create(const char* src_asm);
exp_real d3d_vs_destroy(double shader);
exp_real d3d_set_vs(double shader);
exp_real d3d_set_vs_const(double constant, double x, double y, double z, double w);
exp_real d3d_set_vs_const_col(double constant, double col, double alpha);
exp_real d3d_set_vs_const_matrix(double constant);
exp_real d3d_set_vs_conf(double conf);

exp_real d3d_set_tex(double stage, double tex);
exp_real d3d_set_tex_all(double tex);
exp_real d3d_set_tex_int(double stage, double mode);
exp_real d3d_set_tex_wrap(double stage, double xmode, double ymode);
exp_real d3d_set_tex_border(double stage, double col, double alpha);
exp_real d3d_set_tex_aniso(double stage, double anisotropy);
exp_real d3d_set_tex_mip(double stage, double mode);
exp_real d3d_set_tex_conf(double conf);

exp_real d3d_conf_ps_create();
exp_real d3d_conf_ps_set(double conf, double constant, double r, double g, double b, double a);
exp_real d3d_conf_vs_create();
exp_real d3d_conf_vs_set(double conf, double constant, double x, double y, double z, double w);
exp_real d3d_conf_tex_create();
exp_real d3d_conf_tex_set(double conf, double stage, double tex, double interp, double xmode, double ymode);

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