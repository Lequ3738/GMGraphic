#pragma once
// GMGraphic 双后端适配器(D3D8/D3D9 运行时自动分发): 本头只用 DWORD/UINT/指针/float/std 类型,
// 实现分 d3d_adapter8.cpp/d3d_adapter9.cpp 两 TU 各含一个版本的 D3D 头; 共享代码沿用 D3D8 枚举(DX8/DX9 数值一致)。
#include <windows.h>
#include <string>
#include <vector>

namespace d3d
{
    enum Version : int { UNKNOWN = 0, V8 = 8, V9 = 9 };
    enum VertexFmt { VERT_DEFAULT, VERT_EXT };   // GM 的两种顶点布局

    // 只保留 GMGraphic 实际读取的设备能力字段。
    struct Caps
    {
        float max_point_size;          // D3D8 是 FLOAT, D3D9 是 DWORD(同 bit), 统一存 float
        DWORD pixel_shader_version;
        DWORD vertex_shader_version;   // HLSL vs profile 自适应(ps_2_0/3_0 同理)
        DWORD max_tex_w, max_tex_h, max_tex_stages, max_aniso;
        DWORD prim_misc_caps, raster_caps;
        char  adapter_desc[512];
    };

    // ---- uniform 常量寄存器类型 ----
    // SM3.0 才有独立 int(iN)/bool(bN) 寄存器组; SM2.0/D3D8 全在 float 寄存器, int/bool uniform 编进 float → RegisterSet 报 FLOAT4, 自然回退。
    enum ConstKind : int { CK_NONE = -1, CK_FLOAT = 0, CK_INT = 1, CK_BOOL = 2 };
    // count = D3DXCONSTANT_DESC.RegisterCount(占用的寄存器数): FLOAT/INT 按 int4 计(标量/向量=1),
    // BOOL 按单个布尔寄存器计(bool 标量=1, bool4=4)。写入时 FLOAT/INT 恒用 1(BOOL 用 count)。
    struct UniformLoc { int reg = -1; int kind = CK_NONE; int count = 1; };   // kind 取 ConstKind

    // ---- 初始化 / 检测 ----
    int  version();                          // 惰性检测并缓存; 未初始化时默认 V8
    void ensure_version(void* device, void* iface);

    // 原始 COM 指针访问(仅适配器实现内部使用)。
    void* device();
    void* iface();

    // ---- 内部实现: 中性签名, 定义在 d3d_adapter8.cpp / d3d_adapter9.cpp ----
    namespace impl8
    {
        HRESULT set_render_state(DWORD, DWORD);
        HRESULT set_tex_stage_state(DWORD, DWORD, DWORD);
        HRESULT get_transform(DWORD, float*);
        HRESULT draw_primitive_up(DWORD, DWORD, const void*, DWORD);
        UINT    get_available_tex_mem();

        HRESULT assemble_ps(const char*, size_t, std::vector<BYTE>&, std::string*);
        HRESULT assemble_vs(const char*, size_t, std::vector<BYTE>&, std::vector<BYTE>&, std::string*);
        HRESULT create_pixel_shader(const BYTE*, DWORD*);
        HRESULT delete_pixel_shader(DWORD);
        HRESULT set_pixel_shader(DWORD);
        HRESULT get_pixel_shader(DWORD*);
        HRESULT set_ps_const_typed(DWORD, ConstKind, const float*, DWORD);

        HRESULT create_vertex_shader(VertexFmt, const BYTE*, const BYTE*, size_t, DWORD*);
        HRESULT delete_vertex_shader(DWORD);
        HRESULT set_vertex_shader(bool fvf_mode, DWORD fvf, DWORD handle);
        // ps-only 着色器配套: 无自定义 VS 时绑"仿固定管线 VS"(D3D9 = 透传 VS + 声明 + WVP;
        // D3D8 = 固定顶点管线 FVF)。用于 ps_3_0 像素着色器的 v0/v1 输入喂给。
        HRESULT set_vertex_shader_passthrough(VertexFmt);
        HRESULT set_vs_const_typed(DWORD, ConstKind, const float*, DWORD);

        // HLSL 依赖 D3DX9 常量表, D3D8 不支持 —— 桩实现(一律失败/返回空)。
        HRESULT compile_hlsl(const char*, size_t, const char*, const char*,
                             std::vector<BYTE>&, void**, std::string*);
        HRESULT constant_table_set_defaults(void*);
        void*   constant_table_get_constant_by_name(void*, const char*);
        UniformLoc constant_table_get_uniform(void*, void*);   // 返回 {寄存器号, ConstKind}; 失败 reg=-1
        int     constant_table_get_sampler_register(void*, void*);

        HRESULT set_texture(DWORD, void*);
        HRESULT create_texture(UINT, UINT, UINT, DWORD, DWORD, DWORD, void**);
        HRESULT upload_texture(void*, UINT, UINT, DWORD, const void*, UINT);
        void    release(void*);
        HRESULT read_texture(void*, std::vector<BYTE>&, UINT&, UINT&);

        // ---- vertex_* vertex-buffer pipeline (D3D9 only; D3D8 stubs return E_FAIL) ----
        HRESULT set_vertex_declaration(void*);
        HRESULT get_vertex_declaration(void**);
        HRESULT get_vertex_shader(DWORD*);
        HRESULT set_vertex_shader_handle(DWORD);
        HRESULT get_fvf(DWORD*);
        HRESULT set_fvf(DWORD);
        HRESULT draw_primitive(DWORD, DWORD, DWORD);
        HRESULT create_vertex_buffer(UINT, void**);
        HRESULT upload_vertex_buffer(void*, const void*, UINT);
        HRESULT set_stream_source(DWORD, void*, DWORD);
        // elems = D3DVERTEXELEMENT9-layout array (see vertex.h); count excludes D3DDECL_END.
        HRESULT create_vertex_declaration(const void*, UINT, void**);
        // Bind passthrough VS onto a custom decl (vertex_submit with no VS); also refreshes WVP.
        HRESULT set_vertex_shader_passthrough_decl(void*);

        std::string error_text(HRESULT);
        bool get_caps(Caps&);
    }
    namespace impl9
    {
        HRESULT set_render_state(DWORD, DWORD);
        HRESULT set_tex_stage_state(DWORD, DWORD, DWORD);
        HRESULT get_transform(DWORD, float*);
        HRESULT draw_primitive_up(DWORD, DWORD, const void*, DWORD);
        UINT    get_available_tex_mem();

        HRESULT assemble_ps(const char*, size_t, std::vector<BYTE>&, std::string*);
        HRESULT assemble_vs(const char*, size_t, std::vector<BYTE>&, std::vector<BYTE>&, std::string*);
        HRESULT create_pixel_shader(const BYTE*, DWORD*);
        HRESULT delete_pixel_shader(DWORD);
        HRESULT set_pixel_shader(DWORD);
        HRESULT get_pixel_shader(DWORD*);
        HRESULT set_ps_const_typed(DWORD, ConstKind, const float*, DWORD);

        HRESULT create_vertex_shader(VertexFmt, const BYTE*, const BYTE*, size_t, DWORD*);
        HRESULT delete_vertex_shader(DWORD);
        HRESULT set_vertex_shader(bool fvf_mode, DWORD fvf, DWORD handle);
        // ps-only 着色器配套: 无自定义 VS 时绑"仿固定管线 VS"(D3D9 = 透传 VS + 声明 + WVP;
        // D3D8 = 固定顶点管线 FVF)。用于 ps_3_0 像素着色器的 v0/v1 输入喂给。
        HRESULT set_vertex_shader_passthrough(VertexFmt);
        void*   get_passthrough_vs_ptr();  // 透传 VS 变量地址(&s_passthrough_vs), 供注册到 GMDirectX9
        HRESULT set_vs_const_typed(DWORD, ConstKind, const float*, DWORD);

        // HLSL(D3D9 专属): D3DXCompileShader + ID3DXConstantTable, 常量表是 COM 对象,
        // 用公共 release(void*) 释放。table 可为空(如 compile_hlsl 失败时不写)。
        HRESULT compile_hlsl(const char* src, size_t len, const char* entry, const char* profile,
                             std::vector<BYTE>& code, void** table, std::string* err);
        HRESULT constant_table_set_defaults(void* table);
        void*   constant_table_get_constant_by_name(void* table, const char* name);
        UniformLoc constant_table_get_uniform(void* table, void* handle);        // {寄存器号, ConstKind}; 失败 reg=-1
        int     constant_table_get_sampler_register(void* table, void* handle);  // 采样器寄存器号(sN), 非采样器返回 -1

        HRESULT set_texture(DWORD, void*);
        HRESULT create_texture(UINT, UINT, UINT, DWORD, DWORD, DWORD, void**);
        HRESULT upload_texture(void*, UINT, UINT, DWORD, const void*, UINT);
        void    release(void*);
        HRESULT read_texture(void*, std::vector<BYTE>&, UINT&, UINT&);

        // ---- vertex_* vertex-buffer pipeline (D3D9 only; D3D8 stubs return E_FAIL) ----
        HRESULT set_vertex_declaration(void*);
        HRESULT get_vertex_declaration(void**);
        HRESULT get_vertex_shader(DWORD*);
        HRESULT set_vertex_shader_handle(DWORD);
        HRESULT get_fvf(DWORD*);
        HRESULT set_fvf(DWORD);
        HRESULT draw_primitive(DWORD, DWORD, DWORD);
        HRESULT create_vertex_buffer(UINT, void**);
        HRESULT upload_vertex_buffer(void*, const void*, UINT);
        HRESULT set_stream_source(DWORD, void*, DWORD);
        HRESULT create_vertex_declaration(const void*, UINT, void**);
        HRESULT set_vertex_shader_passthrough_decl(void*);

        std::string error_text(HRESULT);
        bool get_caps(Caps&);
    }

    // ---- 公共 API(运行时按后端分发, 每处一行) ----
    inline HRESULT set_render_state(DWORD s, DWORD v)
    { return version() == V9 ? impl9::set_render_state(s, v) : impl8::set_render_state(s, v); }
    inline HRESULT set_tex_stage_state(DWORD stage, DWORD type, DWORD v)
    { return version() == V9 ? impl9::set_tex_stage_state(stage, type, v) : impl8::set_tex_stage_state(stage, type, v); }
    inline HRESULT get_transform(DWORD state, float* m16)
    { return version() == V9 ? impl9::get_transform(state, m16) : impl8::get_transform(state, m16); }
    inline HRESULT draw_primitive_up(DWORD prim, DWORD count, const void* verts, DWORD stride)
    { return version() == V9 ? impl9::draw_primitive_up(prim, count, verts, stride) : impl8::draw_primitive_up(prim, count, verts, stride); }
    inline UINT get_available_tex_mem()
    { return version() == V9 ? impl9::get_available_tex_mem() : impl8::get_available_tex_mem(); }

    inline HRESULT assemble_ps(const char* src, size_t len, std::vector<BYTE>& code, std::string* err)
    { return version() == V9 ? impl9::assemble_ps(src, len, code, err) : impl8::assemble_ps(src, len, code, err); }
    inline HRESULT assemble_vs(const char* src, size_t len, std::vector<BYTE>& code, std::vector<BYTE>& constants, std::string* err)
    { return version() == V9 ? impl9::assemble_vs(src, len, code, constants, err) : impl8::assemble_vs(src, len, code, constants, err); }
    inline HRESULT create_pixel_shader(const BYTE* code, DWORD* handle)
    { return version() == V9 ? impl9::create_pixel_shader(code, handle) : impl8::create_pixel_shader(code, handle); }
    inline HRESULT delete_pixel_shader(DWORD handle)
    { return version() == V9 ? impl9::delete_pixel_shader(handle) : impl8::delete_pixel_shader(handle); }
    inline HRESULT set_pixel_shader(DWORD handle)
    { return version() == V9 ? impl9::set_pixel_shader(handle) : impl8::set_pixel_shader(handle); }
    inline HRESULT get_pixel_shader(DWORD* handle)
    { return version() == V9 ? impl9::get_pixel_shader(handle) : impl8::get_pixel_shader(handle); }
    inline HRESULT set_ps_const_typed(DWORD reg, ConstKind kind, const float* v, DWORD count)
    { return version() == V9 ? impl9::set_ps_const_typed(reg, kind, v, count) : impl8::set_ps_const_typed(reg, kind, v, count); }

    inline HRESULT create_vertex_shader(VertexFmt fmt, const BYTE* code, const BYTE* constants, size_t constants_sz, DWORD* handle)
    { return version() == V9 ? impl9::create_vertex_shader(fmt, code, constants, constants_sz, handle) : impl8::create_vertex_shader(fmt, code, constants, constants_sz, handle); }
    inline HRESULT delete_vertex_shader(DWORD handle)
    { return version() == V9 ? impl9::delete_vertex_shader(handle) : impl8::delete_vertex_shader(handle); }
    inline HRESULT set_vertex_shader(bool fvf_mode, DWORD fvf, DWORD handle)
    { return version() == V9 ? impl9::set_vertex_shader(fvf_mode, fvf, handle) : impl8::set_vertex_shader(fvf_mode, fvf, handle); }
    inline HRESULT set_vertex_shader_passthrough(VertexFmt fmt)
    { return version() == V9 ? impl9::set_vertex_shader_passthrough(fmt) : impl8::set_vertex_shader_passthrough(fmt); }
    inline HRESULT set_vs_const_typed(DWORD reg, ConstKind kind, const float* v, DWORD count)
    { return version() == V9 ? impl9::set_vs_const_typed(reg, kind, v, count) : impl8::set_vs_const_typed(reg, kind, v, count); }

    // HLSL(D3D9 专属; D3D8 走 impl8 桩, 一律失败/返回空)。
    inline HRESULT compile_hlsl(const char* src, size_t len, const char* entry, const char* profile,
                                std::vector<BYTE>& code, void** table, std::string* err)
    { return version() == V9 ? impl9::compile_hlsl(src, len, entry, profile, code, table, err)
                             : impl8::compile_hlsl(src, len, entry, profile, code, table, err); }
    inline HRESULT constant_table_set_defaults(void* table)
    { return version() == V9 ? impl9::constant_table_set_defaults(table) : impl8::constant_table_set_defaults(table); }
    inline void* constant_table_get_constant_by_name(void* table, const char* name)
    { return version() == V9 ? impl9::constant_table_get_constant_by_name(table, name) : impl8::constant_table_get_constant_by_name(table, name); }
    inline UniformLoc constant_table_get_uniform(void* table, void* handle)
    { return version() == V9 ? impl9::constant_table_get_uniform(table, handle) : impl8::constant_table_get_uniform(table, handle); }
    inline int constant_table_get_sampler_register(void* table, void* handle)
    { return version() == V9 ? impl9::constant_table_get_sampler_register(table, handle) : impl8::constant_table_get_sampler_register(table, handle); }

    inline HRESULT set_texture(DWORD stage, void* tex)
    { return version() == V9 ? impl9::set_texture(stage, tex) : impl8::set_texture(stage, tex); }
    inline HRESULT create_texture(UINT w, UINT h, UINT levels, DWORD usage, DWORD fmt, DWORD pool, void** out)
    { return version() == V9 ? impl9::create_texture(w, h, levels, usage, fmt, pool, out) : impl8::create_texture(w, h, levels, usage, fmt, pool, out); }
    inline HRESULT upload_texture(void* tex, UINT w, UINT h, DWORD fmt, const void* px, UINT pitch)
    { return version() == V9 ? impl9::upload_texture(tex, w, h, fmt, px, pitch) : impl8::upload_texture(tex, w, h, fmt, px, pitch); }
    inline void release(void* com)
    { if (version() == V9) impl9::release(com); else impl8::release(com); }
    inline HRESULT read_texture(void* tex, std::vector<BYTE>& dest, UINT& width, UINT& height)
    { return version() == V9 ? impl9::read_texture(tex, dest, width, height) : impl8::read_texture(tex, dest, width, height); }

    // ---- vertex_* vertex-buffer pipeline (D3D9 only; D3D8 stubs return E_FAIL) ----
    inline HRESULT set_vertex_declaration(void* decl)
    { return version() == V9 ? impl9::set_vertex_declaration(decl) : impl8::set_vertex_declaration(decl); }
    inline HRESULT get_vertex_declaration(void** decl)
    { return version() == V9 ? impl9::get_vertex_declaration(decl) : impl8::get_vertex_declaration(decl); }
    inline HRESULT get_vertex_shader(DWORD* handle)
    { return version() == V9 ? impl9::get_vertex_shader(handle) : impl8::get_vertex_shader(handle); }
    inline HRESULT set_vertex_shader_handle(DWORD handle)
    { return version() == V9 ? impl9::set_vertex_shader_handle(handle) : impl8::set_vertex_shader_handle(handle); }
    inline HRESULT get_fvf(DWORD* fvf)
    { return version() == V9 ? impl9::get_fvf(fvf) : impl8::get_fvf(fvf); }
    inline HRESULT set_fvf(DWORD fvf)
    { return version() == V9 ? impl9::set_fvf(fvf) : impl8::set_fvf(fvf); }
    inline HRESULT draw_primitive(DWORD prim, DWORD count, DWORD start)
    { return version() == V9 ? impl9::draw_primitive(prim, count, start) : impl8::draw_primitive(prim, count, start); }
    inline HRESULT create_vertex_buffer(UINT size, void** vb)
    { return version() == V9 ? impl9::create_vertex_buffer(size, vb) : impl8::create_vertex_buffer(size, vb); }
    inline HRESULT upload_vertex_buffer(void* vb, const void* data, UINT size)
    { return version() == V9 ? impl9::upload_vertex_buffer(vb, data, size) : impl8::upload_vertex_buffer(vb, data, size); }
    inline HRESULT set_stream_source(DWORD stream, void* vb, DWORD stride)
    { return version() == V9 ? impl9::set_stream_source(stream, vb, stride) : impl8::set_stream_source(stream, vb, stride); }
    inline HRESULT create_vertex_declaration(const void* elems, UINT count, void** out)
    { return version() == V9 ? impl9::create_vertex_declaration(elems, count, out) : impl8::create_vertex_declaration(elems, count, out); }
    inline HRESULT set_vertex_shader_passthrough_decl(void* decl)
    { return version() == V9 ? impl9::set_vertex_shader_passthrough_decl(decl) : impl8::set_vertex_shader_passthrough_decl(decl); }

    // 错误码 → 可读文本(D3D8 用 DXGetErrorDescription8A, D3D9 用内置错误表)。
    inline std::string error_text(HRESULT hr)
    { return version() == V9 ? impl9::error_text(hr) : impl8::error_text(hr); }

    inline bool get_caps(Caps& out)
    { return version() == V9 ? impl9::get_caps(out) : impl8::get_caps(out); }
}
