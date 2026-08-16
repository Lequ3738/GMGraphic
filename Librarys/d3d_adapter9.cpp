// D3D9 后端实现(认识 d3d9.h/d3dx9.h; GMDirectX9 插件下设备指针 0x58d388 是 IDirect3DDevice9 对象)。
// 与 D3D8 差异: 声明 D3DVSD→D3DVERTEXELEMENT9, 句柄 DWORD→对象指针, 释放走对象 Release(); D3DX9 不静态链 d3dx9.lib(D3DX8/D3DX9 同名 stdcall 撞名)→LoadLibrary+GetProcAddress, D3D8 仍静态链。
#include <cstring>
#include <cstdio>
#include "d3d_adapter.h"
#include "../Direct3D_9/d3dx9.h"

// 经典 d3d9.h 里的常量, 这份(GMDirectX9 拷贝的)现代头没带, 补上。
#ifndef D3DENUM_NO_WHQL_LEVEL
#define D3DENUM_NO_WHQL_LEVEL  0x00000002
#endif

namespace d3d
{
    namespace impl9
    {
        static IDirect3DDevice9* dev()  { return (IDirect3DDevice9*)device(); }
        static IDirect3D9*       intf() { return (IDirect3D9*)iface(); }

        // ---- D3DX9 运行时解析(不静态链 d3dx9.lib, 避免与 D3DX8 撞名) ----
        // 签名直接取自 d3dx9shader.h / d3dx9tex.h。
        typedef HRESULT(WINAPI* D3DX9_ASSEMBLE_SHADER)(LPCSTR, UINT, const D3DXMACRO*, LPD3DXINCLUDE, DWORD, LPD3DXBUFFER*, LPD3DXBUFFER*);
        typedef HRESULT(WINAPI* D3DX9_COMPILE_SHADER)(LPCSTR, UINT, const D3DXMACRO*, LPD3DXINCLUDE, LPCSTR, LPCSTR, DWORD, LPD3DXBUFFER*, LPD3DXBUFFER*, LPD3DXCONSTANTTABLE*);
        typedef HRESULT(WINAPI* D3DX9_LOAD_SURFACE_FROM_MEMORY)(LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*, LPCVOID, D3DFORMAT, UINT, const PALETTEENTRY*, const RECT*, DWORD, D3DCOLOR);
        typedef HRESULT(WINAPI* D3DX9_LOAD_SURFACE_FROM_SURFACE)(LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*, LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*, DWORD, D3DCOLOR);
        static HMODULE s_d3dx9 = nullptr;
        static D3DX9_ASSEMBLE_SHADER           s_assemble  = nullptr;
        static D3DX9_COMPILE_SHADER            s_compile   = nullptr;
        static D3DX9_LOAD_SURFACE_FROM_MEMORY  s_load_mem  = nullptr;
        static D3DX9_LOAD_SURFACE_FROM_SURFACE s_load_surf = nullptr;

        static bool load_d3dx9()
        {
            if (s_d3dx9) return s_assemble && s_compile && s_load_mem && s_load_surf;
            s_d3dx9 = LoadLibraryW(L"D3DX9_43.dll");   // GMDirectX9 的 gex 已带此 DLL
            if (!s_d3dx9) return false;
            s_assemble  = (D3DX9_ASSEMBLE_SHADER)GetProcAddress(s_d3dx9, "D3DXAssembleShader");
            s_compile   = (D3DX9_COMPILE_SHADER)GetProcAddress(s_d3dx9, "D3DXCompileShader");
            s_load_mem  = (D3DX9_LOAD_SURFACE_FROM_MEMORY)GetProcAddress(s_d3dx9, "D3DXLoadSurfaceFromMemory");
            s_load_surf = (D3DX9_LOAD_SURFACE_FROM_SURFACE)GetProcAddress(s_d3dx9, "D3DXLoadSurfaceFromSurface");
            return s_assemble && s_compile && s_load_mem && s_load_surf;
        }

        // ---- 同签名转发(与 D3D8 签名逐字相同, 仅 vtable 槽位不同) ----
        HRESULT set_render_state(DWORD s, DWORD v)
        { return dev()->SetRenderState((D3DRENDERSTATETYPE)s, v); }
        HRESULT get_render_state(DWORD s, DWORD* v)
        { return dev()->GetRenderState((D3DRENDERSTATETYPE)s, v); }
        HRESULT set_tex_stage_state(DWORD stage, DWORD type, DWORD v)
        { return dev()->SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)type, v); }
        HRESULT get_tex_stage_state(DWORD stage, DWORD type, DWORD* v)
        { return dev()->GetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)type, v); }
        HRESULT get_transform(DWORD state, float* m16)
        { return dev()->GetTransform((D3DTRANSFORMSTATETYPE)state, (D3DMATRIX*)m16); }
        HRESULT draw_primitive_up(DWORD prim, DWORD count, const void* verts, DWORD stride)
        { return dev()->DrawPrimitiveUP((D3DPRIMITIVETYPE)prim, count, verts, stride); }
        UINT get_available_tex_mem() { return dev()->GetAvailableTextureMem(); }

        // ---- 汇编: D3DX9AssembleShader(7 参, 含 pInclude+Flags; 支持 1.1-3.0) ----
        // 注意是 7 参而非旧资料里的 5 参 —— d3dx9.lib 导入名 _D3DXAssembleShader@28 可证。
        static HRESULT assemble_impl(const char* src, size_t len, std::vector<BYTE>& code, std::string* err)
        {
            if (!load_d3dx9()) return E_FAIL;
            LPD3DXBUFFER shader = nullptr, errors = nullptr;
            HRESULT hr = s_assemble(src, (UINT)len, nullptr, nullptr, 0, &shader, &errors);
            if (FAILED(hr))
            {
                if (err && errors)
                    err->assign((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
                if (errors) errors->Release();
                return hr;
            }
            code.assign((BYTE*)shader->GetBufferPointer(),
                        (BYTE*)shader->GetBufferPointer() + shader->GetBufferSize());
            shader->Release();
            return S_OK;
        }

        HRESULT assemble_ps(const char* src, size_t len, std::vector<BYTE>& code, std::string* err)
        { return assemble_impl(src, len, code, err); }
        HRESULT assemble_vs(const char* src, size_t len, std::vector<BYTE>& code,
                            std::vector<BYTE>& /*constants*/, std::string* err)
        { return assemble_impl(src, len, code, err); }

        // ---- 像素着色器(句柄 = IDirect3DPixelShader9*, x86 下往返 DWORD) ----
        HRESULT create_pixel_shader(const BYTE* code, DWORD* handle)
        { return dev()->CreatePixelShader((const DWORD*)code, (IDirect3DPixelShader9**)handle); }
        // D3D9 无 DeletePixelShader: shader 是 COM 对象, 释放 = 调对象自己的 Release()。
        HRESULT delete_pixel_shader(DWORD handle)
        { if (handle) ((IDirect3DPixelShader9*)handle)->Release(); return S_OK; }
        HRESULT set_pixel_shader(DWORD handle)
        { return dev()->SetPixelShader((IDirect3DPixelShader9*)handle); }
        HRESULT get_pixel_shader(DWORD* handle)
        { return dev()->GetPixelShader((IDirect3DPixelShader9**)handle); }
        // ---- 常量寄存器写入: 按类型分发(SM3.0 的 int/bool 寄存器组, SM2.0 恒 float) ----
        HRESULT set_ps_const_typed(DWORD reg, ConstKind kind, const float* v, DWORD count)
        {
            switch (kind)
            {
            case CK_INT:
            {
                std::vector<int> iv((size_t)count * 4);   // count 个 int4 寄存器
                for (size_t i = 0; i < iv.size(); ++i) iv[i] = (int)v[i];
                return dev()->SetPixelShaderConstantI(reg, iv.data(), count);
            }
            case CK_BOOL:
            {
                std::vector<BOOL> bv((size_t)count * 4);
                for (size_t i = 0; i < bv.size(); ++i) bv[i] = (v[i] >= 0.5f) ? TRUE : FALSE;
                return dev()->SetPixelShaderConstantB(reg, bv.data(), count);
            }
            case CK_FLOAT:
            default:
                return dev()->SetPixelShaderConstantF(reg, v, count);
            }
        }

        // ---- 顶点着色器: D3D8 的 D3DVSD 声明 -> D3D9 的 D3DVERTEXELEMENT9 ----
        // 偏移与 vert_default(24B: pos|c|uv)/vert_ext(96B: pos|normal|c|s|uv) 逐字节对上; D3D9 对 vs_1_1/ps_1.x 字节码向后兼容, 现有 1.x 汇编 shader 零改写。
        static const D3DVERTEXELEMENT9 ext_elems[] = {
            { 0,  0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            { 0, 12, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
            { 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 }, // 漫反射 -> PS v0
            { 0, 28, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    1 }, // 高光   -> PS v1
            { 0, 32, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 }, // uv[0..1]  -> t0
            { 0, 36, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 }, // uv[2..3]  -> t1
            { 0, 40, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
            { 0, 44, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
            { 0, 48, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4 },
            { 0, 52, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 5 },
            { 0, 56, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 6 },
            { 0, 60, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 7 },
            D3DDECL_END()
        };
        static const D3DVERTEXELEMENT9 default_elems[] = {
            { 0,  0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
            { 0, 16, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
            D3DDECL_END()
        };

        // D3D9 顶点声明与着色器是独立对象。GMGraphic 所有顶点着色器都用同一种
        // ext 布局, 所以共享一个声明对象(惰性创建), 无需按 shader 各建一个。
        static IDirect3DVertexDeclaration9* s_decl_ext = nullptr;
        static IDirect3DVertexDeclaration9* s_decl_default = nullptr;
        static IDirect3DVertexDeclaration9* ensure_decl(VertexFmt fmt)
        {
            IDirect3DVertexDeclaration9*& d = (fmt == VERT_DEFAULT) ? s_decl_default : s_decl_ext;
            if (!d)
                dev()->CreateVertexDeclaration((fmt == VERT_DEFAULT) ? default_elems : ext_elems, &d);
            return d;
        }

        HRESULT create_vertex_shader(VertexFmt /*fmt*/, const BYTE* code, const BYTE* /*constants*/,
                                     size_t /*constants_sz*/, DWORD* handle)
        {
            return dev()->CreateVertexShader((const DWORD*)code, (IDirect3DVertexShader9**)handle);
        }
        // D3D9 无 DeleteVertexShader: 释放 = 调对象自己的 Release()。
        HRESULT delete_vertex_shader(DWORD handle)
        { if (handle) ((IDirect3DVertexShader9*)handle)->Release(); return S_OK; }
        // D3D9: FVF 与着色器拆成两个入口; 着色器模式下还需绑定顶点声明。
        HRESULT set_vertex_shader(bool fvf_mode, DWORD fvf, DWORD handle)
        {
            if (fvf_mode)
            {
                HRESULT hr = dev()->SetVertexShader(nullptr);
                if (SUCCEEDED(hr)) hr = dev()->SetVertexDeclaration(nullptr);
                if (SUCCEEDED(hr)) hr = dev()->SetFVF(fvf);
                return hr;
            }
            HRESULT hr = dev()->SetVertexDeclaration(ensure_decl(VERT_EXT));
            if (FAILED(hr)) return hr;
            return dev()->SetVertexShader((IDirect3DVertexShader9*)handle);
        }
        // ---- 仿固定管线/透传 VS(D3D9 only): 给 ps-only shader 喂 ps_3_0 的 v0(texcoord)/v1(color), FVF 只喂 ps_2.x 的 t0/v0 → ps_3_0 读 0 全透明。----
        // 必须 vs_3_0 显式 dcl_color o1/dcl_texcoord o2.xy(vs_2_0 隐式 oT0/oD0 喂不进); 矩阵必须写 mul(uWVP,v.pos)(HLSL float4x4 常量寄存器列主序 vs SetVertexShaderConstantF 写行主序, 反写丢投影平移→出屏)。
        static IDirect3DVertexShader9* s_passthrough_vs = nullptr;
        static const char s_passthrough_vs_hlsl[] =
            "float4x4 uWVP : register(c0);"                                                                 "\n"
            "struct VS_IN { float4 pos: POSITION; float4 color: COLOR0; float2 uv: TEXCOORD0; };"          "\n"
            "struct VS_OUT { float4 pos: POSITION; float4 color: COLOR0; float2 uv: TEXCOORD0; };"         "\n"
            "VS_OUT main(VS_IN v) {"                                                                       "\n"
            "  VS_OUT o;"                                                                                  "\n"
            "  o.pos = mul(uWVP, v.pos);"                                                                  "\n"
            "  o.color = v.color;"                                                                         "\n"
            "  o.uv = v.uv;"                                                                               "\n"
            "  return o;"                                                                                  "\n"
            "}";
        // out = a * b (4x4, row-major, 与 D3DXMatrixMultiply 同约定)。D3D9 分支不静态链
        // d3dx9.lib(与 D3DX8 撞名), 故手动乘, 不复用 D3DXMatrixMultiply。
        static void mul4x4(const float* a, const float* b, float* out)
        {
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                {
                    float s = 0.0f;
                    for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
                    out[r * 4 + c] = s;
                }
        }
        HRESULT set_vertex_shader_passthrough(VertexFmt fmt)
        {
            if (!s_passthrough_vs)
            {
                if (!load_d3dx9()) return E_FAIL;
                LPD3DXBUFFER shader = nullptr, errors = nullptr;
                HRESULT hr = s_compile(s_passthrough_vs_hlsl, (UINT)sizeof(s_passthrough_vs_hlsl) - 1,
                    nullptr, nullptr, "main", "vs_3_0", 0, &shader, &errors, nullptr);
                if (FAILED(hr))
                {
                    if (errors) errors->Release();
                    return hr;
                }
                hr = dev()->CreateVertexShader((const DWORD*)shader->GetBufferPointer(), &s_passthrough_vs);
                shader->Release();
                if (FAILED(hr)) return hr;
            }
            HRESULT hr = dev()->SetVertexDeclaration(ensure_decl(fmt));
            if (FAILED(hr)) return hr;
            // WVP = W*V*P(行向量约定, 与引擎 FFP 的 pos*W*V*P 一致)。uWVP 是唯一 uniform,
            // 编译器固定分配 c0-c3。GetTransform 此刻返回引擎当前矩阵(投影在帧首设好)。
            float world[16], view[16], proj[16], wv[16], wvp[16];
            dev()->GetTransform(D3DTS_WORLD, (D3DMATRIX*)world);
            dev()->GetTransform(D3DTS_VIEW, (D3DMATRIX*)view);
            dev()->GetTransform(D3DTS_PROJECTION, (D3DMATRIX*)proj);
            mul4x4(world, view, wv);
            mul4x4(wv, proj, wvp);
            hr = dev()->SetVertexShaderConstantF(0, wvp, 4);
            if (FAILED(hr)) return hr;
            return dev()->SetVertexShader(s_passthrough_vs);
        }
        // [GM80-2026-08-09] 透传 VS 槽地址: 返回 &s_passthrough_vs(变量地址, 非值)。注册到
        // GMDirectX9 后, 其 SetVertexShader 钩子读此槽识别"当前绑的是透传 VS" → 刷新 c0-c3 WVP
        // 到当前投影(surface_set_target 重设投影后不失真)。懒创建: 首次 shader_set ps-only 才非空,
        // 槽始终有效(存的是变量地址, 值创建后自动可见)。
        void* get_passthrough_vs_ptr() { return &s_passthrough_vs; }

        // ---- vertex_* vertex-buffer pipeline (D3D9 only) ----
        // vertex_submit needs: custom decl + arbitrary VS (or passthrough VS) + save/restore
        // of the engine's VS / decl / FVF.
        HRESULT set_vertex_declaration(void* decl)
        { return dev()->SetVertexDeclaration((IDirect3DVertexDeclaration9*)decl); }
        HRESULT get_vertex_declaration(void** decl)
        { return dev()->GetVertexDeclaration((IDirect3DVertexDeclaration9**)decl); }
        HRESULT get_vertex_shader(DWORD* handle)
        { return dev()->GetVertexShader((IDirect3DVertexShader9**)handle); }
        HRESULT set_vertex_shader_handle(DWORD handle)
        { return dev()->SetVertexShader((IDirect3DVertexShader9*)handle); }
        HRESULT get_fvf(DWORD* fvf) { return dev()->GetFVF(fvf); }
        HRESULT set_fvf(DWORD fvf)  { return dev()->SetFVF(fvf); }
        HRESULT draw_primitive(DWORD prim, DWORD count, DWORD start)
        { return dev()->DrawPrimitive((D3DPRIMITIVETYPE)prim, start, count); }
        // Static read-only VB (freeze): D3DUSAGE_WRITEONLY + default pool, uploaded once.
        HRESULT create_vertex_buffer(UINT size, void** vb)
        {
            return dev()->CreateVertexBuffer(size, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
                (IDirect3DVertexBuffer9**)vb, nullptr);
        }
        HRESULT upload_vertex_buffer(void* vb, const void* data, UINT size)
        {
            void* p = nullptr;
            HRESULT hr = ((IDirect3DVertexBuffer9*)vb)->Lock(0, size, &p, D3DLOCK_DISCARD);
            if (FAILED(hr)) return hr;
            memcpy(p, data, size);
            return ((IDirect3DVertexBuffer9*)vb)->Unlock();
        }
        HRESULT set_stream_source(DWORD stream, void* vb, DWORD stride)
        { return dev()->SetStreamSource(stream, (IDirect3DVertexBuffer9*)vb, 0, stride); }
        // elems = D3DVERTEXELEMENT9-layout array (see vertex.h); count excludes D3DDECL_END().
        HRESULT create_vertex_declaration(const void* elems, UINT count, void** out)
        {
            return dev()->CreateVertexDeclaration((const D3DVERTEXELEMENT9*)elems,
                (IDirect3DVertexDeclaration9**)out);
        }
        // Passthrough VS onto a custom decl (vertex_submit with no VS): reuse s_passthrough_vs
        // (lazy), only swap the decl and refresh WVP. Same shape as set_vertex_shader_passthrough.
        HRESULT set_vertex_shader_passthrough_decl(void* decl)
        {
            if (!s_passthrough_vs)
            {
                if (!load_d3dx9()) return E_FAIL;
                LPD3DXBUFFER shader = nullptr, errors = nullptr;
                HRESULT hr = s_compile(s_passthrough_vs_hlsl, (UINT)sizeof(s_passthrough_vs_hlsl) - 1,
                    nullptr, nullptr, "main", "vs_3_0", 0, &shader, &errors, nullptr);
                if (FAILED(hr))
                {
                    if (errors) errors->Release();
                    return hr;
                }
                hr = dev()->CreateVertexShader((const DWORD*)shader->GetBufferPointer(), &s_passthrough_vs);
                shader->Release();
                if (FAILED(hr)) return hr;
            }
            HRESULT hr = dev()->SetVertexDeclaration((IDirect3DVertexDeclaration9*)decl);
            if (FAILED(hr)) return hr;
            float world[16], view[16], proj[16], wv[16], wvp[16];
            dev()->GetTransform(D3DTS_WORLD, (D3DMATRIX*)world);
            dev()->GetTransform(D3DTS_VIEW, (D3DMATRIX*)view);
            dev()->GetTransform(D3DTS_PROJECTION, (D3DMATRIX*)proj);
            mul4x4(world, view, wv);
            mul4x4(wv, proj, wvp);
            hr = dev()->SetVertexShaderConstantF(0, wvp, 4);
            if (FAILED(hr)) return hr;
            return dev()->SetVertexShader(s_passthrough_vs);
        }

        // ---- render-target bridge (gpart evolution pass) ----
        HRESULT get_surface_level(void* tex, UINT level, void** surface)
        { return ((IDirect3DTexture9*)tex)->GetSurfaceLevel(level, (IDirect3DSurface9**)surface); }
        HRESULT get_render_target(DWORD index, void** surface)
        { return dev()->GetRenderTarget(index, (IDirect3DSurface9**)surface); }
        HRESULT set_render_target(DWORD index, void* surface)
        { return dev()->SetRenderTarget(index, (IDirect3DSurface9*)surface); }
        HRESULT clear_target(DWORD color)
        { return dev()->Clear(0, nullptr, D3DCLEAR_TARGET, color, 0.0f, 0); }
        HRESULT set_viewport(UINT w, UINT h)
        {
            D3DVIEWPORT9 vp = { 0, 0, w, h, 0.0f, 1.0f };
            return dev()->SetViewport(&vp);
        }
        HRESULT get_viewport(UINT* w, UINT* h)
        {
            D3DVIEWPORT9 vp{};
            HRESULT hr = dev()->GetViewport(&vp);
            if (SUCCEEDED(hr)) { *w = vp.Width; *h = vp.Height; }
            return hr;
        }
        HRESULT set_vs_const_typed(DWORD reg, ConstKind kind, const float* v, DWORD count)
        {
            switch (kind)
            {
            case CK_INT:
            {
                std::vector<int> iv((size_t)count * 4);
                for (size_t i = 0; i < iv.size(); ++i) iv[i] = (int)v[i];
                return dev()->SetVertexShaderConstantI(reg, iv.data(), count);
            }
            case CK_BOOL:
            {
                std::vector<BOOL> bv((size_t)count * 4);
                for (size_t i = 0; i < bv.size(); ++i) bv[i] = (v[i] >= 0.5f) ? TRUE : FALSE;
                return dev()->SetVertexShaderConstantB(reg, bv.data(), count);
            }
            case CK_FLOAT:
            default:
                return dev()->SetVertexShaderConstantF(reg, v, count);
            }
        }

        // ---- HLSL(D3D9 专属): D3DXCompileShader + ID3DXConstantTable ----
        // 编译单文件单入口; 常量表随第 10 个出参返回(HLSL 专属, asm 走 D3DXAssembleShader)。
        HRESULT compile_hlsl(const char* src, size_t len, const char* entry, const char* profile,
                             std::vector<BYTE>& code, void** table, std::string* err)
        {
            if (!load_d3dx9()) return E_FAIL;
            LPD3DXBUFFER shader = nullptr, errors = nullptr;
            ID3DXConstantTable* ct = nullptr;
            HRESULT hr = s_compile(src, (UINT)len, nullptr, nullptr, entry, profile, 0,
                                   &shader, &errors, &ct);
            if (FAILED(hr))
            {
                if (err && errors)
                    err->assign((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
                if (errors) errors->Release();
                return hr;
            }
            code.assign((BYTE*)shader->GetBufferPointer(),
                        (BYTE*)shader->GetBufferPointer() + shader->GetBufferSize());
            shader->Release();
            if (table) *table = ct;
            return S_OK;
        }

        HRESULT constant_table_set_defaults(void* table)
        {
            if (!table) return E_INVALIDARG;
            return ((ID3DXConstantTable*)table)->SetDefaults(dev());
        }

        void* constant_table_get_constant_by_name(void* table, const char* name)
        {
            if (!table || !name) return nullptr;
            return (void*)((ID3DXConstantTable*)table)->GetConstantByName(nullptr, name);
        }

        UniformLoc constant_table_get_uniform(void* table, void* handle)
        {
            UniformLoc loc;
            if (!table || !handle) return loc;
            D3DXCONSTANT_DESC desc{};
            UINT count = 1;
            if (FAILED(((ID3DXConstantTable*)table)->GetConstantDesc((D3DXHANDLE)handle, &desc, &count)))
                return loc;
            loc.reg = (int)desc.RegisterIndex;
            loc.count = (int)desc.RegisterCount;
            // 寄存器组决定写入路径: SM3.0 int→INT4/bool→BOOL, SM2.0 全在 FLOAT4。
            switch (desc.RegisterSet)
            {
            case D3DXRS_INT4:   loc.kind = CK_INT;   break;
            case D3DXRS_BOOL:   loc.kind = CK_BOOL;  break;
            case D3DXRS_FLOAT4:
            default:            loc.kind = CK_FLOAT; break;
            }
            return loc;
        }

        int constant_table_get_sampler_register(void* table, void* handle)
        {
            if (!table || !handle) return -1;
            D3DXCONSTANT_DESC desc{};
            UINT count = 1;
            if (FAILED(((ID3DXConstantTable*)table)->GetConstantDesc((D3DXHANDLE)handle, &desc, &count)))
                return -1;
            if (desc.Class != D3DXPC_OBJECT || desc.Type != D3DXPT_SAMPLER)
                return -1;
            return (int)desc.RegisterIndex;
        }

        // ---- 纹理桥 ----
        HRESULT set_texture(DWORD stage, void* tex)
        { return dev()->SetTexture(stage, (IDirect3DBaseTexture9*)tex); }
        HRESULT get_texture(DWORD stage, void** tex)
        { return dev()->GetTexture(stage, (IDirect3DBaseTexture9**)tex); }
        HRESULT create_texture(UINT w, UINT h, UINT levels, DWORD usage, DWORD fmt, DWORD pool, void** out)
        {
            IDirect3DTexture9* tex = nullptr;
            // 现代头 CreateTexture 8 参(多共享句柄), 传 nullptr 即"不共享"的经典行为。
            HRESULT hr = dev()->CreateTexture(w, h, levels, usage, (D3DFORMAT)fmt, (D3DPOOL)pool, &tex, nullptr);
            if (SUCCEEDED(hr)) *out = tex;
            return hr;
        }
        HRESULT upload_texture(void* tex_, UINT w, UINT h, DWORD fmt, const void* px, UINT pitch)
        {
            if (!load_d3dx9()) return E_FAIL;
            IDirect3DTexture9* tex = (IDirect3DTexture9*)tex_;
            IDirect3DSurface9* surf = nullptr;
            HRESULT hr = tex->GetSurfaceLevel(0, &surf);
            if (FAILED(hr)) return hr;
            RECT r = { 0, 0, (LONG)w, (LONG)h };
            hr = s_load_mem(surf, nullptr, &r, px, (D3DFORMAT)fmt, pitch,
                            nullptr, &r, D3DX_FILTER_NONE, 0);
            if (SUCCEEDED(hr)) tex->AddDirtyRect(&r);   // DEFAULT 池其实不必要, 但无害
            surf->Release();
            return hr;
        }
        void release(void* com)
        { if (com) ((IUnknown*)com)->Release(); }
        HRESULT read_texture(void* tex_, std::vector<BYTE>& dest, UINT& width, UINT& height)
        {
            if (!load_d3dx9()) return E_FAIL;
            IDirect3DTexture9*   tex = (IDirect3DTexture9*)tex_;
            IDirect3DSurface9 *surface = nullptr, *surface_mem = nullptr;
            D3DSURFACE_DESC desc{};
            HRESULT hr = tex->GetLevelDesc(0, &desc);
            if (SUCCEEDED(hr))
            {
                width = desc.Width; height = desc.Height;
                hr = tex->GetSurfaceLevel(0, &surface);
            }
            if (SUCCEEDED(hr))
                // D3D8 的 CreateImageSurface 在 D3D9 等价物是 CreateOffscreenPlainSurface(SYSTEMMEM, 可锁定)
                hr = dev()->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surface_mem, nullptr);
            if (SUCCEEDED(hr))
                hr = s_load_surf(surface_mem, nullptr, nullptr, surface,
                                 nullptr, nullptr, D3DX_FILTER_NONE, 0);
            if (SUCCEEDED(hr))
            {
                D3DLOCKED_RECT lock{};
                hr = surface_mem->LockRect(&lock, nullptr, 0);
                if (SUCCEEDED(hr))
                {
                    dest.resize((size_t)width * height * 4);
                    BYTE* src = (BYTE*)lock.pBits;
                    size_t sp = 0, dp = 0, stride = (size_t)width * 4;
                    for (UINT i = 0; i < height; ++i)
                    {
                        memcpy(dest.data() + dp, src + sp, stride);
                        sp += lock.Pitch; dp += stride;
                    }
                    surface_mem->UnlockRect();
                }
            }
            if (surface_mem) surface_mem->Release();
            if (surface) surface->Release();
            return hr;
        }

        // ---- 错误文本(D3D9 精简表: 覆盖 GMGraphic 实际会遇到的错误码) ----
        // 不引入 DX SDK 的 dxerr.cpp(3967 行, 依赖 d3d10/11 等头), 用 switch 表即可。
        std::string error_text(HRESULT hr)
        {
            switch (hr)
            {
            case D3D_OK:                                return "D3D_OK";
            case D3DERR_DEVICELOST:                     return "D3DERR_DEVICELOST";
            case D3DERR_DEVICENOTRESET:                 return "D3DERR_DEVICENOTRESET";
            case D3DERR_DEVICEREMOVED:                  return "D3DERR_DEVICEREMOVED";
            case D3DERR_DRIVERINTERNALERROR:            return "D3DERR_DRIVERINTERNALERROR";
            case D3DERR_INVALIDCALL:                    return "D3DERR_INVALIDCALL";
            case D3DERR_NOTAVAILABLE:                   return "D3DERR_NOTAVAILABLE";
            case D3DERR_NOTFOUND:                       return "D3DERR_NOTFOUND";
            case D3DERR_OUTOFVIDEOMEMORY:               return "D3DERR_OUTOFVIDEOMEMORY";
            case D3DERR_WASSTILLDRAWING:                return "D3DERR_WASSTILLDRAWING";
            case E_FAIL:                                return "E_FAIL";
            case E_INVALIDARG:                          return "E_INVALIDARG";
            case E_OUTOFMEMORY:                         return "E_OUTOFMEMORY";
            case E_NOTIMPL:                             return "E_NOTIMPL";
            default:
                char buf[32];
                sprintf(buf, "0x%08X", (unsigned)hr);
                return buf;
            }
        }

        // ---- 设备能力 ----
        bool get_caps(Caps& out)
        {
            D3DCAPS9 caps{};
            D3DADAPTER_IDENTIFIER9 aid{};
            if (FAILED(dev()->GetDeviceCaps(&caps))) return false;
            if (FAILED(intf()->GetAdapterIdentifier(D3DADAPTER_DEFAULT, D3DENUM_NO_WHQL_LEVEL, &aid))) return false;
            float max_ps = 0.0f;
            memcpy(&max_ps, &caps.MaxPointSize, sizeof(max_ps));   // D3D9 是 DWORD(同 bit)
            out.max_point_size       = max_ps;
            out.pixel_shader_version = caps.PixelShaderVersion;
            out.vertex_shader_version = caps.VertexShaderVersion;
            out.max_tex_w            = caps.MaxTextureWidth;
            out.max_tex_h            = caps.MaxTextureHeight;
            out.max_tex_stages       = caps.MaxSimultaneousTextures;
            out.max_aniso            = caps.MaxAnisotropy;
            out.prim_misc_caps       = caps.PrimitiveMiscCaps;
            out.raster_caps          = caps.RasterCaps;
            out.vertex_tex_filter_caps = caps.VertexTextureFilterCaps;
            strncpy(out.adapter_desc, aid.Description, sizeof(out.adapter_desc) - 1);
            out.adapter_desc[sizeof(out.adapter_desc) - 1] = 0;
            return true;
        }
    }
}
