// ============================================================================
// GMGraphic 新 Shader API — GML 侧调用参考
//
// 设计定稿(2026-08-07): C++ 导出名即 shader_set_uniform_f/i/b(固定 5 参:
//   handle + 4 个值)。游戏侧用 external_define 绑定(见 Nature Edition 的
//   graphic_dll_init.gml), 调用时少传值 GM8 自动补 0 —— 无需额外变参脚本。
//
// 示例(GML):
//   var h = external_call(global._shader_get_uniform, sh, "uColor");
//   external_call(global._shader_set_uniform_f, h, 1, 0, 0);      // 3 个值, w=0
//   external_call(global._shader_set_uniform_f, h, 1, 0, 0, 1);   // 4 个值
// ============================================================================

// ============================================================================
// C++ 导出签名清单(GMGraphic.dll, 全部 dll_cdecl)
// ============================================================================
// ---- Shader API ----
//   shader_create_asm(vs, ps)                 字符串, 字符串        (双空返回 -1)
//   shader_create(src, vs, ps)                字符串 x3             (D3D9 only, D3D8 返回 -1)
//   shader_destroy(sh)                        实数
//   shader_set(sh)                            实数                  (自动 SetDefaults)
//   shader_reset()                            无
//   shader_current()                          无                    (返回当前 shader 或 -1)
//   shader_get_uniform(sh, uni)               实数, 字符串          (找不到返回 -1)
//   shader_has_uniform(sh, uni)               实数, 字符串          (D3D9 only)
//   shader_get_sampler_index(sh, uni)         实数, 字符串
//   shader_set_uniform_f(h, x, y, z, w)       实数 x5               (少传自动补 0)
//   shader_set_uniform_i(h, x, y, z, w)       实数 x5
//   shader_set_uniform_b(h, x, y, z, w)       实数 x5               (非 0 → 1)
//   shader_set_uniform_color(h, col, alpha)   实数 x3
//   shader_set_uniform_matrix(h, mtx, size)   实数 x3               (mtx: world=1/view=2/proj=4/wvp=7)
//
// ---- 采样器 ----
//   texture_set_stage(sampler, tex)           实数, 实数            (绑纹理, tex<0 清空)
//   gpu_set_texfilter_ext(sampler, filter)    实数, 实数            (0=point/1=linear/2=aniso/3=none)
//   gpu_set_texrepeat_ext(sampler, h, v, bcol) 实数 x4              (h/v 0→clamp, bcol=GM 颜色)
//   gpu_set_tex_max_aniso_ext(sampler, aniso) 实数, 实数            (1,2,4,8,16)
//   gpu_set_tex_mip_filter_ext(sampler, filter) 实数, 实数          (0=none/1=point/3=linear)
//   gpu_set_tex_border_ext(sampler, col, alpha) 实数 x3
//
// ---- GPU Control ----
//   gpu_set_fog(enable, col, start, end, mode, density) 实数 x6     (mode: 0=线性/1=exp/2=exp2; 传 4 参=线性)
//   gpu_set_zwriteenable(enable)              实数
//   gpu_set_ztestenable(enable)               实数
//   gpu_set_ztestfunc(func)                   实数                 (cmp_ 常量)
//   gpu_set_alphatestenable(enable)           实数
//   gpu_set_alphatestref(ref)                 实数                 (0-255)
//   gpu_set_alphatestfunc(func)               实数                 (cmp_ 常量, D3D 扩展)
//   gpu_set_colourwriteenable(enable, r,g,b,a) 实数 x5
//   gpu_set_depth(depth)                      实数                 (0-16)
//   gpu_set_fillmode(mode)                    实数
//
// ---- 保留 d3d_(GMS2 无对等 / 图元) ----
//   d3d_set_normal_auto(state)
//   d3d_set_point_size / size_min / size_max / scale / scale_coef / sprite
//   d3d_primitive_begin_ext / d3d_vertex_ext / d3d_vertex_ext_tex / d3d_vertex_ext_next / d3d_primitive_end_ext
//   draw_primitive_begin_ext / draw_vertex_ext / draw_vertex_ext_tex / draw_vertex_ext_next / draw_primitive_end_ext
// ============================================================================
