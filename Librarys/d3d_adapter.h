#pragma once
// ============================================================================
// GMGraphic 双后端适配器 —— 版本中立接口(D3D8 / D3D9 运行时自动分发)
//
// 规则: 本头只出现 DWORD / UINT / 指针 / float / std 类型, 绝不出现
//       IDirect3DDevice8/9、D3DFORMAT 等 D3D 类型。这样共享代码无需认识
//       任何一个具体版本的接口, 也就没有 d3d8.h 与 d3d9.h 的头冲突。
//
// 实现分两个 TU:
//   d3d_adapter8.cpp  仅 include d3d8.h + d3dx8.h  (D3D8 分支)
//   d3d_adapter9.cpp  仅 include d3d9.h + d3dx9.h  (D3D9 分支)
// 两者永不同时进入同一编译单元, 头冲突从根上消除。公共 API 按运行时的
// 后端选择(见 version()/ensure_version())。未初始化时默认按 D3D8(历史行为)。
//
// 为什么共享代码沿用 D3D8 枚举: gmapi.h 经 GmapiDefs.h 在 GMAPI_USE_D3D 下
// 已引入 d3d8.h; 且 D3DFMT_/D3DPOOL_/D3DRS_/D3DTSS_/D3DPT_/D3DFVF_ 等枚举值
// 在 DX8 与 DX9 中数值完全一致。共享代码把 D3D8 枚举值当 DWORD 传入,
// 适配器按后端转回对应版本的类型。此前提由 GMDirectX9 移植实机验证过。
// ============================================================================
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
        DWORD max_tex_w, max_tex_h, max_tex_stages, max_aniso;
        DWORD prim_misc_caps, raster_caps;
        char  adapter_desc[512];
    };

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
        HRESULT set_ps_const(DWORD, const float*, DWORD);

        HRESULT create_vertex_shader(VertexFmt, const BYTE*, const BYTE*, size_t, DWORD*);
        HRESULT delete_vertex_shader(DWORD);
        HRESULT set_vertex_shader(bool fvf_mode, DWORD fvf, DWORD handle);
        HRESULT set_vs_const(DWORD, const float*, DWORD);

        HRESULT set_texture(DWORD, void*);
        HRESULT create_texture(UINT, UINT, UINT, DWORD, DWORD, DWORD, void**);
        HRESULT upload_texture(void*, UINT, UINT, DWORD, const void*, UINT);
        void    release(void*);
        HRESULT read_texture(void*, std::vector<BYTE>&, UINT&, UINT&);

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
        HRESULT set_ps_const(DWORD, const float*, DWORD);

        HRESULT create_vertex_shader(VertexFmt, const BYTE*, const BYTE*, size_t, DWORD*);
        HRESULT delete_vertex_shader(DWORD);
        HRESULT set_vertex_shader(bool fvf_mode, DWORD fvf, DWORD handle);
        HRESULT set_vs_const(DWORD, const float*, DWORD);

        HRESULT set_texture(DWORD, void*);
        HRESULT create_texture(UINT, UINT, UINT, DWORD, DWORD, DWORD, void**);
        HRESULT upload_texture(void*, UINT, UINT, DWORD, const void*, UINT);
        void    release(void*);
        HRESULT read_texture(void*, std::vector<BYTE>&, UINT&, UINT&);

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
    inline HRESULT set_ps_const(DWORD reg, const float* v, DWORD count)
    { return version() == V9 ? impl9::set_ps_const(reg, v, count) : impl8::set_ps_const(reg, v, count); }

    inline HRESULT create_vertex_shader(VertexFmt fmt, const BYTE* code, const BYTE* constants, size_t constants_sz, DWORD* handle)
    { return version() == V9 ? impl9::create_vertex_shader(fmt, code, constants, constants_sz, handle) : impl8::create_vertex_shader(fmt, code, constants, constants_sz, handle); }
    inline HRESULT delete_vertex_shader(DWORD handle)
    { return version() == V9 ? impl9::delete_vertex_shader(handle) : impl8::delete_vertex_shader(handle); }
    inline HRESULT set_vertex_shader(bool fvf_mode, DWORD fvf, DWORD handle)
    { return version() == V9 ? impl9::set_vertex_shader(fvf_mode, fvf, handle) : impl8::set_vertex_shader(fvf_mode, fvf, handle); }
    inline HRESULT set_vs_const(DWORD reg, const float* v, DWORD count)
    { return version() == V9 ? impl9::set_vs_const(reg, v, count) : impl8::set_vs_const(reg, v, count); }

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

    // 错误码 → 可读文本(D3D8 用 DXGetErrorDescription8A, D3D9 用内置错误表)。
    inline std::string error_text(HRESULT hr)
    { return version() == V9 ? impl9::error_text(hr) : impl8::error_text(hr); }

    inline bool get_caps(Caps& out)
    { return version() == V9 ? impl9::get_caps(out) : impl8::get_caps(out); }
}
