// state_guard.h —— D3D 状态保存/恢复与自有引用的 RAII 封装。
//
// 引用纪律(重要):
//   - d3d::get_*/create_* 返回的 COM 指针带一次引用, 归调用方所有, 必须释放。
//     用 d3d::Ref 包装后析构自动释放, "忘记 Release"从写法上不可能发生。
//   - gmapi->GetDirect3DTexture 等返回的是引擎资源表里的借用裸指针(无引用),
//     绝不能交给 d3d::Ref 或手动 Release —— 那等于替引擎放掉它唯一的引用。
//   一句话: d3d::get_*/create_* 的结果归你; gmapi->Get* 只是看一眼。

#pragma once
#include "d3d_adapter.h"
#include <cstdint>

namespace d3d
{
	// 自有 COM 引用(唯一所有权): 析构自动 Release。只接受 get_*/create_* 的结果。
	struct Ref
	{
		void* p = nullptr;
		Ref() = default;
		explicit Ref(void* q) : p(q) {}
		Ref(const Ref&) = delete;
		Ref& operator=(const Ref&) = delete;
		Ref(Ref&& o) noexcept : p(o.p) { o.p = nullptr; }
		Ref& operator=(Ref&& o) noexcept
		{
			if (this != &o) { if (p) release(p); p = o.p; o.p = nullptr; }
			return *this;
		}
		~Ref() { if (p) release(p); }
		void* get() const { return p; }
		explicit operator bool() const { return p != nullptr; }
	};
}

// VS + 顶点声明 + FVF 三件套守卫(vertex_submit 等自定义声明绘制用):
// 构造保存当前状态, 析构恢复并释放 Get* 带回的引用。
struct ShaderStateGuard
{
	void* vs = nullptr;
	void* decl = nullptr;
	DWORD fvf = 0;

	ShaderStateGuard()
	{
		d3d::get_vertex_shader((DWORD*)&vs);
		d3d::get_vertex_declaration(&decl);
		d3d::get_fvf(&fvf);
	}
	~ShaderStateGuard()
	{
		d3d::set_vertex_shader_handle((DWORD)(uintptr_t)vs);
		d3d::set_vertex_declaration(decl);
		d3d::set_fvf(fvf);
		d3d::release(vs);
		d3d::release(decl);
	}
	ShaderStateGuard(const ShaderStateGuard&) = delete;
	ShaderStateGuard& operator=(const ShaderStateGuard&) = delete;
};

// 完整渲染状态 + 前 STAGES 个纹理槽守卫(gpart 演化/绘制 pass 用)。
// Get* 的加引用结果由成员 d3d::Ref 自动释放(成员在析构体之后销毁,
// 因此"先设回状态、后自动释放引用"的正确顺序由语言保证)。
template <int STAGES = 6>
struct RenderStateGuardT
{
	d3d::Ref vs, ps, decl, rt0;
	DWORD fvf = 0;
	DWORD zenable = 0, blend = 0, src = 0, dst = 0;
	DWORD cull = 0;
	DWORD ps_en = 0, ps_scale = 0, ps_min = 0, ps_max = 0, ps_size = 0;
	float minv = 0, maxv = 0, sizev = 0;
	UINT vp_w = 0, vp_h = 0;
	void* tex[STAGES] = {};

	RenderStateGuardT()
	{
		DWORD vs_raw = 0, ps_raw = 0;
		d3d::get_vertex_shader(&vs_raw);
		d3d::get_pixel_shader(&ps_raw);
		vs = d3d::Ref((void*)(uintptr_t)vs_raw);
		ps = d3d::Ref((void*)(uintptr_t)ps_raw);
		d3d::get_fvf(&fvf);
		void* decl_raw = nullptr;
		d3d::get_vertex_declaration(&decl_raw);
		decl = d3d::Ref(decl_raw);
		void* rt_raw = nullptr;
		d3d::get_render_target(0, &rt_raw);
		rt0 = d3d::Ref(rt_raw);
		d3d::get_render_state(D3DRS_ZENABLE, &zenable);
		d3d::get_render_state(D3DRS_ALPHABLENDENABLE, &blend);
		d3d::get_render_state(D3DRS_SRCBLEND, &src);
		d3d::get_render_state(D3DRS_DESTBLEND, &dst);
		d3d::get_render_state(D3DRS_CULLMODE, &cull);
		d3d::get_render_state(D3DRS_POINTSPRITEENABLE, &ps_en);
		d3d::get_render_state(D3DRS_POINTSCALEENABLE, &ps_scale);
		d3d::get_render_state(D3DRS_POINTSIZE_MIN, &ps_min);
		d3d::get_render_state(D3DRS_POINTSIZE_MAX, &ps_max);
		d3d::get_render_state(D3DRS_POINTSIZE, &ps_size);
		minv = (float)ps_min;
		maxv = (float)ps_max;
		sizev = (float)ps_size;
		d3d::get_viewport(&vp_w, &vp_h);
		for (int i = 0; i < STAGES; ++i)
			d3d::get_texture(i, &tex[i]);
	}
	~RenderStateGuardT()
	{
		for (int i = 0; i < STAGES; ++i)
			d3d::set_texture(i, tex[i]);
		d3d::set_vertex_shader_handle((DWORD)(uintptr_t)vs.get());
		d3d::set_pixel_shader((DWORD)(uintptr_t)ps.get());
		d3d::set_vertex_declaration(decl.get());
		d3d::set_fvf(fvf);
		d3d::set_render_target(0, rt0.get());
		d3d::set_render_target(1, nullptr);
		d3d::set_render_target(2, nullptr);
		d3d::set_render_state(D3DRS_ZENABLE, zenable);
		d3d::set_render_state(D3DRS_ALPHABLENDENABLE, blend);
		d3d::set_render_state(D3DRS_SRCBLEND, src);
		d3d::set_render_state(D3DRS_DESTBLEND, dst);
		d3d::set_render_state(D3DRS_CULLMODE, cull);
		d3d::set_render_state(D3DRS_POINTSPRITEENABLE, ps_en);
		d3d::set_render_state(D3DRS_POINTSCALEENABLE, ps_scale);
		d3d::set_render_state(D3DRS_POINTSIZE_MIN, ps_min);
		d3d::set_render_state(D3DRS_POINTSIZE_MAX, ps_max);
		d3d::set_render_state(D3DRS_POINTSIZE, ps_size);
		d3d::set_viewport(vp_w, vp_h);
	}
	RenderStateGuardT(const RenderStateGuardT&) = delete;
	RenderStateGuardT& operator=(const RenderStateGuardT&) = delete;
};

typedef RenderStateGuardT<6> RenderStateGuard;
