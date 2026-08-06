#pragma once
// GMGraphic

#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <math.h>
#include <filesystem>
#include "gmapi.h"
#include "d3d_adapter.h"
#include "dxerr8.h"

using path = std::filesystem::path;

extern gm::CGMAPI* gmapi;
extern std::string str_ret; // Used to return strings by macro.

typedef double gm_real;
typedef const char* gm_string;
typedef gm::CGMVariable var;

#define expdll extern "C" __declspec(dllexport)
#define exp_real expdll gm_real _cdecl
#define exp_str expdll gm_string _cdecl
#define exp_uint expdll unsigned int _cdecl

constexpr double gtrue = 1.0;     // Success
constexpr double gfalse = 0.0;    // Failure
constexpr double gerror = -1.0;   // User input error
typedef unsigned long ulong;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;
typedef uint d3dcolor;
constexpr double pi = 3.1415926535897932384626433832795;
constexpr gm_string crlf = "\r\n";
constexpr double epsilon = 0.00001;

#define complain(x) gm::show_error(x,false); return gerror;
#define simple_catch(funcname, returns)															\
	catch (const std::exception& e)	{															\
		gm::show_error("An error occurred while executing function " funcname					\
			" in GMGraphic.dll:\r\n" + std::string(e.what()), false);							\
		return returns;																			\
	}
#define transpond_catch(funcname)																\
	catch (const std::exception& e)	{															\
		throw std::runtime_error("    in function " funcname ":\r\n" + std::string(e.what()));	\
	}

// Define away a bunch of gibberish...
#define return_string(str)  str_ret = str;  return str_ret.c_str();
#define dword               DWORD                               // Capslock is autopilot for cool!
#define d3dvar(x)           *((DWORD*)&x)                       // Float as dword pointer for D3D's more shitastic functions.
#define d3dcheck(f)         return (double) (D3D_OK == (f))     // Returns status immediately.
#define d3dfail(v,f)        if (D3D_OK != (f)) { return v; }    // Returns status only on fail.
#define d3drs(state,value)  d3d::set_render_state((state),(value))  // 经适配器按后端分发
#define d3dcrs(state,value) d3dcheck(d3drs(state,value))        // Set RS && return status immediately.

// Buffer parameters
#define fvf_default  ( D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 ) // GM's FVF for D3D.
#define fvf_ext      ( D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX8 )  // M-M-M-MEGA KILL

constexpr int vb_count = 8192; // Max verts per prim; GM's limit of 1024 is excessively conservative.
#define vb_ext_bytes vb_count * sizeof(vert_ext)  // 768KB
#define vb_default_bytes vb_count * sizeof(vert_default)  // 192KB

constexpr static double degtorad_mul = pi / 180.0;
constexpr static double radtodeg_mul = 180.0 / pi;
constexpr static double pi2 = pi * 2.0;

inline void D3DCheck(HRESULT result, int pos = 0)
{
	if (SUCCEEDED(result))
		return;

	throw std::runtime_error("Pos " + std::to_string(pos) + ": " + DXGetErrorDescription8A(result));
}

namespace atlas
{
	// 纹理一律不透明 void*: D3D8/9 后端下它分别是 IDirect3DTexture8/9 对象,
	// 共享代码绝不直接调它的方法, 只经 d3d:: 适配器。
	void start_draw(void* texture, D3DFORMAT format);
	void end_draw();

	struct texture_info
	{
		void* texture = nullptr;
		D3DFORMAT format = D3DFMT_A8R8G8B8;
	};
}

extern atlas::texture_info current_texture;

namespace gm
{
	extern int argument_list;
}