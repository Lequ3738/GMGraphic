// D3D8 后端实现。本 TU 只认识 d3d8.h / d3dx8.h。
// 所有函数签名与 d3d_adapter.h 的中性声明一一对应。
#include <cstring>
#include <algorithm>
#include "d3d_adapter.h"
#include "d3dx8.h"
#include "dxerr8.h"

namespace d3d
{
    namespace impl8
    {
        static IDirect3DDevice8* dev()  { return (IDirect3DDevice8*)device(); }
        static IDirect3D8*       intf() { return (IDirect3D8*)iface(); }

        // ---- 同签名转发(与 D3D9 签名逐字相同, 仅 vtable 槽位不同) ----
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

        // ---- 汇编: D3DX8AssembleShader(带 ppConstants, 仅顶点着色器用到) ----
        static HRESULT assemble_impl(const char* src, size_t len, LPD3DXBUFFER* out_const,
                                     std::vector<BYTE>& code, std::string* err)
        {
            LPD3DXBUFFER shader = nullptr, errors = nullptr;
            HRESULT hr = D3DXAssembleShader((LPCVOID)src, (UINT)len, 0, out_const, &shader, &errors);
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
        { return assemble_impl(src, len, nullptr, code, err); }

        HRESULT assemble_vs(const char* src, size_t len, std::vector<BYTE>& code,
                            std::vector<BYTE>& constants, std::string* err)
        {
            LPD3DXBUFFER cbuf = nullptr;
            HRESULT hr = assemble_impl(src, len, &cbuf, code, err);
            if (FAILED(hr)) return hr;
            if (cbuf)
            {
                constants.assign((BYTE*)cbuf->GetBufferPointer(),
                                 (BYTE*)cbuf->GetBufferPointer() + cbuf->GetBufferSize());
                cbuf->Release();
            }
            return S_OK;
        }

        // ---- 像素着色器(D3D8 句柄就是 DWORD) ----
        HRESULT create_pixel_shader(const BYTE* code, DWORD* handle)
        { return dev()->CreatePixelShader((const DWORD*)code, handle); }
        HRESULT delete_pixel_shader(DWORD handle) { return dev()->DeletePixelShader(handle); }
        HRESULT set_pixel_shader(DWORD handle)    { return dev()->SetPixelShader(handle); }
        HRESULT get_pixel_shader(DWORD* handle)   { return dev()->GetPixelShader(handle); }
        // D3D8 无独立 int/bool 寄存器(SM1.x), 所有常量都是 4 分量 float 寄存器。
        HRESULT set_ps_const_typed(DWORD reg, ConstKind /*kind*/, const float* v, DWORD count)
        { return dev()->SetPixelShaderConstant(reg, (const DWORD*)v, count); }

        // ---- 顶点着色器: 声明 = [D3DX 常量表][D3DVSD 流] ----
        // 与旧 shader.cpp 逐字节一致: 常量表区固定 96*5 DWORD, D3DVSD 从其后开始。
        static void build_vsd_decl(VertexFmt fmt, DWORD* d)
        {
            d[0] = D3DVSD_STREAM(0);
            if (fmt == VERT_EXT)
            {
                d[1]  = D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3);
                d[2]  = D3DVSD_REG(D3DVSDE_NORMAL, D3DVSDT_FLOAT3);
                d[3]  = D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR);
                d[4]  = D3DVSD_REG(D3DVSDE_SPECULAR, D3DVSDT_D3DCOLOR);
                d[5]  = D3DVSD_REG(D3DVSDE_TEXCOORD0, D3DVSDT_FLOAT2);
                d[6]  = D3DVSD_REG(D3DVSDE_TEXCOORD1, D3DVSDT_FLOAT2);
                d[7]  = D3DVSD_REG(D3DVSDE_TEXCOORD2, D3DVSDT_FLOAT2);
                d[8]  = D3DVSD_REG(D3DVSDE_TEXCOORD3, D3DVSDT_FLOAT2);
                d[9]  = D3DVSD_REG(D3DVSDE_TEXCOORD4, D3DVSDT_FLOAT2);
                d[10] = D3DVSD_REG(D3DVSDE_TEXCOORD5, D3DVSDT_FLOAT2);
                d[11] = D3DVSD_REG(D3DVSDE_TEXCOORD6, D3DVSDT_FLOAT2);
                d[12] = D3DVSD_REG(D3DVSDE_TEXCOORD7, D3DVSDT_FLOAT2);
                d[13] = D3DVSD_END();
            }
            else
            {
                d[1] = D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3);
                d[2] = D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR);
                d[3] = D3DVSD_REG(D3DVSDE_TEXCOORD0, D3DVSDT_FLOAT2);
                d[4] = D3DVSD_END();
            }
        }

        HRESULT create_vertex_shader(VertexFmt fmt, const BYTE* code, const BYTE* constants,
                                     size_t constants_sz, DWORD* handle)
        {
            const size_t o = 96 * 5;   // 96 sets of [d3dvsd_const + 4 floats]
            DWORD vsdec[o + 32]{};
            SecureZeroMemory(vsdec, sizeof(vsdec));
            if (constants && constants_sz)
                memcpy(vsdec, constants, std::min(constants_sz, o * sizeof(DWORD)));
            build_vsd_decl(fmt, &vsdec[o]);
            return dev()->CreateVertexShader(vsdec, (const DWORD*)code, handle,
                                             D3DUSAGE_SOFTWAREPROCESSING);
        }
        HRESULT delete_vertex_shader(DWORD handle) { return dev()->DeleteVertexShader(handle); }
        // D3D8: FVF 与着色器是同一个入口, 用哪个由调用方语义决定。
        HRESULT set_vertex_shader(bool fvf_mode, DWORD fvf, DWORD handle)
        { return dev()->SetVertexShader(fvf_mode ? fvf : handle); }
        // D3D8 无 ps_3_0 输入路由问题(最高 ps_1.4 走 tN/v0), 无自定义 VS 就保持固定顶点管线。
        HRESULT set_vertex_shader_passthrough(VertexFmt fmt)
        {
            DWORD fvf = (fmt == VERT_EXT)
                ? (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX8)
                : (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            return set_vertex_shader(true, fvf, 0);
        }
        HRESULT set_vs_const_typed(DWORD reg, ConstKind /*kind*/, const float* v, DWORD count)
        { return dev()->SetVertexShaderConstant(reg, (const DWORD*)v, count); }

        // ---- HLSL(D3D8 不支持, 桩) ----
        HRESULT compile_hlsl(const char*, size_t, const char*, const char*,
                             std::vector<BYTE>&, void**, std::string*) { return E_NOTIMPL; }
        HRESULT constant_table_set_defaults(void*) { return E_NOTIMPL; }
        void*   constant_table_get_constant_by_name(void*, const char*) { return nullptr; }
        UniformLoc constant_table_get_uniform(void*, void*) { return UniformLoc{}; }
        int     constant_table_get_sampler_register(void*, void*) { return -1; }

        // ---- 纹理桥(纹理一律不透明 void*) ----
        HRESULT set_texture(DWORD stage, void* tex)
        { return dev()->SetTexture(stage, (IDirect3DBaseTexture8*)tex); }
        HRESULT get_texture(DWORD stage, void** tex)
        { return dev()->GetTexture(stage, (IDirect3DBaseTexture8**)tex); }
        HRESULT upload_texture_rect(void*, UINT, UINT, UINT, UINT, DWORD, const void*, UINT)
        { return E_FAIL; }
        HRESULT create_texture(UINT w, UINT h, UINT levels, DWORD usage, DWORD fmt, DWORD pool, void** out)
        {
            IDirect3DTexture8* tex = nullptr;
            HRESULT hr = dev()->CreateTexture(w, h, levels, usage, (D3DFORMAT)fmt, (D3DPOOL)pool, &tex);
            if (SUCCEEDED(hr)) *out = tex;
            return hr;
        }
        HRESULT upload_texture(void* tex_, UINT w, UINT h, DWORD fmt, const void* px, UINT pitch)
        {
            IDirect3DTexture8* tex = (IDirect3DTexture8*)tex_;
            IDirect3DSurface8* surf = nullptr;
            HRESULT hr = tex->GetSurfaceLevel(0, &surf);
            if (FAILED(hr)) return hr;
            RECT r = { 0, 0, (LONG)w, (LONG)h };
            hr = D3DXLoadSurfaceFromMemory(surf, nullptr, &r, px, (D3DFORMAT)fmt, pitch,
                                           nullptr, &r, D3DX_FILTER_NONE, 0);
            if (SUCCEEDED(hr)) tex->AddDirtyRect(&r);
            surf->Release();
            return hr;
        }
        void release(void* com)
        { if (com) ((IUnknown*)com)->Release(); }
        HRESULT read_texture(void* tex_, std::vector<BYTE>& dest, UINT& width, UINT& height)
        {
            IDirect3DTexture8*   tex = (IDirect3DTexture8*)tex_;
            IDirect3DSurface8 *surface = nullptr, *surface_mem = nullptr;
            D3DSURFACE_DESC desc{};
            HRESULT hr = tex->GetLevelDesc(0, &desc);
            if (SUCCEEDED(hr))
            {
                width = desc.Width; height = desc.Height;
                hr = tex->GetSurfaceLevel(0, &surface);
            }
            if (SUCCEEDED(hr))
                hr = dev()->CreateImageSurface(width, height, D3DFMT_A8R8G8B8, &surface_mem);
            if (SUCCEEDED(hr))
                hr = D3DXLoadSurfaceFromSurface(surface_mem, nullptr, nullptr, surface,
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

        // ---- vertex_* vertex-buffer pipeline (D3D9 only, D3D8 stubs return E_FAIL) ----
        HRESULT set_vertex_declaration(void*) { return E_FAIL; }
        HRESULT get_vertex_declaration(void**) { return E_FAIL; }
        HRESULT get_vertex_shader(DWORD*) { return E_FAIL; }
        HRESULT set_vertex_shader_handle(DWORD) { return E_FAIL; }
        HRESULT get_fvf(DWORD*) { return E_FAIL; }
        HRESULT set_fvf(DWORD) { return E_FAIL; }
        HRESULT draw_primitive(DWORD, DWORD, DWORD) { return E_FAIL; }
        HRESULT create_vertex_buffer(UINT, void**) { return E_FAIL; }
        HRESULT upload_vertex_buffer(void*, const void*, UINT) { return E_FAIL; }
        HRESULT set_stream_source(DWORD, void*, DWORD) { return E_FAIL; }
        HRESULT create_vertex_declaration(const void*, UINT, void**) { return E_FAIL; }
        HRESULT set_vertex_shader_passthrough_decl(void*) { return E_FAIL; }

        // ---- render-target bridge (gpart evolution pass; D3D8 stubs) ----
        HRESULT get_surface_level(void*, UINT, void**) { return E_FAIL; }
        HRESULT get_render_target(DWORD, void**) { return E_FAIL; }
        HRESULT set_render_target(DWORD, void*) { return E_FAIL; }
        HRESULT clear_target(DWORD) { return E_FAIL; }
        HRESULT set_viewport(UINT, UINT) { return E_FAIL; }
        HRESULT get_viewport(UINT*, UINT*) { return E_FAIL; }

        // ---- 错误文本(D3D8 全表来自 DXGetErrorDescription8A) ----
        std::string error_text(HRESULT hr)
        {
            const char* p = DXGetErrorDescription8A(hr);
            return p ? p : "Unknown error";
        }

        // ---- 设备能力 ----
        bool get_caps(Caps& out)
        {
            D3DCAPS8 caps{};
            D3DADAPTER_IDENTIFIER8 aid{};
            if (FAILED(dev()->GetDeviceCaps(&caps))) return false;
            if (FAILED(intf()->GetAdapterIdentifier(D3DADAPTER_DEFAULT, D3DENUM_NO_WHQL_LEVEL, &aid))) return false;
            out.max_point_size       = caps.MaxPointSize;           // D3D8 是 FLOAT
            out.pixel_shader_version = caps.PixelShaderVersion;
            out.vertex_shader_version = caps.VertexShaderVersion;
            out.max_tex_w            = caps.MaxTextureWidth;
            out.max_tex_h            = caps.MaxTextureHeight;
            out.max_tex_stages       = caps.MaxSimultaneousTextures;
            out.max_aniso            = caps.MaxAnisotropy;
            out.prim_misc_caps       = caps.PrimitiveMiscCaps;
            out.raster_caps          = caps.RasterCaps;
            out.vertex_tex_filter_caps = 0;   // D3D8 无 VTF
            strncpy(out.adapter_desc, aid.Description, sizeof(out.adapter_desc) - 1);
            out.adapter_desc[sizeof(out.adapter_desc) - 1] = 0;
            return true;
        }
    }
}
