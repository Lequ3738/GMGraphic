// D3D9 后端实现。本 TU 认识 d3d9.h / d3dx9.h(现代 Windows SDK 的 D3D9Ex 合并头)。
// 在 GMDirectX9 插件下, 设备指针(0x58d388)是 IDirect3DDevice9 对象。
//
// 与 D3D8 的差异集中体现在本文件:
//   1. 顶点着色器声明: D3D8 用 D3DVSD 记号流, D3D9 用 D3DVERTEXELEMENT9
//   2. 着色器句柄: D3D8 是 DWORD, D3D9 是对象指针(x86 下互转安全)
//   3. 着色器释放: D3D8 用设备 Delete*Shader(handle); D3D9 无此方法,
//      shader 是 COM 对象, 直接对对象调 Release()(引用计数到 0 自动释放)
//   4. 常量寄存器: Set*ShaderConstant -> Set*ShaderConstantF
//   5. 创建方法的共享句柄参数: 现代头 CreateTexture / CreateOffscreenPlainSurface
//      是 8 参(多 HANDLE* pSharedHandle), 传 nullptr 即"不共享"的经典行为
//
// 关于 D3DX9 为什么【不能】像 D3D8 那样静态链 d3dx9.lib:
//   D3DX8 与 D3DX9 有大量同名函数(如 D3DXLoadSurfaceFromMemory, 都是 @40),
//   stdcall 修饰名相同, 静态链接必然撞名, 导致两个分支的 D3DX 调用都解析错误。
//   因此 D3DX9 走运行时 LoadLibrary + GetProcAddress(与 GMDirectX9 自身一致);
//   D3D8 分支保持静态链 d3dx8.lib(原样)。d3d9.h / d3dx9.h 仍完整引入供类型使用。
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
        typedef HRESULT(WINAPI* D3DX9_LOAD_SURFACE_FROM_MEMORY)(LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*, LPCVOID, D3DFORMAT, UINT, const PALETTEENTRY*, const RECT*, DWORD, D3DCOLOR);
        typedef HRESULT(WINAPI* D3DX9_LOAD_SURFACE_FROM_SURFACE)(LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*, LPDIRECT3DSURFACE9, const PALETTEENTRY*, const RECT*, DWORD, D3DCOLOR);
        static HMODULE s_d3dx9 = nullptr;
        static D3DX9_ASSEMBLE_SHADER           s_assemble  = nullptr;
        static D3DX9_LOAD_SURFACE_FROM_MEMORY  s_load_mem  = nullptr;
        static D3DX9_LOAD_SURFACE_FROM_SURFACE s_load_surf = nullptr;

        static bool load_d3dx9()
        {
            if (s_d3dx9) return s_assemble && s_load_mem && s_load_surf;
            s_d3dx9 = LoadLibraryW(L"D3DX9_43.dll");   // GMDirectX9 的 gex 已带此 DLL
            if (!s_d3dx9) return false;
            s_assemble  = (D3DX9_ASSEMBLE_SHADER)GetProcAddress(s_d3dx9, "D3DXAssembleShader");
            s_load_mem  = (D3DX9_LOAD_SURFACE_FROM_MEMORY)GetProcAddress(s_d3dx9, "D3DXLoadSurfaceFromMemory");
            s_load_surf = (D3DX9_LOAD_SURFACE_FROM_SURFACE)GetProcAddress(s_d3dx9, "D3DXLoadSurfaceFromSurface");
            return s_assemble && s_load_mem && s_load_surf;
        }

        // ---- 同签名转发(与 D3D8 签名逐字相同, 仅 vtable 槽位不同) ----
        HRESULT set_render_state(DWORD s, DWORD v)
        { return dev()->SetRenderState((D3DRENDERSTATETYPE)s, v); }
        HRESULT set_tex_stage_state(DWORD stage, DWORD type, DWORD v)
        { return dev()->SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)type, v); }
        HRESULT get_transform(DWORD state, float* m16)
        { return dev()->GetTransform((D3DTRANSFORMSTATETYPE)state, (D3DMATRIX*)m16); }
        HRESULT draw_primitive_up(DWORD prim, DWORD count, const void* verts, DWORD stride)
        { return dev()->DrawPrimitiveUP((D3DPRIMITIVETYPE)prim, count, verts, stride); }
        UINT get_available_tex_mem() { return dev()->GetAvailableTextureMem(); }

        // ---- 汇编: D3DX9AssembleShader(7 参; 支持 1.1-3.0) ----
        // 注意: d3dx9_43.dll 的 D3DXAssembleShader 是 7 参(含 pInclude + Flags),
        // 不是某些旧资料里的 5 参 —— d3dx9.lib 导入名 _D3DXAssembleShader@28 可证。
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
        HRESULT set_ps_const(DWORD reg, const float* v, DWORD count)
        { return dev()->SetPixelShaderConstantF(reg, v, count); }

        // ---- 顶点着色器: D3D8 的 D3DVSD 声明 -> D3D9 的 D3DVERTEXELEMENT9 ----
        // 偏移必须与 vert_default(24B)/vert_ext(96B) 逐字节对上。
        //   vert_ext: pos[0..11] | normal[12..23] | c[24] | s[28] | uv[32..95]
        //   vert_default: pos[0..11] | c[12] | uv[16..23]
        // D3D9 对 vs_1_1 / ps_1.x 字节码向后兼容, 现有 1.x 汇编 shader 零改写。
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
                HRESULT hr = dev()->SetFVF(fvf);
                if (SUCCEEDED(hr)) dev()->SetVertexDeclaration(nullptr);
                return hr;
            }
            HRESULT hr = dev()->SetVertexDeclaration(ensure_decl(VERT_EXT));
            if (FAILED(hr)) return hr;
            return dev()->SetVertexShader((IDirect3DVertexShader9*)handle);
        }
        HRESULT set_vs_const(DWORD reg, const float* v, DWORD count)
        { return dev()->SetVertexShaderConstantF(reg, v, count); }

        // ---- 纹理桥 ----
        HRESULT set_texture(DWORD stage, void* tex)
        { return dev()->SetTexture(stage, (IDirect3DBaseTexture9*)tex); }
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
            out.max_tex_w            = caps.MaxTextureWidth;
            out.max_tex_h            = caps.MaxTextureHeight;
            out.max_tex_stages       = caps.MaxSimultaneousTextures;
            out.max_aniso            = caps.MaxAnisotropy;
            out.prim_misc_caps       = caps.PrimitiveMiscCaps;
            out.raster_caps          = caps.RasterCaps;
            strncpy(out.adapter_desc, aid.Description, sizeof(out.adapter_desc) - 1);
            out.adapter_desc[sizeof(out.adapter_desc) - 1] = 0;
            return true;
        }
    }
}
