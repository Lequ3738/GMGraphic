// ============================================================================
// GMGraphic 新 Shader API — GML 变参分发脚本
//
// 依据: 桌面 "Shader API 设计.md" §1 末注
//   "GML 层统一变参: shader_set_uniform_f(h, ...) 支持 1-4 个值, 用
//    do switch argument_count 分发到 C++ 固定参导出(_4f 等)"
//
// 用法: 把下面的脚本复制进游戏工程(GM8 脚本资源, 名字与 #define 相同)。
//       同时要在 GM8 全局设置 → DLL 里绑定对应 C++ 导出(shader_set_uniform_4f
//       等, 见文件末的绑定清单)。C++ 导出名以 DLL 实际导出为准。
//
// 固定参 C++ 导出(GM8 DLL 绑定, 名字=GMGraphic.dll 导出名):
//   shader_set_uniform_4f(handle, x, y, z, w)  5 个实数
//   shader_set_uniform_4i(handle, x, y, z, w)  5 个实数
//   shader_set_uniform_4b(handle, x, y, z, w)  5 个实数
// ============================================================================


// shader_set_uniform_f(uniform, v1, [v2, v3, v4])
// 浮点 uniform, 1-4 个值; 缺省分量补 0。
#define shader_set_uniform_f
    ///shader_set_uniform_f(uniform, v1, [v2, v3, v4])
    do switch (argument_count - 1) {
        case 1: shader_set_uniform_4f(argument0, argument[1], 0, 0, 0) exit
        case 2: shader_set_uniform_4f(argument0, argument[1], argument[2], 0, 0) exit
        case 3: shader_set_uniform_4f(argument0, argument[1], argument[2], argument[3], 0) exit
        default: shader_set_uniform_4f(argument0, argument[1], argument[2], argument[3], argument[4])
    }


// shader_set_uniform_i(uniform, v1, [v2, v3, v4])
// 整数 uniform(以 float 写入 D3D9 寄存器)。
#define shader_set_uniform_i
    ///shader_set_uniform_i(uniform, v1, [v2, v3, v4])
    do switch (argument_count - 1) {
        case 1: shader_set_uniform_4i(argument0, argument[1], 0, 0, 0) exit
        case 2: shader_set_uniform_4i(argument0, argument[1], argument[2], 0, 0) exit
        case 3: shader_set_uniform_4i(argument0, argument[1], argument[2], argument[3], 0) exit
        default: shader_set_uniform_4i(argument0, argument[1], argument[2], argument[3], argument[4])
    }


// shader_set_uniform_b(uniform, v1, [v2, v3, v4])
// 布尔 uniform(非 0 → 1.0)。
#define shader_set_uniform_b
    ///shader_set_uniform_b(uniform, v1, [v2, v3, v4])
    do switch (argument_count - 1) {
        case 1: shader_set_uniform_4b(argument0, argument[1], 0, 0, 0) exit
        case 2: shader_set_uniform_4b(argument0, argument[1], argument[2], 0, 0) exit
        case 3: shader_set_uniform_4b(argument0, argument[1], argument[2], argument[3], 0) exit
        default: shader_set_uniform_4b(argument0, argument[1], argument[2], argument[3], argument[4])
    }


// ============================================================================
// 固定参导出绑定清单(GM8 全局设置 → DLL)
// 名字 = GMGraphic.dll 导出名; 类型: 返回实数, 参数类型按下面标注。
//
//   shader_create_asm(vs, ps)         参数: 字符串, 字符串
//   shader_create(src, vs, ps)        参数: 字符串, 字符串, 字符串  (D3D9 only)
//   shader_destroy(sh)                参数: 实数
//   shader_set(sh)                    参数: 实数
//   shader_reset()                    参数: (无)
//   shader_current()                  参数: (无)
//   shader_get_uniform(sh, uni)       参数: 实数, 字符串
//   shader_has_uniform(sh, uni)       参数: 实数, 字符串  (D3D9 only)
//   shader_get_sampler_index(sh, uni) 参数: 实数, 字符串
//   shader_set_uniform_4f(h,x,y,z,w)  参数: 实数 x5
//   shader_set_uniform_4i(h,x,y,z,w)  参数: 实数 x5
//   shader_set_uniform_4b(h,x,y,z,w)  参数: 实数 x5
//   shader_set_uniform_color(h,col,a) 参数: 实数 x3
//   shader_set_uniform_matrix(h,mtx,n) 参数: 实数 x3
//   texture_set_stage(samp,tex)       参数: 实数, 实数
//   texture_set_stage_interpolation(samp,mode)   参数: 实数, 实数
//   texture_set_stage_repeat(samp,h,v,border_col) 参数: 实数 x4
//   texture_set_stage_border(samp,col,alpha)     参数: 实数 x3
//   texture_set_stage_anisotropy(samp,aniso)     参数: 实数, 实数
//   texture_set_stage_mipmap(samp,mode)          参数: 实数, 实数
// ============================================================================
