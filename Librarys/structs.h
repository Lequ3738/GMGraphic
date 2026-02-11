#pragma once

#include "..\main.h"

// Pixel shader constant.
struct ps_const
{
	float r;
	float g;
	float b;
	float a;
};

// User-defined constant register states
struct ps_conf
{
	bool     set[8];
	ps_const c[8];  // Values
};

// Vertex shader constant.
struct vs_const
{
	float x;
	float y;
	float z;
	float w;
};

// User-defined constant register states
struct vs_conf
{
	vs_const c[96];     // Hardware suports more, but GM is software-only.
	bool     set[96];
};

// User-defined texture stage states
struct tex_conf
{
	bool     set[8];  // Is this tex stage set?
	d3dtex* tex[8];  // Texture pointer
	dword    in[8];  // Texture interpolation mode
	dword    xwrap[8];  // Texture wrapping modes
	dword    ywrap[8];  // 
};

// GM's default D3D type, 36 bytes.
struct vert_default
{
	float x, y, z;
	float nx, ny, nz;
	dword c;
	float uv[2];
};

// Extended type, 96 bytes.
struct vert_ext
{
	float x, y, z;     // Position
	float nx, ny, nz;  // Normal
	dword c;           // Diffuse col,  PS v0
	dword s;           // Specular col, PS v1
	float uv[16];      // Texcoords,    PS t0-t5. 6+7 are not accessible to pixel shaders.
};