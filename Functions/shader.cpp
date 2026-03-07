// Shader Extension
// Version: 1.4
// Updated: 22/Dec/2010  by LSnK
//          2026/2/10    by Lequ

#include "math_s.h"
#include "shader.h"
#include "draw_text.h"
#include "pixel_shader_defs.h"

// ============================================================================
// Variables
// ============================================================================

static IDirect3DDevice8*       d3ddev;	// D3D device pointer
static IDirect3D8*             d3dint;	// D3D interface pointer
static D3DCAPS8                d3dcaps; // GPU capability struct
static D3DADAPTER_IDENTIFIER8  d3daid;  // GPU identification struct

LPDIRECT3DVERTEXBUFFER8 vbuff_d3d;            // Pointer to D3D vertex buffer
vert_ext                vbuff_int[vb_count];  // Internal vertex buffer
uint                    vbuff_c;              // Internal counter
D3DPRIMITIVETYPE        vbuff_prim;           // Primitive to draw
bool                    vbuff_usevs;          // Use vertex shader?
bool                    vbuff_autoinc;        // Automatic increment?
bool					vbuff_use_struct;     // Use struct vertices instead of raw data?

static std::vector<ps_conf>    conf_vec_ps;			 // Dynamic arrays for configs
static std::vector<vs_conf>    conf_vec_vs;          // 
static std::vector<tex_conf>   conf_vec_tex;         // 

dword ps_sdf_comp = NULL, ps_sdf_premul_comp = NULL;

namespace gm
{
    int argument_list = noone;
}

// ============================================================================
// Initialisation
// ============================================================================

// Initialises device pointer, GPU information, buffers, etc.
exp_real init(gm_real arg_list)
{
    d3ddev = gmapi->GetDirect3DDevice();
    d3dint = gmapi->GetDirect3DInterface();

    d3dint->GetAdapterIdentifier(D3DADAPTER_DEFAULT, D3DENUM_NO_WHQL_LEVEL, &d3daid);
    d3ddev->GetDeviceCaps(&d3dcaps);

    d3ddev->CreateVertexBuffer(vb_bytes, D3DUSAGE_WRITEONLY, fvf_ext, D3DPOOL_MANAGED, 
        &vbuff_d3d);

    gm::argument_list = (int)arg_list;

    ps_sdf_comp = (dword)d3d_ps_create(ps_sdf);
	ps_sdf_premul_comp = (dword)d3d_ps_create(ps_sdf_premul);

    sdf::shader = ps_sdf_comp;

    return gtrue;
}

// ============================================================================
// Information
// ============================================================================

// GPU name.
exp_str d3d_dev_get_name() { return_string(d3daid.Description); }

// Maximum size of point primitives.
exp_real d3d_dev_get_point_max_size() { return (double)d3dcaps.MaxPointSize; }

// GPU pixel shader version. 10 to 14.
// Most modern GPUs support higher versions but they're not reported here.
exp_real d3d_dev_get_ps_version()
{
    uint v = (uint)d3dcaps.PixelShaderVersion;
    return (double)((((v >> 8) & 0xFF) * 10) + v & 0xFF);
}

// Maximum texture width. Applies to all graphical resources.
exp_real d3d_dev_get_tex_max_width() { return (double)d3dcaps.MaxTextureWidth; }

// Height.
exp_real d3d_dev_get_tex_max_height() { return (double)d3dcaps.MaxTextureHeight; }

// Maximum simultaneous textures. Limits how many texture stages you can use.
exp_real d3d_dev_get_tex_max_stages()
{
    return (double)d3dcaps.MaxSimultaneousTextures;
}

// Free texture memory in bytes. Approximate. This ISN'T the VRAM size.
exp_real d3d_dev_get_tex_mem()
{
    return (double)d3ddev->GetAvailableTextureMem();
}

// ============================================================================
// Pixel shaders
// ============================================================================

// Assembles and creates pixel shader. Returns handle.
exp_real d3d_ps_create(const char* src_asm)
{
    using namespace std;
    
    string       str = src_asm;
    string       err;
    dword        shader;
    LPD3DXBUFFER psc;
    LPD3DXBUFFER errors;

    if (d3dcaps.PixelShaderVersion < D3DPS_VERSION(1, 4))
    {
        err = "PS 1.4 unsupported by GPU.";
        complain(err.c_str());
        return gfalse;
    }

    if (D3D_OK != D3DXAssembleShader((LPCVOID)str.c_str(), str.length(), 0, nullptr, 
        &psc, &errors))
    {
        err.append(crlf);
        err.append("Shader assembly error:");
        err.append(crlf);
        err.append(crlf);
        err.append((char*)errors->GetBufferPointer());
        err.append(crlf);
        err.append(crlf);
        err.append(str);
        complain(err.c_str());
        return gerror;
    }

    if (D3D_OK != d3ddev->CreatePixelShader((dword*)psc->GetBufferPointer(), &shader))
    {
        err = "Shader creation failed.  This should never happen.";
        complain(err.c_str());
        return gfalse;
    }

    return (double)shader;
}

// Free shader.
exp_real d3d_ps_destroy(double shader)
{
    d3dcheck(d3ddev->DeletePixelShader((dword)shader));
}

// Set active pixel shader, or -1 to disable.
exp_real d3d_set_ps(double shader)
{
    if (shader < 0) { d3dcheck(d3ddev->SetPixelShader(NULL)); }
    else { d3dcheck(d3ddev->SetPixelShader((dword)shader)); }
}

// Set active pixel shader and configuration.
exp_real d3d_set_ps_ext(double shader, double conf)
{
    bool a, b;
    a = (d3d_set_ps(shader) > 0.0);
    b = (d3d_set_ps_conf(conf) > 0.0);

    if (a && b)
        return gtrue;
    else
        return gerror;
}

// Set pixel shader constant register.
// There are 8 registers indexed 0-7 each containing 4 floating point values
// between -1 and 1. This is the same as using "def" in the shader.
exp_real d3d_set_ps_const(double constant, double r, double g, double b, double a)
{
    ps_const cx = {
        .r = (float)clamp(r, -1.0, 1.0),
        .g = (float)clamp(g, -1.0, 1.0),
        .b = (float)clamp(b, -1.0, 1.0),
        .a = (float)clamp(a, -1.0, 1.0)
    };

    d3dcheck(d3ddev->SetPixelShaderConstant((dword)constant, &cx, 1));
}

// Set PS constant as colour. Handy shortcut.
exp_real d3d_set_ps_const_col(double constant, double col, double alpha)
{
    int      c = (int)col;
    ps_const cx = {
        .r = (float(col_red(c)) / 255.0f),
        .g = (float(col_green(c)) / 255.0f),
        .b = (float(col_blue(c)) / 255.0f),
        .a = (float)clamp(alpha, 0.0, 1.0)
    };

    d3dcheck(d3ddev->SetPixelShaderConstant((dword)constant, (void*)&cx, 1));
}

// Set PS constant registers from a predefined configuration.
// Constants not set in the config aren't overwritten.
exp_real d3d_set_ps_conf(double conf)
{
    if (conf > conf_vec_ps.size() - 1)
        return gerror;

    uint     vpos = (uint)floor(conf);
    ps_conf* px = &(conf_vec_ps[vpos]);

    for (uint i = 0; i < 8; i++)
    {
        if (px->set[i])
            d3ddev->SetPixelShaderConstant(i, &(px->c[i]), 1);
    }

    return gtrue;
}

// ============================================================================
// Vertex shaders
// ============================================================================

// Assembles && creates vertex shader. Returns handle.
// Whoever wrote the D3D API should be punched in the balls. Just for the record.
exp_real d3d_vs_create(const char* src_asm)
{
    using namespace std;
    
    string       str = src_asm;
    string       err;
    DWORD        shader;
    LPD3DXBUFFER vsc;
    LPD3DXBUFFER constants;
    LPD3DXBUFFER errors;

    if (D3D_OK != D3DXAssembleShader((LPCVOID)str.c_str(), str.length(), 0, &constants, 
        &vsc, &errors))
    {
        err.append(crlf);
        err.append("Shader assembly error:");
        err.append(crlf);
        err.append(crlf);
        err.append((char*)errors->GetBufferPointer());
        err.append(crlf);
        err.append(crlf);
        err.append(str);
        complain(err.c_str());
        return gerror;
    }


    const uint o = (96 * 5); // 96 sets of [d3dvsd_const + 4 floats]
    dword      vsdec[o + 32]{};

    SecureZeroMemory((PVOID)vsdec, sizeof(vsdec));
    memcpy(vsdec, constants->GetBufferPointer(), constants->GetBufferSize());

    vsdec[o] = D3DVSD_STREAM(0);                                 // VSR     V-out    PSR
    vsdec[o + 1] = D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3);  // v0  ->  oPos
    vsdec[o + 2] = D3DVSD_REG(D3DVSDE_NORMAL, D3DVSDT_FLOAT3);  // v3
    vsdec[o + 3] = D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR);  // v5  ->  oD0   -> v0
    vsdec[o + 4] = D3DVSD_REG(D3DVSDE_SPECULAR, D3DVSDT_D3DCOLOR);  // v6  ->  oD1   -> v1
    vsdec[o + 5] = D3DVSD_REG(D3DVSDE_TEXCOORD0, D3DVSDT_FLOAT2);  // v7  ->  oT0   -> t0
    vsdec[o + 6] = D3DVSD_REG(D3DVSDE_TEXCOORD1, D3DVSDT_FLOAT2);  // v8  ->  oT1   -> t1
    vsdec[o + 7] = D3DVSD_REG(D3DVSDE_TEXCOORD2, D3DVSDT_FLOAT2);  // v9  ->  oT2   -> t2
    vsdec[o + 8] = D3DVSD_REG(D3DVSDE_TEXCOORD3, D3DVSDT_FLOAT2);  // v10 ->  oT3   -> t3
    vsdec[o + 9] = D3DVSD_REG(D3DVSDE_TEXCOORD4, D3DVSDT_FLOAT2);  // v11 ->  oT4   -> t4
    vsdec[o + 10] = D3DVSD_REG(D3DVSDE_TEXCOORD5, D3DVSDT_FLOAT2);  // v12 ->  oT5   -> t5
    vsdec[o + 11] = D3DVSD_REG(D3DVSDE_TEXCOORD6, D3DVSDT_FLOAT2);  // v13 ->  oT6
    vsdec[o + 12] = D3DVSD_REG(D3DVSDE_TEXCOORD7, D3DVSDT_FLOAT2);  // v14 ->  oT7
    vsdec[o + 13] = D3DVSD_END();

    if (D3D_OK != d3ddev->CreateVertexShader(vsdec, (dword*)vsc->GetBufferPointer(), 
        &shader, D3DUSAGE_SOFTWAREPROCESSING))
    {
        err.append(crlf);
        err.append("Vertex shader creation failed.");
        err.append(crlf);
        err.append(crlf);
        err.append(str);
        complain(err.c_str());
        return gerror;
    }

    return (double)shader;
}

// Free vertex shader.
exp_real d3d_vs_destroy(double shader)
{
    d3dcheck(d3ddev->DeleteVertexShader((DWORD)shader));
}

// Set current vertex shader or -1 for none. GM's normal drawing functions are not affected.
exp_real d3d_set_vs(double shader)
{
    if (shader < 0)
    {
        vbuff_usevs = false;
        d3dcheck(d3ddev->SetVertexShader(fvf_ext));
    }
    else {
        vbuff_usevs = true;
        d3dcheck(d3ddev->SetVertexShader((dword)shader));
    }
}

// Set VS constant register.  There are 96 4-component registers available.
exp_real d3d_set_vs_const(double constant, double x, double y, double z, double w)
{
    vs_const vx = {
        .x = (float)x,
        .y = (float)y,
        .z = (float)z,
        .w = (float)w
    };

    d3dcheck(d3ddev->SetVertexShaderConstant((dword)clamp(constant, 0.0, 95.0), &vx, 1));
}

// Set VS constant as colour. Handy shortcut.
exp_real d3d_set_vs_const_col(double constant, double col, double alpha)
{
    int      c = (int)col;
    vs_const vx = {
        .x = (float(col_red(c)) / 255.0f),
        .y = (float(col_green(c)) / 255.0f),
        .z = (float(col_blue(c)) / 255.0f),
        .w = (float)clamp(alpha, 0.0, 1.0)
    };

    d3dcheck(d3ddev->SetVertexShaderConstant((dword)clamp(constant, 0.0, 95.0), &vx, 1));
}

// Sets four constants as the transposed world*view*projection matrix.
// You can then use [m4x4 oPos,v0,cn] in the shader to transform the vertices in
// keeping with GM's normal behaviour.
// Ex: 0 would set c0,c1,c2,c3; 4 would set c4,c5,c6,c7.
exp_real d3d_set_vs_const_matrix(double constant)
{
    D3DXMATRIX world, proj, view, out, in;
    d3ddev->GetTransform(D3DTS_WORLD, &world);
    d3ddev->GetTransform(D3DTS_VIEW, &view);
    d3ddev->GetTransform(D3DTS_PROJECTION, &proj);

    in = world * view * proj;
    D3DXMatrixTranspose(&out, &in);

    d3dcheck(d3ddev->SetVertexShaderConstant((dword)clamp(constant, 0.0, 92.0), out, 4));
}

// Set VS constant registers from a predefined configuration.
exp_real d3d_set_vs_conf(double conf)
{
    if (conf > conf_vec_vs.size() - 1)
        return gerror;

    uint     vpos = (uint)floor(conf);
    vs_conf* vx = &(conf_vec_vs[vpos]);

    for (uint i = 0; i < 96; i++)
    {
        if (vx->set[i])
            d3ddev->SetVertexShaderConstant(i, &(vx->c[i]), 1);
    }

    return gtrue;
};

// ============================================================================
// Texture stages
// ============================================================================

// Set texture for this texture stage or -1 for none.
// Read the texture in a shader by using (texld r<stage>,t0).
// GM uses 0 for drawing so don't fiddle with it.
exp_real d3d_set_tex(double stage, double tex)
{
    uint s = (uint)stage;

    if (s < d3dcaps.MaxSimultaneousTextures)
    {
        if (tex < 0.0)
        {
            d3dcheck(d3ddev->SetTexture(s, NULL));
        }
        else
        {
            d3dtex* t = gmapi->GetDirect3DTexture((int)tex);

            if (t == nullptr)
                return gerror;

            d3dcheck(d3ddev->SetTexture(s, t));
        }
    }
    else
        return gerror;

    return gtrue;
}

// Set all available texture stages. -1 for no texture.
exp_real d3d_set_tex_all(double tex)
{
    for (uint i = 0; i < d3dcaps.MaxSimultaneousTextures; i++)
    {
        if (!d3d_set_tex(i, tex))
            return gerror;
    }

    return gtrue;
}

// Set texture stage interpolation mode. Use tex_int_ constant.
// Defaults to nearest, which is usually undesirable.
// Stage 0 is also controlled by texture_set_interpolation in GM.
exp_real d3d_set_tex_int(double stage, double mode)
{
    dword s = (dword)stage;

    if (stage >= d3dcaps.MaxSimultaneousTextures)
        return gerror;

    if ((D3D_OK == d3ddev->SetTextureStageState(s, D3DTSS_MAGFILTER, (dword)mode))
        && (D3D_OK == d3ddev->SetTextureStageState(s, D3DTSS_MINFILTER, (dword)mode)))
    {
        return gtrue;
    }

    return gfalse;
}

// Set texture stage wrapping mode.  Use tex_wrap_ constant.
exp_real d3d_set_tex_wrap(double stage, double xmode, double ymode)
{
    dword s = (dword)stage;

    if (stage >= d3dcaps.MaxSimultaneousTextures)
        return gerror;

    if ((D3D_OK == d3ddev->SetTextureStageState(s, D3DTSS_ADDRESSU, (dword)xmode))
        && (D3D_OK == d3ddev->SetTextureStageState(s, D3DTSS_ADDRESSV, (dword)ymode)))
    {
        return gtrue;
    }

    return gfalse;
}

// Set colour to use with tex_wrap_border.
exp_real d3d_set_tex_border(double stage, double col, double alpha)
{
    dword s = (dword)stage;

    if (s >= d3dcaps.MaxSimultaneousTextures)
        return gerror;

    d3dcheck(d3ddev->SetTextureStageState(s, D3DTSS_BORDERCOLOR, col_d3d((int)col, alpha)));
}

// Set anisotropic filtering level for tex_int_anisotropic.
// Values: 1,2,4,8,16.  1=none.  Defaults to 1.
exp_real d3d_set_tex_aniso(double stage, double anisotropy)
{
    dword s = (dword)stage;

    if (s >= d3dcaps.MaxSimultaneousTextures)
        return gerror;

    d3dcheck(d3ddev->SetTextureStageState(s, D3DTSS_MAXANISOTROPY, 
        (dword)std::min((dword)anisotropy, d3dcaps.MaxAnisotropy)));
}

// Set mipmap filtering mode. tex_int_nearest or tex_int_bilinear.
// The latter when combined with tex_int_bilinear on the texture itself results
// in trilinear filtering.
exp_real d3d_set_tex_mip(double stage, double mode)
{
    dword s = (dword)stage;

    if (s >= d3dcaps.MaxSimultaneousTextures)
        return gerror;

    d3dcheck(d3ddev->SetTextureStageState(s, D3DTSS_MIPFILTER, (dword)mode));
}

// Set texture stage state from a predefined configuration.
exp_real d3d_set_tex_conf(double conf)
{
    if (conf > conf_vec_tex.size() - 1)
        return gerror;

    uint vpos = (uint)floor(conf);

    for (uint i = 0; i < (uint)std::min(8, (int)d3dcaps.MaxSimultaneousTextures); i++)
    {
        if (conf_vec_tex[vpos].set[i])
        {
            d3ddev->SetTexture(i, conf_vec_tex[vpos].tex[i]);
            d3d_set_tex_int(i, conf_vec_tex[vpos].in[i]);
            d3d_set_tex_wrap(i, conf_vec_tex[vpos].xwrap[i],
                conf_vec_tex[vpos].ywrap[i]);
        }
    }
    return gtrue;
}

// ============================================================================
// Configurations
// ============================================================================

// Creates new pixel shader configuration and returns its index.
exp_real d3d_conf_ps_create()
{
    ps_conf conf{};

    SecureZeroMemory((PVOID)&conf, sizeof(ps_conf));

    conf_vec_ps.push_back(conf);
    return (double)conf_vec_ps.size() - 1;
}

// Defines PS constant configuration.
exp_real d3d_conf_ps_set(double conf, double constant, double r, double g, 
    double b, double a)
{
    if (conf > conf_vec_ps.size() - 1)
        return gerror;

    uint vpos = (uint)floor(conf);
    uint cpos = (uint)floor(clamp(constant, 0.0, 7.0));

    conf_vec_ps[vpos].c[cpos].r = (float)clamp(r, -1.0, 1.0);
    conf_vec_ps[vpos].c[cpos].g = (float)clamp(g, -1.0, 1.0);
    conf_vec_ps[vpos].c[cpos].b = (float)clamp(b, -1.0, 1.0);
    conf_vec_ps[vpos].c[cpos].a = (float)clamp(a, -1.0, 1.0);
    conf_vec_ps[vpos].set[cpos] = true;

    return gtrue;
}

// Creates new vertex shader configuration && returns its index.
exp_real d3d_conf_vs_create()
{
    vs_conf conf{};

    SecureZeroMemory((PVOID)&conf, sizeof(vs_conf));

    conf_vec_vs.push_back(conf);
    return (double)conf_vec_vs.size() - 1;
}

// Defines VS constant configuration.
exp_real d3d_conf_vs_set(double conf, double constant, double x, double y, 
    double z, double w)
{
    if (conf > conf_vec_vs.size() - 1)
        return gerror;

    uint vpos = (uint)floor(conf);
    uint cpos = (uint)floor(clamp(constant, 0.0, 255.0));

    conf_vec_vs[vpos].c[cpos].x = (float)x;
    conf_vec_vs[vpos].c[cpos].y = (float)y;
    conf_vec_vs[vpos].c[cpos].z = (float)z;
    conf_vec_vs[vpos].c[cpos].w = (float)w;
    conf_vec_vs[vpos].set[cpos] = true;

    return gtrue;
}

// Creates new texture stage configuration && returns its index.
exp_real d3d_conf_tex_create()
{
    tex_conf conf{};

    SecureZeroMemory((PVOID)&conf, sizeof(tex_conf));

    conf_vec_tex.push_back(conf);
    return (double)conf_vec_tex.size() - 1;
}

exp_real d3d_conf_tex_set(double conf, double stage, double tex, double interp, 
    double xmode, double ymode)
{
    if (conf > conf_vec_tex.size() - 1)
        return gerror;

    uint vpos = (uint)floor(conf);
    uint s = (uint)stage;

    if (s >= d3dcaps.MaxSimultaneousTextures)
        return gerror;

    d3dtex* t = gmapi->GetDirect3DTexture((int)tex);

    if (t != nullptr)
    {
        conf_vec_tex[vpos].tex[s] = t;
        conf_vec_tex[vpos].in[s] = (dword)interp;
        conf_vec_tex[vpos].xwrap[s] = (dword)xmode;
        conf_vec_tex[vpos].ywrap[s] = (dword)ymode;
        conf_vec_tex[vpos].set[s] = true;
        return gtrue;
    }
    else
        return gerror;
}

// ============================================================================
// Fog
// ============================================================================

// Turn fog on or off.
exp_real d3d_set_fog_state(double state) { d3dcrs(D3DRS_FOGENABLE, (state > 0.0)); }

// Set the type of fog.  Use fog_type_ constants.  Linear by default.
exp_real d3d_set_fog_type(double fog_type)
{
    d3dcrs(D3DRS_FOGTABLEMODE, (dword)fog_type);
}

// Controls density of exponential fog. 0-1.
exp_real d3d_set_fog_density(double density)
{
    float d = (float)clamp(density, 0, 1);
    d3dcrs(D3DRS_FOGDENSITY, d3dvar(d));
}

// Set fog colour.
exp_real d3d_set_fog_color(double col)
{
    d3dcrs(D3DRS_FOGCOLOR, col_d3d((int)col, 0.0));
}

// Set fog start.
exp_real d3d_set_fog_start(double dist)
{
    float d = (float)dist;
    d3dcrs(D3DRS_FOGSTART, d3dvar(d));
}

// Set fog end.
exp_real d3d_set_fog_end(double dist)
{
    float d = (float)dist;
    d3dcrs(D3DRS_FOGEND, d3dvar(d));
}

// ============================================================================
// Points
// ============================================================================

// Set drawing size for point primitives.
exp_real d3d_set_point_size(double size)
{
    float x = (float)clamp(size, 1.0, d3dcaps.MaxPointSize);
    d3dcrs(D3DRS_POINTSIZE, d3dvar(x));
}

// Set size clamp, useful for scaled points in 3D mode. Defaults to 1.
exp_real d3d_set_point_size_min(double size)
{
    float x = (float)clamp(size, 1.0, d3dcaps.MaxPointSize);
    d3dcrs(D3DRS_POINTSIZE_MIN, d3dvar(x));
}

// Set size clamp.  Defaults to 64.
exp_real d3d_set_point_size_max(double size)
{
    float x = (float)clamp(size, 1.0, d3dcaps.MaxPointSize);
    d3dcrs(D3DRS_POINTSIZE_MAX, d3dvar(x));
}

// Enable point scaling. This scales points based on their distance from the camera.
exp_real d3d_set_point_scale(double state)
{
    d3dcrs(D3DRS_POINTSCALEENABLE, (state > 0.0));
}

// Configure point scaling formula.  Defaults to (1,0,0).
// The formula is:
// size * sqrt(1/( ceof1 + (coef2*distancetocamera) + (coef3*sqr(distancetocamera)) ))
exp_real d3d_set_point_scale_coef(double coef1, double coef2, double coef3)
{
    dword a, b, c;
    float ca, cb, cc;

    ca = (float)abs(coef1);
    cb = (float)abs(coef2);
    cc = (float)abs(coef3);

    a = d3drs(D3DRS_POINTSCALE_A, d3dvar(ca));
    b = d3drs(D3DRS_POINTSCALE_B, d3dvar(cb));
    c = d3drs(D3DRS_POINTSCALE_C, d3dvar(cc));
    return (double)(a == D3D_OK && b == D3D_OK && c == D3D_OK);
}

// When enabled, this causes point primitives to be drawn with textures applied.
exp_real d3d_set_point_sprite(double state)
{
    d3dcrs(D3DRS_POINTSPRITEENABLE, (state > 0.0));
}

// ============================================================================
// Render control
// ============================================================================

// Set colour writemask.
// You can enable/disable writing of each channel independently. All enabled by default.
exp_real d3d_set_mask(double r, double g, double b, double a)
{
    if (!(d3dcaps.PrimitiveMiscCaps & D3DPMISCCAPS_COLORWRITEENABLE))
        return gfalse;

    dword mask[4]{};
    mask[0] = (r > 0.0) ? D3DCOLORWRITEENABLE_RED : 0;
    mask[1] = (g > 0.0) ? D3DCOLORWRITEENABLE_GREEN : 0;
    mask[2] = (b > 0.0) ? D3DCOLORWRITEENABLE_BLUE : 0;
    mask[3] = (a > 0.0) ? D3DCOLORWRITEENABLE_ALPHA : 0;

    d3dcrs(D3DRS_COLORWRITEENABLE, mask[0] | mask[1] | mask[2] | mask[3]);
}

// Set z-buffer writing.
// Enabled by default. Disabling it prevents overwriting of the z-buffer.
exp_real d3d_set_zwrite(double state)
{
    d3dcrs(D3DRS_ZWRITEENABLE, (state > 0.0));
}

// Prevents drawing of pixels that don't meet the given alpha criteria. 
// Use the cmp_ constants. Pass -1 as value to disable alpha testing.
exp_real d3d_set_alphatest(double value, double mode)
{
    dword a, b, c;
    if (value < 0.0)
    {
        d3dcrs(D3DRS_ALPHATESTENABLE, false);
    }
    else
    {
        a = d3drs(D3DRS_ALPHATESTENABLE, true);
        b = d3drs(D3DRS_ALPHAREF, (dword)clamp(value, 0x00000000, 0x000000FF));
        c = d3drs(D3DRS_ALPHAFUNC, (dword)mode);
    }

    return (a == D3D_OK && b == D3D_OK && c == D3D_OK);
}

// Prevents drawing of pixels that don't meet the given depth criteria. 
// Use the cmp_ constants. Defaults to <=. The other value is the current z-buffer value.
exp_real d3d_set_ztest(double mode)
{
    if (!(d3dcaps.RasterCaps & D3DPRASTERCAPS_ZTEST))
        return gfalse;

    d3dcrs(D3DRS_ZFUNC, (dword)mode);
}

// Offsets the drawing depth, allowing polygons with the same positions to be 
// drawn without z-fighting artifacts.  Useful for shadows, decals, etc.
// Integer 0-16, defaults to 0. Higher values appear in front of lower ones.
exp_real d3d_set_zbias(double bias)
{
    d3dcrs(D3DRS_ZBIAS, (uint)floor(clamp(bias, 0, 16)));
}

// Set how D3D renders polygons: point, wireframe or solid.
// Use d3d_fillmode_ constants. Defaults to solid.
exp_real d3d_set_fillmode(double mode) { d3dcrs(D3DRS_FILLMODE, (dword)mode); }

// Automatically normalises vectors when rendering.
// Should solve problems with models changing brightness when scaled.
exp_real d3d_set_normal_auto(double state)
{
    d3dcrs(D3DRS_NORMALIZENORMALS, (state > 0.0));
}

// ============================================================================
// Primitives
// ============================================================================

// Begin drawing an extended primitive.
void vertex::begin(D3DPRIMITIVETYPE primitive, bool textured)
{
    vbuff_c = 0;
    vbuff_autoinc = textured;
    vbuff_prim = primitive;
    vbuff_use_struct = false;

    // Zero the buffer.
	std::memset(vbuff_int, 0, vb_bytes);

    if (!textured)
        d3d_set_tex_all(-1);
}

exp_real d3d_primitive_begin_ext(double primitive, double textured)
{
    D3DPRIMITIVETYPE prim = D3DPT_POINTLIST;
    switch ((int)primitive)
    {
        case 1: { prim = D3DPT_POINTLIST; } break;
        case 2: { prim = D3DPT_LINELIST; } break;
        case 3: { prim = D3DPT_LINESTRIP; } break;
        case 4: { prim = D3DPT_TRIANGLELIST; } break;
        case 5: { prim = D3DPT_TRIANGLESTRIP; } break;
        case 6: { prim = D3DPT_TRIANGLEFAN; } break;
        default: { return gerror; } break;
    }
    
    vertex::begin(prim, textured < 0.5);
    return gtrue;
}

vert_ext* vertex::get_struct()
{
    vbuff_use_struct = true;
    return &vbuff_int[vbuff_c++];
}

vert_ext* vertex::get_struct(uint pos)
{
    vbuff_use_struct = true;
    return &vbuff_int[pos];
}

// Position, normal, diffuse/specular colour and alpha.
void vertex::add(float x, float y, float z, float nx, float ny, float nz, uint col, uint speccol)
{
    vbuff_int[vbuff_c].x = x;
    vbuff_int[vbuff_c].y = y;
    vbuff_int[vbuff_c].z = z;
    vbuff_int[vbuff_c].nx = nx;
    vbuff_int[vbuff_c].ny = ny;
    vbuff_int[vbuff_c].nz = nz;
    vbuff_int[vbuff_c].c = col;
    vbuff_int[vbuff_c].s = speccol;

    if (vbuff_autoinc)
        vbuff_c++;
}

exp_real d3d_vertex_ext(double x, double y, double z, double nx, double ny, double nz, 
    double col, double alpha, double speccol, double specalpha)
{
    vertex::add((float)x, (float)y, (float)z, (float)nx, (float)ny, (float)nz,
        col_d3d((int)col, alpha), col_d3d((int)speccol, specalpha));

    return gtrue;
}

// Set vertex texture coordinates.
// There are eight sets indexed 0-7, one for each texture stage.
void vertex::add_tex(uint stage, float xtex, float ytex)
{
    if (stage < 0) stage = 0;
    else if (stage > 7) stage = 7;

    uint ind = (uint)(stage * 2);

    vbuff_int[vbuff_c].uv[ind] = xtex;
    vbuff_int[vbuff_c].uv[ind + 1] = ytex;
}

exp_real d3d_vertex_ext_tex(double stage, double xtex, double ytex)
{
    vertex::add_tex((uint)stage, (float)xtex, (float)ytex);
    return gtrue;
}

// Call when finished with the current vertex to start defining the next one.
// Only required for textured primitives.
void vertex::next()
{
    if (vbuff_autoinc)
    {
        gm::show_error("d3d_vertex_ext_next() is called automatically for untextured"
            "extended primitives.", false);
    }
    vbuff_c++;
}

exp_real d3d_vertex_ext_next()
{
    vertex::next();
    return gtrue;
}

// Draw the primitive.
void vertex::end()
{
    try
    {
		uint count = vbuff_c;
        if (!vbuff_autoinc && !vbuff_use_struct)
            count += 1;

        if (count < 1)
            return;

        BYTE* bytep;
        uint  size = vbuff_c * sizeof(vert_ext);
        uint  prims;

        switch (vbuff_prim)
        {
            case D3DPT_POINTLIST:     prims = count; break;
            case D3DPT_LINELIST:      prims = count / 2; break;
            case D3DPT_LINESTRIP:     prims = count - 1; break;
            case D3DPT_TRIANGLELIST:  prims = count / 3; break;
            case D3DPT_TRIANGLESTRIP: prims = count - 2; break;
            case D3DPT_TRIANGLEFAN:   prims = count - 2; break;
            default: throw std::runtime_error("Unknown primitive type."); break;
        }

        if (prims < 1)
            throw std::runtime_error("The number of vertices is too small.");

        D3DCheck(vbuff_d3d->Lock(0, size, &bytep, 0), 0);
        memcpy(bytep, vbuff_int, size); // Copy only used part to conserve bandwidth
        D3DCheck(vbuff_d3d->Unlock(), 1);

        D3DCheck(d3ddev->SetStreamSource(0, vbuff_d3d, sizeof(vert_ext)), 2);

        if (!vbuff_usevs)
            D3DCheck(d3ddev->SetVertexShader(fvf_ext), 3);

        D3DCheck(d3ddev->DrawPrimitive(vbuff_prim, 0, prims), 4);
    }
    transpond_catch("vertex::end()")
}

exp_real d3d_primitive_end_ext()
{
    try
    {
        vertex::end();
        return gtrue;
    }
    simple_catch("d3d_primitive_end_ext", gerror)
}

// 2D equivalent.
exp_real draw_primitive_begin_ext(double primitive, double textured)
{
    return d3d_primitive_begin_ext(primitive, textured);
}

// 2D equivalent.
// The buffer is zeroed by begin_ext; no need to set the 3D stuff here.
exp_real draw_vertex_ext(double x, double y, double col, double alpha, 
    double speccol, double specalpha)
{
    vbuff_int[vbuff_c].x = (float)x;
    vbuff_int[vbuff_c].y = (float)y;

    vbuff_int[vbuff_c].c = col_d3d((int)col, alpha);
    vbuff_int[vbuff_c].s = col_d3d((int)speccol, specalpha);

    if (vbuff_autoinc)
        vbuff_c++;

    return gtrue;
}

// 2D equivalent.
exp_real draw_vertex_ext_tex(double stage, double xtex, double ytex)
{
    return d3d_vertex_ext_tex(stage, xtex, ytex);
}

// 2D equivalent.
exp_real draw_vertex_ext_next() { return d3d_vertex_ext_next(); }

// 2D equivalent.
exp_real draw_primitive_end_ext() { return d3d_primitive_end_ext(); }