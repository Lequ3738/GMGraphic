#pragma once
#include "../main.h"
#include "shader.h"

// vertex_* API (DX9 only, D3D8 returns gerror).
// Semantics aligned with modern GameMaker's vertex_format / vertex_create_buffer /
// vertex_submit family. Layout constants below match D3D9's D3DDECLTYPE / D3DDECLUSAGE.
enum vertex_decl_type : DWORD
{
    VT_FLOAT1 = 0, VT_FLOAT2 = 1, VT_FLOAT3 = 2, VT_FLOAT4 = 3,
    VT_D3DCOLOR = 4, VT_UBYTE4N = 8,
};
enum vertex_decl_usage : DWORD
{
    VU_POSITION = 0, VU_NORMAL = 3, VU_TEXCOORD = 5, VU_COLOR = 10,
};

// Same memory layout as D3DVERTEXELEMENT9 (Stream:Offset:Type:Method:Usage:UsageIndex).
struct vertex_element
{
    unsigned short stream, offset;
    unsigned char  type, method, usage, usage_index;
};
constexpr vertex_element vertex_element_end() { return { 0xFF, 0, 0, 0, 0, 0 }; }

// vertex_format_add_custom type constants (GML passes these as reals).
enum vertex_custom_type : int
{
    VTYPE_FLOAT1 = 1, VTYPE_FLOAT2 = 2, VTYPE_FLOAT3 = 3, VTYPE_FLOAT4 = 4,
    VTYPE_COLOUR = 5, VTYPE_UBYTE4 = 6,
};

namespace vertex
{
    enum CompKind : int
    {
        K_POS2, K_POS3, K_COLOR, K_NORMAL, K_TEXCOORD,
        K_FLOAT1, K_FLOAT2, K_FLOAT3, K_FLOAT4, K_ARGB, K_UBYTE4
    };
    struct Comp { int kind; int usage; int offset; int size; };

    struct Format
    {
        std::vector<Comp> comps;                  // components in add order
        std::vector<vertex_element> elems;        // D3DVERTEXELEMENT9 array (no D3DDECL_END)
        UINT stride = 0;
        void* decl = nullptr;                     // IDirect3DVertexDeclaration9*
        bool ended = false;
    };

    struct Buffer
    {
        int fmt_id = -1;
        std::vector<BYTE> data;                   // vertex bytes (CPU copy unless frozen)
        size_t vert_count = 0;                    // committed vertices
        bool frozen = false;
        void* vb = nullptr;                       // IDirect3DVertexBuffer9* after freeze
        bool in_vertex = false;                   // currently filling a vertex
        int float_cursor = 0;                     // custom-float cursor inside current vertex
    };
}

// buffer plugin bridge: reuse Librarys/buffer.h's ImportBufferModule (call it from GML first).
// ---- format layer (vertex_format_*) ----
exp_real vertex_format_begin();
exp_real vertex_format_add_position(double fmt);
exp_real vertex_format_add_position_3d(double fmt);
exp_real vertex_format_add_colour(double fmt);
exp_real vertex_format_add_normal(double fmt);
exp_real vertex_format_add_texcoord(double fmt);
exp_real vertex_format_add_custom(double fmt, double type, double set);
exp_real vertex_format_end(double fmt);
exp_real vertex_format_delete(double fmt);

// ---- buffer layer (vertex_create_buffer_* / vertex_* writers / submit) ----
exp_real vertex_create_buffer(double fmt);
exp_real vertex_create_buffer_from_buffer(double buffer, double fmt);
exp_real vertex_create_buffer_from_buffer_ext(double buffer, double fmt, double src_offset, 
    double vert_num);
exp_real vertex_delete_buffer(double buf);
exp_real vertex_begin(double buf);
exp_real vertex_end();
exp_real vertex_position(double x, double y);
exp_real vertex_position_3d(double x, double y, double z);
exp_real vertex_colour(double col, double alpha);
exp_real vertex_normal(double x, double y, double z);
exp_real vertex_texcoord(double u, double v);
exp_real vertex_float1(double x);
exp_real vertex_float2(double x, double y);
exp_real vertex_float3(double x, double y, double z);
exp_real vertex_float4(double x, double y, double z, double w);
exp_real vertex_argb(double argb);
exp_real vertex_ubyte4(double x, double y, double z, double w);
exp_real vertex_submit(double buf, double primitive, double texture);
exp_real vertex_submit_ext(double buf, double primitive, double texture, double start, 
    double count);
exp_real vertex_get_number(double buf);
exp_real vertex_get_buffer_size(double buf);
exp_real vertex_freeze(double buf);
exp_real vertex_update_buffer_from_vertex(double dest, double dest_vert, double src, 
    double src_vert, double vert_num);
exp_real vertex_update_buffer_from_buffer(double dest, double dest_offset, double src_buffer, 
    double src_offset, double src_size);