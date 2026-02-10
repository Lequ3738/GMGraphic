// Defines function prototypes so the compiler doesn't go full retard.

#include <string>
#include <vector>
#include <fstream>
#include <math.h>
#include "gmapi.h"
#include "d3dx8.h"

typedef double gm_real;
typedef const char* gm_string;
typedef gm::CGMVariable var;

#define expdll extern "C" __declspec(dllexport)
#define exp_real expdll gm_real _cdecl
#define exp_str expdll gm_string _cdecl
#define exp_uint expdll unsigned int _cdecl

// Internal

inline double roundd(double x);
inline double frac(double x);
inline bool   eps(double a, double b);
inline bool   eps2(double a, double b, double eps);
inline double slope(double x1, double y1, double x2, double y2);
inline double lerp(double vala, double valb, double x);
inline double cerp(double vala, double valb, double x);
inline double curp(double a, double b, double c, double d, double x);
inline double unlerp(double val, double minval, double maxval);
inline double clamp(double val, double minval, double maxval);
inline double snap_low(double x, double cellw);
inline double snap_high(double x, double cellw);
inline double snap_near(double x, double cellw);
inline void   tr_rot(double& x, double& y, double dir);
inline void   tr_move(double& x, double& y, double len, double dir);
inline void   tr_scale(double& x, double& y, double scale);
inline int    col_red(int col);
inline int    col_green(int col);
inline int    col_blue(int col);
inline int    col_make(int r, int g, int b);
int           col_lerp(int cola, int colb, double a);



// External

exp_real init();

exp_str d3d_dev_get_name();
exp_real d3d_dev_get_point_max_size();
exp_real d3d_dev_get_ps_version();
exp_real d3d_dev_get_tex_max_width();
exp_real d3d_dev_get_tex_max_height();
exp_real d3d_dev_get_tex_max_stages();
exp_real d3d_dev_get_tex_mem();

exp_real d3d_ps_create(char* src_asm);
exp_real d3d_ps_destroy(double shader);
exp_real d3d_set_ps(double shader);
exp_real d3d_set_ps_ext(double shader, double conf);
exp_real d3d_set_ps_const(double constant, double r, double g, double b, double a);
exp_real d3d_set_ps_const_col(double constant, double col, double alpha);
exp_real d3d_set_ps_conf(double conf);

exp_real d3d_vs_create(char* src_asm);
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
















