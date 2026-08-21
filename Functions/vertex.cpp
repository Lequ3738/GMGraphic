#include "vertex.h"
#include "../Librarys/math_s.h"
#include "../Librarys/buffer.h"
#include "../Librarys/state_guard.h"
#include <cstdint>
#include <algorithm>

// ============================================================================
// vertex_* API (DX9 only)
// Custom vertex formats + vertex buffers, aligned with modern GameMaker's
// vertex_format / vertex_create_buffer / vertex_submit family.
//
// Threading note: D3D9 vertex declarations are real objects here; formats cache
// their declaration at vertex_format_end. Buffers grow on demand (2x) while
// being written; vertex_submit draws via DrawPrimitiveUP (or a frozen static
// VB after vertex_freeze) and always restores the engine's VS/decl/FVF.
// ============================================================================

static std::unordered_map<int, vertex::Format> g_formats;
static std::unordered_map<int, vertex::Buffer> g_buffers;
static int g_fmt_counter = 1;   // ids start at 1 (0 = invalid)
static int g_buf_counter = 1;
static int g_cur_buf = -1;      // vertex_* writers target the buffer bound by vertex_begin

static bool vtx_d3d9() { return d3d::version() == d3d::V9; }

static vertex::Format* fmt_at(int id)
{
    auto it = g_formats.find(id);
    return it == g_formats.end() ? nullptr : &it->second;
}
static vertex::Buffer* buf_at(int id)
{
    auto it = g_buffers.find(id);
    return it == g_buffers.end() ? nullptr : &it->second;
}
static vertex::Buffer* cur_buf()
{
    vertex::Buffer* b = buf_at(g_cur_buf);
    if (!b) throw std::runtime_error("No vertex buffer bound (call vertex_begin first).");
    return b;
}
static vertex::Format* cur_fmt()
{
    vertex::Buffer* b = buf_at(g_cur_buf);
    if (!b) throw std::runtime_error("No vertex buffer bound (call vertex_begin first).");
    vertex::Format* f = fmt_at(b->fmt_id);
    if (!f) throw std::runtime_error("Invalid format referenced by current vertex buffer.");
    return f;
}

// ============================================================================
// buffer plugin bridge
// Reuses Librarys/buffer.h (ImportBufferModule). vertex_*_from_buffer functions
// require the buffer plugin to be imported first (GML: ImportBufferModule("Http.dll")).
// ============================================================================
static void buffer_plugin_guard()
{
    if (BufferDLL == nullptr || gm::buffer_exists == nullptr || gm::buffer_get_size == nullptr
        || gm::buffer_get_address == nullptr)
        throw std::runtime_error("Buffer plugin not imported (call ImportBufferModule first).");
}

// ============================================================================
// Format layer
// ============================================================================

static void add_comp(int fmt, vertex::CompKind kind, int size, DWORD type,
    DWORD usage, int usage_index)
{
    vertex::Format& f = *fmt_at(fmt);
    if (f.ended) throw std::runtime_error("Format already ended (call vertex_format_end last).");

    vertex::Comp c;
    c.kind = kind;
    c.usage = usage_index;
    c.offset = (int)f.stride;
    c.size = size;
    f.comps.push_back(c);

    vertex_element e;
    e.stream = 0;
    e.offset = (unsigned short)c.offset;
    e.type = (unsigned char)type;
    e.method = 0;                       // D3DDECLMETHOD_DEFAULT
    e.usage = (unsigned char)usage;
    e.usage_index = (unsigned char)usage_index;
    f.elems.push_back(e);

    f.stride += (UINT)size;
}

exp_real vertex_format_begin()
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Format f;
        int id = g_fmt_counter++;
        g_formats.emplace(id, std::move(f));
        return (double)id;
    }
    simple_catch("vertex_format_begin", gerror)
}

exp_real vertex_format_add_position(double fmt)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        if (!fmt_at((int)fmt)) throw std::runtime_error("Invalid vertex format.");
        add_comp((int)fmt, vertex::K_POS2, 8, VT_FLOAT2, VU_POSITION, 0);
        return gtrue;
    }
    simple_catch("vertex_format_add_position", gerror)
}

exp_real vertex_format_add_position_3d(double fmt)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        if (!fmt_at((int)fmt)) throw std::runtime_error("Invalid vertex format.");
        add_comp((int)fmt, vertex::K_POS3, 12, VT_FLOAT3, VU_POSITION, 0);
        return gtrue;
    }
    simple_catch("vertex_format_add_position_3d", gerror)
}

exp_real vertex_format_add_colour(double fmt)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        if (!fmt_at((int)fmt)) throw std::runtime_error("Invalid vertex format.");
        add_comp((int)fmt, vertex::K_COLOR, 4, VT_D3DCOLOR, VU_COLOR, 0);
        return gtrue;
    }
    simple_catch("vertex_format_add_colour", gerror)
}

exp_real vertex_format_add_normal(double fmt)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        if (!fmt_at((int)fmt)) throw std::runtime_error("Invalid vertex format.");
        add_comp((int)fmt, vertex::K_NORMAL, 12, VT_FLOAT3, VU_NORMAL, 0);
        return gtrue;
    }
    simple_catch("vertex_format_add_normal", gerror)
}

exp_real vertex_format_add_texcoord(double fmt)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        if (!fmt_at((int)fmt)) throw std::runtime_error("Invalid vertex format.");
        add_comp((int)fmt, vertex::K_TEXCOORD, 8, VT_FLOAT2, VU_TEXCOORD, 0);
        return gtrue;
    }
    simple_catch("vertex_format_add_texcoord", gerror)
}

// type: 1..4 = float1..4, 5 = colour, 6 = ubyte4; set = TEXCOORD/COLOR usage index.
exp_real vertex_format_add_custom(double fmt, double type, double set)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        if (!fmt_at((int)fmt)) throw std::runtime_error("Invalid vertex format.");
        int s = (int)set;
        if (s < 0) throw std::runtime_error("vertex_format_add_custom requires set >= 0 (usage index).");

        switch ((int)type)
        {
            case 1: add_comp((int)fmt, vertex::K_FLOAT1, 4, VT_FLOAT1, VU_TEXCOORD, s); break;
            case 2: add_comp((int)fmt, vertex::K_FLOAT2, 8, VT_FLOAT2, VU_TEXCOORD, s); break;
            case 3: add_comp((int)fmt, vertex::K_FLOAT3, 12, VT_FLOAT3, VU_TEXCOORD, s); break;
            case 4: add_comp((int)fmt, vertex::K_FLOAT4, 16, VT_FLOAT4, VU_TEXCOORD, s); break;
            case 5: add_comp((int)fmt, vertex::K_COLOR, 4, VT_D3DCOLOR, VU_COLOR, s); break;
            case 6: add_comp((int)fmt, vertex::K_UBYTE4, 4, VT_UBYTE4N, VU_TEXCOORD, s); break;
            default: throw std::runtime_error("Invalid custom type (1..4=float1..4, 5=colour, 6=ubyte4).");
        }
        return gtrue;
    }
    simple_catch("vertex_format_add_custom", gerror)
}

exp_real vertex_format_end(double fmt)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Format* f = fmt_at((int)fmt);
        if (!f) throw std::runtime_error("Invalid vertex format.");
        if (f->ended) throw std::runtime_error("Format already ended.");
        if (f->comps.empty()) throw std::runtime_error("Format has no components.");

        D3DCheck(d3d::create_vertex_declaration(f->elems.data(), (UINT)f->elems.size(), &f->decl), 1);
        f->ended = true;
        return gtrue;
    }
    simple_catch("vertex_format_end", gerror)
}

exp_real vertex_format_delete(double fmt)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        int id = (int)fmt;
        for (auto& kv : g_buffers)
            if (kv.second.fmt_id == id)
                throw std::runtime_error("Format is still referenced by a vertex buffer.");
        auto it = g_formats.find(id);
        if (it == g_formats.end()) return gtrue;
        if (it->second.decl) d3d::release(it->second.decl);
        g_formats.erase(it);
        return gtrue;
    }
    simple_catch("vertex_format_delete", gerror)
}

// ============================================================================
// Buffer layer
// ============================================================================

exp_real vertex_create_buffer(double fmt)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Format* f = fmt_at((int)fmt);
        if (!f) throw std::runtime_error("Invalid vertex format.");
        if (!f->ended) throw std::runtime_error("Format not ended (call vertex_format_end first).");

        vertex::Buffer b;
        b.fmt_id = (int)fmt;
        int id = g_buf_counter++;
        g_buffers.emplace(id, std::move(b));
        return (double)id;
    }
    simple_catch("vertex_create_buffer", gerror)
}

exp_real vertex_delete_buffer(double buf)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        int id = (int)buf;
        auto it = g_buffers.find(id);
        if (it == g_buffers.end()) return gtrue;
        if (it->second.frozen && it->second.vb) d3d::release(it->second.vb);
        if (g_cur_buf == id) g_cur_buf = -1;
        g_buffers.erase(it);
        return gtrue;
    }
    simple_catch("vertex_delete_buffer", gerror)
}

exp_real vertex_begin(double buf)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Buffer* b = buf_at((int)buf);
        if (!b) throw std::runtime_error("Invalid vertex buffer.");
        if (b->frozen) throw std::runtime_error("Vertex buffer is frozen (vertex_freeze).");

        g_cur_buf = (int)buf;
        b->data.clear();
        b->vert_count = 0;
        b->in_vertex = false;
        return gtrue;
    }
    simple_catch("vertex_begin", gerror)
}

exp_real vertex_end()
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Buffer* b = cur_buf();
        if (b->in_vertex) b->vert_count++;
        b->in_vertex = false;
        return gtrue;
    }
    simple_catch("vertex_end", gerror)
}

// position starts a new vertex (commits the previous one), like modern GM.
exp_real vertex_position(double x, double y)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Buffer* b = cur_buf();
        if (b->frozen) throw std::runtime_error("Vertex buffer is frozen.");
        vertex::Format* f = cur_fmt();

        vertex::Comp* pc = nullptr;
        for (auto& c : f->comps)
            if (c.kind == vertex::K_POS2 || c.kind == vertex::K_POS3) { pc = &c; break; }
        if (!pc) throw std::runtime_error("Format has no position component.");

        if (b->in_vertex) b->vert_count++;
        if (b->data.size() < (b->vert_count + 1) * f->stride)
            b->data.resize((b->vert_count + 1) * f->stride);

        float vals[2] = { (float)x, (float)y };
        memcpy(b->data.data() + b->vert_count * f->stride + pc->offset, vals,
            std::min((size_t)pc->size, sizeof(vals)));

        b->in_vertex = true;
        b->float_cursor = 0;
        return gtrue;
    }
    simple_catch("vertex_position", gerror)
}

exp_real vertex_position_3d(double x, double y, double z)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Buffer* b = cur_buf();
        if (b->frozen) throw std::runtime_error("Vertex buffer is frozen.");
        vertex::Format* f = cur_fmt();

        vertex::Comp* pc = nullptr;
        for (auto& c : f->comps)
            if (c.kind == vertex::K_POS3) { pc = &c; break; }
        if (!pc) throw std::runtime_error("Format has no 3D position component.");

        if (b->in_vertex) b->vert_count++;
        if (b->data.size() < (b->vert_count + 1) * f->stride)
            b->data.resize((b->vert_count + 1) * f->stride);

        float vals[3] = { (float)x, (float)y, (float)z };
        memcpy(b->data.data() + b->vert_count * f->stride + pc->offset, vals, sizeof(vals));

        b->in_vertex = true;
        b->float_cursor = 0;
        return gtrue;
    }
    simple_catch("vertex_position_3d", gerror)
}

static vertex::Comp* find_comp(vertex::Format& f, int kind)
{
    for (auto& c : f.comps)
        if (c.kind == kind) return &c;
    return nullptr;
}

static void write_comp(vertex::Buffer& b, int kind, const void* data, int size)
{
    vertex::Format* f = fmt_at(b.fmt_id);
    vertex::Comp* c = find_comp(*f, kind);
    if (!c) throw std::runtime_error("Format has no matching component for this call.");
    if (c->size != size) throw std::runtime_error("Component size mismatch (format layout).");
    if (!b.in_vertex) throw std::runtime_error("Call vertex_position first for each vertex.");
    memcpy(b.data.data() + b.vert_count * f->stride + c->offset, data, (size_t)size);
}

exp_real vertex_colour(double col, double alpha)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        d3dcolor c = col_d3d((int)col, alpha);
        write_comp(*cur_buf(), vertex::K_COLOR, &c, 4);
        return gtrue;
    }
    simple_catch("vertex_colour", gerror)
}

exp_real vertex_argb(double argb)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        uint c = (uint)argb;
        write_comp(*cur_buf(), vertex::K_COLOR, &c, 4);
        return gtrue;
    }
    simple_catch("vertex_argb", gerror)
}

exp_real vertex_normal(double x, double y, double z)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        float v[3] = { (float)x, (float)y, (float)z };
        write_comp(*cur_buf(), vertex::K_NORMAL, v, 12);
        return gtrue;
    }
    simple_catch("vertex_normal", gerror)
}

exp_real vertex_texcoord(double u, double v)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Buffer* b = cur_buf();
        vertex::Format* f = fmt_at(b->fmt_id);
        vertex::Comp* c = nullptr;
        for (auto& cc : f->comps)
            if (cc.kind == vertex::K_TEXCOORD) { c = &cc; break; }
        if (!c) throw std::runtime_error("Format has no texcoord component.");
        float uv[2] = { (float)u, (float)v };
        if (!b->in_vertex) throw std::runtime_error("Call vertex_position first for each vertex.");
        memcpy(b->data.data() + b->vert_count * f->stride + c->offset, uv, sizeof(uv));
        return gtrue;
    }
    simple_catch("vertex_texcoord", gerror)
}

// vertex_floatN writes into the next custom float component (in format add order).
static void write_float(vertex::Buffer& b, int n, const float* vals)
{
    if (!b.in_vertex) throw std::runtime_error("Call vertex_position first for each vertex.");
    vertex::Format* f = fmt_at(b.fmt_id);

    int idx = b.float_cursor;
    int seen = 0;
    vertex::Comp* target = nullptr;
    for (auto& c : f->comps)
        if (c.kind >= vertex::K_FLOAT1 && c.kind <= vertex::K_FLOAT4)
        {
            if (seen == idx) { target = &c; break; }
            seen++;
        }
    if (!target) throw std::runtime_error("Not enough custom float components in format.");

    int tgtn = target->kind - vertex::K_FLOAT1 + 1;
    if (tgtn != n)
        throw std::runtime_error("Float component count mismatch (format declares float" +
            std::to_string(tgtn) + ").");

    memcpy(b.data.data() + b.vert_count * f->stride + target->offset, vals, (size_t)n * 4);
    b.float_cursor++;
}

exp_real vertex_float1(double x)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        float v[1] = { (float)x };
        write_float(*cur_buf(), 1, v);
        return gtrue;
    }
    simple_catch("vertex_float1", gerror)
}
exp_real vertex_float2(double x, double y)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        float v[2] = { (float)x, (float)y };
        write_float(*cur_buf(), 2, v);
        return gtrue;
    }
    simple_catch("vertex_float2", gerror)
}
exp_real vertex_float3(double x, double y, double z)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        float v[3] = { (float)x, (float)y, (float)z };
        write_float(*cur_buf(), 3, v);
        return gtrue;
    }
    simple_catch("vertex_float3", gerror)
}
exp_real vertex_float4(double x, double y, double z, double w)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        float v[4] = { (float)x, (float)y, (float)z, (float)w };
        write_float(*cur_buf(), 4, v);
        return gtrue;
    }
    simple_catch("vertex_float4", gerror)
}

exp_real vertex_ubyte4(double x, double y, double z, double w)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        unsigned char v[4] = {
            (unsigned char)std::clamp((int)x, 0, 255),
            (unsigned char)std::clamp((int)y, 0, 255),
            (unsigned char)std::clamp((int)z, 0, 255),
            (unsigned char)std::clamp((int)w, 0, 255)
        };
        write_comp(*cur_buf(), vertex::K_UBYTE4, v, 4);
        return gtrue;
    }
    simple_catch("vertex_ubyte4", gerror)
}

// ---- submit ----

static D3DPRIMITIVETYPE prim_type(double primitive)
{
    switch ((int)primitive)
    {
        case 1: return D3DPT_POINTLIST;
        case 2: return D3DPT_LINELIST;
        case 3: return D3DPT_LINESTRIP;
        case 4: return D3DPT_TRIANGLELIST;
        case 5: return D3DPT_TRIANGLESTRIP;
        case 6: return D3DPT_TRIANGLEFAN;
        default: throw std::runtime_error("Invalid primitive (1=pointlist..6=trianglefan).");
    }
}

static UINT prim_count(D3DPRIMITIVETYPE prim, size_t verts)
{
    switch (prim)
    {
        case D3DPT_POINTLIST:     return (UINT)verts;
        case D3DPT_LINELIST:      return (UINT)(verts / 2);
        case D3DPT_LINESTRIP:     return (UINT)(verts - 1);
        case D3DPT_TRIANGLELIST:  return (UINT)(verts / 3);
        case D3DPT_TRIANGLESTRIP: return (UINT)(verts - 2);
        case D3DPT_TRIANGLEFAN:   return (UINT)(verts - 2);
        default: throw std::runtime_error("Invalid primitive.");
    }
}

// texture >= 0: bind GM texture id via texture_set_stage(0, ...); -1: leave stages alone.
static void submit_impl(int buf, double primitive, double texture, size_t start_vert, size_t vert_count)
{
    vertex::Buffer* b = buf_at(buf);
    if (!b) throw std::runtime_error("Invalid vertex buffer.");
    if (b->in_vertex) throw std::runtime_error("Call vertex_end before vertex_submit.");
    if (vert_count == 0) return;

    vertex::Format* f = fmt_at(b->fmt_id);
    if (!f || !f->ended) throw std::runtime_error("Invalid/unedited vertex format.");
    if (start_vert + vert_count > b->vert_count)
        throw std::runtime_error("Vertex range exceeds buffer contents.");

    D3DPRIMITIVETYPE prim = prim_type(primitive);
    UINT prims = prim_count(prim, vert_count);
    if (prims < 1) return;

    if (texture >= 0.0)
    {
        if (texture_set_stage(0, texture) < 0.0)
            throw std::runtime_error("Invalid texture id passed to vertex_submit.");
    }

    // Save engine state, bind our decl + a suitable VS, draw, restore.
    ShaderStateGuard engine_state;   // RAII: 析构恢复 VS/decl/FVF 并释放 Get* 引用

    D3DCheck(d3d::set_vertex_declaration(f->decl), 4);
    dword vs = 0;
    if (vertex_current_vs(&vs))
        D3DCheck(d3d::set_vertex_shader_handle(vs), 5);
    else
        D3DCheck(d3d::set_vertex_shader_passthrough_decl(f->decl), 5);

    if (b->frozen)
    {
        D3DCheck(d3d::set_stream_source(0, b->vb, f->stride), 6);
        D3DCheck(d3d::draw_primitive((DWORD)prim, prims, (DWORD)start_vert), 7);
    }
    else
    {
        D3DCheck(d3d::draw_primitive_up((DWORD)prim, prims,
            b->data.data() + start_vert * f->stride, f->stride), 7);
    }
}   // engine_state 析构: 恢复引擎状态 + 释放引用

exp_real vertex_submit(double buf, double primitive, double texture)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        submit_impl((int)buf, primitive, texture, 0,
            buf_at((int)buf) ? buf_at((int)buf)->vert_count : 0);
        return gtrue;
    }
    simple_catch("vertex_submit", gerror)
}

exp_real vertex_submit_ext(double buf, double primitive, double texture, double start, double count)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        submit_impl((int)buf, primitive, texture, (size_t)std::max((int)start, 0),
            (size_t)std::max((int)count, 0));
        return gtrue;
    }
    simple_catch("vertex_submit_ext", gerror)
}

exp_real vertex_get_number(double buf)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Buffer* b = buf_at((int)buf);
        if (!b) throw std::runtime_error("Invalid vertex buffer.");
        return (double)b->vert_count;
    }
    simple_catch("vertex_get_number", gerror)
}

exp_real vertex_get_buffer_size(double buf)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Buffer* b = buf_at((int)buf);
        if (!b) throw std::runtime_error("Invalid vertex buffer.");
        return (double)(b->vert_count * fmt_at(b->fmt_id)->stride);
    }
    simple_catch("vertex_get_buffer_size", gerror)
}

// ---- freeze ----

exp_real vertex_freeze(double buf)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Buffer* b = buf_at((int)buf);
        if (!b) throw std::runtime_error("Invalid vertex buffer.");
        if (b->frozen) return gfalse;
        if (b->in_vertex) throw std::runtime_error("Call vertex_end before vertex_freeze.");
        if (b->vert_count == 0) return gfalse;

        vertex::Format* f = fmt_at(b->fmt_id);
        size_t bytes = b->vert_count * f->stride;
        D3DCheck(d3d::create_vertex_buffer((UINT)bytes, &b->vb), 1);
        D3DCheck(d3d::upload_vertex_buffer(b->vb, b->data.data(), (UINT)bytes), 2);
        b->frozen = true;
        b->data.clear();
        b->data.shrink_to_fit();
        return gtrue;
    }
    simple_catch("vertex_freeze", gerror)
}

// ---- cross-buffer update ----

exp_real vertex_update_buffer_from_vertex(double dest, double dest_vert, double src,
    double src_vert, double vert_num)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Buffer* d = buf_at((int)dest);
        vertex::Buffer* s = buf_at((int)src);
        if (!d || !s) throw std::runtime_error("Invalid vertex buffer.");
        if (d->frozen) throw std::runtime_error("Destination buffer is frozen.");
        if (d->in_vertex) throw std::runtime_error("Call vertex_end on destination first.");
        if (s->frozen) throw std::runtime_error("Frozen source has no CPU data to copy.");

        vertex::Format* df = fmt_at(d->fmt_id);
        vertex::Format* sf = fmt_at(s->fmt_id);
        if (df->stride != sf->stride)
            throw std::runtime_error("Source/destination formats must match.");

        int sv = (int)src_vert < 0 ? 0 : (int)src_vert;
        size_t vn = (vert_num < 0.0) ? (s->vert_count - (size_t)sv) : (size_t)vert_num;
        if (sv < 0 || (size_t)sv + vn > s->vert_count)
            throw std::runtime_error("Source vertex range out of bounds.");

        int dv = (int)dest_vert < 0 ? 0 : (int)dest_vert;
        size_t need = (size_t)(dv)+vn;
        if (d->data.size() < need * df->stride)
            d->data.resize(need * df->stride);

        memcpy(d->data.data() + (size_t)dv * df->stride,
            s->data.data() + (size_t)sv * df->stride, vn * df->stride);
        d->vert_count = std::max(d->vert_count, need);
        return gtrue;
    }
    simple_catch("vertex_update_buffer_from_vertex", gerror)
}

// ============================================================================
// from_buffer family (depends on an imported buffer plugin)
// ============================================================================

exp_real vertex_create_buffer_from_buffer(double buffer, double fmt)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        buffer_plugin_guard();
        vertex::Format* f = fmt_at((int)fmt);
        if (!f) throw std::runtime_error("Invalid vertex format.");
        if (!f->ended) throw std::runtime_error("Format not ended (call vertex_format_end first).");
        if (gm::buffer_exists(buffer) < 1.0) throw std::runtime_error("Invalid buffer.");

        char* addr = (char*)(intptr_t)gm::buffer_get_address(buffer, 0.0);
        if (!addr) throw std::runtime_error("Buffer returned a null address.");
        size_t len = (size_t)gm::buffer_get_size(buffer);
        size_t n = len / f->stride;

        vertex::Buffer b;
        b.fmt_id = (int)fmt;
        b.data.resize(n * f->stride);
        if (n) memcpy(b.data.data(), addr, n * f->stride);
        b.vert_count = n;

        int id = g_buf_counter++;
        g_buffers.emplace(id, std::move(b));
        return (double)id;
    }
    simple_catch("vertex_create_buffer_from_buffer", gerror)
}

exp_real vertex_create_buffer_from_buffer_ext(double buffer, double fmt,
    double src_offset, double vert_num)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        buffer_plugin_guard();
        vertex::Format* f = fmt_at((int)fmt);
        if (!f) throw std::runtime_error("Invalid vertex format.");
        if (!f->ended) throw std::runtime_error("Format not ended (call vertex_format_end first).");
        if (gm::buffer_exists(buffer) < 1.0) throw std::runtime_error("Invalid buffer.");

        char* addr = (char*)(intptr_t)gm::buffer_get_address(buffer, 0.0);
        if (!addr) throw std::runtime_error("Buffer returned a null address.");
        size_t len = (size_t)gm::buffer_get_size(buffer);

        int off = (int)src_offset;
        size_t vn = (size_t)std::max((int)vert_num, 0);
        size_t bytes = vn * f->stride;
        if (off < 0 || (size_t)off + bytes > len)
            throw std::runtime_error("Source range out of bounds.");

        vertex::Buffer b;
        b.fmt_id = (int)fmt;
        b.data.resize(bytes);
        if (bytes) memcpy(b.data.data(), addr + off, bytes);
        b.vert_count = vn;

        int id = g_buf_counter++;
        g_buffers.emplace(id, std::move(b));
        return (double)id;
    }
    simple_catch("vertex_create_buffer_from_buffer_ext", gerror)
}

// dest_offset/src_offset/src_size are in BYTES. src_size = -1 copies to source end.
// Destination is auto-grown (stride-aligned), an improvement over modern GM.
exp_real vertex_update_buffer_from_buffer(double dest, double dest_offset, double src_buffer,
    double src_offset, double src_size)
{
    if (!vtx_d3d9()) return gerror;
    try
    {
        vertex::Buffer* d = buf_at((int)dest);
        if (!d) throw std::runtime_error("Invalid vertex buffer.");
        if (d->frozen) throw std::runtime_error("Destination buffer is frozen.");
        if (d->in_vertex) throw std::runtime_error("Call vertex_end on destination first.");

        buffer_plugin_guard();
        vertex::Format* df = fmt_at(d->fmt_id);
        if (gm::buffer_exists(src_buffer) < 1.0) throw std::runtime_error("Invalid buffer.");

        char* addr = (char*)(intptr_t)gm::buffer_get_address(src_buffer, 0.0);
        if (!addr) throw std::runtime_error("Buffer returned a null address.");
        size_t slen = (size_t)gm::buffer_get_size(src_buffer);

        int so = (int)src_offset < 0 ? 0 : (int)src_offset;
        size_t ss = (src_size < 0.0) ? (slen - (size_t)so) : (size_t)src_size;
        if (so < 0 || (size_t)so + ss > slen)
            throw std::runtime_error("Source range out of bounds.");

        int dto = (int)dest_offset < 0 ? 0 : (int)dest_offset;
        size_t need_bytes = (size_t)dto + ss;
        size_t need_verts = (need_bytes + df->stride - 1) / df->stride;

        if (d->data.size() < need_verts * df->stride)
            d->data.resize(need_verts * df->stride);
        if (ss) memcpy(d->data.data() + (size_t)dto, addr + (size_t)so, ss);
        d->vert_count = std::max(d->vert_count, need_verts);
        return gtrue;
    }
    simple_catch("vertex_update_buffer_from_buffer", gerror)
}