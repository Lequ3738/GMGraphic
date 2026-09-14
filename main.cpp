// GMAPI setup

#include "main.h"
#include "texture_atlas.h"
#include "draw_text.h"
#include "string_make.h"
#include "shader.h"
#include <cstdint>

gm::CGMAPI* gmapi;
std::string str_ret = "BABEBEEF"; // Used to return strings by macro.
HINSTANCE g_dllInstance = nullptr;

bool WINAPI DllMain(HINSTANCE aModuleHandle, int aReason, int aReserved)
{
	switch (aReason)
	{
		case DLL_PROCESS_ATTACH:
		{
			ulong result = 0;
			g_dllInstance = aModuleHandle;   // 供 FindResource 加载内嵌 shader 源码
			gmapi = gm::CGMAPI::Create(&result);

			// Check the initialization
			if (result == gm::GMAPI_INITIALIZATION_FAILED)
			{
				complain("Unable to initialize GMAPI.");
				return FALSE;
			}

			// [2026-09-14] 后端检测提前到加载时刻: 任何导出若先于 GML 的 init() 被
			// 调用, d3d::version() 不再误判为 V8(装着 GMDirectX9 时按 D3D8 vtable
			// 槽位调用 D3D9 设备 = 崩溃)。设备指针此刻为空也无妨 —— ensure_version
			// 的 d3d9.dll 在场兜底仍生效, init() 会用真实指针再确认一次。
			d3d::ensure_version((void*)gmapi->GetDirect3DDevice(),
				(void*)gmapi->GetDirect3DInterface());

#ifdef _DEBUG
			gm::show_message("Debug Mode.");
#endif
		}
		break;

		case DLL_PROCESS_DETACH:
		{
			// 先停掉 shader 异步编译 worker(避免线程引用已卸载模块)。
			shader_compile_shutdown();

			// 如下内容提前清理，确保不会发生全局变量析构顺序问题。
			game_texture_atlas.clear();
			game_sdf_glyphs.clear();

			shader_destroy((double)sdf_shader);
			shader_destroy((double)sdf_shader_premul);

			gmapi->Destroy();
		}
		break;
	}

	return true;
}

atlas::texture_info current_texture;

// ============================================================================
// 批段快照(快照式合批, 2026-09-14)
// 图集批的观感 = 烘焙顶点 + 批打开时刻的设备继承态。start_draw 把继承态定格进
// g_batch_snap, end_draw 按快照提交并还原 flush 时刻现场。
// flush 时机(2026-09-14 二批起): GMDirectX9 全闭合设备钩 —— 提交/内容/目标/状态
// 全部先冲刷, 状态写入会把批切段, 因此"批打开→flush 之间"设备状态不再可能变化,
// 本侧快照降为纵深防御(防 state block 等钩外路径)。仅剩的钩外状态 = SDF 的 CPU
// 全局(size/sharpness/thickness/premul), 由 draw_text.cpp 的 sdf_segment_batch 在
// 各 setter 内自律切段。
// ============================================================================
static d3d::DeviceStateSnap g_batch_snap;
static bool g_sdf_snap_use_shader = false;
static int  g_sdf_snap_shader = -1;

// ============================================================================
// [2026-09-14 桥梁期修复①] 批冲刷最小自愈(自碰清单)
// end_draw 实际触碰的设备状态只有: 纹理 stage0-7、TSS0 的 ADDRESSU/V/COLOROP/
// COLORARG1/COLORARG2、PS/VS/顶点声明/FVF(vertex::end 与 SDF shader_set 所写)。
// 在 43 槽字面闭合不变式下, flush 时刻设备状态 == 批打开时刻状态 == 引擎现场,
// 触碰前从 GMDirectX9 状态影子表(修复③)读出的值就是引擎现场值 —— 提交后按清单
// 精确归还(与影子现状不同者才落设备调用, 典型整段仅 stage0 纹理一写)。三轮 38 项
// 全量快照(捕获 38 GET + 两次回放 76 SET ≈ 114 次设备调用/flush)退役为调试对照:
// 环境变量 GMGRAPHIC_BATCH_FULL_SNAPSHOT=1 走旧路径(dssnap 捕获+两次回放), 怀疑
// 状态污染时一键切回比对; 影子读口不可用(未装 GMDirectX9/旧版)同样自动落回。
// ============================================================================

// GMDirectX9 影子表读口 v1(ABI 镜像 GMDirectX9 source/state_shadow.h, 只增不改)。
struct Gmdx9ShadowApiV1
{
    unsigned long size;
    unsigned long version;
    bool  (__cdecl* live)();
    bool  (__cdecl* get_rs)(unsigned long state, unsigned long* out);
    bool  (__cdecl* get_tss)(unsigned long stage, unsigned long type, unsigned long* out);
    bool  (__cdecl* get_sampler)(unsigned long sampler, unsigned long type, unsigned long* out);
    void* (__cdecl* get_texture)(unsigned long sampler);
    bool  (__cdecl* get_xf)(unsigned long state, void* out4x4);
    bool  (__cdecl* get_vp)(void* out);
    bool  (__cdecl* get_fvf)(unsigned long* out);
    void* (__cdecl* get_decl)();
    void* (__cdecl* get_vs)();
    void* (__cdecl* get_ps)();
};
static_assert(sizeof(Gmdx9ShadowApiV1) == 52, "Gmdx9ShadowApiV1 ABI 镜像失配");

static const Gmdx9ShadowApiV1* g_shadow_api = nullptr;

// init() 调用: 解析 GMDirectX9 影子表读口。未装/旧版无此导出 → 保持 nullptr,
// 批自愈自动落回全量快照路径(与上一版行为一致)。
void batch_state_init()
{
	if (HMODULE hdx9 = GetModuleHandleA("GMDirectX9.dll"))
	{
		typedef const Gmdx9ShadowApiV1* (__cdecl* GetApiFn)();
		if (GetApiFn get = (GetApiFn)GetProcAddress(hdx9, "gmdx9_shadow_api"))
		{
			const Gmdx9ShadowApiV1* api = get();
			if (api && api->size >= sizeof(Gmdx9ShadowApiV1) && api->version >= 1)
				g_shadow_api = api;
		}
	}
}

namespace
{
	bool full_snapshot_debug()
	{
		static int v = -1;
		if (v < 0)
		{
			char b[2] = { 0 };
			v = (GetEnvironmentVariableA("GMGRAPHIC_BATCH_FULL_SNAPSHOT", b, 2) > 0
				&& b[0] != '0') ? 1 : 0;
		}
		return v == 1;
	}

	bool minimal_heal_active()
	{
		return g_shadow_api != nullptr && g_shadow_api->live() && !full_snapshot_debug();
	}

	// 触碰清单快照(值来自影子 == 引擎现场; TSS 枚举两代同值:
	// D3DTSS_ADDRESSU=13/ADDRESSV=14/COLOROP=1/COLORARG1=2/COLORARG2=3)。
	struct TouchState
	{
		void* tex[8];
		dword tss[5];
		void* ps;
		void* vs;
		void* decl;
		dword fvf;
		bool  fvf_ok;
	};
	const dword touch_tss_ids[5] = { 13, 14, 1, 2, 3 };

	void save_touch_state(TouchState& t)
	{
		for (int i = 0; i < 8; ++i)
			t.tex[i] = g_shadow_api->get_texture((unsigned long)i);
		for (int k = 0; k < 5; ++k)
		{
			dword v = 0;
			t.tss[k] = g_shadow_api->get_tss(0, touch_tss_ids[k], &v) ? v : 0;
		}
		t.ps = g_shadow_api->get_ps();
		t.vs = g_shadow_api->get_vs();
		t.decl = g_shadow_api->get_decl();
		unsigned long fvf = 0;
		t.fvf_ok = g_shadow_api->get_fvf(&fvf);
		t.fvf = (dword)fvf;
	}

	void restore_touch_state(const TouchState& t)
	{
		// 差异才落设备(我们的触碰已过 43 钩, 影子即设备现状); Set 全走适配器包装
		// —— 与 dssnap_apply 同一路径, 处于 g_flush_active 重入保护之下不递归。
		for (int i = 0; i < 8; ++i)
			if (g_shadow_api->get_texture((unsigned long)i) != t.tex[i])
				d3d::set_texture((dword)i, t.tex[i]);
		for (int k = 0; k < 5; ++k)
		{
			dword v = 0;
			if (g_shadow_api->get_tss(0, touch_tss_ids[k], &v) && v != t.tss[k])
				d3d::set_tex_stage_state(0, touch_tss_ids[k], t.tss[k]);
		}
		if (g_shadow_api->get_ps() != t.ps)
			d3d::set_pixel_shader((dword)(uintptr_t)t.ps);
		if (g_shadow_api->get_vs() != t.vs)
			d3d::set_vertex_shader_handle((dword)(uintptr_t)t.vs);
		if (g_shadow_api->get_decl() != t.decl)
			d3d::set_vertex_declaration(t.decl);
		unsigned long fvf = 0;
		if (t.fvf_ok && g_shadow_api->get_fvf(&fvf) && (dword)fvf != t.fvf)
			d3d::set_fvf(t.fvf);
	}
}

// 栈上快照的 COM 引用释放守卫(dssnap_free 幂等, 显式释放后再析构是空操作)。
struct SnapReleaser
{
	d3d::DeviceStateSnap* s;
	explicit SnapReleaser(d3d::DeviceStateSnap* p) : s(p) {}
	~SnapReleaser() { if (s) d3d::dssnap_free(*s); }
	SnapReleaser(const SnapReleaser&) = delete;
	SnapReleaser& operator=(const SnapReleaser&) = delete;
};

// GMDirectX9 设备钩子的 flush 入口(自动 force_draw_to_screen)。调用发生在引擎
// 绘制序列中途(vtable 钩子), 异常绝不能穿过钩子(会炸穿引擎汇编帧) —— 全部吞掉,
// 批留在缓冲等下一次机会; end_draw 状态原子化后, 中途触发对引擎绘制零扰动。
void atlas_flush_noexcept(void)
{
	try { atlas::end_draw(); }
	catch (...) {}
}

void atlas::start_draw(void* texture, D3DFORMAT format)
{
	try
	{
		if (texture == nullptr)
			throw std::runtime_error("The texture atlas is null.");

		vertex::begin(D3DPT_TRIANGLELIST, true);
		current_texture = { texture, format };

		// 积累时刻观感定格。仅 V9: V8 无自动 flush 钩子, 历史行为不快照。
		if (d3d::version() == d3d::V9)
		{
			// [修复①] 最小自愈路径不再捕获全量快照(状态来源=影子表, 见 end_draw);
			// 全量捕获仅在全快照调试模式/影子读口不可用时保留。
			if (!minimal_heal_active())
				d3d::dssnap_capture(g_batch_snap);
			g_sdf_snap_use_shader = sdf::use_shader;
			g_sdf_snap_shader = sdf::shader;
		}
	}
	transpond_catch("atlas::start_draw(void*)")
}

void atlas::end_draw()
{
	try
	{
		if (current_texture.texture == nullptr)
			return;

		int prev_shader = -1;
		const bool atomic = (d3d::version() == d3d::V9);
		// [修复①] 最小自愈 = V9 + 影子读口可用 + 未开全量快照调试。
		const bool minimal = atomic && minimal_heal_active();

		// 状态原子化: 自动 flush 发生在引擎绘制序列中途(引擎已设好自己的纹理/采样/
		// 变换/着色器), 提交完批必须原样还回。旧实现除 A8 的 TSS 分支外一概不还原,
		// 手工 flush 靠"引擎下个绘制自带状态设置"侥幸成立, 自动化后必须显式还原。
		TouchState pre = {};
		if (minimal)
			save_touch_state(pre);   // 触碰前自影子读出(= 引擎现场, 字面闭合不变式)

		d3d::DeviceStateSnap now = {};
		SnapReleaser now_guard{ &now };   // 异常路径也释放捕获期的 COM 引用(dssnap_free 幂等)
		if (atomic && !minimal)
		{
			d3d::dssnap_capture(now);
			d3d::dssnap_apply(g_batch_snap);
		}

		texture_clear_all();
		D3DCheck(d3d::set_texture(0, current_texture.texture), 0);
		D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP), 1);
		D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP), 2);

		if (current_texture.format == D3DFMT_A8)  // 字体纹理
		{
			if (g_sdf_snap_use_shader)
			{
				prev_shader = (int)shader_current();
				shader_set(g_sdf_snap_shader);

				// 保持旧 d3d_set_ps_const 的 ps_1.4 寄存器 [-1,1] clamp 行为,
				// 避免新 shader_set_uniform_f(不 clamp)改变 SDF 文字锐度。
				double sharpness = std::clamp(sdf::font_sharpness * sdf::game_font_size * 0.005,
					-1.0, 1.0);
				double thickness = std::clamp((-(sdf::font_thickness - 500.0f) +
					500.0f) / 1000.0f, 0.1f, 0.9f);

				// uniform 按后端分发: DX8 asm 写 c0(scale/thickness); DX9 HLSL 写
				// u_buffer/thickness(SDF 阈值) + u_gamma/边缘软度。
				if (d3d::version() == d3d::V9)
				{
					double scale = pt_to_px(sdf::game_font_size) / std::max(sdf::font_sharpness, 0.01f);
					double gamma = std::clamp(0.044 / scale, 0.005, 0.2);
					shader_set_uniform_f(sdf_shader_uniform_buffer, thickness, 0, 0, 0);
					shader_set_uniform_f(sdf_shader_uniform_gamma,  gamma,     0, 0, 0);
				}
				else
					shader_set_uniform_f(sdf_shader_uniform, sharpness, thickness, 0, 0);
			}
			else
			{
				// 颜色 = 直接使用顶点颜色 (忽略纹理中不存在的RGB)
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), 5);
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE), 6);
			}
		}
		else if (current_texture.format != D3DFMT_A8R8G8B8)
			throw std::runtime_error("Unsupported texture format.");

		vertex::end();

		if (current_texture.format == D3DFMT_A8)
		{
			if (g_sdf_snap_use_shader)
				shader_set(prev_shader);
			else
			{
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLOROP, D3DTOP_MODULATE), 8);
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLORARG1, D3DTA_TEXTURE), 9);
				D3DCheck(d3d::set_tex_stage_state(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE), 10);
			}
		}

		// 还原 flush 时刻现场。
		// [修复①] 最小自愈: 按触碰清单归还, 差异者才落设备调用(典型整段仅
		// stage0 纹理一写; TSS/着色器/FVF 多数与影子同值直接跳过)。
		// 全快照调试路径: 全量回放 now(A8 分支上面的 TSS 恢复被此处覆盖, 属冗余
		// 而非冲突; COM 引用由 now_guard 析构释放)。
		if (minimal)
			restore_touch_state(pre);
		else if (atomic)
			d3d::dssnap_apply(now);
		current_texture = { nullptr, D3DFMT_A8R8G8B8 };
	}
	transpond_catch("atlas::end_draw()")
}